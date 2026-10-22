/**
 * 「打开面板」指令：浏览器按钮把意愿交给宿主，桌面伴随层轮询时取走。
 *
 * 为什么绕一道宿主：浏览器（dsh-app:// 或 http）碰不到 Win32 窗口，而
 * 伴随层本来就每秒拉一次 /api/state。把指令挂在状态上，就不必为这一件
 * 事新增一条长连接；宿主重启后 seq 会归零，所以伴随层按时间戳判新旧。
 */

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';

export type UiOpenTarget = 'panel' | 'graph';

export interface UiCommand {
  seq: number;
  target: UiOpenTarget;
  /** 指令生成时刻（epoch ms）。伴随层按它判新旧与新鲜度。 */
  at: number;
}

export interface UiOpenOutcome {
  command: UiCommand;
  /** 伴随层此刻在线（最近几秒轮询过）。 */
  companion: boolean;
  /** 宿主这次是否顺手把它拉起来了。 */
  launched: boolean;
  /** 没连上、也没能拉起时，给客户端一句能照着做的话。 */
  hint?: string;
}

export interface UiBridge {
  /** 最近一条指令；从没点过时 seq=0、at=0。 */
  snapshot(): UiCommand;
  companionAlive(): boolean;
  /** 收到一次带伴随层令牌的请求：证明它活着。 */
  noteCompanion(): void;
  open(target: UiOpenTarget): UiOpenOutcome;
}

export interface UiBridgeOptions {
  now?: () => number;
  /** 把伴随层拉起来；返回是否真的起了。 */
  launch?: () => boolean;
  /** 多少毫秒内有轮询就算在线。 */
  aliveWindowMs?: number;
  /** 两次自动拉起之间的最小间隔：连点两下不该开出两个托盘图标。 */
  launchCooldownMs?: number;
}

const DEFAULT_ALIVE_WINDOW_MS = 5_000;
const DEFAULT_LAUNCH_COOLDOWN_MS = 10_000;

function offlineHint(hasLauncher: boolean, cooledDown: boolean): string {
  if (!hasLauncher) return '桌面伴随层没在运行';
  if (cooledDown) return '桌面伴随层刚拉起过，等它连上来再试一次';
  return '桌面伴随层启动失败：看看 dshwork-shell 的日志';
}

export function createUiBridge(options: UiBridgeOptions = {}): UiBridge {
  const now = options.now ?? Date.now;
  const aliveWindow = options.aliveWindowMs ?? DEFAULT_ALIVE_WINDOW_MS;
  const launchCooldown = options.launchCooldownMs ?? DEFAULT_LAUNCH_COOLDOWN_MS;
  let seq = 0;
  let command: UiCommand = { seq: 0, target: 'panel', at: 0 };
  let lastCompanionAt = Number.NEGATIVE_INFINITY;
  let lastLaunchAt = Number.NEGATIVE_INFINITY;

  const companionAlive = (): boolean => now() - lastCompanionAt <= aliveWindow;

  return {
    snapshot: () => command,
    companionAlive,
    noteCompanion() {
      lastCompanionAt = now();
    },
    open(target) {
      seq += 1;
      command = { seq, target, at: now() };
      if (companionAlive()) return { command, companion: true, launched: false };

      if (options.launch) {
        const cooledDown = now() - lastLaunchAt <= launchCooldown;
        if (!cooledDown) {
          lastLaunchAt = now();
          let launched = false;
          try {
            launched = options.launch();
          } catch {
            launched = false;
          }
          if (launched) return { command, companion: false, launched: true };
        }
        return {
          command,
          companion: false,
          launched: false,
          hint: offlineHint(true, cooledDown),
        };
      }

      return { command, companion: false, launched: false, hint: offlineHint(false, false) };
    },
  };
}

/** 开发布局里伴随层可执行文件的位置；按顺序取第一个真实存在的。 */
export function resolveCompanionExecutable(candidates: Array<string | undefined>): string | null {
  for (const candidate of candidates) {
    if (typeof candidate !== 'string' || candidate.trim() === '') continue;
    try {
      if (existsSync(candidate)) return candidate;
    } catch {
      /* 权限/非法路径都当作没有 */
    }
  }
  return null;
}

/** 拉一个脱离宿主的伴随层：宿主退出/重启不该把它带走。 */
export function spawnCompanion(executable: string): boolean {
  try {
    const child = spawn(executable, [], { detached: true, stdio: 'ignore', windowsHide: true });
    // spawn 找不到文件时不会抛，而是异步 emit('error')；不接会掀掉整个宿主。
    child.on('error', () => undefined);
    child.unref();
    return true;
  } catch {
    return false;
  }
}
