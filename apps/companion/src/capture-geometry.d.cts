/**
 * `capture-geometry.cjs` 的类型声明。
 *
 * 这个包是纯 CJS、没有构建步骤（伴随层直接跑源码），所以真实实现只有 .cjs 一份；
 * 这里的声明只服务于类型检查与编辑器补全。**改实现时必须同步改这里** ——
 * 两处不一致时 tsc 会在测试文件里报错，这就是我们想要的提醒方式。
 */

export interface Point {
  x: number;
  y: number;
}

export interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface Size {
  width: number;
  height: number;
}

export declare function normalizeSelection(start: Point, end: Point): Rect;

export declare function toCropRect(input: {
  /** 显示器尺寸（DIP） */
  display: Size;
  /** 缩略图尺寸（像素） */
  image: Size;
  /** 选框（DIP，相对显示器左上角） */
  selection: Rect;
  /** 小于这个边长就当误触，默认 8 */
  minSize?: number;
}): Rect | null;

export declare function matchSource(
  /** 只用到 id；bounds 完全不被读取，这里故意放宽到 Partial */
  display: { id: number; bounds?: Partial<Rect> },
  index: number,
  sources: Array<{ id: string; display_id: string }>,
): { id: string; display_id: string } | null;
