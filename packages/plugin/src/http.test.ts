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
import type { LearnApi, RecognizeApi, RecognizeJobView, WorkRuntime } from './http.ts';
import { createUiBridge } from './ui-open.ts';
import type { UiBridge } from './ui-open.ts';

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

async function start(options: {
  recognize?: RecognizeApi;
  authorize?: () => number | null;
  ui?: UiBridge;
  learn?: LearnApi;
}): Promise<Harness> {
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
    ...(options.ui === undefined ? {} : { ui: options.ui }),
    ...(options.learn === undefined ? {} : { learn: options.learn }),
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
    collapse(id: string, nodeId: string) {
      calls.push(`collapse:${id}:${nodeId}`);
      return { ok: true as const, removed: 2 };
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
    collapse: () => ({ ok: false as const, reason: '未使用' }),
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

test('打开面板：POST 留指令，/api/state 把它送给伴随层', async () => {
  let now = 10_000;
  const launches: string[] = [];
  const ui = createUiBridge({
    now: () => now,
    launch: () => {
      launches.push('launch');
      return true;
    },
  });
  const h = await start({ ui });
  try {
    const opened = await fetch(`${h.base}/api/ui/open`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ target: 'graph' }),
    });
    assert.equal(opened.status, 200);
    const outcome = (await opened.json()) as {
      ok: boolean;
      companion: boolean;
      launched: boolean;
      command: { seq: number; target: string; at: number };
    };
    assert.equal(outcome.ok, true);
    assert.equal(outcome.command.target, 'graph');
    assert.equal(outcome.command.seq, 1);
    assert.equal(outcome.launched, true);
    assert.deepEqual(launches, ['launch']);

    now = 11_000;
    const state = (await (await fetch(`${h.base}/api/state`)).json()) as {
      ui: { seq: number; target: string; at: number; companion: boolean } | null;
    };
    assert.equal(state.ui?.target, 'graph');
    assert.equal(state.ui?.seq, 1);
    assert.equal(state.ui?.at, 10_000);
    assert.equal(state.ui?.companion, false);

    // target 缺省就是 panel；乱写要挡在路由口上，别把垃圾指令发给伴随层。
    const fallback = await fetch(`${h.base}/api/ui/open`, {
      method: 'POST',
      body: JSON.stringify({}),
    });
    assert.equal(fallback.status, 200);
    const panel = (await fallback.json()) as { command: { target: string } };
    assert.equal(panel.command.target, 'panel');

    const bad = await fetch(`${h.base}/api/ui/open`, {
      method: 'POST',
      body: JSON.stringify({ target: 'desktop' }),
    });
    assert.equal(bad.status, 400);
  } finally {
    await h.close();
  }
});

test('打开面板：没注入桥时 503，GET 405，state 的 ui 为 null', async () => {
  const h = await start({});
  try {
    const res = await fetch(`${h.base}/api/ui/open`, { method: 'POST', body: JSON.stringify({}) });
    assert.equal(res.status, 503);

    const state = (await (await fetch(`${h.base}/api/state`)).json()) as { ui: unknown };
    assert.equal(state.ui, null);
  } finally {
    await h.close();
  }

  const ui = createUiBridge({ now: () => 1 });
  const withUi = await start({ ui });
  try {
    const wrongMethod = await fetch(`${withUi.base}/api/ui/open`);
    assert.equal(wrongMethod.status, 405);
  } finally {
    await withUi.close();
  }
});
test('讲解：200 时同时给 text/alt 和卡片要的 view', async () => {
  const learn: LearnApi = {
    async request(nodeId) {
      assert.equal(nodeId, 'n1');
      return { ok: true, text: '**幂等**：重发一次也不变。', alt: '把 `POST` 发两遍' };
    },
  };
  const h = await start({ learn });
  try {
    const res = await fetch(`${h.base}/api/learn`, { method: 'POST', body: JSON.stringify({ nodeId: 'n1' }) });
    assert.equal(res.status, 200);
    const body = (await res.json()) as {
      ok: boolean;
      text: string;
      alt?: string;
      view: { phase: string; title: string; text: string; hint: string; unsupported: boolean };
    };
    assert.equal(body.ok, true);
    assert.equal(body.alt, '把 `POST` 发两遍');
    assert.equal(body.view.phase, 'done');
    assert.equal(body.view.text, '幂等：重发一次也不变。');
    assert.equal(body.view.hint, '试一下：把 POST 发两遍');
    assert.equal(body.view.unsupported, false);
  } finally {
    await h.close();
  }
});

test('讲解：失败按状态码回可读文案，不把卡片留给空白', async () => {
  const cases: { status: number; reason: string; title: string }[] = [
    { status: 404, reason: '没有这个节点', title: '这颗节点不在了' },
    { status: 503, reason: '宿主没有提供 llm 服务', title: '宿主没接模型' },
    { status: 500, reason: '模型超时', title: '讲的时候出错了' },
  ];
  for (const item of cases) {
    const h = await start({
      learn: {
        async request() {
          return { ok: false as const, status: item.status, reason: item.reason };
        },
      },
    });
    try {
      const res = await fetch(`${h.base}/api/learn`, { method: 'POST', body: JSON.stringify({ nodeId: 'n1' }) });
      assert.equal(res.status, item.status);
      const body = (await res.json()) as {
        ok: boolean;
        error: string;
        view: { phase: string; title: string; text: string; hint: string; unsupported: boolean };
      };
      assert.equal(body.ok, false);
      // error 保持原样给机器看，view 是给人看的：两层不能混。
      assert.equal(body.error, item.reason);
      assert.equal(body.view.phase, 'error');
      assert.equal(body.view.title, item.title);
      assert.equal(body.view.text, item.reason);
      assert.notEqual(body.view.hint.trim(), '');
      assert.equal(body.view.unsupported, false);
    } finally {
      await h.close();
    }
  }
});

test('讲解：GET 405，且明确告诉原生「这个宿主没有这条路」', async () => {
  const h = await start({});
  try {
    const wrong = await fetch(`${h.base}/api/learn`);
    assert.equal(wrong.status, 405);
    const body = (await wrong.json()) as { ok: boolean; view: { unsupported: boolean } };
    assert.equal(body.ok, false);
    assert.equal(body.view.unsupported, true);
  } finally {
    await h.close();
  }
});
