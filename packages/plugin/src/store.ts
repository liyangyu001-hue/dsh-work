/**
 * 任务状态的持久化与事务边界。
 *
 * 设计要点（对应 docs/ADR/0003-事件日志为真相源.md）：
 *
 * 1. **快照 + 日志双写**：每次提交都追加一行 journal，并原子重写 work.json。
 *    日志是「怎么走到这里」的审计与崩溃恢复依据，快照是「现在是什么样」的快速载入路径。
 * 2. **单写者**：所有提交进同一条 Promise 队列，天然串行，不需要文件锁。
 * 3. **不吞错**：apply 失败不落一行日志；写盘失败要能回溯到具体意图。
 *
 * 目录形状：
 * ```
 * <workspace>/.dsh-work/
 *   work.json        # 快照（人可读、可提交进 git）
 *   journal.ndjson   # 追加日志（每行一条意图）
 *   captures/        # 截图资产（默认可被 .gitignore）
 * ```
 */

import { appendFile, mkdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';

import { apply, emptyDoc, parseJournal, replay, SCHEMA_VERSION as DOC_SCHEMA_VERSION } from '@dshwork/core';
import type {
  Actor,
  ApplyContext,
  ApplyResult,
  Intent,
  JournalEntry,
  WorkDoc,
} from '@dshwork/core';

export const SNAPSHOT_SCHEMA_VERSION = 1;

export interface SnapshotFile {
  schemaVersion: number;
  savedAt: number;
  /** 与 `doc.seq` 冗余存放，便于人工核对日志是否被截断。 */
  seq: number;
  doc: WorkDoc;
}

export interface StoreOptions {
  /** 工作区根目录；`.dsh-work` 会在其下创建。 */
  root: string;
  /** 首次创建时的任务标题。 */
  title?: string;
  /** 每次提交是否立刻落盘快照。默认 true。 */
  snapshotEveryCommit?: boolean;
  now?: () => number;
  newId?: () => string;
}

export interface ChangeEvent {
  doc: WorkDoc;
  entry: JournalEntry;
}

export type ChangeListener = (event: ChangeEvent) => void;

export class WorkStore {
  readonly root: string;
  readonly stateDir: string;
  readonly capturesDir: string;
  private readonly snapshotPath: string;
  private readonly journalPath: string;
  private readonly listeners = new Set<ChangeListener>();
  private readonly snapshotEveryCommit: boolean;
  private readonly now: () => number;
  private doc: WorkDoc;
  private queue: Promise<unknown> = Promise.resolve();
  private lastWriteError: string | null = null;

  private constructor(options: StoreOptions, doc: WorkDoc) {
    this.root = options.root;
    this.stateDir = path.join(options.root, '.dsh-work');
    this.capturesDir = path.join(this.stateDir, 'captures');
    this.snapshotPath = path.join(this.stateDir, 'work.json');
    this.journalPath = path.join(this.stateDir, 'journal.ndjson');
    this.snapshotEveryCommit = options.snapshotEveryCommit ?? true;
    this.now = options.now ?? (() => Date.now());
    this.doc = doc;
  }

  /**
   * 打开（或初始化）状态目录。幂等：目录已存在就接着用。
   *
   * 恢复顺序：快照 → 回放快照之后的日志行。日志尾部有损坏行时容忍，
   * 但只要坏行之后的条目还能解析，就继续回放（宁可多恢复也不能丢状态）。
   */
  static async open(options: StoreOptions): Promise<WorkStore> {
    const stateDir = path.join(options.root, '.dsh-work');
    await mkdir(path.join(stateDir, 'captures'), { recursive: true });

    const now = options.now ?? (() => Date.now());
    const snapshotPath = path.join(stateDir, 'work.json');
    const journalPath = path.join(stateDir, 'journal.ndjson');

    let doc: WorkDoc | null = null;
    const rawSnapshot = await readFileOrNull(snapshotPath);
    if (rawSnapshot !== null) {
      const parsed = safeJson<SnapshotFile>(rawSnapshot);
      if (
        parsed &&
        parsed.schemaVersion === SNAPSHOT_SCHEMA_VERSION &&
        parsed.doc &&
        parsed.doc.schema === DOC_SCHEMA_VERSION
      ) {
        doc = parsed.doc;
      }
    }

    if (doc === null) {
      doc = emptyDoc({
        id: options.newId ? options.newId() : `wrk_${Date.now().toString(36)}`,
        title: options.title ?? '未命名工作区',
        now: now(),
      });
    }

    const rawJournal = await readFileOrNull(journalPath);
    if (rawJournal !== null && rawJournal.trim() !== '') {
      const { entries } = parseJournal(rawJournal);
      if (entries.length > 0) {
        doc = replay(doc, entries).doc;
      }
    }

    const store = new WorkStore({ ...options, now }, doc);
    // 打开时先落一次快照：把「日志里回放出来的状态」变成下次启动的快速路径。
    await store.writeSnapshot();
    return store;
  }

  get snapshot(): WorkDoc {
    return this.doc;
  }

  get seq(): number {
    return this.doc.seq;
  }

  get lastError(): string | null {
    return this.lastWriteError;
  }

  /**
   * 提交一条意图。返回与 `apply` 同形的结果；失败时不落盘、不广播。
   *
   * 所有提交串行化：并发调用会排成一条队，`seq` 单调递增不会打结。
   */
  commit(intent: Intent, by: Actor, options?: { at?: number }): Promise<ApplyResult> {
    return this.enqueue(async () => {
      const at = options?.at ?? this.now();
      const context: ApplyContext = { at, by };
      const result = apply(this.doc, intent, context);
      if (!result.ok) return result;

      const entry: JournalEntry = {
        seq: result.doc.seq,
        at,
        by,
        intent,
      };

      try {
        await appendFile(this.journalPath, `${JSON.stringify(entry)}\n`, 'utf8');
        this.doc = result.doc;
        if (this.snapshotEveryCommit) await this.writeSnapshot();
        this.lastWriteError = null;
      } catch (error) {
        this.lastWriteError = describeError(error);
        throw new Error(`提交已生效但落盘失败：${this.lastWriteError}`, { cause: error });
      }

      this.emit({ doc: this.doc, entry });
      return result;
    });
  }

  /** 批量提交；任一条失败即停止，返回已成功的条数。 */
  async commitMany(
    requests: ReadonlyArray<{ intent: Intent; by: Actor }>,
  ): Promise<{ applied: number; failed: ApplyResult | null }> {
    let applied = 0;
    for (const request of requests) {
      const result = await this.commit(request.intent, request.by);
      if (!result.ok) return { applied, failed: result };
      applied += 1;
    }
    return { applied, failed: null };
  }

  subscribe(listener: ChangeListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** 显式落盘（宿主退出前调用；正常路径不用管）。 */
  async flush(): Promise<void> {
    await this.enqueue(() => this.writeSnapshot());
  }

  /** 清空状态目录里的运行期文件（保留目录本身）。测试与「重置工作区」用。 */
  async reset(): Promise<void> {
    await this.enqueue(async () => {
      await rm(this.snapshotPath, { force: true });
      await rm(this.journalPath, { force: true });
      this.doc = emptyDoc({ id: this.doc.id, title: this.doc.title, now: this.now() });
      await this.writeSnapshot();
    });
  }

  private emit(event: ChangeEvent): void {
    for (const listener of this.listeners) {
      try {
        listener(event);
      } catch {
        // 订阅者自己的异常不能反向污染提交路径。
      }
    }
  }

  private enqueue<T>(task: () => Promise<T>): Promise<T> {
    const next = this.queue.then(task, task);
    this.queue = next.then(
      () => undefined,
      () => undefined,
    );
    return next;
  }

  private async writeSnapshot(): Promise<void> {
    const payload: SnapshotFile = {
      schemaVersion: SNAPSHOT_SCHEMA_VERSION,
      savedAt: this.now(),
      seq: this.doc.seq,
      doc: this.doc,
    };
    await atomicWrite(this.snapshotPath, `${JSON.stringify(payload, null, 2)}\n`);
  }
}

/* ─────────────────────────── 工具函数 ─────────────────────────── */

async function readFileOrNull(file: string): Promise<string | null> {
  try {
    return await readFile(file, 'utf8');
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return null;
    throw error;
  }
}

/** 先写临时文件再改名：Windows 上 rename 覆盖已存在文件是原子替换。 */
export async function atomicWrite(file: string, data: string): Promise<void> {
  const temporary = `${file}.${process.pid}.${Date.now().toString(36)}.tmp`;
  await writeFile(temporary, data, 'utf8');
  try {
    await rename(temporary, file);
  } catch (error) {
    await rm(temporary, { force: true });
    throw error;
  }
}

function safeJson<T>(text: string): T | null {
  try {
    return JSON.parse(text) as T;
  } catch {
    return null;
  }
}

function describeError(error: unknown): string {
  if (error instanceof Error) return error.message;
  return String(error);
}
