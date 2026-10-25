/**
 * 框选截图 → 任务树：一次模型调用，把「屏幕上正在看的东西」变成可执行计划。
 *
 * 为什么单独一个文件、而且**不依赖宿主**：
 *   - 解析（模型输出 → PlanSpec[]）是纯函数，能直接单测；
 *   - 调用只认两个最小接口（流式模型、图片入库），宿主真身由 index.ts 注入。
 *   宿主升级改了 `ctx.llm` 的形状，受影响的只有这一个文件。
 *
 * 刻意不做的事：
 *   - 不新起会话、不注册工具：识别是一次性的内部调用，产出只落状态文档；
 *   - 不在失败时写任何东西：文档只在「模型给出可解析的任务树」之后才被提交。
 */

import type { Intent, PlanSpec, WorkNode } from '@dshwork/core';
import { materializePlan } from '@dshwork/core';

import { normalizePlanSpecs, parsePlanInput } from './plan-text.ts';
import { readUsage, usageMeter } from './usage.ts';

/** 要求模型只输出这个形状；字段与 PlanSpec 同名，解析后可直接materialize。 */
export const RECOGNIZE_PROMPT = [
  '看这张截图，它是用户此刻屏幕上的内容。把用户正在处理的工作拆成任务树。',
  '',
  '只输出一个 JSON 对象，不要 Markdown 代码块，不要解释：',
  '{"title":"整件事的标题","nodes":[{"title":"一步","detail":"这一步具体做什么","acceptance":"什么算做完了","children":[]}]}',
  '',
  '规则：',
  '1. 3~8 个节点，按真实推进顺序排列；有阶段感时用 children 挂子任务。',
  '2. 每个节点都必须写 detail 与 acceptance，且写成可执行、可验收的一句话，不要写「了解/研究/优化」这类空话。',
  '3. 只依据截图里看得到的内容推断；看不清就写得保守一些，不要编造系统里不存在的业务。',
  '4. 如果这张图是「要理解的东西」（概念、资料、报错原理）而不是「要做的事」，该节点加 "kind":"concept"；普通任务不用写 kind。',
].join('\n');

/** 第一次输出不可解析时的「就地修格式」追加指令：同一张图再问一次，不重新理解。 */
export const REPAIR_PROMPT = [
  '你上一次的回答不是合法的任务 JSON。',
  '请只输出那一个 JSON 对象（title + nodes），不要任何解释文字，不要代码块标记。',
].join('\n');

/** 单次模型调用的默认上限；视觉模型首包较慢，给足时间但必须有底线。 */
export const DEFAULT_TIMEOUT_MS = 90_000;

export interface LlmChunk {
  readonly type?: string;
  readonly text?: string;
  readonly reason?: unknown;
  /**
   * 用量分片（`type === 'usage'`）。实测形状见 `usage.ts` 顶部的注释，
   * 字段全可选——不同 provider 给的不一样，读的时候逐个数。
   */
  readonly usage?: unknown;
}

/** 只声明我们真正用到的宿主能力，多了就是会过期的假设。 */
export interface LlmLike {
  stream(options: Record<string, unknown>): AsyncIterable<LlmChunk>;
}

export interface AttachmentsLike {
  /** 图片入库并换成一串可放进消息的 content blocks。 */
  admitPromptContent(parts: unknown[]): Promise<unknown[]>;
}

export interface RecognizeInputs {
  llm: LlmLike;
  attachments: AttachmentsLike;
  provider: string;
  model: string;
  /** 单次调用上限。 */
  timeoutMs?: number;
}

/** collectText 只需要模型三件套，不需要附件——建议链路不传图。 */
export type LlmInputs = Pick<RecognizeInputs, 'llm' | 'provider' | 'model' | 'timeoutMs'>;

/**
 * 二次解构：把草稿里的一颗节点再拆细。
 *
 * 刻意只处理「补全」而不是「重做」：补全只往子层加节点，已经看过、满意的兄弟节点
 * 一个都不动；重做会打乱顺序，是另一件事。
 */
export const EXPAND_PROMPT = [
  '下面这张任务树里的某一个节点，现在要把它拆成可以直接动手的几个小步骤。',
  '',
  '只输出 JSON：{"title":"…","nodes":[{"title":"…","detail":"…","acceptance":"…"}]}',
  '要求：',
  '1. 2~5 个步骤，按先后顺序排；',
  '2. 每一步都要能在一次坐下来里做完，标题不超过 18 个字；',
  '3. acceptance 写“怎么算做完了”，必须能被外人检查；',
  '4. 不要重复父节点的标题，直接写动作；',
  '5. 不要输出 JSON 以外的任何东西。',
].join('\n');

