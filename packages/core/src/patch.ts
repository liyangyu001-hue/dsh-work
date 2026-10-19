/**
 * 增量同步：把「上一份快照」与「当前快照」的差异压成最小操作序列。
 *
 * 为什么不用全量广播：任务树会长到几十上百个节点，每次完成一个节点就全量下发，
 * 既浪费带宽，也让客户端的 diff 无从谈起。这里的实现刻意简单——
 * 节点级增删改，不做字段级 diff，因为一个节点的体积很小，而字段级 diff 的复杂度
 * 与出错概率远大于它省下的那点字节。
 */

import type { NodeId, WorkDoc, WorkNode } from './types.ts';
import { validateDoc } from './state.ts';

export type PatchOp =
  | {
      op: 'meta';
      fields: Partial<Pick<WorkDoc, 'title' | 'pointer' | 'roots' | 'updatedAt' | 'seq'>>;
    }
  | { op: 'node.put'; node: WorkNode }
  | { op: 'node.del'; id: NodeId };

const META_KEYS = ['title', 'pointer', 'roots', 'updatedAt', 'seq'] as const;

/**
 * `prev` 为 null 时表示接收方没有任何状态，此时产出「全量」patch。
 */
export function diffDoc(prev: WorkDoc | null, next: WorkDoc): PatchOp[] {
  const ops: PatchOp[] = [];

  const fields: Record<string, unknown> = {};
  for (const key of META_KEYS) {
    const before = prev ? prev[key] : undefined;
    const after = next[key];
    if (before === undefined || JSON.stringify(before) !== JSON.stringify(after)) {
      fields[key] = after;
    }
  }
  if (Object.keys(fields).length > 0) {
    ops.push({ op: 'meta', fields: fields as Extract<PatchOp, { op: 'meta' }>['fields'] });
  }

  for (const id of Object.keys(next.nodes)) {
    const after = next.nodes[id]!;
    const before = prev?.nodes[id];
    if (!before || JSON.stringify(before) !== JSON.stringify(after)) {
      ops.push({ op: 'node.put', node: after });
    }
  }

  if (prev) {
    for (const id of Object.keys(prev.nodes)) {
      if (!next.nodes[id]) ops.push({ op: 'node.del', id });
    }
  }

  return ops;
}

export type PatchResult =
  | { ok: true; doc: WorkDoc }
  | { ok: false; reason: string };

export function applyPatch(input: WorkDoc, ops: readonly PatchOp[]): PatchResult {
  const draft: WorkDoc = {
    ...input,
    nodes: { ...input.nodes },
    roots: [...input.roots],
  };

  for (const op of ops) {
    switch (op.op) {
      case 'meta': {
        const { roots, ...scalar } = op.fields;
        Object.assign(draft, scalar);
        if (roots) draft.roots = [...roots];
        break;
      }
      case 'node.put': {
        draft.nodes[op.node.id] = op.node;
        break;
      }
      case 'node.del': {
        delete draft.nodes[op.id];
        break;
      }
      default: {
        return { ok: false, reason: `未知 patch 操作 ${(op as { op?: string }).op ?? '?'}` };
      }
    }
  }

  const invalid = validateDoc(draft);
  if (invalid) return { ok: false, reason: `patch 应用后状态非法：${invalid}` };
  return { ok: true, doc: draft };
}
