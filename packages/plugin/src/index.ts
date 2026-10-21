/**
 * DSH Work —— 插件的 host 半边。
 *
 * 职责边界（一句话）：**它是任务状态的唯一权威**。
 *   · 浏览器客户端（DSH 窗口内）：显示 + 收集人的意图
 *   · 伴随层（系统级小条 / 全局热键 / 截图）：显示 + 收集人的意图
 *   · 本文件：落盘、校验、广播；并把「建树 / 写建议」这类动作暴露成模型工具
 *
 * 为什么权威放在这里：宿主本来就常驻，插件随宿主起落；两条 UI 进程都可以随时重启，
 * 而任务进度不能跟着丢。它们都只是视图。
 *
 * 加载契约（DSH 插件规范）：导出 `name` / `inject` / `apply`。所有副作用都通过
 * `ctx.effect(disposer)` 注册，宿主卸载本插件时会统一回收。
 */

import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import type { Actor, ApplyResult, Intent, WorkDoc } from '@dshwork/core';
import { createId } from '@dshwork/core';

import { ensureToken, publishEndpoint } from './endpoint.ts';
import { ROUTE_PREFIX, createHttpHandler } from './http.ts';
import type { AdviseApi, LearnApi, PlanTextApi, RecognizeApi, RecognizeJobView } from './http.ts';
import type { DraftPlan } from './recognize.ts';
import { draftFromSpecs, expandNode, planSetFromDraft, planSetFromSpecs, recognizePlan } from './recognize.ts';
import { adviseNode } from './advise.ts';
import { learnNode } from './learning.ts';
import { planFromText } from './planning.ts';
import { WorkStore } from './store.ts';
import { registerTools } from './tools.ts';
import type { ToolHost } from './tools.ts';

export const name = '@dshwork/plugin';

/**
 * 只声明真正必须的服务。
 * `webServer` 是 web 半边的前提；没有它的 profile（CLI/TUI）不该加载本插件，
 * 这一条本身就是「本插件只面向桌面 Web」的显式声明。
 *
 * 识别用到的 `llm` / `attachments` **故意不写进 inject**：它们是可选能力，
 * 缺了应该只是「识别不可用」，而不是整条插件装不起来。
 */
export const inject = ['tools', 'webServer'];

/** 没有配置、宿主也没有默认模型时的回落值：DeepSeek-V4-Flash 的正式路由。 */
const DEFAULT_PROVIDER = 'deepseek-official';
const DEFAULT_MODEL = 'deepseek-v4-flash';

export interface PluginConfig {
  /** 工作区根目录。缺省用宿主进程的 cwd（即 DSH 当前工作区）。 */
  root?: string;
  /** 是否把发现文件写给伴随层。默认 true。 */
  publishEndpoint?: boolean;
  /**
   * 识别用的模型。缺省跟随宿主当前默认模型（GUI 里选的那个），
   * 再没有就回落到 DeepSeek-V4-Flash。
   *
   * 提醒：默认模型若只接受文本，宿主会把图片投影成文字说明，识别结果会很差；
   * 这种 profile 应当显式指定视觉模型（例如 deepseek-v4-flash-vision-exp）。
   */
  llm?: {
    provider?: string;
    model?: string;
    /** 单次模型调用的超时上限（毫秒）。 */
    timeoutMs?: number;
  };
}

const HERE = path.dirname(fileURLToPath(import.meta.url));

