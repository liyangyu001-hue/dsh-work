/**
 * 框选几何的测试
 *
 * 重点覆盖三件真实会出事的事：150% 缩放、副屏负坐标、边缘取整。
 */

import { test } from "node:test";
import assert from "node:assert/strict";

import { normalizeSelection, toCropRect, matchSource } from "./capture-geometry.cjs";

const display = { width: 2560, height: 1440 };

test("反向拖拽也能得到正矩形", () => {
  const rect = normalizeSelection({ x: 400, y: 300 }, { x: 100, y: 120 });
  assert.deepEqual(rect, { x: 100, y: 120, width: 300, height: 180 });
});

test("100% 缩放下像素与 DIP 一一对应", () => {
  const crop = toCropRect({
    display,
    image: { width: 2560, height: 1440 },
    selection: { x: 100, y: 200, width: 300, height: 150 },
  });
  assert.deepEqual(crop, { x: 100, y: 200, width: 300, height: 150 });
});

test("150% 缩放下选框被放大到物理像素", () => {
  const crop = toCropRect({
    display,
    image: { width: 3840, height: 2160 },
    selection: { x: 100, y: 200, width: 300, height: 150 },
  });
  assert.deepEqual(crop, { x: 150, y: 300, width: 450, height: 225 });
});

test("缩略图被等比缩小过时按实际比例换算，而不是按 scaleFactor", () => {
  // 请求 1920 宽的缩略图，显示器是 2560 → 比例 0.75
  const crop = toCropRect({
    display,
    image: { width: 1920, height: 1080 },
    selection: { x: 0, y: 0, width: 2560, height: 1440 },
  });
  assert.deepEqual(crop, { x: 0, y: 0, width: 1920, height: 1080 });
});

test("选框越界会被夹回显示器内", () => {
  const crop = toCropRect({
    display,
    image: { width: 2560, height: 1440 },
    selection: { x: 2500, y: 1400, width: 500, height: 500 },
  });
  assert.deepEqual(crop, { x: 2500, y: 1400, width: 60, height: 40 });
});

test("小于最小边长的误触返回 null", () => {
  const crop = toCropRect({
    display,
    image: { width: 2560, height: 1440 },
    selection: { x: 10, y: 10, width: 3, height: 400 },
  });
  assert.equal(crop, null);
});

test("裁剪矩形永不越出图像边界", () => {
  const crop = toCropRect({
    display,
    image: { width: 1000, height: 1000 },
    selection: { x: 0, y: 0, width: 2560, height: 1440 },
  });
  assert.ok(crop);
  assert.ok(crop.x + crop.width <= 1000);
  assert.ok(crop.y + crop.height <= 1000);
});

test("副屏负坐标：选框本就相对该屏，不需要再平移", () => {
  const left = { width: 1920, height: 1080 };
  const crop = toCropRect({
    display: left,
    image: { width: 1920, height: 1080 },
    selection: { x: 50, y: 60, width: 200, height: 100 },
  });
  assert.deepEqual(crop, { x: 50, y: 60, width: 200, height: 100 });
});

test("优先按 display_id 匹配屏幕源，缺失时按顺序回退", () => {
  const sources = [
    { id: "screen:0:0", display_id: "111" },
    { id: "screen:1:0", display_id: "222" },
  ];
  assert.equal(matchSource({ id: 222, bounds: {} }, 0, sources)?.id, "screen:1:0");

  const noIds = [{ id: "a", display_id: "" }, { id: "b", display_id: "" }];
  assert.equal(matchSource({ id: 999, bounds: {} }, 1, noIds)?.id, "b");
  assert.equal(matchSource({ id: 999, bounds: {} }, 5, noIds), null);
});
