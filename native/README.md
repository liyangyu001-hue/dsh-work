# 原生伴随层（C++ / Win32）

Electron 版（`apps/companion`）仍然是可用的，这一层是它的**原生替代品**，
按"小条 → 面板 → 框选覆盖层"三步迁移，两边并存，随时可以退回去。

产品理由写在 `docs/ARCHITECTURE.md`，这里只说工程事实。

## 现在有什么

| 产物 | 源码 | 作用 | 体积 |
|---|---|---|---|
| `dshwork-shell.exe` | `native/shell/src/` | 桌面顶部玻璃小条 | ~318 KB |
| `dshwork-input.exe` | `native/input-helper/src/` | 低级键盘钩子，按需"吃掉"按键并上报 | ~225 KB |

对比：Electron 版解压后 210 MB、冷启动 1.5 s、常驻 4 个进程。
原生版启动在 100 ms 内，内存是单进程量级。

## 构建

```powershell
node scripts/build-native.mjs            # 两个一起编
node scripts/build-native.mjs dshwork-shell   # 只编一个
```

需要 Visual Studio 2022 生成工具（勾选「使用 C++ 的桌面开发」）。脚本会自己找
`vcvars64.bat`；找不到时会把需要的组件和下载地址打出来，不会只给一句"失败"。

几个必须记住的编译开关，少一个都会以奇怪的方式失败：

- `/utf-8`：源码注释和 `L"中文"` 都是 UTF-8。不加这个开关，MSVC 按本地代码页
  （GBK）解释，**中文注释会把下一行代码吞掉**，报错位置还会飘。
- `/ENTRY:mainCRTStartup`：窗口程序（`/SUBSYSTEM:WINDOWS`）但我们写的是 `int main()`，
  不指入口的话链接器会去找 `WinMain`。
- 每个工程单独列 `libs`：输入助手要 `user32`，小条还要 `d2d1`/`dwrite`/`winhttp`/`gdi32`。

## 小条是怎么画出来的

**分层窗口**（`WS_EX_LAYERED` + `UpdateLayeredWindow`），不是普通窗口。原因：
普通窗口的圆角只有系统那个固定半径、边缘没有抗锯齿，做不出 iOS 那种"一整块玻璃"
的感觉；分层窗口是逐像素 alpha，圆角、发丝描边、柔和阴影全部由我们控制。

渲染走 **Direct2D + DirectWrite**：真亚像素中文，不是浏览器排版近似。

**为什么不用 DWM 毛玻璃**（`SetWindowCompositionAttribute`）：那是未公开接口，
行为随系统版本漂移。小条面积太小，模糊不模糊几乎看不出差别，不值得冒这个险。
所以采用 `DESIGN-SYSTEM.md` 里的配方：半透明深色底 + 0.5 DIP 发丝描边 + 多层柔和阴影。

阴影是"叠六层同心圆角矩形、每层 alpha 递减"凑出来的软阴影——没有 GPU 模糊时的
老办法。层数少了会有明显的光晕感，这一点调参时很容易看出来。

## 怎么跟宿主说话

- 读 `%USERPROFILE%\.dsh\dsh-work\endpoint.json` 拿 `url` 和 `token`
  （和 Node 侧同一口径：优先 `DSH_HOME`）。
- **每个轮询周期都重读这个文件**：宿主重启会换端口和 token，缓存下来就永远连不上。
- 用系统自带 **WinHTTP** 拉 `GET /dsh-work/api/state`，带 `x-dsh-work-token` 头。
- 轮询 1 秒一次。**为什么不用 SSE**：小条只要"当前节点 + 进度"这几百字节，1 秒的
  延迟感知不到；而流式要自己处理分块、断线重连、半行数据，风险与收益不成比例。
  要更低延迟时再升级，接口不变（`PROTOCOL.md` 一行不用改）。
- 401/403 时说明"宿主重启后要重新读 endpoint.json"，不静默失败。

## 线程与生命周期

一个轮询线程负责 HTTP，算好的模型用互斥量交接 + `PostMessage` 通知 UI 线程重绘；
UI 线程只碰 D2D/DWrite。关窗口时先让轮询线程退出再 join，不留挂起线程。

**它现在是纯展示、点击穿透（`WS_EX_TRANSPARENT`）的**——所以没有托盘、没有退出按钮。
停掉用：

```powershell
Stop-Process -Name dshwork-shell -Force
```

托盘与退出入口在切片二（面板）里一起做。

## 已知边界

- 只支持 Windows。macOS 那套（`NSWindow` + Core Animation）要另写，Rust/ObjC 都行，
  但不在这三片迁移范围内。
- 小条固定贴主屏工作区顶部正中；多显示器时跟着主显示器，**不跟光标**。
- 窗口尺寸按内容算（标题多长、药丸多宽），但标题超过 420 DIP 会截断，还没有省略号。