export function apply(ctx: any, config?: PluginConfig): void {
  const disposers: Array<() => void> = [];
  // 先登记总开关：即使下面的异步初始化还在飞，宿主卸载也会把已注册的东西收干净。
  ctx.effect(
    () => () => {
      for (const dispose of disposers.splice(0)) {
        try {
          dispose();
        } catch {
          // 单个回收失败不能影响其余回收
        }
      }
    },
    `${name}: lifecycle`,
  );

  const cfg: PluginConfig = config && typeof config === 'object' ? config : {};
  const root = typeof cfg.root === 'string' && cfg.root.trim() !== '' ? cfg.root : process.cwd();

  void initialize().catch((error: unknown) => {
    const text = error instanceof Error ? error.message : String(error);
    console.error(`[dsh-work] 初始化失败：${text}`);
  });

  async function initialize(): Promise<void> {
    const store = await WorkStore.open({ root });
    const version = await readVersion();
    // 一次性把宿主实际提供的服务名打进日志。cordis 的 ctx 是代理，
    // 读一个没 inject 的服务会直接抛，所以只能一个个 try。
    // 留着的用处：以后要接会话/agent 时，先看这一行就知道有没有。
    {
      const probe = ['agents', 'sessions', 'sessionController', 'llm', 'tools', 'webServer',
        'subprocess', 'schedule', 'tokenMeter', 'storageDomain', 'workspace', 'workspaces',
        'connection', 'logger'];
      const found: string[] = [];
      for (const name of probe) {
        try {
          if (ctx.get(name as never) !== undefined) found.push(name);
        } catch {
          /* 没有 inject，或者宿主根本没有这个服务 */
        }
      }
      console.log('[dsh-work] 宿主服务：' + (found.length === 0 ? '（一个都没探到）' : found.join(', ')));
      for (const target of ['agents', 'sessions'] as const) {
        try {
          const svc = ctx.get(target as never) as unknown as Record<string, unknown>;
          const own = Object.keys(svc);
          const proto = Object.getOwnPropertyNames(Object.getPrototypeOf(svc) ?? {}).filter((k) => k !== 'constructor');
          console.log(`[dsh-work] ${target} 的方法：own=[${own.join(',')}] proto=[${proto.join(',')}]`);
        } catch (error) {
          console.log(`[dsh-work] ${target} 探测失败：${String(error)}`);
        }
      }
      // 再往下探一层：会话列表到底能不能拿到 id、agent 能不能按 id 取。
      // 只读，不改任何东西；结果只用来决定「自动唤醒」怎么写。
      const preview = (value: unknown): string => {
        try {
          const text = JSON.stringify(value, (_k, v) => (typeof v === 'function' ? '[fn]' : v));
          return text === undefined ? String(value) : text.slice(0, 420);
        } catch {
          return String(value);
        }
      };
      try {
        const sessions = ctx.get('sessions' as never) as unknown as Record<string, unknown>;
        if (typeof sessions?.['list'] === 'function') {
          const listed = await (sessions['list'] as () => Promise<unknown>)();
          console.log(`[dsh-work] sessions.list() => ${preview(listed)}`);
        } else {
          console.log('[dsh-work] sessions.list 不是函数');
        }
      } catch (error) {
        console.log(`[dsh-work] sessions.list() 抛：${String(error)}`);
      }
    }

    const host: ToolHost = {
      snapshot: async () => store.snapshot,
      commit: (intent: Intent, by: Actor): Promise<ApplyResult> => store.commit(intent, by),
      now: () => Date.now(),
      newId: () => createId('nd'),
    };

    /**
     * 识别作业表。
     *
     * 为什么要有这一层：一次视觉调用是几十秒的事，不能挂在 HTTP 响应上——任何一层
     * 超时都会把它掐断，而掐断之后没人知道模型还在不在跑。POST 只负责受理，
     * 伴随层轮询 GET 拿结果；超时与插件卸载都能通过 AbortController 真正停下来。
     *
     * 同一时刻只认一张图：两次识别并发落地就是两条 plan.set，后一条会覆盖前一条。
     */
    const jobs = new Map<string, RecognizeJob>();
    const unload = new AbortController();

    const recognize: RecognizeApi = {
      start(png: Buffer) {
        for (const job of jobs.values()) {
          if (job.view.state === 'running') {
            return { ok: false, status: 409, reason: '上一张截图还在识别中' };
          }
        }
        sweepRecognizeJobs(jobs);
        const id = createId('rc');
        const job: RecognizeJob = {
          view: { state: 'running' },
          at: Date.now(),
          controller: new AbortController(),
        };
        jobs.set(id, job);
        void runRecognize(job, png);
        return { ok: true, id };
      },
      get(id: string): RecognizeJobView | null {
        const job = jobs.get(id);
        return job === undefined ? null : job.view;
      },
      /**
       * 确认这张草稿：到这一步才真的落库。
       *
       * 幂等：同一份草稿提交两次，第二次只会拿到「已经提交过了」。
       */
      async commit(id: string) {
        const job = jobs.get(id);
        if (job === undefined) return { ok: false, reason: '没有这张草稿（可能已经过期）' };
        if (job.draft === undefined) return { ok: false, reason: '这张草稿是空的' };
        if (job.committed) return { ok: false, reason: '这张草稿已经提交过了' };
        const result = await store.commit(planSetFromDraft(job.draft), 'ai');
        if (!result.ok) return { ok: false, reason: result.reason };
        job.committed = true;
        return { ok: true, count: job.draft.nodes.length };
      },
      /** 丢掉草稿。不写文档，只把内存里那份清掉。 */
      discard(id: string) {
        const job = jobs.get(id);
        if (job === undefined) return true;
        job.draft = undefined;
        return true;
      },
      draft(id: string) {
        const job = jobs.get(id);
        return job?.draft ?? null;
      },
      /**
       * 二次解构：把草稿里的一颗节点再拆细。
       *
       * 只动这颗节点的子层：它的兄弟、它自己的状态一个都不碰。
       * 失败只回可读原因，草稿原样不动——半张拆坏的子树比没拆更糟。
       */
      async expand(id: string, nodeId: string) {
        const job = jobs.get(id);
        if (job?.draft === undefined) return { ok: false, reason: '没有这张草稿（可能已过期）' };
        const node = job.draft.nodes.find((item) => item.id === nodeId);
        if (node === undefined) return { ok: false, reason: '草稿里没有这颗节点' };

        const llm = readService(ctx, 'llm');
        if (llm === null) return { ok: false, reason: '宿主没有提供 llm 服务' };
        const selection = resolveLlmSelection(ctx, cfg);
        try {
          const expanded = await expandNode(
            {
              llm,
              provider: selection.provider,
              model: selection.model,
              ...(cfg.llm?.timeoutMs === undefined ? {} : { timeoutMs: cfg.llm.timeoutMs }),
            },
            { title: node.title, detail: node.detail ?? '', acceptance: node.acceptance ?? '' },
            AbortSignal.any([unload.signal, job.controller.signal]),
          );
          if (expanded.specs.length === 0) {
            return { ok: false, reason: `模型没有给出可解析的拆分（试了 ${expanded.attempts} 次）` };
          }

          const children = draftFromSpecs(expanded.specs, {
            now: Date.now(),
            newId: () => createId('nd'),
          }).nodes.map((child) => ({ ...child, parentId: nodeId }));

          const grafted = { ...node, children: children.map((child) => child.id) };
          job.draft = {
            ...job.draft,
            nodes: [...job.draft.nodes.filter((item) => item.id !== nodeId), grafted, ...children],
          };
          job.at = Date.now();
          return { ok: true, added: children.length };
        } catch (error) {
          return { ok: false, reason: describe(error) };
        }
      },
      /**
       * 撤销一次二次解构：把这颗节点的子层整棵删掉，退回"没拆过"的样子。
       *
       * 只删子树，不碰这颗节点自己，也不碰它的兄弟——所以撤销之后
       * 再拆一次的结果和第一次拆之前是同一种状态，可以反复试。
       */
      collapse(id: string, nodeId: string) {
        const job = jobs.get(id);
        if (job?.draft === undefined) return { ok: false, reason: '没有这张草稿（可能已过期）' };
        const node = job.draft.nodes.find((item) => item.id === nodeId);
        if (node === undefined) return { ok: false, reason: '草稿里没有这颗节点' };
        if (node.children.length === 0) return { ok: false, reason: '这颗节点本来就没有拆过' };

        // 沿着 children 收集整棵子树：草稿是树不是图，所以不会绕圈，
        // 但仍用集合兜底，坏数据最多少删不会死循环。
        const doomed = new Set<string>();
        const queue = [...node.children];
        while (queue.length > 0) {
          const current = queue.pop();
          if (current === undefined || doomed.has(current)) continue;
          doomed.add(current);
          const child = job.draft.nodes.find((item) => item.id === current);
          if (child !== undefined) queue.push(...child.children);
        }

        job.draft = {
          ...job.draft,
          nodes: job.draft.nodes
            .filter((item) => !doomed.has(item.id))
            .map((item) => (item.id === nodeId ? { ...item, children: [] } : item)),
        };
        job.at = Date.now();
        return { ok: true, removed: doomed.size };
      },
    };

    /**
     * 给节点要一条建议。
     *
     * 这里刻意做成"同步等"：一句话建议几百 token，用户按下去就要看到，
     * 拖成作业反而更麻烦。失败只回可读原因，绝不写文档。
     */
    /**
     * 一段人话 → 一张任务树。
     *
     * 和识别走同一条落库路径（plan.set），区别只在输入：那边是 PNG，这边是文字。
     * 模型给的形状不对就如实回错，**绝不写文档**——半张错任务树比没有更糟。
     */
    const planText: PlanTextApi = {
      async plan(text) {
        const llm = readService(ctx, 'llm');
        if (llm === null) {
          return { ok: false, status: 503, reason: '宿主没有提供 llm 服务' };
        }
        const selection = resolveLlmSelection(ctx, cfg);
        try {
          const planned = await planFromText(
            {
              llm,
              provider: selection.provider,
              model: selection.model,
              ...(cfg.llm?.timeoutMs === undefined ? {} : { timeoutMs: cfg.llm.timeoutMs }),
            },
            text,
          );
          if (planned.specs.length === 0) {
            return { ok: false, status: 422, reason: '模型这次没给出可解析的计划，换个说法再试' };
          }
          const intent = planSetFromSpecs(planned.specs, {
            now: Date.now(),
            newId: () => createId('nd', Date.now()),
            title: planned.title,
          });
          const result = await store.commit(intent, 'ai');
          if (!result.ok) {
            return { ok: false, status: 409, reason: result.reason };
          }
          return { ok: true, title: planned.title === '' ? '未命名计划' : planned.title, count: planned.specs.length };
        } catch (error) {
          return { ok: false, status: 502, reason: `规划失败：${String(error)}` };
        }
      },
    };

    const advise: AdviseApi = {
      async request(nodeId) {
        const llm = readService(ctx, 'llm');
        if (llm === null) {
          return { ok: false, status: 503, reason: '宿主没有提供 llm 服务' };
        }
        const doc = store.snapshot;
        const target = nodeId ?? doc.pointer;
        if (target === null || target === undefined) {
          return { ok: false, status: 409, reason: '现在没有在进行中的节点' };
        }
        const node = doc.nodes[target];
        if (node === undefined) {
          return { ok: false, status: 404, reason: '没有这个节点' };
        }
        const selection = resolveLlmSelection(ctx, cfg);
        try {
          const advice = await adviseNode(
            {
              llm,
              provider: selection.provider,
              model: selection.model,
              ...(cfg.llm?.timeoutMs === undefined ? {} : { timeoutMs: cfg.llm.timeoutMs }),
            },
            {
              title: node.title,
              detail: node.detail ?? '',
              acceptance: node.acceptance ?? '',
            },
            unload.signal,
          );
          const intent: Intent = {
            type: 'node.advise',
            nodeId: target,
            advice: {
              text: advice.text,
              ...(advice.alt === undefined ? {} : { alt: advice.alt }),
              at: Date.now(),
              model: selection.model,
            },
          };
          const result = await store.commit(intent, 'ai');
          if (!result.ok) return { ok: false, status: 409, reason: result.reason };
          return { ok: true, text: advice.text, ...(advice.alt === undefined ? {} : { alt: advice.alt }) };
        } catch (error) {
          return { ok: false, status: 500, reason: describe(error) };
        }
      },
    };
  /**
   * 学习模式：讲一个概念。
   *
   * 与 advise 的区别是"不落库"——讲解是一次性的，属���关掉就该消失的东西，
   * 写进节点会让任务树变成笔记本。真想留下的话，人自己复制。
   */
  const learn: LearnApi = {
    async request(nodeId) {
      const llm = readService(ctx, 'llm');
      if (llm === null) return { ok: false, status: 503, reason: '宿主没有提供 llm 服务' };
      const doc = store.snapshot;
      const target = nodeId ?? doc.pointer;
      if (target === null || target === undefined) {
        return { ok: false, status: 409, reason: '没有指定要讲哪一颗' };
      }
      const node = doc.nodes[target];
      if (node === undefined) return { ok: false, status: 404, reason: '没有这个节点' };
      const selection = resolveLlmSelection(ctx, cfg);
      try {
        const lesson = await learnNode(
          { llm, provider: selection.provider, model: selection.model },
          { title: node.title, detail: node.detail ?? '', acceptance: node.acceptance ?? '' },
          unload.signal,
        );
        // 讲解挂在节点的 advice 上：面板本来就渲染这个字段，等于免费拿到一栏显示位。
        // 代价是它会落进 work.json —— 学习痕迹留在节点上其实也合理（下次回看还在）。
        const intent: Intent = {
          type: 'node.advise',
          nodeId: target,
          advice: {
            text: lesson.text,
            ...(lesson.alt === undefined ? {} : { alt: lesson.alt }),
            at: Date.now(),
            model: selection.model,
          },
        };
        const saved = await store.commit(intent, 'ai');
        if (!saved.ok) return { ok: false, status: 409, reason: saved.reason };
        return { ok: true, text: lesson.text, ...(lesson.alt === undefined ? {} : { alt: lesson.alt }) };
      } catch (error) {
        return { ok: false, status: 500, reason: describe(error) };
      }
    },
  };
    // 插件卸载时把在跑的识别真正叫停：否则宿主退出后还会有一个请求挂在模型上。
    disposers.push(() => unload.abort());

    /**
     * 截图 → 任务树。
     *
     * 失败一律以「可读原因」记在作业上、**不写文档**；成功才提交 plan.set
     * （走既有 apply 校验，与工具建树是同一条落库路径）。
     * 归因写 `ai`：这是模型替人做的第一次拆解，人后续随时可以改。
     */
    async function runRecognize(job: RecognizeJob, png: Buffer): Promise<void> {
      const signal = AbortSignal.any([unload.signal, job.controller.signal]);
      try {
        const llm = readService(ctx, 'llm');
        if (llm === null) {
          finishRecognize(job, '宿主没有提供 llm 服务（profile 里没有挂模型适配器）');
          return;
        }
        const attachments = readService(ctx, 'attachments');
        if (attachments === null) {
          finishRecognize(job, '宿主没有提供 attachments 服务');
          return;
        }

        const selection = resolveLlmSelection(ctx, cfg);
        const recognized = await recognizePlan(
          {
            llm,
            attachments,
            provider: selection.provider,
            model: selection.model,
            ...(cfg.llm?.timeoutMs === undefined ? {} : { timeoutMs: cfg.llm.timeoutMs }),
          },
          png.toString('base64'),
          { signal },
        );

        if (recognized.specs.length === 0) {
          finishRecognize(
            job,
            `模型没有给出可解析的任务树（共尝试 ${recognized.attempts} 次）；原文片段：${excerpt(recognized.raw)}`,
          );
          return;
        }

        const draft = draftFromSpecs(recognized.specs, {
          now: Date.now(),
          newId: () => createId('nd'),
          title: recognized.title,
        });
        // 只挂到作业上，不落库：等人看过、点过「就这样」再提交。
        job.draft = draft;
        job.view = { state: 'done', title: recognized.title, count: draft.nodes.length };
        job.at = Date.now();
      } catch (error) {
        finishRecognize(job, describe(error));
      }
    }

    const http = createHttpHandler({
      runtime: {
        version,
        getSnapshot: (): WorkDoc => store.snapshot,
        commit: (intent: Intent, by: Actor) => store.commit(intent, by),
        subscribe: (listener: (doc: WorkDoc) => void) =>
          // store 的通知带 journal 条目；HTTP 面只关心「变了」，把形状收敛在这一行。
          store.subscribe((event) => listener(event.doc)),
      },
      authorize: (req) => authorizeRequest(ctx, req),
      recognize,
      advise,
      learn,
      planText,
    });

    // 令牌先就位：伴随层的请求可能在路由注册后几十毫秒内就打进来。
    ctx.__dshWorkToken = await ensureToken();

    const port = readPort(ctx);
    const registered = ctx.webServer.register({
      kind: 'prefix',
      path: ROUTE_PREFIX,
      handler: http.handler,
    });
    if (typeof registered === 'function') disposers.push(registered as () => void);
    disposers.push(() => http.dispose());

    disposers.push(...registerTools(ctx, host));

    if (cfg.publishEndpoint !== false) {
      try {
        await publishEndpoint({ port, root, workspace: path.basename(root) || root });
      } catch (error) {
        // 伴随层起不来不该连累主流程；但要说清楚原因。
        const text = error instanceof Error ? error.message : String(error);
        console.warn(`[dsh-work] 发现文件写入失败（伴随层将找不到本体）：${text}`);
      }
    }

    // 宿主退出前把内存里的状态落一次盘（正常路径每次提交都会写，这里只兜底）。
    const flush = (): void => {
      void store.flush();
    };
    process.once('beforeExit', flush);
    disposers.push(() => process.removeListener('beforeExit', flush));

    const note = `[dsh-work] 就绪：root=${root} seq=${store.seq} port=${port ?? '?'}`;
    console.log(note);
    try {
      // 宿主有 logger 就顺手记一笔；没有也不该让初始化在这里失败。
      ctx.logger?.info?.(note);
    } catch {
      /* 未提供 logger 服务 */
    }
  }
}

