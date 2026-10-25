/**
 * DSH Work · 客户端半边（浏览器侧）
 *
 * 职责边界（见 docs/ARCHITECTURE.md）：
 *   这一层**只负责"显示"和"把人想做的事变成意图"**，绝不做业务判断。
 *   任务状态的唯一权威在 host 半边（packages/plugin），这里拿到的任何
 *   数据都必须是 host 算好的结果，前端不重算指针、不猜下一步。
 *
 * 加载契约（由宿主 __ModuleLoader__.load 调用）：
 *   - 本文件被 esbuild 打成 CJS，外层由 scripts/build-client.mjs 套上
 *     `window.__ModuleLoader__.load({id, factory})` 的壳。
 *   - 导出只需 { inject, apply } 两个符号；`inject` 是**服务名数组**，
 *     不是包名 —— 写成包名会在宿主里静默拿不到服务。
 *   - apply(ctx) 内做全部副作用，注册函数返回 disposer 由宿主回收。
 */

import { useEffect, useState } from "react";
import { StudyPage } from "./study.tsx";

import { graphEntry, openFeedback, type OpenReply, type OpenTarget } from "./open-entry.ts";
import {
  detectSurfaceTone,
  paletteFor,
  watchSurfaceTone,
  type BarPalette,
  type SurfaceTone,
} from "./theme.ts";

/** 轮询间隔。真实推送走 SSE 是后续优化项；先保证"打开就能看到"这条最短路径。 */
const POLL_MS = 3000;

/** 相对路径：桌面端页面 origin 是 dsh-app://app，web 端是 http://host:port，
 *  两者都用相对路径解析到插件自注册的 /dsh-work 前缀，不硬编码端口。 */
const STATE_URL = "/dsh-work/api/state";
const OPEN_URL = "/dsh-work/api/ui/open";

type NodeStatus = "todo" | "active" | "done" | "skipped";

interface NodeView {
  id: string;
  title: string;
  status: NodeStatus;
  kind: "task" | "concept";
  parentId: string | null;
  children: string[];
  /** 学习页用：写给人看的解释与验收标准（宿主那边叫 detail / acceptance）。 */
  detail?: string;
  acceptance?: string;
}

interface DocView {
  title: string;
  pointer: string | null;
  roots: string[];
  nodes: Record<string, NodeView>;
}

interface Snapshot {
  seq: number;
  doc: DocView;
}

interface WorkState {
  snapshot: Snapshot | null;
  online: boolean;
}

/**
 * 订阅工作区状态。
 * 用 setTimeout 串行轮询而不是 setInterval：请求慢时不堆积，
 * 失败时也能自然地降频（catch 分支同样排下一次）。
 */
function useWorkState(): WorkState {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [online, setOnline] = useState(false);

  useEffect(() => {
    let alive = true;
    let timer: number | undefined;

    const tick = async (): Promise<void> => {
      try {
        // same-origin 带上宿主会话 cookie —— 插件的 authorize 就是靠它认人
        const res = await fetch(STATE_URL, { credentials: "same-origin" });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const data = (await res.json()) as Snapshot;
        if (!alive) return;
        setSnapshot(data);
        setOnline(true);
      } catch {
        if (alive) setOnline(false);
      }
      if (alive) timer = window.setTimeout(() => void tick(), POLL_MS);
    };

    void tick();
    return () => {
      alive = false;
      if (timer !== undefined) window.clearTimeout(timer);
    };
  }, []);

  return { snapshot, online };
}

/** 视觉 token 一律带回落值：宿主换主题时不至于渲染成透明块。 */
const glass: React.CSSProperties = {
  background: "var(--dsw-menu-surface-fill, rgba(255, 255, 255, 0.72))",
  backdropFilter: "var(--dsw-menu-backdrop-filter, blur(24px) saturate(180%))",
  WebkitBackdropFilter: "var(--dsw-menu-backdrop-filter, blur(24px) saturate(180%))",
  border: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.08))",
  boxShadow: "var(--dsw-elevation-panel, 0 8px 32px rgba(0, 0, 0, 0.12))",
  borderRadius: 22,
};

/** 窗口顶部是 -webkit-app-region:drag 的拖拽带；整条标成 no-drag，
 *  否则条上的按钮点下去会变成拖窗口。React 的 CSSProperties 没收录这个
 *  非标准属性，只能绕一次类型（运行时由 Chromium 认这个前缀）。 */
const noDrag = { WebkitAppRegion: "no-drag" } as unknown as React.CSSProperties;

/** 面板图标：一个带两行字的窗口，比纯文字更像「点开有东西」。 */
function PanelIcon(): React.ReactElement {
  return (
    <svg width="13" height="13" viewBox="0 0 16 16" fill="none" aria-hidden="true">
      <rect x="1.7" y="2.6" width="12.6" height="10.8" rx="2.2" stroke="currentColor" strokeWidth="1.3" />
      <path d="M5.4 6.3h5.2M5.4 9.3h3.2" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" />
    </svg>
  );
}

