/**
 * 状态机：`apply(doc, intent, ctx)` 是唯一合法的状态变更入口。
 *
 * 三条硬规矩：
 * 1. **纯函数**。不读时钟、不读文件、不取随机数；`at` / id 全部由参数传入。
 * 2. **不产出非法状态**。每次应用完都跑 `validateDoc`，不合法就整体拒绝、原样返回。
 * 3. **不改入参**。始终返回新对象（结构共享，未变更的节点保持同一引用）。
 *
 * 因为日志要能回放，所以同一串 intent 必须得到同一个 doc——这条由测试守着。
 */

import type {
  Actor,
  ApplyContext,
  Intent,
  NodeId,
  NodeStatus,
  WorkDoc,
  WorkNode,
} from './types.ts';
import { SCHEMA_VERSION } from './types.ts';

export type ApplyResult =
  | { ok: true; doc: WorkDoc }
  | { ok: false; reason: string };

/** 空文档。`id` 与时间由调用方给，保证可预测。 */
export function emptyDoc(input: { id: string; title: string; now: number }): WorkDoc {
  return {
    schema: SCHEMA_VERSION,
    id: input.id,
    title: input.title,
    createdAt: input.now,
    updatedAt: input.now,
    seq: 0,
    pointer: null,
    roots: [],
    nodes: {},
  };
}

/* ─────────────────────────── 遍历 ─────────────────────────── */

/**
 * 深度优先先序展开。卡片左右滑动、指针前进、校验都基于这个顺序。
 * 访问过的节点不会重复进入，因此环不会造成死循环（环由 validateDoc 兜底判定）。
 */
export function linearOrder(doc: WorkDoc): NodeId[] {
  const out: NodeId[] = [];
  const seen = new Set<NodeId>();
  const walk = (ids: readonly NodeId[]): void => {
    for (const id of ids) {
      if (seen.has(id)) continue;
      seen.add(id);
      out.push(id);
      const node = doc.nodes[id];
      if (node) walk(node.children);
    }
  };
  walk(doc.roots);
  return out;
}

/** 尚未完成（todo / active）的节点。 */
export function isOpen(status: NodeStatus): boolean {
  return status === 'todo' || status === 'active';
}

/** 进度统计，供界面显示。 */
export function progress(doc: WorkDoc): { done: number; total: number; ratio: number } {
  const ids = linearOrder(doc);
  let done = 0;
  for (const id of ids) {
    const node = doc.nodes[id];
    if (node && (node.status === 'done' || node.status === 'skipped')) done += 1;
  }
  const total = ids.length;
  return { done, total, ratio: total === 0 ? 0 : done / total };
}

/* ─────────────────────────── 校验 ─────────────────────────── */

/**
 * 返回 null 表示合法，否则返回人类可读的原因。
 * 加载磁盘数据、重放日志、应用意图三处共用同一个校验。
 */
export function validateDoc(doc: WorkDoc): string | null {
  if (doc.schema !== SCHEMA_VERSION) {
    return `schema 不匹配：期望 ${SCHEMA_VERSION}，实际 ${doc.schema}`;
  }

  const order = linearOrder(doc);
  const seen = new Set(order);

  for (const id of Object.keys(doc.nodes)) {
    if (!seen.has(id)) return `节点 ${id} 不可从根到达（可能是环或孤儿）`;
  }

  let activeCount = 0;
  for (const id of order) {
    const node = doc.nodes[id];
    if (!node) return `顺序表引用了不存在的节点 ${id}`;
    if (node.status === 'active') activeCount += 1;
  }
  if (activeCount > 1) return `同时存在 ${activeCount} 个 active 节点`;

  // 父子一致性 + 根节点约束
  const rootSet = new Set(doc.roots);
  for (const id of order) {
    const node = doc.nodes[id];
    if (!node) continue;
    const isRoot = rootSet.has(id);
    if (isRoot && node.parentId !== null) return `根节点 ${id} 的 parentId 应为 null`;
    if (!isRoot) {
      if (node.parentId === null) return `节点 ${id} 不是根，但 parentId 为 null`;
      const parent = doc.nodes[node.parentId];
      if (!parent) return `节点 ${id} 的父节点 ${node.parentId} 不存在`;
      if (!parent.children.includes(id)) return `节点 ${id} 未列在其父节点的 children 中`;
    }
    for (const childId of node.children) {
      if (!doc.nodes[childId]) return `节点 ${id} 的子节点 ${childId} 不存在`;
    }
  }

  if (doc.pointer !== null && !doc.nodes[doc.pointer]) {
    return `pointer 指向不存在的节点 ${doc.pointer}`;
  }

  return null;
}

