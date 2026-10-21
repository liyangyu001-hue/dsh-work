/**
 * 交给模型的工具。
 *
 * 设计边界（改之前先读 docs/ARCHITECTURE.md 的「权限模型」）：
 * - 模型**可以**：建树、写建议、交付交接任务。
 * - 模型**不可以**：替人判定「完成」。完成与回退永远由人按键触发（走 http.ts 的 intent 通道）。
 *
 * 宿主契约（已按真机校准，改的时候别踩）：
 * - `parameters` 不是完整 JSON Schema，而是「属性名 → DSL 节点」；`required` 只能写在属性节点里，
 *   不能出现在 `items` 内部（宿主会报 `... is not supported by the value schema DSL`）。
 * - `output` 必须给 `{ schema, render }`；`execute` 的返回值会先按 `output.schema` 校验，
 *   所以这里统一返回字符串。
 * - 注册返回 disposer，必须由 `ctx.effect` 收口。
 */

import { defineTool } from '@deepseek-ai/dsh-tools';

import type { Actor, ApplyResult, Intent, WorkDoc } from '@dshwork/core';
import { linearOrder, materializePlan, progress } from '@dshwork/core';

import { formatStatus, formatTree, offeredNodes, titleOf } from './format.ts';
import { parsePlanInput } from './plan-text.ts';

/** 工具需要的最小提交面：由 index.ts 注入，单测里可以直接造假。 */
export interface ToolHost {
  snapshot(): Promise<WorkDoc>;
  commit(intent: Intent, by: Actor): Promise<ApplyResult>;
  now(): number;
  newId(): string;
}

/** 所有工具共用同一个输出契约：一段给人/模型读的文本。 */
const TEXT_OUTPUT = {
  schema: { type: 'string' as const },
  render: (_args: unknown, value: unknown) => [
    { type: 'text' as const, text: typeof value === 'string' ? value : JSON.stringify(value) },
  ],
};