export async function expandNode(
  inputs: LlmInputs,
  node: { title: string; detail: string; acceptance: string },
  signal?: AbortSignal,
): Promise<Recognized> {
  const ask = [
    EXPAND_PROMPT,
    '',
    `父节点：${node.title}`,
    node.detail === '' ? '' : `目标：${node.detail}`,
    node.acceptance === '' ? '' : `原来的验收：${node.acceptance}`,
  ]
    .filter((line) => line !== '')
    .join('\n');

  const raw = await collectText(inputs, [{ role: 'user', content: [{ type: 'text', text: ask }] }], signal);
  const first = parseRecognizedPlan(raw);
  if (first.specs.length > 0) return { ...first, raw, attempts: 1 };

  const repaired = await collectText(
    inputs,
    [
      {
        role: 'user',
        content: [
          { type: 'text', text: ask },
          { type: 'text', text: REPAIR_PROMPT },
        ],
      },
    ],
    signal,
  ).catch(() => '');

  const second = parseRecognizedPlan(repaired === '' ? raw : repaired);
  return { ...second, raw: repaired === '' ? raw : repaired, attempts: 2 };
}

export interface RecognizedPlan {
  title: string;
  specs: PlanSpec[];
}

export interface Recognized extends RecognizedPlan {
  /** 模型最终返回的原文，失败排查与日志用。 */
  raw: string;
  /** 实际调用了几次（1 = 一次过；2 = 修过一次格式）。 */
  attempts: number;
}

/**
 * 让模型看图并产出计划。
 *
 * 为什么解析失败还要再问一次：视觉模型偶尔会把 JSON 包在解释性文字里，
 * 或者少一个引号 —— 这是「格式」问题而不是「理解」问题，追加一句约束重问一次
 * 的成功率很高；仍然失败就如实返回空结果，由调用方给出可读原因，绝不写文档。
 */
export async function recognizePlan(
  inputs: RecognizeInputs,
  pngBase64: string,
  options?: { signal?: AbortSignal; repair?: boolean },
): Promise<Recognized> {
  const content = await admitImage(inputs.attachments, pngBase64);

  const ask = async (extra?: string): Promise<string> => {
    const parts = extra === undefined ? content : [...content, { type: 'text', text: extra }];
    return collectText(inputs, [{ role: 'user', content: parts }], options?.signal);
  };

  const raw = await ask();
  const first = parseRecognizedPlan(raw);
  if (first.specs.length > 0 || options?.repair === false) {
    return { ...first, raw, attempts: 1 };
  }

  // 第二次调用失败**不要**盖掉第一次的真实错误：用第一次原文去解析，让它自己失败。
  const repaired = await ask(REPAIR_PROMPT).catch(() => '');
  const second = parseRecognizedPlan(repaired === '' ? raw : repaired);
  return {
    ...second,
    raw: repaired === '' ? raw : repaired,
    attempts: 2,
  };
}

/** 把模型输出解析成任务树。任何形状不对都收敛成空结果，由调用方决定怎么报错。 */
export function parseRecognizedPlan(raw: string): RecognizedPlan {
  const text = typeof raw === 'string' ? raw.trim() : '';
  if (text === '') return { title: '', specs: [] };

  const parsed = extractJson(text);
  if (Array.isArray(parsed)) {
    return { title: '', specs: normalizePlanSpecs(parsed) };
  }
  if (typeof parsed === 'object' && parsed !== null) {
    const record = parsed as Record<string, unknown>;
    const nodes = Array.isArray(record['nodes'])
      ? record['nodes']
      : Array.isArray(record['tasks'])
        ? record['tasks']
        : [];
    const title = typeof record['title'] === 'string' ? record['title'].trim() : '';
    return { title, specs: normalizePlanSpecs(nodes) };
  }

  // 连 JSON 都没有：可能写成缩进大纲（模型偶尔这样），也可能只是一句解释。
  // 只有「确实像大纲」才收 —— 否则一句「抱歉，看不清」也会被当成一个任务。
  if (looksLikeOutline(text)) return { title: '', specs: parsePlanInput(text) };
  return { title: '', specs: [] };
}

/** 计划树 → 一条 plan.set 意图。放在这里是为了让「解析 → 落库」只有一条路径。 */
export function planSetFromSpecs(
  specs: readonly PlanSpec[],
  options: { now: number; newId: () => string; title?: string },
): Extract<Intent, { type: 'plan.set' }> {
  const plan = materializePlan(specs, { now: options.now, newId: options.newId });
  return planSetIntent(plan.roots, plan.nodes, options.title);
}

/**
 * 草稿：已经物化成节点、但**还没有进文档**的一张计划树。
 *
 * 为什么要这一层：解构结果现在要先给人看一眼、允许就地再拆，满意了才落库。
 * 把「草稿」和「文档」分开存，用户中途关掉什么都不留；这条和「识别失败不写任何东西」
 * 是同一条原则的下半句。
 */
export interface DraftPlan {
  title: string;
  roots: readonly string[];
  nodes: readonly WorkNode[];
}

