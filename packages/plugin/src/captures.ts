// 截图留存：列目录、按修改时间删老图、一句话说明它放在哪。
//
// 为什么要清：`%USERPROFILE%\.dsh\dsh-work\captures` 到 2026-10-25 已经 64 个文件
// 2.89 MB，全是整屏 PNG，一天几十张就是几十 MB。`docs/GUIDE.md` 的风险台账写着
// 「默认 7 天清理」，但代码里一直没实现——这里是补上那一句承诺。
//
// 边界（比功能重要）：
// 1) 只删**本插件命名**的文件（`yyyyMMdd-HHmmss-mmm.png` 与 `...tmp-...-N.png`），
//    目录里别人放的东西一个都不碰；
// 2) 按**修改时间**判断，不解析文件名里的日期（用户拷进来的旧图不该被当成今天的）；
// 3) `keepDays <= 0` = 永不删（配置里可以这么关掉）；
// 4) 删不掉（占用/权限）就跳过，不抛错——清理失败不该让宿主起不来。

import { readdir, stat, unlink } from 'node:fs/promises';
import { homedir } from 'node:os';
import { join } from 'node:path';

/** 默认保留天数。 */
export const DEFAULT_CAPTURE_KEEP_DAYS = 7;

const DAY_MS = 24 * 60 * 60 * 1000;

/** `20261025-235437-705.png`：`capture.h` 的 `TimestampedPath()` 生成的名字。 */
const CAPTURE_NAME = /^\d{8}-\d{6}-\d{3}\.png$/;

/** `.tmp-20261019-232038-517-0.png`：多屏合成时的中间文件。 */
const TEMP_NAME = /^\.tmp-\d{8}-\d{6}-\d{3}-\d+\.png$/;

/** 截图目录：与原生侧 `capture.h:186` 同一条路径（`%USERPROFILE%\.dsh\dsh-work\captures`）。
 *
 * `DSH_WORK_CAPTURES_DIR` 可以把它挪走——探针与将来的测试靠这个在不碰真图的前提下
 * 验「删旧的、留新的、别动别人的文件」。
 */
export function captureDir(env: NodeJS.ProcessEnv = process.env): string {
  const override = env.DSH_WORK_CAPTURES_DIR?.trim();
  if (override !== undefined && override !== '') return override;
  const profile = env.USERPROFILE ?? env.HOME ?? homedir();
  return join(profile, '.dsh', 'dsh-work', 'captures');
}

/** 这个名字是不是本插件自己写的截图（决定「能不能删」）。 */
export function isCaptureName(name: string): boolean {
  return CAPTURE_NAME.test(name) || TEMP_NAME.test(name);
}

export interface SweepResult {
  /** 真正删掉的文件数。 */
  readonly removed: number;
  /** 留着没动的截图数（不含别人的文件）。 */
  readonly kept: number;
  /** 留着的那部分一共多少字节。 */
  readonly bytes: number;
  /** 目录还不存在（第一次用、或者用户从没截过图）。 */
  readonly missing: boolean;
}

/**
 * 清一次老截图。
 *
 * `now` 是为了测试能给定时刻，生产代码不用传。
 */
export async function sweepCaptures(
  dir: string,
  keepDays: number,
  now: number = Date.now(),
): Promise<SweepResult> {
  let names: string[];
  try {
    names = await readdir(dir);
  } catch {
    return { removed: 0, kept: 0, bytes: 0, missing: true };
  }

  const cutoff = now - keepDays * DAY_MS;
  let removed = 0;
  let kept = 0;
  let bytes = 0;

  for (const name of names) {
    if (!isCaptureName(name)) continue; // 不是自己写的东西：不数、不删
    const full = join(dir, name);
    let info;
    try {
      info = await stat(full);
    } catch {
      continue; // 正好被别的进程删了
    }
    if (!info.isFile()) continue;
    if (keepDays > 0 && info.mtimeMs < cutoff) {
      try {
        await unlink(full);
        removed += 1;
      } catch {
        // 占用或权限不足：留着，下次再说
        kept += 1;
        bytes += info.size;
      }
      continue;
    }
    kept += 1;
    bytes += info.size;
  }

  return { removed, kept, bytes, missing: false };
}

/** 一句话说明：「截图存在 …\captures，保留 7 天（现在 3 张 / 1.2 MB）」。 */
export function describeCaptures(dir: string, keepDays: number, result: SweepResult): string {
  const size = result.bytes >= 1024 * 1024
    ? `${(result.bytes / (1024 * 1024)).toFixed(1)} MB`
    : `${Math.round(result.bytes / 1024)} KB`;
  const policy = keepDays > 0 ? `保留 ${keepDays} 天` : '永不自动删';
  if (result.missing) return `截图还没地方放：${dir}（${policy}）`;
  const removed = result.removed > 0 ? `，这次清掉 ${result.removed} 张` : '';
  return `截图存在 ${dir}（${policy}，现在 ${result.kept} 张 / ${size}${removed}）`;
}
