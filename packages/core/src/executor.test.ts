import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  apply,
  createSequentialIdFactory,
  emptyDoc,
  materializePlan,
  validateDoc,
} from './index.ts';
import type { ApplyContext, Executor, Intent, WorkDoc } from './index.ts';

/**
 * 「这一步该谁做」——用户 m04063 第 3 条问的「哪部分 AI 做、哪部分人做」。
 *
 * 原先只能靠 `handoff` 在不在来推测，那是过程痕迹：一颗没派的节点和一颗
 * 明确要人做的节点长得一模一样。这个文件把显式字段的规则钉死。
 */

const NOW = 1_760_000_000_000;
const ctx: ApplyContext = { at: NOW, by: 'human' };

function seed(): WorkDoc {
  const base = emptyDoc({ id: 'wrk_1', title: '归属测试', now: NOW });
  const { roots, nodes } = materializePlan(
    [{ title: 'A', children: [{ title: 'A1' }, { title: 'A2' }] }, { title: 'B' }],
    { now: NOW, newId: createSequentialIdFactory('nd') },
  );
  const result = apply(base, { type: 'plan.set', roots, nodes }, ctx);
  assert.ok(result.ok, result.ok ? '' : result.reason);
  return result.doc;
}

function assign(doc: WorkDoc, nodeId: string, executor: Executor | null): WorkDoc {
  const result = apply(doc, { type: 'node.assign', nodeId, executor } as Intent, ctx);
  assert.ok(result.ok, result.ok ? '' : result.reason);
  return result.doc;
}

test('标执行者：human / ai / either 都能落，落完文档仍然合法', () => {
  let doc = seed();
  for (const executor of ['human', 'ai', 'either'] as const) {
    doc = assign(doc, 'nd_0002', executor);
    assert.equal(doc.nodes.nd_0002?.executor, executor);
    assert.equal(validateDoc(doc), null);
  }
});

test('default 是「未定」：没标过的节点读出来是 undefined，不是 human', () => {
  const doc = seed();
  assert.equal(doc.nodes.nd_0001?.executor, undefined);
});

test('传 null 等于取消标记，能回到「都行 / 还没定」', () => {
  let doc = assign(seed(), 'nd_0003', 'ai');
  doc = assign(doc, 'nd_0003', null);
  assert.equal(doc.nodes.nd_0003?.executor, null);
  assert.equal(validateDoc(doc), null);
});

test('拼错的执行者被拒，且原文档一个字节没变', () => {
  const doc = seed();
  const before = structuredClone(doc);
  // 从 HTTP / 工具参数进来的可能是任意字符串，必须在落盘前挡住
  const result = apply(doc, { type: 'node.assign', nodeId: 'nd_0002', executor: 'robot' } as unknown as Intent, ctx);
  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /不认识的执行者/);
  assert.deepEqual(structuredClone(doc), before);
});

test('节点不存在时拒绝，理由点名是哪一颗', () => {
  const doc = seed();
  const result = apply(doc, { type: 'node.assign', nodeId: 'nd_9999', executor: 'ai' }, ctx);
  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /nd_9999/);
});

test('load 时也会校验：磁盘里塞了脏值，validateDoc 要指出来', () => {
  const doc = seed();
  const broken: WorkDoc = structuredClone(doc);
  broken.nodes.nd_0001 = { ...broken.nodes.nd_0001!, executor: 'robot' as unknown as Executor };
  assert.match(validateDoc(broken) ?? '', /执行者 robot 不认识/);
});

test('拆解时就能定的归属跟着节点一起落下（PlanSpec.executor）', () => {
  const { nodes } = materializePlan(
    [
      { title: '写方案', executor: 'human' },
      { title: '跑回归', executor: 'ai' },
      { title: '还没想清楚' },
    ],
    { now: NOW, newId: createSequentialIdFactory('nd') },
  );
  assert.equal(nodes[0]?.executor, 'human');
  assert.equal(nodes[1]?.executor, 'ai');
  assert.equal(nodes[2]?.executor, undefined);
});

test('已收尾的节点也能补标执行者（记录实际是谁做的），但不会改状态', () => {
  let doc = seed();
  const done = apply(doc, { type: 'node.complete', nodeId: 'nd_0001' }, ctx);
  assert.ok(done.ok, done.ok ? '' : done.reason);
  doc = done.doc;
  assert.equal(doc.nodes.nd_0001?.status, 'done');

  doc = assign(doc, 'nd_0001', 'human');
  assert.equal(doc.nodes.nd_0001?.executor, 'human');
  assert.equal(doc.nodes.nd_0001?.status, 'done');
  assert.equal(validateDoc(doc), null);
});

test('标执行者不会顺手把节点改成「交给 AI」：意图各管一件事', () => {
  const doc = assign(seed(), 'nd_0002', 'ai');
  assert.equal(doc.nodes.nd_0002?.handoff, null);
  assert.notEqual(doc.nodes.nd_0002?.status, 'active');
});