/** 生成草稿：同样的 materialize，只是不提交。 */
export function draftFromSpecs(
  specs: readonly PlanSpec[],
  options: { now: number; newId: () => string; title?: string },
): DraftPlan {
  const plan = materializePlan(specs, { now: options.now, newId: options.newId });
  return { title: options.title ?? '', roots: plan.roots, nodes: plan.nodes };
}

/** 草稿 → plan.set：确认之后才走这一步，落库只有这一条路。 */
export function planSetFromDraft(draft: DraftPlan): Extract<Intent, { type: 'plan.set' }> {
  return planSetIntent([...draft.roots], [...draft.nodes], draft.title);
}

function planSetIntent(
  roots: string[],
  nodes: WorkNode[],
  title: string | undefined,
): Extract<Intent, { type: 'plan.set' }> {
  // 标题只有真的存在时才带上：多余的空字段会让日志更难读。
  return title === undefined || title === ''
    ? { type: 'plan.set', roots, nodes }
    : { type: 'plan.set', roots, nodes, title };
}

/* ─────────────────────────── 内部 ─────────────────────────── */

async function admitImage(attachments: AttachmentsLike, pngBase64: string): Promise<unknown[]> {
  try {
    return await attachments.admitPromptContent([
      { type: 'text', text: RECOGNIZE_PROMPT },
      { type: 'image', data: pngBase64, mediaType: 'image/png', name: 'capture.png' },
    ]);
  } catch (error) {
    throw new Error(`截图入库失败：${describe(error)}`, { cause: error });
  }
}

/** 把流式分片拼成一段文本；流里的失败会变成可读错误，而不是静默的空字符串。 */
export async function collectText(
  inputs: LlmInputs,
  messages: unknown[],
  outer: AbortSignal | undefined,
): Promise<string> {
  const timeoutMs = inputs.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  const signal = outer === undefined ? controller.signal : AbortSignal.any([outer, controller.signal]);

  let text = '';
  usageMeter.callStarted();
  try {
    const stream = inputs.llm.stream({
      provider: inputs.provider,
      model: inputs.model,
      messages,
      signal,
    });
    for await (const chunk of stream) {
      if (chunk?.type === 'text-delta' && typeof chunk.text === 'string') {
        text += chunk.text;
        continue;
      }
      const usage = readUsage(chunk);
      if (usage !== null) {
        usageMeter.addUsage(usage);
        continue;
      }
      const failure = failureOf(chunk?.reason);
      if (failure !== null) throw new Error(failure);
    }
    return text;
  } catch (error) {
    usageMeter.callFailed();
    if (controller.signal.aborted && outer?.aborted !== true) {
      throw new Error(`识别超时（${Math.round(timeoutMs / 1000)} 秒）`, { cause: error });
    }
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

/** 终止分片里的错误原因 → 一句中文。非错误原因（正常结束、被取消）返回 null。 */
function failureOf(reason: unknown): string | null {
  if (typeof reason !== 'object' || reason === null) return null;
  const record = reason as { kind?: unknown; failure?: unknown };
  if (record.kind !== 'error') return null;
  const failure = (record.failure ?? {}) as { code?: unknown; message?: unknown };
  const code = typeof failure.code === 'string' && failure.code !== '' ? failure.code : '模型调用失败';
  const message = typeof failure.message === 'string' && failure.message !== '' ? `：${failure.message}` : '';
  return `${code}${message}`;
}

/** 从可能带代码块 / 前后废话的文本里抠出第一个 JSON 值。 */
function extractJson(text: string): unknown {
  const stripped = text
    .replace(/^\s*```(?:json)?\s*/i, '')
    .replace(/\s*```\s*$/, '')
    .trim();

  const object = sliceBetween(stripped, '{', '}');
  if (object !== null) {
    try {
      return JSON.parse(object);
    } catch {
      // 落到数组分支再试
    }
  }
  const array = sliceBetween(stripped, '[', ']');
  if (array !== null) {
    try {
      return JSON.parse(array);
    } catch {
      return undefined;
    }
  }
  return undefined;
}

function sliceBetween(text: string, open: string, close: string): string | null {
  const start = text.indexOf(open);
  const end = text.lastIndexOf(close);
  if (start < 0 || end <= start) return null;
  return text.slice(start, end + 1);
}

/**
 * 像不像一份缩进大纲：至少一行带列表前缀，且带前缀的行数过半数。
 * 阈值取一半是因为大纲里常夹着说明行，而一段解释性散文最多只有一行是列表。
 */
function looksLikeOutline(text: string): boolean {
  const lines = text.split(/\r?\n/).filter((line) => line.trim() !== '');
  if (lines.length === 0) return false;
  const bullets = lines.filter((line) => /^\s*(?:[-*+•]|\d+[.)])\s+\S/.test(line)).length;
  return bullets > 0 && bullets * 2 >= lines.length;
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
