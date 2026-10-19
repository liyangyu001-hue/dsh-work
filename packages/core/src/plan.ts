/**
 * 把 AI 产出的计划树变成带 id 的真实节点。
 *
 * 为什么不放进 apply：apply 必须纯净（不能取随机数），而 id 生成必然用到随机性。
 * 所以「生成 id」这一步被提到调用方，apply 只负责校验和落库。
 */

import type { MaterializeOptions, NodeId, PlanSpec, WorkNode } from './types.ts';

export interface Materialized {
  roots: NodeId[];
  nodes: WorkNode[];
}

export function materializePlan(specs: readonly PlanSpec[], opts: MaterializeOptions): Materialized {
  const nodes: WorkNode[] = [];

  const build = (spec: PlanSpec, parentId: NodeId | null): NodeId => {
    const id = opts.newId();
    const children: NodeId[] = [];
    const node: WorkNode = {
      id,
      parentId,
      children,
      title: spec.title.trim() || '未命名任务',
      kind: spec.kind ?? 'task',
      status: 'todo',
      evidence: [],
      advice: null,
      handoff: null,
      createdAt: opts.now,
      updatedAt: opts.now,
    };
    if (spec.detail !== undefined) node.detail = spec.detail;
    if (spec.acceptance !== undefined) node.acceptance = spec.acceptance;
    nodes.push(node);

    for (const child of spec.children ?? []) {
      children.push(build(child, id));
    }
    return id;
  };

  const roots = specs.map((spec) => build(spec, opts.parentId ?? null));
  return { roots, nodes };
}
