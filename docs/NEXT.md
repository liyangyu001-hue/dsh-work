# 接力板（每轮开发循环只读这一个文件）

> 这个文件是**跨会话的唯一交接物**。任何一轮（人工或自动）开工前先读它，收工前改它。
> 项目是什么、每一层为什么存在、角色与工程规则，见 [项目说明](GUIDE.md)。
> 格式故意定得很死：改动只碰"进度"和"当前任务"，历史追加到最下面，不删旧行。

---

## 进度
- 2026-10-23 · 玻璃条加「打开节点图」第二入口（`e1f8191`）：用户原话「在dsh界面装个打开的按钮 不然不知道怎么打开」，而 `target:"graph"` 通道早就通了、只缺 UI。在「打开面板」右侧加一枚描边 ghost 按钮（图标＝两颗上游圆＋一颗下游圆），点一下直接开只读节点图；没有任务树/状态未到时置灰并把原因写进 title（原生 `ShowReviewDoc` 对空 doc 是静默 `return`，点了会像没反应）。两个入口共用 `openCompanion(target)`，四档反馈文案抽进 `packages/plugin/src/client/open-entry.ts`（6 个单测）。真机 1.5x：click 后 1.2s 小条浮出「节点图已打开」、伴随层被顺手拉起、审阅窗 2400×390 物理自动弹前台
- 2026-10-23 · 节点图上概念节点有了「讲讲它」入口：右下角原来的「学」换成可点 pill，点了先出「正在讲…」，模型讲完把正文 + 练习提示落成图区底部的卡片（Esc / 点空白收起，再点一颗换内容，窗口随卡片长高不盖节点）。宿主 `/api/learn` 回包补了 `view`（标题/正文/提示/是否支持），失败按「宿主没这个接口 / 没这个节点 / 模型没答上来 / 连不上宿主」分四档写清，没有接口时按钮置灰且不再发请求。顺手修了一处真 bug：审阅窗按键被 `WM_KEYDOWN` 与定时器轮询各投一遍，一次 Esc 会先收卡再收窗（240Hz 下两遍间隔 < 60ms），改成同键 220ms 去重（`8fb316f`）。真机 1.5x：点第 7 颗 6s 出卡（物理 2400×390 → 2400×522）、点卡外收起回 390、Esc 后窗口仍在（修复前 `vis=False`）
- 2026-10-22 · 节点图默认宽 1320 → **1600 DIP**（一长条一次看全 7 颗，全尺寸不缩），并加**屏幕工作区兜底**：工作区比 1600 DIP 窄时窗口退到「工作区宽 − 32 DIP」（下限 640），不再冲出屏幕。真机 1.5x（物理 3840×2160）验证：窗口 2400×390 物理 = 1600×260 DIP 居中，7 颗全尺寸；临时把常量抬到 2600 复验兜底落回 2528 DIP、两侧各 16 DIP（`62a10bd`）
- 2026-10-22 · 节点图跨行连线改成直角折线（出父节点先横一小段 → 竖直落到子那一行 → 横着进子节点，拐角 4 DIP 小圆角），流光改按折线累计弧长定位、拐角不掉速；方向标改落在最后那段横线的中段，同行相邻仍是直线（`eba985a`）。真机 1.5x DPI 验证：四条跨行边全是直角，竖线逐行量过 x 不变，流光在竖段上连续
- 2026-10-22 · DSH 玻璃条加「打开面板」按钮（点一下就能开桌面面板，没在跑时宿主顺手拉起伴随层）；宿主新增 `POST /dsh-work/api/ui/open`，指令随 `/api/state` 的 `ui` 字段下发给原生轮询（`eed4aed`）
- 2026-10-22 · 审阅窗改成传统连线节点图（横向流程 + 连线 + 运行中节点流光边框）；面板右列重排；面板可点进只读文档节点图
- 2026-10-22 · 学习模式第一版（自出题 / 自识别 / 自验证，packages/plugin/src/learning.ts）
- 2026-10-22 · 节点排版收敛：字号 16 DIP、框 200×64、窗口高度随内容长、两行标题同字级不切字（4701e74）
- 2026-10-21 · 解构审阅窗上线（草稿层 + 可反复拆 + 确认才落库）；并把非 Tab 的全局热键全部拆掉，面板改用可点的「完成」（`d402078`）

