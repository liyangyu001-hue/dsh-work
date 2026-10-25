/**
 * 网页侧的「学习页」。
 *
 * 和原生节点图右侧那栏是同一件事的两种场景：
 * - 站在图里（原生）：右栏跟着选中的节点走，边看图边读；
 * - 坐在网页前（这里）：左边一列要学的东西，右边这一颗的基础知识与详解流程。
 *
 * 数据全走已有的 `/dsh-work/api/state` 轮询（父组件传 doc 进来）；
 * 只有「追问」这一件事会打到宿主的 `/dsh-work/api/chat`（模型在宿主那边，网页不直连模型）。
 */
import { useEffect, useRef, useState } from "react";

import {
  executorLabel,
  filterStudyRows,
  studyRows,
  studyProgress,
  studyTips,
  statusLabel,
  type StudyDocLike,
} from "./study-model.ts";

/** 追问最多带这么多轮上文：带太多会被旧话题拖走，和宿主那边 chat.ts 的取法保持一致。 */
const CHAT_HISTORY_TURNS = 6;
const CHAT_URL = "/dsh-work/api/chat";

interface ChatTurn {
  readonly question: string;
  readonly answer: string;
}

const SURFACE: React.CSSProperties = {
    // 宿主那套玻璃色是半透明的（实测 rgba(248,249,250,0.58)），一页正文摆上去会透出底下的会话列表。
  // 这一页要读字，用系统画布色：不透明、且跟着明暗主题走。
  background: "Canvas",
  border: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.08))",
  boxShadow: "var(--dsw-elevation-panel, 0 8px 32px rgba(0, 0, 0, 0.12))",
};

const FONT = "system-ui, -apple-system, 'Segoe UI', sans-serif";

function statusColor(status: string): string {
  if (status === "done") return "var(--dsw-text-tertiary, #9a9a9e)";
  if (status === "active") return "var(--dsw-accent, #0a84ff)";
  if (status === "skipped") return "var(--dsw-text-tertiary, #c8c8cc)";
  return "var(--dsw-text-secondary, #4a4a4e)";
}

