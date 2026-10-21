/**
 * HTTP 面：给浏览器客户端与伴随层用的一条最小通道。
 *
 * 为什么不是 RPC：DSH 的 Typert/Remote 机制面向宿主内部域，第三方插件接入成本高、
 * 且把「伴随层（Electron）」也拖进宿主类型体系。这里只要三件事——
 * 读快照、提交意图、订阅变更——用普通 HTTP + SSE 表达最直接，任何语言都能对接。
 *
 * 路由表（全部挂在 `/dsh-work` 前缀下）：
 *   GET  /api/health    探活 + 版本
 *   GET  /api/state     整个快照
 *   POST /api/intent    提交一条意图
 *   POST /api/recognize 一张 PNG → 提交识别作业（立即返回 jobId）
 *   GET  /api/recognize/<jobId> 查询识别进度 / 结果
 *   POST /api/plan-text 一段人话 → 一张任务树（同步）
 *   GET  /api/events    SSE：快照 + 增量
 *
 * 识别**不**走同步响应：一次视觉模型调用要几十秒，挂在 HTTP 上会被任何一层
 * 超时（浏览器、代理、宿主）掐断，而且掐断后没法知道模型还在不在跑。
 * 作业化以后，POST 立刻返回，伴随层按需轮询；超时与取消在作业内部用
 * AbortController 兜住（见 index.ts 的 runRecognize）。
 *
 * 鉴权不在这里做：调用方通过 `options.authorize` 注入宿主的信任判定，
 * 让本文件可以脱离宿主单测（见 http.test.ts）。
 */

import type { IncomingMessage, ServerResponse } from 'node:http';

import type { Actor, ApplyResult, Intent, WorkDoc, WorkNode } from '@dshwork/core';

export const ROUTE_PREFIX = '/dsh-work';

/** 请求体上限：意图报文很小，超过这个数一定是走错了。 */
const MAX_BODY_BYTES = 64 * 1024;

/** 截图上限：框选区域最大也就几 MB，超过说明发错了东西。 */
const MAX_PNG_BYTES = 12 * 1024 * 1024;

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
  /** 识别作业：没注入时该路由回 503，本文件因此可以脱离宿主单测。 */
  recognize?: RecognizeApi;
  /** 节点建议：同上，没注入就 503。 */
  advise?: AdviseApi;
  /** 学习模式：讲一个概念。没有就 503，不影响其它路由。 */
  learn?: LearnApi;
  /** 一句话生成计划：同样可选，没注入就 503。 */
  planText?: PlanTextApi;
}

/** 识别作业的对外视图：状态 + 一句人话，不含模型原文与图片。 */
export interface RecognizeJobView {
  state: 'running' | 'done' | 'error';
  title?: string;
  count?: number;
  reason?: string;
  /** 作业 id：拿到它才能去 /api/plan/<id> 取草稿、或者提交草稿。 */
  draft?: string;
}

export interface RecognizeApi {
  /** 提交一张截图。同一时刻只允许一个作业在跑。 */
  start(png: Buffer): { ok: true; id: string } | { ok: false; status: number; reason: string };
  /** 查询作业；不存在（含已过期）返回 null。 */
  get(id: string): RecognizeJobView | null;
  /** 确认草稿：到这一步才落库。 */
  commit(id: string): Promise<{ ok: true; count: number } | { ok: false; reason: string }>;
  /** 丢掉草稿；不写文档。 */
  discard(id: string): boolean;
  /** 取草稿树；没有就 null。 */
  draft(id: string): { title: string; roots: readonly string[]; nodes: readonly WorkNode[] } | null;
  /** 二次解构：把一颗节点再拆细，成功后草稿就地更新。 */
  expand(id: string, nodeId: string): Promise<{ ok: true; added: number } | { ok: false; reason: string }>;
  /** 撤销一次二次解构：把这颗节点的子树删掉，退回没拆过的样子。 */
  collapse(id: string, nodeId: string): { ok: true; removed: number } | { ok: false; reason: string };
}


