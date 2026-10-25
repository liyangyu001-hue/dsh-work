// 节点档案的读写边界测试：路径必须留在 `.dsh-work/nodes/` 里。
import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { test } from 'node:test';

import type { WorkNode } from '@dshwork/core';

import {
  ARTIFACT_DIR,
  artifactRelPath,
  artifactTemplate,
  ensureArtifact,
  readArtifact,
  resolveArtifactPath,
  writeArtifact,
} from './artifact.ts';

const AT = 1_700_000_000_000;

function node(overrides: Partial<WorkNode> = {}): WorkNode {
  return {
    id: 'nd_demo',
    parentId: null,
    children: [],
    title: '写第一个重试场景',
    detail: '十分钟写下你遇到过的失败重试场景',
    acceptance: '能讲清重试和幂等的区别',
    kind: 'task',
    status: 'active',
    evidence: [],
    createdAt: AT,
    updatedAt: AT,
    ...overrides,
  };
}

async function withRoot(fn: (root: string) => Promise<void>): Promise<void> {
  const root = await mkdtemp(path.join(os.tmpdir(), 'dshwork-artifact-'));
  try {
    await fn(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

test('路径：相对路径统一用正斜杠，非法 id 直接拒绝', () => {
  assert.equal(artifactRelPath('nd_demo'), `${ARTIFACT_DIR}/nd_demo.md`);
  assert.throws(() => artifactRelPath('../escape'), /不像 id/);
  assert.throws(() => artifactRelPath('a/b'), /不像 id/);
});

test('路径越界：解析后必须仍在产物目录里', () => {
  const root = path.resolve('/tmp/root');
  assert.equal(resolveArtifactPath(root, `${ARTIFACT_DIR}/nd_demo.md`), path.join(root, ARTIFACT_DIR, 'nd_demo.md'));
  assert.throws(() => resolveArtifactPath(root, '.dsh-work/journal.ndjson'), /不在 .dsh-work\/nodes 里/);
  assert.throws(() => resolveArtifactPath(root, `${ARTIFACT_DIR}/../journal.ndjson`), /不在 .dsh-work\/nodes 里/);
  assert.throws(() => resolveArtifactPath(root, ''), /空的/);
});

test('骨架：标题、要做什么、什么算做完都进得去；空字段不留空段', () => {
  const text = artifactTemplate(node(), AT);
  assert.match(text, /^# 写第一个重试场景/);
  assert.match(text, /## 要做什么\n\n十分钟写下你遇到过的失败重试场景/);
  assert.match(text, /## 什么算做完\n\n能讲清重试和幂等的区别/);
  assert.match(text, /## 过程/);
  assert.match(text, /## 结论/);

  const bare = artifactTemplate(node({ detail: '', acceptance: '' }), AT);
  assert.equal(bare.includes('## 要做什么'), false);
  assert.equal(bare.includes('## 什么算做完'), false);
});

test('读写：第一次确保会建骨架，第二次原样读回、不再覆盖', async () => {
  await withRoot(async (root) => {
    const first = await ensureArtifact(root, node(), AT);
    assert.equal(first.created, true);
    assert.equal(first.view.exists, true);
    const onDisk = await readFile(path.join(root, ARTIFACT_DIR, 'nd_demo.md'), 'utf8');
    assert.equal(first.view.content, onDisk);

    // 用户手改过内容，第二次绝不能把骨架写回去覆盖掉
    await writeArtifact(root, artifactRelPath('nd_demo'), '# 手改过的\n');
    const second = await ensureArtifact(root, node(), AT + 1);
    assert.equal(second.created, false);
    assert.equal(second.view.content, '# 手改过的\n');
  });
});

test('读不存在的产物不算错：exists 为 false、content 为 null', async () => {
  await withRoot(async (root) => {
    const view = await readArtifact(root, artifactRelPath('nd_missing'));
    assert.equal(view.exists, false);
    assert.equal(view.content, null);
    assert.equal(view.tooLarge, false);
  });
});

test('太大就不塞进界面：超过上限时 content 为 null、tooLarge 为 true', async () => {
  await withRoot(async (root) => {
    const relPath = artifactRelPath('nd_big');
    const absolute = path.join(root, relPath);
    await writeFile(absolute, 'x'.repeat(600 * 1024), { encoding: 'utf8' }).catch(async () => {
      await writeArtifact(root, relPath, 'x'.repeat(600 * 1024));
    });
    const view = await readArtifact(root, relPath);
    assert.equal(view.exists, true);
    assert.equal(view.content, null);
    assert.equal(view.tooLarge, true);
  });
});
