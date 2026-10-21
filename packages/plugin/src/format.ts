/**
 * 给模型看的文本格式。
 *
 * 为什么单独一个文件：`tools.ts` 顶部要 `import { defineTool } from '@deepseek-ai/dsh-tools'`，
 * 那个包只在宿主运行期存在，Node 直接跑测试会 `ERR_MODULE_NOT_FOUND`。
 * 把纯格式化搬到这个不碰宿主模块的文件里，就能单独测。
 */

import type { WorkDoc } from '@dshwork/core';
import { linearOrder, progress } from '@dshwork/core';

/** 谁被交给了 AI 但还没开始（offered 态）。顺序按树的先序，输出稳定。 */
export function offeredNodes(doc: WorkDoc): Array<WorkDoc['nodes'][string]> {
  const out: Array<WorkDoc['nodes'][string]> = [];
  for (const id of linearOrder(doc)) {
    const node = doc.nodes[id];
    if (node && node.handoff?.state === 'offered') out.push(node);
  }
  return out;
}

/** 给模型看的进度摘要：结论 → 当前节点 → 等在门口的交接 → 整棵树。 */
export function formatStatus(doc: WorkDoc): string {
  const p = progress(doc);
  const current = doc.pointer ? doc.nodes[doc.pointer] : null;
  const head = current
    ? `进度 ${p.done}/${p.total}，当前节点「${current.title}」${current.detail ? `：${current.detail}` : ''}`
    : p.total === 0
      ? '还没有任务树。'
      : `进度 ${p.done}/${p.total}，没有进行中的节点。`;

  const lines: string[] = [head];
  if (current?.acceptance) lines.push(`验收标准：${current.acceptance}`);
  if (current?.advice?.text) lines.push(`已有建议：${current.advice.text}`);
  if (current?.handoff) lines.push(`交接状态：${current.handoff.state}`);

  // 交接不只看当前节点：人可能把别的节点也丢过来了，模型得有地方看见。
  const waiting = offeredNodes(doc);
  if (waiting.length > 0) {
    lines.push('已交给你（等你接手的节点）：');
    for (const node of waiting) {
      const target = node.detail ? ` —— 目标：${node.detail}` : '';
      const check = node.acceptance ? `；验收：${node.acceptance}` : '';
      lines.push(`- ${node.title}（${node.id}）${target}${check}`);
    }
    lines.push('想接就调 work_handoff_start，做完调 work_handoff_submit。');
  }

  if (p.total > 0) {
    lines.push('任务树：');
    lines.push(formatTree(doc));
  }
  return lines.join('\n');
}

export function formatTree(doc: WorkDoc): string {
  const out: string[] = [];
  const walk = (id: string, depth: number): void => {
    const node = doc.nodes[id];
    if (!node) return;
    const mark =
      node.id === doc.pointer
        ? '▶'
        : node.status === 'done'
          ? '✔'
          : node.status === 'skipped'
            ? '⤼'
            : '·';
    const kind = node.kind === 'concept' ? '[学] ' : '';
    out.push(`${'  '.repeat(depth)}${mark} ${kind}${node.title}`);
    for (const child of node.children) walk(child, depth + 1);
  };
  for (const root of doc.roots) walk(root, 0);
  return out.join('\n');
}

export function titleOf(doc: WorkDoc, id: string | null): string {
  if (!id) return '(无)';
  return doc.nodes[id]?.title ?? id;
}
