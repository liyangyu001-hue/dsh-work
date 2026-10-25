/**
 * 「打开面板」指令桥的契约。
 *
 * 这里不碰真进程：拉起按钮只测"有没有发起、会不会连点两下开两个"，
 * 真启动由真机截图那一步验。
 */

import assert from 'node:assert/strict';
import test from 'node:test';

import { createUiBridge } from './ui-open.ts';

test('打开：伴随层不在线时留指令并拉起一次', () => {
  let now = 1_000;
  let launches = 0;
  const bridge = createUiBridge({
    now: () => now,
    launch: () => {
      launches += 1;
      return true;
    },
  });
  assert.equal(bridge.snapshot().seq, 0);
  assert.equal(bridge.companionAlive(), false);

  const outcome = bridge.open('panel');
  assert.equal(outcome.launched, true);
  assert.equal(outcome.companion, false);
  assert.equal(launches, 1);
  assert.deepEqual(outcome.command, { seq: 1, target: 'panel', at: 1_000 });
  assert.deepEqual(bridge.snapshot(), outcome.command);
});

test('打开：伴随层在线时只留指令，不再拉起', () => {
  let now = 1_000;
  let launches = 0;
  const bridge = createUiBridge({
    now: () => now,
    launch: () => {
      launches += 1;
      return true;
    },
  });
  bridge.noteCompanion();
  assert.equal(bridge.companionAlive(), true);

  const outcome = bridge.open('graph');
  assert.equal(outcome.companion, true);
  assert.equal(outcome.launched, false);
  assert.equal(launches, 0);
  assert.equal(outcome.command.target, 'graph');
});

test('在线判定会过期：几秒没轮询就当作掉了', () => {
  let now = 1_000;
  const bridge = createUiBridge({ now: () => now, aliveWindowMs: 5_000 });
  bridge.noteCompanion();
  now = 6_001;
  assert.equal(bridge.companionAlive(), false);
  const outcome = bridge.open('panel');
  assert.equal(outcome.companion, false);
  assert.match(outcome.hint ?? '', /没在运行/);
});

test('拉起失败后进冷却：连点不会反复 spawn', () => {
  let now = 1_000;
  let launches = 0;
  const bridge = createUiBridge({
    now: () => now,
    launch: () => {
      launches += 1;
      return false;
    },
  });

  const first = bridge.open('panel');
  assert.equal(first.launched, false);
  assert.match(first.hint ?? '', /启动失败/);

  now = 2_000;
  const second = bridge.open('panel');
  assert.equal(second.launched, false);
  assert.match(second.hint ?? '', /刚拉起/);
  assert.equal(launches, 1);
  assert.equal(second.command.seq, 2);
});

test('拉起抛错不掀翻宿主：当作没起来', () => {
  const bridge = createUiBridge({
    launch: () => {
      throw new Error('EPERM');
    },
  });
  const outcome = bridge.open('panel');
  assert.equal(outcome.launched, false);
  assert.match(outcome.hint ?? '', /启动失败/);
});

test('打开节点图可以带「怎么读」：rail 跟着指令走，不带就是普通读法', () => {
  const bridge = createUiBridge({ now: () => 5_000 });
  const learn = bridge.open('graph', 'learn');
  assert.deepEqual(learn.command, { seq: 1, target: 'graph', rail: 'learn', at: 5_000 });

  const office = bridge.open('graph', 'office');
  assert.deepEqual(office.command, { seq: 2, target: 'graph', rail: 'office', at: 5_000 });

  // 不带 rail：指令里不该凭空多出一个字段（伴随层按 undefined 判「普通读法」）。
  const plain = bridge.open('graph');
  assert.deepEqual(plain.command, { seq: 3, target: 'graph', at: 5_000 });
  assert.equal('rail' in plain.command, false);
});