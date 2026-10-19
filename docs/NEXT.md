# 接力板（每轮开发循环只读这一个文件）

> 这个文件是**跨会话的唯一交接物**。任何一轮（人工或自动）开工前先读它，收工前改它。
> 格式故意定得很死：改动只碰"进度"和"当前任务"，历史追加到最下面，不删旧行。

---

## 进度

- 阶段：**P1 · 闭环**
- 已完成：P1-1 全局热键 ✅ `4870b3f` ｜ P1-2 截屏框选 ✅（本轮）
- 上一轮收工：P1-2 截屏 + 框选覆盖层，真机全流程验证通过
- 下一轮从：**P1-3 视觉识别** 开始

## 当前任务（只做一个，做完就停）

### P1-3 视觉识别（未开始）

**目标**：框选得到的那张 PNG → 一次模型调用 → 合法的 `plan.set`（父/子任务 + 每节点目标与验收），落到工作区文档，小条立刻显示当前节点。

**验收**
- [ ] 调用走宿主 `ctx.llm`（不新起会话），provider/model 从插件 Config 取，默认 `DeepSeek-V4-Flash`
- [ ] 模型输出必须是可解析的 JSON；解析失败要有一次"就地修格式"的重试，仍失败则返回可读原因且**不写文档**
- [ ] 成功时 `plan.set` 走既有 `apply` 通道（不绕过状态机），`work.json` + `journal.ndjson` 同步落盘
- [ ] 截图里是抽象概念而非任务时，节点 `kind: 'concept'`（本轮只标记，不做学习模式）
- [ ] 端到端：热键 → 框选 → 小条显示"识别中…" → 弹出当前节点标题
- [ ] `pnpm test` / `pnpm typecheck` 绿；新增解析器必须有纯函数单测（不依赖模型）

**禁止**
- 不要接学习模式、不要做 tips 侧栏（那是 P2）
- 不要在客户端做识别（客户端拿不到密钥，也不该拿）
- 不要把模型调用写进 HTTP 路由的同步路径（必须能超时取消）

---

## 队列（做完一个换一个，不要并行）

- **P1-4 卡片轮播**：窗口内与桌面小条同时显示当前节点，`node.advance` / `node.back` 打通
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
   只允许 fast-forward；**绝不允许 force push**。
3. **提交信息**写"改了什么、为什么"，中文正文，一行标题 + 空行 + 要点。
4. **不要新建"临时验证脚本"**留在地上；验证完删掉，或写进 `scripts/` 并说明用途。
5. 起 dev server 前先按端口杀旧进程，否则 `dsh` 会静默起不来：
   `Get-NetTCPConnection -LocalPort 64001 -State Listen | % { Stop-Process -Id $_.OwningProcess -Force }`
6. 改完客户端必须 `pnpm build`，否则浏览器里看到的还是旧 bundle。
7. 每轮结束时，在 `docs/CHANGELOG.md` 追加一行（日期 + 本轮做了什么）。

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

---

## 历史

- 2026-10-19 P0 完成：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条，推送 `84ff6db`。
- 2026-10-19 P1-1 完成：全局热键可配置（默认 `Control+Alt+Space`，裸 Tab 需 `allowBareTab`），注册失败/异常留痕，按下小条压缩 120ms 并记一行，提交 `4870b3f`；测试 46 绿，typecheck 绿，实机热键触发与占用失败告警均已验证。
- 2026-10-19 P1-2 完成：热键 → 冻结整屏（先藏小条再截）→ 全屏半灰覆盖层拖框 → 区域 PNG 落 `~/.dsh/dsh-work/captures/`；Esc/右键/再按热键取消；小条回弹并给 2.4s "已截取 W × H" 提示。150% 缩放下实测 800×600 物理拖拽 → 800×600 像素，零偏移；`capture-geometry.cjs` 9 个纯函数用例，全套 55 绿。
