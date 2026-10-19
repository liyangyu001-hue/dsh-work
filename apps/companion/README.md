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
  main.cjs            主进程：窗口、热键、点击穿透、状态回放、识别请求
  config.cjs          ~/.dsh/dsh-work/config.json 的读取与校验（坏配置不致命）
  paths.cjs           ~/.dsh/dsh-work 目录口径（端点/配置/日志三个文件共用）
  host-link.cjs       与宿主的 SSE 连接 + 退避重连（纯 Node，可脱离 Electron 测）
  capture.cjs         截图冻结与框选层（overlay 窗口、裁剪、落盘）
  capture-geometry.cjs 拖拽坐标 → 裁剪矩形（纯函数，有单测与 .d.cts 声明）
  backdrop.cjs        抓背后桌面给面板当毛玻璃底（CSS blur 拿不到别进程表面）
  log.cjs             ~/.dsh/dsh-work/companion.log，常驻进程唯一的排障出口
  preload.cjs         小条的桥：contextBridge 暴露固定方法，不给渲染进程任何 Node
  bar/index.html      玻璃条本体（收起态；无框架，单文件，含设计 token 与动效）
  panel/index.html    面板本体（展开态：任务卡带 + 详情 + 页脚键位）
  panel/preload.cjs   面板的桥（与小条分开，少给一个方法就少一份权限）
```

## 识别（P1-3）

框选完成即自动识别，不用再按键。链路刻意绕开渲染进程：

```
panel「框完了」 ──IPC──▶ main.cjs 读 PNG ──POST 原始字节──▶ 宿主 /dsh-work/api/recognize
                                              │
                                        受理后轮询 GET /api/recognize/<jobId>（800ms 一次，封顶 150s）
                                              │
            小条与面板「识别中…」◀──IPC── 状态 done{count} / error{reason}
```

- **模型只在宿主里跑**：密钥与 `ctx.llm` 都在宿主进程，伴随层只搬运字节。
- **为什么轮询而不是等长响应**：识别要几十秒，长响应会被任何一层超时掐断；掐断后既拿不到结果，也不知道宿主还在不在跑。作业化以后，失败总能说清原因。
- 识别成功时宿主已经把任务树写进 `work.json`，伴随层不需要、也不允许自己改状态——它只是下一个 SSE 帧到达后重画。

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

## 热键与配置

默认 `Control+Alt+Space`。要换键，写 `~/.dsh/dsh-work/config.json`（可选文件，改完重启伴随层）：

```json
{
  "hotkey": "Control+Alt+Space",
  "allowBareTab": false
}
```

- `hotkey` 用 Electron accelerator 写法（`Control` / `Alt` / `Shift` / `Super` + 键名）。文件坏了或字段不合法会退回默认值，原因写进 `companion.log`；伴随层不会因为配置起不来。
- 裸 `Tab` 默认不注册：它是所有输入框与键盘导航的公共键。确需占用必须同时写 `"allowBareTab": true`（只认布尔 `true`，写字符串 `"true"` 不算）。
- 按下热键时小条做一次 120ms 的压缩反馈，并在 `companion.log` 留一行「热键按下」——这条日志是确认"按键真的到了伴随层"的第一步。

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
