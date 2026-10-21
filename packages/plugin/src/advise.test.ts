// 建议链路的纯函数测试：解析这层不碰网络，也不碰模型。
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseAdvice } from './advise.ts';

test('标准 JSON 能解析出 text 与 alt', () => {
  const advice = parseAdvice('{"text":"先把验收标准写成三行","alt":"先跑一遍现有流程"}');
  assert.deepEqual(advice, { text: '先把验收标准写成三行', alt: '先跑一遍现有流程' });
});

test('没有 alt 时只返回 text', () => {
  assert.deepEqual(parseAdvice('{"text":"把标题缩短到 12 个字"}'), { text: '把标题缩短到 12 个字' });
});

test('前后有解释文字也能挖出来', () => {
  const advice = parseAdvice('好的，建议如下：\n{"text":"先确认这份数据是哪天的"}\n还需要我展开吗？');
  assert.equal(advice?.text, '先确认这份数据是哪天的');
});

test('alt 是空串时当作没有', () => {
  assert.deepEqual(parseAdvice('{"text":"先存一版","alt":"   "}'), { text: '先存一版' });
});

test('text 为空或缺失一律不接受', () => {
  assert.equal(parseAdvice('{"text":""}'), null);
  assert.equal(parseAdvice('{"alt":"只有替代方案"}'), null);
});

test('完全不是 JSON 就返回 null，不编造', () => {
  assert.equal(parseAdvice('我觉得应该先做 A 再做 B'), null);
  assert.equal(parseAdvice(''), null);
});