> **方向调整（2026-10-20）**：伴随层**全换 C++**，三步走——小条 → 面板 → 框选覆盖层。
> Electron 版（`apps/companion`）保留不删，随时可回退。原生层在 `native/`，取舍见 `native/README.md`。

- 阶段：**P2 · AI 在环**
- 原生层三件套全部跑通：小条 ✅ / 面板 ✅ / 框选覆盖层 ✅（440 KB，Electron 版 210 MB）
- **Tab 链路**：Tab → 全屏压暗 → 拖框 → 松手落盘 → 指针处弹四瓣轮盘
  （解构这块 / 继续框选 / 交给 AI / 直接收下）。Tab 是常驻全局热键，
  只有覆盖层开着的那几秒让给覆盖层自己。
- **解构闭环 ✅**：框选 → `POST /dsh-work/api/recognize` → 模型回 JSON →
  `plan.set` 落库 → 面板自动弹开显示新任务树。
- **建议流 ✅**：面板打开时后台请求 `POST /dsh-work/api/advise`，
  模型给一句建议 + 一条更优解，落在节点的 `advice` 挂件上；
  同一节点 60 秒内不重复请求。
- **交接 ✅**：轮盘第四项写 `handoff.offer`，面板最右列显示
  「已交给 AI / AI 正在做 / 已验收 / 已退回」。**只落意愿，不冒充执行**。
- **交接接棒 ✅（这一轮）**：不自己起会话，走「模型自己调工具」的协议——
  `work_status` 新增「已交给你（等你接手的节点）」段，把目标/验收和下一步动作
  直接写进模型看得见的文本里；新增工具 `work_handoff_start` 把 `offered → running`
  （写回 `agentSessionId`），做完照旧 `work_handoff_submit`。
- **说一件事 ✅（这一轮）**：`Ctrl+Alt+N` → 原生输入框（普通窗口 + EDIT，不是分层窗口）
  → `POST /dsh-work/api/plan-text` → 与识别**共用** `parseRecognizedPlan` + `plan.set`
  → 成功后刷新模型并自动摊开面板。真机：「把 README 补一张架构图」→ 8 个节点。
- **解构审阅窗 ✅（这一轮）**：解构不再「一次定终身」。模型拆完先落**草稿**
  （宿主侧 `DraftPlan`，不碰 `work.json`），原生弹审阅窗：纵向缩进树 + 右列详情，
  `Enter` 对选中项再拆一层、`Ctrl+Enter` 就这样、`Esc` 不要了。确认才
  `POST /api/plan/commit`；草稿过期或放弃则什么都不留。
- **下一轮从**（2026-10-22 收工时定的顺序）：
  1. 节点图：概念的极小「学」标记、徽标挪右上、连线加方向感、点已完成节点可重设为当前任务；
  2. 双击 Tab 语义（回到上一个节点 / 完成当前节点）与「图开着时小条认鼠标导致无限叠图」；
  3. 学习模式的「讲一下」入口接到节点图（现在只在面板里）。
  `ctx.schedule.create` 发 followup），让人点完「交给 AI」不用再去对话里催一句。
- 待办：审阅窗的「+N / −N」标记在展开后没画出来（`has_children` 在重建后丢了）；
  托盘与退出入口（现在只能 `Stop-Process -Name dshwork-shell`）；
  学习模式的侧栏 tips（用户明确说本轮不做）。

## 进度
- 2026-10-21 · 解构审阅窗上线（草稿层 + 可反复拆 + 确认才落库）；并把非 Tab 的全局热键全部拆掉，面板改用可点的「完成」（`d402078`）

- 解构闭环打通（原生 → 宿主 → 模型 → 任务树）：✅
  `Tab` 框选 → 松手弹四瓣轮盘 → 选「解构这块」→ 原生层把 PNG `POST /dsh-work/api/recognize`
  → 拿 jobId 每 800ms 轮询 → `done` 后小条提示「已拆成 N 个任务 · 标题」。
  真机两次：7 个任务（7s）/ 5 个任务（33s）。超时放宽到 150 秒（模型慢时 90 秒不够）。
  踩到的坑：后台线程把宽字符塞进 `std::string(w.begin(), w.end())` 会变乱码，改 `Narrow()` 转 UTF-8；
  识别结果用 `WM_APP+6` 回 UI 线程，后台线程不碰窗口。

