# 变更日志

## 2026-10-25 · 插件市场上架：预构建包挂 Release，清单提了 PR

不上 npm 也能进市场——清单条目支持 `tarball:`，只要指向 GitHub Release 里以 `.tgz` 结尾的资产。

- 仓库打上 `dsh-plugin` 等 8 个 topics（清单仓库的硬性要求之一）。
- `npm pack` 出 `dshwork-plugin-0.1.0.tgz`（31.3 kB）：`lib/index.js`、`lib/client.js`、`cordis.patch.yml`、`package.json`。只含宿主半边与 web 客户端，Windows 原生伴随层仍要自己 `node scripts/build-native.mjs dshwork-shell`。
- 建 Release `plugin-v0.1.0` 并把包装上去，下载实测 HTTP 200、`tar -tzf` 校验齐全。
- 清单条目 `data/plugins/liyangyu001-hue__dsh-work--packages-plugin.yml`（`category: workflow`、`name: liyangyu001-hue/dsh-work#dshwork-plugin`、`tarball:` 指向资产），经 fork 分支提 PR → **awesome-dsh-plugin#9411**。
- npm 发布仍待定（本机 `npm whoami` 是 `ENEEDAUTH`，包名/作用域也没定）；有了 npm 包之后可以省掉安装时那道 `allowBuilds` 授权。

## 2026-10-25 · 展开一颗不再让整张图「缩水」：缩放和平移留住，只把点到的那颗挪回视野

上一轮修「左键不能拆」时顺出来的同源毛病：`ReviewExpandSelected()` 在 `BuildReviewModel()` 之后无条件 `FitView()`。而 `BuildReviewModel()` 每次都会新建一个 `Model`（`fitted=false`、`zoom=1`、`pan=0`），函数末尾自带 `if (!fitted) FitView()` —— 于是点一下展开，人自己调好的缩放和平移被抹掉、整张图重新适配到 0.888。

- **重建后把视口原样还回去**：`pan_x/pan_y/zoom/fitted` 从重建前的模型拷回（`RefreshReviewDoc()` 一直这么做，只有这条路径漏了）。
- **新写 `RevealRowsInReview(id)`**：取这颗节点 + 它新露出来的子行的世界矩形，按当前 zoom/pan 换算成屏幕坐标；只有越界才挪平移（先保右/下、再保左/上，最后 `ClampPan`），没越界一个像素都不动。展开的内容因此不会跑到窗外，但整张图的缩放不变。
- **日志带上前后 `zoom`**（`审阅：本地展开 <id>（缩放 1.000000 → 1.000000）`），以后再被谁重置一眼就能看出来。
- **真机验收**（mock 宿主）：`+N` 展开，窗口 390→441，日志 `缩放 1.000000 → 1.000000`，`mock 子节点` 可见；`−N` 收起，441→390，同样 `1.000000 → 1.000000`。构建 598.0 KB（只剩两条旧警告），`pnpm test` 130 绿、`pnpm typecheck` 干净。
- **代价（有意取舍）**：内容比窗口宽时不再自动缩小，右侧会被窗口裁掉，靠拖动/滚轮看。要「全都看得见」就还是会缩水——那正是用户不想要的，所以选「不缩水 + 可平移」。若以后想让两者兼得，正路是让窗口在内容变宽时自己加宽（`kWindowW` 那段），而不是把图缩小。

## 2026-10-25 · 「左键不能拆」：给 `+N / −N` 徽标一个真命中区，撤掉会打架的悬停自动展开

用户原话是「左键不能拆 这种小差错 就不要出现了吧 咱们制作时 可以先想想 顶级的代码工程师会怎么做」。先用 mock 宿主复现，发现根本不是「拆」那段代码坏了——叶子节点连点两次的判定是对的（`again=1` → 「只读文档，叶子不请求拆层」），坏的是**手势所在的那块地方会动**：单击开详解卡让窗口 390→522，重建模型时 `fitted` 被重置、`FitView` 又跑一遍（`zoom` 1.000000→0.887615、`pan` 40,48→26,33），同一个屏幕坐标的第二下已经落在别的控件上（实测点到「讲讲它」，还顺手打出 `讲解：失败 HTTP 404 … /api/learn`）；再加上悬停 260ms 自动展开，会在人点掉之后半秒又自己弹开。

- **`+N / −N` 徽标变成真按钮**：`review.h` 抽出 `ExpandBadgeRect(row)`，绘制与 `ExpandBadgeAt()` 共用同一份矩形（命中外扩 3 DIP，和 `LessonChipAt` 一样）；`WM_LBUTTONDOWN` 在「讲讲它」之后、节点之前先判它 → `PostMessageW(kMsgReview, 2)`；`WM_SETCURSOR` 把徽标也当可点，光标变手型。
- **一个左键只留一个意思**：删掉「再点同一颗 = 拆」这个隐式手势（它撑不住任何会重排的交互），单击永远 = 看详解；拆只有两条明路：点 `+N`、或选中后按 `Enter`。页脚同步：悬停有子节点时会多一句「点 +N = 拆开/收起」。
- **删掉悬停自动展开**：`kHoverExpandMs` / `g_review_hover_at` 一并删除。它和显式命令冲突（点收起 → 260ms 后自己弹开），隐式时钟驱动的动作打不过用户的手，宁可去掉。
- **真机验收**（mock 宿主，不碰真实任务库）：点 `+N` → 窗口 390→441 且子节点出现；点 `−N` → 441→390，静置 2s 不再自己弹开；点节点正文 → 详解卡正常；右键仍走完成守卫（「右键这颗已经收尾，不重复标」）。构建 596.5 KB（只剩 `grab_dxgi.h(219) C4127`、`main.cpp(1343) C4715` 两条旧警告），`pnpm test` 130 绿、`pnpm typecheck` 干净。
- **留下的**：展开一颗之后整图会突然缩到 0.888（`fitted` 被重置导致重新适配），不致命但很怪，已写进「队列」当下一轮候选。

## 2026-10-25 · 节点图上「右键完成、左键看详解」：手不用离开图，也不用等模型

用户原话是「节点图加『完成这颗』——做这个」「当鼠标移到子节点上 就能右键 完成 左键 给到详解」。之前完成一颗只能在面板右下点「完成」（`node.advance`）或者全局双击 Tab；节点图上只有双击设当前任务、Enter 展开/收起、概念节点的「讲讲它」。

- **右键一颗 = 标记完成**：`ReviewProc` 收到 `WM_RBUTTONDOWN` 后把物理坐标 ÷ `ReviewScale()` 换成 DIP，用 `review::NodeAt` 找行，记下 `g_review_menu_row` 再 `PostMessageW(kMsgReview, 7)` 回主线程（鼠标消息里不能直接起线程干活，和现有 1~6 号消息同一套规矩）。`CompleteRow()` 先挡三种情况：草稿模式（节点还没落库，宿主认不出 id）、已经 `done`、已经 `skipped`——分别回一句人话，不发请求；其余起后台线程 `PostIntent("node.complete", id)`，宿主那条 intent 本来就认 `nodeId`（`packages/core/src/state.ts:336`）。
- **左键单击 = 就地看详解**：不再只是「选中」。`ShowNodeDetail()` 把这一颗的 `acceptance`（没有就退回 `detail`）铺进现有的讲解卡（`lesson_phase = 2`），状态单独拼一行「已完成 / 已跳过 / 当前任务 · 正在做 / 正在进行 / 待办 · N 步 · Esc 关掉 · 点别处也行」。**全程不请求模型**——这是本地已有的字段，和「讲讲它」（`/api/learn`）不是一回事。双击仍是设当前任务，再点同一颗仍是拆开/收起。
- **顺手补了两个「写了但从没生效」的东西**：① `review::Anim::hover_row` 在 `main.cpp` 里只有声明、没有赋值，所以绘制侧的悬停高亮和悬停页脚提示一直没出现过——`RepaintReview()` 现在把 `g_review_hover_row` 传进去；② 右键把当前任务标完成后，卡片上那句「当前任务 · 正在做」会留在原地（`RefreshReviewDoc()` 原样保留 `lesson_*`），新增 `RefreshDetailCard()`：只有当卡片是「本地详解」时才按最新行重灌，`讲讲它` 的模型讲解不受影响。
- 页脚：只读文档图改成 `左键 = 看详解 · 右键 = 标记完成 · 双击 = 设为当前任务 · Enter 展开/收起 · Esc 关闭`，草稿改成 `左键 = 看详解 · Enter 再拆一层 · Ctrl+Enter 就这样 · Esc 不要了`，悬停时再教一遍；卡片开着时那句从「讲解开着」改成中性的「卡片开着 · Esc 关掉 · ↑↓ 换一颗 · 拖空白平移 · 滚轮缩放」。

