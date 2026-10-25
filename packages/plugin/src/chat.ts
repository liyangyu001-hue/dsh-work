// 节点上的「追问」：讲完一段之后，人还能接着问。
//
// 与 learning.ts 的分工：
//   learn —— 没问过任何东西时，先给一版「这是什么 + 一个小练习」；
//   chat  —— 人已经带着具体疑问来了，答这一问，不重新讲一遍。
// 返回形状也不同：learn 是 JSON（宿主还要拆成卡片字段），chat 就是一段话。
//
// 为什么不把这些对话也写回 work.json：问一句就落一次盘，任务库会被问答灌满，
// 而且同一个问题问两遍就该有两条记录 —— 那是聊天记录，不是任务状态。

import { collectText } from './recognize.ts';
import type { AdviceInputs, NodeBrief } from './advise.ts';

export const DEFAULT_CHAT_TIMEOUT_MS = 45_000;

/** 一句话最多这么长；再长就不是「追问」而是让它重写整篇了。 */
export const MAX_QUESTION_CHARS = 400;

/** 带上前几轮，模型才接得住「那第二步呢」这种省略主语的问法；太多又会被旧话题拖走。 */
export const MAX_HISTORY_TURNS = 6;

/** 单条回答最多回这么多字；超了截断并加省略号（真长文让用户去面板里看）。 */
export const MAX_ANSWER_CHARS = 600;

export interface ChatTurn {
  readonly question: string;
  readonly answer: string;
}

/** 比 NodeBrief 多一样：上次给他讲过的要点，接着问时不能当它没看过。 */
export interface ChatNodeBrief extends NodeBrief {
  readonly advice?: string;
}

export const CHAT_PROMPT = [
  '你在替一个正在干活的人解释他手上这一步，回答他刚问的那一句。',
  '要求：',
  '1. 第一句直接回答问题，不要复述问题、不要「好的」「当然可以」这类开头；',
  '2. 用他自己的话和这个任务的上下文，别讲通用大道理；不确定就说不确定；',
  '3. 三百字以内，纯文本，不用 markdown、不用列表符号、不要代码块；',
  '4. 如果他问的东西在这颗节点里根本没有，就直说这里没有、并说清该去哪儿看。',
].join('\n');

/** 把用户输入压成一行：换行折成空格，多余空白折叠，超长截断（并留一个可见的省略号）。 */
export function clipText(text: string, limit: number): string {
  const flat = text.replace(/\s+/gu, ' ').trim();
  if (flat.length <= limit) return flat;
  return `${flat.slice(0, Math.max(0, limit - 1)).trimEnd()}…`;
}

/**
 * 拼这一问的 prompt。
 *
 * 节点的四样东西都会带上：标题（是什么）、他自己写的目标（为什么做）、验收标准（做到什么算完）、
 * 已有的讲解（上次讲到哪儿）。历史只留最近几轮，且每轮都压成一行 —— 上下文越长，模型越容易跑题。
 */
export function buildChatPrompt(node: ChatNodeBrief, question: string, history: readonly ChatTurn[] = []): string {
  const lines: string[] = [CHAT_PROMPT, ''];
  if (node.path !== undefined && node.path.length > 0) lines.push(`所在分支：${node.path.join(' → ')}`);
  lines.push(`这一步：${node.title}`);
  if (node.detail !== '') lines.push(`他自己写的目标：${clipText(node.detail, 200)}`);
  if (node.acceptance !== '') lines.push(`做到什么算完：${clipText(node.acceptance, 200)}`);
  if (node.advice !== undefined && node.advice !== '') lines.push(`已经给他讲过的要点：${clipText(node.advice, 200)}`);

  const recent = history.slice(-MAX_HISTORY_TURNS);
  if (recent.length > 0) {
    lines.push('', '刚才的问答（从旧到新）：');
    for (const turn of recent) {
      lines.push(`他问：${clipText(turn.question, 160)}`);
      lines.push(`你答：${clipText(turn.answer, 240)}`);
    }
  }

  lines.push('', `他现在问：${clipText(question, MAX_QUESTION_CHARS)}`);
  return lines.join('\n');
}

/**
 * 模型回来的这一段，直接给人看之前先收拾干净：
 * 去掉三反引号围栏、去掉包裹整段的引号、把多余空行折成单行，最后按上限截断。
 */
export function tidyAnswer(raw: string): string {
  let text = raw.trim();
  if (text.startsWith('```')) {
    text = text.replace(/^```[a-zA-Z]*\n?/u, '').replace(/```$/u, '').trim();
  }
  if (text.length >= 2 && (text.startsWith('"') && text.endsWith('"'))) {
    text = text.slice(1, -1).trim();
  }
  text = text.replace(/\n{2,}/gu, '\n').trim();
  if (text.length <= MAX_ANSWER_CHARS) return text;
  return `${text.slice(0, MAX_ANSWER_CHARS - 1).trimEnd()}…`;
}

/**
 * 问一颗节点一句话。
 *
 * 不落库：问答只活在调用方（网页那一栏 / 未来的原生卡片）里。
 * 模型给的空白答案按失败处理 —— 宁可回一句可读的错误，也不要往界面上贴空白气泡。
 */
export async function askNode(
  inputs: AdviceInputs,
  node: ChatNodeBrief,
  question: string,
  history: readonly ChatTurn[] = [],
  signal?: AbortSignal,
): Promise<string> {
  const asked = clipText(question, MAX_QUESTION_CHARS);
  if (asked === '') throw new Error('没有要问的内容');

  const raw = await collectText(
    {
      llm: inputs.llm,
      provider: inputs.provider,
      model: inputs.model,
      timeoutMs: inputs.timeoutMs ?? DEFAULT_CHAT_TIMEOUT_MS,
    },
    [{ role: 'user', content: [{ type: 'text', text: buildChatPrompt(node, asked, history) }] }],
    signal,
  );

  const answer = tidyAnswer(raw);
  if (answer === '') throw new Error('模型这次没有给出内容，换个问法再试');
  return answer;
}
