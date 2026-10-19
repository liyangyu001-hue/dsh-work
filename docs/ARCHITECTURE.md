# 架构

> 本文回答三个问题：系统由哪些部分组成、它们之间靠什么说话、为什么这样切。
> 具体报文见 [PROTOCOL.md](PROTOCOL.md)，磁盘格式见 [DATA-MODEL.md](DATA-MODEL.md)。

---

## 1. 定位

DSH Work 的核心假设是：**人和 AI 之间缺的不是能力，是共享的上下文与节奏**。

所以这个系统的技术形态不是「一个应用」，而是「一个常驻的任务进程 + 若干视图」。任务进程活着，人与 AI 就始终站在同一条线索上。

## 2. 三端

| 端 | 运行时 | 职责 | 绝不能做 |
|---|---|---|---|
| **宿主 Host** | DSH 插件（Node 子进程） | 任务状态机、事件日志、持久化、AI 编排、工具注册、HTTP/WS 路由 | 直接画界面、直接操作系统 |
| **界面 Client** | DSH Web 客户端 bundle（React） | 工作台：任务树、详情、建议流、学习模式、设置 | 截屏、碰 Electron、跨端口取鉴权、自己改状态 |
| **伴随 Companion** | 独立 Electron 进程 | 全局热键、多屏截图、框选层、置顶玻璃条、托盘、开机自启 | 持有业务状态、直接读写 `journal.ndjson` |

### 为什么必须有三端

- DSH 客户端是**沙箱化的浏览器 bundle**：`require` 只认平台基线，官方明令禁止引入 Electron/Node 能力；主窗口权限层只放行单轨音频，`getDisplayMedia()` 必被拒。
- DSH 插件宿主是**纯 Node 子进程**，`require('electron')` 直接抛错，物理上不存在 `globalShortcut` / `desktopCapturer` / `Tray` / `BrowserWindow`。
- 而「系统级置顶小条 + 任何应用里可用的热键 + 跨应用截屏」这三件事，只能由桌面进程完成。

因此第三端不是设计偏好，是**约束的解**。

### 为什么伴随层用 Electron

| 方案 | 优点 | 代价 |
|---|---|---|
| **Electron** ✅ | 零系统工具链依赖；`globalShortcut`/`desktopCapturer`/`Tray`/`setAlwaysOnTop`/无边框透明窗/`backgroundMaterial:'acrylic'` 一次齐全；与 Client 共用 React 与 token，玻璃与动效像素级一致 | 安装包约 200MB，常驻内存 80–120MB |
| .NET + WebView2 | 安装包 1–5MB，内存 20–40MB | 需装 SDK，玻璃要重写两遍，长期维护成本高 |
| Rust + Tauri | 二进制小、跨平台稳 | 需装 Rust + MSVC，构建链重 |

伴随层被刻意设计成**可替换**：它不持有业务状态，换语言只需重实现 `PROTOCOL.md`，宿主与界面不动。

## 3. 数据流

```
用户动作 ──► Companion（只产生 Intent / 截图）
                │  WS
                ▼
             Host（校验 → 应用 → 落盘 → 广播）
                │  同源 HTTP
                ▼
             Client（纯视图，只读快照）
```

四条铁律：

1. **单写者**：只有 Host 能修改任务状态。Companion 与 Client 发 `intent`，Host 校验后应用。
2. **单向外流**：状态变更以 `state.patch` 广播，带自增 `seq`；视图发现 `seq` 落后就要求 `snapshot` 全量重同步。
3. **可重放**：任何状态都能由 `snapshot + journal` 重建，因此崩溃恢复不需要「上次写到哪儿」的模糊记忆。
4. **不可信输入**：Companion 与 Client 的报文一律视作不可信，Host 侧做结构校验与业务校验（例如节点是否存在、状态迁移是否合法）。

## 4. 分层与依赖方向

```
apps/companion ──┐
packages/plugin ─┼──► packages/core   （纯逻辑，零依赖，可单测）
packages/ui ─────┘         ▲
packages/tokens ───────────┘
```

- `core` 不认识 Node、不认识 React、不认识 WebSocket。它只认识 `WorkDoc` 与 `Intent`。
- **依赖只能向下**。`core` 永远不允许 import 任何上层包——这条一旦破坏，可测试性就没了。

## 5. 关键子系统的位置

| 能力 | 落在哪 | 实现要点 |
|---|---|---|
| 任务树与指针 | `core/state.ts` | 纯函数 `apply(doc, intent) → doc`，同一日志必得同一结果 |
| 持久化 | `plugin/store.ts` | `work.json` 快照 + `journal.ndjson` 追加；写临时文件再 rename |
| 截图 | `companion/capture` | `desktopCapturer` 取全屏 → 框选层 → 只回传选区 PNG |
| 识别与拆解 | `plugin/recognize.ts` + `plugin/http.ts` | 视觉模型一次调用完成「读屏 → 理解 → 拆树」。HTTP 只受理作业、结果另取（`/api/recognize`），避免同步路径里挂一个几十秒的模型调用；解析与校验是纯函数，模型永远不直接写文档——产出的 `plan.set` 仍走 `apply` 通道；截图先经 `ctx.attachments` 变成附件引用再进 `ctx.llm.stream` |
| 建议 | `plugin/ai/advisor` | 直调模型（不进会话日志），按节点内容 hash 缓存 |
| 交给 AI | `plugin/ai/taker` | 建 agent 会话，`followup` 驱动，结果回写节点 |
| 心跳 | `plugin/pulse` | `ctx.schedule` 空闲巡检 + `turn/end` 事件驱动即时跟进 |
| 顶栏 | `ui/Bar` | 同一份组件在 Companion 与 Client 各渲染一次，数据都来自 Host |

## 6. 失败模式与对策

| 故障 | 表现 | 对策 |
|---|---|---|
| Host 未启动 | 伴随层连不上 | 立即退避重连；条子进入「离线」灰态，展示上次缓存快照 |
| Companion 崩溃 | 热键失效、条子消失 | Host 检测 WS 断开 → 重新拉起进程；条子位置与状态从快照恢复 |
| Client 崩溃/刷新 | 工作台白屏 | 无状态设计，重连即重建 |
| 写盘中断 | `work.json` 截断 | 临时文件 + rename 的原子写；启动时校验失败则回退到「快照 + 日志重放」 |
| 模型超时/超预算 | 建议迟迟不来 | 超时降级为「无建议」，并记入节点日志；预算可配 |
| 版本不匹配 | 新老端同时升级中 | 握手时校验 `v`，未知消息类型一律忽略并记警告 |

## 7. 目录

```
packages/core      纯逻辑：任务模型、状态机、协议编解码
packages/tokens    设计 token 单一来源
packages/ui        玻璃组件与动效内核
packages/plugin    DSH 插件（host 半边 + client 半边）
apps/companion     Electron 伴随层（main + renderer）
docs/              本目录
docs/ADR/          架构决策记录，每条含背景、选项、结论、后果
```

## 8. 决策记录索引

- [ADR-0001 三进程与协议边界](ADR/0001-三进程与协议边界.md)
- [ADR-0002 伴随层选用 Electron](ADR/0002-伴随层选用Electron.md)
- [ADR-0003 事件日志而非直接改状态](ADR/0003-事件日志为真相源.md)