验收没有碰真实任务库：起了一个 mock 宿主（`node:` http，把真实 `/api/state` 的一份拷贝里某颗改成 `active`），伴随层用独立 `DSH_HOME` 指过去。左键 → 窗口从 2400×390 长到 2400×522、卡片里是这颗的 acceptance（`rc-detail2.png`）；右键 → 日志「审阅：标记完成 nd_…」「意图已提交：node.complete」，`mock-intents.ndjson` 收到 `{"intent":{"type":"node.complete","nodeId":"nd_01m5fjfadd4ht037a7e"},"by":"human"}`，图跟着状态重画、卡片自己变成「已完成」（`rc-complete.png`）；对已收尾的再点右键，页脚回「这颗已经完成了」（`rc-guard4-footer.png`）。原生 595.5 KB（只剩两条旧警告），`pnpm test` 130 绿、`pnpm typecheck` 干净。

## 2026-10-25 · 节点图连线流光改成「轮动展示」：一次只亮一条边，走完最后一条回第一条

原来 `native/shell/src/review.h` 给每条边错开 `i * 0.11` 的相位（`phase = flow_phase + i * 0.11f`），于是**所有边同时在流**，看上去像一条并行流水线，反而看不出「现在走到哪一步」。按用户「轮动展示」的原话（追问后选定「节点图里节点间的流光线节奏」）改成：

- 整张图共用一个时间槽：每条边占「走完 0.9s + 空档 0.1s」，`active_edge = floor(clock / kEdgeSlotMs) % edge_count`，轮完最后一条回第一条。
- `flow_head` 只在轮到的这条边上从 −18% 扫到 1.0（出发先让尾巴探出来、到达后让尾巴走完）；空档期 `flow_head < 0`，整张图暗一下，眼睛才分得清换了条边。
- 18% 亮弧的判定从「取模回绕」改成「光头减去累计弧长的差」（`behind >= 0 && behind <= kTailLen`），不再跨边回绕。
- 光头前加一颗 2.2 DIP 白点：18% 的尾巴在 313 px 的长边上只有 12 px，轮动时不够扎眼。

真机（1600 DIP @150%，2400×390 物理）：8 张连拍（约 0.8–1.0s 一张）里光头依次落在 x≈1036 → 1358 → 1679 → 2004 →（空档，这一张无光头）→ 386 → 708 → 1031，间距 ≈ 322 px = 一条边，走到最后一条后回第一条；每张只有当前那条边有线光、其余全是灰底线（仓库外 `dshwork-shots\rot-0..7.png`）。原生 590.5 KB（只剩两条旧警告），`pnpm test` 130 绿、`pnpm typecheck` 干净。

## 2026-10-25 · 节点图拖到哪儿，重开还在那儿：位置落盘到 `~/.dsh/dsh-work/review-placement.json`

上一轮加「按住标题栏拖动」时，位置只记在进程内 —— 伴随层一退出就回到屏幕中央。这一轮把它落盘：

- `native/shell/src/host.h`：把「`DSH_HOME` 优先、否则 `USERPROFILE\.dsh`」这段目录解析抽成 `DshWorkDir()`，`EndpointPath()` 复用它；新增 `PlacementPath()` = `DshWorkDir() + L"\\review-placement.json"`。放 `endpoint.json` 同目录、但**单独一个文件**：`endpoint.json` 由宿主每次启动重写，伴随层往里面塞字段会被抹掉。
- `native/shell/src/main.cpp`：新增 `SaveReviewPlacement(int x, int y)` / `bool LoadReviewPlacement(int& x, int& y)`，内容就一句 `{"x": 3888, "y": 460}`。读的一侧容错：文件不存在、JSON 坏了、`x`/`y` 不是数字、超过 4 KB，一律当「没记过」回退到默认居中；写的一侧失败也不打断拖拽。`WM_EXITSIZEMOVE` 记完位置顺手保存；`EnsureReviewWindow()` 开头懒加载一次（`g_review_placement_loaded`），读到就落日志「审阅：接着上次的位置 (x, y) 开」。
- 位置进来之后仍然走上一轮那套 `MonitorFromPoint(…, MONITOR_DEFAULTTONEAREST)` + `LayoutReview()` 末尾夹回 `rcWork`，所以拔掉显示器/改分辨率不会把窗口丢到看不见的地方。

真机三步（每步都是真的杀掉进程重开）：① 删掉文件 → 起伴随层开图落在 `720,144`（居中）→ 拖到第二块屏 `3888,460 1872x390`，文件写出 `{"x": 3888, "y": 460}`；② `Stop-Process` 后起**新进程** → 空窗直接落在 `3888,460 1872x390`，日志「接着上次的位置 (3888, 460) 开」，7 颗节点全在窗内（仓库外 `dshwork-shots\reopen-remembered.png`）；③ 手写坏值 `{"x": 9000, "y": 9000}` 再重启 → 落回 `3888,618`（第二块屏工作区右下角），没跑出屏幕（`reopen-bogus.png`）。

原生 590.0 KB（只比拖动前的 583.5 多 6.5 KB；中途试过 `sstream`/`ifstream` 读文件，白涨 15~17 KB，最后改用 Win32 `CreateFileW` + `ReadFile`），只剩 `grab_dxgi.h(219) C4127`、`main.cpp(1346) C4715` 两条旧警告；`pnpm test` 130 绿、`pnpm typecheck` 干净。

## 2026-10-25 · 审阅窗能按住标题栏拖了：松手记住位置，拖到另一块屏按那块屏的宽度重排

节点图窗口是 `WS_POPUP` 的分层窗、没有标题栏，也从没处理过 `WM_NCHITTEST`——在双屏上想把它挪到另一块屏，只能靠系统窗口菜单或者干脆重开。这一轮补上：

- `ReviewProc` 新增 `WM_NCHITTEST`：`lparam` 是屏幕坐标，落在窗口顶部 `kHeader`（58 DIP）× `ReviewScale()` 的带内就回 `HTCAPTION`。头带里本来只有标题和「N 件事」，没有可点控件，所以不用给控件抠洞。返回 `HTCAPTION` 之后拖动完全交给系统那一套非客户区循环，不再自己算 `SetWindowPos`——否则会绕过 `LayoutReview()` 里「开着时只改尺寸、位置原地」和「夹回工作区」两条 N-5 定下的规矩。
- `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE` 维护 `g_review_moving`；松手时**先** `LayoutReview()` 把窗口夹回所在屏的工作区（实测松手那一刻尾巴能伸出屏幕 802 px），**再** `GetWindowRect` 记住位置并落日志「审阅：拖到 (x, y)，下次打开还从这儿开」。
- `LayoutReview()` 增加「先定屏」：没开着、且人拖过位置时，用 `MonitorFromPoint` 取那块屏，让 `ReviewAvailWidthDip(HMONITOR)` 按**那块**屏算可用宽。原来只按小条/窗口所在屏算，重开时会把 1600 DIP 的宽窗摆到 1920×1080 的窄屏上。`EnsureReviewWindow()` 落位后补一次 `FitView + ClampPan`，否则窄屏上最右边那几颗节点会留在窗外。
- `WM_MOUSELEAVE` 的自动收起加了 `!g_review_moving` 守卫：拖到屏幕边缘时指针会先离开窗口，不绕过它就会出现「拖到一半自己关了」。`WM_SETCURSOR` 在 `HTCAPTION` 上给 `IDC_SIZEALL`，让人看得出这条能拖。

