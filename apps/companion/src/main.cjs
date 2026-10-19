/**
 * 伴随层 · 主进程（CommonJS）
 *
 * 为什么是 .cjs 而不是 ESM：Electron 的内置 `electron` 模块只在 CJS 入口下
 * 被注入。实测在 ESM 主进程里 `require("electron")` 会解析到
 * node_modules/electron/index.js，拿到的是**二进制路径字符串**而不是 API 对象；
 * `import electron from "electron"` 拿到的是同一个东西的包装。换句话说，
 * 主进程坚持 ESM 就等于拿不到 app/BrowserWindow。所以这里退回 CJS，
 * 把 ESM 留给渲染层（纯浏览器环境）。
 *
 * 三条铁律（与 host 半边一致）：
 *   1. 窗口只负责"显示与点击"，不存任务状态 —— 真值永远在 DSH 插件里。
 *   2. 与 DSH 的连接断了就当"离线"，不猜测、不用缓存冒充实时。
 *   3. 不做隐形的输入劫持：热键可配置，默认组合键不打裸 Tab 的主意。
 */

"use strict";

const { app, BrowserWindow, globalShortcut, Menu, screen, ipcMain } = require("electron");
const path = require("node:path");
const { HostLink } = require("./host-link.cjs");
const log = require("./log.cjs");

/** 小条窗口尺寸：高度留出上下各 22px 给投影，宽度按设计系统上限 520 + 边距。 */
const BAR_WIDTH = 564;
const BAR_HEIGHT = 88;
/** 距屏幕顶部的偏移（贴着标题栏下沿，但不压系统拖拽带）。 */
const TOP_OFFSET = 6;

/** 全局热键。裸 Tab 需要用户在设置里显式开启（见 docs/ADR 与 PROTOCOL 的 WorkConfig）。 */
const DEFAULT_HOTKEY = "Control+Alt+Space";

/** @type {BrowserWindow | null} */
let win = null;
/** @type {HostLink | null} */
let link = null;

/**
 * 最后一次推给窗口的状态。
 *
 * 为什么主进程要留一份：渲染进程挂载完成（bar:ready）通常晚于 SSE 的第一帧快照
 * —— 连接是本地回环，毫秒级；页面要解析 HTML、跑脚本。没有这份回放，小条会永远
 * 停在"正在连接"，直到下一次状态变化才会跳正。这是实测踩到的真实缺陷。
 */
let lastPayload = { state: "connecting", snapshot: null };

function barBounds() {
  const { workArea } = screen.getPrimaryDisplay();
  return {
    width: BAR_WIDTH,
    height: BAR_HEIGHT,
    x: Math.round(workArea.x + (workArea.width - BAR_WIDTH) / 2),
    y: workArea.y + TOP_OFFSET,
  };
}

function send(channel, payload) {
  if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

function createWindow() {
  win = new BrowserWindow({
    ...barBounds(),
    frame: false,
    transparent: true,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    fullscreenable: false,
    // 关键：不抢焦点。小条是"眼镜"，不是对话框 —— 点它不该把用户的输入焦点拽走。
    focusable: false,
    skipTaskbar: true,
    hasShadow: false,
    alwaysOnTop: true,
    backgroundColor: "#00000000",
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  });

  // screen-saver 级别：压过全屏应用与任务栏，这是"系统级置顶"的实质
  win.setAlwaysOnTop(true, "screen-saver");
  win.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });

  // 透明窗口的默认行为会吃掉下面应用的点击。先整体穿透，
  // 由渲染进程在鼠标真正进入小条时再打开（forward:true 让页面仍能收到 hover）。
  win.setIgnoreMouseEvents(true, { forward: true });

  win.loadFile(path.join(__dirname, "bar", "index.html"));

  // 右键菜单是这个"没有任务栏图标"的窗口唯一的退出入口
  win.webContents.on("context-menu", () => {
    Menu.buildFromTemplate([
      { label: "重新连接", click: () => { link?.stop(); link?.start(); } },
      { type: "separator" },
      { label: "退出 DSH Work", click: () => app.quit() },
    ]).popup({ window: win });
  });

  win.on("closed", () => { win = null; });

  screen.on("display-metrics-changed", () => {
    if (win && !win.isDestroyed()) win.setBounds(barBounds());
  });
}

function registerHotkey() {
  const ok = globalShortcut.register(DEFAULT_HOTKEY, () => send("hotkey", DEFAULT_HOTKEY));
  if (!ok) console.warn(`[companion] 热键注册失败（可能被占用）：${DEFAULT_HOTKEY}`);
}

app.whenReady().then(() => {
  log.log(`伴随层启动 pid=${process.pid} electron=${process.versions.electron}`);
  const display = screen.getPrimaryDisplay();
  log.log(
    `display bounds=${JSON.stringify(display.bounds)} workArea=${JSON.stringify(display.workArea)} scale=${display.scaleFactor}`,
  );
  createWindow();
  log.log(`window bounds=${JSON.stringify(win?.getBounds())}`);
  registerHotkey();

  link = new HostLink((payload) => {
    lastPayload = payload;
    send("host:state", payload);
  }, log.log);
  link.start();

  // 鼠标进出小条：切换点击穿透。渲染进程只能发意图，真正的开关留在主进程。
  ipcMain.on("bar:pointer", (_event, over) => {
    if (!win || win.isDestroyed()) return;
    if (over) win.setIgnoreMouseEvents(false);
    else win.setIgnoreMouseEvents(true, { forward: true });
  });

  // 渲染进程挂载完成后回放当前状态，避免"永远停在正在连接"
  ipcMain.on("bar:ready", () => send("host:state", lastPayload));

  app.on("activate", () => { if (!win) createWindow(); });
});

app.on("window-all-closed", () => app.quit());
app.on("will-quit", () => {
  globalShortcut.unregisterAll();
  link?.stop();
});
