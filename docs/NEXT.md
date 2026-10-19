# 接力板（每轮开发循环只读这一个文件）

> 这个文件是**跨会话的唯一交接物**。任何一轮（人工或自动）开工前先读它，收工前改它。
> 格式故意定得很死：改动只碰"进度"和"当前任务"，历史追加到最下面，不删旧行。

---

## 进度

- 阶段：**P1 · 闭环**（P0 已完成，2026-10-19）
- 上一轮收工：人工会话完成 P0，推送 `84ff6db`
- 下一轮从：**P1-1 全局热键** 开始

## 当前任务（只做一个，做完就停）

### P1-1 全局热键（进行中）

**目标**：在任何应用里按下热键，桌面小条给出可见反馈（先不截图，只打通"按键 → 事件 → 小条"）。

**验收**
- [ ] 伴随层用 `globalShortcut` 注册 `Control+Alt+Space`，注册失败要写日志（别的软件可能占用）
- [ ] 按下后小条出现一次 120ms 的压缩反馈，并在 `companion.log` 留一行
- [ ] 热键可在设置里改（先只支持从 `~/.dsh/dsh-work/config.json` 读，不急着做 UI）
- [ ] 裸 Tab 需要 `allowBareTab: true` 才注册，默认关

**禁止**
- 不要为了"顺手"去动截图与识别
- 不要把热键写死成常量后就收工

**做完后**：把本节改成 ✅ 并写上 commit 号，把 "当前任务" 换成 P1-2。

---

## 队列（做完一个换一个，不要并行）

- **P1-2 截屏 + 框选覆盖层**：全屏变灰、拖拽框选、松手出区域 PNG（多屏 + 150% DPI 必须正确）
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
- 桌面截图必须 DPI-aware（`SetProcessDpiAwarenessContext(-4)`），否则坐标全错。

---

## 历史

- 2026-10-19 P0 完成：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条，推送 `84ff6db`。
