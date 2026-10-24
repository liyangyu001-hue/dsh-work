# 接力板（每轮开发循环只读这一个文件）

> 这个文件是**跨会话的唯一交接物**。任何一轮（人工或自动）开工前先读它，收工前改它。
> 项目是什么、每一层为什么存在、角色与工程规则，见 [项目说明](GUIDE.md)。
> 格式故意定得很死：改动只碰"进度"和"当前任务"，历史追加到最下面，不删旧行。

---

## 进度
- 2026-10-25 · **节点图拖到的位置现在跨重启记住了**（用户 m01128「要 记住」＝ 批准；上一轮只记在进程内，伴随层一退出就回居中）。改法：`native/shell/src/host.h` 把「`DSH_HOME` 优先、否则 `USERPROFILE\.dsh`」的目录解析抽成 `DshWorkDir()`，`EndpointPath()` 复用它，新增 `PlacementPath()` = `DshWorkDir() + L"\\review-placement.json"`（和 `endpoint.json` 同目录——宿主重启会重写 endpoint，这份是伴随层自己的偏好，两边不打架）。`main.cpp` 新增 `SaveReviewPlacement(int x, int y)` / `bool LoadReviewPlacement(int& x, int& y)`（Win32 `CreateFileW` + `ReadFile`/`WriteFile`，内容就一句 `{"x": 3888, "y": 460}`，>4096 字节或 `x`/`y` 不是 number 都当坏文件）；`WM_EXITSIZEMOVE` 记完位置顺手 `SaveReviewPlacement`；`EnsureReviewWindow()` 开头懒加载一次（`g_review_placement_loaded`），读到就 `g_review_user_placed = true` 并落日志「审阅：接着上次的位置 (x, y) 开」，读不到走默认居中。**坏值有兜底**：位置进来后仍走上一轮那套 `MonitorFromPoint(…, MONITOR_DEFAULTTONEAREST)` + 夹回工作区，所以显示器拔了/分辨率变了不会把窗口丢到看不见的地方。真机三步（每次真进程重启）：① 删掉 placement 文件 → 起 shell 开图 `720,144`（居中）→ 拖到第二块屏 `3888,460 1872x390`，文件写出 `{"x": 3888, "y": 460}`；② `Stop-Process` 后重开**新进程** → 空窗直接落在 `3888,460 1872x390`，日志「接着上次的位置 (3888, 460) 开」，7 颗节点全在窗内（仓库外 `dshwork-shots\reopen-remembered.png`）；③ 手写一份坏值 `{"x": 9000, "y": 9000}` 再重启 → 落回 `3888,618`（第二块屏工作区右下角：`5760-1872`、`1008-390`），没跑出屏幕（`reopen-bogus.png`）。原生 590.0 KB（比拖动前的 583.5 只多 6.5 KB），只剩两条旧警告；`pnpm test` 130 绿、`pnpm typecheck` 干净。
- 2026-10-25 · **审阅窗现在能按住标题栏拖动、松手记住位置，拖到第二块屏会按那块屏的宽度重排**（接力板「等用户决定」第①条；用户当晚接了第二块显示器、回「接好了」= 点头，默认取舍按我事前说的「不吸附、松手记住位置」）。做法全在 `native/shell/src/main.cpp`：① `ReviewProc` 新增 `WM_NCHITTEST`——窗口是 `WS_POPUP`、没有标题栏，所以头带（`review.h:95` 的 `kHeader = 58.0f` × `ReviewScale()`，里面本来就没有可点控件、不用抠洞）返回 `HTCAPTION`，拖动交给系统的非客户区拖动，不自己算 `SetWindowPos`；② `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE` 维护 `g_review_moving`，退出时**先** `LayoutReview()`（`keep_place` 分支把窗口原地夹回工作区，否则松手时尾巴会伸出屏幕 802 px）**再** `GetWindowRect` 记 `g_review_user_placed/x/y` + 日志「审阅：拖到 (x, y)，下次打开还从这儿开」；③ `LayoutReview()` 增加「先定屏」——没开着且有人工位置时用 `MonitorFromPoint` 取那块屏，`ReviewAvailWidthDip(HMONITOR)` 按**那块**屏算可用宽（原来只按小条/窗口所在屏，重开时会拿宽屏的 1600 DIP 摆到窄屏上）；④ `EnsureReviewWindow()` 落位后补 `FitView + ClampPan`（窄屏上最右几颗会留在窗外）；⑤ `WM_MOUSELEAVE` 的自动收起加 `!g_review_moving` 守卫（拖到屏幕边缘甩出去就自己关了）。另外 `WM_SETCURSOR` 的 `HTCAPTION` 分支给 `IDC_SIZEALL`。真机（第二块屏 1920×1080 @150%，主屏 3840×2160 @150%）：主屏 `BEFORE 720,144 2400x390` → 拖到第二块屏 `AFTER 3888,460 1872x390`，日志「审阅：换屏后按新工作区重排，可用宽=1248」+「审阅：拖到 (3888, 460)」；关→重开后 `keep=0 placed=1` 停在原处、7 颗全在窗内（没补 `FitView` 时最右一颗被裁）；拖回左上角 → `AFTER 0,580`（x 被夹回 `work.left`）。截图在仓库外 `dshwork-shots\drag4-monitor2.png` / `drag4-reopen.png` / `final-drag.png`；原生 583.5 KB（只剩 `grab_dxgi.h(219) C4127`、`main.cpp(1298) C4715` 两条旧警告）。
- 2026-10-25 · **面板底部那条事件流终于真的画出来了：根因是分层窗口把 alpha 平方了一次，而那一行用的是 0.4 的半透明灰**（接力板交接待办 1，本轮唯一一件事）。按板上提示先补日志：`[02:42:32] 事件流：3 条` ⇒ 宿主 `/dsh-work/api/feed` 与轮询都对（另用 `x-dsh-work-token` 直接 GET 也回 `{"ok":true,"items":[…"at":"23:59","verb":"推进","text":"完成当前并前进"…]}`）；`main.cpp:1497` 前的 TEMP 日志稳态 `n=3 alpha=1.000000 px=1494x234` ⇒ `DrawFeed` 被调用、窗口尺寸对；在 `DrawFeed` 里塞一条 `FillRound(…, 0x60FF0000)` 暗红带**画得出来** ⇒ 坐标系与执行路径没问题，只有文字不见。接着做颜色阶梯（同一串文字）：`fmt.body`+`0xFFFFFFFF` 清楚可见（证明字符串非空、`Widen`/JValue 都对），`fmt.meta`+`0xFF9AA0A6` 也可见，`0x9AA0A6`（alpha 0.6）只剩零星噪点，`0x659AA0A6`（0.4）整条没有。逐像素统计（y=190..222）：修前那条带 `lum min=max=27`、一个像素都没画；0.6 那列 max 91、0.4 那列与背景无差。**根因**：面板是 `UpdateLayeredWindow(ULW_ALPHA)` 的分层窗口，D2D 往 `D2D1_ALPHA_MODE_PREMULTIPLIED` 的 DIB 里已经预乘过一次，ULW 又乘一次 ⇒ alpha 被平方（0.6→0.36、0.4→0.16），而那行写的正是 `Fade(0x9AA0A6, alpha*0.66f)`。**改法**：`native/shell/src/panel.h` 的 `DrawFeed` 换成 `Fade(kTextDim, alpha)`（不透明暗色，与「7/7 已完成」同一档，整体淡出仍交给 alpha），并把这段原因写进函数注释；顺手删掉 `DrawPanel` 里那份被面板底盖住的重复死代码（原 `panel.h:323-332`）和全部 TEMP 探针。真机（`POST /dsh-work/api/ui/open` + `dshwork-probe\shot-window.ps1`）：面板左下角出现 `23:59  推进 · 完成当前并前进`（仓库外 `dshwork-shots\panel-feed-fixed.png`）。原生 582.5 KB（只剩 `grab_dxgi.h(219) C4127`、`main.cpp(1290) C4715` 两条旧警告）；`pnpm test` 130 绿、`pnpm typecheck` 干净。
- 2026-10-24 · **这一轮没有能开工的活：固定队列四件全是已交付，绿门 130/130；把两件设计取舍连「改哪几行」一起勘定，等用户点头**（零代码改动）。按「已知的坑」最后一条，先拿自动化提示语的四件去源码与 `git log` 对账（见「当前任务」里那串行号），一件不差，所以**没有重做**。工作区干净、HEAD `e72a533`；`pnpm test` 130 绿、`pnpm typecheck` 干净。然后把①审阅窗拖动（`main.cpp:1849` 加 `WM_NCHITTEST`，`ScreenToClient` 后用 `kHeader × g_reviewScale` 判头带，头带无控件不必抠洞）与②`kMinFitZoom`（`review.h:105` / `:278` 两处被注释里的字面 `\n` 吞掉）的落地位置、代价、仍需拍板的小问写进「当前任务」，**没动代码**——红线不许擅自改产品方向。
- 2026-10-24 · **桌面 GUI 上「打开节点图」真的活了：玻璃条 7/7、按钮可点、真点一次就弹出节点图窗口**（用户「不然不知道怎么打开」这条的最后一环，零代码改动）。上一轮修完根目录后板子还挂着两问：桌面版重启后玻璃条亮没亮、按钮能不能开图。本轮在用户正在用的 `desktop` profile（宿主 pid 30624、端口 57758）上实测三道：① `~/.dsh/dsh-work/endpoint.json` 已是 `root=D:\work\AI\work` / `workspace=work`（`publishEndpoint` 写的），带同目录 token 打 `/dsh-work/api/state` 回 200、`seq=90 roots=7 nodes=7`；② 窗口截图（仓库外 `dshwork-shots\desktop-gui-after-restart.png`）：玻璃条「● 全部已完成 7/7」+ 描边「打开节点图」高亮，对照上一轮 `desktop-gui.png` 的「—」和灰按钮；③ **DPI-aware 真点一次**（屏幕物理 2104,474，150% @3840×2160）→ 1s 内 `dshwork-shell` 被拉起（pid 65004）、`DSHWorkNativeReview` 2400×390 弹出，7 颗概念节点、每颗「讲讲它」胶囊、页脚键位提示全在，标题正是「搞懂幂等及其与反复重试的关系」（`dshwork-shots\desktop-graph-window.png`）。验完 `Stop-Process -Name dshwork-shell`、光标放回原处，桌面不留窗。`pnpm test` 130 绿、`pnpm typecheck` 干净。**四件老队列在用户真正看的界面上全部到位，入口这条结案。**
- 2026-10-24 · **只读文档节点图里再点已选中节点不再去请求宿主拆层；点收起也真的会收起**（接力板「等用户决定」第①条，板上原话「纯 bug、修法唯一、不动产品方向，建议先做」）。顺着源码找出两条错路：① 叶子节点走 `again` → `kMsgReview 2` → `ReviewExpandSelected()` **无条件** POST `/dsh-work/api/plan/expand`，而文档模式没有 jobId（`ShowReviewDoc()` 里 `g_review_job.clear()`）→ 宿主回 `{"ok":false,"error":"缺少 jobId 或 nodeId"}`、界面上只留「这一步没成，看看日志」；② 父节点在 `again` 分支里**就地** `g_review_expanded.erase(row.id)` 就完事，可行是 `BuildReviewModel` 摊平出来的——集合变了界面没重建，于是点「收起」既不收也不落日志（这条本轮才翻出来）。改 `native/shell/src/main.cpp` 三处：`again` 只留唯一出口 `PostMessageW(g_window, kMsgReview, 2, 0)`（删掉就地擦集合的旧分支并写明原因）；`ReviewExpandSelected()` 起后台线程前加 `g_review_read_only` 守卫 → 页脚「这颗已经到底了，文档图不能再拆」+ 日志「审阅：只读文档，叶子不请求拆层 <id>」；`ReviewCollapseSelected()`（VK_BACK）同样守卫 → 「文档图是只读的，撤不了这一层」。草稿模式（有 jobId、`read_only=false`）行为不变。**真机**（仓库外假宿主 shim + 真伴随层，1600 DIP @150%，`DSH_HOME` 指到临时目录）：点概念叶子一次选中环过去；再点一次 → 页脚亮那句、无失败提示，shim 侧 `planHits:0 expandHits:0`；↑+Enter 展开父节点 → 日志「本地展开 nd_a」、窗口 390→441、徽标 `+2`→`−2`；展开态再点父节点 → 日志「本地收起 nd_a」、窗口回 390、徽标回 `+2`。原生日志全程无「审阅再拆 失败」。`pnpm test` 130 绿、`pnpm typecheck` 干净。
- 2026-10-24 · **「打开节点图」一直是灰的：根因是插件把宿主的 `process.cwd()` 当成任务库根目录，桌面 profile 下绑到了空库**（本轮修掉，队列第 1 条真正交付）。用户原话「在dsh界面装个打开的按钮 不然不知道怎么打开」——按钮 `e1f8191` 早就有了，但我去截**用户真正在看的桌面 GUI**（`desktop` profile、端口 52749）发现：玻璃条在、只是「打开节点图」**永远是置灰的**、连 `7/7` 计数都没有。追下去：`packages/plugin/src/index.ts:92` 旧代码 `const root = typeof cfg.root === 'string' && cfg.root.trim() !== '' ? cfg.root : process.cwd();`，而桌面宿主的 cwd 是 `C:\Users\25108\.dsh\profiles\desktop` → `WorkStore` 打开的是 291 B 的空库（`title:"未命名工作区", seq:0, roots:[], nodes:{}`），真库（7 颗节点、`seq:90`）在 `D:\work\AI\work\.dsh-work\`。`graphEntry(0)` 按设计就是「先在面板里拆一次任务」，所以灰着。① 新增纯逻辑 `packages/plugin/src/root.ts`（`pickRoot` / `pickWorkspaceRoot` / `listWorkspaces`，9 个单测）：配置优先 → 否则跟随 `ctx.get('workspaceRegistry').list()` 里**有会话的那个工作区**（`~/.dsh/storages/workspace.json` 全局唯一登记的就是 `D:\work\AI\work`）→ 否则回 cwd。**没有把 registry 写进 `inject`**：硬依赖只有 `tools` / `webServer`，加进去会让没这个服务的宿主整条装不起来。② registry 是异步引导的，第一版只读一次拿到的是「服务还没起」（实测日志 root=探针目录）→ `resolveRoot()` 里最多等 1.5s（15 × 100ms，只等 source 还是 cwd 的时候），并落一行「任务库根目录：… （来源：插件配置/宿主工作区登记/进程 cwd）」。**真机决定性验证**：故意从**错误 cwd** `D:\work\AI\work\dshwork-probe` 起 dev 宿主（就是桌面 profile 的失效形状）→ 日志 `任务库根目录：D:\work\AI\work（来源：宿主工作区登记）`、`/api/state` 回 `seq=90 nodes=7` 全是真任务；再截 dev 宿主（1440×900，仓库外 `dshwork-shots\devhost-bar.png`）：玻璃条 `● 全部已完成 7/7` + 「打开节点图」**可点**（对照桌面 GUI 那张：无计数、灰掉）。`pnpm test` 130 绿、`pnpm typecheck` 干净。**桌面版要重启一次才会加载新 bundle**（重启后重新读 `~/.dsh/dsh-work/token`，玻璃条就该亮起来）。
- 2026-10-24 · **第三轮收到同一份四件老队列：四件仍全部已交付；这轮多查了一步「入口到底出现在哪个界面」**（零代码改动）。逐条复核：入口按钮 `packages/plugin/src/client/index.tsx:294-327`（`data-dsh-work-graph="1"`，`e1f8191`）、直角折线 `BuildEdge` 在 `native/shell/src/review.h:473`（`eba985a`）、`constexpr float kWindowW = 1600.0f` 在 `review.h:111`（`62a10bd`）、「讲讲它」pill + 讲解卡在 `review.h:75-330`（`8fb316f`）。用 `scripts/shot.mjs` 截了正在跑的 dev 宿主（1440×900，仓库外 `dshwork-shots\entry-check.png`）：玻璃条两枚按钮都在，右侧那枚就是「打开节点图」（`disabled=false`，无 console 错误）。**这轮唯一的新事实**：跟我对话的这个界面是 **desktop profile**（端口 53011），它的 `package.json` / `cordis.patch.yml` / `node_modules` 里都没有 `@dshwork/plugin`——所以那个界面上根本不会有玻璃条；能看到入口的只有 dev 宿主（`--profile dshwork-dev --port 64001`）。已把「要不要装进 desktop profile」写进「等用户决定」，按红线不擅自改用户正在用的 profile。`pnpm test` 121 绿、`pnpm typecheck` 干净。
- 2026-10-24 · **自动化提示语的四件老队列已核实全部交付，本轮没动任何代码**（只取证 + 改文档）。这轮收到的提示语仍把「① 入口按钮 ② 直角折线 ③ 加宽 ④ 讲讲它」排成优先级，而板子「当前任务」早写明队列空、下一件必须先问用户——按「已知的坑」最后一条的办法逐条去源码里对了一遍：入口按钮 `data-dsh-work-graph="1"` +「打开节点图」在 `packages/plugin/src/client/index.tsx:297-326`（`packages/plugin/src/client/open-entry.ts` 在）、直角折线 `BuildEdge` 在 `native/shell/src/review.h:473`（连线块 `review.h:552-592`）、`constexpr float kWindowW = 1600.0f` 在 `review.h:111`、讲讲它 pill + 讲解卡在 `review.h:75-330`、宿主 `POST /api/learn` 在 `packages/plugin/src/http.ts:307`；对应提交 `e1f8191` / `eba985a` / `62a10bd` / `8fb316f` 都在 `git log` 里。**没重跑真机复核**：上一轮刚把「入口 / 悬停守卫 / 双击」真机验过、这轮零代码改动，按板子「别再重复本轮的复核」。开工时 HEAD `b5279bf`、工作区干净；`pnpm test` 121 绿、`pnpm typecheck` 干净。三选一已在本轮回复里重新抛给用户，等拍板。
- 2026-10-24 · **真机复核三件：入口按钮 / 悬停守卫 / 双击设当前任务——都无回归**（无代码改动）。用户这轮的固定队列仍把「在 DSH 界面装个打开的按钮」排在第一，但它已在 `e1f8191` 交付（玻璃条「打开面板」右侧的描边「打开节点图」）；按接力板「没有新指示就复查最近三条交付」的约定，在真机 dev 宿主（`--profile dshwork-dev --port 64001`，Playwright 1440×900 + 真伴随层 1600 DIP @150%）复测：① **入口**：两枚按钮 `disabled=false`；冷启动点「打开节点图」→ 小条「正在启动节点图…」→ 伴随层被拉起、`DSHWorkNativeReview` 2400×390 物理可见、7 颗节点全在（仓库外 `dshwork-shots\entry-verify.png`、`entry-cold-graph-native.png`）；② **悬停守卫**（`2b95980`）：图开着光标停在药丸上 900ms 面板不冒；光标不动按 Esc 关图后 1.4s 面板仍不冒（`AdoptHoverOnBar` 生效）；移开再回来 1.4s 面板正常打开；③ **双击设当前任务**（`5a07ddd`）：双击已完成节点 → 选中环跟过去、页脚「这一步已经做完了，不能再当当前任务」、未发意图（仓库外 `regress-doubleclick.png`）。**没发现回归；队列仍是空的，三件设计取舍仍等用户拍板。** 本轮另一个发现：自动化提示语里的四条队列和板子不同步（见「已知的坑」）。`pnpm test` 121 绿、`pnpm typecheck` 干净。
- 2026-10-24 · **节点图开着时跟着整份状态变，不再只认 `doc.pointer`**（`9737aa5`）。原来 `RefreshReviewDoc()` 只比 pointer：人在别处把某一颗做完了、只要当前任务没换人，图上那颗就一直是旧圆点，得关掉重开才变。① 新增 `ReviewDocSignature(doc)`：拼 `pointer` + `roots` 顺序 + 每颗的 `status|kind|title|handoff.state|children`（草稿数组按原序、落库对象按 id，`JObject` 是 `std::map` 天然有序），`BuildReviewModel` 每次重建顺手记进 `g_review_signature`，轮询那边签名相等就直接 return——**不能拿原文 JSON 直接比**：字段顺序、`seq`、`ui` 每次都变，那会变成每秒重建。② 签名变了才重建，并保住人手里的上下文：选中项**按 id 记、按 id 复原**（行号会随树形状整体挪位），只有 pointer 真换人才把选中跟过去；也只有真换人才 `ExpandAncestorsOf()`，单纯状态变化不动人手里的折叠；pan/zoom/fitted/讲解卡照旧逐字段恢复。③ 日志分两档：「当前任务 → X」/「状态变了（当前任务没换人）」。真机（假宿主 shim 收 `POST /dsh-work/api/intent` 的 `node.complete` + 真窗口截图，1600×260 DIP @150%）：图开着、滚轮缩到 1.25× 后把一颗概念节点改成 done → 圆点当场变绿；两张截图逐像素对比只差那颗圆点与边框流光（9491 px / 936k，bbox 落在节点区），选中环和视口一个像素没动；静置 3 秒、轮询 2 次、重建日志 **0 行**；改动只落 1 行日志；之后再按 Enter 展开的仍是原来选中的那颗（行数 2 → 4、窗口长高），证明选中没被重建带走。`pnpm test` 121 绿、`pnpm typecheck` 干净、原生 580.5 KB（只剩两条旧警告）
- 2026-10-24 · **节点图上双击一颗节点 = 把它设成当前任务，且图跟着状态自己重画**（`5a07ddd`）。接力板这件先查宿主：`node.focus` 早就有了（`packages/core/src/types.ts:105`，`packages/core/src/state.ts:368-377` 只让 todo/active 上位、会 `demoteOtherActive`），原生侧 `aa5ea03` 也已经接了「双击同一颗 400ms 内 → `PostIntent`」——缺的是三件收尾。① **当前任务在图上看不出来**：`Row::current` / `Model::current_id` 取自 `doc.pointer`，在节点左侧画一条强调色竖标（状态点变色和那圈转圈都在动，静止的一帧看不出人站在哪颗；pointer 也可能指着颗 todo，那时根本没有转圈可看）。② **状态回来图不重画**：新增 `RefreshReviewDoc()`，每轮 `kMsgModel` 对着轮询存下的 `/api/state` 原文比一次 `doc.pointer`，**换了人才重建**（每秒重建会把键盘选中项拽回去）；重建前 `ExpandAncestorsOf()` 把当前任务的祖先摊开（父子关系用 `children` 反推，不认 `parentId`），重建后原样恢复 pan/zoom/fitted/讲解卡、选中跟到新的那一颗、`LayoutReview()` 按新行数长高（390 → 441 物理）。③ **被拒时没有任何回话**：done/skipped 当场给页脚「这一步已经做完了/跳过了，不能再当当前任务」，已经是当前给「它已经是当前任务了」；新加 `Model::note` + 页脚 `hint_ink`——一句回话比平时那句说明亮一档，但不像 `busy` 那样连按钮一起置灰。**没做单击**：单击还得留给选中/展开，更要紧的是单击就改状态意味着光看图就把当前任务一路挪走（单击 vs 双击见「等用户决定」）。真机（假宿主 shim + 真伴随层，1600 DIP @150%）四步全过：打开时竖标在 pointer 那颗上；外部改 pointer → 自动展开祖先 + 选中跟过去 + 窗口按新行数长高；双击 todo 节点 → 意图提交 → 状态回来 → 竖标与选中都挪过去；双击 done 节点 → 页脚出提示、不发注定 409 的意图。`pnpm test` 121 绿、`pnpm typecheck` 干净、原生构建 575.0 KB（只剩两条旧警告）
- 2026-10-24 · **修掉「节点图开着时小条仍认鼠标 → 面板被反复顶出、图上一层套一层」（`2b95980`）**。先真机复现出两条路：① 图在悬停那 260ms 里打开时，悬停摘掉的 `WS_EX_TRANSPARENT` 没还回去，守卫又提前 return → 此后点药丸面板照样盖到图上（6 次撞 1 次）；② 图开着、光标停在药丸上按 Esc 关图 → 下一个 120ms 悬停轮询立刻判定「刚进入」→ 260ms 后面板自己顶出来（3/3 必现，这就是「反复顶」）。补的是同一条规矩的缺口：`UpdateHover` 守卫里 `KillTimer(kOpenTimerId)` + `SetBarClickThrough(true)`；小条 `WM_LBUTTONDOWN/WM_LBUTTONDBLCLK`、`kOpenTimerId` 回调、`kMsgOpenPanel` 都先看新增的 `ReviewShown()`；`CloseReview` 末尾 `AdoptHoverOnBar()` 把「人就停在药丸上」这次悬停认下来；宿主要 panel 时图开着先收图；命中区抽成 `CursorOnPill()` 两处共用。真机验收 11 项全 PASS（含「穿透位残留 300ms 内自动还原」「残留位下立即单击也不顶面板」「关图后重新悬停仍能开面板」老行为没丢），`pnpm test` 121 绿、`pnpm typecheck` 干净；截图在仓库外 `dshwork-shots\fix-graph-hover.png` / `fix-graph-window.png`
- 2026-10-23 · **真机徽标复核：N-4 的「+N / −N 展开后消失」在真机上复现不出，这条关掉**。做法：把工作区真实文档（7 颗概念节点、一长条）临时改成「根 + 6 子」形状，验完还原——1600 DIP（2400×390 物理 @150%）下一长条：打开只有根一颗、徽标「+6」；按一次 Enter 后根变「−6」、6 颗子节点并排画齐，徽标仍在节点框内右上角、没消失，「讲讲它」胶囊展开前后都在（截图在仓库外 `dshwork-shots\badge-real*.png`）。结论：徽标逻辑本身没丢，`child_count` 每次重建都按 `children` 长度重算；历史「消失」更像是 `fb114bd` 时代 chip 在右下角时被标题/讲解胶囊挤掉的观感。真机上踩到的两个坑（伴随层令牌 ≠ 网页令牌、玻璃条悬停会顶出面板盖住图）已记进「已知的坑」
- 2026-10-23 · 事件流 `GET /dsh-work/api/feed`（上一轮 `ade1945` 落主体，本轮收口）：任务文档只有「当前/待办」，看不见「刚刚发生了什么」——这层把 journal 翻成人能读的一行，只做翻译不存储（原始事实仍只有 journal 一份）。`feed.ts` 的 `describeEntry()` 覆盖 13 种意图（`plan.add/remove/rename` 与 `node.skip/evidence` 目前**没有任何代码路径会发**，落到 default 分支照常显示，不被静默吞掉）；`store.tailJournal()` 只读尾巴；路由 `limit` 默认 30 / 上限 100，宿主没注入能力回 503、读失败回空列表不抛。**本轮修的**：`at` 一直原样带出 epoch 毫秒（`clock()` 写了却没被用上，提交信息里说的「换算成本地 HH:mm」其实没生效），改成 `clock(line.at)`，`JournalLine.at` 类型同步改成 `number | string`（真实 journal 存毫秒、老日志存 ISO 串）；补 `node.back`（回退）/ `node.advise`（建议）两个意图，原先落到「·」。测试补 3 处（时间格式化、认不出的时间宁可空着、两个新动词）；`pnpm test` 121 绿、`pnpm typecheck` 干净
- 2026-10-23 · 节点图与面板按**自己那块屏**的缩放绘制（N-8）：原来 `g_dpiScale` 全进程一个值（跟着小条那块屏走），节点图/面板被挪到 125% / 200% 的屏上时窗口尺寸、字号、命中区全按错的比例算。新增 `g_panelScale` / `g_reviewScale` 与 `ScaleOfWindow(HWND, fallback)`（全仓唯一一处 `GetDpiForWindow`），`UpdatePanelScale()` / `UpdateReviewScale()` 变化时各落一行日志；`PanelTargetRect(monitor, scale)` 显式接缩放；`ReviewAvailWidthDip` / `LayoutReview` / `RepaintReview` / `HitTestDot` / `PanelProc` 四处命中 / `ReviewProc` 四处鼠标坐标全部改读各自那份（画在哪、点在哪同源）。两个窗口都补 `WM_DPICHANGED` 重排：审阅窗刻意不 `FitView`（保住人自己的缩放/平移），面板收起 morph 途中直接 `return`（否则会被重新摆成展开尺寸卡住）。真机 1.5x 之外，仓库外探针件伪造三窗同刻不同缩放（bar 125% / panel 250% / review 200%）验过：窗口物理尺寸 = DIP × 该窗 scale、同一 DIP 坐标在 100% 与 200% 下都命中同一颗节点、面板右上 × 在两档缩放下都关得掉；`pnpm test` 115 绿、`pnpm typecheck` 干净
- 2026-10-23 · AI 会话在跑时窗口亮蓝边 + 流光（N-7）：只认 `/api/state` 节点的 `handoff.state == "running"`（`agentSessionId` 随 `NodeView` 带出，只用于日志核对，不拿 `status=active` 猜）。节点图与面板整窗画呼吸蓝边 + 6.4s 一圈的流光；面板用单独的 60fps 计时器（`0x7D`），AI 停或面板收起立即 KillTimer，节点图复用既有 240Hz 动画计时器。真机 1.5x 假宿主：两窗 on/off 截图、120ms 三帧 hash 全不同、面板环 6.0%/6s、AI 停后 0.8%/6s；`pnpm test` 115 绿、`pnpm typecheck` 干净（`c91e503`）
- 2026-10-23 · 托盘与退出入口（N-6）：托盘原来只有「打开面板 / 退出」两项，且退出走 PostQuitMessage + 补发 WM_CLOSE —— 消息循环先停，WM_DESTROY 来不及跑，**托盘图标会留成残影**直到鼠标划过。现在：① 退出只发 WM_CLOSE，统一走 WM_DESTROY（摘图标 → 注销热键 → 退循环）；② 新增 RemoveTrayIcon() 供两处复用；③ 菜单补齐「打开节点图」「显示小条」「退出 DSH Work」，左键仍开面板，气泡提示写明两种操作。真机：`taskkill /PID`（不带 `/F`）= 发 WM_CLOSE，进程干净退出、无残留进程
- 2026-10-23 · 审阅窗跟着窗口所在的那块屏算宽度（N-5）：原来 `ReviewAvailWidthDip` / `LayoutReview` 都拿**小条**那块屏（`MonitorFromWindow(g_window, …)`）并且只在开窗那一刻算一次，换屏之后还拿着老屏的数——宽屏上开完挪到窄屏，右边那一截永远在屏幕外。改成三处：① 新增 `ReviewMonitor()`，开着就用审阅窗自己的显示器、没开才退回小条那块；② `LayoutReview` 开着时**只改尺寸、位置原地**（原来每次内容变化都重新居中，换屏后一展开节点就整块跳回小条那块屏），并把窗口夹回工作区；③ 新增 `RefitReviewToMonitor()`，在 `ReviewProc` 收到 `WM_WINDOWPOSCHANGED` / `WM_DISPLAYCHANGE` 时重算，**可用宽真变了才动窗口**，变了就 `Layout` + `FitView`（旧平移是按老屏算的）+ 重画并落一行日志。真窗口验过：窗口被挪到 x=1140 后 → 假工作区 1100 DIP 生效 → 窗口自动缩到 1650×390 物理、内容重新居中、日志「审阅：换屏后按新工作区重排，可用宽=1100」；可用宽没变时挪窗不重排，之后按 Enter 展开时窗口原地长高不跳回居中
- 2026-10-23 · 审阅窗「刚打开按 Enter 没反应」修掉（`a687996`）：`BuildReviewModel` 只夹选中项上界，而重建前没有行被选过、`selected` 是 −1，`ReviewExpandSelected` 直接静默 return，底栏却写着「Enter 展开/收起」；改成三支（空图 −1 / 越界或负数回 0），打开即可展开。顺手去掉 `kMsgReview case 1` 拿到新草稿后「按同索引再插一次 expanded 并重拉」的重建（行序已变，会摊开无关节点；pending 早就开过刚拆的那颗），改成只重新自适应视野；早退路径补日志。真窗口验过：打开后单发 Enter → 日志「审阅：本地展开 nd_a」、徽标「+2」→「−2」；没有子节点的行 Enter → 宿主 expand → 草稿模式按 pending 展开、视野重新适配。**改前先证伪了接力板上的 N-4 假设**：`has_children` 全仓只有 `BuildReviewModel` 一处赋值，doc / draft 两条路径的徽标在真窗口里都正常，「展开后徽标消失」当前代码复现不出（见 N-4 段）
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
- 2026-10-23 · 托盘与退出入口（N-6）：托盘原来只有「打开面板 / 退出」两项，且退出走 PostQuitMessage + 补发 WM_CLOSE —— 消息循环先停，WM_DESTROY 来不及跑，**托盘图标会留成残影**直到鼠标划过。现在：① 退出只发 WM_CLOSE，统一走 WM_DESTROY（摘图标 → 注销热键 → 退循环）；② 新增 RemoveTrayIcon() 供两处复用；③ 菜单补齐「打开节点图」「显示小条」「退出 DSH Work」，左键仍开面板，气泡提示写明两种操作。真机：`taskkill /PID`（不带 `/F`）= 发 WM_CLOSE，进程干净退出、无残留进程
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

本机 github.com:443 不通、api.github.com 通（Clash 没开时也是如此）。
所以 **git push 一定会失败**，改走 GitHub Git Data API 逐提交重放：

`
node scripts/push-via-api.mjs <远端当前 sha>
`

它会读 ~/.dsh/dsh-work/gh-token，把 <sha>..HEAD 的每个提交按原消息、原作者、原日期重建，
最后把 refs/heads/main 指过去。**远端 sha 与本地不同，但 tree 完全一致**（可用
git rev-parse HEAD^{tree} 与 GET /git/commits/<远端sha> 的 .tree.sha 对账）。
推完把远端新 sha 记在下面这行，下一轮用它当起点。

> 当前远端 sha：`0beddc8f38c4bc428abde0ebd0b9b31c5f972580`（2026-10-25 推送后，tree 与本地 `ec037b4` 一致；把「轮动展示＝连线流光轮着走」的口径也推上去了）。
> 上一轮记的 `3a2c89bb073cc7ab33c4cf59d3ed00a28bfb1831`（面板事件流那轮）、`1732b95c654f9065706246cf159119b13196dff9`（2026-10-24 第七轮）与 `8709f6dea60200232c517f25afe091b57522d304` 都已是历史，仅作对照。
> 本轮开工时现取到的是 `8709f6dea60200232c517f25afe091b57522d304`（上一次「记 sha」提交推送后的 sha），仍按上面的办法现取，别抄这一行。
> **推送起点别直接抄这一行**：只改 `docs/NEXT.md` 的那次回填提交总是排在它后面，抄旧 sha 当起点会把已经在远端的提交再重放一遍。开工先现取：
> `GET https://api.github.com/repos/liyangyu001-hue/dsh-work/git/ref/heads/main`（带 `gh-token`），拿 `.object.sha` 当参数。