/* ─────────────────────────── 应用 ─────────────────────────── */

type Draft = WorkDoc;

function withNode(draft: Draft, node: WorkNode): void {
  draft.nodes[node.id] = node;
}

function demoteOtherActive(draft: Draft, exceptId: NodeId | null): void {
  for (const id of Object.keys(draft.nodes)) {
    if (id === exceptId) continue;
    const node = draft.nodes[id];
    if (node && node.status === 'active') {
      draft.nodes[id] = { ...node, status: 'todo', updatedAt: draft.updatedAt };
    }
  }
}

/** 从 `from` 之后找下一个 todo；找不到则回头从开头找。 */
function nextOpen(doc: WorkDoc, from: NodeId): NodeId | null {
  const order = linearOrder(doc);
  const index = order.indexOf(from);
  const pick = (ids: readonly NodeId[]): NodeId | null => {
    for (const id of ids) {
      if (doc.nodes[id]?.status === 'todo') return id;
    }
    return null;
  };
  if (index < 0) return pick(order);
  return pick(order.slice(index + 1)) ?? pick(order.slice(0, index));
}

/** 当前节点收尾后移动指针。 */
function advancePointer(draft: Draft, closedId: NodeId): void {
  draft.pointer = nextOpen(draft, closedId);
}

function closeNode(draft: Draft, nodeId: NodeId, status: NodeStatus, at: number): WorkNode {
  const node = draft.nodes[nodeId]!;
  const next: WorkNode = { ...node, status, updatedAt: at };
  withNode(draft, next);
  return next;
}

function fail(reason: string): ApplyResult {
  return { ok: false, reason };
}

/**
 * 应用一条意图。
 *
 * 注意：任何分支里都不允许出现 throw——业务性失败一律走 `{ok:false}`，
 * 这样调用方（Host）可以统一地回 `intent.ack {applied:false}` 而不炸进程。
 */
