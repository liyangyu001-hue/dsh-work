import assert from 'node:assert/strict';
import { test } from 'node:test';

import { describeEntry, titleLookup } from './feed.ts';
import type { JournalLine } from './feed.ts';

const at = '2026-10-23T15:00:00.000Z';

function line(seq: number, intent: Record<string, unknown>): JournalLine {
  return { seq, at, by: 'human', intent };
}

const titles = titleLookup({ n1: { title: '整理需求' } });
const titleOf = (id: string): string => titles(id);

test('plan.set 翻成「生成 N 件事」，带标题时附在末尾', () => {
  const a = describeEntry(line(1, { type: 'plan.set', nodes: [{}, {}, {}] }), titleOf);
  assert.equal(a.verb, '拆解');
  assert.equal(a.text, '生成 3 件事');

  const b = describeEntry(line(2, { type: 'plan.set', nodes: [{}, {}], title: '写文档' }), titleOf);
  assert.equal(b.text, '生成 2 件事「写文档」');
});

test('节点类意图取当前标题；取不到时退回 id 尾巴', () => {
  assert.equal(describeEntry(line(3, { type: 'node.complete', nodeId: 'n1' }), titleOf).text, '整理需求');
  assert.equal(describeEntry(line(4, { type: 'node.focus', nodeId: 'n2' }), titleOf).text, '#n2');
  // 空 id 不显示假尾巴
  assert.equal(describeEntry(line(5, { type: 'node.advance' }), titleOf).text, '完成当前并前进');
});

test('交接五态各有动词，退回标记为不顺', () => {
  const offer = describeEntry(line(6, { type: 'handoff.offer', nodeId: 'n1' }), titleOf);
  assert.equal(offer.verb, '交给 AI');
  assert.equal(offer.ok, true);

  const back = describeEntry(line(7, { type: 'handoff.return', nodeId: 'n1' }), titleOf);
  assert.equal(back.verb, '退回');
  assert.equal(back.ok, false);
});

test('未知意图照常显示，不被静默吞掉', () => {
  const unknown = describeEntry(line(8, { type: 'node.rename', nodeId: 'n1' }), titleOf);
  assert.equal(unknown.verb, '·');
  assert.equal(unknown.text, 'node.rename');
  assert.equal(describeEntry(line(9, {}), titleOf).text, '（无类型）');
});

test('时间统一成本地 HH:mm：毫秒数与老日志的 ISO 串都认', () => {
  // 现行 journal 存的是 epoch 毫秒（老日志才是 ISO 串，见 feed.ts 的 clock()）
  const ms = describeEntry(
    { seq: 10, at: 1792762590296, by: 'ai', intent: { type: 'node.focus', nodeId: 'n1' } },
    titleOf,
  );
  assert.match(ms.at, /^\d{2}:\d{2}$/);

  // 老日志是 ISO 串
  assert.match(describeEntry(line(11, { type: 'node.focus', nodeId: 'n1' }), titleOf).at, /^\d{2}:\d{2}$/);

  // 认不出的时间宁可空着，也别把 "NaN:NaN" 显示给人
  const broken = describeEntry({ seq: 12, at: 'not-a-date', by: 'human', intent: { type: 'node.advance' } }, titleOf);
  assert.equal(broken.at, '');
});

test('node.back / node.advise 有动词，别落到「·」', () => {
  assert.equal(describeEntry(line(13, { type: 'node.back' }), titleOf).verb, '回退');
  assert.equal(describeEntry(line(14, { type: 'node.advise', nodeId: 'n1' }), titleOf).verb, '建议');
});