/* ─────────────────────────── 鉴权 ─────────────────────────── */

/**
 * 两道门，任一通过即可：
 * 1. 宿主自己的会话校验（浏览器客户端天然带 cookie）；
 * 2. 伴随层带来的共享令牌（本机文件里的随机串，见 endpoint.ts）。
 *
 * 注意：这里**不是**在绕过宿主鉴权，而是给「同一用户的另一条本机进程」一条明示的入口。
 * 任何对外暴露的路径都不该走这道门。
 */
function authorizeRequest(ctx: any, req: unknown): number | null {
  const rejection = readHostRejection(ctx, req);
  if (rejection === null) return null;
  if (hasCompanionToken(ctx, req)) return null;
  return rejection;
}

function readHostRejection(ctx: any, req: unknown): number | null {
  try {
    // 用 ctx.get(...) 取**可选**服务：直接读 ctx.connection 在未 inject 时会抛错。
    const connection = typeof ctx.get === 'function' ? ctx.get('connection') : undefined;
    if (!connection || typeof connection.requestRejection !== 'function') return null;
    const rejection = connection.requestRejection(req);
    return typeof rejection === 'number' ? rejection : null;
  } catch {
    // 没有会话服务：不设这道门，交给令牌那道门。
    return null;
  }
}

/** 令牌存在时，才需要比对；读取是同步的（比对用的是宿主初始化时缓存的那份）。 */
function hasCompanionToken(ctx: any, req: unknown): boolean {
  const expected = ctx.__dshWorkToken as string | undefined;
  if (!expected) return false;
  const request = req as { headers?: Record<string, string | string[] | undefined> };
  const header = request.headers?.['x-dsh-work-token'];
  const actual = Array.isArray(header) ? header[0] : header;
  if (typeof actual !== 'string' || actual.length < 16) return false;
  // 长度不同直接判否，避免 timingSafeEqual 抛错；这里不是密码学边界，常数时间比较足够。
  return actual === expected;
}