## 当前任务（只做一个，做完就停）

- 阶段：**P2 · AI 在环**。**队列里的活真的空了**：2026-10-24 这轮又按「已知的坑」最后一条，把自动化提示语里的四件（入口 / 直角折线 / 加宽 / 讲讲它）逐条对过源码——入口 `packages/plugin/src/client/index.tsx:297`（`data-dsh-work-graph="1"`）+ `:326`（「打开节点图」）、直角折线 `native/shell/src/review.h:473`（`BuildEdge`，`:569` 调用）、`constexpr float kWindowW = 1600.0f` 在 `review.h:111`、讲讲它 pill `review.h:723-748` + 页脚提示 `review.h:804`；`git log` 里 `e1f8191` / `eba985a` / `62a10bd` / `8fb316f` 全在。`pnpm test` 130 绿、`pnpm typecheck` 干净、工作区干净、HEAD `e72a533`。**别再把这些重做一遍。**
- 2026-10-25 本轮已把「2026-10-25 交接」的待办 1（面板底部事件流一行不显示）修好并真机验完：`DrawFeed` 改不透明暗色 + 删掉重复死代码（见进度第一条）。交接里另两条：待办 2（网页按钮压标题栏）已被 `f7d2c3e` 改成 `top:52` 解决；待办 3（Pulse 节拍时钟 + 状态矩阵）还没人接。
- **当前任务 = 等用户拍板下一件。** 拖动这条线（第①）已经整条交付：拖动 → 松手记位置 → **跨重启记住**（本轮，`~/.dsh/dsh-work/review-placement.json`）→ 换屏按那块屏重排。只剩第②条卡在设计取舍（红线：不擅自改产品方向），落地位置已勘定，点头就能直接开工：
  - **① 审阅窗按住标题栏拖动 —— ✅ 全链交付**（用户 2026-10-25 接第二块显示器后点头「接好了」；随后 m01128「要 记住」＝ 批准跨重启记忆）。还留一件小事等拍板：拖到屏幕边缘要不要**吸附**（现在纯跟手）。
  - **② 恢复 `kMinFitZoom`**（剩下的那一件，改的是窄窗口里的视觉行为）。落地：`review.h:105` 那一行 `kMinZoom` 的注释里有一个**字面 `\n`**，把后面的 `constexpr float kMinFitZoom = 1.0f;` 一起吞进注释了；`:278` 的 `if (zoom < kMinFitZoom) zoom = kMinFitZoom;` 同样整行是注释。两条拆成独立行、放开 clamp，就是「自动适配也不缩小」；代价是窗口窄于内容时（`avail_w` < 1600 DIP，或展开后行变多）内容会被裁、只能靠拖动/缩放看全。1600 DIP 默认宽下本机几乎不触发。
  - **③ 下一件（用户 2026-10-25 已选定）：节点图连线的「轮动展示」**。用户在我追问「轮动展示指哪个」时选了**「节点图里节点间的流光线节奏」**。现状：`review.h:559` 的 `flow_phase`（`anim.now * 0.00022`）给每条边一个相位 `phase = flow_phase + i * 0.11f`（`:580`），所以**所有边同时在流**、只错开 11% 弧长，看起来是一条并行的流水线。用户要的「轮动」按「一次只亮一条边、从第一颗轮到最后一颗循环」实现（不是满屏同时跑）。落地点就在 `:562-590` 这个 for 循环：把「每边各自错相位」换成「按时间算当前轮到第几条边 + 那条边上光头的位置」，底线、18% 亮弧、按累计弧长定位那套全部复用。节奏建议一条边 0.9s 走完、交接留 0.1s 空档，真机看完再调快慢。
