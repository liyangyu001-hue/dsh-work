/**
 * 交接（handoff）五态的完整迁移表。
 *
 * ROADMAP P2 那条「交接五态迁移有单测，非法迁移被拒绝」的落点：
 * 六个起点（没有交接 + 五个状态）× 五个意图，逐格断言放行还是拒绝，并且
 *   - 放行时结果文档仍能过 `validateDoc`，落到的状态与设计表一致；
 *   - 拒绝时**原文档一个字节都不变**（不是「改了再改回来」），理由是人话。
 *
 * 这张表就是规格：改状态机先改这里，别只改代码。
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  apply,
  createSequentialIdFactory,
  emptyDoc,
  materializePlan,
  validateDoc,
} from './index.ts';
import type { ApplyContext, HandoffState, Intent, WorkDoc } from './index.ts';

const NOW = 1_760_000_000_000;
const ctx: ApplyContext = { at: NOW, by: 'human' };
const NODE = 'nd_0001';

/** 种子：A(nd_0001) ├─ A1(nd_0002) └─ A2(nd_0003)，B(nd_0004)。 */
function seed(): WorkDoc {
  const base = emptyDoc({ id: 'wrk_1', title: '交接测试', now: NOW });
  const { roots, nodes } = materializePlan(
    [
      { title: 'A', children: [{ title: 'A1' }, { title: 'A2' }] },
      { title: 'B' },
    ],
    { now: NOW, newId: createSequentialIdFactory('nd') },
  );
  const result = apply(base, { type: 'plan.set', roots, nodes }, ctx);
  assert.ok(result.ok, result.ok ? '' : result.reason);
  return result.doc;
}

/** 预期成功的那些步，用它推进到某个状态；失败直接让测试炸出来。 */
function must(doc: WorkDoc, intent: Intent, at = NOW): WorkDoc {
  const result = apply(doc, intent, { at, by: 'human' });
  assert.ok(result.ok, result.ok ? '' : result.reason);
  return result.doc;
}

type Stage = 'none' | HandoffState;

const offer = (): Intent => ({ type: 'handoff.offer', nodeId: NODE });
const start = (): Intent => ({ type: 'handoff.start', nodeId: NODE, agentSessionId: 'ses_1' });
const submit = (): Intent => ({ type: 'handoff.submit', nodeId: NODE, note: '写完了' });
const accept = (): Intent => ({ type: 'handoff.accept', nodeId: NODE });
const back = (): Intent => ({ type: 'handoff.return', nodeId: NODE, reason: '方向不对' });

/** 把种子推进到指定起点状态。 */
const toStage: Record<Stage, (doc: WorkDoc) => WorkDoc> = {
  none: (doc) => doc,
  offered: (doc) => must(doc, offer()),
  running: (doc) => must(must(doc, offer()), start()),
  review: (doc) => must(must(must(doc, offer()), start()), submit()),
  accepted: (doc) => must(must(must(must(doc, offer()), start()), submit()), accept()),
  returned: (doc) => must(must(must(doc, offer()), start()), back()),
};

/**
 * 设计表：每个起点状态下，五个意图各自的落点。
 * `null` = 必须被拒绝；字符串 = 必须成功且落到该状态。
 */
const TABLE: Record<Stage, Record<'offer' | 'start' | 'submit' | 'accept' | 'return', HandoffState | null>> = {
  none: { offer: 'offered', start: null, submit: null, accept: null, return: null },
  offered: { offer: 'offered', start: 'running', submit: null, accept: null, return: null },
  running: { offer: null, start: null, submit: 'review', accept: null, return: 'returned' },
  review: { offer: null, start: null, submit: null, accept: 'accepted', return: 'returned' },
  accepted: { offer: null, start: null, submit: null, accept: null, return: null },
  returned: { offer: 'offered', start: null, submit: null, accept: null, return: null },
};

// `return` 是关键字，只能写成「键名: 引用」，不能简写。
const INTENTS = { offer, start, submit, accept, return: back } as const;

test('交接迁移表：6 个起点 × 5 个意图逐格对账', () => {
  for (const stage of Object.keys(TABLE) as Stage[]) {
    for (const name of Object.keys(INTENTS) as (keyof typeof INTENTS)[]) {
      const before = toStage[stage](seed());
      const snapshot = structuredClone(before);
      const expected = TABLE[stage][name];
      const result = apply(before, INTENTS[name](), { at: NOW + 1_000, by: 'human' });
      const label = `${stage} --${name}-->`;

      if (expected === null) {
        assert.equal(result.ok, false, `${label} 应该被拒绝`);
        assert.ok(!result.ok && result.reason.trim().length > 0, `${label} 拒绝时要给人话理由`);
        // 拒绝不能留下半截修改。
        assert.deepEqual(before, snapshot, `${label} 被拒后原文档必须原样`);
        continue;
      }

      assert.ok(result.ok, `${label} 应该放行：${result.ok ? '' : result.reason}`);
      assert.equal(result.doc.nodes[NODE]?.handoff?.state, expected, `${label} 落点状态不对`);
      assert.equal(validateDoc(result.doc), null, `${label} 之后文档不合法`);
    }
  }
});

test('交接后再被拒的动作，节点状态与指针都不动', () => {
  const doc = toStage.review(seed());
  const before = structuredClone(doc);
  const result = apply(doc, accept(), ctx);
  assert.ok(result.ok, result.ok ? '' : result.reason);
  assert.equal(result.doc.nodes[NODE]?.status, 'done');
  assert.equal(result.doc.pointer, 'nd_0002', '验收后指针应该前进到下一颗');
  // 验收之后再打回：状态机不允许，且不能把已完成改回去。
  const after = structuredClone(result.doc);
  const rejected = apply(result.doc, back(), ctx);
  assert.equal(rejected.ok, false);
  assert.deepEqual(result.doc, after, '被拒后不能动已完成节点');
  assert.notDeepEqual(before, result.doc, '验收本身要真的改了文档');
});

test('重复提议只是重置计时，不会叠出第二个交接', () => {
  const doc = must(seed(), offer());
  const again = must(doc, offer(), NOW + 5_000);
  assert.equal(again.nodes[NODE]?.handoff?.state, 'offered');
  assert.equal(again.nodes[NODE]?.handoff?.since, NOW + 5_000, '重复提议应刷新 since');
});

test('已收尾的节点不能再交给 AI', () => {
  // 完成一颗再提议：语义上说不通，必须被挡。
  let doc = must(seed(), { type: 'node.complete', nodeId: 'nd_0004' });
  const rejectedDone = apply(doc, { type: 'handoff.offer', nodeId: 'nd_0004' }, ctx);
  assert.equal(rejectedDone.ok, false, '已完成的节点不该能被提议交接');
  assert.match(rejectedDone.ok ? '' : rejectedDone.reason, /完成|收尾/);

  // 跳过的同理。
  doc = must(seed(), { type: 'node.skip', nodeId: 'nd_0004' });
  const rejectedSkipped = apply(doc, { type: 'handoff.offer', nodeId: 'nd_0004' }, ctx);
  assert.equal(rejectedSkipped.ok, false, '已跳过的节点不该能被提议交接');
  assert.match(rejectedSkipped.ok ? '' : rejectedSkipped.reason, /跳过|收尾/);
});

test('验收之后交接状态停在 accepted，不会被下一次提议悄悄复活', () => {
  const doc = toStage.accepted(seed());
  assert.equal(doc.nodes[NODE]?.handoff?.state, 'accepted');
  const result = apply(doc, offer(), ctx);
  assert.equal(result.ok, false, '已验收（节点已完成）的交接不能再提议');
});
