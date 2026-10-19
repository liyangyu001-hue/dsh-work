/**
 * 伴随层 · 框选几何（纯函数，可在 Node 里直接测）
 *
 * 这一层单独拎出来的原因：坐标系是这类软件最容易出"看起来对、边缘差两像素"的地方，
 * 而它又必须有图形界面才能肉眼验证。把换算抽成纯函数，就能在 150% 缩放、负坐标副屏
 * 这些真实场景下用测试钉死，而不是靠截图对着数格子。
 *
 * 三套坐标，别混：
 *   - 屏幕坐标（DIP）：Electron 的 BrowserWindow / 鼠标事件都在这套里，可能带负值。
 *   - 显示器局部坐标（DIP）：相对该显示器左上角，框选拖拽发生在这里。
 *   - 图像像素：desktopCapturer 拿到的缩略图，物理像素，通常 = DIP × scaleFactor。
 */

"use strict";

/**
 * 由一次拖拽的起止点得到规范矩形（支持反向拖拽）。
 * @param {{x:number,y:number}} start
 * @param {{x:number,y:number}} end
 * @returns {{x:number,y:number,width:number,height:number}}
 */
function normalizeSelection(start, end) {
  return {
    x: Math.min(start.x, end.x),
    y: Math.min(start.y, end.y),
    width: Math.abs(end.x - start.x),
    height: Math.abs(end.y - start.y),
  };
}

function clamp(value, min, max) {
  return value < min ? min : value > max ? max : value;
}

/**
 * 把「显示器局部坐标下的选框」换算成「图像像素下的裁剪矩形」。
 *
 * 不用 scaleFactor 直接乘：缩略图的实际尺寸与 scaleFactor 不总是严格相等
 * （Electron 会按请求的 thumbnailSize 做等比缩放）。所以用
 * `图像宽 / 显示器宽` 反算比例，让两边永远自洽。
 *
 * @param {object} input
 * @param {{width:number,height:number}} input.display 显示器尺寸（DIP）
 * @param {{width:number,height:number}} input.image 缩略图尺寸（像素）
 * @param {{x:number,y:number,width:number,height:number}} input.selection 选框（DIP，相对显示器）
 * @param {number} [input.minSize] 小于这个边长就当误触
 * @returns {{x:number,y:number,width:number,height:number} | null} 像素矩形；误触返回 null
 */
function toCropRect({ display, image, selection, minSize = 8 }) {
  if (!display || !image || !selection) return null;
  if (display.width <= 0 || display.height <= 0) return null;
  if (image.width <= 0 || image.height <= 0) return null;

  const left = clamp(selection.x, 0, display.width);
  const top = clamp(selection.y, 0, display.height);
  const right = clamp(selection.x + selection.width, 0, display.width);
  const bottom = clamp(selection.y + selection.height, 0, display.height);

  if (right - left < minSize || bottom - top < minSize) return null;

  const ratioX = image.width / display.width;
  const ratioY = image.height / display.height;

  const x = Math.round(left * ratioX);
  const y = Math.round(top * ratioY);
  // 右/下边界单独取整，避免"宽高分别取整"在边缘多出一像素
  const maxX = Math.round(right * ratioX);
  const maxY = Math.round(bottom * ratioY);

  const width = Math.max(1, Math.min(maxX - x, image.width - x));
  const height = Math.max(1, Math.min(maxY - y, image.height - y));

  return { x, y, width, height };
}

/**
 * 把显示器匹配到 desktopCapturer 的 source。
 *
 * Windows 上 `display_id` 可能为空字符串（老版本 / 某些驱动），
 * 这时退回按顺序配对 —— 顺序与 screen.getAllDisplays() 一致。
 *
 * @param {{id:number, bounds:{x:number,y:number,width:number,height:number}}} display
 * @param {number} index
 * @param {Array<{id:string, display_id:string}>} sources
 */
function matchSource(display, index, sources) {
  const byId = sources.find((s) => s.display_id && String(s.display_id) === String(display.id));
  if (byId) return byId;
  return sources[index] ?? null;
}

module.exports = { normalizeSelection, toCropRect, matchSource };
