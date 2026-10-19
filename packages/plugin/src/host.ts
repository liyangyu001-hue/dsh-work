/**
 * 我们对宿主（cordis ctx）的**全部依赖**，集中声明在这一个文件里。
 *
 * 这是整个插件的「宿主接缝」：宿主升级导致某个服务改了形状，只需要改这里，
 * 其余代码不受影响；同时它也是一份可执行的文档——一眼能看出这个插件到底用了宿主什么。
 *
 * 取舍：只声明我们真正调用的成员，不做完整镜像。多声明一个成员就多一处会过期的假设。
 */

import type { IncomingMessage, ServerResponse } from 'node:http';

/** `ctx.webServer.register` 的路由形态。 */
export interface WebRoute {
  /** `exact` 逐字匹配；`prefix` 匹配 `p` 与 `p/<anything>`。 */
  kind: 'exact' | 'prefix';
  /** 绝对路径，结尾不带斜杠。 */
  path: string;
  handler: (req: IncomingMessage, res: ServerResponse) => void | Promise<void>;
}

/** 连接层给的信任判定结果：undefined = 放行，否则是应回的状态码。 */
export type ConnectionRejection = number | { status?: number } | undefined;

export interface HostContext {
  /** 注册模型侧工具；返回退订函数。 */
  tools: {
    register(tool: unknown): () => void;
  };

  /**
   * 注册一个随插件生命周期自动回收的副作用。
   * 传入函数返回 disposer；插件卸载时宿主调用它。
   */
  effect(run: () => void | (() => void)): void;

  webServer: {
    register(route: WebRoute): () => void;
    /** 监听端口；配置为 0 时是操作系统分配的端口。 */
    readonly port: number;
  };

  /**
   * 连接层的信任校验。`requestRejection` 复用宿主的 Host/Origin 与浏览器会话判定，
   * 让我们手写的路由与宿主自带路由处在同一条信任边界上。
   */
  connection?: {
    requestRejection(request: { headers: Record<string, string | string[] | undefined> }): ConnectionRejection;
  };

  /** 结构化日志。 */
  logger?: (name: string) => {
    info(message: string): void;
    warn(message: string): void;
    error(message: string): void;
  };
}

/** `requestRejection` 的返回值归一成状态码。 */
export function rejectionStatus(rejection: ConnectionRejection): number | null {
  if (rejection === undefined) return null;
  if (typeof rejection === 'number') return rejection;
  return typeof rejection.status === 'number' ? rejection.status : 403;
}
