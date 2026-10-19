/**
 * 伴随层 · 截屏与框选
 *
 * 流程刻意分成两段：
 *   1. **先冻结整屏**（desktopCapturer），把画面变成一张静态图；
 *   2. **再开覆盖层**让用户在这张图上拖框。
 *
 * 为什么不是"边框边实时截"：桌面在动，框选过程中下面的窗口可能刷新、弹窗、播放视频，
 * 用户框住的区域和他松手瞬间看到的区域会对不上。先冻结再框，所见即所得。
 *
 * 坐标换算不在这里做 —— 全部交给 capture-geometry.cjs 的纯函数，那份有测试。
 *
 * 为什么截图落临时文件而不是 data URL：一张 4K 图转 base64 是 8~10 MB 字符串，
 * 多屏时翻倍，经 IPC 传进渲染进程既慢又占内存。落盘再让 <img src="file://..."> 加载，
 * 省掉这一整段拷贝。
 */

"use strict";

const fs = require("node:fs/promises");
const path = require("node:path");
const { pathToFileURL } = require("node:url");

const { BrowserWindow, desktopCapturer, ipcMain, screen } = require("electron");

const { matchSource, toCropRect } = require("./capture-geometry.cjs");
const { workDir } = require("./paths.cjs");
const log = require("./log.cjs");

const OVERLAY_PRELOAD = path.join(__dirname, "overlay", "preload.cjs");
const OVERLAY_HTML = path.join(__dirname, "overlay", "index.html");

/** 框选层开太久会挡住整个桌面，超过这个时间自动收摊，防止用户被困。 */
const AUTO_CANCEL_MS = 120_000;

/**
 * @typedef {object} CaptureSession
 * @property {BrowserWindow[]} windows
 * @property {Map<string, {display: Electron.Display, image: Electron.NativeImage, file: string}>} shots
 * @property {NodeJS.Timeout | null} timer
 * @property {() => void} restoreBar
 */

/** @type {CaptureSession | null} */
let session = null;
/**
 * webContents.id → 该覆盖层的初始化数据。
 *
 * 用 Map 而不是在创建时直接 send：页面什么时候能收，只有页面自己知道
 * （见 begin() 里的注释）。重载多少次都能从这份表里重新取到同一份数据。
 *
 * @type {Map<number, {displayId: string, imageUrl: string, width: number, height: number}>}
 */
const overlayInit = new Map();
/** 回调只挂一次，避免多轮框选后 IPC 监听器越积越多 */
let hooksBound = false;
/** @type {{ onResult?: (r: {file: string, width: number, height: number}) => void, onCancel?: (reason: string) => void }} */
let listeners = {};

function capturesDir() {
  return path.join(workDir(), "captures");
}

function stamp(date = new Date()) {
  const p = (n, w = 2) => String(n).padStart(w, "0");
  return (
    `${date.getFullYear()}${p(date.getMonth() + 1)}${p(date.getDate())}` +
    `-${p(date.getHours())}${p(date.getMinutes())}${p(date.getSeconds())}-${p(date.getMilliseconds(), 3)}`
  );
}

function isActive() {
  return session !== null;
}

/**
 * 清掉上次异常退出留下的临时整屏图。
 *
 * 只在开新一轮框选时扫一次：这些文件是"冻结画面"的中转站，正常情况下成功/取消
 * 都会立刻删掉；进程被强杀时会残留 —— 一张 4K 图几 MB，攒着没意义。
 * 用 1 小时的阈值而不是全删，避免误删用户此刻正在框选的那一张。
 */
async function sweepStaleTemps() {
  const dir = capturesDir();
  let names;
  try {
    names = await fs.readdir(dir);
  } catch {
    return; // 目录还不存在，正常
  }

  const cutoff = Date.now() - 60 * 60 * 1000;
  for (const name of names) {
    if (!name.startsWith(".tmp-") || !name.endsWith(".png")) continue;
    const file = path.join(dir, name);
    try {
      const stat = await fs.stat(file);
      if (stat.mtimeMs < cutoff) await fs.unlink(file);
    } catch {
      // 单个文件删不掉不影响主流程
    }
  }
}

