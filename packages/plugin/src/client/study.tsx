/**
 * 网页侧的「学习页」。
 *
 * 和原生节点图右侧那栏是同一件事的两种场景：
 * - 站在图里（原生）：右栏跟着选中的节点走，边看图边读；
 * - 坐在网页前（这里）：左边一列要学的东西，右边这一颗的基础知识与详解流程。
 *
 * 数据全走已有的 `/dsh-work/api/state` 轮询（父组件传 doc 进来），
 * 不另开接口：这一页是「读」，不需要跟随伴层，也不需要宿主额外配合。
 */
import { studyRows, studyProgress, studyTips, statusLabel, type StudyDocLike } from "./study-model.ts";

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
  const progress = studyProgress(rows);
  // 没选中过就落到当前任务，再不行落到第一步 —— 打开就有内容，不摆空壳
  const first = props.doc.pointer ?? rows[0]?.id ?? null;
  const active = props.selected ?? first;
  const tips = studyTips(props.doc, active);

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
          {rows.length === 0 ? (
            <p style={{ margin: "12px 10px", fontSize: 12, opacity: 0.6 }}>
              还没有任务树：先在面板里拆一次任务，这里就会长出要学的东西。
            </p>
          ) : (
            rows.map((row) => {
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
              <p style={{ margin: "0 0 18px", fontSize: 12, opacity: 0.55 }}>{tips.state}</p>

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
            </>
          )}
        </section>
      </div>
    </div>
  );
}
