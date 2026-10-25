// 截图清理的测试：只认自己命名的文件、按 mtime 判断、删失败不抛。
import assert from 'node:assert/strict';
import { mkdtemp, rm, stat, utimes, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { captureDir, describeCaptures, isCaptureName, sweepCaptures } from './captures.ts';

const DAY = 24 * 60 * 60 * 1000;

async function scratch(): Promise<string> {
  return await mkdtemp(join(tmpdir(), 'dshwork-captures-'));
}

test('只认自己写的名字：编号图算，别人的图不算', () => {
  assert.equal(isCaptureName('20261025-235437-705.png'), true);
  assert.equal(isCaptureName('.tmp-20261019-232038-517-0.png'), true);
  assert.equal(isCaptureName('notes.md'), false);
  assert.equal(isCaptureName('screenshot.png'), false);
  assert.equal(isCaptureName('20261025-235437-705.png.bak'), false);
  assert.equal(isCaptureName('20261025-235437.png'), false); // 少了毫秒段
});

test('7 天前的删掉、今天的留着、别人的文件一个都不碰', async () => {
  const dir = await scratch();
  const now = Date.UTC(2026, 9, 26, 0, 0, 0);
  const old = join(dir, '20261010-101010-101.png');
  const fresh = join(dir, '20261025-235437-705.png');
  const foreign = join(dir, 'my-notes.png');
  await writeFile(old, Buffer.alloc(1024));
  await writeFile(fresh, Buffer.alloc(2048));
  await writeFile(foreign, 'x');
  const past = new Date(now - 9 * DAY);
  await utimes(old, past, past);

  const result = await sweepCaptures(dir, 7, now);

  assert.equal(result.removed, 1);
  assert.equal(result.kept, 1); // 今天的那个
  assert.equal(result.bytes, 2048);
  await assert.rejects(() => stat(old));
  assert.equal((await stat(fresh)).size, 2048);
  assert.equal((await stat(foreign)).size, 1); // 别人的文件还在
  await rm(dir, { recursive: true, force: true });
});

test('keepDays = 0 表示永不删，只数不删', async () => {
  const dir = await scratch();
  const now = Date.UTC(2026, 9, 26, 0, 0, 0);
  const old = join(dir, '20250101-000000-000.png');
  await writeFile(old, Buffer.alloc(10));
  const past = new Date(now - 300 * DAY);
  await utimes(old, past, past);

  const result = await sweepCaptures(dir, 0, now);

  assert.equal(result.removed, 0);
  assert.equal(result.kept, 1);
  assert.equal((await stat(old)).size, 10);
  await rm(dir, { recursive: true, force: true });
});

test('目录还不存在时安静地返回 missing，不抛错', async () => {
  const result = await sweepCaptures(join(tmpdir(), 'dshwork-captures-nonexistent-' + Date.now()), 7);
  assert.deepEqual(result, { removed: 0, kept: 0, bytes: 0, missing: true });
});

test('临时文件也按同一规则清（`.tmp-…-N.png`）', async () => {
  const dir = await scratch();
  const now = Date.UTC(2026, 9, 26, 0, 0, 0);
  const tmp = join(dir, '.tmp-20261001-120000-000-1.png');
  await writeFile(tmp, Buffer.alloc(5));
  const past = new Date(now - 8 * DAY);
  await utimes(tmp, past, past);

  const result = await sweepCaptures(dir, 7, now);
  assert.equal(result.removed, 1);
  await rm(dir, { recursive: true, force: true });
});

test('captureDir 跟原生侧同一条路径；describeCaptures 把路径与期限说清楚', () => {
  assert.match(captureDir({ USERPROFILE: 'C:\\Users\\x' }), /captures$/);
  assert.match(captureDir({ USERPROFILE: 'C:\\Users\\x' }), /\.dsh/);
  // 探针/测试可以把它挪到别处，免得动真图
  assert.equal(captureDir({ DSH_WORK_CAPTURES_DIR: 'D:\\tmp\\caps' }), 'D:\\tmp\\caps');

  const line = describeCaptures('C:\\c', 7, { removed: 2, kept: 3, bytes: 1536, missing: false });
  assert.match(line, /C:\\c/);
  assert.match(line, /保留 7 天/);
  assert.match(line, /3 张/);
  assert.match(line, /清掉 2 张/);

  assert.match(describeCaptures('C:\\c', 0, { removed: 0, kept: 0, bytes: 0, missing: false }), /永不自动删/);
  assert.match(describeCaptures('C:\\c', 7, { removed: 0, kept: 0, bytes: 0, missing: true }), /还没地方放/);
});
