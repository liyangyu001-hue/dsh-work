import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  apply,
  applyPatch,
  buildEnvelope,
  createSequentialIdFactory,
  decodeEnvelope,
  diffDoc,
  emptyDoc,
  linearOrder,
  materializePlan,
  parseJournal,
  replay,
  validateDoc,
} from './index.ts';
import type { ApplyContext, Intent, JournalEntry, WorkDoc } from './index.ts';

const NOW = 1_760_000_000_000;
const ctx: ApplyContext = { at: NOW, by: 'human' };

/**
 * 种子任务树：
 *   A (nd_0001)
 *   ├─ A1 (nd_0002)
 *   └─ A2 (nd_0003)
 *   B (nd_0004)
 */
function seed(): WorkDoc {
  const base = emptyDoc({ id: 'wrk_1', title: '测试任务', now: NOW });
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

function run(doc: WorkDoc, intent: Intent, at = NOW): WorkDoc {
  const result = apply(doc, intent, { at, by: 'human' });
  assert.ok(result.ok, result.ok ? '' : result.reason);
  return result.doc;
}

function expectReject(doc: WorkDoc, intent: Intent, fragment?: string): void {
  const result = apply(doc, intent, ctx);
  assert.equal(result.ok, false, `预期被拒绝，但成功了：${intent.type}`);
  if (!result.ok && fragment) {
    assert.match(result.reason, new RegExp(fragment));
  }
}

test('空文档合法，且深度优先顺序正确', () => {
  const doc = seed();
  assert.equal(validateDoc(doc), null);
  assert.deepEqual(linearOrder(doc), ['nd_0001', 'nd_0002', 'nd_0003', 'nd_0004']);
});

test('plan.set 自动把第一个 todo 设为 active 并移动指针', () => {
  const doc = seed();
  assert.equal(doc.pointer, 'nd_0001');
  assert.equal(doc.nodes['nd_0001']!.status, 'active');
  assert.equal(doc.nodes['nd_0002']!.status, 'todo');
});

test('advance 按深度优先前进，最后一个节点完成后指针归零', () => {
  let doc = seed();
  doc = run(doc, { type: 'node.advance' });
  assert.equal(doc.nodes['nd_0001']!.status, 'done');
  assert.equal(doc.pointer, 'nd_0002');

  doc = run(doc, { type: 'node.advance' });
  assert.equal(doc.pointer, 'nd_0003');

  doc = run(doc, { type: 'node.advance' });
  assert.equal(doc.pointer, 'nd_0004');

  doc = run(doc, { type: 'node.advance' });
  assert.equal(doc.pointer, null);
  assert.equal(validateDoc(doc), null);
});

test('back 回到上一个节点：前一个节点重新 active，当前退成 todo', () => {
  let doc = seed();
  doc = run(doc, { type: 'node.advance' }); // A done，指针到 A1
  doc = run(doc, { type: 'node.advance' }); // A1 done，指针到 A2
  assert.equal(doc.pointer, 'nd_0003');

  doc = run(doc, { type: 'node.back' });
  assert.equal(doc.pointer, 'nd_0002');
  assert.equal(doc.nodes['nd_0002']!.status, 'active');
  assert.equal(doc.nodes['nd_0003']!.status, 'todo');
  assert.equal(validateDoc(doc), null);
});

test('back 在第一个节点时被拒绝', () => {
  expectReject(seed(), { type: 'node.back' }, '第一个节点');
});

test('全树至多一个 active', () => {
  let doc = seed();
  doc = run(doc, { type: 'node.advance' });
  const actives = Object.values(doc.nodes).filter((node) => node.status === 'active');
  assert.equal(actives.length, 1);
});

test('focus 可以把指针挪到后面的 todo', () => {
  let doc = seed();
  doc = run(doc, { type: 'node.focus', nodeId: 'nd_0004' });
  assert.equal(doc.pointer, 'nd_0004');
  assert.equal(doc.nodes['nd_0001']!.status, 'todo');
  assert.equal(validateDoc(doc), null);
});

test('complete 非当前节点不影响指针', () => {
  let doc = seed();
  doc = run(doc, { type: 'node.complete', nodeId: 'nd_0004' });
  assert.equal(doc.nodes['nd_0004']!.status, 'done');
  assert.equal(doc.pointer, 'nd_0001');
});

test('skip 当前节点会前进', () => {
  let doc = seed();
  doc = run(doc, { type: 'node.skip', nodeId: 'nd_0001' });
  assert.equal(doc.nodes['nd_0001']!.status, 'skipped');
  assert.equal(doc.pointer, 'nd_0002');
});

test('交接五态：offer → start → submit → accept 会完成节点', () => {
  let doc = seed();
  doc = run(doc, { type: 'handoff.offer', nodeId: 'nd_0001' });
  assert.equal(doc.nodes['nd_0001']!.handoff?.state, 'offered');

  doc = run(doc, { type: 'handoff.start', nodeId: 'nd_0001', agentSessionId: 'ses_1' });
  assert.equal(doc.nodes['nd_0001']!.handoff?.state, 'running');

  doc = run(doc, { type: 'handoff.submit', nodeId: 'nd_0001', note: '写完了' });
  assert.equal(doc.nodes['nd_0001']!.handoff?.state, 'review');
  // 待确认期间节点仍未完成——完成判定权在人手里
  assert.equal(doc.nodes['nd_0001']!.status, 'active');

  doc = run(doc, { type: 'handoff.accept', nodeId: 'nd_0001' });
  assert.equal(doc.nodes['nd_0001']!.status, 'done');
  assert.equal(doc.pointer, 'nd_0002');
});

test('交接非法迁移被拒绝', () => {
  const doc = seed();
  expectReject(doc, { type: 'handoff.submit', nodeId: 'nd_0001' }, 'running');
  expectReject(doc, { type: 'handoff.accept', nodeId: 'nd_0001' }, 'review');
  expectReject(
    doc,
    { type: 'handoff.start', nodeId: 'nd_0001', agentSessionId: 'ses_x' },
    'offered',
  );
});

test('handoff.return 把节点交还给人', () => {
  let doc = seed();
  doc = run(doc, { type: 'handoff.offer', nodeId: 'nd_0001' });
  doc = run(doc, { type: 'handoff.start', nodeId: 'nd_0001', agentSessionId: 'ses_1' });
  doc = run(doc, { type: 'handoff.submit', nodeId: 'nd_0001' });
  doc = run(doc, { type: 'handoff.return', nodeId: 'nd_0001', reason: '方向不对' });
  assert.equal(doc.nodes['nd_0001']!.handoff?.state, 'returned');
  assert.equal(doc.nodes['nd_0001']!.status, 'active');
});

test('未知意图被拒绝而不是静默吞掉', () => {
  const doc = seed();
  const weird = { type: 'node.explode' } as unknown as Intent;
  expectReject(doc, weird, '未知意图');
});

test('apply 不修改入参', () => {
  const doc = seed();
  const before = JSON.stringify(doc);
  run(doc, { type: 'node.advance' });
  assert.equal(JSON.stringify(doc), before);
});

test('删除子树后状态仍合法', () => {
  let doc = seed();
  doc = run(doc, { type: 'plan.remove', nodeId: 'nd_0002' });
  assert.equal(doc.nodes['nd_0002'], undefined);
  assert.deepEqual(linearOrder(doc), ['nd_0001', 'nd_0003', 'nd_0004']);
  assert.equal(validateDoc(doc), null);
});

test('validateDoc 能发现环', () => {
  const doc = seed();
  const broken: WorkDoc = {
    ...doc,
    nodes: {
      ...doc.nodes,
      nd_0001: { ...doc.nodes['nd_0001']!, parentId: 'nd_0003' },
    },
  };
  assert.notEqual(validateDoc(broken), null);
});

test('日志重放的结果与逐条 apply 完全一致', () => {
  const intents: Intent[] = [
    { type: 'node.advance' },
    { type: 'node.advance' },
    { type: 'node.back' },
    { type: 'node.skip', nodeId: 'nd_0003' },
    { type: 'node.advance' },
  ];

  let sequential = seed();
  const entries: JournalEntry[] = [];
  intents.forEach((intent, index) => {
    sequential = run(sequential, intent, NOW + index);
    // 日志序号取「apply 之后」的 seq：这样重放时它与快照天然对齐。
    entries.push({ seq: sequential.seq, at: NOW + index, by: 'human', intent });
  });

  const fromScratch = replay(seed(), entries);
  assert.equal(fromScratch.rejected.length, 0);
  assert.equal(fromScratch.applied, intents.length);
  assert.deepEqual(fromScratch.doc.nodes, sequential.nodes);
  assert.equal(fromScratch.doc.pointer, sequential.pointer);
  assert.equal(fromScratch.doc.seq, sequential.seq);
});

test('replay 会跳过 seq 已覆盖的条目（幂等）', () => {
  const doc = seed();
  const entries: JournalEntry[] = [
    { seq: doc.seq + 1, at: NOW, by: 'human', intent: { type: 'node.advance' } },
  ];
  const first = replay(doc, entries);
  const second = replay(first.doc, entries);
  assert.equal(first.applied, 1);
  assert.equal(second.applied, 0);
  assert.deepEqual(second.doc.nodes, first.doc.nodes);
});

test('parseJournal 容忍损坏行并计数', () => {
  const text =
    '{"seq":1,"at":1,"by":"human","intent":{"type":"node.advance"}}\n' +
    'not json\n' +
    '\n' +
    '{"seq":2,"at":2,"by":"ai","intent":{"type":"node.back"}}\n';
  const parsed = parseJournal(text);
  assert.equal(parsed.entries.length, 2);
  assert.equal(parsed.corruptLines, 1);
});

test('增量 patch 可无损还原目标状态', () => {
  const before = seed();
  let after = run(before, { type: 'node.advance' });
  after = run(after, { type: 'node.advise', nodeId: 'nd_0002', advice: { text: '先想清楚', at: NOW } });

  const ops = diffDoc(before, after);
  const restored = applyPatch(before, ops);
  assert.ok(restored.ok, restored.ok ? '' : restored.reason);
  assert.deepEqual(restored.doc.nodes, after.nodes);
  assert.equal(restored.doc.pointer, after.pointer);
});

test('从零 patch（prev=null）能完整重建', () => {
  const doc = seed();
  const ops = diffDoc(null, doc);
  const rebuilt = applyPatch(emptyDoc({ id: doc.id, title: doc.title, now: NOW }), ops);
  assert.ok(rebuilt.ok, rebuilt.ok ? '' : rebuilt.reason);
  assert.deepEqual(rebuilt.doc.nodes, doc.nodes);
  assert.deepEqual(rebuilt.doc.roots, doc.roots);
});

test('协议信封：未知类型可解析但标记为 unknown', () => {
  const envelope = buildEnvelope('future.thing', { a: 1 }, { id: 'm1', ts: NOW });
  const decoded = decodeEnvelope(JSON.stringify(envelope));
  assert.ok(decoded.ok);
  assert.equal(decoded.known, false);
});

test('协议信封：高版本直接拒绝', () => {
  const envelope = buildEnvelope('hello', {}, { id: 'm2', ts: NOW });
  const decoded = decodeEnvelope(JSON.stringify({ ...envelope, v: 99 }));
  assert.equal(decoded.ok, false);
  assert.equal(decoded.code, 'version/incompatible');
});

test('协议信封：坏 JSON 返回可读原因', () => {
  const decoded = decodeEnvelope('{]');
  assert.equal(decoded.ok, false);
});
