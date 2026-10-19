# @dshwork/companion · 伴随层

> 系统的感官与脸：全局热键、屏幕截取、桌面置顶玻璃条。

## 为什么必须有它

DSH 插件跑在宿主进程里，拿不到 `globalShortcut` / `desktopCapturer` / 任何窗口能力
（宿主只给 `tools` / `webServer` / `session` / `llm` 这些服务，没有任何 Electron seam）。
而「桌面上方一条跨应用的玻璃条」「在任何应用里按一下键」天生属于系统层。

于是三层里它负责**离人最近的那一层**：

```
宿主（Node，真相源） ──同源 HTTP/SSE── 这里（Electron，感官+脸）
```

它**不持有任务状态**。任务树的唯一真相源在插件宿主，它只是把 `snapshot` 画出来，
把用户的操作翻译成 `Intent` 提回去。这样任何一端都能单独重写。

## 为什么是 Electron 而不是 Tauri / WPF

本机实测 `dotnet` / `cargo` / `go` / `cl` 全部缺失，装工具链就得先跟网络搏斗；
而 Electron 与 DSH 自己的运行时同源，`pnpm install` 后零系统依赖即可跑通。
代价是二进制体积（约 180 MB），P3 阶段再评估是否换壳——协议层不变，换壳不影响另外两端。
详见 [ADR 0002](../../docs/ADR/0002-伴随层选用Electron.md)。

## 目录

```
src/
  main.cjs        主进程：窗口、热键、点击穿透、状态回放
  host-link.cjs   与宿主的 SSE 连接 + 退避重连（纯 Node，可脱离 Electron 测）
  preload.cjs     唯一的桥：contextBridge 暴露四个方法，不给渲染进程任何 Node
  bar/index.html  玻璃条本体（无框架，单文件，含设计 token 与动效）
  log.cjs         ~/.dsh/dsh-work/companion.log，常驻进程唯一的排障出口
```

## 跑起来

```bash
cd apps/companion
./node_modules/electron/dist/electron.exe .     # Linux/macOS: ./node_modules/.bin/electron .
```

### 两个必踩的坑

1. **`ELECTRON_RUN_AS_NODE` 必须先清掉**。DSH 自己的诊断环境可能带上它，
   Electron 见到它就退化成纯 Node 进程，不建任何窗口，也不报错。
2. **`electron` 的 postinstall 可能被墙**。二进制下不下来时，`node_modules/electron/dist`
   会是空的；`npm_config_mirror` 对 pnpm 无效，直接下镜像包解压到该目录即可。

## 状态机

```
connecting ──连上──▶ online ──断开──▶ offline ──退避 1/2/5/10s──▶ connecting
```

**状态回放**：渲染进程挂载完成时（`bar:ready`）主进程会把最后一次状态再推一遍。
本地回环的 SSE 第一帧往往比页面脚本更早到达，没有这一手，小条会永远停在「正在连接」。

## 调试

- 日志：`~/.dsh/dsh-work/companion.log`（800 行自动裁剪）
- 端点：`~/.dsh/dsh-work/endpoint.json` 里有宿主的 `url` 与令牌，可以 `curl` 直接验：
  ```bash
  curl -N -H "x-dsh-work-token: <token>" http://127.0.0.1:64001/dsh-work/api/events
  ```
- 截图对比：`node ../../scripts/shot.mjs --url ... --out shot.png`（走系统 Chrome）