- 切片一（原生小条）：✅ `4c06d87`
- 切片二（原生面板 + 托盘 + 全局热键 + 键盘钩子）：✅ `719779b`
- 切片二b（面板改叠放卡片、缩到 660x318）：✅ 见本提交

## 推送方式（重要，每天都用得上）

本机 github.com:443 不通、pi.github.com 通（Clash 没开时也是如此）。
所以 **git push 一定会失败**，改走 GitHub Git Data API 逐提交重放：

`
node scripts/push-via-api.mjs <远端当前 sha>
`

它会读 ~/.dsh/dsh-work/gh-token，把 <sha>..HEAD 的每个提交按原消息、原作者、原日期重建，
最后把 refs/heads/main 指过去。**远端 sha 与本地不同，但 tree 完全一致**（可用
git rev-parse HEAD^{tree} 与 GET /git/commits/<远端sha> 的 .tree.sha 对账）。
推完把远端新 sha 记在下面这行，下一轮用它当起点。

> 当前远端 sha：`78bb3f673c12a78d3bdb369d88efee6afd2d85a3`（2026-10-23 22:07，tree 与本地 `8ddcb8f` 一致 = `fa4b9b2`；本地在其之上只有 1 个回填本行的提交，下轮推送会一起带走）

## 当前任务（只做一个，做完就停）

- 阶段：**P2 · AI 在环**。交接入口已通：玻璃条上的「打开面板」（`eed4aed`）与「打开节点图」（`e1f8191`）。**当前任务 = N-4**（审阅窗展开后「+N / −N」徽标消失）。
- 规矩不变：一件事做完就停；需要拍板的设计问题写进「等用户决定」，不要卡在那里。

### N-1 节点图：连线从斜切贝塞尔改成直角折线 —— ✅ 已交付（`eba985a`）

**为什么**：跨行连线是一条斜着切的贝塞尔，两行之间谁连谁看不清；用户要「先出一小段 → 竖直落下 → 横向进下一颗」，流光沿折线跑。

**做法**：`native/shell/src/review.h` 新增「连线的几何」块——`EdgePath`（点 + 累计弧长 + 方向标落点）、`EdgePush/EdgeLine/EdgeCorner`、`BuildEdge`（同行 0.5 DIP 内走直线；跨行「短横 14 DIP → 竖 → 横」，二阶圆角 4 DIP）；`Draw` 的连线块改成 `BuildEdge` + 逐段画线，流光按 `edge.lengths/edge.total` 定位，方向标取 `edge.arrow`。

**验收**（全部真机过）
- [x] 同行相邻仍是直线；跨行是「短横 → 竖 → 横」，拐角直角（4 DIP 圆角）
- [x] 流光沿三段折线连续跑，拐角不回头、不跳帧（按弧长归一化，拐角只是路径上一点）
- [x] 线宽/颜色/流光亮度与之前逐字未改（1.6 / 1.9、白 0.22、accent 0.10~0.65）
- [x] `Stop-Process -Name dshwork-shell` → `node scripts/build-native.mjs dshwork-shell` → 真机截图（1.5x DPI）
- [x] `pnpm test` 92 绿、`pnpm typecheck` 干净

**留下的**：窄间距（≤ 8 DIP 或放不下两段圆角）时退回直连；N-2 加宽后已核对：`dx` 在内容坐标里是 `kGapX = 48`，缩放不参与，所以加宽不会把这条边推进「退回直连」。

### N-2 节点图默认宽度 1320 DIP → 1600，屏幕窄时回退 —— ✅ 已交付（`62a10bd`）

**为什么**：一长条是按行号横向排开的，1320 DIP 里只放得下 5 颗多一点；用户要「一长条一次能看见更多节点」。

**做法**
- `kWindowW` 1320 → 1600：一颗占 224 DIP，1600 正好装下 7 颗（7×224−48+2×26 = 1568）。
- `Model` 新增 `avail_w`（屏幕工作区能给的宽度，DIP；0 = 不设限）。`WindowWidth(m)` / `BodyWidth(m)` 都改走它：`avail_w` 有值且比 `kWindowW` 窄时用 `avail_w`。排版（`Layout`）、`FitView`、`ClampPan`、`CommitHit` 全从这两个函数取宽，所以自动一致。
- `main.cpp` 新增 `ReviewAvailWidthDip()`：`GetMonitorInfoW(MonitorFromWindow(g_window, …)).rcWork` 宽 ÷ `g_dpiScale` − 两侧各 16 DIP，下限 640。`BuildReviewModel` 在 `Layout/FitView` **之前**填一次（开窗时已经定了宽），`LayoutReview` 每次弹窗再重算一次（换显示器 / 改缩放也能落回可用区）。
- 含 `windows.h` 的 TU 里一律手写比较，没引入 `std::min/max/clamp`。

