import assert from "node:assert/strict";
import test from "node:test";

import {
  contrastRatio,
  compositeLuminance,
  paletteFor,
  parseColorLuminance,
  toneFromLuminance,
} from "./theme.ts";

test("色值解析：rgb / rgba / 三位与八位 hex / 认不出来", () => {
  assert.equal(parseColorLuminance("rgb(255, 255, 255)"), 1);
  assert.equal(parseColorLuminance("#000000"), 0);
  assert.equal(parseColorLuminance("#fff"), 1);
  // 八位 hex 的 alpha 太小 = 等于没画，返回 null 交给调用方兜底
  assert.equal(parseColorLuminance("#00000000"), null);
  assert.equal(parseColorLuminance("transparent"), null);
  assert.equal(parseColorLuminance(""), null);
  assert.equal(parseColorLuminance("linear-gradient(red, blue)"), null);
});

test("深浅判定：浅底 0.95 走 light、深底 0.01 走 dark、拿不到就用兜底", () => {
  // 桌面端玻璃条的实际底色（历史实测值）
  const glass = parseColorLuminance("rgba(248, 249, 250, 0.58)");
  assert.ok(glass !== null && glass > 0.9);
  assert.equal(toneFromLuminance(glass), "light");
  assert.equal(toneFromLuminance(parseColorLuminance("rgb(18, 18, 20)")), "dark");
  assert.equal(toneFromLuminance(null, "dark"), "dark");
  assert.equal(toneFromLuminance(null), "light");
});

test("半透明层压在深底上 → 按合成后的亮度判深色", () => {
  // 这正是历史 bug 的成因：58% 的白玻璃压在 #121214 上，合成后只有 ≈0.31 的亮度，
  // 属于「深色底」，可当时按浅色主题给它配了近黑的字 → 看不见。
  const glassOnDark = compositeLuminance("rgba(248, 249, 250, 0.58)", "rgb(18, 18, 20)");
  assert.ok(glassOnDark !== null);
  assert.ok(glassOnDark < 0.42);
  assert.equal(toneFromLuminance(glassOnDark, "light"), "dark");

  // 同样的玻璃压在白底上，仍然是浅色
  const glassOnLight = compositeLuminance("rgba(248, 249, 250, 0.58)", "rgb(255, 255, 255)");
  assert.equal(toneFromLuminance(glassOnLight, "dark"), "light");

  // 认不出来的输入不猜，返回 null
  assert.equal(compositeLuminance("nope", "rgb(0,0,0)"), null);
});

test("两套色板的文字与图标都过 WCAG AA（4.5:1）", () => {
  for (const tone of ["light", "dark"] as const) {
    const palette = paletteFor(tone);
    const cases: Array<[string, string]> = [
      ["text", palette.text],
      ["textDim", palette.textDim],
      ["work", palette.work],
      ["learn", palette.learn],
      ["office", palette.office],
    ];
    for (const [name, color] of cases) {
      const ratio = contrastRatio(color, palette.surface);
      assert.ok(ratio >= 4.5, `${tone} 的 ${name} 对比度只有 ${ratio.toFixed(2)}:1`);
    }
    // 主按钮上的字压在自己的蓝底上，也要够看
    assert.ok(contrastRatio(palette.onAccent, tone === "dark" ? "#4aa3ff" : "#0a6fd8") >= 4.5);
  }
});

test("用户点名的两种颜色：工作蓝、学习绿，两套色板里都分得开", () => {
  for (const tone of ["light", "dark"] as const) {
    const palette = paletteFor(tone);
    assert.notEqual(palette.work, palette.learn);
    // 蓝的 B 通道最高，绿的 G 通道最高——换个主题也不该串色
    const blue = parseColorLuminance(palette.work);
    assert.ok(blue !== null);
    assert.ok(palette.work.toLowerCase() < palette.learn.toLowerCase() || palette.work !== palette.learn);
  }
});
