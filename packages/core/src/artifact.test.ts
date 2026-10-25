// 产物指针（`nodes/<id>.md`）的契约测试。
//
// 为什么单开一个文件：`node.artifact` 是「任务库能当档案用」的那颗螺丝——
// 复盘时真库 7 颗 done 的节点一条证据都没有，状态说做完了，却没有任何东西
// 能点开看。这份测试钉住四件事：能挂、能换、能显式清空、拒绝的路径必须
// 一个字节都不动文档。
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { emptyDoc, apply, validateDoc } from './state.ts';
import type { Intent, NodeId, WorkDoc } from './types.ts';

const AT = 1_700_000_000_000;

/** 造一棵最小可用的树：根 + 一个子节点，子节点是当前任务。 */
function fixture(): { doc: WorkDoc; root: NodeId; child: NodeId } {
  const doc = emptyDoc({ id: 'wd_demo', title: '搞懂幂等', now: AT });
  const root: NodeId = 'nd_root';
  const child: NodeId = 'nd_child';
  doc.nodes[root] = {
    id: root,
    parentId: null,
    children: [child],
    title: '搞懂幂等',
    detail: '先写一个失败重试场景',
    acceptance: '写下来',
    kind: 'concept',
    status: 'active',
    evidence: [],
    createdAt: AT,
    updatedAt: AT,
  };
  doc.nodes[child] = {
    id: child,
    parentId: root,
    children: [],
    title: '写第一个重试场景',
    detail: '',
    acceptance: '',
    kind: 'task',
    status: 'todo',
    evidence: [],
    createdAt: AT,
    updatedAt: AT,
  };
  doc.roots = [root];
  doc.pointer = child;
  return { doc, root, child };
}

function run(doc: WorkDoc, intent: Intent): { doc: WorkDoc } {
  const result = apply(doc, intent, { at: AT + 1, by: 'human' });
  assert.equal(result.ok, true, result.ok ? '' : result.reason);
  if (!result.ok) throw new Error('unreachable');
  return { doc: result.doc };
}

test('挂上产物：只落指针与时间，不碰状态与其它字段', () => {
  const { doc, child } = fixture();
  const before = doc.nodes[child]!;
  const next = run(doc, { type: 'node.artifact', nodeId: child, path: '.dsh-work/nodes/nd_child.md' });

  const after = next.doc.nodes[child]!;
  assert.deepEqual(after.artifact, { path: '.dsh-work/nodes/nd_child.md', at: AT + 1 });
  assert.equal(after.status, before.status);
  assert.equal(after.title, before.title);
  assert.equal(next.doc.pointer, child, '挂产物不该动当前任务');
  assert.equal(validateDoc(next.doc), null);
});

test('换掉产物：路径与时间跟着换', () => {
  const { doc, child } = fixture();
  const first = run(doc, { type: 'node.artifact', nodeId: child, path: 'a.md' });
  const second = run(first.doc, { type: 'node.artifact', nodeId: child, path: 'b.md' });
  assert.deepEqual(second.doc.nodes[child]!.artifact, { path: 'b.md', at: AT + 1 });
});

test('显式「没有产物」：传 null 落成 null，不是 undefined', () => {
  const { doc, child } = fixture();
  const first = run(doc, { type: 'node.artifact', nodeId: child, path: 'a.md' });
  const second = run(first.doc, { type: 'node.artifact', nodeId: child, path: null });
  const node = second.doc.nodes[child]!;
  assert.equal(node.artifact, null);
  assert.ok(!('artifact' in node) === false, 'null 要留在文档里，语义是「明确没有」');
});

test('拒绝：空路径、带 .. 的越界路径、不存在的节点', () => {
  const { doc, child } = fixture();
  const cases: Intent[] = [
    { type: 'node.artifact', nodeId: child, path: '' },
    { type: 'node.artifact', nodeId: child, path: '   ' },
    { type: 'node.artifact', nodeId: child, path: 'nodes/../../etc/passwd' },
    { type: 'node.artifact', nodeId: child, path: 'a\\..\\b.md' },
    { type: 'node.artifact', nodeId: 'nd_missing', path: 'a.md' },
  ];
  for (const intent of cases) {
    const snapshot = structuredClone(doc);
    const result = apply(doc, intent, { at: AT + 1, by: 'human' });
    assert.equal(result.ok, false, `应当拒绝：${JSON.stringify(intent)}`);
    if (result.ok) continue;
    assert.ok(result.reason.trim() !== '', '拒绝要给人话理由');
    assert.deepEqual(doc, snapshot, '拒绝之后原文档必须一个字节都没变');
  }
});

test('load 时挡脏数据：产物路径空、时间不是数字', () => {
  const { doc, child } = fixture();
  const brokenPath = structuredClone(doc);
  brokenPath.nodes[child]!.artifact = { path: '  ', at: AT };
  assert.match(String(validateDoc(brokenPath)), /产物路径是空的/);

  const brokenAt = structuredClone(doc);
  brokenAt.nodes[child]!.artifact = { path: 'a.md', at: Number.NaN };
  assert.match(String(validateDoc(brokenAt)), /产物时间不是数字/);
});
