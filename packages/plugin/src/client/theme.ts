/**
 * 顶部小条的配色：黑白自适应。
 *
 * 为什么自己算而不直接用宿主的主题变量：
 *   用户原话「我希望把这个部分做一个黑白自适应 不然根本看不清」。
 *   宿主的 `--dsw-menu-surface-fill` 在桌面端实测是 `rgba(248,249,250,0.58)`
 *   （浅色玻璃），而页面本体在深色会话背景上——玻璃条于是黑字压深底，
 *   几乎看不见。变量名对、值不一定是当前底色打底的最优解。
 *
 * 做法：读「这一条背后大概是深还是浅」，据此给两套**不透明**的色板。
 *   不透明是关键：小条本身只有 44px 高，再叠一层半透明就会跟着底色一起糊。
 *
 * 这个文件不碰 DOM（除了 `detectSurfaceTone` 这个薄封装），
 * 所以纯逻辑都能在 node 里单测。
 */

export type SurfaceTone = "light" | "dark";

/** rgba/rgb/hex 都收，认不出来返回 null（调用方走兜底）。 */
export function parseColorLuminance(input: string): number | null {
  const text = input.trim().toLowerCase();
  if (text === "" || text === "transparent" || text === "none") return null;

  const hex = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/.exec(text);
  if (hex !== null) {
    const body = hex[1] ?? "";
    const full =
      body.length === 3
        ? body
            .split("")
            .map((c) => c + c)
            .join("")
        : body;
    const r = parseInt(full.slice(0, 2), 16);
    const g = parseInt(full.slice(2, 4), 16);
    const b = parseInt(full.slice(4, 6), 16);
    const a = full.length === 8 ? parseInt(full.slice(6, 8), 16) / 255 : 1;
    return a < 0.05 ? null : luminance(r, g, b);
  }

  const rgb = /^rgba?\(([^)]+)\)$/.exec(text);
  if (rgb === null) return null;
  const parts = (rgb[1] ?? "")
    .split(/[,\s/]+/)
    .filter((piece) => piece !== "")
    .map((piece) => Number.parseFloat(piece));
  if (parts.length < 3) return null;
  const [r, g, b] = parts;
  const alpha = parts.length >= 4 ? (parts[3] ?? 1) : 1;
  if (!Number.isFinite(r) || !Number.isFinite(g) || !Number.isFinite(b)) return null;
  if (!Number.isFinite(alpha) || alpha < 0.05) return null;
  return luminance(r ?? 0, g ?? 0, b ?? 0);
}

/** WCAG 的相对亮度（0 黑 → 1 白）。 */
function luminance(r: number, g: number, b: number): number {
  const channel = (value: number): number => {
    const v = Math.min(255, Math.max(0, value)) / 255;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
}

/**
 * 底色偏深就是 dark。
 * 0.42 这个门槛不是拍脑袋：深色主题的页面底普遍在 0.02–0.10，
 * 浅色主题普遍在 0.85–0.98，中间地带只有「灰玻璃」那种半透明值。
 */
export function toneFromLuminance(value: number | null, fallback: SurfaceTone = "light"): SurfaceTone {
  if (value === null) return fallback;
  return value < 0.42 ? "dark" : "light";
}

export interface BarPalette {
  /** 条的底色（不透明，见文件头注释）。 */
  surface: string;
  /** 主文字。 */
  text: string;
  /** 次要文字（进度数字、说明）。 */
  textDim: string;
  /** 分隔线 / 未启用按钮的描边。 */
  separator: string;
  /** 「工作」这一侧：打开面板、打开节点图。用户点名要蓝色。 */
  work: string;
  /** 「学习」这一侧。用户点名要绿色。 */
  learn: string;
  /** 分工：既不是工作也不是学习，走中性色，避免三条线打架。 */
  office: string;
  /** 主按钮（打开面板）上的字。 */
  onAccent: string;
  shadow: string;
}

/** 两套色板都按 WCAG 算过对比度（见 theme.test.ts）。 */
export function paletteFor(tone: SurfaceTone): BarPalette {
  if (tone === "dark") {
    return {
      surface: "rgba(24, 24, 27, 0.96)",
      text: "#f2f2f5",
      textDim: "#a8a8b0",
      separator: "rgba(255, 255, 255, 0.16)",
      work: "#4aa3ff",
      learn: "#3ddc84",
      office: "#c8c8d0",
      onAccent: "#0b0b0d",
      shadow: "0 8px 32px rgba(0, 0, 0, 0.55)",
    };
  }
  return {
    surface: "rgba(255, 255, 255, 0.97)",
    text: "#1b1b1f",
    textDim: "#5f5f67",
    separator: "rgba(0, 0, 0, 0.14)",
    work: "#0a6fd8",
    learn: "#12854a",
    office: "#4a4a52",
    onAccent: "#ffffff",
    shadow: "0 8px 28px rgba(0, 0, 0, 0.16)",
  };
}

/** 对比度（1..21）。测试和实机校验都用它，别凭眼睛说「够清楚了」。 */
export function contrastRatio(foreground: string, background: string): number {
  const a = parseColorLuminance(foreground);
  const b = parseColorLuminance(background);
  if (a === null || b === null) return 0;
  const light = Math.max(a, b);
  const dark = Math.min(a, b);
  return (light + 0.05) / (dark + 0.05);
}

/** 把 `rgba(...)` 的 alpha 折算到「它压在这层底上」的实际亮度，用于判断玻璃层。 */
export function compositeLuminance(overlay: string, underneath: string): number | null {
  const top = parseRgba(overlay);
  const bottom = parseRgba(underneath);
  if (top === null || bottom === null) return null;
  const blend = (t: { value: number; alpha: number }, b: { value: number }): number =>
    t.value * t.alpha + b.value * (1 - t.alpha);
  return luminance(
    blend({ value: top.r, alpha: top.alpha }, { value: bottom.r }),
    blend({ value: top.g, alpha: top.alpha }, { value: bottom.g }),
    blend({ value: top.b, alpha: top.alpha }, { value: bottom.b }),
  );
}

function parseRgba(input: string): { r: number; g: number; b: number; alpha: number } | null {
  const text = input.trim().toLowerCase();
  const rgb = /^rgba?\(([^)]+)\)$/.exec(text);
  if (rgb === null) return null;
  const parts = (rgb[1] ?? "")
    .split(/[,\s/]+/)
    .filter((piece) => piece !== "")
    .map((piece) => Number.parseFloat(piece));
  if (parts.length < 3) return null;
  return {
    r: parts[0] ?? 0,
    g: parts[1] ?? 0,
    b: parts[2] ?? 0,
    alpha: parts.length >= 4 ? (parts[3] ?? 1) : 1,
  };
}

