// 学习模式：给"一个要懂的概念"配一段讲解 + 一个能立刻做的小练习。
//
// 与 advise.ts 的分工：
//   advise —— 这一步怎么做更好（行动建议，动词开头）
//   learn  —— 这东西到底是什么意思（解释 + 练习）
// 两者返回形状一样（{text, alt}），但 prompt 完全不同，别混用。

import { collectText } from './recognize.ts';
import { parseAdvice, type Advice, type AdviceInputs, type NodeBrief } from './advise.ts';

export const DEFAULT_LEARN_TIMEOUT_MS = 45_000;

export const LEARN_PROMPT = [
  '你在给一个正在干活的人做即时讲解，不是在写教科书。',
  '它可能是个抽象概念（不是一件要做的事），目标是让他三十秒内"懂了并能用"。',
  '输出两个字段：',
  '1. text：三句话以内说清它是什么、为什么在这里重要；不超过 80 个字；不要复述标题；',
  '2. alt：一个能立刻动手试的小练习，或一句能验证自己是否真的懂了的追问；不超过 40 个字。',
  '只输出 JSON：{"text":"...","alt":"..."}。不要解释、不要 markdown、不要多余字段。',
].join('\n');

/**
 * 讲一个概念。
 *
 * 和 adviseNode 一样容忍"JSON 前后有杂字"，但仍然不返回半成品：
 * 解析不出来就抛错，由调用方给出可读原因。
 */
export async function learnNode(
  inputs: AdviceInputs,
  node: NodeBrief,
  signal?: AbortSignal,
): Promise<Advice> {
  const ask = [
    LEARN_PROMPT,
    '',
    node.path === undefined || node.path.length === 0 ? '' : `所在分支：${node.path.join(' → ')}`,
    `标题：${node.title}`,
    node.detail === '' ? '' : `他自己写的目标：${node.detail}`,
  ]
    .filter((line) => line !== '')
    .join('\n');

  const raw = await collectText(
    {
      llm: inputs.llm,
      provider: inputs.provider,
      model: inputs.model,
      timeoutMs: inputs.timeoutMs ?? DEFAULT_LEARN_TIMEOUT_MS,
    },
    [{ role: 'user', content: [{ type: 'text', text: ask }] }],
    signal,
  );

  const parsed = parseAdvice(raw);
  if (parsed === null) {
    throw new Error(`模型没有给出可用的讲解；原文片段：${raw.replace(/\s+/gu, ' ').slice(0, 120)}`);
  }
  return parsed;
}
