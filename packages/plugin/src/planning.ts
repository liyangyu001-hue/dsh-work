/**
 * 一句人话 → 任务树：不想截图的人可以直接说「我要做 X」，这里负责把它变成计划。
 *
 * 和 recognize.ts 的关系：**共用同一条解析与落库路径**（parseRecognizedPlan → plan.set），
 * 区别只在输入——那边给一张图，这边给一段文字。共用是为了让「模型胡说」的容错只写一遍。
 *
 * 刻意不做的事：不新起会话、不注册工具、失败不写文档。识别/规划都是一次性内部调用。
 */

import type { PlanSpec } from '@dshwork/core';

import {
  DEFAULT_TIMEOUT_MS,
  collectText,
  parseRecognizedPlan,
  type LlmInputs,
} from './recognize.ts';

/** 要求模型只输出这个形状；与 recognize 的 schema 完全一致，解析器才能复用。 */
export const PLAN_TEXT_PROMPT = [
  '用户想把一件事交给 AI 安排。把他说的这件事拆成可执行的任务树。',
  '',
  '只输出一个 JSON 对象，不要 Markdown 代码块，不要解释：',
  '{"title":"整件事的标题","nodes":[{"title":"一步","detail":"这一步具体做什么","acceptance":"什么算做完了","children":[]}]}',
  '',
  '规则：',
  '1. 3~8 个节点，按真实推进顺序排列；有阶段感时用 children 挂子任务。',
  '2. 每个节点都必须写 detail 与 acceptance，写成可执行、可验收的一句话；不要写「了解/研究/优化」这类空话。',
  '3. 用户说得含糊的地方，按最合理的一种做法补全，但**不要编造他的业务背景**。',
  '4. 如果用户想做的是「理解某件事」（概念、原理、报错原因）而不是「做成一件事」，该节点加 "kind":"concept"。',
  '5. 第一件事要小到「十分钟内能开始」。',
].join('\n');

/** 第一次输出不可解析时，追加一句只要 JSON —— 这是格式问题，不是理解问题。 */
const REPAIR_PROMPT = [
  '你上一次的回答不是合法的任务 JSON。',
  '请只输出那一个 JSON 对象（title + nodes），不要任何解释文字，不要代码块标记。',
].join('\n');

export interface PlannedFromText {
  title: string;
  specs: PlanSpec[];
  /** 模型最后返回的原文，排查用。 */
  raw: string;
  /** 实际调用次数：1 = 一次过；2 = 修过一次格式。 */
  attempts: number;
}

/**
 * 让模型把一段人话拆成任务树。
 *
 * 解析失败会就地修一次格式；仍然失败就返回空 specs（调用方据此报可读原因，**不落库**）。
 */
export async function planFromText(
  inputs: LlmInputs,
  request: string,
  signal?: AbortSignal,
): Promise<PlannedFromText> {
  const ask = async (extra?: string): Promise<string> => {
    const prompt = extra === undefined ? `${PLAN_TEXT_PROMPT}\n\n用户说：${request}` : extra;
    return collectText(inputs, [{ role: 'user', content: [{ type: 'text', text: prompt }] }], signal);
  };

  const raw = await ask();
  const first = parseRecognizedPlan(raw);
  if (first.specs.length > 0) return { ...first, raw, attempts: 1 };

  const repaired = await ask(`${REPAIR_PROMPT}\n\n用户说的是：${request}`).catch(() => '');
  const second = parseRecognizedPlan(repaired === '' ? raw : repaired);
  return { ...second, raw: repaired === '' ? raw : repaired, attempts: 2 };
}

export const PLAN_TEXT_DEFAULT_TIMEOUT_MS = DEFAULT_TIMEOUT_MS;