/**
 * 在这台机器上，这一条背后是深还是浅。
 *
 * 顺序：body 的背景色 → html 的背景色 → `prefers-color-scheme`。
 * 不读 `--dsw-*`：那套变量在本项目的历史里正是「黑字压深底」的来源。
 * 拿不到 DOM（比如单测）就原样返回兜底值。
 */
export function detectSurfaceTone(fallback: SurfaceTone = "light"): SurfaceTone {
  if (typeof document === "undefined" || typeof window === "undefined") return fallback;
  const read = (element: Element | null): number | null => {
    if (element === null) return null;
    const style = window.getComputedStyle(element);
    const value = parseColorLuminance(style.backgroundColor);
    // 半透明的底不能直接当底色用：它透出的是更下面那层，宁可信主题偏好
    if (value !== null && (style.backgroundColor.includes("rgba") && !style.backgroundColor.endsWith(", 1)"))) {
      return null;
    }
    return value;
  };
  const fromBody = read(document.body);
  if (fromBody !== null) return toneFromLuminance(fromBody, fallback);
  const fromHtml = read(document.documentElement);
  if (fromHtml !== null) return toneFromLuminance(fromHtml, fallback);
  // 桌面端整页常常是「body / html 都透明，真正上色的在最外层容器上」，
  // 这时再往下看一层，最后才信主题偏好。
  const shell = document.body?.firstElementChild ?? null;
  const fromShell = read(shell);
  if (fromShell !== null) return toneFromLuminance(fromShell, fallback);
  if (typeof window.getComputedStyle === "function") {
    const scheme = window.getComputedStyle(document.documentElement).colorScheme ?? "";
    if (scheme.includes("dark") && !scheme.includes("light")) return "dark";
    if (scheme.includes("light") && !scheme.includes("dark")) return "light";
  }
  if (typeof window.matchMedia === "function") {
    const query = window.matchMedia("(prefers-color-scheme: dark)");
    if (query.matches) return "dark";
  }
  return fallback;
}

/**
 * 订阅底色变化：宿主切主题（改 class / data-theme）、系统切明暗、窗口挪到另一块屏。
 * 没有它，条会一直停在打开那一刻的颜色上。
 */
export function watchSurfaceTone(onChange: (tone: SurfaceTone) => void): () => void {
  if (typeof window === "undefined" || typeof document === "undefined") return () => undefined;
  let last = detectSurfaceTone();
  const push = (): void => {
    const next = detectSurfaceTone(last);
    if (next === last) return;
    last = next;
    onChange(next);
  };
  const observer =
    typeof MutationObserver === "function"
      ? new MutationObserver(push)
      : null;
  observer?.observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["class", "style", "data-theme", "data-color-scheme"],
  });
  if (document.body !== null) {
    observer?.observe(document.body, {
      attributes: true,
      attributeFilter: ["class", "style", "data-theme", "data-color-scheme"],
    });
  }
  const media =
    typeof window.matchMedia === "function" ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  const onMedia = (): void => push();
  media?.addEventListener?.("change", onMedia);
  return () => {
    observer?.disconnect();
    media?.removeEventListener?.("change", onMedia);
  };
}
