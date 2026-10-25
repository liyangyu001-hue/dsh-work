// 建议链路的纯函数测试：解析这层不碰网络，也不碰模型。
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { adviceSignature, cachedAdvice, parseAdvice } from './advise.ts';

const brief = { title: '写出当前理解', detail: '写下失败的重试场景', acceptance: '两百字以内' };

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

/* ───────────── 指纹与缓存：同一个问题不该问两次模型 ───────────── */

test('同样的内容算出同样的指纹', () => {
  const a = adviceSignature(brief, 'deepseek', 'v4');
  const b = adviceSignature({ ...brief }, 'deepseek', 'v4');
  assert.equal(a, b);
});

test('指纹忽略首尾空白与换行写法（人改个空格不该重新计费）', () => {
  const a = adviceSignature(brief, 'deepseek', 'v4');
  const b = adviceSignature(
    { title: ' 写出当前理解 ', detail: '写下失败的重试场景\n', acceptance: '两百字以内  ' },
    'deepseek',
    'v4',
  );
  assert.equal(a, b);
});

test('换了模型或改了内容，指纹必须变', () => {
  const base = adviceSignature(brief, 'deepseek', 'v4');
  assert.notEqual(adviceSignature(brief, 'deepseek', 'v5'), base);
  assert.notEqual(adviceSignature({ ...brief, acceptance: '三百字以内' }, 'deepseek', 'v4'), base);
  assert.notEqual(adviceSignature({ ...brief, title: '写出理解' }, 'deepseek', 'v4'), base);
});

test('标题里带分隔符也拼不出同一个指纹', () => {
  const one = adviceSignature({ title: 'a\u0001b', detail: 'c', acceptance: '' }, 'p', 'm');
  const two = adviceSignature({ title: 'a', detail: 'b\u0001c', acceptance: '' }, 'p', 'm');
  assert.notEqual(one, two);
});

test('指纹一致就复用旧建议，不再调模型', () => {
  const sig = adviceSignature(brief, 'deepseek', 'v4');
  const cached = cachedAdvice({ text: '先写一句失败场景', alt: '先翻上次的失败记录', sig }, sig);
  assert.deepEqual(cached, { text: '先写一句失败场景', alt: '先翻上次的失败记录' });
});

test('没指纹、指纹过期、正文为空，一律不复用', () => {
  const sig = adviceSignature(brief, 'deepseek', 'v4');
  assert.equal(cachedAdvice({ text: '旧建议' }, sig), null);
  assert.equal(cachedAdvice({ text: '旧建议', sig: '别的指纹' }, sig), null);
  assert.equal(cachedAdvice({ text: '   ', sig }, sig), null);
  assert.equal(cachedAdvice(null, sig), null);
  assert.equal(cachedAdvice(undefined, sig), null);
});