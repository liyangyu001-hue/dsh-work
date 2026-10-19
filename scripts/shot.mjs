/**
 * 视觉验收脚本：给一个 URL，截图 + 收集控制台错误。
 *
 * 为什么不用 `chrome --screenshot`：DSH 页面挂着长连接（SSE / WebSocket），
 * 命令行截图的"等网络空闲"永远等不到，会直接挂死。这里用 Playwright 显式
 * 控制等待时机（domcontentloaded + 固定延时），并在超时后一定会退出。
 *
 * 用法：
 *   node scripts/shot.mjs <url> <out.png> [等待毫秒]
 * 依赖：playwright-core（复用本机已装的 Chrome，不额外下载浏览器）
 */

import { chromium } from "playwright-core";

const [url, out, waitMsRaw] = process.argv.slice(2);
if (!url || !out) {
  console.error("用法: node scripts/shot.mjs <url> <out.png> [等待毫秒]");
  process.exit(2);
}
const waitMs = Number(waitMsRaw ?? 5000);

const browser = await chromium.launch({ channel: "chrome", headless: true });
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });

const errors = [];
page.on("pageerror", (err) => errors.push(`pageerror: ${err.message}`));
page.on("console", (msg) => {
  if (msg.type() === "error") errors.push(`console.error: ${msg.text()}`);
});

try {
  await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 });
  // 等 React 挂载 + 我们的轮询首帧；这段时间是给"渲染完成"留的余量，不是等网络空闲
  await page.waitForTimeout(waitMs);
  await page.screenshot({ path: out });

  // 站点自身的关键状态用文本取证，避免只靠肉眼看图
  const probe = await page.evaluate(() => {
    const bar = document.querySelector('[aria-live="polite"]');
    return {
      title: document.title,
      hasBar: Boolean(bar),
      barText: bar?.textContent ?? null,
      bodyHead: document.body.innerText.replace(/\s+/g, " ").slice(0, 160),
    };
  });
  console.log(JSON.stringify({ ok: true, shot: out, probe, errors }, null, 2));
} catch (err) {
  console.log(JSON.stringify({ ok: false, shot: null, error: String(err), errors }, null, 2));
  process.exitCode = 1;
} finally {
  await browser.close();
}