**验收**（全部真机过）
- [x] 默认宽度下同行一眼 7 颗（全尺寸，字号/间距/连线几何一字未动）
- [x] 屏幕不够宽时不出界：临时把 `kWindowW` 抬到 2600 重编，窗口落回 2528 DIP（= 2560 − 32），两侧各 16 DIP，居中不压任务栏
- [x] 150% DPI 复看（本机就是 1.5x：窗口 2400×390 物理 = 1600×260 DIP）
- [x] 先 `Stop-Process -Name dshwork-shell`，再 `node scripts/build-native.mjs dshwork-shell`，真机截图
- [x] `pnpm test` 92 绿、`pnpm typecheck` 干净

**留下的**：`avail_w` 只按 `g_window` 所在显示器的 `rcWork` 算；把审阅窗拖到另一块屏后不会立刻改宽（下次弹窗才认）。多显示器那轮再收。加宽不影响「窄间距退回直连」分支：`dx` 是内容坐标里的 `kGapX = 48`，缩放不参与。

### N-3 概念节点上的「讲讲它」入口 —— ✅ 已交付（`8fb316f`）

**目标**：概念节点给一个能点的入口，点了走宿主 `POST /dsh-work/api/learn`（`packages/plugin/src/learning.ts` 已通），把解释显示出来。

**结果**
- 入口：「学」→ 可点 pill「讲讲它」（46×17，accent + `fmt.meta`，与徽标同一套）；标题第二行让出 42 DIP 不再被压住；悬停高亮 + 手型光标。
- **先画再发**：点下去立刻切「讲解中…」再起后台线程，不在 HTTP 上等（模型要二三十秒）。
- 卡片：图区底部一条，标题 + 正文（折行到 2 行）+ 练习提示；窗口同步长高 88 DIP，不盖节点；`Esc` / 点卡片外收起并缩回原高。
- 失败四档文案，写在 `packages/plugin/src/lesson.ts`（纯函数 + 单测）：宿主没这个接口 / 没有这个节点 / 模型没答上来 / 连不上宿主。`unsupported` 时按钮置灰，之后不再发请求。
- 顺手修：审阅窗按键双投递，一次 Esc 连收卡带收窗（见「已知的坑」）。

**验收**
- [x] 概念节点有可点标记，与现有「学」/徽标同一套视觉，不新造一套
- [x] 点了先显示「正在讲…」，失败给可读原因；不阻塞面板其它交互
- [x] 接口 404 / 宿主不在时按钮不装作已通（置灰 + 说明）
- [x] 纯逻辑（状态机、文案）抽出来有单测；`pnpm test` 109 绿、`pnpm typecheck` 干净

**留下的**：讲解会写进节点 `advice`（宿主 `learn` 既有设计，直接覆盖旧建议）——要不要单独开一个「讲解」字段见「等用户决定」。卡片正文固定 2 行，超长靠省略号；想读全只能再点一次让模型讲短些（提示词已限 80 字）。

### 插入任务（2026-10-23 用户拍板）：玻璃条「打开节点图」第二入口 —— ✅ 已交付（`e1f8191`）

**为什么**：用户原话「在dsh界面装个打开的按钮 不然不知道怎么打开」。原来玻璃条只有一枚「打开面板」，节点图要先进面板再点当前任务，两跳、没人猜得到；而 `POST /dsh-work/api/ui/open` 早就认 `target:"graph"`（`eed4aed` 留的通道），缺的只是 UI。

**做法**
- 玻璃条「打开面板」右侧加描边 ghost 按钮「打开节点图」（`data-dsh-work-graph="1"`），图标＝两颗上游圆 + 一颗下游圆 + 连线；实心=主入口、描边=第二入口，不抢层级。玻璃条宽度**不动**（仍 `min(560px, 64vw)`）。
- 置灰规则：`nodeCount === null` → 「正在读取任务状态…」；`0` → 「先在面板里拆一次任务」；`>0` → 可点，hint 「打开已落库文档的节点图（只读）」。原因写进 `title` / `aria-label`——原生 `ShowReviewDoc` 对空 doc 是静默 `return`，不拦就会像「点了没反应」。
- 入口合并：`openPanel()` → `openCompanion(target: OpenTarget)`，两个按钮共用；`open-entry.ts` 里 `graphEntry()` / `openFeedback()` 是纯函数（6 个单测），失败优先用宿主 hint。

