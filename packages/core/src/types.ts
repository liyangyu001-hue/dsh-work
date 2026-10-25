/**
 * 核心数据结构。
 *
 * 设计约束：
 * - 全部可 JSON 序列化（要落盘、要过 WS、要能被人手改）。
 * - 不使用 enum（Node 的类型擦除不支持），一律用字面量联合。
 * - 时间戳一律由调用方传入，本包永不读时钟。
 */

export const SCHEMA_VERSION = 1;

export type NodeId = string;
export type WorkId = string;

/** 节点的推进状态。`active` 全树最多一个，由 validateDoc 保证。 */
export type NodeStatus = 'todo' | 'active' | 'done' | 'skipped';

/** `task` = 要做的事；`concept` = 要理解的东西（学习模式入口）。 */
export type NodeKind = 'task' | 'concept';

/** 变更归因：这件事是人的决定、AI 的建议，还是系统巡检。 */
export type Actor = 'human' | 'ai' | 'system';

/**
 * 这一步该谁做。
 *
 * 用户 m04063 第 3 条问的是「哪部分 AI 做、哪部分人做」——
 * 原先只能靠 `handoff` 是否存在来**推测**，那是过程痕迹不是意图：
 * 一颗还没派的节点和一颗明确要人做的节点看起来一模一样。
 * 所以给它一个显式字段，`either` 表示「都行，谁有空谁上」。
 */
export type Executor = 'human' | 'ai' | 'either';

export type HandoffState = 'offered' | 'running' | 'review' | 'accepted' | 'returned';

/** AI 给出的建议 / 更优解。异步写入，不阻塞任务推进。 */
export interface Advice {
  text: string;
  /** 备选方案。有则界面以「或者」并列呈现。 */
  alt?: string;
  at: number;
  model?: string;
  /**
   * 这条建议是按什么问出来的（节点内容 + 模型拼成的指纹）。
   *
   * 有了它，「同一个节点、内容没变」就能直接复用旧建议，不再调一次模型；
   * 缺这个字段的旧数据一律当作过期，只是多问一次，不会给出错的话。
   */
  sig?: string;
}

/** 交给 AI 执行时的状态机。完成判定权始终在人手里。 */
export interface Handoff {
  state: HandoffState;
  /** AI 侧 agent 会话 id，用于把产出接回会话。 */
  agentSessionId?: string;
  since: number;
  note?: string;
}

/** 证据：凭什么说这件事做完了。 */
export interface Evidence {
  /** 指向 captures/ 下的资源，或外部引用。 */
  assetId: string;
  note?: string;
  at: number;
}

/**
 * 这一步的产物：一份落在 `nodes/<id>.md` 的档案。
 *
 * 为什么要有它：复盘时发现真库 7 颗 `done` 的节点、`evidence` 全空——
 * 「做完了」只有状态，没有任何东西能点开看。`path` 相对任务库根目录存，
 * 换机器/换目录都不会失效；文件本身由插件那边读写，core 只记这个指针。
 */
export interface Artifact {
  /** 相对任务库根目录的路径，例如 `.dsh-work/nodes/nd_xxx.md`。 */
  path: string;
  /** 第一次挂上这份产物的时刻。 */
  at: number;
}

export interface WorkNode {
  id: NodeId;
  parentId: NodeId | null;
  /** 子节点顺序，决定深度优先遍历与卡片排列。 */
  children: NodeId[];
  title: string;
  /** 这一步到底要做什么。 */
  detail?: string;
  /** 什么算做完了。AI 拆解时应尽量给出。 */
  acceptance?: string;
  kind: NodeKind;
  status: NodeStatus;
  /** 这一步该谁做；没标 = 还没想清楚（读到 undefined 一律当「未定」，不要猜）。 */
  executor?: Executor | null;
  advice?: Advice | null;
  handoff?: Handoff | null;
  /** 产物指针；`null` = 显式「这一步没有产物」，`undefined` = 还没挂过。 */
  artifact?: Artifact | null;
  evidence: Evidence[];
  createdAt: number;
  updatedAt: number;
}

export interface WorkDoc {
  schema: number;
  id: WorkId;
  title: string;
  createdAt: number;
  updatedAt: number;
  /** 已应用的最后一条日志序号，与 journal 对齐。 */
  seq: number;
  /** 当前推进到的节点。null 表示全部完成或尚未开始。 */
  pointer: NodeId | null;
  roots: NodeId[];
  nodes: Record<NodeId, WorkNode>;
}

/* ─────────────────────────── 意图 ─────────────────────────── */

export interface ApplyContext {
  /** 发生时刻（毫秒）。由调用方提供，保证 apply 纯净。 */
  at: number;
  by: Actor;
}

export type Intent =
  /** 整体设定任务树：首次拆解、或重新拆解。 */
  | { type: 'plan.set'; roots: NodeId[]; nodes: WorkNode[]; title?: string }
  | { type: 'plan.add'; parentId: NodeId | null; node: WorkNode; index?: number }
  | { type: 'plan.remove'; nodeId: NodeId }
  | { type: 'plan.rename'; nodeId: NodeId; title?: string; detail?: string; acceptance?: string }
  /** 当前节点完成并前进（人按「完成」）。 */
  | { type: 'node.advance' }
  /** 回到上一个任务（人按「上一个」）。 */
  | { type: 'node.back' }
  | { type: 'node.complete'; nodeId: NodeId }
  | { type: 'node.skip'; nodeId: NodeId }
  | { type: 'node.focus'; nodeId: NodeId }
  | { type: 'node.advise'; nodeId: NodeId; advice: Advice }
  /** 标这一步该谁做；`null` = 清掉标记，回到「都行 / 还没定」。 */
  | { type: 'node.assign'; nodeId: NodeId; executor: Executor | null }
  /** 挂上/换掉这一步的产物指针；`null` = 显式声明「这一步没有产物」。 */
  | { type: 'node.artifact'; nodeId: NodeId; path: string | null }
  | { type: 'node.evidence'; nodeId: NodeId; evidence: Evidence }
  | { type: 'handoff.offer'; nodeId: NodeId }
  /**
   * 撤回：把「已经交给 AI 但还没人接」的节点收回来，交接清空。
   *
   * 没有这一条时 `offered` 是个死胡同 —— `start` 只有 AI 能发、`return` 又要求
   * `running|review`，人手一抖点了「交给 AI」，这颗节点就永远挂着（真库里挂过三天）。
   * 只允许从 `offered` / `returned` 撤回；`running` 要走打回、`review` 要验收或打回。
   */
  | { type: 'handoff.cancel'; nodeId: NodeId }
  | { type: 'handoff.start'; nodeId: NodeId; agentSessionId: string }
  | { type: 'handoff.submit'; nodeId: NodeId; note?: string }
  | { type: 'handoff.accept'; nodeId: NodeId }
  | { type: 'handoff.return'; nodeId: NodeId; reason?: string };

export type IntentType = Intent['type'];

/* ─────────────────────── 计划（AI 产出） ──────────────────── */

/** AI 拆解出的计划节点：没有 id，由 materializePlan 补。 */
export interface PlanSpec {
  title: string;
  detail?: string;
  acceptance?: string;
  kind?: NodeKind;
  /** 拆解时就能判断归属的，直接标上；判断不了就留空。 */
  executor?: Executor;
  children?: PlanSpec[];
}

export interface MaterializeOptions {
  now: number;
  newId: () => NodeId;
  parentId?: NodeId | null;
}
