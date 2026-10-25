// 用量账本：这一趟宿主进程里，AI 一共被问了几次、花了多少 token。
//
// 为什么要有这个文件：用户问过「一次会话到底花了多少」，而模型调用散在识别、
// 建议、讲解、追问、拆解五条路上，各写各的迟早对不上。收在一处记账，
// 界面只读这一本账。
//
// 数据来源是宿主 llm 流里的 `usage` 分片（实测形状：
// `{"type":"usage","usage":{"inputTokens":144,"outputTokens":357,"cacheReadTokens":128,
// "cacheWriteTokens":0,"totalTokens":629}}`）。宿主不给就**什么都不写**——
// 宁可显示「还不知道」，也不拿字数硬换成 token 糊弄人。

/** 宿主报回来的一次调用用量。字段全可选：不同 provider 给的不一样。 */
export interface UsageNumbers {
  inputTokens?: number;
  outputTokens?: number;
  cacheReadTokens?: number;
  cacheWriteTokens?: number;
  totalTokens?: number;
}

export interface UsageSnapshot {
  /** 发起过多少次模型调用（含失败的）。 */
  calls: number;
  /** 其中失败的次数。失败可能已经计费，所以单独记，不从 calls 里减。 */
  failed: number;
  /** 宿主报回来的 token 数，累加。 */
  inputTokens: number;
  outputTokens: number;
  cacheReadTokens: number;
  /** 因为「内容没变」而直接复用的建议，省下的调用次数。 */
  savedCalls: number;
  /** 第一次调用发生的时刻（毫秒），没调用过是 null。 */
  startedAt: number | null;
}

function toCount(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) && value > 0 ? Math.round(value) : 0;
}

/** 从流里的一片 `usage` 里取出数字。形状不对返回 null。 */
export function readUsage(chunk: unknown): UsageNumbers | null {
  if (typeof chunk !== 'object' || chunk === null) return null;
  const record = chunk as { type?: unknown; usage?: unknown };
  if (record.type !== 'usage') return null;
  if (typeof record.usage !== 'object' || record.usage === null) return null;
  const usage = record.usage as Record<string, unknown>;
  const numbers: UsageNumbers = {
    inputTokens: toCount(usage['inputTokens']),
    outputTokens: toCount(usage['outputTokens']),
    cacheReadTokens: toCount(usage['cacheReadTokens']),
  };
  // 一个数字都没有的分片没有信息量，不要让它把「开始了」记成一次用量。
  if (numbers.inputTokens === 0 && numbers.outputTokens === 0 && numbers.cacheReadTokens === 0) {
    return null;
  }
  return numbers;
}

export class UsageMeter {
  private calls = 0;
  private failed = 0;
  private inputTokens = 0;
  private outputTokens = 0;
  private cacheReadTokens = 0;
  private savedCalls = 0;
  private startedAt: number | null = null;

  /** 调用发起前记一笔。失败也要记——失败通常一样计费。 */
  callStarted(now: number = Date.now()): void {
    this.calls += 1;
    if (this.startedAt === null) this.startedAt = now;
  }

  /** 收到 usage 分片时累加。 */
  addUsage(numbers: UsageNumbers): void {
    this.inputTokens += toCount(numbers.inputTokens);
    this.outputTokens += toCount(numbers.outputTokens);
    this.cacheReadTokens += toCount(numbers.cacheReadTokens);
  }

  callFailed(): void {
    this.failed += 1;
  }

  /** 建议没问模型就复用了，记一笔「省下的调用」。 */
  savedCall(): void {
    this.savedCalls += 1;
  }

  snapshot(): UsageSnapshot {
    return {
      calls: this.calls,
      failed: this.failed,
      inputTokens: this.inputTokens,
      outputTokens: this.outputTokens,
      cacheReadTokens: this.cacheReadTokens,
      savedCalls: this.savedCalls,
      startedAt: this.startedAt,
    };
  }

  reset(): void {
    this.calls = 0;
    this.failed = 0;
    this.inputTokens = 0;
    this.outputTokens = 0;
    this.cacheReadTokens = 0;
    this.savedCalls = 0;
    this.startedAt = null;
  }
}

/**
 * 这一趟宿主进程的账本。
 *
 * 进程级单例是刻意的：宿主重启 = 新的一次会话，账本自然从头记。
 * 想跨会话累计得落盘，那是另一个决定（也还没人需要）。
 */
export const usageMeter = new UsageMeter();

/** 一句话说清这本账（界面直接用）。没有数据就说没有。 */
export function describeUsage(snapshot: UsageSnapshot): string {
  if (snapshot.calls === 0 && snapshot.savedCalls === 0) return '还没问过 AI';
  const parts: string[] = [`${snapshot.calls} 次调用`];
  const tokens = snapshot.inputTokens + snapshot.outputTokens;
  if (tokens > 0) parts.push(`${(tokens / 1000).toFixed(1)}k tok`);
  if (snapshot.cacheReadTokens > 0) parts.push(`其中缓存命中 ${(snapshot.cacheReadTokens / 1000).toFixed(1)}k`);
  if (snapshot.failed > 0) parts.push(`${snapshot.failed} 次失败`);
  if (snapshot.savedCalls > 0) parts.push(`省下 ${snapshot.savedCalls} 次`);
  return parts.join(' · ');
}