真机验收（用户当晚接上的第二块屏 1920×1080 @150%，主屏 3840×2160 @150%）：主屏 `BEFORE 720,144 2400x390` → 拖到第二块屏 `AFTER 3888,460 1872x390`，日志「审阅：换屏后按新工作区重排，可用宽=1248」+「审阅：拖到 (3888, 460)」；关掉再开 `keep=0 placed=1` 停在原处、7 颗节点全在窗内；拖回左上角 `AFTER 0,580`（x 被工作区左边界夹住）。截图在仓库外 `dshwork-shots\drag4-monitor2.png`、`drag4-reopen.png`、`final-drag.png`。原生构建 583.5 KB，只剩 `grab_dxgi.h(219) C4127`、`main.cpp(1298) C4715` 两条旧警告；`pnpm test` 130 绿（第一次跑挂了一条 `store.test.ts` 的 `ENOTEMPTY` 清理竞态，重跑即过）、`pnpm typecheck` 干净。

位置目前只记在本次进程内：跨重启落盘、拖到屏边要不要吸附，都写进 `docs/NEXT.md` 的「等用户决定」等拍板。

## 2026-10-25 · 面板底部事件流一行终于画出来了：分层窗口把 alpha 平方了一次

交接板上挂了两轮的「面板底部事件流一行从来不显示」修掉了。查法按板上留的线索走：先补一行永久日志确认数据到没到——`[02:42:32] 事件流：3 条`，说明 `/dsh-work/api/feed` 与每秒轮询都正常（`main.cpp:445-480` 拼 `at + 双空格 + verb + " · " + text`，`:505-508` 进 `g_model`，`:2754-2769` 进 `g_panel_model`）。再在 `main.cpp:1497` 前打 `DrawFeed` 的入参，稳态 `n=3 alpha=1.000000 px=1494x234`——调用与尺寸都对。接着往 `DrawFeed` 里塞一条暗红 `FillRound`：带子画出来了，文字仍然没有，于是问题锁死在「这段文字本身画不出来」。

颜色阶梯很快给出答案：同一串 `23:59  推进 · 完成当前并前进`，`fmt.body` + `0xFFFFFFFF` 清楚、`fmt.meta` + `0xFF9AA0A6` 可见、`0x9AA0A6`（alpha 0.6）只剩几十个噪点、`0x659AA0A6`（0.4）与背景完全无差（逐像素 `lum min=max=27`）。面板是 `UpdateLayeredWindow(ULW_ALPHA)` 的分层窗口，D2D 目标是 `D2D1_ALPHA_MODE_PREMULTIPLIED` 的 DIB——alpha 被预乘了**两次**，0.6 实际变 0.36、0.4 变 0.16。而事件流那行写的正是 `Fade(0x9AA0A6, alpha*0.66f)`，约 0.4，于是永远看不见。

改法：`native/shell/src/panel.h` 的 `DrawFeed` 改用不透明暗色 `Fade(kTextDim, alpha)`（与「7/7 已完成」同档灰，整体淡出仍交给 alpha），并把这段原因写进函数注释，免得下一轮又有人用半透明去调暗；顺手删掉 `DrawPanel` 里那份被面板底盖住、从来没生效过的重复画法（原 `panel.h:323-332`）和全部临时探针。

真机验收（`POST /dsh-work/api/ui/open {"target":"panel"}` 打开面板 + DPI-aware 抓窗）：左下角出现 `23:59  推进 · 完成当前并前进`（仓库外 `dshwork-shots\panel-feed-fixed.png`）。原生构建 582.5 KB，只剩 `grab_dxgi.h(219) C4127`、`main.cpp(1290) C4715` 两条旧警告；`pnpm test` 130 绿、`pnpm typecheck` 干净。

## 2026-10-24 · 队列真的空了：四件老活逐条对账，两件设计取舍连落地位置勘定好，等用户点头

**零代码改动**的一轮。自动化提示语每次仍把「① 入口按钮 ② 直角折线 ③ 加宽 ④ 讲讲它」排成优先级，但这四件早已分别由 `e1f8191` / `eba985a` / `62a10bd` / `8fb316f` 交付，接力板「当前任务」也早写了队列空。开工照「已知的坑」最后一条逐条对源码：

- 入口：`packages/plugin/src/client/index.tsx:297`（`data-dsh-work-graph="1"`）与 `:326`（「打开节点图」）
- 直角折线：`native/shell/src/review.h:473`（`BuildEdge`），`:569` 调用
- 宽度：`review.h:111` 的 `constexpr float kWindowW = 1600.0f`
- 讲讲它：`review.h:723-748` 的 pill，`:804` 页脚提示里也露过脸

一件不差，所以没有重做（重做会覆盖后面叠的悬停守卫、双击、跟状态重画）。工作区干净、HEAD `e72a533`；`pnpm test` 130 绿、`pnpm typecheck` 干净。

剩下两件都卡在产品取舍上，按红线不擅自改，但**把落地位置提前勘定好**写进接力板「当前任务」，用户点头即可开工：① 审阅窗拖动 → `ReviewProc`（`main.cpp:1849`）加 `WM_NCHITTEST`（`ScreenToClient` 后用 `kHeader × g_reviewScale` 判头带；头带没有可点控件，不必抠洞）；② 恢复 `kMinFitZoom` → `review.h:105` / `:278` 两处被注释里的**字面 `\n`** 吞掉，拆行即可，代价是窄窗口里内容会被裁。

## 2026-10-24 · 桌面 GUI 入口实测：玻璃条 7/7、「打开节点图」可点、真点一次弹出节点图窗口

上一轮修掉「插件绑空任务库」之后，玻璃条本该亮起来。这一轮去**用户真正在用的 desktop profile**（宿主 pid 30624、端口 57758）把它验实，**零代码改动**：

- `~/.dsh/dsh-work/endpoint.json` 已是 `root=D:\work\AI\work` / `workspace=work`（`publishEndpoint` 写的），带同目录 token 打 `/dsh-work/api/state` 回 200、`seq=90 roots=7 nodes=7`——桌面宿主绑的是真库。
- 窗口截图（仓库外 `dshwork-shots\desktop-gui-after-restart.png`）：玻璃条「● 全部已完成 7/7」+ 描边「打开节点图」高亮可点；对照上一轮 `dshwork-shots\desktop-gui.png` 的计数「—」与灰按钮。
- **DPI-aware 真点一次**（屏幕物理 `2104,474`，150% @3840×2160）：1s 内 `dshwork-shell` 被拉起（pid 65004），`DSHWorkNativeReview` 2400×390 弹出，7 颗概念节点、每颗「讲讲它」胶囊、页脚键位提示齐全，标题正是「搞懂幂等及其与反复重试的关系」（`dshwork-shots\desktop-graph-window.png`）。验完 `Stop-Process -Name dshwork-shell` 并让光标归位，桌面不留窗。

`pnpm test` 130 绿、`pnpm typecheck` 干净。四件老队列（入口 / 直角折线 / 加宽 / 讲讲它）到此在用户真正看的界面上全部到位，入口这条结案。

## 2026-10-24 · 只读文档节点图：再点已选中节点不再请求宿主拆层，点收起也真的会收起

接力板「等用户决定」第①条（板上注明「纯 bug、修法唯一、不动产品方向，建议先做」）。在只读文档图里再点一颗已选中的节点，原来有两条错路：

- **叶子节点会去请求宿主拆层**：`ReviewProc` 的 `again` 分支发 `kMsgReview 2` → `ReviewExpandSelected()` **无条件** POST `/dsh-work/api/plan/expand`，而文档模式没有 jobId（`ShowReviewDoc()` 里 `g_review_job.clear()`）→ 宿主回 `{"ok":false,"error":"缺少 jobId 或 nodeId"}`，界面上只留一句「这一步没成，看看日志」。
- **点父节点「收起」其实没反应**：`again` 分支就地 `g_review_expanded.erase(row.id)` 就完了，可行是 `BuildReviewModel` 摊平出来的，集合变了界面没重建——既不收起也不落日志（这条是本轮顺手翻出来的，接力板原来只记了上面那条）。

改 `native/shell/src/main.cpp` 三处：① `again` 只保留唯一出口 `PostMessageW(g_window, kMsgReview, 2, 0)`（删掉就地擦集合的分支，并注明「行是 BuildReviewModel 摊平出来的，集合变了界面却没重建」）；② `ReviewExpandSelected()` 在起后台线程前加 `g_review_read_only` 守卫：页脚 `SetReviewHint(L"这颗已经到底了，文档图不能再拆")` + 日志「审阅：只读文档，叶子不请求拆层 <id>」；③ `ReviewCollapseSelected()`（VK_BACK）同样守卫，回「文档图是只读的，撤不了这一层」。草稿模式（`g_review_job` 在、`read_only=false`）行为不变。