**验收**（全部真机过）
- [x] `node scripts/build-plugin.mjs` + `node scripts/build-client.mjs`，按端口杀掉 64001 监听进程，用 `dsh.cmd --profile dshwork-dev --port 64001 --no-open` 在 `D:\work\AI\work` 重启（token 变了，从 `dev-out.log` 重新取）
- [x] 真机截图 1440×900：两枚按钮并排、节点图按钮可点（`disabled=false`，title 为「打开已落库文档的节点图（只读）」）
- [x] Playwright 真点：1.2s 后小条浮出「节点图已打开」，伴随层进程被拉起；审阅窗 `DSHWorkNativeReview` 可见、物理 2400×390 = 1600×260 DIP，7 颗节点全在（截图在仓库外 `dshwork-shots\ui-entry-v2*.png`）
- [x] `pnpm test` 115 绿、`pnpm typecheck` 干净

**留下的**：`opening` 是两枚按钮共用的单标志，点一个时另一个也会短暂变淡（一起 1~2s，可接受，真机没看出来）；置灰态只靠 `disabled` + `opacity .55`，没做专门的「空任务树」截图（切空工作区会动用户数据，没做）。

---

## 队列（做完一个换一个，不要并行）

- 多显示器时 `avail_w` 不跟着窗口所在屏变（只在弹窗时算一次）—— **下一轮**（N-4 交付后接这件）
- 托盘与退出入口（现在只能 `Stop-Process -Name dshwork-shell`）
- N-4 审阅窗「+N / −N」徽标随重建丢失 → **已提到「当前任务」**
- 已交付：N-1 直角折线（`eba985a`）、N-2 加宽（`62a10bd`）、N-3 讲讲它（`8fb316f`）、插入任务「打开节点图」（`e1f8191`）；入口按钮（`eed4aed`）——复核结论见各自「留下的」

---

## 等用户决定

- `kMinFitZoom` 那句「自动适配也不缩小」目前是死的（见「已知的坑」）。恢复它 = 窄窗口里字不再被缩小、内容超出就靠人拖/滚；不恢复 = 现在这样自动缩到装下。要不要恢复？先不动，N-2 加宽只是让它不那么常触发。
- **「讲讲它」的讲解会覆盖节点原有的 `advice`**（宿主 `learn` 直接 `node.advise` 落库，这是它有意的设计：面板本来就渲染这个字段）。要不要给讲解单独一个字段、与「建议」并存？先不动，等你拍。

---
## 硬约束（每一轮都必须遵守）

1. **测试与类型检查必须绿**才允许提交：`pnpm test`（39+ 用例）、`pnpm typecheck`。
2. **推送方式**（无 gh CLI，用 PAT）：
   ```powershell
   $token = (Get-Content "C:\Users\25108\.dsh\dsh-work\gh-token" -Raw).Trim()
   $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("x-access-token:$token"))
   git -c http.extraHeader="AUTHORIZATION: basic $b64" push https://github.com/liyangyu001-hue/dsh-work.git HEAD:main
   ```
   只允许 fast-forward；**绝不允许 force push**。唯一例外：清理误提交的隐私内容，且必须在提交信息里写清为什么。
3. **提交信息**写"改了什么、为什么"，中文正文，一行标题 + 空行 + 要点。
4. **不要新建"临时验证脚本"**留在地上；验证完删掉，或写进 `scripts/` 并说明用途。
5. **`docs/assets/` 只准放 `scripts/make-shots.mjs` 生成的合成图**——仓库是公开的，桌面截图会带出别人窗口、标签页、账号。`pnpm test` 里有白名单守卫拦着。
6. 起 dev server 前先按端口杀旧进程，否则 `dsh` 会静默起不来：
   `Get-NetTCPConnection -LocalPort 64001 -State Listen | % { Stop-Process -Id $_.OwningProcess -Force }`
