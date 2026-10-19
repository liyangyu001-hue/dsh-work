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

/** 轮询间隔。真实推送走 SSE 是后续优化项；先保证"打开就能看到"这条最短路径。 */
const POLL_MS = 3000;

/** 相对路径：桌面端页面 origin 是 dsh-app://app，web 端是 http://host:port，
 *  两者都用相对路径解析到插件自注册的 /dsh-work 前缀，不硬编码端口。 */
const STATE_URL = "/dsh-work/api/state";

type NodeStatus = "todo" | "active" | "done" | "skipped";

interface NodeView {
  id: string;
  title: string;
  status: NodeStatus;
  kind: "task" | "concept";
  parentId: string | null;
  children: string[];
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

/**
 * 顶部小条。
 *
 * 位置说明：shell.overlay 的父层是 position:absolute; inset:0; pointer-events:none，
 * 直系子元素自动 pointer-events:auto。窗口顶部有 -webkit-app-region:drag 的拖拽带，
 * 所以这里要显式把整条标成 no-drag，否则按钮点了会变成拖窗口。
 */
function Bar(): React.ReactElement {
  const { snapshot, online } = useWorkState();
  const doc = snapshot?.doc;

  // 统计口径：skipped 不计入分母 —— 它是"明确不做"，不该拖累进度
  const nodes = doc ? Object.values(doc.nodes) : [];
  const counted = nodes.filter((n) => n.status !== "skipped");
  const done = counted.filter((n) => n.status === "done").length;
  const current = doc?.pointer ? doc.nodes[doc.pointer] : undefined;

  return (
    <div
      style={{
        position: "fixed",
        top: 8,
        left: "50%",
        transform: "translateX(-50%)",
        display: "flex",
        alignItems: "center",
        gap: 12,
        height: 44,
        padding: "0 18px",
        maxWidth: "min(520px, 60vw)",
        font: "500 13px/1.2 system-ui, -apple-system, 'Segoe UI', sans-serif",
        color: "var(--dsw-text-primary, #1a1a1a)",
        ...noDrag,
        ...glass,
      }}
      role="status"
      aria-live="polite"
    >
      <span
        style={{
          width: 7,
          height: 7,
          borderRadius: 999,
          flex: "0 0 auto",
          background: online ? "var(--dsw-accent, #0a84ff)" : "var(--dsw-text-tertiary, #9a9a9e)",
          boxShadow: online ? "0 0 8px var(--dsw-accent, #0a84ff)" : "none",
        }}
      />
      <span
        style={{
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
          opacity: 0.55,
          fontSize: 12,
        }}
      >
        {counted.length > 0 ? `${done}/${counted.length}` : "—"}
      </span>
    </div>
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
