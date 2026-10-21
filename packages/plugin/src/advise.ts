// 节点建议：给"当前这件事"配一句可执行的下一步。
//
// 与识别（recognize.ts）的区别：那边看的是截图、产出是一棵树；这边看的是
// 已经在文档里的节点、产出一句话。共用的只是"调模型 + 把流拼成字符串"。

import { collectText, type LlmLike } from './recognize.ts';

export interface AdviceInputs {
  llm: LlmLike;
  provider: string;
  model: string;
  /** 单次调用上限。建议很短，默认给 30 秒就够。 */
  timeoutMs?: number;
}

/** 节点里真正需要模型看的东西。路径是它在树里的位置，帮模型理解上下文。 */
export interface NodeBrief {
  title: string;
  detail: string;
  acceptance: string;
  path?: string[];
}

export interface Advice {
  text: string;
  alt?: string;
}

export const DEFAULT_ADVISE_TIMEOUT_MS = 30_000;

export const ADVISE_PROMPT = [
  '你在帮一个人推进他手头的一件事。他不是程序员，只需要"下一步该做什么"。',
  '基于给定节点的标题、目标、验收标准，给出：',
  '1. text：一句可立即执行的动作建议，不超过 40 个字，动词开头，不要泛泛而谈；',
  '2. alt：一条更省事或更稳妥的替代做法，不超过 40 个字；如果确实没有更好的办法就省略这个字段。',
  '只输出 JSON：{"text":"...","alt":"..."}。不要解释、不要 markdown、不要多余字段。',
].join('\n');

/**
 * 要一条建议。
 *
 * 模型偶尔会包一层解释文字或漏引号，所以这里容忍"JSON 前后有杂字"：
 * 先直接解析，失败就截取第一个 { 到最后一个 } 再试。仍然失败就抛错，
 * 由调用方给出可读原因——不返回半成品建议。
 */
export async function adviseNode(
  inputs: AdviceInputs,
  node: NodeBrief,
  signal?: AbortSignal,
): Promise<Advice> {
  const ask = [
    ADVISE_PROMPT,
    '',
    node.path === undefined || node.path.length === 0 ? '' : `所在分支：${node.path.join(' → ')}`,
    `标题：${node.title}`,
    node.detail === '' ? '' : `目标：${node.detail}`,
    node.acceptance === '' ? '' : `验收：${node.acceptance}`,
  ]
    .filter((line) => line !== '')
    .join('\n');

  const raw = await collectText(
    {
      llm: inputs.llm,
      provider: inputs.provider,
      model: inputs.model,
      ...(inputs.timeoutMs === undefined ? {} : { timeoutMs: inputs.timeoutMs }),
    },
    [{ role: 'user', content: [{ type: 'text', text: ask }] }],
    signal,
  );

  const parsed = parseAdvice(raw);
  if (parsed === null) {
    throw new Error(`模型没有给出可用的建议；原文片段：${excerpt(raw)}`);
  }
  return parsed;
}

/** 从模型输出里挖出 {text, alt}。挖不到返回 null。 */
export function parseAdvice(raw: string): Advice | null {
  const text = typeof raw === 'string' ? raw.trim() : '';
  if (text === '') return null;

  const candidates = [text];
  const open = text.indexOf('{');
  const close = text.lastIndexOf('}');
  if (open >= 0 && close > open) candidates.push(text.slice(open, close + 1));

  for (const candidate of candidates) {
    let value: unknown;
    try {
      value = JSON.parse(candidate);
    } catch {
      continue;
    }
    if (typeof value !== 'object' || value === null) continue;
    const record = value as { text?: unknown; alt?: unknown };
    if (typeof record.text !== 'string') continue;
    const advice = record.text.trim();
    if (advice === '') continue;
    const alt = typeof record.alt === 'string' ? record.alt.trim() : '';
    return alt === '' ? { text: advice } : { text: advice, alt };
  }
  return null;
}

function excerpt(raw: string): string {
  const clean = raw.replace(/\s+/gu, ' ').trim();
  return clean.length <= 120 ? clean : `${clean.slice(0, 120)}…`;
}