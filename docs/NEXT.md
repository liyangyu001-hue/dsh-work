# 接力板（每轮开发循环只读这一个文件）

> 这个文件是**跨会话的唯一交接物**。任何一轮（人工或自动）开工前先读它，收工前改它。
> 项目是什么、每一层为什么存在、角色与工程规则，见 [项目说明](GUIDE.md)。
> 格式故意定得很死：改动只碰"进度"和"当前任务"，历史追加到最下面，不删旧行。

---

## 进度

> **方向调整（2026-10-20 早）**：用户拍板伴随层**全换 C++**，分三步走——小条 → 面板 → 框选覆盖层。
> Electron 版（`apps/companion`）保留不删，随时可回退。原生层在 `native/`，构建与取舍见 `native/README.md`。
>
> **切片一已完成**：`native/shell` 原生小条真机跑通（318 KB，Electron 版是 210 MB）；
> 顶部居中、逐像素抗锯齿圆角、DirectWrite 真中文、1 秒轮询宿主状态、逐显示器 DPI 感知。
> **待办**：托盘与退出入口——现在它点击穿透、没有 UI 出口，只能 `Stop-Process -Name dshwork-shell`。
>
> **仓库删除确认**：2026-10-20 07:40 查 `GET /repos/...` 仍返回 200，用户还没删。
> 删之前所有提交照常推（反正删库会连同历史一起没掉）。

- 阶段：**P1 · 闭环**
- 已完成：P1-1 全局热键 ✅ `4870b3f` ｜ P1-2 截屏框选 ✅ ｜ P1-2b 面板 ✅ `4a0877e` ｜ **P1-3 视觉识别 ✅ `7e53a78`（真机待验）** ｜ P1-2c 面板输入模型 ✅（不抢焦点 + 打开期间独占按键）
- 上一轮收工：按 lolPing 的原理重做面板的输入方式——面板 `focusable:false` + `showInactive()`，键盘靠"打开期间用 globalShortcut 独占 Enter/Backspace/Tab/Esc/←/→"，关面板立刻释放；实测前台一直是 Edge，按 Enter 照样推进任务。
- 下一轮从：**切片三 · 原生框选覆盖层** 开始
- 最近一次远端 sha：`c66753a`

## 进度

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
最后把 efs/heads/main 指过去。**远端 sha 与本地不同，但 tree 完全一致**（可用
git rev-parse HEAD^{tree} 与 GET /git/commits/<远端sha> 的 .tree.sha 对账）。
推完把远端新 sha 记在下面这行，下一轮用它当起点。

## 当前任务（只做一个，做完就停）

### P1-4 卡片轮播（未开始）

**目标**：当前节点在窗口内卡片带与桌面小条上始终是同一个，且切换时有连续感——`Enter` 完成并前进、`Backspace` 退一步，两边同步、不闪、不抖。

**验收**
- [ ] 面板里左右切卡（`←`/`→`）时，桌面小条同步显示同一节点，不需要等下一次快照
- [ ] `Enter` / `Backspace` 落地后，小条标题与进度（n/N）同步更新；跨节点的切换有轻量过渡（淡入或位移 120~180ms），不是硬跳
- [ ] 已完成节点回退查看时，小条标明「已完成」而不是把它显示成当前要做的
- [ ] 长标题在小条上省略而不是撑宽（窗口宽度固定），面板里能看到全文
- [ ] `pnpm test` / `pnpm typecheck` 绿；「下一个该做的节点」这类选取逻辑必须抽成纯函数并有单测
- [ ] 改完客户端（`packages/plugin/src/client` 或 `apps/companion`）必须 `pnpm build` 并真机看一眼

**禁止**
- 不动 `docs/PROTOCOL.md` 的语义（本任务不出协议变更）
- 不做拖拽/手势轮播（P3 再评估），不引入任何前端框架
- 不顺手改小条与面板的视觉体系（`docs/DESIGN-SYSTEM.md` 是冻结的）

**开工前置**：宿主当前进程加载的是 P1-3 之前的 bundle——`/dsh-work/api/recognize` 在跑着的宿主上还是 404。重启一次 `dsh web`，用 `~/.dsh/dsh-work/captures/` 里已有的一张图走一遍（或热键→框选），确认「识别中… → 已拆成 N 个任务 → 小条显示当前节点标题」整条通。

---

## 队列（做完一个换一个，不要并行）