export function apply(input: WorkDoc, intent: Intent, ctx: ApplyContext): ApplyResult {
  const draft: Draft = {
    ...input,
    nodes: { ...input.nodes },
    roots: [...input.roots],
    updatedAt: ctx.at,
  };

  const node = (id: NodeId): WorkNode | undefined => draft.nodes[id];
  const requireNode = (id: NodeId): WorkNode | null => node(id) ?? null;

  switch (intent.type) {
    /* ── 计划 ───────────────────────────────────────────── */

    case 'plan.set': {
      const nodes: Record<NodeId, WorkNode> = {};
      for (const n of intent.nodes) nodes[n.id] = n;
      draft.nodes = nodes;
      draft.roots = [...intent.roots];
      draft.pointer = null;

      const order = linearOrder(draft);
      if (order.length === 0) break;
      const firstTodo = order.find((id) => draft.nodes[id]?.status === 'todo');
      if (firstTodo) {
        demoteOtherActive(draft, firstTodo);
        const fresh = draft.nodes[firstTodo]!;
        draft.nodes[firstTodo] = { ...fresh, status: 'active', updatedAt: ctx.at };
        draft.pointer = firstTodo;
      }
      break;
    }

    case 'plan.add': {
      if (draft.nodes[intent.node.id]) return fail(`节点 ${intent.node.id} 已存在`);
      const parentId = intent.parentId;
      if (parentId !== null) {
        const parent = requireNode(parentId);
        if (!parent) return fail(`父节点 ${parentId} 不存在`);
      }
      withNode(draft, { ...intent.node, parentId });
      if (parentId === null) {
        const index = clampIndex(intent.index, draft.roots.length);
        draft.roots.splice(index, 0, intent.node.id);
      } else {
        const parent = draft.nodes[parentId]!;
        const index = clampIndex(intent.index, parent.children.length);
        const children = [...parent.children];
        children.splice(index, 0, intent.node.id);
        withNode(draft, { ...parent, children, updatedAt: ctx.at });
      }
      if (draft.pointer === null && intent.node.status !== 'done') {
        draft.pointer = intent.node.id;
        const fresh = draft.nodes[intent.node.id]!;
        demoteOtherActive(draft, intent.node.id);
        draft.nodes[intent.node.id] = { ...fresh, status: 'active', updatedAt: ctx.at };
      }
      break;
    }

    case 'plan.remove': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      const doomed = collectSubtree(draft, intent.nodeId);
      for (const id of doomed) delete draft.nodes[id];

      if (target.parentId === null) {
        draft.roots = draft.roots.filter((id) => id !== intent.nodeId);
      } else {
        const parent = draft.nodes[target.parentId]!;
        withNode(draft, {
          ...parent,
          children: parent.children.filter((id) => id !== intent.nodeId),
          updatedAt: ctx.at,
        });
      }

      if (draft.pointer !== null && doomed.has(draft.pointer)) {
        draft.pointer = target.parentId;
        if (draft.pointer !== null && draft.nodes[draft.pointer]?.status === 'todo') {
          const p = draft.nodes[draft.pointer]!;
          draft.nodes[draft.pointer] = { ...p, status: 'active', updatedAt: ctx.at };
        }
      }
      break;
    }

    case 'plan.rename': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      withNode(draft, {
        ...target,
        title: intent.title ?? target.title,
        detail: intent.detail ?? target.detail,
        acceptance: intent.acceptance ?? target.acceptance,
        updatedAt: ctx.at,
      });
      break;
    }

    /* ── 推进 ───────────────────────────────────────────── */

    case 'node.advance': {
      const current = draft.pointer;
      if (!current) return fail('当前没有进行中的节点');
      const target = requireNode(current);
      if (!target) return fail(`pointer 指向不存在的节点 ${current}`);
      if (target.status === 'done' || target.status === 'skipped') {
        return fail(`节点 ${current} 已收尾，不能再次完成`);
      }
      closeNode(draft, current, 'done', ctx.at);
      advancePointer(draft, current);
      const next = draft.pointer;
      if (next) {
        demoteOtherActive(draft, next);
        const fresh = draft.nodes[next]!;
        draft.nodes[next] = { ...fresh, status: 'active', updatedAt: ctx.at };
      }
      break;
    }

    case 'node.back': {
      const current = draft.pointer;
      if (!current) return fail('当前没有进行中的节点');
      const order = linearOrder(draft);
      let index = order.indexOf(current) - 1;
      while (index >= 0) {
        const candidateId = order[index]!;
        const candidate = draft.nodes[candidateId]!;
        if (candidate.status !== 'skipped') break;
        index -= 1;
      }
      if (index < 0) return fail('已经是第一个节点');
      const prevId = order[index]!;

      // 当前节点退回未完成，指针与前一个节点都归位。保持「active 至多一个」。
      demoteOtherActive(draft, prevId);
      const currentNode = draft.nodes[current]!;
      if (currentNode.status === 'active') {
        draft.nodes[current] = { ...currentNode, status: 'todo', updatedAt: ctx.at };
      }
      const prev = draft.nodes[prevId]!;
      draft.nodes[prevId] = { ...prev, status: 'active', updatedAt: ctx.at };
      draft.pointer = prevId;
      break;
    }

    case 'node.complete': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      closeNode(draft, intent.nodeId, 'done', ctx.at);
      if (draft.pointer === intent.nodeId) {
        advancePointer(draft, intent.nodeId);
        const next = draft.pointer;
        if (next) {
          demoteOtherActive(draft, next);
          const fresh = draft.nodes[next]!;
          draft.nodes[next] = { ...fresh, status: 'active', updatedAt: ctx.at };
        }
      }
      break;
    }

    case 'node.skip': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      closeNode(draft, intent.nodeId, 'skipped', ctx.at);
      if (draft.pointer === intent.nodeId) {
        advancePointer(draft, intent.nodeId);
        const next = draft.pointer;
        if (next) {
          demoteOtherActive(draft, next);
          const fresh = draft.nodes[next]!;
          draft.nodes[next] = { ...fresh, status: 'active', updatedAt: ctx.at };
        }
      }
      break;
    }

    case 'node.focus': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      if (target.status === 'done' || target.status === 'skipped') {
        return fail(`节点 ${intent.nodeId} 已收尾，不能成为当前节点`);
      }
      demoteOtherActive(draft, intent.nodeId);
      draft.nodes[intent.nodeId] = { ...target, status: 'active', updatedAt: ctx.at };
      draft.pointer = intent.nodeId;
      break;
    }

    case 'node.advise': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      withNode(draft, { ...target, advice: intent.advice, updatedAt: ctx.at });
      break;
    }

    case 'node.evidence': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      withNode(draft, {
        ...target,
        evidence: [...target.evidence, intent.evidence],
        updatedAt: ctx.at,
      });
      break;
    }

    /* ── 交接 ───────────────────────────────────────────── */

    case 'handoff.offer': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      const state = target.handoff?.state;
      if (state === 'running' || state === 'review') {
        return fail(`节点 ${intent.nodeId} 正在被 AI 执行，不能重复提议`);
      }
      withNode(draft, {
        ...target,
        handoff: { state: 'offered', since: ctx.at },
        updatedAt: ctx.at,
      });
      break;
    }

    case 'handoff.start': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      if (target.handoff?.state !== 'offered') return fail('必须先提议（offered）才能开始');
      withNode(draft, {
        ...target,
        handoff: { state: 'running', agentSessionId: intent.agentSessionId, since: ctx.at },
        updatedAt: ctx.at,
      });
      break;
    }

    case 'handoff.submit': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      if (target.handoff?.state !== 'running') return fail('只有运行中（running）才能交回');
      withNode(draft, {
        ...target,
        handoff: { ...target.handoff, state: 'review', note: intent.note, since: ctx.at },
        updatedAt: ctx.at,
      });
      break;
    }

    case 'handoff.accept': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      if (target.handoff?.state !== 'review') return fail('只有待确认（review）才能验收');
      withNode(draft, {
        ...target,
        handoff: { ...target.handoff, state: 'accepted', since: ctx.at },
        updatedAt: ctx.at,
      });
      closeNode(draft, intent.nodeId, 'done', ctx.at);
      if (draft.pointer === intent.nodeId) {
        advancePointer(draft, intent.nodeId);
        const next = draft.pointer;
        if (next) {
          demoteOtherActive(draft, next);
          const fresh = draft.nodes[next]!;
          draft.nodes[next] = { ...fresh, status: 'active', updatedAt: ctx.at };
        }
      }
      break;
    }

    case 'handoff.return': {
      const target = requireNode(intent.nodeId);
      if (!target) return fail(`节点 ${intent.nodeId} 不存在`);
      const state = target.handoff?.state;
      if (state !== 'review' && state !== 'running') return fail('只有运行中或待确认才能打回');
      const backToActive = draft.pointer === intent.nodeId;
      withNode(draft, {
        ...target,
        status: backToActive ? 'active' : 'todo',
        handoff: { ...target.handoff, state: 'returned', note: intent.reason, since: ctx.at },
        updatedAt: ctx.at,
      });
      break;
    }

    default: {
      // 未知意图：宁可拒绝，也不要静默吞掉。
      return fail(`未知意图 ${(intent as { type?: string }).type ?? '?'}`);
    }
  }

  const invalid = validateDoc(draft);
  if (invalid) return fail(`状态校验失败：${invalid}`);

  return { ok: true, doc: draft };
}