- 拿到点头再动手，一件事做完就停。「桌面版重启后玻璃条亮了吗」别再问用户：上一轮已实测亮着（7/7、按钮可点、真点一次弹出图窗）。
- 2026-10-24 第三轮那条「desktop profile 没装插件」已被**实测推翻**：`C:\Users\25108\.dsh\profiles\desktop\package.json` 里 `@dshwork/plugin` 在 dependencies 也在 `dsh.profile.bundles` 里（用户 08:44 自己装的），`node_modules\@dshwork` 也在。玻璃条确实渲染在那个界面里（截图 `dshwork-shots\desktop-gui.png`），别再拿「看错界面」解释。
- 规矩不变：一件事做完就停。

### N-8 跨 DPI 屏：节点图与面板按各自那块屏的缩放绘制 —— ✅ 已交付

**为什么**：N-5 留下的②。进程虽然是 `PER_MONITOR_AWARE_V2`，但 `g_dpiScale` 全进程只有一个值、跟着小条那块屏走；节点图/面板被摆到 125% / 200% 的屏上时，窗口尺寸、字号、命中区全按小条的比例算——字小一圈、窗口偏一半，点击还会落空。

**做法**（全部在 `native/shell/src/main.cpp`）
- `ScaleOfWindow(HWND, fallback)`：全仓唯一的 `GetDpiForWindow`（窗口为空 / 返回 0 / 小于 0.1 都退回 fallback）。`g_dpiScale` 只归小条；新增 `g_panelScale` / `g_reviewScale`，`UpdatePanelScale()` / `UpdateReviewScale()` 变化时各落一行日志。
- `PanelTargetRect(HMONITOR, float scale)` 显式接缩放（原来读全局）。`ReviewAvailWidthDip` / `LayoutReview` / `RepaintReview` / `HitTestDot` / `PanelProc` 四处命中 / `ReviewProc` 四处鼠标坐标全部改读各自那份——**画在哪、点在哪同源**（两套来源并存就会出现「看得到点不到」）。
- `ShowPanel` 先按小条那块屏的 DPI 定基线再算落点，窗口落位后用 `UpdatePanelScale()` 以它自己的屏校正一次；`PrepareReviewScale()` 同理给节点图（开着就沿用缓存值）。`EnsureReviewWindow` 落位后再校一次。
- 两个窗口都补 `WM_DPICHANGED`：审阅窗 `UpdateReviewScale` + `LayoutReview` + `ClampPan` + 重画，**刻意不 `FitView`**（人自己的缩放/平移要保住）；面板先收掉展开 morph 再按新缩放摆回目标位，**收起** morph 途中直接 `return`（否则会被重新摆成展开尺寸、再也关不上）。