/**
 * 开一轮框选。
 *
 * @param {object} options
 * @param {() => void} options.hideBar 截图前把小条藏起来（否则它会被拍进去）
 * @param {() => void} options.showBar 截完恢复
 * @param {(r: {file: string, width: number, height: number}) => void} options.onResult
 * @param {(reason: string) => void} options.onCancel
 */
async function begin({ hideBar, showBar, onResult, onCancel }) {
  if (session) {
    log.warn("框选已在进行中，忽略重复触发");
    return;
  }

  bindIpc();
  listeners = { onResult, onCancel };
  await sweepStaleTemps();

  hideBar();
  // 让合成器把窗口真正撤下来再截，否则小条的残影会留在静态图里
  await new Promise((resolve) => setTimeout(resolve, 120));

  let sources;
  try {
    const displays = screen.getAllDisplays();
    const maxWidth = Math.max(...displays.map((d) => Math.round(d.bounds.width * d.scaleFactor)));
    const maxHeight = Math.max(...displays.map((d) => Math.round(d.bounds.height * d.scaleFactor)));
    sources = await desktopCapturer.getSources({
      types: ["screen"],
      thumbnailSize: { width: maxWidth, height: maxHeight },
      fetchWindowIcons: false,
    });
  } catch (err) {
    log.error(`截屏失败：${err && err.message ? err.message : String(err)}`);
    showBar();
    onCancel?.("capture-failed");
    return;
  }

  const displays = screen.getAllDisplays();
  const dir = capturesDir();
  await fs.mkdir(dir, { recursive: true });

  /** @type {CaptureSession["shots"]} */
  const shots = new Map();
  const tmpFiles = [];

  for (const [index, display] of displays.entries()) {
    const source = matchSource(display, index, sources);
    if (!source || !source.thumbnail || source.thumbnail.isEmpty()) {
      log.warn(`显示器 ${display.id} 没有可用的屏幕源，跳过`);
      continue;
    }
    // 落临时文件让渲染进程自己加载，别用 IPC 搬大字符串
    const file = path.join(dir, `.tmp-${stamp()}-${index}.png`);
    await fs.writeFile(file, source.thumbnail.toPNG());
    tmpFiles.push(file);
    shots.set(String(display.id), { display, image: source.thumbnail, file });
  }

  if (shots.size === 0) {
    await cleanup(tmpFiles);
    showBar();
    log.error("没有任何可用的屏幕源，无法框选");
    onCancel?.("no-screen-source");
    return;
  }

  session = { windows: [], shots, timer: null, restoreBar: showBar };

  for (const [displayId, shot] of shots) {
    const overlay = new BrowserWindow({
      ...shot.display.bounds,
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
      enableLargerThanScreen: true,
      backgroundColor: "#00000000",
      webPreferences: {
        preload: OVERLAY_PRELOAD,
        contextIsolation: true,
        nodeIntegration: false,
        sandbox: false,
      },
    });

    overlay.setAlwaysOnTop(true, "screen-saver");
    overlay.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
    overlay.setBounds(shot.display.bounds);

    // 初始化数据必须在 loadFile **之前**登记：页面的脚本在加载过程中就会
    // 发 overlay:ready，等到 await loadFile 之后再登记就晚了一步，
    // 那次请求会拿到空值，页面于是永远停在"透明但没画面"。
    overlayInit.set(overlay.webContents.id, {
      displayId,
      imageUrl: pathToFileURL(shot.file).href,
      width: shot.display.bounds.width,
      height: shot.display.bounds.height,
    });

    // 覆盖层需要键盘（Esc 取消），所以它是可聚焦的 —— 这是它和小条最大的不同
    await overlay.loadFile(OVERLAY_HTML);

    // 数据为什么由页面主动来要（overlay:ready → overlay:init），而不是这里推：
    // ①页面还没注册监听器时 send 会被静默丢弃；②页面一旦重载（devtools 刷新），
    // 推过的数据就没了。请求式让"何时能收"由接收方决定，重载多少次都能自愈。
    overlay.showInactive();
    overlay.focus();

    // 兜底的 Esc：不依赖渲染进程是否抢到焦点
    overlay.webContents.on("before-input-event", (_event, input) => {
      if (input.type === "keyDown" && input.key === "Escape") cancel("escape");
    });

    session.windows.push(overlay);
  }

  session.timer = setTimeout(() => cancel("timeout"), AUTO_CANCEL_MS);
  log.log(`框选开始：${shots.size} 个显示器`);
}

