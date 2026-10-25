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

import { lessonFailure, lessonSuccess } from './lesson.ts';
import { MAX_QUESTION_CHARS, type ChatTurn } from './chat.ts';
import type { UsageSnapshot } from './usage.ts';
import type { UiBridge, UiOpenTarget, UiRail } from './ui-open.ts';

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

/** 事件流：把 journal 翻成给人读的行。 */
export interface FeedApi {
  read(limit: number): Promise<unknown[]>;
}

/**
 * 这一趟会话花了多少：调用次数与 token。
 *
 * 数不出来就如实说数不出来——`known: false` 时界面只显示次数，
 * 不要拿字符数硬折成 token。
 */
export interface StatsApi {
  read(): Promise<{
    usage: UsageSnapshot;
    /** 一句话版本，界面直接显示，省得两边各写一套格式化。 */
    line: string;
    /** 截图放在哪、保留几天。界面拿它说一句人话。 */
    captures?: { dir: string; keepDays: number; line: string };
  }>;
}

/**
 * 节点档案：`.dsh-work/nodes/<id>.md`。
 *
 * 三件事分开做而不是一个「万能」接口：读（只读、不建文件）、建（写骨架并挂上指针）、
 * 摘（明确声明「这一步没有产物」）。读者不会因为「点开看一眼」就产生副作用。
 */
export interface ArtifactApi {
  read(nodeId: string | undefined): Promise<
    | { ok: true; path: string; exists: boolean; content: string | null; tooLarge: boolean }
    | { ok: false; status: number; reason: string }
  >;
  ensure(nodeId: string | undefined): Promise<
    | { ok: true; path: string; created: boolean; content: string | null }
    | { ok: false; status: number; reason: string }
  >;
  detach(nodeId: string | undefined): Promise<{ ok: true } | { ok: false; status: number; reason: string }>;
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
  /** 追问：讲解之后接着问一句。没有就 503。 */
  chat?: ChatApi;
  /** 一句话生成计划：同样可选，没注入就 503。 */
  planText?: PlanTextApi;
  /** 事件流：没注入就 503；纯展示，失败也不该拖垮面板。 */
  feed?: FeedApi;
  /** 会话用量：没注入就 503。只读，不写任何东西。 */
  stats?: StatsApi;
  /** 节点档案 `nodes/<id>.md`：没注入就 503。 */
  artifact?: ArtifactApi;
  /**
   * 打开桌面面板的桥：浏览器按钮 → 宿主 → 伴随层轮询。
   * 没注入时按钮回 503，其余路由不受影响。
   */
  ui?: UiBridge;
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
  /**
   * `cached: true` 表示这条建议是复用节点上已有的，没有真的调模型。
   * 界面据此可以说一句「这条没变，没再问模型」。
   */
  request(
    nodeId: string | undefined,
  ): Promise<{ ok: true; text: string; alt?: string; cached?: boolean } | { ok: false; status: number; reason: string }>;
}
export interface LearnApi {
  request(nodeId: string | undefined): Promise<{ ok: true; text: string; alt?: string } | { ok: false; status: number; reason: string }>;
}
/**
 * 节点上的「追问」：人在讲解之后接着问一句。
 *
 * 与 LearnApi 的区别在于**不落库**——问答只活在界面上，不写进 work.json；
 * 所以它的返回值里没有 text/alt 那套卡片字段，只有一段话。
 */
export interface ChatApi {
  request(input: {
    nodeId: string | undefined;
    question: string;
    history: readonly ChatTurn[];
  }): Promise<{ ok: true; answer: string } | { ok: false; status: number; reason: string }>;
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