真机验证（仓库外假宿主 shim + 真伴随层，1600 DIP @150%，`DSH_HOME` 指到临时目录）：① 点概念叶子一次 → 选中环过去；② 再点一次 → 页脚亮「这颗已经到底了，文档图不能再拆」、无失败提示，shim 侧 `planHits:0 expandHits:0`；③ ↑+Enter 展开父节点 → 日志「审阅：本地展开 nd_a」、窗口 390→441、徽标 `+2`→`−2`；④ 展开态再点父节点一次 → 日志「审阅：本地收起 nd_a」、窗口回到 390、徽标回 `+2`——证明删掉就地擦集合后点收起真的会重建。原生日志全程没有「审阅再拆 失败」。

`pnpm test` 130 绿、`pnpm typecheck` 干净（本轮只改原生）。

## 2026-10-24 · 修掉「打开节点图」永远置灰：插件改为跟随宿主工作区登记

用户说的「在dsh界面装个打开的按钮 不然不知道怎么打开」——按钮早在 `e1f8191` 就有，真正的问题是它**永远是灰的**：插件拿 `process.cwd()` 当任务库根目录，而桌面 profile 的 cwd 是 profile 目录，于是绑到 291 B 的空库（0 颗节点），`graphEntry(0)` 按设计置灰；真库在 `D:\work\AI\work\.dsh-work\`（`seq=90`、7 颗节点）。

- 新增 `packages/plugin/src/root.ts`：`pickRoot` / `pickWorkspaceRoot` / `listWorkspaces`——**插件配置 `root` > `ctx.get('workspaceRegistry').list()` 里带会话的那个工作区 > 进程 cwd**。不把 registry 写进 `inject`（硬依赖只有 `tools` / `webServer`，加进去会让没这个服务的宿主整条装不起来），改用可选 `ctx.get`，读不到就回落 cwd。9 个单测。
- `packages/plugin/src/index.ts`：`initialize()` 先 `await resolveRoot()`。registry 是异步引导的（第一版只读一次拿到「服务还没起」，日志里 root 是探针目录），改成最多等 1.5s（15 × 100ms，只在 source 仍是 cwd 时等），并落一行「任务库根目录：… （来源：插件配置/宿主工作区登记/进程 cwd）」。
- 真机决定性验证：故意从**错误 cwd** `D:\work\AI\work\dshwork-probe` 起 dev 宿主（正是桌面 profile 的失效形状）→ 日志「来源：宿主工作区登记」、`/api/state` 回 `seq=90 nodes=7`；随后截 dev 宿主（1440×900，仓库外 `dshwork-shots\devhost-bar.png`）玻璃条 `7/7` + 「打开节点图」可点（对照桌面 GUI 那张：无计数、灰掉）。
- **桌面版要重启一次**才会加载新 bundle；重启后「打开节点图」应当从灰变亮。
- `pnpm test` 130 绿、`pnpm typecheck` 干净。

## 2026-10-24 · 第三轮核实四件老队列，并查到「入口只在 dev 预览里」

同一份固定提示语第三次把「① 入口按钮 ② 直角折线 ③ 加宽 ④ 讲讲它」排成优先级。逐条在源码和 `git log` 里对：四件分别对应 `e1f8191` / `eba985a` / `62a10bd` / `8fb316f`，全部已交付，本轮零代码改动。

- `scripts/shot.mjs` 截了正在跑的 dev 宿主（1440×900，仓库外 `dshwork-shots\entry-check.png`）：玻璃条「打开面板」右侧就是「打开节点图」，`disabled=false`、无 console 错误。
- **新发现**：跟我对话的这个 GUI（端口 53011）是 `desktop` profile，`package.json` / `cordis.patch.yml` / `node_modules` 里都没有 `@dshwork/plugin`——那个界面不会出现玻璃条，「找不到入口」很可能是看错了界面。已把「要不要装进 desktop profile」写进 `NEXT.md` 的「等用户决定」。
- `pnpm test` 121 绿、`pnpm typecheck` 干净。

## 2026-10-24 · 真机复核三件（入口按钮 / 悬停守卫 / 双击设当前任务），无代码改动

用户这轮的固定队列仍把「在 DSH 界面装个打开的按钮」排在第一。先核对：它已在 `e1f8191` 交付——玻璃条「打开面板」右侧那枚描边「打开节点图」就是。按接力板「没有新指示就复查最近三条交付」的约定做了真机复测。

- **入口按钮**：Playwright（1440×900，带网页 token）读到两枚按钮 `disabled=false`；冷启动点「打开节点图」→ 小条浮出「正在启动节点图…」→ 伴随层被拉起，`DSHWorkNativeReview` 2400×390 物理可见、7 颗概念节点全在；再点「打开面板」→「桌面面板已打开」，按「宿主要 panel 时先收图」的规矩图自动收起。无 pageerror / console.error。
- **悬停守卫（`2b95980`）**：图开着时光标停在药丸上 900ms 面板不冒；光标不动用键盘 Esc 关图后 1.4s 面板仍不冒（`AdoptHoverOnBar` 生效）；移开再回来才正常开面板。三条路径全过。
- **双击设当前任务（`5a07ddd`）**：双击一颗已 done 的节点 → 选中环跟过去、页脚给「这一步已经做完了，不能再当当前任务」、不往宿主发注定 409 的意图。
- 结论：没有回归，本轮不改代码。`pnpm test` 121 绿、`pnpm typecheck` 干净。队列仍是空的，下一件仍等用户从「等用户决定」三件里拍板。
- 顺带记一个坑：自动化提示语里的队列还会是已交付的老四条，和接力板不同步——见 `docs/NEXT.md`「已知的坑」最后一条。

## 2026-10-24 · 节点图开着时跟着整份状态变，不再只认 `doc.pointer`

别的窗口里把某一颗做完了、只要当前任务没换人，图上的圆点就一直停在旧的 todo，得关掉重开才变。

- **判定改成语义签名**：新增 `ReviewDocSignature()`，拼 `pointer` + `roots` 顺序 + 每颗的 `status|kind|title|handoff.state|children`；`BuildReviewModel` 每次重建顺手记进 `g_review_signature`，轮询那边签名相同就直接 `return`。不拿 `/api/state` 原文比——字段顺序、`seq`、`ui` 每次都动，那会变成每秒重建。
- **重建不夺人手里的东西**：选中项按 **id** 记、按 **id** 复原（行号会随树形状整体挪位）；只有 pointer 真换人才把选中跟过去、并展开它的祖先，单纯状态变化不动人手里的折叠；pan/zoom/fitted/讲解卡照旧逐字段恢复。
- **日志分两档**：`节点图跟着状态重画：当前任务 → <id>` / `节点图跟着状态重画：状态变了（当前任务没换人）`，便于以后查「到底是谁触发的重画」。
- 真机（假宿主收 `node.complete` 意图 + 真窗口截图，1600×260 DIP @150%）：滚轮缩到 1.25× 后把一颗概念节点改成 done → 圆点当场变绿，逐像素对比只差那颗圆点与边框流光（9491 px / 936k，bbox 全在节点区），选中环与视口一个像素没动；静置 3 秒、轮询 2 次、重建日志 0 行。`pnpm test` 121 绿、`pnpm typecheck` 干净、原生 580.5 KB。

## 2026-10-24 · 节点图上双击一颗节点 = 把它设成当前任务

`node.focus` 宿主侧早有（只让 todo/active 上位），原生也早已接双击，缺的是三件收尾。

- **看得见当前任务**：`Model.current_id` 取自 `doc.pointer`，当前那颗节点左侧画一条强调色竖标——状态点和转圈都在动，静止一帧里看不出人站在哪颗；pointer 也可能指着颗 todo，那时没有转圈可看。
- **状态回来图自己重画**：新增 `RefreshReviewDoc()`（每轮轮询对一次 pointer，换了人才重建）；重建前用 `children` 反推父级、把当前任务的祖先摊开，重建后原样恢复 pan/zoom/fitted/讲解卡、选中跟到新的那一颗、窗口按新行数长高。
- **被拒时当场回话**：双击 done/skipped 节点给页脚「这一步已经做完了/跳过了，不能再当当前任务」，已经是当前给「它已经是当前任务了」；新加 `Model.note` + 页脚 `hint_ink`，一句话提示不吃按钮。
- 手势仍是**双击**：单击要留给选中/展开，且 `node.focus` 会挪走当前任务，光看图不该改动进度（单击 vs 双击记进 `docs/NEXT.md`「等用户决定」）。
- 真机（假宿主 + 真伴随层 1600 DIP @150%）四步全过；`pnpm test` 121 绿、`pnpm typecheck` 干净、原生 575.0 KB。

## 2026-10-23 · 真机徽标复核（N-4 关闭）

接力板 N-4「+N / −N 展开后消失」定案：真机复现不出。

- 用工作区真实文档（7 颗概念节点）临时改成「根 + 6 子」，1600 DIP 一长条（2400×390 物理 @150%）实测：打开「+6」→ 按一次 Enter 后「−6」且 6 颗子节点并排画齐，徽标稳定在节点框内右上角，没消失；验完 `work.json` 已还原。
- 结论写进 `docs/NEXT.md` 的 N-4 段（含截图路径与探针用法）；本条关闭，不再单开一轮。
- 无代码改动（只改文档）；`pnpm test` 121 绿、`pnpm typecheck` 干净。

## 2026-10-23 · 事件流 /api/feed：把日志翻成人能读的一行

接力板「看不见过程」。这层只做翻译、不做存储——原始事实仍然只有 `journal.ndjson` 一份。

- **翻译**：`feed.ts` 的 `describeEntry()` 把意图翻成「动词 + 文本」（拆解 N 件事 / 完成 / 切到 / 交给 AI / 接手 / 交付 / 验收 / 退回…），`text` 多数取节点当前标题（日志只存 id，标题会变，翻译时现取）；未知意图照常显示类型名，不被静默吞掉。`plan.add/remove/rename` 与 `node.skip/evidence` 目前没有代码路径会发，走 default 分支。
- **只读尾巴**：`store.tailJournal(limit)` 从文件尾往前收，日志涨大也不拖慢开面板；半行（写到一半断电）跳过即可。
- **路由**：`GET /dsh-work/api/feed?limit=30`（上限 100）；宿主没注入能力回 503，读失败回 `{ ok:false, items:[] }` 不抛。
- **收口**：`at` 原本原样带出 epoch 毫秒（`clock()` 写了却没被用上，本地 HH:mm 其实没生效），改成 `clock(line.at)`，`JournalLine.at` 类型同步放宽成 `number | string`；补 `node.back` / `node.advise` 两个动词。
- 测试补 3 处（毫秒数与 ISO 串都换算、认不出的时间宁可空着、两个新动词）；`pnpm test` 121 绿、`pnpm typecheck` 干净。

## 2026-10-23 · 节点图与面板按各自那块屏的缩放绘制

接力板 N-8。`g_dpiScale` 原来全进程一个值，节点图 / 面板被挪到 125% / 200% 的屏上时，窗口尺寸、字号、命中区都按小条那块屏的比例算。

- **每窗一份缩放**：新增 `ScaleOfWindow(HWND, fallback)`（全仓唯一的 `GetDpiForWindow`）与 `g_panelScale` / `g_reviewScale`；`UpdatePanelScale()` / `UpdateReviewScale()` 变化时各落一行日志。`PanelTargetRect(monitor, scale)` 显式接缩放；`ReviewAvailWidthDip`、`LayoutReview`、`RepaintReview`、`HitTestDot`、`PanelProc` 四处命中、`ReviewProc` 四处鼠标坐标全部改读各自那份 —— 画在哪、点在哪同源。
- **换屏重排**：两个窗口都处理 `WM_DPICHANGED`。审阅窗 `UpdateReviewScale` + `LayoutReview` + `ClampPan` + 重画，刻意不 `FitView`（保住人自己的缩放 / 平移）；面板先收掉展开 morph 再按新缩放摆回目标位；**收起** morph 途中直接返回，否则面板会被重新摆成展开尺寸、再也关不上。
- 验证：真机 1.5x 走完 bar → 面板 → 节点图（面板 1494×198、节点图 2400×390 物理，与改前一致，无回归）；仓库外探针件伪造三窗同刻不同缩放（bar 125% / panel 250% / review 200%），窗口物理尺寸 = DIP × 该窗 scale，同一 DIP 坐标在 100% / 200% 下都命中同一颗节点，面板右上 × 两档缩放下都关得掉。
- 踩坑：`WM_DPICHANGED` 既 `PostMessage` 不了（1159 `ERROR_MESSAGE_SYNC_ONLY`）也 `SendMessageTimeout` 不进去，单屏验重排只能在探针件里另挂一个可投的消息触发器。

## 2026-10-23 · AI 会话在跑时，节点图与面板边缘亮蓝边 + 流光

接力板 N-7。数据只认 `/api/state` 节点 `handoff.state == "running"`（`agentSessionId` 只用于日志核对，不拿 `status=active` 猜）。

- **整窗蓝边 + 流光**：`doc.h` 把 `agentSessionId` 带进 `NodeView`，新增 `HasRunningAgent()` / `RunningAgentSession()`；`panel.h` 新增圆角矩形周长参数化 `FramePoint()` 与 `DrawAiFrame()`——三圈随呼吸变亮的 accent 边 + 沿边顺时针 6.4s 一圈的锥形流光（分层窗下用圆点叠，不用 `DrawLine`）。
- **两个窗口都接上**：节点图画在 `review::Draw` 最后（22 DIP 圆角，不挡节点/页脚/按钮）；面板画在 `DrawPanel` 最后，单独恢复 `Scale(scale)`，不跟内容 zoom 缩。`BuildReviewModel` 开窗即按 running 置位，不依赖后续状态变化。
- **面板自己的计时器**：面板默认静止，不能借 240Hz 的 morph 计时器整块重画。新增 `kAiRingTimerId = 0x7D / 16ms`，AI 停或 `HidePanel` 立即 KillTimer；节点图沿用既有动画计时器。
- 踩坑：`0x7C` 与既有 `kBtnPulseTimerId` 撞车，`WM_TIMER` 先命中按钮脉冲分支，环只动了一帧。全仓搜过再定 `0x7D`。

真机 1.5x（仓库外假宿主）：节点图/面板 on/off 截图；120ms 间隔三帧 hash 全不同，流光在走；面板环 60fps 时 CPU 6.0%/6s，AI 停后 0.8%/6s。`pnpm test` 115 绿、`pnpm typecheck` 干净。

## 2026-10-23 · 审阅窗：换到另一块屏后，宽度跟着那块屏走

接力板上 N-2 留下的问题——`avail_w` 只在开窗那一刻按**小条**那块屏算一次，换屏后还拿着老屏的数。

- **认窗口自己的屏**：新增 `ReviewMonitor()`，开着就 `MonitorFromWindow(g_review, MONITOR_DEFAULTTONEAREST)`，没开才退回小条那块（那是它马上要被摆上去的屏）。`ReviewAvailWidthDip()` / `LayoutReview()` 都改用这个。
- **开着时只改尺寸、不换位置**：`LayoutReview()` 原来每次都按小条那块屏重新居中，换屏后一展开节点整块跳回去；现在开着就原地改宽高，再夹回当前屏工作区（长高之后尾巴不会沉到屏幕外）。
- **换屏才重排**：新增 `RefitReviewToMonitor()`，`ReviewProc` 收到 `WM_WINDOWPOSCHANGED` / `WM_DISPLAYCHANGE` 时重算；可用宽没变就一个像素都不碰，变了才 `Layout` + `FitView`（旧平移是按老屏算的）+ `ClampPan` + 重画，并落一行日志。

真机 1.5x（本机只有一块屏，用临时补丁把「挪到右边」模拟成 1100 DIP 工作区）：挪窗后窗口自动缩到 1650×390 物理、内容重新居中不被截断、日志「审阅：换屏后按新工作区重排，可用宽=1100」；可用宽没变时挪窗不重排，之后按 Enter 展开，窗口原地长高、x/y 不动。补丁已删并重编。`pnpm test` 115 绿、`pnpm typecheck` 干净。

## 2026-10-23 · 审阅窗：刚打开就有选中项，Enter 立刻能展开

接力板上挂着 N-4「展开后 +N / −N 徽标消失」。先把这条假设证伪，再修掉同一条线上真机真按不动的那处。

- **证伪「`has_children` 在重建后丢了」**：全仓只有 `BuildReviewModel` 一处给它赋值（取 `children` 数组长度），绘制处还在节点框内、被 `PushAxisAlignedClip` 裁着。用假宿主 + 真窗口把两条数据形态都试了：doc（`nodes` 是对象）「+2」→ 展开「−2」；draft（`nodes` 是数组）同样「−2」，子行都在。待办句最早写在 `fb114bd`，那时徽标还在右下角、与第二轮标题/讲解 pill 抢位置，`aa5ea03` 挪到右上角后这个现象就复现不出来了。
- **真机上真正按不动的是 Enter**：`ShowReviewDoc` / `ShowReview` 重建前没有行被选过，`selected` 是 −1；`BuildReviewModel` 只夹了上界，于是 `ReviewExpandSelected` 静默 `return`——底栏却写着「Enter 展开/收起」。改成三支：空图 −1、负数或越界回 0。
- **再拆回来不再重复重建**：`kMsgReview` 的 case 1 拿到新草稿后又按"同索引"往 `g_review_expanded` 塞一次再重拉。展开/拆细后行序会变，同索引已不是刚拆的那颗，会摊开无关节点；而 `ReloadReviewDraft` 早按 `g_review_pending_expand` 展开过。改成重新自适应视野（`FitView` + `ClampPan`）。
- 「没有选中的行」这条早退路径补了一行日志，免得以后又是"按了没反应"却查不到。

真机 1.5x：打开节点图后**单发 Enter** 直接展开根节点（日志「审阅：本地展开 nd_a」，徽标 +2 → −2）；选到没有子节点的行按 Enter → 宿主 `/api/plan/expand` → 草稿模式加载、根节点按 pending 展开、视野重新适配、没有别的节点被误展开。`pnpm test` 115 绿、`pnpm typecheck` 干净。

## 2026-10-23 · 玻璃条上多了「打开节点图」：一眼找得到入口

用户原话是「在dsh界面装个打开的按钮 不然不知道怎么打开」——玻璃条上原来只有一枚「打开面板」，节点图还得先进面板再点当前任务，路径两跳、没人猜得到。宿主的 `POST /dsh-work/api/ui/open` 早就认 `target:"graph"`，缺的只是这颗按钮。

- **第二入口**：「打开面板」右侧加一枚描边 ghost 按钮「打开节点图」（图标 = 两颗上游圆 + 一颗下游圆 + 连线），点一下直接开只读节点图。层级上不抢主按钮：实心=主入口，描边=第二入口。
- **没有任务树就置灰**：原生 `ShowReviewDoc` 在文档为空时直接 `return`，点下去表现就像「没反应」；所以 UI 侧先拦——`nodeCount === null`（状态还没到）说「正在读取任务状态…」，`0` 说「先在面板里拆一次任务」，理由写进悬停 `title` 与 `aria-label`。
- **两个入口共用一条路**：`openPanel` 改成 `openCompanion(target)`，只发意愿、不碰另一个进程的 Win32 窗口；四档反馈（打开了 / 正在起 / 没接上 / 宿主报错，失败优先用宿主给的 hint）抽到 `packages/plugin/src/client/open-entry.ts`，配 6 个单测。
- **不动宽度**：玻璃条仍 `min(560px, 64vw)`，两枚按钮固定开销约 303px，1440 宽下标题还剩约 257px，不挤。

真机 1.5x DPI：1440×900 截图里两枚按钮并排、节点图按钮为可点态；`click` 后 1.2s 小条浮出「节点图已打开」，伴随层被宿主顺手拉起，审阅窗（物理 2400×390 = 1600×260 DIP）自动弹到前台，7 颗节点全在。`pnpm test` 115 绿、`pnpm typecheck` 干净。

## 2026-10-23 · 节点图上的概念节点有了「讲讲它」

概念节点本来就只画一个「学」字，点了什么都没发生——宿主 `POST /dsh-work/api/learn` 早就通了，缺的只是入口。

- **入口**：「学」换成可点的 pill「讲讲它」（46×17，同一套 accent + `fmt.meta`），标题第二行给它让出 42 DIP，长标题不再被按钮压住。悬停高亮，`WM_SETCURSOR` 变手型。
- **先画再发**：点下去立刻切「讲解中…」、卡片撑开放到图区底部（窗口同步长高，不盖节点），再起后台线程 POST。模型要二三十秒，等 HTTP 回来才刷就等于「点了没反应」——这个项目已经踩过一次。
- **卡片**：标题 + 正文（≤120 字，按像素折行到 2 行）+ 一行练习提示；Esc / 点卡片外收起，收起后窗口缩回原高；再点另一颗直接换内容。
- **失败说清原因**：宿主回包新增 `view`（`title/text/hint/unsupported`）。四档文案——宿主没这个接口（按钮置灰、以后不再发请求）/ 没有这个节点 / 模型没答上来 / 连不上宿主，都不再只有一句「失败了」。
- **顺手修的真 bug**：审阅窗的按键会被两条路各投一遍（窗口有焦点时的 `WM_KEYDOWN` + 动画定时器里 `GetAsyncKeyState` 的按下沿）。240Hz 下两遍间隔 < 60ms，按一次 Esc 会**先收卡再收窗**——截图验证时整个节点图直接消失。改成同一个键 220ms 内只认一次（不能删掉任一条路：焦点不保证拿得到）。

真机 1.5x DPI 验证：点第 7 颗 → 6s 后卡片出现在底部（窗口 2400×390 → 2400×522 物理）、正文与练习提示完整；点卡片外收起回 390；重开再点 → Esc 后窗口仍在、高度回 390（修复前是 `vis=False`）。宿主侧 `learn.test.ts` 12 例 + `http.test.ts` 补路由级断言，`pnpm test` 109 绿、`pnpm typecheck` 干净。

## 2026-10-22 · 节点图默认宽度 1320 → 1600 DIP，屏幕窄时自己收窄

一长条按行号横向排开，1320 DIP 只放得下 5 颗多；用户要「一长条一次能看见更多节点」。

- **默认宽 1600 DIP**：一颗占 224 DIP，1600 正好装满 7 颗（7×224−48+2×26 = 1568），且不占满整个屏宽，仍看得出是浮层。
- **屏幕工作区兜底**：`Model.avail_w` 记「这块屏能给的宽度（DIP）」，`WindowWidth/BodyWidth` 在它比 1600 窄时用它；`main.cpp` 的 `ReviewAvailWidthDip()` = `rcWork` 宽 ÷ `g_dpiScale` − 两侧各 16 DIP，下限 640。排版、自动适配、平移夹取、点击命中全走同一对函数，不会各算各的。
- **开窗前就定宽**：`BuildReviewModel` 在 `Layout/FitView` 之前填 `avail_w`，避免「先按 1600 排好再改窗口」；`LayoutReview` 每次弹窗重算一遍，换显示器 / 改缩放也能落回可用区。
- 字号、间距、配色、连线几何都没动；含 `windows.h` 的 TU 里一律手写比较，没碰 `std::min/max/clamp` 宏。

真机 1.5x DPI（物理 3840×2160）：窗口 2400×390 物理 = 1600×260 DIP 居中，7 颗全尺寸一次看全。把常量临时抬到 2600 复验兜底：窗口落回 2528 DIP（工作区 2560 − 32），两侧各 16 DIP，不压任务栏。

## 2026-10-22 · 节点图的跨行连线改成直角折线

斜切贝塞尔在跨行时是一条缓坡，得顺着看才知道谁连谁。现在改成「出门 → 落下 → 进门」：

- **同行相邻仍是直线**，只有跨行才拐直角：出父节点先横 14 DIP → 竖直落到子那一行 → 横着进子节点；拐角用二阶曲线切 4 DIP 小圆角，缩放后没有硬刺。
- **窄间距退回直连**：间距 ≤ 8 DIP 或摆不下两段圆角时，画回勾比画斜线更难读。
- **流光改按折线累计弧长定位**：拐角只是路径上普通一点，所以不掉速、不会在拐角重来一段。
- **方向标**改落在最后那段横线的中段——原先取采样中段，跨行时会落到竖线上，指错方向。
- 线宽/颜色/亮度一字未改：底线白 0.22 · 1.6 DIP，流光 accent · 1.9 DIP。

真机 1.5x DPI 验证：三条向下、一条向上跨两层，四条边全是直角；竖线逐行量过 x 不变；同行两条仍是带方向标的直线。

## 2026-10-22 · 解构多了一层「可以反复拆的草稿」

以前模型拆完就直接 `plan.set`：给几个节点就落几个，不满意只能整轮重来。
现在中间夹一层草稿，落库变成**人的一个动作**。

- **草稿不落正式文档**：宿主把模型结果存成 `DraftPlan`（带过期时间），
  `work.json` 一个字节不写。放弃、超时、关窗，都等于没发生过。
- **审阅窗**：原生自绘，纵向缩进树 + 右侧「目标 / 验收」列。
  `↑↓` 选、`Enter` 对选中项**再拆一层**、`Ctrl+Enter` 就这样、`Esc` 不要了；
  点同一行两次也能展开/收起。
- **已经拆过的本地展开**：不重复花模型调用；只有「没拆过的」才请求
  `POST /api/plan/expand`，拆完自动把新子节点摊开。
- **确认才落库**：`POST /api/plan/commit` 一次性 `plan.set`，然后面板自动弹开。
- 修掉两个真 bug：① `BuildReviewModel` 每次重建都 `clear()` 展开集合，
  子行刚放出来就在下一帧塌回去；② 审阅窗的消息号与输入框的 `WM_APP+7/+8` 撞车。
- 上一节的「按住 220ms 才召唤」已经改回来了：现在是**按一下就进入并停在那儿**，
  `Esc` 或完成动作才退出——真机上按住再松手，覆盖层会跟着手指闪一下。

## 2026-10-21 · Tab 改成「按住」手势；动画跟着刷新率与显示器走

两件事都是被真机逼出来的。

- **Tab 是长按，不是连点**：按下只记时间，**按住超过 220ms** 才召唤覆盖层，松手才结束这一次手势
  （有选框就弹轮盘，没选框就退出）。自动重复由 `MOD_NOREPEAT` 挡掉，代码里再兜一层「同一手势不重置起点」。
  为什么不用 `WH_KEYBOARD_LL` 判断抬起：那个钩子会被杀软当键盘记录器。改用 `GetAsyncKeyState` 轮询，
  16ms 一次，代价可以忽略。
  代价说清楚：**DSH Work 跑着的时候 Tab 是被我们吞掉的**，短按也不会给别的程序——这是「一直保持监听」的必然。
- **覆盖层开着时 Tab 只归我们**：召唤前先把小条的全局 Tab 注销，由覆盖层自己注册，
  两个窗口不抢同一个键，Windows 也不会在框选途中把焦点挪走。
- **高刷屏**：动画原来是「每 tick 前进一点」，等于帧率说了算。现在全部按真实时间推进
  （轮盘张开按 `GetTickCount64` 差值算进度），动画定时器的间隔改成 `1000 / 显示器刷新率`（夹在 4–16ms）。
  本机实测探测到 `刷新率=240Hz 帧间隔=4ms`。
- **双屏**：小条不再钉死主屏，热键唤起时先 `PositionWindow(MonitorUnderCursor())` 搬到光标所在那块屏，
  搬完立刻 `UpdateDpi()` 重算缩放（150% + 100% 混搭是常态，用旧值搬过去会错位）。
  轮盘的动画帧间隔也取轮盘所在那块屏的刷新率，不再永远读主屏。
- 顺带：轮盘扇区几乎不透明了（0.90 → 0.985）。底下哪怕只是压暗的桌面，透出来也会显脏。
## 2026-10-21 · 建议流 + 交给 AI + 接棒

面板会说话了：打开时它自己去要一句建议；轮盘多了一项「交给 AI」；AI 那一侧现在也能接得住。

- **建议流**：面板一打开，后台向 `POST /dsh-work/api/advise` 要一条建议和一条更优解，
  落成节点的 `advice` 挂件，右侧两行「建议 · …」「更优解 · …」。同一节点 60 秒内不重复请求——
  面板每秒轮询一次，没有这个闸门就会变成每秒一次模型调用。
- **交给 AI**：轮盘第四项从占位的「自动后续」改成「交给 AI」，写 `handoff.offer`。
  它**只落意愿不冒充执行**：真正干活的是宿主那侧的会话，原生层没有资格假装。
  面板最右列显示交接态（已交给 AI / AI 正在做 / 已验收 / 已退回）。
- **接棒**：不去自己起会话——`ctx.agents.create` 的签名没验证过，摸黑调等于埋一个不知道什么时候炸的雷。
  改成「模型自己调工具」的协议：`work_status` 多出一段「已交给你（等你接手的节点）」，
  把每个待接节点的目标、验收和「想接就调 work_handoff_start」写进模型看得见的文本；
  新工具 `work_handoff_start` 负责 `offered → running` 并把 `agentSessionId` 写回去，做完照旧 `work_handoff_submit`。
- **顺手修的结构问题**：`formatStatus` / `formatTree` / `offeredNodes` 从 `tools.ts` 搬到 `format.ts`。
  `tools.ts` 顶部静态 import 了 `@deepseek-ai/dsh-tools`（只在宿主运行期存在），
  一旦被测试文件间接引入就 `ERR_MODULE_NOT_FOUND`；搬出来才能单测。
- **修掉一个「某一瓣缺一块」的老 bug**：扇区判定原来是 `ang < a0 || ang > a1` 直接比角度，
  但 `ang` 取模到 [0, 2π)，而正上方那一瓣的跨度是 [-86.6°, +86.6°]（`a0` 是负数）——
  一取模整瓣只剩右半边，且**跨 0° 的瓣永远进不来**，看起来就是「按钮消失了一部分」。
  改成「相对起始边、绕一圈取模」后与 `a0` 的符号无关：`rel = (ang - a0) mod 2π`，再判 `rel <= span`。
  顺带解决了相邻两瓣重叠混色的问题（以前是叠着画的，重叠处会更实）。
- **踩到的坑**：
  1. `Begin()` 忘了复位 `stage`——上一轮停在轮盘态时，下一轮一按 Tab 就直接画上一轮的轮盘，
     鼠标事件全被它吃掉，看起来像「Tab 坏了」。
  2. 面板只有 132 DIP 高，交接提示一开始塞在详情列 140 DIP 处，整行越界看不见；
     挪到最右列 `已连接` 下面才安顿好。
  3. 轮盘那次「有一瓣不填色」的真因是 **GDI 往分层窗口的 32bpp DIB 上写字会把 alpha 刷成 0**，
     补救办法是先把字画到离屏掩膜，再按亮度当覆盖率逐像素混。


## 2026-10-21 · 解构闭环打通

框选之后选「解构这块」，这一块就真的被拆成任务树了。

- **链路**：原生层把刚存的 PNG 直接 `POST /dsh-work/api/recognize` → 宿主受理回 202 + jobId → 原生层每 800ms 查一次作业 → `done` 后小条提示「已拆成 N 个任务 · 标题」，面板同步长出新节点。
- **两次真机**：7 个任务（7 秒）、5 个任务（33 秒）。超时放宽到 150 秒——模型慢的时候 90 秒不够。
- **踩到的坑**：后台线程把宽字符塞进 `std::string(w.begin(), w.end())` 写日志会变乱码，改 `Narrow()` 做 UTF-8 转换；识别结果用 `WM_APP+6` 回 UI 线程，后台线程一律不碰窗口。

## 2026-10-20 · Tab 二级菜单改成 lolPing 那样的环形轮盘

截图之后不再是"存盘就结束"：松手会在选区右下角张开一圈四瓣轮盘，缺口朝向不再重要，鼠标指哪一瓣亮哪一瓣，回车或再点一下落子。

- **四瓣**：重新框选（右上）/ 解构这块（右下）/ 直接保存（左下）/ 取消（左上）。圆心是死区，指到圆心就是取消，误松手时最省事的路径正好在手指停的地方。
- **画法**：每像素 2x2 采样算覆盖率，所以圆环和扇区边缘没有锯齿；命中瓣用强调蓝 `(10,132,255)` 实心，其余瓣深色 `(28,30,38)`；中心 hub 大字显示当前项名。
- **可点也可键**：鼠标移动改指向，左键点击落子，回车落当前瓣，Esc 直接退出。
- **踩到的坑**：一个位图同一时刻只能选进一个 DC —— 用 GDI 写轮盘文字时若另开一个 `CreateCompatibleDC` 并 `SelectObject`，第二次会静默失败，于是文字画到了错误的位图上；改成全程用 `s.work` 那一个 DC。

前一轮的文档条目"竖排菜单"已被本条取代。
## 2026-10-20 07:57

- **原生面板**：Ctrl+Alt+Space 展开，卡带 + 详情 + 页脚键位；Esc 收起。
- **原生键盘**：WH_KEYBOARD_LL 只在面板打开时吞 Enter/Backspace/←/→/Esc，其余一律放行——面板窗口 WS_EX_NOACTIVATE，不抢焦点、不打断你正在打的字。
- **托盘**：左键开合面板，右键菜单可退出。
- **README/package.json 加制作人**：人负责取舍与验收，AI 负责架构与实现。

# 变更日志

## 2026-10-20 · 面板改成叠放卡片、原生层全面接手

- 面板不再三张并排，改成一副叠起来的牌：当前任务在最上面百分百可见，做完的往左叠、
  待做的往右叠，都只露一条边，越远越淡。面板尺寸 760x396 → 660x318。
- 左右键在卡片间移动时，侧边卡露出的那条边会亮一道竖线，详情栏同步显示被指着的那条。
- 推送方式改道：github.com 不通的机器上改走 GitHub Git Data API 重放提交（scripts/push-via-api.mjs），
  历史与作者日期都保留。
> 每个阶段一节；每轮开发循环追加一行。**只写人需要知道的**，不复制 git 提交列表。
> 格式：`- YYYY-MM-DD · 一句话说明什么变了、为什么`。

## P0 · 地基（2026-10-19）

- 2026-10-19 · 落地仓库骨架：`core`（纯函数状态机 + 只追加日志 + 快照）、`plugin`（宿主半边）、`companion`（Electron 伴随层）三层，协议 v1 冻结
- 2026-10-19 · 插件在真实 DSH 实例上跑通：`/dsh-work/api/{health,state,intent,events}`，杀进程重启后任务状态完整
- 2026-10-19 · 窗口内玻璃条挂上 `shell.overlay`，与宿主同源通信（未走宿主鉴权，靠自带令牌）
- 2026-10-19 · 桌面置顶玻璃条打通：无边框透明窗 + `alwaysOnTop` + 点击穿透 + SSE 状态流，修掉"永远停在正在连接"的首帧丢失缺陷

## P1 · 闭环

- 2026-10-19 · 伴随层全局热键落地（P1-1）：默认 `Control+Alt+Space`，可从 `~/.dsh/dsh-work/config.json` 改；裸 Tab 必须显式 `allowBareTab: true`；注册失败会写 `companion.log`，按下热键小条压缩 120ms
- 2026-10-19 · 截屏框选落地（P1-2）：热键 → 先藏小条再冻结整屏 → 全屏半灰覆盖层拖框 → 区域 PNG 落 `~/.dsh/dsh-work/captures/`。150% 缩放下实测拖拽 800×600 物理像素得到 800×600 像素图，零偏移；Esc / 右键 / 再按热键都能取消；小条回弹后给 2.4 秒"已截取 W × H"，不弹窗、不打断手上的事
- 2026-10-19 · 面板落地（P1-2b）：热键从"按下即截图"改成"按下开面板"。小条是收起态、面板是展开态，同一个东西的两个形态；面板里有任务卡带（当前居中，左右是已完成/待进行）、详情列（目标 / 验收）、页脚键位提示。`Enter` 完成并前进，`Backspace` 退一步，`Esc` 收起，`Tab` 进框选，框选结束或取消后按原样把面板放回来。玻璃底换成"抓一张背后桌面 → CSS 模糊"（`backdrop.cjs`）：CSS 的 `backdrop-filter` 对透明窗口背后的桌面无效（拿不到别进程的表面），Win11 的 `acrylic` 能模糊桌面但会吃掉圆角——第三条路才是对的。
- 2026-10-19 · `plan.set` 新增可选 `title`：任务树的标题应该由"正在做的工作"决定，而不是创建文档时随手填的占位串。
- 2026-10-19 · 视觉识别落地（P1-3）：框完即识别，不再要用户按键。伴随层把 PNG 交给宿主，宿主用 `ctx.attachments` 入库、`ctx.llm.stream` 调一次视觉模型，把截图拆成带目标与验收的任务树，解析后走既有 `plan.set` 通道落盘。HTTP 做成作业（POST 只受理、GET 轮询取结果）：几十秒的调用不能挂在同步响应上，超时与插件卸载用 `AbortController` 真停。解析失败会「就地修格式」重问一次，仍失败就返回可读原因、一个字节都不写文档；缩进大纲也收，但用 `looksLikeOutline` 挡住"抱歉看不清"被当成任务。小条挂「识别中…」，面板给开始/成功/失败三种文案。真机端到端待宿主重启后补验。
<!-- 开发循环从这里往下追加 -->

- 2026-10-19 P1-2c 完成：**面板的输入模型按 lolPing 的原理重做**。旧做法是 `show()+focus()`——面板一弹就把键盘从用户正在打字的软件里抢走，本质是个模态框。改成 `focusable:false` + `showInactive()`：面板永远不夺焦点，键盘靠"打开期间用 `globalShortcut` 独占 Enter/Backspace/Tab/Esc/←/→，关面板立刻释放"（实测 6/6 注册成功）。验证方式是可证伪的：先记下前台窗口（Edge），按 Enter，任务 seq 7→8、pointer 前进，前台窗口**仍是 Edge**。顺带补了每次显示重申 `setAlwaysOnTop(true,"screen-saver")`（Windows 会悄悄降级，lolPing #70），并删掉 `focusable:false` 下永远不会触发的 `blur` 收起逻辑。
- 2026-10-19 隐私修正：`docs/assets/` 里的桌面截图（整屏 2 张 + 框选 2 张）已从工作区与 **全部 git 历史**中移除，并强推覆盖远端 main。文档配图改由 `scripts/make-shots.mjs` 用 Playwright + 合成数据 + 合成底图渲染真实 HTML——控件排版是真的，里面的内容全是假数据。另加 `scripts/check-assets.mjs`（白名单 + 尺寸上限）接进 `pnpm test`，防止复发。
- 2026-10-22 · 入口按钮：DSH 玻璃条加「打开面板」，点一下就能开桌面面板；宿主新增 `POST /dsh-work/api/ui/open`，指令随 `/api/state` 的 `ui` 字段下发给原生轮询，伴随层没跑时自动拉起（`eed4aed`）
- 2026-10-24 · 修「节点图开着时小条仍认鼠标」：小条悬停会摘掉 `WS_EX_TRANSPARENT`、并排一个 260ms 的开面板计时器，而「图开着」的守卫是提前 return——穿透位没还、计时器没撤、单击也不看图的可见性，于是图开着点一下面板就盖上来（6 次撞 1 次）；Esc 关图时光标常在药丸上，下一个悬停轮询立刻判「刚进入」，260ms 后面板自己弹（3/3）。现在守卫里撤计时器 + 还原穿透位，单击/双击/计时器到点/宿主要 panel 都先看 `ReviewShown()`，`CloseReview` 末尾把「人就停在药丸上」这次悬停认下来；真机 11 项全过，`pnpm test` 121 绿（`2b95980`）
- 2026-10-24 · 核对「自动化提示语里的四件固定队列」：提示语仍把入口按钮 / 直角折线 / 加宽 / 讲讲它排成优先级，逐条 grep 源码确认四件都还在（`packages/plugin/src/client/index.tsx:297-326` 的 `data-dsh-work-graph`、`native/shell/src/review.h:473` 的 `BuildEdge`、`review.h:111` 的 `kWindowW = 1600`、`review.h:75-330` 的讲解卡，宿主 `packages/plugin/src/http.ts:307` 的 `/api/learn`），**零代码改动、不重做**；按接力板把下一件交回用户三选一。`pnpm test` 121 绿、`pnpm typecheck` 干净
