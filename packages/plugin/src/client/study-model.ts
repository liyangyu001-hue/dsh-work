/**
 * 「学习页」的数据整理（纯逻辑，不碰 DOM，方便单测）。
 *
 * 为什么要单独一页：用户 m01594 的原话是「学习界面就是在基础 DSH 面板的按钮上
 * 有个切换选项，切换到学习」——原生节点图的右栏是「人站在图里」时用的，
 * 而网页这一侧要的是「坐下来读」：左边一串要学的东西，右边给这一颗的
 * 基础知识与详解流程。
 *
 * 这里只做两件事：把树摊平成可读的一列（studyRows），
 * 以及取出某一颗该讲的内容（studyTips）。
 */

export type StudyStatus = "todo" | "active" | "done" | "skipped";

export interface StudyNodeLike {
  readonly id: string;
  readonly title: string;
  readonly status: StudyStatus;
  readonly kind?: "task" | "concept";
  readonly children?: readonly string[];
  readonly detail?: string;
  readonly acceptance?: string;
}

export interface StudyDocLike {
  readonly roots: readonly string[];
  readonly nodes: Readonly<Record<string, StudyNodeLike>>;
  /** 当前任务。学习页默认落到它上面 —— 打开就有内容。 */
  readonly pointer?: string | null;
}

export interface StudyRow {
  readonly id: string;
  readonly title: string;
  readonly status: StudyStatus;
  readonly kind: StudyNodeLike["kind"];
  /** 0 = 根节点；子节点 +1。列里靠它缩进。 */
  readonly depth: number;
  readonly childCount: number;
}

export interface StudyTips {
  readonly title: string;
  /** 这一颗的基础知识：优先 detail（写给人看的解释），没有就退回验收标准。 */
  readonly body: string;
  /** 详解流程：这一颗底下拆出来的步骤，按顺序。 */
  readonly steps: readonly string[];
  /** 一句状态话，让人知道自己看到的是哪一步。 */
  readonly state: string;
}

/** 状态 → 人话。和原生那边 `panel.h` 的口径保持一致。 */
export function statusLabel(status: StudyStatus): string {
  switch (status) {
    case "done":
      return "已完成";
    case "active":
      return "正在做";
    case "skipped":
      return "已跳过";
    default:
      return "待办";
  }
}

/**
 * 深度优先摊平整棵树。
 *
 * 为什么不用 `Object.values(nodes)`：那个顺序是 JSON 的键序，和「先看第一步、
 * 再看第二步」没关系；学习页必须按树的顺序读。缺节点/成环都要能活着返回。
 */
export function studyRows(doc: StudyDocLike | undefined): StudyRow[] {
  if (!doc) return [];
  const out: StudyRow[] = [];
  const seen = new Set<string>();

  const walk = (id: string, depth: number): void => {
    if (seen.has(id)) return; // 脏数据（成环或重复挂）时不死循环
    const node = doc.nodes[id];
    if (!node) return;
    seen.add(id);
    out.push({
      id: node.id,
      title: node.title,
      status: node.status,
      kind: node.kind,
      depth,
      childCount: node.children?.length ?? 0,
    });
    for (const child of node.children ?? []) walk(child, depth + 1);
  };

  for (const root of doc.roots) walk(root, 0);
  return out;
}

/** 某一颗该讲什么。节点不存在时返回 null，由调用方决定显示什么。 */
export function studyTips(doc: StudyDocLike | undefined, id: string | null): StudyTips | null {
  if (!doc || id === null) return null;
  const node = doc.nodes[id];
  if (!node) return null;
  const detail = (node.detail ?? "").trim();
  const acceptance = (node.acceptance ?? "").trim();
  const steps = (node.children ?? [])
    .map((childId) => doc.nodes[childId]?.title)
    .filter((title): title is string => typeof title === "string" && title.trim() !== "");
  return {
    title: node.title,
    body: detail !== "" ? detail : acceptance !== "" ? acceptance : "这颗还没写解释：先在面板里拆一次，或者点「讲讲它」让 AI 补一段。",
    steps,
    state: statusLabel(node.status),
  };
}

/** 页头那行进度：已完成 / 不算跳过的总数。和顶部小条同一口径。 */
export function studyProgress(rows: readonly StudyRow[]): { done: number; total: number } {
  const counted = rows.filter((row) => row.status !== "skipped");
  return { done: counted.filter((row) => row.status === "done").length, total: counted.length };
}

/**
 * 左列按标题过滤。
 *
 * 只按标题匹配（不搜正文）：这一列是目录，用户按名字找；搜正文会把「哪颗提到过 X」
 * 和「哪颗叫 X」混在一起，反而更难选。空查询原样返回，调用方不用特判。
 */
export function filterStudyRows(rows: readonly StudyRow[], query: string): readonly StudyRow[] {
  const needle = query.trim().toLowerCase();
  if (needle === "") return rows;
  return rows.filter((row) => row.title.toLowerCase().includes(needle));
}