- **P2-1 建议流**：节点进行中让 AI 给建议与更优解
- **P2-2 交给你来**：把节点交给 AI 执行并回收结果

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
- 桌面截图必须 DPI-aware（`SetProcessDpiAwarenessContext(-4)`），否则坐标全错。
- `nativeImage` 没有 `width`/`height` 属性，几何换算必须传 `getSize()`；传对象本体不会报错，只会得到一堆 NaN 和 0 字节 PNG。
- `store.test.ts` 偶发 `ENOTEMPTY ... rmdir` —— 临时目录清理与原子写（临时文件 600ms 内 rename）踩在同一拍上，重跑即过；真出现先重跑一次再判断是不是代码坏了。
- 视觉模型不是默认就收图：`agentDefaultModel` 若选的是纯文本模型，宿主会把图片投影成文字说明，识别质量会很差。要换 `deepseek-v4-flash-vision-exp` 或在插件 Config 里写 `llm.model`。
- **`focusable:false` 的窗口收不到任何 DOM 键盘事件**。要键盘可用只有两条路：抢焦点（那就会打断用户正在打的字），或者像 lolPing 那样在输入层把手势键吃掉。本机实测 `globalShortcut.register("Enter" / "Backspace" / "Tab" / "Escape" / "Right" / "Left")` 在 Windows 上 **6/6 全部成功**，这就是"打开期间独占、关掉立刻释放"方案的依据。
- **置顶窗口会被 Windows 悄悄降级**（lolPing issue #70）：每次 `show()` 之后都要重新 `setAlwaysOnTop(true,"screen-saver")`，否则面板过一会儿就跑到别的窗口后面。
- 面板既然不抢焦点，`blur` 事件永远不会来——**别用 blur 做"点外面就收起"**，那条路在 `focusable:false` 下是死的。收起只走 Esc / 热键 / 关闭按钮。

---

## 历史

- 2026-10-19 P0 完成：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条，推送 `84ff6db`。
- 2026-10-19 P1-1 完成：全局热键可配置（默认 `Control+Alt+Space`，裸 Tab 需 `allowBareTab`），注册失败/异常留痕，按下小条压缩 120ms 并记一行，提交 `4870b3f`；测试 46 绿，typecheck 绿，实机热键触发与占用失败告警均已验证。
- 2026-10-19 P1-2 完成：热键 → 冻结整屏（先藏小条再截）→ 全屏半灰覆盖层拖框 → 区域 PNG 落 `~/.dsh/dsh-work/captures/`；Esc/右键/再按热键取消；小条回弹并给 2.4s "已截取 W × H" 提示。150% 缩放下实测 800×600 物理拖拽 → 800×600 像素，零偏移；`capture-geometry.cjs` 9 个纯函数用例，全套 55 绿。

- 2026-10-19 P1-2b 完成：**面板**落地。热键语义从"按下即截图"改为"按下开面板"——小条是收起态、面板是展开态，同一个东西的两个形态；面板里做任务卡带（当前居中、左右为已完成/待进行，`scroll-snap` 居中）、详情列（目标/验收）、页脚键位提示，键盘 `Enter` 完成并前进、`Backspace` 退一步、`Esc` 收起、`Tab` 进框选，框选结束/取消后按原样恢复面板。玻璃底改用"抓一张背后桌面 → CSS 模糊"的方案（`backdrop.cjs`）：CSS `backdrop-filter` 对透明窗口背后的桌面无效，Win11 `acrylic` 又会吃掉圆角。`plan.set` 新增可选 `title`，拆解时顺带给文档命名。

- 2026-10-19 P1-3 完成：**视觉识别**落地，提交 `7e53a78`。链路 `panel → main.cjs → POST /dsh-work/api/recognize → 宿主 ctx.llm.stream → plan.set`；作业化（POST 只受理、GET 轮询取结果）是为了不让几十秒的模型调用挂在同步响应上，超时/卸载用 `AbortController` 真停。解析是纯函数：失败一次「就地修格式」重问一次，仍失败返回可读原因且不写文档；`looksLikeOutline` 挡住散文被误认成任务；`kind:'concept'` 走同一解析路径。模型选择 插件 Config → `agentDefaultModel` → `deepseek-official/deepseek-v4-flash`。新增 `recognize.ts`、`recognize.test.ts`、`http.test.ts`，测试 73 绿、typecheck 绿、build 通过。**遗留**：真机端到端没跑——跑着的宿主加载的还是旧 bundle（`/api/recognize` 返回 404），重启 `dsh web` 后补验；`attachments.admitPromptContent` 与视觉模型的实际输出质量也未实测。
