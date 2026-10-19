"use strict";

/**
 * 背景取样：把"窗口背后那一块桌面"抓下来，当作玻璃的底。
 *
 * 为什么需要它
 * ─────────────
 * CSS 的 `backdrop-filter` 只模糊**同一个页面里**位于其后的内容。对我们这种
 * 透明窗口来说，背后是桌面（另一个进程、甚至另一个 DWM 表面），浏览器拿不到，
 * 于是 `backdrop-filter` 什么也不做 —— 面板会变成一块半透明色板，而不是玻璃。
 *
 * Win11 的 `backgroundMaterial: "acrylic"` 能真的模糊桌面，但它由 DWM 画在
 * 窗口矩形上，**没有圆角**，而且不可控（不能调模糊半径、不能跟着面板变）。
 * 我们试过：圆角全被吃掉，iOS 的观感就没了。
 *
 * 所以走第三条路：自己抓一张背后的截图，塞进面板里做背景，用 CSS 去模糊它。
 * 代价是展开面板时多一次 ~120ms 的截屏；换来的是真正的毛玻璃 + 圆角 + 可控。
 *
 * 边界：只取面板所在显示器的那一块；抓不到就返回 null，调用方退化成纯色玻璃。
 */

const { desktopCapturer, screen } = require("electron");
const { toCropRect, matchSource } = require("./capture-geometry.cjs");
const log = require("./log.cjs");

/**
 * 抓取某个矩形区域，返回可直接塞进 <img src> 的 data URL。
 *
 * @param {{x:number,y:number,width:number,height:number}} rect 屏幕坐标（DIP），通常就是面板自己的 bounds
 * @returns {Promise<string | null>}
 */
async function grab(rect) {
  if (!rect || rect.width <= 0 || rect.height <= 0) return null;

  try {
    const displays = screen.getAllDisplays();
    const display = screen.getDisplayMatching({
      x: Math.round(rect.x),
      y: Math.round(rect.y),
      width: Math.round(rect.width),
      height: Math.round(rect.height),
    });

    // 只抓这块屏幕，尺寸按它的物理像素算：抓整屏再裁会浪费一倍内存
    const width = Math.round(display.bounds.width * display.scaleFactor);
    const height = Math.round(display.bounds.height * display.scaleFactor);
    const sources = await desktopCapturer.getSources({
      types: ["screen"],
      thumbnailSize: { width, height },
      fetchWindowIcons: false,
    });

    const index = displays.findIndex((d) => d.id === display.id);
    const source = matchSource(display, index < 0 ? 0 : index, sources);
    if (!source || !source.thumbnail || source.thumbnail.isEmpty()) return null;

    // 面板 bounds 是世界坐标；toCropRect 要的是相对这块屏幕的坐标
    const selection = {
      x: rect.x - display.bounds.x,
      y: rect.y - display.bounds.y,
      width: rect.width,
      height: rect.height,
    };

    // 注意：nativeImage 没有 width/height 属性，几何换算要的是 getSize() 的纯数据
    const crop = toCropRect({ display: display.bounds, image: source.thumbnail.getSize(), selection });
    if (!crop) return null;

    const png = source.thumbnail.crop(crop).toPNG();
    if (png.length === 0) return null;

    return `data:image/png;base64,${png.toString("base64")}`;
  } catch (err) {
    // 背景取样失败不该影响面板本身：退化成纯色玻璃即可
    log.warn(`背景取样失败：${err && err.message ? err.message : String(err)}`);
    return null;
  }
}

module.exports = { grab };
