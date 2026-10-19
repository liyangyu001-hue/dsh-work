import assert from 'node:assert/strict';
import test from 'node:test';

import { normalizePlanSpecs, parseOutline, parsePlanInput } from './plan-text.ts';

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

