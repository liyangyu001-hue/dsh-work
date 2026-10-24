/**
 * 「任务库落在哪个目录」这件事的解析。
 *
 * 为什么要单独一个文件：真正的判据是三条优先级 + 一次带守卫的服务读取，
 * 在 `index.ts` 里写成嵌套三元表达式既读不出来也没法测；而这段判据直接决定
 * 玻璃条绑到谁的树上 —— 绑错了，用户看到的就是一条「全部已完成 / 打开节点图置灰」
 * 的空条。
 *
 * 踩过的坑（2026-10-24 真机取证）：`dsh --profile desktop` 的进程 cwd 是
 * `%USERPROFILE%\.dsh\profiles\desktop`（宿主把工作目录当 profile 目录用），
 * 不是用户的工作区。原先直接拿 `process.cwd()` 当 root，桌面 profile 里的
 * 任务库就是 `profiles\desktop\.dsh-work\work.json` 那个空库，节点的树在
 * `D:\work\AI\work\.dsh-work\` 里躺着却读不到，「打开节点图」于是永远置灰。
 * 宿主登记的 `workspaceRegistry` 才记着用户真正在用的目录。
 */

/** 根目录是从哪一条规则得出的 —— 写进日志，出事时一眼看出绑到了谁身上。 */
export type RootSource = 'config' | 'workspace' | 'cwd';

export interface RootDecision {
  readonly root: string;
  readonly source: RootSource;
}

/**
 * `workspaceRegistry.list()` 返回的实体的最小投影。
 * 只认这两个字段：`path` 是目录，`sessionIds` 用来判断这个工作区有没有人用。
 */
export interface WorkspaceLike {
  readonly path: string;
  readonly sessionIds?: readonly unknown[];
}

function usable(workspace: WorkspaceLike): boolean {
  return typeof workspace.path === 'string' && workspace.path.trim() !== '';
}

/**
 * 从「宿主登记的工作区」里挑一个当任务库根目录。
 *
 * 排序口径：**有会话的工作区优先**，其次登记顺序里的第一个。
 * 理由：登记表里可能有装插件时顺手建的空工作区，跟着它走等于换了个空库；
 * 有会话的那个才是用户真正在用的。登记表本身是 durable registry order，
 * 所以不需要额外的时间戳。
 */
export function pickWorkspaceRoot(workspaces: readonly WorkspaceLike[] | undefined): string | undefined {
  if (workspaces === undefined) return undefined;
  const usableList = workspaces.filter(usable);
  if (usableList.length === 0) return undefined;
  const used = usableList.find((workspace) => (workspace.sessionIds?.length ?? 0) > 0);
  const picked = used ?? usableList[0];
  return picked === undefined ? undefined : picked.path;
}

/**
 * 三条优先级：显式配置 > 宿主登记的工作区 > 进程 cwd。
 *
 * 配置排第一是因为它最明确（用户自己写的）；cwd 垫底是因为在桌面 profile 里
 * 它是 profile 目录，只能当「什么都不知道」时的最后一个答案。
 */
export function pickRoot(options: {
  readonly configured?: string | undefined;
  readonly workspaces?: readonly WorkspaceLike[] | undefined;
  readonly cwd: string;
}): RootDecision {
  const configured = options.configured === undefined ? '' : options.configured.trim();
  if (configured !== '') return { root: configured, source: 'config' };
  const fromWorkspace = pickWorkspaceRoot(options.workspaces);
  if (fromWorkspace !== undefined) return { root: fromWorkspace, source: 'workspace' };
  return { root: options.cwd, source: 'cwd' };
}

/**
 * 读宿主的 `workspaceRegistry`，读不到就回 undefined。
 *
 * 为什么用 `get` 而不是 `inject`：本插件的硬依赖只有 `tools` / `webServer`，
 * 把 `workspaceRegistry` 写进 inject 会让它在没有该服务的宿主上整条装不起来 ——
 * 一个「根目录猜想」不值得这个代价。读不到就退回 cwd，行为与旧版一致。
 */
export function listWorkspaces(get: (name: string) => unknown): readonly WorkspaceLike[] | undefined {
  try {
    const registry = get('workspaceRegistry') as { list?: () => unknown } | undefined;
    if (registry === undefined || registry === null) return undefined;
    if (typeof registry.list !== 'function') return undefined;
    const list = registry.list();
    if (!Array.isArray(list)) return undefined;
    return list.filter(
      (item): item is WorkspaceLike =>
        item !== null && typeof item === 'object' && typeof (item as WorkspaceLike).path === 'string',
    );
  } catch {
    // 服务还没起来 / 名字不存在 / list 抛错：都只是「不知道」，不是失败。
    return undefined;
  }
}
