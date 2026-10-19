# ADR-0002 · 伴随层选用 Electron

- 状态：已采纳
- 日期：2026-10-19

## 背景

伴随层要同时做到：全局热键、跨应用截屏、系统级置顶、无边框透明窗、托盘、开机自启，并且要**做出玻璃质感与克制动效**。

本机实测工具链：Node 24 / pnpm 11 / Python 3.14 可用，`dotnet`、`cargo`、`rustc`、`go`、`cl` 全部缺失。

## 选项

| 方案 | 优 | 劣 |
|---|---|---|
| **Electron** | 全部能力开箱；UI 与 DSH 内共用 React 与 token，观感一致；零额外工具链 | 安装包 ~200MB；常驻 80–120MB |
| .NET + WPF/WebView2 | 轻；Windows 原生能力最顺 | 需装 SDK；WebView2 透明窗调校麻烦；UI 要重写两遍；mac 等于重来 |
| Rust + Tauri | 小、稳、跨平台 | 需装 Rust + MSVC Build Tools；构建链对非开发者不友好 |
| Python + tkinter | 零依赖 | 做不出玻璃与动效，直接违背需求 |

## 结论

选 **Electron**。

理由排序：① 本机零安装成本；② 同一套 React/token 能让顶栏和窗口内工作台长得一样，这是产品要求不是技术偏好；③ 长期只有一种语言栈要维护。

## 后果

- 用「顶栏隐藏时销毁 renderer 进程」压内存；用 `backgroundMaterial:'acrylic'` 拿真背景模糊，CSS 兜底。
- 安装包大是已知代价，通过 portable zip 缓解（不强制安装）。
- 若将来包体成为硬伤，可整体替换为 Tauri——协议不变即可。
