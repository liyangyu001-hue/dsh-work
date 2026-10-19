# @dshwork/core

三端共用的**纯逻辑层**。

## 约束（破了就失去可测性）

- 不 import `node:*`、不 import `react`、不 import WebSocket 库。
- `apply()` 必须是纯函数：不允许读时钟、读文件、取随机数。时间与 id 一律从参数传入。

## 内容

| 文件 | 职责 |
|---|---|
| `types.ts` | `WorkDoc` / `WorkNode` / `Intent` 等全部数据结构 |
| `state.ts` | `apply(doc, intent, ctx)`：唯一的状态变更入口；`validateDoc` 保证不会产出非法状态 |
| `patch.ts` | `diffDoc` / `applyPatch`：增量同步用 |
| `journal.ts` | 事件日志的编码、解析、重放 |
| `protocol.ts` | WS 信封的编解码与兼容性检查 |
| `plan.ts` | `materializePlan`：把 AI 产出的计划树变成带 id 的节点 |

## 为什么不在这里做 IO

因为 `core` 要被三个完全不同的运行时复用（Node 插件 / 浏览器 / Electron）。
一旦引入 IO，就只能在一处跑，单测也会变成集成测试。
