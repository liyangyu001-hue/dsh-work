// 节点档案：`.dsh-work/nodes/<id>.md` 的读写。
//
// 复盘时发现真库 7 颗 `done` 的节点、`evidence` 全空——「做完了」只有状态，
// 没有任何东西能点开看。这一步把产物做成一份真文件：能写、能看、能 diff，
// 而且换机器换目录都不失效（`WorkNode.artifact.path` 存的是相对任务库根目录的路径）。
//
// 边界：**只碰 `.dsh-work/nodes/` 下面的文件**。路径从 HTTP 进来可能是任意字符串，
// 所以读写的入口都先做一次「解析后必须仍在该目录内」的检查，越界一律拒绝。

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

import type { WorkNode } from '@dshwork/core';

/** 产物根目录（相对任务库根目录），用正斜杠存进文档，换平台也不变义。 */
export const ARTIFACT_DIR = '.dsh-work/nodes';

/** 一份产物最多读这么多：够看，不至于把整本大书塞进界面。 */
export const MAX_ARTIFACT_BYTES = 512 * 1024;

/** 文件名里只允许这些字符，别让 `../` 这类东西混进来。 */
const SAFE_ID = /^[A-Za-z0-9_-]{1,128}$/;

/** 这个节点的产物路径（相对任务库根目录，正斜杠）。 */
export function artifactRelPath(nodeId: string): string {
  if (!SAFE_ID.test(nodeId)) throw new Error(`节点 id 不像 id：${nodeId}`);
  return `${ARTIFACT_DIR}/${nodeId}.md`;
}

/**
 * 把文档里存的相对路径解析成绝对路径，并确认它真的在产物目录里。
 *
 * 文档是用户可编辑的，`artifact.path` 可能是手改过的任意值，所以这里
 * 不能只信调用方；越界就抛错，绝不悄悄写到别处去。
 */
export function resolveArtifactPath(root: string, relPath: string): string {
  if (relPath.trim() === '') throw new Error('产物路径是空的');
  const dir = path.resolve(root, ARTIFACT_DIR);
  const absolute = path.resolve(root, relPath);
  const prefix = dir + path.sep;
  if (absolute !== dir && !absolute.startsWith(prefix)) {
    throw new Error(`产物路径 ${relPath} 不在 ${ARTIFACT_DIR} 里`);
  }
  return absolute;
}

/** 第一次写档案时用的骨架：把已知的目标与验收先摆进去，省得对着空文件发呆。 */
export function artifactTemplate(node: WorkNode, at: number): string {
  const when = new Date(at).toISOString().replace('T', ' ').slice(0, 16);
  const lines = [
    `# ${node.title}`,
    '',
    `> 建档案时间：${when} · 节点 ${node.id}`,
    '',
  ];
  if (typeof node.detail === 'string' && node.detail.trim() !== '') {
    lines.push('## 要做什么', '', node.detail.trim(), '');
  }
  if (typeof node.acceptance === 'string' && node.acceptance.trim() !== '') {
    lines.push('## 什么算做完', '', node.acceptance.trim(), '');
  }
  lines.push(
    '## 过程',
    '',
    '（把做过的事、遇到的问题写在这里；写完这一步，节点才算真的有东西可看）',
    '',
    '## 结论',
    '',
    '（一句话说清结果；没有结论就先空着）',
    '',
  );
  return lines.join('\n');
}

export interface ArtifactView {
  /** 相对任务库根目录的路径，正斜杠。 */
  readonly path: string;
  /** 文件是否存在（还没写过就是 false）。 */
  readonly exists: boolean;
  /** 内容；不存在或超出上限时为 null。 */
  readonly content: string | null;
  /** 超出上限时为 true，界面据此提示「文件太大，用编辑器打开」。 */
  readonly tooLarge: boolean;
}

/** 读一份产物；文件不存在不算错误，返回 exists: false。 */
export async function readArtifact(root: string, relPath: string): Promise<ArtifactView> {
  const absolute = resolveArtifactPath(root, relPath);
  try {
    const content = await readFile(absolute, 'utf8');
    const tooLarge = Buffer.byteLength(content, 'utf8') > MAX_ARTIFACT_BYTES;
    return { path: relPath, exists: true, content: tooLarge ? null : content, tooLarge };
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') {
      return { path: relPath, exists: false, content: null, tooLarge: false };
    }
    throw error;
  }
}

/** 覆盖写一份产物（父目录不存在就建）。 */
export async function writeArtifact(root: string, relPath: string, text: string): Promise<void> {
  const absolute = resolveArtifactPath(root, relPath);
  await mkdir(path.dirname(absolute), { recursive: true });
  await writeFile(absolute, text, 'utf8');
}

/**
 * 保证这个节点有一份档案：已经存在就原样读回来，不存在就写骨架。
 *
 * 返回的 `created` 让调用方知道是新建的还是复用的——界面据此说
 * 「已建档案」还是「档案还在那儿」。
 */
export async function ensureArtifact(
  root: string,
  node: WorkNode,
  at: number,
): Promise<{ view: ArtifactView; created: boolean }> {
  return ensureArtifactAt(root, artifactRelPath(node.id), node, at);
}

/**
 * 按给定路径保证档案存在。
 *
 * 文档里已经挂过指针时（用户可能手改过文件名）要认那一份，所以路径
 * 得能从外面传进来；`ensureArtifact()` 只是它按默认命名的薄壳。
 */
export async function ensureArtifactAt(
  root: string,
  relPath: string,
  node: WorkNode,
  at: number,
): Promise<{ view: ArtifactView; created: boolean }> {
  const existing = await readArtifact(root, relPath);
  if (existing.exists) return { view: existing, created: false };
  const text = artifactTemplate(node, at);
  await writeArtifact(root, relPath, text);
  return { view: { path: relPath, exists: true, content: text, tooLarge: false }, created: true };
}
