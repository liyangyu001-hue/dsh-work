/**
 * HTTP 面：给浏览器客户端与伴随层用的一条最小通道。
 *
 * 为什么不是 RPC：DSH 的 Typert/Remote 机制面向宿主内部域，第三方插件接入成本高、
 * 且把「伴随层（Electron）」也拖进宿主类型体系。这里只要三件事——
 * 读快照、提交意图、订阅变更——用普通 HTTP + SSE 表达最直接，任何语言都能对接。
 *
 * 路由表（全部挂在 `/dsh-work` 前缀下）：
 *   GET  /api/health   探活 + 版本
 *   GET  /api/state    整个快照
 *   POST /api/intent   提交一条意图
 *   GET  /api/events   SSE：快照 + 增量
 *
 * 鉴权不在这里做：调用方通过 `options.authorize` 注入宿主的信任判定，
 * 让本文件可以脱离宿主单测（见 http.test.ts）。
 */

import type { IncomingMessage, ServerResponse } from 'node:http';

import type { Actor, ApplyResult, Intent, WorkDoc } from '@dshwork/core';

export const ROUTE_PREFIX = '/dsh-work';

/** 请求体上限：意图报文很小，超过这个数一定是走错了。 */
const MAX_BODY_BYTES = 64 * 1024;

/** SSE 心跳间隔：中间设备与浏览器都会掐掉长期静默的连接。 */
const HEARTBEAT_MS = 25_000;

export interface WorkRuntime {
  readonly version: string;
  getSnapshot(): WorkDoc;
  commit(intent: Intent, by: Actor): Promise<ApplyResult>;
  subscribe(listener: (doc: WorkDoc) => void): () => void;
}

export interface HttpOptions {
  runtime: WorkRuntime;
  /** 返回 null 放行；返回数字则作为拒绝状态码。 */
  authorize?: (req: IncomingMessage) => number | null;
}

export function createHttpHandler(options: HttpOptions) {
  const { runtime } = options;
  const clients = new Set<ServerResponse>();

  const broadcast = (doc: WorkDoc): void => {
    const frame = `event: snapshot\ndata: ${JSON.stringify({ seq: doc.seq, doc })}\n\n`;
    for (const client of clients) {
      try {
        client.write(frame);
      } catch {
        clients.delete(client);
      }
    }
  };

  const unsubscribe = runtime.subscribe(broadcast);

  const heartbeat = setInterval(() => {
    for (const client of clients) {
      try {
        client.write(': ping\n\n');
      } catch {
        clients.delete(client);
      }
    }
  }, HEARTBEAT_MS);
  heartbeat.unref?.();

  return {
    handler(req: IncomingMessage, res: ServerResponse): void {
      const rejection = options.authorize?.(req) ?? null;
      if (rejection !== null) {
        sendJson(res, rejection, { error: 'forbidden' });
        return;
      }
      route(req, res).catch((error: unknown) => {
        if (!res.headersSent) sendJson(res, 500, { error: describe(error) });
        else res.end();
      });
    },
    dispose(): void {
      clearInterval(heartbeat);
      unsubscribe();
      for (const client of clients) {
        try {
          client.end();
        } catch {
          /* 已经断了 */
        }
      }
      clients.clear();
    },
  };

  async function route(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? '/', 'http://localhost');
    const pathname = url.pathname.startsWith(ROUTE_PREFIX)
      ? url.pathname.slice(ROUTE_PREFIX.length) || '/'
      : url.pathname;

    if (pathname === '/api/health' || pathname === '/api/health/') {
      sendJson(res, 200, {
        ok: true,
        version: runtime.version,
        seq: runtime.getSnapshot().seq,
        platform: process.platform,
      });
      return;
    }

    if (pathname === '/api/state') {
      if (req.method !== 'GET' && req.method !== 'HEAD') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      sendJson(res, 200, { seq: runtime.getSnapshot().seq, doc: runtime.getSnapshot() });
      return;
    }

    if (pathname === '/api/intent') {
      if (req.method !== 'POST') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { error: body.reason });
        return;
      }
      const payload = body.value as { intent?: unknown; by?: unknown };
      if (!isIntentLike(payload.intent)) {
        sendJson(res, 400, { error: 'intent 形状不合法' });
        return;
      }
      const by: Actor =
        payload.by === 'ai' || payload.by === 'system' || payload.by === 'human'
          ? payload.by
          : 'human';

      const result = await runtime.commit(payload.intent, by);
      sendJson(res, result.ok ? 200 : 409, {
        applied: result.ok,
        seq: runtime.getSnapshot().seq,
        reason: result.ok ? undefined : result.reason,
      });
      return;
    }

    if (pathname === '/api/events') {
      openStream(req, res);
      return;
    }

    sendJson(res, 404, { error: 'not found' });
  }

  function openStream(req: IncomingMessage, res: ServerResponse): void {
    res.writeHead(200, {
      'content-type': 'text/event-stream; charset=utf-8',
      'cache-control': 'no-cache, no-transform',
      connection: 'keep-alive',
      'x-accel-buffering': 'no',
    });
    // 先补一份当前快照：客户端可能是在断线后重连，不能假设它手里有最新状态。
    const doc = runtime.getSnapshot();
    res.write(`event: snapshot\ndata: ${JSON.stringify({ seq: doc.seq, doc })}\n\n`);

    clients.add(res);
    const drop = (): void => {
      clients.delete(res);
    };
    req.on('close', drop);
    res.on('close', drop);
    res.on('error', drop);
  }
}

/* ─────────────────────────── 小工具 ─────────────────────────── */

function sendJson(res: ServerResponse, status: number, body: unknown): void {
  const text = JSON.stringify(body);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store',
    'content-length': Buffer.byteLength(text),
  });
  res.end(text);
}

type BodyResult =
  | { ok: true; value: unknown }
  | { ok: false; status: number; reason: string };

async function readJsonBody(req: IncomingMessage): Promise<BodyResult> {
  const chunks: Buffer[] = [];
  let total = 0;
  for await (const chunk of req) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk as string);
    total += buffer.length;
    if (total > MAX_BODY_BYTES) return { ok: false, status: 413, reason: '请求体过大' };
    chunks.push(buffer);
  }
  const text = Buffer.concat(chunks).toString('utf8').trim();
  if (text === '') return { ok: false, status: 400, reason: '请求体为空' };
  try {
    return { ok: true, value: JSON.parse(text) };
  } catch {
    return { ok: false, status: 400, reason: '请求体不是合法 JSON' };
  }
}

function isIntentLike(value: unknown): value is Intent {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { type?: unknown }).type === 'string'
  );
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