7. 改完客户端必须 `pnpm build`，否则浏览器里看到的还是旧 bundle。
8. 每轮结束时，在 `docs/CHANGELOG.md` 追加一行（日期 + 本轮做了什么）。

---

## 已知的坑（踩过一次就别再踩）

- 工具的 `output.schema` 会校验返回值；schema 是 string 就必须 `return string`。
- 工具 `parameters` 不是 JSON Schema，`required` 只能写在属性节点里，写进 `items` 会报错。
- 读未 `inject` 的 cordis 服务会抛 `cannot get property "x" without inject`，可选服务用 `ctx.get('x')`。
- 第三方插件想 import `@deepseek-ai/dsh-tools`，必须在自身 package.json 声明 peerDependency。
- pnpm 11 不再读 `.npmrc` 里的 pnpm 专属设置，要写进 `pnpm-workspace.yaml`。
- 伴随层启动前必须 `Remove-Item Env:ELECTRON_RUN_AS_NODE`，否则 Electron 静默退化成 Node。
- 伴随层测试跑在 Node 里，`electron` 模块不可 require；窗口/热键相关只能靠逻辑抽离到纯模块再测。
- `.cjs` 被 `.ts` 测试 import 时，typecheck 需要同名 `*.d.cts` 声明（`capture-geometry.d.cts` 即样板）；**实现与声明必须同步改**，不一致会当场在 typecheck 里报出来。
- 回调放在"收摊函数"里调用 = 静默失效：`finish()` 会清空 listeners，必须先取出再调用（P1-2 踩过，截完图一声不响）。
- IPC 初始化数据一律"页面主动来要"（`xxx:ready` → `xxx:init`），别在主进程 push：页面脚本在 `loadFile` 解析完之前就会跑，push 会丢。
- `transparent: true` 的窗口里，`pathToFileURL()` 才是合法的 `file://` 三斜杠形式；手拼 `file://C:/...` 会被当成主机名。
- 桌面截图与**窗口几何**都必须 DPI-aware（`SetProcessDpiAwarenessContext(-4)`）：非 DPI-aware 的 PowerShell/.NET 里 `Screen.AllScreens` 和 `GetWindowRect` 会报 2560×1440 这种**虚拟化后的假数**（本机真实物理 3840×2160、工作区 3840×2088、DPI 1.5），拿它算 DIP 会差 1.5 倍，还会误判「窗口跑出屏幕」。
- `nativeImage` 没有 `width`/`height` 属性，几何换算必须传 `getSize()`；传对象本体不会报错，只会得到一堆 NaN 和 0 字节 PNG。
- `store.test.ts` 偶发 `ENOTEMPTY ... rmdir` —— 临时目录清理与原子写（临时文件 600ms 内 rename）踩在同一拍上，重跑即过；真出现先重跑一次再判断是不是代码坏了。
- 视觉模型不是默认就收图：`agentDefaultModel` 若选的是纯文本模型，宿主会把图片投影成文字说明，识别质量会很差。要换 `deepseek-v4-flash-vision-exp` 或在插件 Config 里写 `llm.model`。
- **`focusable:false` 的窗口收不到任何 DOM 键盘事件**。要键盘可用只有两条路：抢焦点（那就会打断用户正在打的字），或者像 lolPing 那样在输入层把手势键吃掉。本机实测 `globalShortcut.register("Enter" / "Backspace" / "Tab" / "Escape" / "Right" / "Left")` 在 Windows 上 **6/6 全部成功**，这就是"打开期间独占、关掉立刻释放"方案的依据。
- **置顶窗口会被 Windows 悄悄降级**（lolPing issue #70）：每次 `show()` 之后都要重新 `setAlwaysOnTop(true,"screen-saver")`，否则面板过一会儿就跑到别的窗口后面。
- 面板既然不抢焦点，`blur` 事件永远不会来——**别用 blur 做"点外面就收起"**，那条路在 `focusable:false` 下是死的。收起只走 Esc / 热键 / 关闭按钮。
- **心跳不能看"请求带没带令牌"**：带伴随层令牌的 `POST /api/ui/open` 会先把自己标成在线，于是明明没进程也回 `companion:true`（本轮自己踩到的）。心跳只认 `/api/state` 轮询，见 `packages/plugin/src/index.ts` 的 `isStatePoll`。
- **改浏览器侧要重编客户端**：`scripts/build-plugin.mjs` 只打宿主 `lib/index.js`；按钮/小条在 `packages/plugin/src/client/`，必须再跑 `node scripts/build-client.mjs`（或 `pnpm build`），否则页面里还是旧 bundle。**esbuild 默认把中文转成 `\uXXXX`**，用 `Select-String` 搜中文会误判"没编进去"，搜 ASCII 的类名/属性更可靠。
- **dev 宿主的登录 token 每重启一次就变**：URL 从 `D:\work\AI\work\dshwork-shots\dev-out.log` 里 `dsh web: http://127.0.0.1:64001/?token=…` 那一行取；自动化截图必须带它，否则进的是登录页。
- 真机截图会被「预览版说明」欢迎弹窗挡住：Playwright 里先点「继续」再截，别以为页面没渲染。
- 宿主重启前必须按端口杀监听进程（`Get-NetTCPConnection -LocalPort 64001 -State Listen`），并确认工作目录是 `D:\work\AI\work`，否则任务为空；起完检查 `endpoint.json` 的 pid 是否变了。
- **验证原生界面不必重启真宿主**：把 `DSH_HOME` 指到临时目录（`<tmp>\dsh-work\endpoint.json` 写 `{"version":1,"url":"http://127.0.0.1:64077","token":"<够长>"}`）再起 `dshwork-shell.exe`，它会读这个 endpoint；本地起个 shim 服务 `/dsh-work/api/state` 就能拿假文档渲染**真窗口**。`ui.target` 必须配一个 15 秒内的 `ui.at`（epoch 毫秒）才会被当成「人刚点的按钮」，否则只更新小条不弹窗。
- **`FindWindow("DSHWorkNativeReview")` 可能返回 0**（跨进程 + DPI 虚拟化时踩到），改用 `EnumWindows` 按 pid 枚举再比类名；要展开节点不用抢焦点，`PostMessage(hwnd, WM_KEYDOWN, VK_DOWN/VK_RETURN, 0)` 直接投递即可。注意 `ShowReviewDoc` 会清空 `g_review_expanded`——文档节点图刚打开时**只有根节点**，得再按一次 Enter 展开才有跨行连线。
- **别用最近邻放大拼图判断线直不直**：3~4 倍 `NearestNeighbor` 重采样会让本来就直的竖线看起来错位 1 px（本轮差点误判成「折线有缝」）。量像素用 `GetPixel` 逐行扫，一次就能定性。
- `native/shell/src/review.h` 的 `kMinFitZoom` 是**假生效**：第 80 行注释里有一个字面 `\n`，把 `constexpr float kMinFitZoom = 1.0f;` 一起吞进注释；第 234 行的 `if (zoom < kMinFitZoom) …` 同样整行是注释。所以「自动适配也不缩小」其实没实现，内容宽时会缩到 1.0 以下（N-2 加宽后本机已不再缩；真要修会改变窄屏行为，见「等用户决定」）。同样的字面 `\n` 写法别再引入。
- **要验证「屏幕不够宽就收窄」必须临时把 `kWindowW` 抬到工作区宽（DIP）以上**：本机工作区 2560 DIP，1600 永远走不到回退分支，看着像根本没做。用 2600 那次实测落回 2528 DIP；**验完一定改回来重编**——真机跑的是最后一次编出的二进制，别把调试值留在仓库里。
- **审阅窗的按键会被投递两遍**：窗口有焦点时来 `WM_KEYDOWN`，动画定时器里的 `GetAsyncKeyState` 又观测到同一个按下沿；240Hz 下两遍间隔 < 60ms。N-3 截图验证时一次 Esc 直接把整张图关了（第一遍收讲解卡，第二遍看到卡已收起就收窗）。`ReviewKey` 里按「同一个键 220ms 内只认一次」去重；**不能删掉任一条路**——焦点不保证拿得到，轮询就是为这个留的。
- **原生层调宿主 API 要带 `x-dsh-work-token` 头**（值取 `~/.dsh/dsh-work/endpoint.json` 的 `token`），不是 `Authorization: Bearer`；带错会回 401。`POST /dsh-work/api/ui/open` 的 `target` 只认 `panel` / `graph`，写 `review` 回 400。
- **本机 `dsh` 不在 PATH 里**：重启 dev 宿主用 `C:\Users\25108\AppData\Local\Programs\DeepSeek Harness\resources\runtime\cli\bin\dsh.cmd --profile dshwork-dev --port 64001 --no-open`，工作目录必须是 `D:\work\AI\work`。
- **`host.h` 的 `HttpPostJson` 现在能传 `receive_ms`**（默认仍是 30000，讲解那条传 60000 兜住 45s 的模型超时）：别把讲解的超时调回 30s 以下，模型慢时会被判「连不上」，看起来像接口坏了。
- **验证「讲讲它」别拿有 advice 的节点试**：讲解会覆盖该节点已有建议（见「等用户决定」）。真实数据里后两颗（制作重试检查清单 / 用自测讲清二者关系）advice 为空，拿它们截图。
- **玻璃条上的「打开节点图」必须自己判空**：原生 `ShowReviewDoc` 读到空 doc 是静默 `return`——不弹窗、不报错、小条也没提示，表现就是「点了没反应」。所以按钮按节点数置灰，`nodeCount===null`（状态还没到）也别当成 0。
- **点「打开节点图」会顺手把伴随层拉起来**（宿主 `spawn` 的是 `native/shell/build/dshwork-shell.exe`），随后桌面会多一个 TOPMOST 的审阅窗。验证完记得 `Stop-Process -Name dshwork-shell`，别把用户的桌面一直占着；顺带它还会把 `endpoint.json` 的 pid 写成自己。
- **Playwright 探针取提示胶囊要取全部 `[aria-live="polite"]`**：玻璃条标题也是 `role=status aria-live=polite`，`querySelector` 只拿得到第一个（标题），提示胶囊是第二个、2.8s 后自己消失；`querySelectorAll` 再挑最后一句才靠得住。

