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
const { HostLink, readEndpoint } = require("./host-link.cjs");
const capture = require("./capture.cjs");
const backdrop = require("./backdrop.cjs");
const { readConfig, resolveHotkey } = require("./config.cjs");
const log = require("./log.cjs");

/** 小条窗口尺寸：高度留出上下各 22px 给投影，宽度按设计系统上限 520 + 边距。 */
const BAR_WIDTH = 564;
const BAR_HEIGHT = 88;
/** 面板尺寸：卡片带要露出左右邻卡，详情栏要能放下"目标/验收/建议"三段。 */
const PANEL_WIDTH = 760;
const PANEL_HEIGHT = 396;
/** 距屏幕顶部的偏移（贴着标题栏下沿，但不压系统拖拽带）。 */
const TOP_OFFSET = 6;

/** 全局热键的默认值在 config.cjs 里；这里只关心"注没注册上"。 */

/** @type {BrowserWindow | null} */
let win = null;
/** @type {BrowserWindow | null} */
let panel = null;
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
/** 面板最近一次采到的桌面底图（data URL），首帧挂载时回放 */
let lastBackdrop = null;

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
  if (panel && !panel.isDestroyed()) panel.webContents.send(channel, payload);
}

/** 只发给小条：小条和面板对同一事件的反应不同（例如"已截取"提示面板不显示气泡）。 */
function sendBar(channel, payload) {
  if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

function panelBounds() {
  const { workArea } = screen.getPrimaryDisplay();
  return {
    width: PANEL_WIDTH,
    height: PANEL_HEIGHT,
    x: Math.round(workArea.x + (workArea.width - PANEL_WIDTH) / 2),
    // 贴在小条正下方：两者是同一个东西的"收起态 / 展开态"
    y: workArea.y + TOP_OFFSET + BAR_HEIGHT + 2,
  };
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

/**
 * 面板：这个产品真正的承载体。
 *
 * 小条只回答"现在到哪了"，面板回答"这一步干什么、下一步是什么、我能做什么"。
 * 两者是同一个东西的收起态与展开态，所以面板永远贴在小条正下方弹出。
 *
 * 与创建小条时的三个不同：
 *   ① 面板**必须能拿焦点**（要接键盘：Tab/Enter/Backspace），小条则故意不抢焦点；
 *   ② 面板不做点击穿透（它就是来接收点击的）；
 *   ③ 面板初始隐藏，由热键/小条唤出，失焦即收起。
 */
function createPanel() {
  panel = new BrowserWindow({
    ...panelBounds(),
    show: false,
    frame: false,
    transparent: true,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    fullscreenable: false,
    skipTaskbar: true,
    hasShadow: false,
    alwaysOnTop: true,
    backgroundColor: "#00000000",
    webPreferences: {
      preload: path.join(__dirname, "panel", "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  });

  panel.setAlwaysOnTop(true, "screen-saver");
  panel.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
  panel.loadFile(path.join(__dirname, "panel", "index.html"));

  panel.webContents.on("console-message", (_event, ...args) => {
    const details = args[0] && typeof args[0] === "object" ? args[0] : null;
    log.log(`[panel] ${details ? details.message : args[1]}`);
  });

  // 点到别处就收起：悬浮面板留在屏幕上挡视线，比"多按一次 Esc"更烦人。
  // 框选期间例外 —— 那时是我们主动藏起来的，不应触发收起逻辑。
  panel.on("blur", () => {
    if (!capture.isActive()) hidePanel();
  });

  panel.on("closed", () => { panel = null; });
}

function isPanelOpen() {
  return Boolean(panel && !panel.isDestroyed() && panel.isVisible());
}

function showPanel() {
  if (!panel || panel.isDestroyed()) createPanel();
  if (!panel) return;
  panel.setBounds(panelBounds());
  panel.show();
  panel.focus();
  // 采样与首帧状态互不依赖：先发状态（快），背景慢慢补（慢 120ms）
  void refreshBackdrop();
  if (panel.webContents.isLoading()) return;
  panel.webContents.send("host:state", lastPayload);
}

/**
 * 重新采一张"面板背后的桌面"，供面板做毛玻璃底。
 *
 * 采集前把面板设成完全透明（setOpacity(0)）而不是先隐藏：
 * 隐藏会让面板闪一下，而 opacity=0 时它仍在原位置，只是不参与合成结果，
 * 于是这一帧拍到的正好是"没有面板的桌面"。采完立刻恢复。
 */
async function refreshBackdrop() {
  if (!panel || panel.isDestroyed() || !panel.isVisible()) return;

  const bounds = panel.getBounds();
  panel.setOpacity(0);
  const url = await backdrop.grab(bounds);
  if (!panel || panel.isDestroyed()) return;

  panel.setOpacity(1);
  if (url) {
    lastBackdrop = url;
    log.log(`背景已取样 ${Math.round(bounds.width)}x${Math.round(bounds.height)}`);
    panel.webContents.send("panel:backdrop", url);
  }
}

function hidePanel() {
  if (!panel || panel.isDestroyed()) return;
  if (panel.isVisible()) panel.hide();
}

function togglePanel() {
  if (isPanelOpen()) hidePanel();
  else showPanel();
}

/**
 * 把一条意图交给宿主。
 *
 * 渲染层不碰网络、不碰令牌：它只说"我按了完成"，由主进程带着令牌去 POST。
 * 这样面板页面即使在 devtools 里被翻遍，也拿不到能操作宿主的凭证。
 *
 * @param {object} intent
 */
async function postIntent(intent) {
  const endpoint = await readEndpoint();
  if (!endpoint) {
    log.warn(`意图未发送（DSH 未连接）：${intent?.type}`);
    return;
  }

  try {
    const res = await fetch(`${endpoint.url}/dsh-work/api/intent`, {
      method: "POST",
      headers: { "content-type": "application/json", "x-dsh-work-token": endpoint.token },
      body: JSON.stringify({ intent, by: "human" }),
    });
    if (!res.ok) {
      log.warn(`意图被拒绝（HTTP ${res.status}）：${intent?.type}`);
    }
  } catch (err) {
    log.warn(`意图发送失败：${err && err.message ? err.message : String(err)}`);
  }
}

/**
 * 热键按下后的唯一出口。
 *
 * 做成"再按一次就取消"而不是"按了没反应"：框选层盖住整个桌面，
 * 用户中途反悔时最自然的动作就是再按一次刚才那个键。
 */
function triggerCapture() {
  if (capture.isActive()) {
    capture.cancel("hotkey-toggle");
    return;
  }

  // 面板也一起藏：它盖在屏幕上方，不藏就会被拍进静态图
  const panelWasOpen = isPanelOpen();
  hidePanel();

  void capture.begin({
    // 先把自己藏起来：小条也在截屏范围内，不藏就会被拍进静态图，
    // 用户框选时看到自己框里有一块"幽灵小条"
    hideBar: () => win?.hide(),
    showBar: () => {
      win?.showInactive();
      if (panelWasOpen) showPanel();
    },
    onResult: (result) => {
      sendBar("capture:result", result);
      if (panel && !panel.isDestroyed()) panel.webContents.send("capture:result", result);
      sendBar("hotkey", null);
    },
    onCancel: (reason) => sendBar("capture:cancel", reason),
  });
}

/**
 * 热键的统一出口。
 *
 * 第一版是"按下即截图"，但那等于把面板（任务、详情、操作）这个承载体整个跳过了；
 * 现在改成"按下开面板"，框选退到面板里的一个动作（Tab）。语义上更接近
 * "Tab 打开一个能承载信息的面"。
 */
function onHotkey(combo) {
  log.log(`热键按下：${combo}`);
  if (capture.isActive()) {
    capture.cancel("hotkey-toggle");
    return;
  }
  sendBar("hotkey", combo);
  togglePanel();
}

/**
 * 注册全局热键。
 *
 * 失败一定要留痕：热键被别的软件占用是常态，用户只会看到"按了没反应"，
 * 日志是唯一能区分"没注册上"和"注册上了但窗口没显示"的证据。
 *
 * @param {{ hotkey: string, allowBareTab: boolean }} config
 */
function registerHotkey(config) {
  const resolved = resolveHotkey(config);
  if (!resolved.ok) {
    log.warn(`热键未注册：${resolved.reason}`);
    return;
  }

  try {
    const ok = globalShortcut.register(resolved.combo, () => onHotkey(resolved.combo));
    if (!ok) {
      log.warn(`热键注册失败（可能被其他软件占用）：${resolved.combo}`);
      return;
    }
    log.log(`热键已注册：${resolved.combo}`);
  } catch (err) {
    // 非法 accelerator 在部分 Electron 版本上直接抛异常，不能让它带崩整个伴随层
    log.error(`热键注册异常：${resolved.combo} ${err && err.message ? err.message : String(err)}`);
  }
}

app.whenReady().then(() => {
  log.log(`伴随层启动 pid=${process.pid} electron=${process.versions.electron}`);

  // 配置先读：热键注册失败的原因（文件坏了 / 裸 Tab / 被占用）要按顺序都能在日志里看到
  const { config, problems, file, exists } = readConfig();
  for (const problem of problems) log.warn(`配置：${problem}`);
  log.log(`配置 ${file} ${exists ? "已读取" : "不存在，使用默认值"}`);

  const display = screen.getPrimaryDisplay();
  log.log(
    `display bounds=${JSON.stringify(display.bounds)} workArea=${JSON.stringify(display.workArea)} scale=${display.scaleFactor}`,
  );
  createWindow();
  log.log(`window bounds=${JSON.stringify(win?.getBounds())}`);
  createPanel();
  registerHotkey(config);

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
  // 小条页面的 console 转发到日志文件：这个窗口没有 devtools 可开，
  // 出问题时"页面里到底发生了什么"只能靠这条缝看出去。
  win.webContents.on("console-message", (_event, ...args) => {
    const details = args[0] && typeof args[0] === "object" ? args[0] : null;
    const message = details ? details.message : args[1];
    log.log(`[bar] ${message}`);
  });

  ipcMain.on("bar:ready", () => sendBar("host:state", lastPayload));
  ipcMain.on("bar:open", () => showPanel());

  /* ── 面板 → 主进程 ──────────────────────────────────────
   * 面板只发意图，真正的动作（发网络请求、开框选、关窗）都在主进程做。
   */
  ipcMain.on("panel:ready", (event) => {
    if (event.sender.isDestroyed()) return;
    event.sender.send("host:state", lastPayload);
    if (lastBackdrop) event.sender.send("panel:backdrop", lastBackdrop);
  });
  ipcMain.on("panel:intent", (_event, intent) => void postIntent(intent));
  ipcMain.on("panel:capture", () => {
    // 不在这里 hidePanel：triggerCapture 要先读到"面板原本开着"这个事实，
    // 再由它把面板和小条一起藏起来，框选结束/取消时按原样恢复。
    triggerCapture();
  });
  ipcMain.on("panel:close", () => hidePanel());

  app.on("activate", () => { if (!win) createWindow(); });
});

app.on("window-all-closed", () => app.quit());
app.on("will-quit", () => {
  globalShortcut.unregisterAll();
  link?.stop();
});
