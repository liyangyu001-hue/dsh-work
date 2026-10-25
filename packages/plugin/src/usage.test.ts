// 用量账本的纯函数测试：不碰网络、不碰宿主，只算数。
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { UsageMeter, describeUsage, readUsage } from './usage.ts';

test('从 usage 分片里读数：实测形状能认出来', () => {
  const chunk = {
    type: 'usage',
    usage: { inputTokens: 144, outputTokens: 357, cacheReadTokens: 128, cacheWriteTokens: 0, totalTokens: 629 },
  };
  assert.deepEqual(readUsage(chunk), { inputTokens: 144, outputTokens: 357, cacheReadTokens: 128 });
});

test('不是 usage 分片、没有数字、形状不对，一律返回 null', () => {
  assert.equal(readUsage({ type: 'text-delta', text: '你好' }), null);
  assert.equal(readUsage({ type: 'usage' }), null);
  assert.equal(readUsage({ type: 'usage', usage: {} }), null);
  assert.equal(readUsage({ type: 'usage', usage: { inputTokens: -3, outputTokens: 0 } }), null);
  assert.equal(readUsage(null), null);
  assert.equal(readUsage('usage'), null);
});

test('累加：多次调用相加，failed 单独记、不从 calls 里减', () => {
  const meter = new UsageMeter();
  meter.callStarted(1000);
  meter.addUsage({ inputTokens: 100, outputTokens: 50, cacheReadTokens: 20 });
  meter.callStarted(2000);
  meter.addUsage({ inputTokens: 40, outputTokens: 10, cacheReadTokens: 5 });
  meter.callStarted(3000);
  meter.callFailed();

  const snapshot = meter.snapshot();
  assert.equal(snapshot.calls, 3);
  assert.equal(snapshot.failed, 1);
  assert.equal(snapshot.inputTokens, 140);
  assert.equal(snapshot.outputTokens, 60);
  assert.equal(snapshot.cacheReadTokens, 25);
  // 第一次调用的时刻留着，后面再调更新不了它
  assert.equal(snapshot.startedAt, 1000);
});

test('复用的建议记成「省下的调用」，不算进 calls', () => {
  const meter = new UsageMeter();
  meter.savedCall();
  meter.savedCall();
  const snapshot = meter.snapshot();
  assert.equal(snapshot.savedCalls, 2);
  assert.equal(snapshot.calls, 0);
  assert.equal(snapshot.startedAt, null);
});

test('一句话账本：没调用过就说没问过，有 token 才报 token', () => {
  const meter = new UsageMeter();
  assert.equal(describeUsage(meter.snapshot()), '还没问过 AI');

  meter.callStarted(1000);
  assert.equal(describeUsage(meter.snapshot()), '1 次调用');

  meter.addUsage({ inputTokens: 600, outputTokens: 800, cacheReadTokens: 300 });
  const line = describeUsage(meter.snapshot());
  assert.equal(line, '1 次调用 · 1.4k tok · 其中缓存命中 0.3k');

  meter.callFailed();
  meter.savedCall();
  assert.equal(describeUsage(meter.snapshot()), '1 次调用 · 1.4k tok · 其中缓存命中 0.3k · 1 次失败 · 省下 1 次');
});

test('reset 之后账本归零', () => {
  const meter = new UsageMeter();
  meter.callStarted(1000);
  meter.addUsage({ inputTokens: 10, outputTokens: 20 });
  meter.callFailed();
  meter.savedCall();
  meter.reset();
  assert.deepEqual(meter.snapshot(), {
    calls: 0,
    failed: 0,
    inputTokens: 0,
    outputTokens: 0,
    cacheReadTokens: 0,
    savedCalls: 0,
    startedAt: null,
  });
});