/** 批量应用（重放日志用），遇到拒绝就跳过并记录。 */
export function applyMany(
  doc: WorkDoc,
  entries: ReadonlyArray<{ intent: Intent; ctx: ApplyContext; seq?: number }>,
): { doc: WorkDoc; rejected: Array<{ index: number; reason: string }> } {
  let current = doc;
  const rejected: Array<{ index: number; reason: string }> = [];
  entries.forEach((entry, index) => {
    const result = apply(current, entry.intent, entry.ctx);
    if (result.ok) {
      current = entry.seq === undefined ? result.doc : { ...result.doc, seq: entry.seq };
    } else {
      rejected.push({ index, reason: result.reason });
    }
  });
  return { doc: current, rejected };
}

/* ─────────────────────────── 小工具 ───────────────────────── */

function clampIndex(index: number | undefined, length: number): number {
  if (index === undefined || Number.isNaN(index)) return length;
  return Math.max(0, Math.min(index, length));
}

function collectSubtree(doc: WorkDoc, rootId: NodeId): Set<NodeId> {
  const out = new Set<NodeId>();
  const walk = (id: NodeId): void => {
    if (out.has(id)) return;
    out.add(id);
    const node = doc.nodes[id];
    if (node) node.children.forEach(walk);
  };
  walk(rootId);
  return out;
}

export type { Actor };