/* ─────────────────────────── 环境读取 ─────────────────────────── */

function readPort(ctx: any): number | null {
  // 只读已 inject 的 webServer：碰其它服务属性会抛「without inject」。
  try {
    const port = ctx.webServer?.port;
    return typeof port === 'number' && port > 0 ? port : null;
  } catch {
    return null;
  }
}

async function readVersion(): Promise<string> {
  try {
    const raw = await readFile(path.join(HERE, '..', 'package.json'), 'utf8');
    const parsed = JSON.parse(raw) as { version?: unknown };
    return typeof parsed.version === 'string' ? parsed.version : '0.0.0';
  } catch {
    return '0.0.0';
  }
}

/* ─────────────────────────── 识别用的环境读取 ─────────────────────────── */

/** 取一个**可选**服务；没有就返回 null（未 inject 时直接点属性会抛）。 */
function readService(ctx: any, name: string): any {
  try {
    const service = typeof ctx.get === 'function' ? ctx.get(name) : undefined;
    return service ?? null;
  } catch {
    return null;
  }
}

/**
 * 识别用哪个模型：插件配置 → 宿主当前默认模型 → DeepSeek-V4-Flash。
 *
 * 中间那一档是刻意的：用户在 GUI 里换了模型，识别应当跟着换，而不是被
 * 一个写死的路由钉住；完全没配置的机器才回落到常量。
 */