export function registerTools(ctx: any, host: ToolHost): Array<() => void> {
  const disposers: Array<() => void> = [];
  const register = (spec: Record<string, unknown>): void => {
    disposers.push(ctx.tools.register(defineTool(spec as never)));
  };

  register({
    name: 'work_status',
    description:
      '读取当前工作区的任务进度与节点树。想确认「到哪一步了」「这一步要做什么」「上一步的结论是什么」都先调它。只读。',
    parameters: {},
    output: TEXT_OUTPUT,
    isConcurrencySafe: () => true,
    timeoutMs: 5_000,
    async execute() {
      return formatStatus(await host.snapshot());
    },
  });

  register({
    name: 'work_plan',
    description:
      '把一项工作拆成任务树落到看板上。这是**覆盖式**提交：替换整棵树并把指针移到第一个待办节点，所以先看 work_status 再决定是否重排。父节点是阶段、子节点是可独立完成的一步。\n' +
      'plan 有两种写法，任选其一：\n' +
      '① 缩进大纲（推荐，最快）：每行一个节点，两个空格一级，可带 "- " 前缀；`标题 :: 验收标准` 可附带验收。\n' +
      '② JSON 数组：需要 detail / kind 时用，形状 [{"title":"…","acceptance":"…","children":[…]}]。',
    parameters: {
      plan: {
        type: 'string',
        required: true,
        description:
          '任务树。例：\n- 需求梳理\n  - 明确目标\n  - 列出约束\n- 实现\n  - 写核心逻辑 :: 单元测试通过',
      },
    },
    output: TEXT_OUTPUT,
    timeoutMs: 10_000,
    async execute(args: { plan?: string }) {
      const specs = parsePlanInput(args?.plan);
      if (specs.length === 0) {
        return '没有解析出任何节点（plan 为空或格式不对），未做任何修改。';
      }
      const plan = materializePlan(specs, { now: host.now(), newId: () => host.newId() });
      const result = await host.commit(
        { type: 'plan.set', roots: plan.roots, nodes: plan.nodes },
        'ai',
      );
      if (!result.ok) return `拆解没有落地：${result.reason}`;

      const doc = await host.snapshot();
      return `已建树，共 ${linearOrder(doc).length} 个节点，指针在「${titleOf(doc, doc.pointer)}」。\n${formatStatus(doc)}`;
    },
  });

  register({
    name: 'work_advise',
    description:
      '给当前节点（或指定节点）写一条建议 / 更优解法，会实时出现在用户屏幕上的任务卡里。要短、可执行、具体到下一步动作；有更优路径就填 alt。这不是任务本身，不要写长文。',
    parameters: {
      text: { type: 'string', required: true, description: '建议正文，一到三句' },
      alt: { type: 'string', description: '另一条更优解的思路（可选）' },
      nodeId: { type: 'string', description: '目标节点 id，省略则用当前节点' },
      model: { type: 'string', description: '产出这条建议的模型标识（可选）' },
    },
    output: TEXT_OUTPUT,
    timeoutMs: 5_000,
    async execute(args: { text?: string; alt?: string; nodeId?: string; model?: string }) {
      const doc = await host.snapshot();
      const nodeId = args?.nodeId ?? doc.pointer;
      if (!nodeId) return '当前没有进行中的节点，建议没有落地。';
      if (!doc.nodes[nodeId]) return `找不到节点 ${nodeId}。`;
      const text = typeof args?.text === 'string' ? args.text.trim() : '';
      if (text === '') return '建议正文为空，未做任何修改。';

      const advice: NonNullable<NonNullable<WorkDoc['nodes'][string]>['advice']> = {
        text,
        at: host.now(),
      };
      if (args.alt) advice.alt = args.alt;
      if (args.model) advice.model = args.model;

      const result = await host.commit({ type: 'node.advise', nodeId, advice }, 'ai');
      return result.ok ? `建议已送达「${titleOf(doc, nodeId)}」。` : `建议没有落地：${result.reason}`;
    },
  });

  register({
    name: 'work_handoff_start',
    description:
      '接下一个人交给你的节点。当 work_status 里出现「已交给你」的节点、而你也真的开始做了，就调它；' +
      '这会把交接状态标成 running，人能在屏幕上的小条里看到「AI 正在做」。做完再调 work_handoff_submit。',
    parameters: {
      nodeId: { type: 'string', description: '要接的节点 id，省略则用当前节点' },
      sessionId: { type: 'string', description: '你的会话 id（不知道就省略）' },
    },
    output: TEXT_OUTPUT,
    timeoutMs: 5_000,
    async execute(args: { nodeId?: string; sessionId?: string }) {
      const doc = await host.snapshot();
      const nodeId = args?.nodeId ?? doc.pointer;
      if (!nodeId) return '当前没有进行中的节点。';
      const node = doc.nodes[nodeId];
      if (!node) return `找不到节点 ${nodeId}。`;
      if (node.handoff?.state === 'running') return `「${node.title}」已经在做了。`;
      if (node.handoff?.state !== 'offered') {
        return `「${node.title}」还没有被交给你（当前交接状态：${node.handoff?.state ?? '无'}），先让人在轮盘里点「交给 AI」。`;
      }
      const result = await host.commit(
        { type: 'handoff.start', nodeId, agentSessionId: args?.sessionId ?? 'ai' },
        'ai',
      );
      return result.ok
        ? `已接下「${node.title}」，人的小条上会显示「AI 正在做」。做完用 work_handoff_submit 交回。`
        : `接下没有落地：${result.reason}`;
    },
  });

  register({
    name: 'work_handoff_submit',
    description:
      '当你**真的做完了**被交接的那部分工作时调用：说明你产出了什么、放在哪里、怎么验证。这不会自动结束节点 —— 人看过之后才会点通过。',
    parameters: {
      note: { type: 'string', description: '交付说明：做了什么、产物在哪、怎么验证' },
      nodeId: { type: 'string', description: '目标节点 id，省略则用当前节点' },
    },
    output: TEXT_OUTPUT,
    timeoutMs: 5_000,
    async execute(args: { note?: string; nodeId?: string }) {
      const doc = await host.snapshot();
      const nodeId = args?.nodeId ?? doc.pointer;
      if (!nodeId) return '当前没有进行中的节点。';
      const intent: Intent = args?.note
        ? { type: 'handoff.submit', nodeId, note: args.note }
        : { type: 'handoff.submit', nodeId };
      const result = await host.commit(intent, 'ai');
      return result.ok
        ? `已提交交付说明，等待人确认「${titleOf(doc, nodeId)}」。`
        : `提交没有落地：${result.reason}`;
    },
  });

  return disposers;
}

/* ─────────────────────────── 格式化 ─────────────────────────── */