function bindIpc() {
  if (hooksBound) return;
  hooksBound = true;

  ipcMain.on("overlay:ready", (event) => {
    const init = overlayInit.get(event.sender.id);
    if (init) event.sender.send("overlay:init", init);
  });

  ipcMain.on("overlay:select", (_event, payload) => {
    void handleSelect(payload);
  });

  ipcMain.on("overlay:cancel", (_event, reason) => {
    cancel(reason || "user");
  });
}

/**
 * @param {{displayId: string, rect: {x:number,y:number,width:number,height:number}}} payload
 */
async function handleSelect(payload) {
  if (!session) return;
  const shot = session.shots.get(String(payload?.displayId));
  if (!shot) {
    log.warn(`框选返回了未知显示器：${payload?.displayId}`);
    cancel("unknown-display");
    return;
  }

  const crop = toCropRect({
    display: { width: shot.display.bounds.width, height: shot.display.bounds.height },
    image: shot.image.getSize(),
    selection: payload.rect,
  });

  if (!crop) {
    cancel("too-small");
    return;
  }

  const out = path.join(capturesDir(), `${stamp()}.png`);
  try {
    const png = shot.image.crop(crop).toPNG();
    await fs.writeFile(out, png);
    log.log(`框选完成：${crop.width}x${crop.height} → ${out}`);
    const result = { file: out, width: crop.width, height: crop.height };
    // 先把回调取出来再收摊：finish() 会清空 listeners，
    // 顺序反了就是"回调被清空后调用"，表现是截完图一声不响。
    const hooks = finish();
    hooks.onResult?.(result);
  } catch (err) {
    log.error(`保存截图失败：${err && err.message ? err.message : String(err)}`);
    const hooks = finish();
    hooks.onCancel?.("write-failed");
  }
}

/** @param {string} reason */
function cancel(reason) {
  if (!session) return;
  log.log(`框选取消：${reason}`);
  const hooks = finish();
  hooks.onCancel?.(reason);
}

/**
 * 收摊：关窗、删临时图、恢复小条。任何出口都必须过这里，否则会漏窗或漏文件。
 *
 * 返回**收摊前**的回调表 —— 调用方要在这之后才能安全地回调，
 * 而 listeners 此刻已经被清空（避免下一次框选拿到上一次的闭包）。
 *
 * @returns {{ onResult?: (r: {file: string, width: number, height: number}) => void, onCancel?: (reason: string) => void }}
 */
function finish() {
  const current = session;
  const hooks = listeners;
  session = null;
  listeners = {};

  if (!current) return hooks;
  if (current.timer) clearTimeout(current.timer);

  for (const win of current.windows) {
    if (!win.isDestroyed()) win.destroy();
  }

  const tmp = [...current.shots.values()].map((s) => s.file);
  void cleanup(tmp);
  try {
    current.restoreBar();
  } catch (err) {
    log.warn(`恢复小条失败：${err && err.message ? err.message : String(err)}`);
  }

  return hooks;
}

async function cleanup(files) {
  await Promise.all(
    files.map((file) => fs.rm(file, { force: true }).catch(() => {})),
  );
}

module.exports = { begin, cancel, isActive, capturesDir };