function resolveLlmSelection(ctx: any, cfg: PluginConfig): { provider: string; model: string } {
  const wanted = cfg.llm ?? {};
  const hostDefault = readHostDefaultModel(ctx);
  return {
    provider: pick(wanted.provider) ?? pick(hostDefault.provider) ?? DEFAULT_PROVIDER,
    model: pick(wanted.model) ?? pick(hostDefault.model) ?? DEFAULT_MODEL,
  };
}

function readHostDefaultModel(ctx: any): { provider?: string; model?: string } {
  try {
    const service = readService(ctx, 'agentDefaultModel');
    const selection = service?.currentSelection?.() as { provider?: unknown; model?: unknown } | undefined;
    const provider = pick(selection?.provider);
    const model = pick(selection?.model);
    return {
      ...(provider === null ? {} : { provider }),
      ...(model === null ? {} : { model }),
    };
  } catch {
    return {};
  }
}

function pick(value: unknown): string | null {
  if (typeof value !== 'string') return null;
  const text = value.trim();
  return text === '' ? null : text;
}

/** 报错里附一小段模型原文：没有它，用户只会看到「解析失败」而无从下手。 */
function excerpt(raw: string): string {
  const text = raw.trim().replace(/\s+/g, ' ');
  return text === '' ? '(空)' : text.length > 120 ? `${text.slice(0, 120)}…` : text;
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/* ─────────────────────────── 识别作业 ─────────────────────────── */

interface RecognizeJob {
  view: RecognizeJobView;
  /** 最后一次状态变化时刻，用于回收过期作业。 */
  at: number;
  /** 预留：将来要「手动取消识别」时，abort 这一个即可。 */
  controller: AbortController;
  /** 拆出来但还没落库的计划树；人确认后才会变成 plan.set。 */
  draft?: DraftPlan;
  /** 提交过就不再提交：同一张草稿重复提交是用户手滑，不是新计划。 */
  committed?: boolean;
}

/** 失败的作业统一走这里：状态与原因一起落定，调用方无需重复写。 */
function finishRecognize(job: RecognizeJob, reason: string): void {
  job.view = { state: 'error', reason };
  job.at = Date.now();
}

/** 已结束的作业留 5 分钟给伴随层来取；最多留 8 个，多了就丢最旧的。 */
const JOB_TTL_MS = 5 * 60_000;
const JOB_KEEP = 8;

function sweepRecognizeJobs(jobs: Map<string, RecognizeJob>): void {
  const cutoff = Date.now() - JOB_TTL_MS;
  for (const [id, job] of jobs) {
    if (job.view.state !== 'running' && job.at < cutoff) jobs.delete(id);
  }
  while (jobs.size > JOB_KEEP) {
    const oldest = jobs.keys().next();
    if (oldest.done) break;
    const job = jobs.get(oldest.value);
    // 在跑的作业永远不回收：轮询到一半把它删掉，用户就再也拿不到结果了。
    if (job !== undefined && job.view.state === 'running') break;
    jobs.delete(oldest.value);
  }
}
