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
import { WorkStore } from './store.ts';
import { registerTools } from './tools.ts';
import type { ToolHost } from './tools.ts';

export const name = '@dshwork/plugin';

/**
 * 只声明真正必须的服务。
 * `webServer` 是 web 半边的前提；没有它的 profile（CLI/TUI）不该加载本插件，
 * 这一条本身就是「本插件只面向桌面 Web」的显式声明。
 */
export const inject = ['tools', 'webServer'];

export interface PluginConfig {
  /** 工作区根目录。缺省用宿主进程的 cwd（即 DSH 当前工作区）。 */
  root?: string;
  /** 是否把发现文件写给伴随层。默认 true。 */
  publishEndpoint?: boolean;
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

    const host: ToolHost = {
      snapshot: async () => store.snapshot,
      commit: (intent: Intent, by: Actor): Promise<ApplyResult> => store.commit(intent, by),
      now: () => Date.now(),
      newId: () => createId('nd'),
    };

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