/** 节点图图标：两颗上游汇到一颗下游，一眼能认出「这是一张图」。 */
function GraphIcon(): React.ReactElement {
  return (
    <svg width="13" height="13" viewBox="0 0 16 16" fill="none" aria-hidden="true">
      <circle cx="3.7" cy="4.2" r="1.9" stroke="currentColor" strokeWidth="1.3" />
      <circle cx="3.7" cy="11.8" r="1.9" stroke="currentColor" strokeWidth="1.3" />
      <circle cx="12.3" cy="8" r="1.9" stroke="currentColor" strokeWidth="1.3" />
      <path d="M5.4 5 10.6 7.1M5.4 11 10.6 8.9" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" />
    </svg>
  );
}

/**
 * 顶部小条。
 *
 * 位置说明：shell.overlay 的父层是 position:absolute; inset:0; pointer-events:none，
 * 直系子元素自动 pointer-events:auto。窗口顶部有 -webkit-app-region:drag 的拖拽带，
 * 所以这里要显式把整条标成 no-drag，否则按钮点了会变成拖窗口。
 *
 * 「打开面板」按钮：桌面面板是另一个进程的 Win32 窗口，浏览器碰不到它，
 * 所以按钮只把意愿交给宿主（POST /api/ui/open），伴随层轮询状态时取走。
 * 用户原话是「不然不知道怎么打开」—— 入口必须写清名字，不能只放个图标。
 */
/**
 * 「学习 / 分工」这类二级入口的样式：描边小胶囊，颜色由这一侧的意思决定。
 *
 * 用户原话「工作模式的icon可以是蓝色 但是学习 可以为绿色」——所以颜色按
 * **用途**给，不按启用态给：禁用时只把同一个色降透明度，语义不丢。
 */
