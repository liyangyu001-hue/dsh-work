import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

import { createSequentialIdFactory, materializePlan } from '@dshwork/core';
import { WorkStore } from './store.ts';

async function tempRoot(): Promise<string> {
  return await mkdtemp(path.join(os.tmpdir(), 'dshwork-store-'));
}

function planOf(titles: string[]) {
  const newId = createSequentialIdFactory('st');
  const { roots, nodes } = materializePlan(
    titles.map((title) => ({ title })),
    { now: 1000, newId },
  );
  return { roots, nodes };
}

test('空目录打开即初始化，并落下快照', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root, title: '测试' });
    assert.equal(store.snapshot.title, '测试');
    assert.equal(store.seq, 0);
    const raw = await readFile(path.join(root, '.dsh-work', 'work.json'), 'utf8');
    const parsed = JSON.parse(raw);
    assert.equal(parsed.schemaVersion, 1);
    assert.equal(parsed.doc.title, '测试');
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('提交后重开，状态完整还原', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root, title: '还原' });
    const result = await store.commit({ type: 'plan.set', ...planOf(['一', '二']) }, 'ai');
    assert.ok(result.ok, result.ok ? '' : result.reason);
    const pointer = store.snapshot.pointer;

    const reopened = await WorkStore.open({ root });
    assert.equal(reopened.snapshot.pointer, pointer);
    assert.equal(reopened.seq, 1);
    assert.deepEqual(
      Object.keys(reopened.snapshot.nodes).sort(),
      Object.keys(store.snapshot.nodes).sort(),
    );
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('日志一行一条，且坏行不影响恢复', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root });
    await store.commit({ type: 'plan.set', ...planOf(['甲', '乙']) }, 'ai');
    await store.commit({ type: 'node.advance' }, 'human');

    const journalPath = path.join(root, '.dsh-work', 'journal.ndjson');
    const text = await readFile(journalPath, 'utf8');
    assert.equal(text.trim().split('\n').length, 2);

    // 人为破坏一行，模拟断电写了半行
    await writeFile(journalPath, `${text}半行坏掉`, 'utf8');
    const reopened = await WorkStore.open({ root });
    assert.equal(reopened.seq, 2);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('被拒绝的意图不落日志', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root });
    await store.commit({ type: 'plan.set', ...planOf(['甲']) }, 'ai');
    const before = await readFile(path.join(root, '.dsh-work', 'journal.ndjson'), 'utf8');

    const rejected = await store.commit({ type: 'node.back' }, 'human');
    assert.equal(rejected.ok, false);

    const after = await readFile(path.join(root, '.dsh-work', 'journal.ndjson'), 'utf8');
    assert.equal(after, before);
    assert.equal(store.seq, 1);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('并发提交被串行化：seq 不跳号、不丢意图', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root });
    await store.commit({ type: 'plan.set', ...planOf(['一', '二', '三', '四', '五', '六']) }, 'ai');

    const results = await Promise.all(
      Array.from({ length: 4 }, () => store.commit({ type: 'node.advance' }, 'human')),
    );
    assert.equal(results.filter((item) => item.ok).length, 4);
    assert.equal(store.seq, 5);

    const journal = await readFile(path.join(root, '.dsh-work', 'journal.ndjson'), 'utf8');
    assert.equal(journal.trim().split('\n').length, 5);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('订阅者能收到变更，且订阅者异常不外溢', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root });
    const seen: number[] = [];
    store.subscribe(() => {
      throw new Error('订阅者自己炸了');
    });
    store.subscribe((event) => seen.push(event.entry.seq));

    const result = await store.commit({ type: 'plan.set', ...planOf(['甲']) }, 'ai');
    assert.ok(result.ok);
    assert.deepEqual(seen, [1]);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test('reset 清空日志但保留标题', async () => {
  const root = await tempRoot();
  try {
    const store = await WorkStore.open({ root, title: '保留我' });
    await store.commit({ type: 'plan.set', ...planOf(['甲']) }, 'ai');
    await store.reset();
    assert.equal(store.seq, 0);
    assert.equal(store.snapshot.title, '保留我');
    assert.equal(Object.keys(store.snapshot.nodes).length, 0);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
