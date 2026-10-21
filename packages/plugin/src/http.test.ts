/**
 * HTTP 层的行为契约：识别路由必须「只受理、另取结果」，并守住几个边界。
 *
 * 为什么用真 HTTP 而不是直接调内部函数：伴随层就是通过套接字来的，
 * 状态码、请求体上限、方法错配这些事只有真的发一遍才看得见。
 */

import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import type { Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import test from 'node:test';

import type { ApplyResult, WorkDoc } from '@dshwork/core';

import { createHttpHandler } from './http.ts';
import type { RecognizeApi, RecognizeJobView, WorkRuntime } from './http.ts';

const PNG = Buffer.from([
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
]);

function emptyDoc(): WorkDoc {
  return {
    schema: 1,
    id: 'wd_test',
    title: '测试',
    createdAt: 1,
    updatedAt: 1,
    seq: 1,
    pointer: null,
    roots: [],
    nodes: {},
  };
}

interface Harness {
  base: string;
  close(): Promise<void>;
}

async function start(options: { recognize?: RecognizeApi; authorize?: () => number | null }): Promise<Harness> {
  const runtime: WorkRuntime = {
    version: '0.0.0-test',
    getSnapshot: emptyDoc,
    commit: async (): Promise<ApplyResult> => ({ ok: true, doc: emptyDoc() }),
    subscribe: () => () => undefined,
  };

  const created = createHttpHandler({
    runtime,
    ...(options.authorize === undefined ? {} : { authorize: options.authorize }),
    ...(options.recognize === undefined ? {} : { recognize: options.recognize }),
  });

  const server: Server = createServer(created.handler);
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address() as AddressInfo;
  return {
    base: `http://127.0.0.1:${port}/dsh-work`,
    close: () =>
      new Promise<void>((resolve) => {
        created.dispose();
        server.close(() => resolve());
      }),
  };
}

function fakeApi(views: Map<string, RecognizeJobView>): { api: RecognizeApi; calls: string[] } {
  const calls: string[] = [];
  const api: RecognizeApi = {
    start(png: Buffer) {
      calls.push(`start:${png.length}`);
      return { ok: true, id: 'rc_1' };
    },
    get(id: string) {
      calls.push(`get:${id}`);
      return views.get(id) ?? null;
    },
    async commit(id: string) {
      calls.push(`commit:${id}`);
      return { ok: true as const, count: 3 };
    },
    discard(id: string) {
      calls.push(`discard:${id}`);
      return true;
    },
    draft(id: string) {
      calls.push(`draft:${id}`);
      return null;
    },
    async expand(id: string, nodeId: string) {
      calls.push(`expand:${id}:${nodeId}`);
      return { ok: true as const, added: 2 };
    },
  };
  return { api, calls };
}

test('识别：POST 受理返回 202，GET 另取结果', async () => {
  const { api, calls } = fakeApi(new Map<string, RecognizeJobView>([['rc_1', { state: 'done', count: 7 }]]));
  const h = await start({ recognize: api });
  try {
    const posted = await fetch(`${h.base}/api/recognize`, {
      method: 'POST',
      headers: { 'content-type': 'image/png' },
      body: PNG,
    });
    assert.equal(posted.status, 202);
    const accepted = (await posted.json()) as { ok: boolean; jobId: string };
    assert.equal(accepted.ok, true);
    assert.equal(accepted.jobId, 'rc_1');

    const polled = await fetch(`${h.base}/api/recognize/rc_1`);
    assert.equal(polled.status, 200);
    const view = (await polled.json()) as Record<string, unknown>;
    assert.equal(view['ok'], true);
    assert.equal(view['state'], 'done');
    assert.equal(view['count'], 7);

    assert.deepEqual(calls, [`start:${PNG.length}`, 'get:rc_1']);
  } finally {
    await h.close();
  }
});

test('识别：同一时刻只认一张图（409 原样透传）', async () => {
  const api: RecognizeApi = {
    start: () => ({ ok: false, status: 409, reason: '上一张截图还在识别中' }),
    get: () => null,
    commit: async () => ({ ok: false as const, reason: '未使用' }),
    discard: () => true,
    draft: () => null,
    expand: async () => ({ ok: false as const, reason: '未使用' }),
  };
  const h = await start({ recognize: api });
  try {
    const res = await fetch(`${h.base}/api/recognize`, { method: 'POST', body: PNG });
    assert.equal(res.status, 409);
    const body = (await res.json()) as { error: string };
    assert.match(body.error, /还在识别中/);
  } finally {
    await h.close();
  }
});

test('识别：请求体不是 PNG 时 400，且不会惊动作业表', async () => {
  const { api, calls } = fakeApi(new Map());
  const h = await start({ recognize: api });
  try {
    const res = await fetch(`${h.base}/api/recognize`, { method: 'POST', body: 'not a png' });
    assert.equal(res.status, 400);
    assert.deepEqual(calls, []);
  } finally {
    await h.close();
  }
});

test('识别：空请求体 400，方法错配 405', async () => {
  const { api } = fakeApi(new Map());
  const h = await start({ recognize: api });
  try {
    const empty = await fetch(`${h.base}/api/recognize`, { method: 'POST' });
    assert.equal(empty.status, 400);

    const wrongMethod = await fetch(`${h.base}/api/recognize`);
    assert.equal(wrongMethod.status, 405);
  } finally {
    await h.close();
  }
});

test('识别：查不到的作业 404，宿主没有识别能力时 503', async () => {
  const { api } = fakeApi(new Map());
  const withApi = await start({ recognize: api });
  try {
    const missing = await fetch(`${withApi.base}/api/recognize/rc_nope`);
    assert.equal(missing.status, 404);
  } finally {
    await withApi.close();
  }

  const noApi = await start({});
  try {
    const res = await fetch(`${noApi.base}/api/recognize`, { method: 'POST', body: PNG });
    assert.equal(res.status, 503);
  } finally {
    await noApi.close();
  }
});

test('鉴权：令牌不对时先被拦下，路由逻辑不执行', async () => {
  const { api, calls } = fakeApi(new Map());
  const h = await start({ recognize: api, authorize: () => 401 });
  try {
    const res = await fetch(`${h.base}/api/recognize`, { method: 'POST', body: PNG });
    assert.equal(res.status, 401);
    assert.deepEqual(calls, []);
  } finally {
    await h.close();
  }
});
