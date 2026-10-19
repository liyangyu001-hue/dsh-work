/**
 * 把模型写的「计划文本」解析成 PlanSpec[]。
 *
 * 这个文件**不依赖宿主**（不 import 任何 @deepseek-ai 模块），所以可以直接单测，
 * 也可以被客户端、伴随层复用做预览。工具层只负责把解析结果交给状态机。
 *
 * 两种输入形态：
 * - 缩进大纲（推荐）：`- 标题`，两个空格一级，`标题 :: 验收标准` 附带验收；
 * - JSON 数组：形状同 PlanSpec[]，需要 detail / kind 时用它。
 */

import type { PlanSpec } from '@dshwork/core';

export function parsePlanInput(raw: unknown): PlanSpec[] {
  if (Array.isArray(raw)) return normalizePlanSpecs(raw);
  if (typeof raw !== 'string') return [];
  const text = raw.trim();
  if (text === '') return [];
  if (text.startsWith('[') || text.startsWith('{')) {
    try {
      const parsed: unknown = JSON.parse(text);
      const nodes =
        Array.isArray(parsed) ? parsed : (parsed as { nodes?: unknown }).nodes;
      return normalizePlanSpecs(nodes);
    } catch {
      // JSON 坏了就退回大纲解析：一行行拆通常还能救回结构
      return parseOutline(text);
    }
  }
  return parseOutline(text);
}

/** 缩进大纲 → PlanSpec[]。缩进按 2 空格一级（Tab 视作 2 空格）。 */
export function parseOutline(text: string): PlanSpec[] {
  const roots: PlanSpec[] = [];
  /** 栈：depth → 该深度上最近一个节点，用于挂子节点。 */
  const stack: Array<{ spec: PlanSpec; depth: number }> = [];

  for (const rawLine of text.split(/\r?\n/)) {
    if (rawLine.trim() === '') continue;
    const depth = Math.floor(countIndent(rawLine) / 2);
    const body = rawLine.trim().replace(/^(?:[-*+•]|\d+[.)])\s+/, '');
    if (body === '') continue;

    const [titlePart, acceptancePart] = splitAcceptance(body);
    const title = titlePart.trim();
    if (title === '') continue;

    const spec: PlanSpec = { title };
    if (acceptancePart) spec.acceptance = acceptancePart;

    while (stack.length > 0 && stack[stack.length - 1]!.depth >= depth) stack.pop();
    const parent = stack[stack.length - 1];
    if (parent) {
      parent.spec.children ??= [];
      parent.spec.children.push(spec);
    } else {
      roots.push(spec);
    }
    stack.push({ spec, depth });
  }

  return roots;
}

/** 把 JSON 结构收敛成合法 PlanSpec[]：丢无标题节点、限制递归深度、剥掉多余字段。 */
export function normalizePlanSpecs(raw: unknown, depth = 0): PlanSpec[] {
  if (!Array.isArray(raw) || depth > 8) return [];
  const specs: PlanSpec[] = [];
  for (const item of raw) {
    if (typeof item !== 'object' || item === null) continue;
    const record = item as Record<string, unknown>;
    const title = typeof record.title === 'string' ? record.title.trim() : '';
    if (title === '') continue;

    const spec: PlanSpec = { title };
    if (typeof record.detail === 'string' && record.detail.trim()) spec.detail = record.detail.trim();
    if (typeof record.acceptance === 'string' && record.acceptance.trim()) {
      spec.acceptance = record.acceptance.trim();
    }
    if (record.kind === 'concept' || record.kind === 'task') spec.kind = record.kind;
    const children = normalizePlanSpecs(record.children, depth + 1);
    if (children.length > 0) spec.children = children;
    specs.push(spec);
  }
  return specs;
}

function countIndent(line: string): number {
  let width = 0;
  for (const char of line) {
    if (char === ' ') width += 1;
    else if (char === '\t') width += 2;
    else break;
  }
  return width;
}

function splitAcceptance(body: string): [string, string | undefined] {
  const index = body.indexOf('::');
  if (index < 0) return [body, undefined];
  const acceptance = body.slice(index + 2).trim();
  return [body.slice(0, index), acceptance === '' ? undefined : acceptance];
}
