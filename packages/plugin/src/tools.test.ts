import assert from 'node:assert/strict';
import test from 'node:test';

import { normalizePlanSpecs, parseOutline, parsePlanInput } from './plan-text.ts';
import { emptyDoc, materializePlan } from '@dshwork/core';
import type { WorkDoc } from '@dshwork/core';
import { formatStatus, offeredNodes } from './format.ts';

test('缩进大纲：两个空格一级，挂到父节点下', () => {
  const specs = parseOutline(['- 需求梳理', '  - 明确目标', '  - 列出约束', '- 实现', '  - 写核心逻辑'].join('\n'));
  assert.equal(specs.length, 2);
  assert.equal(specs[0]!.title, '需求梳理');
  assert.deepEqual(
    specs[0]!.children?.map((c) => c.title),
    ['明确目标', '列出约束'],
  );
  assert.equal(specs[1]!.title, '实现');
  assert.equal(specs[1]!.children?.[0]?.title, '写核心逻辑');
});

test('缩进大纲：标题后的 :: 变成验收标准', () => {
  const specs = parseOutline('- 写测试 :: 全部通过');
  assert.equal(specs[0]!.title, '写测试');
  assert.equal(specs[0]!.acceptance, '全部通过');
});

test('缩进大纲：Tab 等同于两个空格，序号前缀被剥掉', () => {
  const specs = parseOutline(['1. 第一层', '\t1) 第二层'].join('\n'));
  assert.equal(specs[0]!.title, '第一层');
  assert.equal(specs[0]!.children?.[0]?.title, '第二层');
});

test('缩进大纲：跳跃缩进仍挂到最近的更浅节点上', () => {
  const specs = parseOutline(['- A', '      - 深', '- B'].join('\n'));
  assert.equal(specs.length, 2);
  assert.equal(specs[0]!.children?.[0]?.title, '深');
});

test('parsePlanInput：JSON 数组直通', () => {
  const specs = parsePlanInput('[{"title":"A","acceptance":"done","children":[{"title":"A1"}]}]');
  assert.equal(specs[0]!.title, 'A');
  assert.equal(specs[0]!.acceptance, 'done');
  assert.equal(specs[0]!.children?.[0]?.title, 'A1');
});

test('parsePlanInput：坏 JSON 退回大纲解析，不整条丢弃', () => {
  const specs = parsePlanInput('[\n- A\n- B\n]');
  assert.ok(specs.length >= 1, '至少救回一个节点');
});

test('parsePlanInput：空输入与非法类型返回空数组', () => {
  assert.deepEqual(parsePlanInput(''), []);
  assert.deepEqual(parsePlanInput(undefined), []);
  assert.deepEqual(parsePlanInput(42), []);
});

test('normalizePlanSpecs：丢掉无标题节点并剥掉多余字段', () => {
  const specs = normalizePlanSpecs([{ title: '  A  ', junk: 1 }, { title: '   ' }, null]);
  assert.equal(specs.length, 1);
  assert.equal(specs[0]!.title, 'A');
  assert.deepEqual(Object.keys(specs[0]!).sort(), ['title']);
});



/* ── 交接：谁在等我接 ───────────────────────────────────────── */

function docWithOneOffered(): WorkDoc {
  const plan = materializePlan(
    [
      { title: '先做这个' },
      { title: '交给 AI 的那个' },
      { title: '后面再说' },
    ],
    { now: 1_700_000_000_000, newId: (() => {
      let n = 0;
      return () => `nd_${++n}`;
    })() },
  );
  const base = emptyDoc({ id: 'wk_test', title: '测试', now: 1_700_000_000_000 });
  // materializePlan 给的是数组，文档里要的是按 id 索引的表
  const nodes: WorkDoc['nodes'] = {};
  for (const node of plan.nodes) nodes[node.id] = node;
  const doc: WorkDoc = { ...base, roots: plan.roots, nodes };
  const target = plan.roots[1]!;
  doc.nodes[target] = {
    ...doc.nodes[target]!,
    handoff: { state: 'offered', since: 1_700_000_100_000 },
  };
  return doc;
}

test('offeredNodes：只挑出 offered 的节点，顺序按树', () => {
  const doc = docWithOneOffered();
  const waiting = offeredNodes(doc);
  assert.equal(waiting.length, 1);
  assert.equal(waiting[0]!.title, '交给 AI 的那个');
});

test('offeredNodes：running / review 不算「等我接」', () => {
  const doc = docWithOneOffered();
  const id = doc.roots[1]!;
  doc.nodes[id] = { ...doc.nodes[id]!, handoff: { state: 'running', agentSessionId: 's1', since: 1 } };
  assert.equal(offeredNodes(doc).length, 0);
});

test('formatStatus：有节点被交出来时给出明确动作提示', () => {
  const text = formatStatus(docWithOneOffered());
  assert.match(text, /已交给你/);
  assert.match(text, /work_handoff_start/);
  assert.match(text, /交给 AI 的那个/);
});

test('formatStatus：没有交接时不出现这一段', () => {
  const doc = docWithOneOffered();
  const id = doc.roots[1]!;
  doc.nodes[id] = { ...doc.nodes[id]!, handoff: null };
  assert.doesNotMatch(formatStatus(doc), /已交给你/);
});
