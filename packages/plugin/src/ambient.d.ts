/**
 * 宿主运行时模块的最小类型声明。
 *
 * 为什么需要它：`@deepseek-ai/dsh-tools` 由 DSH 在**运行期**通过自己的模块解析层提供，
 * 不从 npm 安装（npm 上也没有对应可解析的公开包）。因此：
 *   - 构建时把它标记为 external，由宿主在载入时解析真身；
 *   - 本地 typecheck 用这份声明兜底。
 *
 * 这里的形状取自同机第三方插件的真实调用（dsh-blender-plugin/lib/index.js:1153-1161）。
 * 宿主升级若改了形状，只需动这一个文件。
 */
declare module '@deepseek-ai/dsh-tools' {
  /** 参数描述：属性名 → JSON Schema 片段。宿主 `defineTool` 的简写形态。 */
  export type ToolParameters = Record<string, unknown>;

  export interface ToolOutput {
    /** 返回值的 JSON Schema（宿主用 lossless-JSON 规则校验）。 */
    schema: Record<string, unknown>;
    /** 把返回值渲染成会话里的内容块数组。 */
    render: (args: unknown, value: unknown) => unknown;
  }

  /** 宿主注入的执行上下文。`signal` 在用户打断 / 超时截止时被 abort。 */
  export interface ToolExecuteContext {
    readonly signal?: AbortSignal;
    readonly [key: string]: unknown;
  }

  export interface ToolSpec<A = unknown, R = unknown> {
    name: string;
    description: string;
    parameters?: ToolParameters;
    output?: ToolOutput;
    /** 静态截止时间（毫秒）。宿主到点会判超时。 */
    timeoutMs?: number;
    isConcurrencySafe?: (args: A) => boolean;
    execute: (args: A, context: ToolExecuteContext) => Promise<R> | R;
  }

  /**
   * 校验并规范化工具定义。
   * 注意：必须把完整对象一次性交给它，自己 spread 一个成品对象会绕过校验。
   */
  export function defineTool<A = unknown, R = unknown>(spec: ToolSpec<A, R>): unknown;
}