    if (pathname === '/api/feed') {
      if (req.method !== 'GET' && req.method !== 'HEAD') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      if (!options.feed) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供事件流能力' });
        return;
      }
      const asked = Number.parseInt(url.searchParams.get('limit') ?? '', 10);
      // 上限 100：这是给人看的最近几十条，别让人把整个日志拖下来
      const limit = Number.isFinite(asked) && asked > 0 ? Math.min(asked, 100) : 30;
      try {
        sendJson(res, 200, { ok: true, items: await options.feed.read(limit) });
      } catch (error) {
        sendJson(res, 200, { ok: false, error: error instanceof Error ? error.message : String(error), items: [] });
      }
      return;
    }
    if (pathname === '/api/artifact') {
      if (options.artifact === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供节点档案' });
        return;
      }
      const send = (result: { ok: boolean; status?: number; [key: string]: unknown }): void => {
        const { status, ...rest } = result;
        sendJson(res, result.ok ? 200 : (status ?? 400), rest);
      };
      if (req.method === 'GET' || req.method === 'HEAD') {
        const nodeId = url.searchParams.get('nodeId') ?? undefined;
        send(await options.artifact.read(nodeId));
        return;
      }
      if (req.method !== 'POST') {
        sendJson(res, 405, { ok: false, error: 'method not allowed' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { nodeId?: unknown; action?: unknown };
      const nodeId = typeof payload.nodeId === 'string' && payload.nodeId !== '' ? payload.nodeId : undefined;
      if (payload.action === 'detach') {
        send(await options.artifact.detach(nodeId));
        return;
      }
      if (payload.action !== undefined && payload.action !== 'ensure') {
        sendJson(res, 400, { ok: false, status: 400, error: `不认识的 action ${String(payload.action)}` });
        return;
      }
      send(await options.artifact.ensure(nodeId));
      return;
    }
    if (pathname === '/api/stats') {
      if (req.method !== 'GET' && req.method !== 'HEAD') {
        sendJson(res, 405, { ok: false, error: 'method not allowed' });
        return;
      }
      if (options.stats === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供用量统计' });
        return;
      }
      try {
        const { usage, line, captures } = await options.stats.read();
        // captures 只带「放哪、留几天」的一句话，不带文件列表——界面不需要，也不该到处爬目录。
        sendJson(res, 200, captures === undefined ? { ok: true, usage, line } : { ok: true, usage, line, captures });
      } catch (error) {
        sendJson(res, 200, {
          ok: false,
          error: error instanceof Error ? error.message : String(error),
        });
      }
      return;
    }
    if (pathname === '/api/state') {
      if (req.method !== 'GET' && req.method !== 'HEAD') {
        sendJson(res, 405, { error: 'method not allowed' });
        return;
      }
      const snapshot = runtime.getSnapshot();
      sendJson(res, 200, {
        seq: snapshot.seq,
        doc: snapshot,
        // 伴随层每秒拉一次状态，顺手把「打开面板」指令带走；
        // 没有桥时给 null，老的伴随层读到 null 会当没请求。
        ui: options.ui ? { ...options.ui.snapshot(), companion: options.ui.companionAlive() } : null,
      });
      return;
    }

    if (pathname === '/api/ui/open') {
      if (req.method !== 'POST') {
        sendJson(res, 405, { ok: false, error: 'method not allowed' });
        return;
      }
      if (options.ui === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供打开面板的通道' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { target?: unknown; rail?: unknown };
      const target: UiOpenTarget | null =
        payload.target === undefined || payload.target === 'panel'
          ? 'panel'
          : payload.target === 'graph'
            ? 'graph'
            : null;
      if (target === null) {
        sendJson(res, 400, { ok: false, error: 'target 只能是 panel 或 graph' });
        return;
      }
      // rail 只在开节点图时有意义；乱写给 400，别把伴随层看不懂的值塞进状态里。
      const rail: UiRail | undefined | null =
        payload.rail === undefined
          ? undefined
          : payload.rail === 'learn' || payload.rail === 'office'
            ? payload.rail
            : null;
      if (rail === null) {
        sendJson(res, 400, { ok: false, error: 'rail 只能是 learn 或 office' });
        return;
      }
      sendJson(res, 200, { ok: true, ...options.ui.open(target, rail) });
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
      sendJson(res, 200, {
        ok: true,
        text: result.text,
        ...(result.alt === undefined ? {} : { alt: result.alt }),
        ...(result.cached === true ? { cached: true } : {}),
      });
      return;
    }
    if (pathname === '/api/learn') {
      // 讲解这条路原生的节点图会直接调，所以它比别的路由多带一个 view：
      // 卡片上的标题/正文/提示都是宿主这边写好的（见 lesson.ts），
      // 原生只画字。旧宿主没有这个字段，原生据此把「讲讲它」置灰。
      if (req.method !== 'POST') {
        sendJson(res, 405, {
          ok: false,
          error: 'method not allowed',
          view: lessonFailure(405, 'method not allowed'),
        });
        return;
      }
      if (options.learn === undefined) {
        sendJson(res, 503, {
          ok: false,
          error: '宿主没有提供讲解能力',
          view: lessonFailure(503, '宿主没有提供讲解能力'),
        });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, {
          ok: false,
          error: body.reason,
          view: lessonFailure(body.status, body.reason),
        });
        return;
      }
      const payload = body.value as { nodeId?: unknown };
      const nodeId = typeof payload.nodeId === 'string' && payload.nodeId !== '' ? payload.nodeId : undefined;
      const result = await options.learn.request(nodeId);
      if (!result.ok) {
        sendJson(res, result.status, {
          ok: false,
          error: result.reason,
          view: lessonFailure(result.status, result.reason),
        });
        return;
      }
      sendJson(res, 200, {
        ok: true,
        text: result.text,
        ...(result.alt === undefined ? {} : { alt: result.alt }),
        view: lessonSuccess(result.text, result.alt),
      });
      return;
    }
    if (pathname === '/api/chat') {
      // 追问：讲解之后接着问一句。和 /api/learn 不同，这条路**不落库**——
      // 问一句就写一次 work.json 的话，任务库会被问答灌满，而问答本来就不是任务状态。
      if (req.method !== 'POST') {
        sendJson(res, 405, { ok: false, error: 'method not allowed' });
        return;
      }
      if (options.chat === undefined) {
        sendJson(res, 503, { ok: false, error: '宿主没有提供追问能力' });
        return;
      }
      const body = await readJsonBody(req);
      if (!body.ok) {
        sendJson(res, body.status, { ok: false, error: body.reason });
        return;
      }
      const payload = body.value as { nodeId?: unknown; question?: unknown; history?: unknown };
      const question = typeof payload.question === 'string' ? payload.question.trim() : '';
      if (question === '') {
        sendJson(res, 400, { ok: false, error: '缺少要问的问题' });
        return;
      }
      if (question.length > MAX_QUESTION_CHARS) {
        sendJson(res, 400, { ok: false, error: `问题太长了（最多 ${MAX_QUESTION_CHARS} 字）` });
        return;
      }
      const nodeId = typeof payload.nodeId === 'string' && payload.nodeId !== '' ? payload.nodeId : undefined;
      const history: ChatTurn[] = Array.isArray(payload.history)
        ? payload.history
            .filter((turn): turn is { question: string; answer: string } => {
              if (typeof turn !== 'object' || turn === null) return false;
              const item = turn as { question?: unknown; answer?: unknown };
              return typeof item.question === 'string' && typeof item.answer === 'string';
            })
            .map((turn) => ({ question: turn.question, answer: turn.answer }))
        : [];
      const asked = await options.chat.request({ nodeId, question, history });
      if (!asked.ok) {
        sendJson(res, asked.status, { ok: false, error: asked.reason });
        return;
      }
      sendJson(res, 200, { ok: true, answer: asked.answer });
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