function railButtonStyle(enabled: boolean, accent: string, palette: BarPalette): React.CSSProperties {
  return {
    flex: "0 0 auto",
    display: "inline-flex",
    alignItems: "center",
    gap: 5,
    height: 28,
    padding: "0 10px 0 8px",
    borderRadius: 999,
    border: `0.5px solid ${enabled ? accent : palette.separator}`,
    background: "transparent",
    color: enabled ? accent : palette.textDim,
    font: "600 12px/1 system-ui, -apple-system, 'Segoe UI', sans-serif",
    cursor: enabled ? "pointer" : "default",
    opacity: enabled ? 1 : 0.55,
    ...noDrag,
  };
}
/** 学习：一本翻开的小书。绿色那一侧用它，比纯文字好认。 */
function LearnIcon(): React.ReactElement {
  return (
    <svg width="12" height="12" viewBox="0 0 16 16" fill="none" aria-hidden="true">
      <path
        d="M8 4.2c-1.5-1-3.4-1.3-5.4-1v8.9c2-.3 3.9 0 5.4 1 1.5-1 3.4-1.3 5.4-1V3.2c-2-.3-3.9 0-5.4 1Z"
        stroke="currentColor"
        strokeWidth="1.2"
        strokeLinejoin="round"
      />
      <path d="M8 4.4v8.6" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" />
    </svg>
  );
}
/** 分工：两栏并排，左边 AI、右边人——一眼看出「这事归谁」。 */
function SplitIcon(): React.ReactElement {
  return (
    <svg width="12" height="12" viewBox="0 0 16 16" fill="none" aria-hidden="true">
      <rect x="1.6" y="2.6" width="5.6" height="10.8" rx="1.6" stroke="currentColor" strokeWidth="1.2" />
      <rect x="8.8" y="2.6" width="5.6" height="10.8" rx="1.6" stroke="currentColor" strokeWidth="1.2" />
      <path d="M4.4 6.2v3.6M11.6 6.2v3.6" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" />
    </svg>
  );
}
function Bar(): React.ReactElement {
  const { snapshot, online } = useWorkState();
  const [note, setNote] = useState<string | null>(null);
  const [opening, setOpening] = useState(false);
  // 学习页是网页侧的一整页（不用伴随层）：只读、纯前端渲染，数据就是这份 doc。
  const [studyOpen, setStudyOpen] = useState(false);
  const [studySelected, setStudySelected] = useState<string | null>(null);
  // 黑白自适应：跟着这一条背后的底色走；切主题、换系统明暗都能跟上。
  const [tone, setTone] = useState<SurfaceTone>(() => detectSurfaceTone());
  useEffect(() => watchSurfaceTone(setTone), []);
  const palette = paletteFor(tone);
  const doc = snapshot?.doc;

  // 统计口径：skipped 不计入分母 —— 它是"明确不做"，不该拖累进度
  const nodes = doc ? Object.values(doc.nodes) : [];
  const counted = nodes.filter((n) => n.status !== "skipped");
  const done = counted.filter((n) => n.status === "done").length;
  const current = doc?.pointer ? doc.nodes[doc.pointer] : undefined;
  // doc 还没到手时 nodeCount 用 null：置灰但不说成「没有任务」
  const graph = graphEntry(doc ? nodes.length : null);

  useEffect(() => {
    if (note === null) return;
    const timer = window.setTimeout(() => setNote(null), 2800);
    return () => window.clearTimeout(timer);
  }, [note]);

  /**
   * 两个入口走同一条路：只把意愿交给宿主（POST /api/ui/open），
   * 桌面窗口是另一个进程的 Win32 窗口，浏览器碰不到它。
   * 反馈文案收在 open-entry.ts 里，四档都有单测。
   */
  const openCompanion = async (target: OpenTarget, rail?: "learn" | "office"): Promise<void> => {
    if (opening) return;
    setOpening(true);
    try {
      const res = await fetch(OPEN_URL, {
        method: "POST",
        credentials: "same-origin",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(rail === undefined ? { target } : { target, rail }),
      });
      const data = (await res.json().catch(() => ({}))) as OpenReply;
      setNote(openFeedback(target, { ...data, status: res.status }));
    } catch {
      setNote(openFeedback(target, { offline: true }));
    } finally {
      setOpening(false);
    }
  };

  return (
    <>
      <style>{`
        .dshwork-open { transition: filter 120ms cubic-bezier(.16,1,.3,1), transform 120ms cubic-bezier(.16,1,.3,1), background-color 120ms cubic-bezier(.16,1,.3,1); }
        .dshwork-open:hover:not(:disabled) { filter: brightness(1.07); }
        .dshwork-open:active:not(:disabled) { transform: scale(.97); }
        .dshwork-open:focus-visible { outline: 2px solid ${palette.work}; outline-offset: 2px; }
        .dshwork-ghost:hover:not(:disabled) { background-color: color-mix(in srgb, currentColor 12%, transparent); }
      `}</style>
      <div
        data-dsh-work-bar="1"
        data-dsh-work-tone={tone}
        style={{
          position: "fixed",
          top: 52,  // 52 = 窗口标题栏高度再留一点；贴 top:8 会压在标题栏上
          left: "50%",
          transform: "translateX(-50%)",
          display: "flex",
          alignItems: "center",
          gap: 12,
          height: 44,
          padding: "0 10px 0 18px",
          maxWidth: "min(560px, 64vw)",
          font: "500 13px/1.2 system-ui, -apple-system, 'Segoe UI', sans-serif",
          // 不再用宿主的玻璃色：58% 的白压在深色会话底上会变成「黑字压深底」。
          background: palette.surface,
          color: palette.text,
          border: `0.5px solid ${palette.separator}`,
          boxShadow: palette.shadow,
          borderRadius: 22,
          backdropFilter: "blur(18px) saturate(140%)",
          WebkitBackdropFilter: "blur(18px) saturate(140%)",
          ...noDrag,
        }}
        role="toolbar"
        aria-label="DSH Work 任务条"
      >
        <span
          style={{
            width: 7,
            height: 7,
            borderRadius: 999,
            flex: "0 0 auto",
            background: online ? palette.learn : palette.textDim,
            boxShadow: online ? `0 0 8px ${palette.learn}` : "none",
          }}
        />
        <span
          role="status"
          aria-live="polite"
          style={{
            flex: "1 1 auto",
            minWidth: 0,
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap",
          }}
        >
          {!doc ? "DSH Work · 未连接" : (current?.title ?? "全部已完成")}
        </span>
        <span
          style={{
            flex: "0 0 auto",
            fontVariantNumeric: "tabular-nums",
            color: palette.textDim,
            fontSize: 12,
          }}
        >
          {counted.length > 0 ? `${done}/${counted.length}` : "—"}
        </span>
        <span
          aria-hidden="true"
          style={{
            flex: "0 0 auto",
            width: 1,
            height: 18,
            background: palette.separator,
          }}
        />
        <span
          style={{
            flex: "0 0 auto",
            display: "inline-flex",
            alignItems: "center",
            gap: 8,
          }}
        >
          <button
            type="button"
            className="dshwork-open"
            data-dsh-work-open="1"
            onClick={() => void openCompanion("panel")}
            disabled={opening}
            title="打开桌面任务面板；面板里可以点进已落库文档的节点图"
            aria-label="打开桌面任务面板"
            style={{
              flex: "0 0 auto",
              display: "inline-flex",
              alignItems: "center",
              gap: 6,
              height: 28,
              padding: "0 12px 0 9px",
              borderRadius: 999,
              border: `0.5px solid ${palette.work}`,
              background: palette.work,
              color: palette.onAccent,
              font: "600 12px/1 system-ui, -apple-system, 'Segoe UI', sans-serif",
              cursor: opening ? "default" : "pointer",
              opacity: opening ? 0.65 : 1,
              ...noDrag,
            }}
          >
            <PanelIcon />
            打开面板
          </button>
          <button
            type="button"
            className="dshwork-open dshwork-ghost"
            data-dsh-work-graph="1"
            onClick={() => void openCompanion("graph")}
            disabled={opening || !graph.enabled}
            title={graph.hint}
            aria-label={graph.enabled ? "打开节点图（整张任务地图，只读）" : graph.hint}
            style={{
              flex: "0 0 auto",
              display: "inline-flex",
              alignItems: "center",
              gap: 6,
              height: 28,
              padding: "0 11px 0 9px",
              borderRadius: 999,
              border: `0.5px solid ${graph.enabled ? palette.work : palette.separator}`,
              background: "transparent",
              color: graph.enabled ? palette.work : palette.textDim,
              font: "600 12px/1 system-ui, -apple-system, 'Segoe UI', sans-serif",
              cursor: graph.enabled && !opening ? "pointer" : "default",
              opacity: graph.enabled ? (opening ? 0.65 : 1) : 0.55,
              ...noDrag,
            }}
          >
            <GraphIcon />
            打开节点图
          </button>
          {/* 学习 / 分工：同一张图换一种读法。用户原话是「在基础面板的按钮上有个切换」，
              所以入口放在这里，而不是让人先开图再找。 */}
          <button
            type="button"
            className="dshwork-open dshwork-ghost"
            data-dsh-work-learn="1"
            onClick={() => void openCompanion("graph", "learn")}
            disabled={opening || !graph.enabled}
            title={graph.enabled ? "打开节点图并切到「学习」：右边给这颗的基础知识与详解流程" : graph.hint}
            aria-label="打开节点图的学习页"
            style={railButtonStyle(graph.enabled && !opening, palette.learn, palette)}
          >
            <LearnIcon />
            学习
          </button>
          <button
            type="button"
            className="dshwork-open dshwork-ghost"
            data-dsh-work-office="1"
            onClick={() => void openCompanion("graph", "office")}
            disabled={opening || !graph.enabled}
            title={graph.enabled ? "打开节点图并切到「分工」：哪部分 AI 做、哪部分人做" : graph.hint}
            aria-label="打开节点图的分工页"
            style={railButtonStyle(graph.enabled && !opening, palette.office, palette)}
          >
            <SplitIcon />
            分工
          </button>
          {/* 「学习页」和上面那颗「学习」不是一回事：那颗把原生节点图切到学习栏
              （人站在图里），这颗在网页里直接铺开一页（人坐下来读），不需要伴随层。 */}
          <button
            type="button"
            className="dshwork-open dshwork-ghost"
            data-dsh-work-study-page="1"
            onClick={() => setStudyOpen((open) => !open)}
            disabled={!doc || nodes.length === 0}
            title={graph.enabled ? "在网页里打开学习页：左边要学的，右边这一颗的基础知识与详解流程" : graph.hint}
            aria-label="打开网页学习页"
            style={railButtonStyle(Boolean(doc) && nodes.length > 0, palette.learn, palette)}
          >
            <LearnIcon />
            学习页
          </button>
        </span>
      </div>
      {studyOpen && doc ? (
        <StudyPage
          doc={doc}
          selected={studySelected}
          onSelect={setStudySelected}
          onClose={() => setStudyOpen(false)}
        />
      ) : null}
      {note !== null ? (
        <div
          role="status"
          aria-live="polite"
          style={{
            position: "fixed",
            top: 58,
            left: "50%",
            transform: "translateX(-50%)",
            padding: "7px 14px",
            maxWidth: "min(520px, 70vw)",
            textAlign: "center",
            font: "500 12px/1.35 system-ui, -apple-system, 'Segoe UI', sans-serif",
            color: "var(--dsw-text-secondary, #4a4a4e)",
            ...noDrag,
            ...glass,
            borderRadius: 999,
            boxShadow: "var(--dsw-elevation-popover, 0 6px 20px rgba(0, 0, 0, 0.10))",
          }}
        >
          {note}
        </div>
      ) : null}
    </>
  );
}

/** 宿主要求的服务名数组（不是包名）。slots 服务提供槽位注册能力。 */
export const inject = ["slots"];

export function apply(ctx: any): void {
  // slot 是懒注册的：inject 会在该槽位出现后立即执行注册回调，
  // 槽位后来被销毁时回调返回值（disposer）负责收拾干净。
  ctx.slots.inject("shell.overlay", () =>
    ctx.slots.register(
      { name: "shell.overlay", id: "dshwork-bar", order: 20 },
      () => <Bar />,
    ),
  );
}