---

## 历史

- 2026-10-19 P0 完成：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条，推送 `84ff6db`。
- 2026-10-19 P1-1 完成：全局热键可配置（默认 `Control+Alt+Space`，裸 Tab 需 `allowBareTab`），注册失败/异常留痕，按下小条压缩 120ms 并记一行，提交 `4870b3f`；测试 46 绿，typecheck 绿，实机热键触发与占用失败告警均已验证。
- 2026-10-19 P1-2 完成：热键 → 冻结整屏（先藏小条再截）→ 全屏半灰覆盖层拖框 → 区域 PNG 落 `~/.dsh/dsh-work/captures/`；Esc/右键/再按热键取消；小条回弹并给 2.4s "已截取 W × H" 提示。150% 缩放下实测 800×600 物理拖拽 → 800×600 像素，零偏移；`capture-geometry.cjs` 9 个纯函数用例，全套 55 绿。

- 2026-10-19 P1-2b 完成：**面板**落地。热键语义从"按下即截图"改为"按下开面板"——小条是收起态、面板是展开态，同一个东西的两个形态；面板里做任务卡带（当前居中、左右为已完成/待进行，`scroll-snap` 居中）、详情列（目标/验收）、页脚键位提示，键盘 `Enter` 完成并前进、`Backspace` 退一步、`Esc` 收起、`Tab` 进框选，框选结束/取消后按原样恢复面板。玻璃底改用"抓一张背后桌面 → CSS 模糊"的方案（`backdrop.cjs`）：CSS `backdrop-filter` 对透明窗口背后的桌面无效，Win11 `acrylic` 又会吃掉圆角。`plan.set` 新增可选 `title`，拆解时顺带给文档命名。

- 2026-10-19 P1-3 完成：**视觉识别**落地，提交 `7e53a78`。链路 `panel → main.cjs → POST /dsh-work/api/recognize → 宿主 ctx.llm.stream → plan.set`；作业化（POST 只受理、GET 轮询取结果）是为了不让几十秒的模型调用挂在同步响应上，超时/卸载用 `AbortController` 真停。解析是纯函数：失败一次「就地修格式」重问一次，仍失败返回可读原因且不写文档；`looksLikeOutline` 挡住散文被误认成任务；`kind:'concept'` 走同一解析路径。模型选择 插件 Config → `agentDefaultModel` → `deepseek-official/deepseek-v4-flash`。新增 `recognize.ts`、`recognize.test.ts`、`http.test.ts`，测试 73 绿、typecheck 绿、build 通过。**遗留**：真机端到端没跑——跑着的宿主加载的还是旧 bundle（`/api/recognize` 返回 404），重启 `dsh web` 后补验；`attachments.admitPromptContent` 与视觉模型的实际输出质量也未实测。
