/**
 * 顶部玻璃条上那两个入口的纯逻辑：什么时候能点、点完回哪句话。
 *
 * 为什么单独放一个文件：整条玻璃条是 React，跑不进 node --test；
 * 但「没有任务树时节点图按钮该置灰」和「宿主四种回法各说哪句」
 * 是可以被单测按住的文案与分支，抽出来比在 JSX 里堆三元表达式牢靠。
 */

/** 宿主 `POST /dsh-work/api/ui/open` 认的两个目标。 */
export type OpenTarget = "panel" | "graph";

/** 节点图按钮的结论：能不能点，以及悬停时那句解释。 */
export interface GraphEntry {
  enabled: boolean;
  hint: string;
}

/** 宿主回包里我们认识的字段；多出来的忽略。 */
export interface OpenReply {
  /** fetch 本身失败（宿主没在跑 / 网络断），不是宿主拒绝。 */
  offline?: boolean;
  /** HTTP 状态码；拿不到响应时不填。 */
  status?: number;
  ok?: boolean;
  companion?: boolean;
  launched?: boolean;
  hint?: string;
  error?: string;
}

const NOUN: Record<OpenTarget, string> = { panel: "桌面面板", graph: "节点图" };

/**
 * 节点图按钮的状态。
 *
 * 没有任务树时点开只会得到一张空图 —— 那比「按钮是灰的」更让人迷惑，
 * 所以这里直接置灰，并把「先去拆一次任务」写进悬停说明。
 * `nodeCount === null` 是还没拿到第一份状态：同样先别点，但别误说成「没有任务」。
 */
export function graphEntry(nodeCount: number | null): GraphEntry {
  if (nodeCount === null) return { enabled: false, hint: "正在读取任务状态…" };
  if (nodeCount > 0) return { enabled: true, hint: "打开已落库文档的节点图（只读）" };
  return { enabled: false, hint: "还没有任务树：先在面板里拆一次任务，再来打开节点图" };
}

/**
 * 点完之后顶部那句反馈。
 *
 * 每一档都要说清「现在是什么状态」：打开了、正在起、没接上、还是宿主报错。
 * 失败时优先用宿主给的 hint（它知道原因），别拿 HTTP 码去糊人。
 */
export function openFeedback(target: OpenTarget, reply: OpenReply): string {
  const noun = NOUN[target];
  if (reply.offline) return `连不上宿主，${noun}没打开`;
  const status = reply.status ?? 0;
  if (status >= 400 || reply.ok === false) {
    return reply.hint ?? reply.error ?? `打开失败（HTTP ${status === 0 ? "未知" : status}）`;
  }
  if (reply.companion) return `${noun}已打开`;
  if (reply.launched) return `正在启动${noun}…`;
  // 老宿主（或不带 hint 的实现）走到这里：给一句能照着判断的话，别留空。
  return reply.hint ?? (target === "panel" ? "桌面面板没在运行" : "桌面伴随层没在运行，节点图没打开");
}