/** 节点建议：现在要一条，回来一句话。失败给可读原因，不给半成品。 */
/** 一段人话 → 一张计划。同步返回，模型慢时由调用方超时兜底。 */
export interface PlanTextApi {
  plan(text: string): Promise<
    | { ok: true; title: string; count: number }
    | { ok: false; status: number; reason: string }
  >;
}

export interface AdviseApi {
  request(nodeId: string | undefined): Promise<{ ok: true; text: string; alt?: string } | { ok: false; status: number; reason: string }>;
}
export interface LearnApi {
  request(nodeId: string | undefined): Promise<{ ok: true; text: string; alt?: string } | { ok: false; status: number; reason: string }>;
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

    if (pathname === '/api/advise') {
      if (req.method !== 'POST') {
        sendJson(res, 405, { ok: false, error: 'method not allowed' });
        return;
      }
      if (options.advise === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供建议能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { nodeId?: unknown };
      const nodeId = typeof payload.nodeId === 'string' && payload.nodeId !== '' ? payload.nodeId : undefined;
      const result = await options.advise.request(nodeId);
      if (!result.ok) {
        sendJson(res, result.status, { ok: false, error: result.reason });
        return;
      }
      sendJson(res, 200, { ok: true, text: result.text, ...(result.alt === undefined ? {} : { alt: result.alt }) });
      return;
    }
    if (pathname === '/api/learn') {
      if (req.method !== 'POST') {
        sendJson(res, 405, { ok: false, error: 'method not allowed' });
        return;
      }
      if (options.learn === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供讲解能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { nodeId?: unknown };
      const nodeId = typeof payload.nodeId === 'string' && payload.nodeId !== '' ? payload.nodeId : undefined;
      const result = await options.learn.request(nodeId);
      if (!result.ok) {
        sendJson(res, result.status, { ok: false, error: result.reason });
        return;
      }
      sendJson(res, 200, { ok: true, text: result.text, ...(result.alt === undefined ? {} : { alt: result.alt }) });
      return;
    }
    if (pathname === '/api/plan-text') {
      if (req.method !== 'POST') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      if (options.planText === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供规划能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const record = body.value as Record<string, unknown>;
      const text = typeof record['text'] === 'string' ? record['text'].trim() : '';
      if (text === '') {
        sendJson(res, 400, { ok: false, error: 'text 不能为空' });
        return;
      }
      const result = await options.planText.plan(text);
      if (!result.ok) {
        sendJson(res, result.status, { ok: false, error: result.reason });
        return;
      }
      sendJson(res, 200, { ok: true, title: result.title, count: result.count });
      return;
    }

    if (pathname === '/api/recognize' && req.method !== 'POST') {
      sendJson(res, 405, { error: 'method not allowed' });
      return;
    }

    if (pathname === '/api/recognize' && req.method === 'POST') {
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const body = await readBinaryBody(req, MAX_PNG_BYTES);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      if (!looksLikePng(body.value)) {
        sendJson(res, 400, { ok: false, error: '请求体不是 PNG' });
        return;
      }

      const started = options.recognize.start(body.value);
      if (!started.ok) {
        sendJson(res, started.status, { ok: false, error: started.reason });
        return;
      }
      // 202：作业已受理，结果要另行来拿
      sendJson(res, 202, { ok: true, jobId: started.id });
      return;
    }

    if (pathname.startsWith('/api/recognize/')) {
      if (req.method !== 'GET') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const id = decodeURIComponent(pathname.slice('/api/recognize/'.length));
      const job = options.recognize.get(id);
      if (job === null) {
        sendJson(res, 404, { ok: false, error: '没有这个识别作业（可能已过期）' });
        return;
      }
      sendJson(res, 200, { ok: true, ...job });
      return;
    }

    if (pathname === '/api/plan/commit' && req.method === 'POST') {
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { jobId?: unknown };
      const jobId = typeof payload.jobId === 'string' ? payload.jobId : '';
      if (jobId === '') {
        sendJson(res, 400, { ok: false, error: '缺少 jobId' });
        return;
      }
      const result = await options.recognize.commit(jobId);
      sendJson(res, result.ok ? 200 : 409, { ...result, ok: result.ok });
      return;
    }

    if (pathname === '/api/plan/discard' && req.method === 'POST') {
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { jobId?: unknown };
      const jobId = typeof payload.jobId === 'string' ? payload.jobId : '';
      if (jobId === '') {
        sendJson(res, 400, { ok: false, error: '缺少 jobId' });
        return;
      }
      options.recognize.discard(jobId);
      sendJson(res, 200, { ok: true });
      return;
    }

    if (pathname === '/api/plan/expand' && req.method === 'POST') {
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { jobId?: unknown; nodeId?: unknown };
      const jobId = typeof payload.jobId === 'string' ? payload.jobId : '';
      const nodeId = typeof payload.nodeId === 'string' ? payload.nodeId : '';
      if (jobId === '' || nodeId === '') {
        sendJson(res, 400, { ok: false, error: '缺少 jobId 或 nodeId' });
        return;
      }
      const result = await options.recognize.expand(jobId, nodeId);
      sendJson(res, result.ok ? 200 : 502, { ...result, ok: result.ok });
      return;
    }

    // 撤销二次解构：删子树，不碰这颗节点本身。同步完成，不需要作业。
    if (pathname === '/api/plan/collapse' && req.method === 'POST') {
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { jobId?: unknown; nodeId?: unknown };
      const jobId = typeof payload.jobId === 'string' ? payload.jobId : '';
      const nodeId = typeof payload.nodeId === 'string' ? payload.nodeId : '';
      if (jobId === '' || nodeId === '') {
        sendJson(res, 400, { ok: false, error: '缺少 jobId 或 nodeId' });
        return;
      }
      const result = options.recognize.collapse(jobId, nodeId);
      sendJson(res, result.ok ? 200 : 409, { ...result, ok: result.ok });
      return;
    }

    // 草稿树：给审阅窗口渲染用。刻意和 /api/state 分开——草稿不属于文档。
    if (pathname.startsWith('/api/plan/')) {
      if (req.method !== 'GET') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      if (options.recognize === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供识别能力' });
        return;
      }
      const id = decodeURIComponent(pathname.slice('/api/plan/'.length));
      const draft = options.recognize.draft(id);
      if (draft === null) {
        sendJson(res, 404, { ok: false, error: '没有这张草稿（可能已过期）' });
        return;
      }
      sendJson(res, 200, {
        ok: true,
        title: draft.title,
        roots: draft.roots,
        nodes: draft.nodes.map((node) => ({
          id: node.id,
          parentId: node.parentId,
          title: node.title,
          detail: node.detail,
          acceptance: node.acceptance,
          children: node.children,
          status: node.status,
          kind: node.kind,
        })),
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

/** 原样读二进制体（截图），不做任何解码 —— 解码失败的原因在这里没有意义。 */
async function readBinaryBody(req: IncomingMessage, limit: number): Promise<BodyBinaryResult> {
  const declared = Number(req.headers['content-length']);
  if (Number.isFinite(declared) && declared > limit) {
    return { ok: false, status: 413, reason: '截图过大' };
  }

  const chunks: Buffer[] = [];
  let total = 0;
  for await (const chunk of req) {
    const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk as string);
    total += buffer.length;
    if (total > limit) return { ok: false, status: 413, reason: '截图过大' };
    chunks.push(buffer);
  }
  if (total === 0) return { ok: false, status: 400, reason: '请求体为空' };
  return { ok: true, value: Buffer.concat(chunks) };
}

type BodyBinaryResult =
  | { ok: true; value: Buffer }
  | { ok: false; status: number; reason: string };

/** PNG 的八字节签名。挡掉「路径/JSON 被误发到这里」这类错误，比让模型去猜便宜。 */
function looksLikePng(buffer: Buffer): boolean {
  return (
    buffer.length > 8 &&
    buffer[0] === 0x89 &&
    buffer[1] === 0x50 &&
    buffer[2] === 0x4e &&
    buffer[3] === 0x47
  );
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
