# 接力板（每轮开发循环只读这一个文件）

> 这个文件是**跨会话的唯一交接物**。任何一轮（人工或自动）开工前先读它，收工前改它。
> 格式故意定得很死：改动只碰"进度"和"当前任务"，历史追加到最下面，不删旧行。

---

## 进度

- 阶段：**P1 · 闭环**（P0 已完成，2026-10-19）
- 已完成：P1-1 全局热键 ✅ `4870b3f`
- 上一轮收工：自动会话完成 P1-1 全局热键，提交 `4870b3f`
- 下一轮从：**P1-2 截屏 + 框选覆盖层** 开始

## 当前任务（只做一个，做完就停）

### P1-2 截屏 + 框选覆盖层（进行中）

**目标**：按热键后桌面变灰并允许拖拽框选，松手得到一张区域 PNG，为 P1-3 的识别准备输入。

**验收**
- [ ] 热键触发全屏覆盖层，背景变灰，所有显示器都盖到（多屏）
- [ ] 拖拽出选框并实时显示，松手把区域存成 PNG（`~/.dsh/dsh-work/captures/`，文件名带时间戳）
- [ ] 150% 缩放与负坐标屏幕下，选框与 PNG 像素一致，不偏移
- [ ] Esc 或右键取消，覆盖层消失后小条状态不受影响

**禁止**
- 不要接视觉识别 / 模型调用（那是 P1-3）
- 不要把框选扩成截图工具全家桶（标注、编辑、历史记录先都不做）

**做完后**：把本节改成 ✅ 并写上 commit 号，把 "当前任务" 换成 P1-3。

---

## 队列（做完一个换一个，不要并行）

- **P1-3 视觉识别**：区域图 → 一次模型调用 → 合法 `plan.set`（含目标与验收），走宿主 `ctx.llm`
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
- `apps/companion` 的 `tsconfig` 只覆盖 `*.test.ts`；测试里 require `.cjs` 用 `createRequire(import.meta.url)`，不要给 `.cjs` 配类型声明。
- 桌面截图必须 DPI-aware（`SetProcessDpiAwarenessContext(-4)`），否则坐标全错。

---

## 历史

- 2026-10-19 P0 完成：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条，推送 `84ff6db`。
- 2026-10-19 P1-1 完成：全局热键可配置（默认 `Control+Alt+Space`，裸 Tab 需 `allowBareTab`），注册失败/异常留痕，按下小条压缩 120ms 并记一行，提交 `4870b3f`；测试 46 绿，typecheck 绿，实机热键触发与占用失败告警均已验证。