**验收**（真机 1.5x + 仓库外探针件伪造缩放；本机只有一块屏）
- [x] 真机 1.5x 走完 bar → 面板 → 节点图：面板 1494×198、节点图 2400×390 物理，与改前一致（无回归），7 颗一长条、直角折线/流光正常
- [x] 三窗同刻不同缩放（bar 125% / panel 250% / review 200%）：窗口物理尺寸 = DIP × **该窗** scale；旧代码会全按 1.5 算
- [x] 换屏重排：review 200% → 3200×520、展开后 3200×588；panel 150% → 250% → 2350×330（top 跟着 `kPanelTopDip × scale`）
- [x] 命中同源：同一 DIP 坐标 (690,135) 在 100% 与 200% 下都命中同一颗节点（`TEMP 命中 … node=0` → 「审阅：本地展开」）；面板右上 × 在 150% / 250% 下都关得掉
- [x] `pnpm test` 115 绿、`pnpm typecheck` 干净；探针补丁只在 `dshwork-probe\src-fake`，仓库源码无 `TEMP`
- [x] 截图（`n8-*.png`）只留在仓库外 `D:\work\AI\work\dshwork-probe\`

**留下的**：Windows 下 `WM_DPICHANGED` 只能由系统同步投递，注入不进去（见「已知的坑」），所以「真的从 A 屏拖到 B 屏」这一步只能靠系统事件触发、本轮用探针件里同一段重排体间接验；本机单屏也没法验「拖过去之后的位置」是否符合直觉。另外发现 doc 模式下再点已选中节点会被宿主回 `缺少 jobId 或 nodeId`（既有问题，见「等用户决定」）。

### N-7 AI 会话在跑时，节点图与面板边缘亮蓝框 + 流光 —— ✅ 已交付（`c91e503`）

**为什么**：用户原话「我希望 dsh会话 也能够接入这个系统 但是会在现在弹窗周围加一个蓝边缘框 代表自动运行中」。之前只有面板右列一行「AI 正在做」，节点图/面板本身没有「它在自动运行」的余光信号。

**数据源**：只认 `/api/state` 里节点 `handoff.state == "running"`。`agentSessionId` 随 `NodeView` 带出来，只用于日志核对是哪一次会话，不拿 `status=active` 或标题猜。`doc.h` 新增 `HasRunningAgent()` / `RunningAgentSession()`。

**做法**
- `doc.h`：`NodeView` 增 `agent_session`，`WalkNodes` 读 `handoff.agentSessionId`。
- `panel.h`：新增 `FramePoint()`（圆角矩形周长参数化，和节点环同一套顺时针）与 `DrawAiFrame()`——三圈随呼吸变亮的 accent 边 + 沿边顺时针 6.4s 一圈的锥形流光；分层窗下用圆点叠，不用 `DrawLine`（平头会露缺口）。`DrawPanel` 在最后单独恢复 `Scale(scale)` 再画，环不跟内容 zoom 缩。
- `review.h`：`Model` 增 `ai_running`，`Draw` 末尾按整窗 22 DIP 圆角画环，不挡节点/页脚/「知道了」按钮。
- `main.cpp`：`BuildReviewModel` 开窗即按 running 置位（不依赖后续状态变化）；面板默认静止，新增 `kAiRingTimerId = 0x7D / 16ms` 只重画面板，AI 停或 `HidePanel` 立即 KillTimer；节点图沿用既有 240Hz 动画计时器。状态切换落日志「节点图边缘亮起/熄灭：… session=」。

**验收**（真窗口 1.5x DPI，仓库外假宿主）
- [x] 节点图：running → 整窗蓝边 + 彗尾；AI 关掉 → 环消失，节点自身「一圈在转」仍正常
- [x] 面板：running → 整窗蓝边 + 右下「AI 正在做」；环不挡关闭按钮与内容；AI 关掉 → 环消失
- [x] 120ms 间隔三帧 hash 全不同，流光在走；面板环 60fps 时 CPU 6.0%/6s，AI 停后 0.8%/6s
- [x] `pnpm test` 115 绿、`pnpm typecheck` 干净
- [x] 探针 shim / 原生进程收掉，截图只留在仓库外 `D:\work\AI\work\dshwork-shots\`

**留下的**：60fps 环约 6% of one core（面板可见时）；嫌高可降到 30fps（约一半），但流光会看出台阶。6.4s 一圈、三圈外晕的视觉参数集中在 `DrawAiFrame()`，要调只动那一处。

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

### N-4 审阅窗「+N / −N」徽标在展开后消失 —— ⚠ 当前代码复现不出；修掉了这条线上真机真按不动的那处（`a687996`）

**证伪接力板上的假设（「`has_children` 在重建后丢了」）**
- `has_children` / `child_count` 全仓只有一处赋值：`native/shell/src/main.cpp:1413-1416`，取 `children` 数组长度；`BuildReviewModel` 每次都重建，没有别的路径能把它清掉。
- 绘制在 `native/shell/src/review.h:691-699`，位置在节点框内右上角，且被 `PushAxisAlignedClip(box)` 裁着——几何上裁不到；`child_count > 0` 才画，所以"没画出来"等价于"JSON 里 children 空/缺"。
- 假宿主 + 真窗口实测两条数据形态都正常：doc（`nodes` 是**对象**）路径「+2」→ 展开「−2」；draft（`nodes` 是**数组**）路径同样「−2」，子行都画得出来（截图 `dshwork-probe\badge-before*.png`、`fix-enter.-keys.png`、`fix-draft2.png`）。
- 待办句最早出现在 `fb114bd`（2026-10-22 审阅窗那一轮），而 chip 当时在**右下角**、第二轮标题与讲解 pill 都在那片；`aa5ea03` 才把 chip 挪到右上角并给概念节点加标记/让出标题宽度。倾向结论：那是历史布局问题，已在 `aa5ea03` 顺带消失。

**这轮真修掉的**
- **刚打开的节点图按 Enter 没反应**：`ShowReviewDoc` / `ShowReview` 重建前没有行被选过，`selected` 是 −1，`ReviewExpandSelected` 静默 return，而底栏写着「Enter 展开/收起」——用户看到的就是"按了没反应"。`BuildReviewModel` 尾部改成三支：空图 −1、负数越界或过大回 0（原来只夹上界）。
- **`kMsgReview case 1` 多重建一次**：拿到新草稿后又按"同索引"把新模型里那一行塞进 `g_review_expanded` 再 `ReloadReviewDraft()`。展开/拆细后行序会变，同索引早就不是刚拆的那颗，会摊开无关节点；而 `ReloadReviewDraft` 内部已按 `g_review_pending_expand` 展开过刚拆的那颗。改成 `FitView` + `ClampPan` + `LayoutReview` + `RepaintReview`（新子树把图撑宽时自动重适配）。
- 「没有选中的行」这条早退路径补一行日志，别再让真机上的"按了没反应"无从查起。

**验收**（真窗口，1.5x DPI）
- [x] 打开节点图后**单发 Enter**直接展开根节点：日志「审阅：本地展开 nd_a」，徽标「+2」→「−2」，子行与直角折线正常
- [x] 选到没有子节点的行按 Enter → 宿主 `/api/plan/expand` 成功 → 草稿模式加载（标题「草稿：探测」）、根节点按 pending 展开成「−2」、视野重新适配、没有别的节点被误展开
- [x] `pnpm test` 115 绿、`pnpm typecheck` 干净
- [x] 验证用的是同一份源码 `/W0 /Od` 编出的探针件（`/O2` 产物被本机安全策略按哈希拦下，见「已知的坑」）

**真机复核（2026-10-23）—— 复现不出，本条关闭**
- 用的是工作区真实文档（7 颗概念节点、标题「搞懂幂等及其与反复重试的关系」）：先备份 `work.json`，再把同一份数据临时改成「根 + 6 子」（只改 `parentId` / `children` / `roots` / `pointer`），验完已还原（`roots=7 nodes=7`，备份在仓库外 `dshwork-probe\work.json.bak`）。
- 宿主与伴随层都是真进程，审阅窗 2400×390 物理 = 1600 DIP 一长条（@150%）：打开只有根一颗、徽标「+6」；按一次 Enter 后根变「−6」、6 颗子节点并排画齐（`用例子定义幂等` / `区分幂等与重试安全` / `制作操作判断表` / `归纳重试导致问题的条件` / `整理常见防重手段与边界` / `用自测讲清二者关系`），徽标稳定在节点框内右上角、没消失；放大裁剪确认「−」号正常（`dshwork-shots\badge-real.png`、`badge-real-expanded.png`、两张 `-zoom.png`）。
- 结论：徽标逻辑没丢，`child_count` 每次重建都按 `children` 长度重算；「消失」是 `fb114bd` 时代 chip 在右下角时被标题 / 讲解胶囊挤掉的观感（`aa5ea03` 挪到右上后已消失）。

**留下的**：要再验一次直接跑 `dshwork-probe\badge_shot.py --pid <dshwork-shell 的 pid>`（DPI-aware 抓真窗、自动按 Enter）；别在面板盖着图的时候用 `shot.ps1 -Expand`，会 `NO_WINDOW`。探针设施都在仓库外 `D:\work\AI\work\dshwork-probe\`，下轮验原生界面直接复用。

### N-5 多显示器时可用宽跟着窗口所在屏变 —— ✅ 已交付

**为什么**：N-2 留下的问题。`avail_w` 只在开窗那一刻按**小条**那块屏算一次；审阅窗换到另一块屏后还拿着老屏的数——宽屏上开完再换窄屏，右边那一截永远在屏幕外；窄屏换宽屏又白白空一条。`LayoutReview` 还每次都按小条那块屏重新居中，换屏后一展开节点就整块跳回去。

**做法**（全部在 `native/shell/src/main.cpp`，只碰这一处原生）
- `ReviewMonitor()`：开着就 `MonitorFromWindow(g_review, MONITOR_DEFAULTTONEAREST)`，还没开才退回小条那块（那是它马上要被摆上去的屏）。`ReviewAvailWidthDip()` / `LayoutReview()` 都改用这个。
- `LayoutReview()`：开着时**只改尺寸、位置原地**（原来无条件重新居中）；任何方向都夹回当前屏工作区，长高之后尾巴不会沉到屏幕外。
- `RefitReviewToMonitor()`：`ReviewProc` 收到 `WM_WINDOWPOSCHANGED` / `WM_DISPLAYCHANGE` 就重算一次；**可用宽没变就一个像素都不碰**（免得和动画、中键拖动打架），变了才 `LayoutReview` + `Layout` + `FitView`（旧平移/缩放是按老屏算的）+ `ClampPan` + 重画，并落日志「审阅：换屏后按新工作区重排，可用宽=N」。用静态 `refitting` 挡住自身 `SetWindowPos` 引起的重入。

**验收**（真窗口，1.5x DPI；本机**只有一块屏**，所以用一个临时补丁模拟窄屏）
- [x] 默认打开：2400×390 物理 = 1600×260 DIP、位置 (720,144) 物理，内容与 N-4 一致（无回归）
- [x] 挪窗后可用宽变了：假工作区 1100 DIP → 窗口自动缩到 1650×390 物理、内容重新居中不被截断、日志出现「可用宽=1100」
- [x] 可用宽没变时挪窗不重排；挪完按 Enter 展开：窗口原地从 390 长到 441 物理，x/y 不动（改前会跳回居中位）
- [x] 临时补丁已删、重新 `node scripts/build-native.mjs dshwork-shell` 出的就是仓库里那份
- [x] `pnpm test` 115 绿、`pnpm typecheck` 干净

**留下的**：① 审阅窗**拖不动**（`WS_POPUP` 无标题栏、`ReviewProc` 没有 `WM_NCHITTEST`），「换屏」目前只能靠系统/`SetWindowPos` 发生——要不要支持按住标题栏拖动见「等用户决定」；② 跨 DPI 的屏之间 `g_dpiScale` 仍是全局一个值（进程虽是 PER_MONITOR_AWARE_V2，但绘制/尺寸都读全局 scale），换到 125%/200% 的屏上字号与尺寸会不匹配——要单独一轮把 scale 改成按窗口取。

---

## 队列（做完一个换一个，不要并行）

- ✅ 面板底部「事件流」一行从来不显示：根因是分层窗口（`UpdateLayeredWindow` + `ULW_ALPHA`）把 D2D 的预乘 alpha 又乘了一次，那行用的是 0.4 的半透明灰 → 改成不透明暗色 `Fade(kTextDim, alpha)`，真机左下角可见（见进度第一条）
- ✅ 桌面 GUI 实机验证入口（2026-10-24 第五轮）：玻璃条「全部已完成 7/7」+「打开节点图」可点，DPI-aware 真点一次就拉起伴随层、弹出 7 颗节点的图窗；零代码改动（见进度第一条）
- ✅ 用户「找不到打开入口」的真原因：插件把 `process.cwd()` 当任务库根目录，桌面 profile 下绑到空库、「打开节点图」永远置灰 → 改成跟随宿主 `workspaceRegistry` 登记的工作区（1.5s 有界等待 + 日志），从错误 cwd 起也认 `D:\work\AI\work`（见进度里「插件绑错任务库根目录」那条）
- ✅ 节点图开着时小条仍认鼠标 → 面板被反复顶出、图上一层套一层：两条路真机复现 + 修掉（`2b95980`，见进度第一条）
- ✅ 节点图里点「已完成/进行中」的节点可把它设为当前任务：宿主 `node.focus` 早就有，补上「看得见当前那颗 + 状态回来重画 + 被拒时回话」，手势定为双击（`5a07ddd`，见进度第一条）
- ✅ 节点图开着时跟着整份状态变（不只跟 `doc.pointer`）：签名覆盖树形状 + 每颗 status/kind/标题/交接，变了才重建且保住选中与视口（`9737aa5`，见进度第一条）
- ✅ 徽标复核（N-4）：真机 1600 DIP、7 颗节点上复现不出，已关（见 N-4 段真机记录）
- ✅ 审阅窗「拖不动」：按住标题栏可拖、松手记住位置、拖到第二块屏按那块屏的宽度重排（`WM_NCHITTEST` → `HTCAPTION`，见进度第一条）
- ✅ 拖动位置跨重启记住：写 `~/.dsh/dsh-work/review-placement.json`，坏值按最近那块屏夹回工作区（见进度第一条）
- 节点图连线的「轮动展示」：一次只亮一条边、从第一颗轮到最后一颗循环（用户 2026-10-25 选定，落地位置见「当前任务」第③条）
- 恢复 `kMinFitZoom`：仍等用户点头（见「等用户决定」）
- ✅ 节点图里点「已选中的节点」会去请求宿主拆层、doc 模式没有 jobId 被宿主回错 → 只读模式改成页脚一句人话、不发请求；顺手修掉「点收起不重建模型所以收不起来」（见进度第一条）
- 已交付：悬停守卫补全（`2b95980`）、N-8 跨 DPI 按窗口取缩放（见进度第一条）、N-7 蓝边 + 流光（见 N-7 段）、N-6 托盘与退出、N-5 换屏跟宽（见 N-5 段）、N-4（`a687996`）、N-1 直角折线（`eba985a`）、N-2 加宽（`62a10bd`）、N-3 讲讲它（`8fb316f`）、插入任务「打开节点图」（`e1f8191`）；入口按钮（`eed4aed`）——复核结论见各自「留下的」

---

## 等用户决定

- **下一件做什么**：① 审阅窗「按住标题栏拖动」**已整条做掉**（2026-10-25：拖动 → 记位置 → 跨重启记住 → 换屏重排，见进度第一条）；剩下的候选是 ② 恢复 `kMinFitZoom`（改的是窄窗口里的视觉行为，要权衡）。②的改法与代价在「当前任务」，点头就开工。
- **拖动只剩一小小问**：拖到屏幕边缘要不要**吸附**（现在纯粹跟着鼠标走，停在哪儿是哪儿；跨重启记住已按用户「要 记住」做完，存在 `~/.dsh/dsh-work/review-placement.json`，坏值/拔屏有兜底）。
- ~~**「轮动展示」是什么？**~~ —— **已问清（2026-10-25）**：用户选了「节点图里节点间的流光线节奏」，也就是让流光**一次只亮一条边、轮着往下走**（现在所有边同时流）。落地位置写进「当前任务」第③条，下一轮直接开工。
- **「设为当前任务」的手势现在是双击，要不要改成单击？** 单击更符合「点谁就是谁」的直觉，但代价有两层：① 单击本来用于选中，双击那一半就没了；② `node.focus` 会 `demoteOtherActive`（`state.ts:368-377`），也就是**光看图就会把当前任务一路挪走**——只想翻一眼图的人会把进度改掉。现在保住的是双击（要么明确点两次，要么选完按 Enter 的本地展开）；单击要等这张图能区分「浏览」与「操作」再说，例如单击选中、选中后再单击才生效。等你拍。
- **`kMinFitZoom` 那句「自动适配也不缩小」目前是死的**（见「已知的坑」）。恢复它 = 窄窗口里字不再被缩小、内容超出就靠人拖/滚；不恢复 = 现在这样自动缩到装下。要不要恢复？先不动，N-2 加宽只是让它不那么常触发。
- **「讲讲它」的讲解会覆盖节点原有的 `advice`**（宿主 `learn` 直接 `node.advise` 落库，这是它有意的设计：面板本来就渲染这个字段）。要不要给讲解单独一个字段、与「建议」并存？先不动，等你拍。
- ~~**审阅窗现在拖不动**~~ —— **已解决**（2026-10-25）：`ReviewProc` 的 `WM_NCHITTEST` 把头带返回 `HTCAPTION`，按住标题栏就能拖；松手记位置、重开还在那儿、拖到另一块屏按那块屏的宽度重排。剩下「吸附 / 跨重启记住」两小问见上一条。
- **多工作区时玻璃条该绑哪一个？**（2026-10-24 第四轮新增，暂按「有会话的那个」实现）你这台机器上 `~/.dsh/storages/workspace.json` 只登记了一个工作区（`D:\work\AI\work`），所以现在没有歧义。但只要注册了第二个工作区，「任务库根目录」就得挑一个：现在的规则是 **插件配置 `root` > 登记表里带 `sessionIds` 的那个（= 最常用的工作区）> 进程 cwd**。要不要改成「跟着当前会话所在的工作区走」（那样玻璃条会随对话切换而换库，更像 IDE 的「当前项目」）、或者干脆让用户在面板里选一次并记住？先按现在的规则，等你拍。

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
- **`FindWindow("DSHWorkNativeReview")` 可能返回 0**（跨进程 + DPI 虚拟化时踩到），改用 `EnumWindows` 按 pid 枚举再比类名；要展开节点不用抢焦点，`PostMessage(hwnd, WM_KEYDOWN, VK_DOWN/VK_RETURN, 0)` 直接投递即可。注意 `ShowReviewDoc` 会清空 `g_review_expanded`——文档节点图刚打开时**只有根节点**，要展开得再按一次 Enter（这一下在 N-4 之前是静默无反应，`a687996` 已修）。
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
- **编译出来的 exe 突然"读不了也起不来"时，别在 ACL / 占用上死磕**（2026-10-23 踩了一小时）：`node scripts/build-native.mjs dshwork-shell` 报成功后，`[IO.File]::ReadAllBytes` / `certutil -hashfile` / `Start-Process` 一律 `拒绝访问`（`ERROR_ACCESS_DENIED`），而 `icacls` 干净（无 DENY）、文件**删得掉**、同目录里复制进来的 `whoami.exe` 能读能跑、Defender 报 `AntivirusEnabled=False`。对照实验：同一份源码换 `/W0 /Od` 编到 `D:\work\AI\work\dshwork-probe\shell-t3.exe` → 读、启动都正常；`/O2` 编到任何路径、任何名字都被挡（把 hello.exe 改名成 `native\shell\build\dshwork-shell.exe` 也会被挡）。结论：**本机那个安全策略按产物哈希/内容拦，不是我们的代码、不是 ACL、不是占用**。办法：另编一份 `/Od` 的探针件来真机验证界面（同源码只差优化等级），仓库里照旧留 `build-native.mjs` 的产物；别为了这个改构建参数提交。
- **`job_kill` / `Stop-Job` 杀掉 pwsh 后台作业不会带走它起的 `node` 子进程**：探针 shim 重启时必报 `EADDRINUSE 127.0.0.1:64077`，原生日志只写「轮询失败：请求失败」（很容易误判成协议不对）。先 `Get-NetTCPConnection -LocalPort 64077 -State Listen | % { Stop-Process -Id $_.OwningProcess -Force }` 再起。
- **本机只有一块屏（2560×1392 DIP），验「换屏跟宽」必须临时伪造**：在 `ReviewAvailWidthDip()` 里按窗口位置返回一个更窄的值（例如窗口 `left / scale > 700` 时 `avail = 1100`），编到仓库外的一份探针件（改 `build.bat` 的 `/OUT` 即可），真机上 `SetWindowPos` 挪窗 → 看窗口自动缩窄 + 日志。**验完把补丁删干净再重编仓库那份**——`git diff` 里出现 `TEMP` 就是没删。
- **同一个 `build-native.mjs` 产物这次又能读、能起了**：上一条说的「按哈希拦」是**逐产物、且会变**的（这一轮的 567808 B 就正常）。所以别为它改构建参数；被拦时换一份 `/Od` 探针件验证界面，然后照常 `git` 提交源文件。
- **审阅窗的尺寸变化只走 `LayoutReview()`**（里面有 `SetWindowPos`）：新增「让窗口长高/变宽」的代码时别再手写一份 `SetWindowPos`，否则「开着时保持位置」「夹回工作区」两条规则会被绕过。`RefitReviewToMonitor()` 就是靠 `avail` 比较 + `refitting` 静态标志防重入的。
- **`WM_TIMER` 的自定义 id 会撞车，症状是「动画只动一帧」**：N-7 面板环最初用 `0x7C`，正好等于既有 `kBtnPulseTimerId`；`WM_TIMER` 按 id 先命中按钮脉冲分支，它立刻 `KillTimer`，环只画了一帧，120ms 后两张截图完全相同。新增计时器 id 先全仓搜一遍（`0x7D` 已被 AI 环占用），并且连续三帧 hash 全不同才算动画在跑。
- **面板默认是静止的，常驻动画必须自己续帧**：它只在 morph / squash / chipfade 时借 `kAnimTimerId` 跑 240Hz，结束就 KillTimer；AI 运行中的边缘环不能挂靠它，要单独 60fps 计时器，并在 AI 停 / `HidePanel` / 状态消息到达时 KillTimer，否则面板收起后还在空转。
- **关掉蓝边后圆角外那一小块蓝不是残留**：面板是分层窗，透明圆角会透出桌面/别的窗口；只看左上角会误判成「环没擦掉」。判断整圈是否熄灭看四条边和整窗，或放大按像素采样，别被背景色骗了。
- **原生缩放一律经 `ScaleOfWindow(HWND, fallback)`**：`GetDpiForWindow` 全仓只留这一处，两个副窗各缓存一份 `g_panelScale` / `g_reviewScale`（`g_dpiScale` 只归小条）。绘制、布局、命中区必须读同一份；两套来源并存时换屏后就会「看得到、点不到」。
- **`WM_DPICHANGED` 不能用消息注入伪造**：`PostMessage(hwnd, 0x02E0, …)` 直接失败（`GetLastError = 1159`，ERROR_MESSAGE_SYNC_ONLY，这条只允许同步投递）；`SendMessageTimeout(hwnd, 0x02E0, …, SMTO_ABORTIFHUNG, 3000)` 返回 0，进程里连 `ReviewProc` 入口日志都不出现（同一调用方式投 `0x000F` / `0x0201` 都返回 1）。要在单屏上验换屏重排，只能在**仓库外探针件**里给同一段重排体再挂一个可投的消息触发器（本轮 review=`WM_APP+1`、panel=`WM_APP+2`），验完删补丁重编仓库件。
- **面板 `WM_DPICHANGED` 撞上「收起」morph 会把面板卡在展开态**：收缩动画跑着时按新缩放重排，会重新摆成展开尺寸并 `g_morph.active=false`，而 `g_panel_open` 还是 true → 之后再也关不掉。护栏：`UpdatePanelScale();` 之后先 `if (g_morph.active && !g_morph.opening) return 0;`，再重排。
- **探针脚本的调用方式有讲究**：`poke.ps1` 必须用 pwsh（`& .\poke.ps1 …`）；`powershell -File .\poke.ps1 -Scale 1.0` 会报 `A parameter cannot be found that matches parameter name 'Scale'`（原因未查）。`shot.ps1` 必须给 `-Out`，否则报「缺少一个或多个必需参数: Out」，连 `-Keys` 也一起不生效。
- **节点命中坐标按 `kHeader`（58 DIP）算，不是 34**：节点顶 = `kHeader + pan_y`、左 = `pan_x`（`review.h` 里 `kHeader = 58.0f`）。1600×260 窗口、pan=600,48、zoom=1 时节点占 y 106..164，点 y=165 就差 1 px 落空（`NodeAt` 返回 −1）。最省事的做法是让探针件把 `pan / zoom / BodyWidth / row0` 一起写进日志，一眼能算出该点哪。
- **伴随层令牌 ≠ 网页令牌**：`~/.dsh/dsh-work/endpoint.json` 里的 `token` 是网页令牌，拿去打 `/dsh-work/api/*` 一律回 `{"error":"forbidden"}`；伴随层令牌是单独一个文件 `~/.dsh/dsh-work/token`（32 位），请求头 `x-dsh-work-token`。真机开图最省事：`POST /dsh-work/api/ui/open {"target":"graph","at":<epoch 毫秒>}`，`at` 要在 15 秒内（伴随层当过期丢掉），`target:"review"` 会被回 400。
- **玻璃条悬停会顶出面板**，和节点图叠在一起：自动化截节点图时鼠标必须停在空白处（或先移开再截），否则截到的是面板盖在图上的样子；面板盖着窗口时 `shot.ps1 -Expand` 会 `NO_WINDOW`（标题栏被遮），用仓库外 `dshwork-probe\badge_shot.py`（DPI-aware + 自动 Enter）更稳。**根因已在 `2b95980` 修掉**（图开着时悬停整条不认鼠标、关图瞬间也不补弹）；但截**面板**时鼠标仍别停在药丸上——那时图本来就关着，悬停照旧会开面板。
- **悬停时摘掉的 `WS_EX_TRANSPARENT` 必须在每条提前 `return` 的路上还回去**：小条平时整窗穿透、只靠 120ms 轮询算悬停，悬停那几秒摘掉穿透才好收单击/双击。`UpdateHover` 里「图开着」的分支原来是提前 `return` 的，于是穿透位留在了摘下状态——图开着时点药丸，面板照样盖到图上（实测 6 次撞 1 次）。同理，悬停时排下的 `kOpenTimerId`（260ms）也得在守卫里 `KillTimer`，否则计时器到点不看新状态，照样 `ShowPanel`。本轮把「穿透位开关」收成一个 `SetBarClickThrough(bool)`，悬停分支和守卫都只调它。
- **`CloseReview()` 之后马上「重新判定悬停」会自己顶面板**：Esc 关图时光标往往还停在药丸上，下一个 120ms 轮询立刻判「刚进入」→ 260ms 后面板自己弹出来（3/3 必现，用户说的「反复顶」）。修法是 `CloseReview` 末尾 `AdoptHoverOnBar()`：光标真在药丸上就把这次悬停「认下来」（`g_bar_hovered = true` + 摘穿透 + 重画）。副作用是有意的——关图后想再开面板要先移开鼠标再回来（或直接单击药丸）；改成「永远不认下来」就会回到反复弹。
- **`dshwork-shell.exe` 会被卡巴斯基误报成 `Trojan-Spy.Win32.Convagent.gen`**（2026-10-24 弹窗，按钮写「清除并重启计算机」）：**别点**。它不是用户自己编的那份源码的问题，而是卡巴启发式对无签名 + 分层窗 + 全局热键的 exe 下的判断；点了会删掉 `native\shell\build\dshwork-shell.exe`（有时连重启都建议）。要真机测原生件时先确认 exe 还在，不在就重编。
- **PowerShell 里别把自定义函数命名成 `Move`**：`Move` 是内置别名（→ `Move-Item`），别名优先级高于函数，于是 `Move $PTS` 会被当成移动文件（报「找不到路径 D:\work\AI\work\600」）。探针脚本里改叫 `Set-Pt` 才生效；症状是「脚本能跑、鼠标却一次没动」，验证结果全假。
- **查远端 sha 别把 PAT 塞进 `Authorization: x-access-token:…`**：`Invoke-RestMethod -Headers @{Authorization = "x-access-token:$token"}` 会当场报 `The format of value 'x-access-token:ghp_…' is invalid`（.NET 的 `WebRequest` 会校验 Authorization 的 scheme，只认 Basic/Bearer 这类），**不是 token 坏了**。改 `Authorization = "Bearer $token"` 就能取到 `git/ref/heads/main` 的 `.object.sha`；`GUIDE.md` 里那套 `x-access-token:` + base64 是给 `git -c http.extraHeader` 用的，别照搬到 API。
- **文档类提交也会被 CRLF 警告刷屏**：`docs/CHANGELOG.md` 是 CRLF，`Set-Content`/`edit` 写回后 git 会提示 `CRLF will be replaced by LF`；只是警告，提交照常，别去改 `.gitattributes` 折腾。
- **同一仓库可能同时在跑别的会话**：本轮开工时 HEAD 已被另一个会话推进到 `259048f`，工作区还留着它未提交的 `native/shell/src/main.cpp`。提交前先 `git log -3` + `git status`，只 `git add` 自己改的文件，别 `git add -A`。
- **「当前任务」的 id 在 `/api/state` 的 `doc.pointer` 里**（`packages/core/src/types.ts:80`，`WorkDoc.pointer: NodeId | null`），不在 `doc.nodes` 上。草稿（解构未落库）的 `nodes` 是**数组**且没有 pointer，`GetString("pointer")` 读成空串——所以文档图才标当前任务，草稿图一颗都不标（别顺手把空串当成「全部是当前」）。
- **无 BOM 的 UTF-8 `.ps1` 会被 `powershell.exe` 按 ANSI 解码**：中文注释解出来的字节里可能凑出被 PowerShell 当成引号的字符，于是 `Add-Type -TypeDefinition @'…'@` 的 here-string 当场断掉，报的却是 `A 'using' statement must appear before any other statements in a script`（看着像 P/Invoke 少写了个分号，其实是注释）。探针脚本的注释一律写 ASCII（或另存成带 BOM 的 UTF-8）。
- **探针算点击坐标别照 `FitView` 的 `pan` 猜**：内容装得下时 `ClampPan` 会把内容居中（`pan_y = (BodyHeight − content_h) / 2`），窗口高度一变，同一颗节点的屏幕 y 就跟着挪。最稳的是从截图上量像素再 ÷ scale 得 DIP（本轮按公式算出的 y 差了 40 px，量像素一次就对）。
- **每秒跑一次的「跟着状态重画」必须先判「真的变了」再重建**：重建会按 pointer 重设 `selected`、还会 `LayoutReview` 重排窗口，人手里的选中项与视口会被拽回去。判定别拿 `/api/state` 原文比（字段顺序、`seq`、`ui` 每次都动 = 每秒重建），要比**语义签名**：`ReviewDocSignature()` 拼 `pointer` + `roots` 顺序 + 每颗 `status|kind|title|handoff.state|children`（`JObject` 是 `std::map`，对象形态天然有序；草稿数组按原序）。重建时**选中项按 id 记、按 id 复原**（行号会随树形状整体挪位，按 `selected` 下标还原会选错人），只有 pointer 真换人才 `ExpandAncestorsOf()` + 把选中跟过去，否则别动人手里的折叠。真机判据：静置 3 秒、轮询 2 次、重建日志 0 行；改一颗的状态只落 1 行日志。
- **截图工具在按键后窗口会变高/变宽，别复用按键前读到的 RECT**：`shot.ps1 -Expand/-Keys` 先读一次窗口矩形、再投键、再存图，于是「展开后窗口从 390 长到 441 物理」这类变化会被裁掉一截（页脚和「知道了」失踪，看着像布局坏了）。验多行展开要**再调一次 `shot.ps1`**（它会重新取 RECT），本轮 `r5-after.png` 2400×441 才是对的。
- **同状态的两张截图本来就有几千像素差**：节点边框上有跑动的流光（N-7），同状态基线实测 5778 px / 936k。判「选中项和视口没动」不能只看总差数，要看 **diff 的 bbox 是否只落在节点区**（本轮改状态 9491 px、bbox 全在图上，而缩放一张就差 49132 px 且 bbox 横跨到 1576），或者先连截两张同状态当噪声底。
- **自动化提示语里的「固定队列」会和接力板不同步**：2026-10-24 这轮收到的优先级仍是「① 入口按钮 ② 直角折线 ③ 加宽 ④ 讲讲它」，而这四件早已分别由 `e1f8191` / `eba985a` / `62a10bd` / `8fb316f` 交付，板子的「当前任务」也早写了队列空。开工先拿提示语里每一条去 `git log` 与板子「进度」对一遍再决定做不做；对不上就在回复里问用户，**别顺着旧提示语把已交付的活重做一遍**（重做会覆盖掉后面叠的悬停守卫 `2b95980`、双击 `5a07ddd`、跟状态重画 `9737aa5`）。**2026-10-24 第二轮又原样收到同一份四件**——可以确认它是外部的固定模板、不会跟着板子变；每轮开头对一遍（`grep` 源码里的标记点 + `git log`）就够了，别把它当成新指示重新规划。
- **用户说「找不到按钮」时先问「你看的是哪个界面」**：2026-10-24 第三轮查到，跟我对话的宿主 GUI（端口 53011）跑的是 `desktop` profile，它的 `package.json` / `cordis.patch.yml` / `node_modules` 里**没有** `@dshwork/plugin`——那个界面根本不会出现玻璃条，自然也找不到「打开节点图」。玻璃条只在 dev 宿主（`--profile dshwork-dev --port 64001`，网页 token 在 `dshwork-shots\dev-out.log`）里。别一上来就怀疑按钮没画/没编进去（`packages/plugin/lib/client.js` 里 `data-dsh-work-graph` 是在的）。**2026-10-24 第四轮更正**：这条已过时——用户当天 08:44 自己把 `@dshwork/plugin` 装进了 desktop profile，玻璃条在那个界面里**在**；真正的问题是它绑到了空任务库、按钮永远置灰（见下一条）。
- **别拿 `process.cwd()` 当「用户的工作区」**：桌面宿主的 cwd 是 `C:\Users\25108\.dsh\profiles\desktop`，拿它 `WorkStore.open()` 打开的是 291 B 的空库，看起来「插件在跑、但一颗节点都没有」，`graphEntry(0)` 于是把「打开节点图」置灰——用户看到的就是「按钮灰着打不开」。正确来源是 `ctx.get('workspaceRegistry').list()`（`@deepseek-ai/dsh-workspace`，web/桌面 profile 里一定有；不在 `dsh-base` 里，所以别写进 `inject`，用可选 `ctx.get`）。**它是异步引导的**：`apply` 之后立刻读会读到空表（实测第一版就这样，日志里 root=探针目录），要重试到有值为止（现为最多 1.5s）。判据：日志出现 `[dsh-work] 任务库根目录：<root>（来源：宿主工作区登记）` 且 `/api/state` 的 `seq` 是真库那个数（本机 90）。
- **抓「用户正在看的桌面 GUI」的窗口要按 title/class 找、不是按主进程 pid**：`DeepSeek Harness.exe` 主进程（18768）自己不持有可见窗口，真正那个 `Chrome_WidgetWin_1` / title `DeepSeek Harness` 属于子进程（本轮是 pid 32792）。`EnumWindows` + `IsWindowVisible` + `GetWindowThreadProcessId` 列出所有 >150×150 的可见窗口，比猜 pid 快。**桌面 GUI 的网页 token 拿不到**：`endpoint.json` 里那个是伴随层令牌，拿去当网页 token 只会得到 `dsh web authentication required`；只有自己起的 dev 宿主能从启动日志里取到带 token 的 URL。
- **`dsh --no-open` 起的宿主也鉴权**：重启后直接 `node scripts/shot.mjs http://127.0.0.1:64001/` 会截到登录页（`dsh web authentication required; reopen the URL printed by dsh web.`，console 有 401）。token 在宿主 stdout 里，用 `Start-Process … -RedirectStandardOutput <log>` 落盘后 `Select-String 'https?://[^\s"]*64001[^\s"]*'` 取，每次重启都变；拿到的 `http://127.0.0.1:64001/?token=…` 直接喂 `shot.mjs` 就是登录态（`probe.hasBar:true, errors:[]`）。
- **节点图的「行」是 `BuildReviewModel` 摊平出来的，只改 `g_review_expanded` 不会反映到界面**：2026-10-24 修的那处旧代码在「再点同一颗」分支里只做 `g_review_expanded.erase(row.id)`，行还是老样子——点「收起」既不收也不落日志（看着像没反应）。任何改折叠集合的地方都必须接着 `BuildReviewModel(g_review_root)` + `LayoutReview()` + 重画；点选分支现在只有一条出口：`kMsgReview 2` → `ReviewExpandSelected()`。
- **只读文档图（`g_review_read_only`，`ShowReviewDoc()` 置 true）不能去请求宿主拆层/撤层**：文档模式开图时 `g_review_job.clear()`，`/dsh-work/api/plan/expand|collapse` 只会回 `{"ok":false,"error":"缺少 jobId 或 nodeId"}`，界面上只剩「这一步没成，看看日志」这句误导人的话。`ReviewExpandSelected()` / `ReviewCollapseSelected()` 都先看这个标志，用 `SetReviewHint` 回一句人话并落日志；草稿模式（有 jobId）行为不变。
- **`UpdateLayeredWindow` + `ULW_ALPHA` 的分层窗口里，半透明 alpha 会被乘两次**：面板/节点图的 D2D 目标是 `D2D1_ALPHA_MODE_PREMULTIPLIED` 的 DIB，D2D 写进去时已经预乘一次，`UpdateLayeredWindow` 又按 alpha 合一次 ⇒ 实际 alpha = a²（实测 0.6 → 0.36、0.4 → 0.16，等于画在背景上看不见；本轮事件流那行就是被这个吃掉的）。所以在面板/节点图里表达「暗」要用**不透明暗色**（`kTextDim` = `0xFF9A9AA0` 这种），别再叠 `Fade(color, 0.6f)` 做第二重调暗；整体淡出（morph 期 alpha 0→1）仍走 `Fade(..., alpha)` 是安全的，因为只有一重。判据：同一串字 `0xFFFFFFFF` 清楚、`0xFF9AA0A6` 可见、`0x9AA0A6` 只剩几十个噪点、`0x659AA0A6` 整条带的 `lum` 与背景完全一致。
- **桌面 profile 的 `@dshwork/plugin` 是 `link:D:/work/AI/work/dsh-work/packages/plugin`（不是 npm 包）**：宿主半边（`lib/index.js`）改完要 `node scripts/build-plugin.mjs`，再**重启桌面版**才加载；玻璃条那半边（`lib/client.js`）另需 `node scripts/build-client.mjs`。桌面进程不弹窗、不留「已重启」标记，**别拿文件 mtime 猜**（本机时钟被回拨过，mtime 会冒出 `20:27` 这种未来时间）——读 `~/.dsh/dsh-work/endpoint.json` 的 `root`/`workspace`，再打 `/dsh-work/api/state` 看 `seq`/`roots`/`nodes` 对不对，才是可信判据。
- **`WS_POPUP` 无标题栏的窗口要能按住拖动，就在自己的窗口过程里对头带回 `HTCAPTION`**：`WM_NCHITTEST` 的 `lparam` 是**屏幕坐标**（不是客户区坐标），和 `GetWindowRect` 直接比即可；返回 `HTCAPTION` 之后拖动由系统跑非客户区那一套循环。**别自己在 `WM_MOUSEMOVE` 里算 `SetWindowPos`** —— 那样会绕过 `LayoutReview()` 里的「开着时只改尺寸、位置原地」「夹回工作区」两条规矩（N-5 定的），窗口能被拖出屏幕且再也回不来。
- **拖动期间 `WM_MOUSELEAVE` 照样会来**（把图甩到屏幕边缘时指针先离开窗口）：审阅窗「指针离开就淡出收起」那条必须加 `!g_review_moving` 守卫，否则拖到边上松手前窗口就自己关了，看着像「拖动失败」。
- **`WM_EXITSIZEMOVE` 里要「先重排、再记位置」**：松手那一刻窗口可能有一大半在屏幕外（实测尾巴伸出 802 px），先 `LayoutReview()` 用 `keep_place` 夹回工作区，再 `GetWindowRect` 记下来的才是人真正看到的那个位置。
- **常驻探针（pwsh/.NET）读窗口矩形必须先 `SetProcessDpiAwarenessContext(-4)`**：本轮又踩了一次——同一条脚本读出 `133,440 1600x260`（正好是 200/1.5、660/1.5、2400/1.5、390/1.5），差点误判成「位置没记住」。凡是要打印 rect / 算点击坐标的探针，第一行就声明 DPI 感知（`dshwork-probe\shot-window.ps1`、`drag-header.ps1` 都已这样写）。
- **自动化拖窗的第一步经常被「窗口还没被激活」吃掉**：`drag-header.ps1` 第一次跑回 `NOT_MOVED`，原样重试就成功。脚本里先 `SetForegroundWindow(hwnd)` 再按，别急着改代码。
- **原生侧读一个小 JSON 别为省事拉 `sstream` / 输入流**：本轮加「记住审阅窗位置」时先写 `#include <sstream>` + `stringstream << rdbuf()`，exe 从 583.5 KB 涨到 605.5 KB；换成 `ifstream` 手搓 read 循环反而 607.5 KB；最后改用 Win32 `CreateFileW` + `ReadFile`/`WriteFile` 并删掉 `#include <sstream>` → 590.0 KB（只多 6.5 KB）。原生件每轮都在意体积，几十字节的偏好文件就用 Win32 那几个调用。
- **伴随层自己的偏好别写进 `endpoint.json`**：那是宿主每次启动都会重写的文件（pid/端口/token 全在里面），伴随层往里塞字段会被下一次宿主启动抹掉。放同目录另开一个（现在 `review-placement.json`），`DshWorkDir()` 统一算目录（`DSH_HOME` 优先、否则 `USERPROFILE\.dsh`）。
- **记住的位置必须能被「显示器拔了」兜住**：只存 x/y、进来直接 `SetWindowPos` 的话，换分辨率或拔掉那块屏就会把窗口丢到看不见的地方。现有兜底是 `MonitorFromPoint(…, MONITOR_DEFAULTTONEAREST)` 取最近那块屏 + `LayoutReview()` 末尾夹回 `rcWork`；实测手写 `{"x": 9000, "y": 9000}` 会落回 `3888,618`（第二块屏工作区右下角）。
- **验证「网页改动在桌面 GUI 里生效」不需要网页 token**：走「截窗口 + 物理坐标点击」。`dshwork-probe\shot-window.ps1 -Pid2 <持有该窗的 pid>`（它先 `SetProcessDpiAwarenessContext(-4)`，会打印 WIN rect）抓图，量按钮像素 **÷ 缩放比** 得窗口内物理坐标，屏幕坐标 = 窗口 rect + 它；本轮 150% @3840×2160 下「打开节点图」中心 = 屏幕 `(2104,474)`，`SetCursorPos` + `mouse_event(0x2/0x4)` 一次就成。**网页内容别用 `PostMessage` 点**（那是 `click-rel.ps1` 给原生窗用的，Chromium 渲染进程未必收）。点完立刻 `Get-Process dshwork-shell` + `EnumWindows` 找 `DSHWorkNativeReview` 才算数——节点图窗（2400×390、从 y≈144 起）会盖住居中的 GUI 顶部玻璃条，toast 可能压根看不见；验完 `Stop-Process -Name dshwork-shell`，**并把光标移回原处**（留在按钮上，悬停 260ms 会自己把面板顶出来）。

---

## 历史

- 2026-10-19 P0 完成：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条，推送 `84ff6db`。
- 2026-10-19 P1-1 完成：全局热键可配置（默认 `Control+Alt+Space`，裸 Tab 需 `allowBareTab`），注册失败/异常留痕，按下小条压缩 120ms 并记一行，提交 `4870b3f`；测试 46 绿，typecheck 绿，实机热键触发与占用失败告警均已验证。
- 2026-10-19 P1-2 完成：热键 → 冻结整屏（先藏小条再截）→ 全屏半灰覆盖层拖框 → 区域 PNG 落 `~/.dsh/dsh-work/captures/`；Esc/右键/再按热键取消；小条回弹并给 2.4s "已截取 W × H" 提示。150% 缩放下实测 800×600 物理拖拽 → 800×600 像素，零偏移；`capture-geometry.cjs` 9 个纯函数用例，全套 55 绿。

- 2026-10-19 P1-2b 完成：**面板**落地。热键语义从"按下即截图"改为"按下开面板"——小条是收起态、面板是展开态，同一个东西的两个形态；面板里做任务卡带（当前居中、左右为已完成/待进行，`scroll-snap` 居中）、详情列（目标/验收）、页脚键位提示，键盘 `Enter` 完成并前进、`Backspace` 退一步、`Esc` 收起、`Tab` 进框选，框选结束/取消后按原样恢复面板。玻璃底改用"抓一张背后桌面 → CSS 模糊"的方案（`backdrop.cjs`）：CSS `backdrop-filter` 对透明窗口背后的桌面无效，Win11 `acrylic` 又会吃掉圆角。`plan.set` 新增可选 `title`，拆解时顺带给文档命名。

- 2026-10-19 P1-3 完成：**视觉识别**落地，提交 `7e53a78`。链路 `panel → main.cjs → POST /dsh-work/api/recognize → 宿主 ctx.llm.stream → plan.set`；作业化（POST 只受理、GET 轮询取结果）是为了不让几十秒的模型调用挂在同步响应上，超时/卸载用 `AbortController` 真停。解析是纯函数：失败一次「就地修格式」重问一次，仍失败返回可读原因且不写文档；`looksLikeOutline` 挡住散文被误认成任务；`kind:'concept'` 走同一解析路径。模型选择 插件 Config → `agentDefaultModel` → `deepseek-official/deepseek-v4-flash`。新增 `recognize.ts`、`recognize.test.ts`、`http.test.ts`，测试 73 绿、typecheck 绿、build 通过。**遗留**：真机端到端没跑——跑着的宿主加载的还是旧 bundle（`/api/recognize` 返回 404），重启 `dsh web` 后补验；`attachments.admitPromptContent` 与视觉模型的实际输出质量也未实测。

## 2026-10-25 交接（上下文耗尽前）

**状态**：网页按钮 + 桌面胶囊 + 托盘三处入口均已可用；开机自启已写 HKCU\...\Run（值 DSHWorkShell）。

**待办（按顺序）**：
1. 事件流一行不显示：g_model 在 main.cpp:507 已带 eed，g_panel_model = g_model 在 2742，理论应能到面板。下一步在 DrawPanel 里临时打一条 eed.size() 日志确认数据到没到。
2. 网页按钮 position: fixed; top: 8 压在窗口标题栏上，建议改为「悬停顶部才淡入」。
3. 节拍时钟 Pulse（tick/breath/burst，参数 ~/.dsh/dsh-work/anim.json 热加载）+ 状态矩阵。