export function StudyPage(props: {
  readonly doc: StudyDocLike;
  readonly selected: string | null;
  readonly onSelect: (id: string) => void;
  readonly onClose: () => void;
}): React.ReactElement {
  const rows = studyRows(props.doc);
  // 目录太长时按标题找：只匹配标题，不搜正文（那一列是目录，不是搜索框）。
  const [query, setQuery] = useState("");
  const shown = filterStudyRows(rows, query);
  const progress = studyProgress(rows);
  // 没选中过就落到当前任务，再不行落到第一步 —— 打开就有内容，不摆空壳
  const first = props.doc.pointer ?? rows[0]?.id ?? null;
  const active = props.selected ?? first;
  const tips = studyTips(props.doc, active);
  const activeRow = rows.find((row) => row.id === active);

  // 追问：模型在宿主那边，网页只发一句话过去（/dsh-work/api/chat）。
  // 问答不落库 —— 问一句就写一次 work.json 的话，任务库会被聊天记录灌满。
  const [turns, setTurns] = useState<readonly ChatTurn[]>([]);
  const [draft, setDraft] = useState("");
  const [asking, setAsking] = useState(false);
  const [failure, setFailure] = useState<string | null>(null);
  const listRef = useRef<HTMLDivElement | null>(null);

  // 换一颗节点就把对话清掉：上面问的是上一颗的事，接在这里会答非所问。
  useEffect(() => {
    setTurns([]);
    setDraft("");
    setFailure(null);
  }, [active]);

  const send = async (): Promise<void> => {
    const question = draft.trim();
    if (question === "" || asking) return;
    setAsking(true);
    setFailure(null);
    try {
      const res = await fetch(CHAT_URL, {
        method: "POST",
        credentials: "same-origin",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ nodeId: active, question, history: turns.slice(-CHAT_HISTORY_TURNS) }),
      });
      const body = (await res.json()) as { ok?: boolean; answer?: string; error?: string };
      if (body.ok !== true || typeof body.answer !== "string") {
        setFailure(typeof body.error === "string" ? body.error : `追问失败（HTTP ${res.status}）`);
        return;
      }
      const answer = body.answer;
      setTurns((prev) => [...prev, { question, answer }]);
      setDraft("");
      requestAnimationFrame(() => {
        const el = listRef.current;
        if (el !== null) el.scrollTop = el.scrollHeight;
      });
    } catch (error) {
      setFailure(`追问没发出去：${String(error).slice(0, 120)}`);
    } finally {
      setAsking(false);
    }
  };

  return (
    <div
      role="dialog"
      aria-label="学习页"
      data-dsh-work-study="1"
      style={{
        position: "fixed",
        inset: "64px 24px 24px 24px",
        display: "flex",
        flexDirection: "column",
        borderRadius: 20,
        overflow: "hidden",
        font: `500 13px/1.5 ${FONT}`,
        color: "var(--dsw-text-primary, #1a1a1a)",
        ...SURFACE,
        zIndex: 40,
      }}
    >
      <header
        style={{
          display: "flex",
          alignItems: "center",
          gap: 10,
          padding: "12px 16px",
          borderBottom: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.08))",
        }}
      >
        <strong style={{ font: `600 14px/1.2 ${FONT}` }}>学习</strong>
        <span style={{ fontSize: 12, opacity: 0.6 }}>
          {progress.total > 0 ? `已完成 ${progress.done}/${progress.total}` : "还没有任务树"}
        </span>
        <span style={{ flex: "1 1 auto" }} />
        <button
          type="button"
          onClick={props.onClose}
          aria-label="关掉学习页"
          style={{
            height: 26,
            padding: "0 12px",
            borderRadius: 999,
            border: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.12))",
            background: "transparent",
            color: "var(--dsw-text-secondary, #4a4a4e)",
            font: `600 12px/1 ${FONT}`,
            // DSH 的全局样式会给按钮加下划线；这一页是版面内容，不是链接。
            textDecoration: "none",
            cursor: "pointer",
          }}
        >
          回到任务
        </button>
      </header>

      <div style={{ display: "flex", flex: "1 1 auto", minHeight: 0 }}>
        {/* 左：要学的东西，按树的顺序 */}
        <nav
          style={{
            flex: "0 0 260px",
            overflowY: "auto",
            padding: "10px 8px",
            borderRight: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.08))",
          }}
        >
          <input
            data-dsh-work-study-search="1"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="按标题找…"
            style={{
              width: "calc(100% - 20px)",
              margin: "0 10px 8px",
              padding: "6px 10px",
              borderRadius: 8,
              border: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.12))",
              background: "transparent",
              color: "inherit",
              font: `500 12.5px/1.4 ${FONT}`,
            }}
          />
          {rows.length === 0 ? (
            <p style={{ margin: "12px 10px", fontSize: 12, opacity: 0.6 }}>
              还没有任务树：先在面板里拆一次任务，这里就会长出要学的东西。
            </p>
          ) : shown.length === 0 ? (
            <p style={{ margin: "12px 10px", fontSize: 12, opacity: 0.6 }}>没有标题匹配的节点。</p>
          ) : (
            shown.map((row) => {
              const on = row.id === active;
              return (
                <button
                  key={row.id}
                  type="button"
                  onClick={() => props.onSelect(row.id)}
                  title={`${row.title}（${statusLabel(row.status)}）`}
                  style={{
                    display: "flex",
                    alignItems: "center",
                    gap: 8,
                    width: "100%",
                    padding: `6px 10px 6px ${10 + row.depth * 12}px`,
                    marginBottom: 2,
                    border: "none",
                    borderRadius: 8,
                    background: on ? "color-mix(in srgb, var(--dsw-accent, #0a84ff) 14%, transparent)" : "transparent",
                    color: on ? "var(--dsw-accent, #0a84ff)" : "var(--dsw-text-primary, #1a1a1a)",
                    font: `${on ? 600 : 500} 13px/1.3 ${FONT}`,
                    textAlign: "left",
                    textDecoration: "none",
                    cursor: "pointer",
                  }}
                >
                  <span
                    aria-hidden="true"
                    style={{
                      flex: "0 0 auto",
                      width: 6,
                      height: 6,
                      borderRadius: 999,
                      background: statusColor(row.status),
                    }}
                  />
                  <span style={{ overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{row.title}</span>
                  {executorLabel(row.executor) !== "" ? (
                    <span
                      data-dsh-work-executor={row.executor}
                      style={{
                        flex: "0 0 auto",
                        marginLeft: "auto",
                        padding: "1px 6px",
                        borderRadius: 999,
                        border: "1px solid color-mix(in srgb, currentColor 24%, transparent)",
                        fontSize: 10.5,
                        lineHeight: 1.5,
                        opacity: 0.75,
                        whiteSpace: "nowrap",
                      }}
                    >
                      {executorLabel(row.executor)}
                    </span>
                  ) : null}
                </button>
              );
            })
          )}
        </nav>

        {/* 右：这一颗的基础知识与详解流程 */}
        <section style={{ flex: "1 1 auto", overflowY: "auto", padding: "18px 22px" }}>
          {tips === null ? (
            <p style={{ margin: 0, fontSize: 13, opacity: 0.6 }}>左边挑一颗，这里给它的基础知识与详解流程。</p>
          ) : (
            <>
              <h2 style={{ margin: "0 0 2px", font: `600 17px/1.35 ${FONT}` }}>{tips.title}</h2>
              <p style={{ margin: "0 0 18px", fontSize: 12, opacity: 0.55 }}>
                {tips.state}
                {executorLabel(activeRow?.executor) !== "" ? ` · 这一步：${executorLabel(activeRow?.executor)}` : ""}
              </p>

              <h3 style={{ margin: "0 0 6px", font: `600 12px/1.4 ${FONT}`, color: "var(--dsw-accent, #0a84ff)" }}>
                基础知识
              </h3>
              <p style={{ margin: "0 0 20px", fontSize: 13.5, lineHeight: 1.65, whiteSpace: "pre-wrap" }}>{tips.body}</p>

              <h3 style={{ margin: "0 0 6px", font: `600 12px/1.4 ${FONT}`, color: "var(--dsw-accent, #0a84ff)" }}>
                详解流程
              </h3>
              {tips.steps.length === 0 ? (
                <p style={{ margin: 0, fontSize: 12.5, opacity: 0.6 }}>
                  这颗还没拆出下一步：在面板里点开它，或者回到节点图点「+N」拆一颗。
                </p>
              ) : (
                <ol style={{ margin: 0, paddingLeft: 20, fontSize: 13, lineHeight: 1.8 }}>
                  {tips.steps.map((step) => (
                    <li key={step}>{step}</li>
                  ))}
                </ol>
              )}

              <div style={{ marginTop: 26, paddingTop: 14, borderTop: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.08))" }}>
                <h3 style={{ margin: "0 0 8px", font: `600 12px/1.4 ${FONT}`, color: "var(--dsw-accent, #0a84ff)" }}>追问</h3>
                <div ref={listRef} data-dsh-work-chat-log="1" style={{ maxHeight: 220, overflowY: "auto", marginBottom: 10 }}>
                  {turns.length === 0 ? (
                    <p style={{ margin: 0, fontSize: 12.5, opacity: 0.6 }}>
                      这一颗还有哪儿没讲明白？直接问，答话只在这一页（不写进任务库）。
                    </p>
                  ) : (
                    turns.map((turn, index) => (
                      <div key={index} style={{ marginBottom: 12 }}>
                        <p style={{ margin: 0, fontSize: 12.5, opacity: 0.7 }}>你问：{turn.question}</p>
                        <p style={{ margin: "4px 0 0", fontSize: 13, lineHeight: 1.65, whiteSpace: "pre-wrap" }}>{turn.answer}</p>
                      </div>
                    ))
                  )}
                </div>
                {failure === null ? null : (
                  <p style={{ margin: "0 0 8px", fontSize: 12, color: "#c0392b" }}>{failure}</p>
                )}
                <div style={{ display: "flex", gap: 8, alignItems: "flex-end" }}>
                  <textarea
                    data-dsh-work-chat-input="1"
                    value={draft}
                    rows={2}
                    placeholder="问一句这颗节点的事；回车发送，Shift+回车换行"
                    onChange={(event) => setDraft(event.target.value)}
                    onKeyDown={(event) => {
                      if (event.key === "Enter" && !event.shiftKey) {
                        event.preventDefault();
                        void send();
                      }
                    }}
                    style={{
                      flex: "1 1 auto",
                      resize: "none",
                      padding: "8px 10px",
                      borderRadius: 10,
                      border: "0.5px solid var(--dsw-separator, rgba(0, 0, 0, 0.12))",
                      background: "transparent",
                      color: "inherit",
                      font: `500 13px/1.5 ${FONT}`,
                    }}
                  />
                  <button
                    type="button"
                    data-dsh-work-chat-send="1"
                    disabled={asking || draft.trim() === ""}
                    onClick={() => void send()}
                    style={{
                      height: 34,
                      padding: "0 16px",
                      borderRadius: 999,
                      border: "none",
                      background: asking || draft.trim() === "" ? "var(--dsw-separator, rgba(0, 0, 0, 0.12))" : "var(--dsw-accent, #0a84ff)",
                      color: "#fff",
                      font: `600 12.5px/1 ${FONT}`,
                      textDecoration: "none",
                      cursor: asking || draft.trim() === "" ? "default" : "pointer",
                    }}
                  >
                    {asking ? "问着呢…" : "发送"}
                  </button>
                </div>
              </div>
            </>
          )}
        </section>
      </div>
    </div>
  );
}
