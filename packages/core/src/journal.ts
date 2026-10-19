/**
 * 事件日志：只追加、可回放。
 *
 * 文件格式是 NDJSON —— 每行一条 `JournalEntry`。
 * 选它而不是二进制/数据库，是因为**人要能看懂、git 要能 diff**，
 * 而它的性能瓶颈（几十万行）在「一天几十条意图」的量级下永远不会出现。
 */

import type { Actor, ApplyContext, Intent, WorkDoc } from './types.ts';
import { apply } from './state.ts';

export interface JournalEntry {
  /** 单调递增，与 doc.seq 对齐。 */
  seq: number;
  /** 发生时刻（毫秒）。 */
  at: number;
  by: Actor;
  intent: Intent;
}

export function encodeEntry(entry: JournalEntry): string {
  return `${JSON.stringify(entry)}\n`;
}

export interface ParseResult {
  entries: JournalEntry[];
  /** 解析失败的行数。损坏只可能出现在文件尾部（写一半就崩），其他位置损坏说明有更严重的问题。 */
  corruptLines: number;
}

export function parseJournal(text: string): ParseResult {
  const entries: JournalEntry[] = [];
  let corruptLines = 0;

  for (const rawLine of text.split('\n')) {
    const line = rawLine.trim();
    if (line === '') continue;
    try {
      const parsed = JSON.parse(line) as unknown;
      if (!isJournalEntry(parsed)) {
        corruptLines += 1;
        continue;
      }
      entries.push(parsed);
    } catch {
      corruptLines += 1;
    }
  }

  return { entries, corruptLines };
}

export function isJournalEntry(value: unknown): value is JournalEntry {
  if (typeof value !== 'object' || value === null) return false;
  const candidate = value as Record<string, unknown>;
  if (typeof candidate['seq'] !== 'number' || !Number.isFinite(candidate['seq'])) return false;
  if (typeof candidate['at'] !== 'number' || !Number.isFinite(candidate['at'])) return false;
  if (typeof candidate['by'] !== 'string') return false;
  const intent = candidate['intent'];
  return typeof intent === 'object' && intent !== null && typeof (intent as Intent).type === 'string';
}

export interface ReplayResult {
  doc: WorkDoc;
  /** 被拒绝的意图，附带原因。出现大量拒绝通常意味着日志版本与代码版本不匹配。 */
  rejected: Array<{ seq: number; reason: string }>;
  applied: number;
}

/**
 * 从快照出发重放日志。只处理 `seq > doc.seq` 的条目，因此重复调用是安全的。
 */
export function replay(doc: WorkDoc, entries: readonly JournalEntry[]): ReplayResult {
  let current = doc;
  let applied = 0;
  const rejected: Array<{ seq: number; reason: string }> = [];

  for (const entry of entries) {
    if (entry.seq <= current.seq) continue;
    const ctx: ApplyContext = { at: entry.at, by: entry.by };
    const result = apply(current, entry.intent, ctx);
    if (!result.ok) {
      rejected.push({ seq: entry.seq, reason: result.reason });
      continue;
    }
    current = { ...result.doc, seq: entry.seq };
    applied += 1;
  }

  return { doc: current, rejected, applied };
}
