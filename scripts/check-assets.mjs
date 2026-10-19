/**
 * 文档配图守卫：`docs/assets` 下只允许放"由脚本渲染的合成图"。
 *
 * 起因很具体：早期把桌面截图直接提交进了公开仓库，图里带着用户其他窗口、
 * 浏览器标签页和账号名。事后清理历史很贵，所以把规则做成机器能查的东西：
 *
 *   1. 文件名必须在下面的白名单里（白名单就是 `scripts/make-shots.mjs` 的产物）；
 *   2. 长边不得超过 2400px（桌面截图通常更大；合成图按 DPR=2 渲染，不会超）。
 *
 *   node scripts/check-assets.mjs
 */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join } from "node:path";

const DIR = "docs/assets";
const ALLOWED = new Set(["bar.png", "panel.png"]);
const MAX_EDGE = 2400;

/** 只读 PNG 头里的 IHDR，避免为一个检查引入图片库。 */
function pngSize(path) {
  const header = readFileSync(path).subarray(16, 24);
  return { width: header.readUInt32BE(0), height: header.readUInt32BE(4) };
}

const problems = [];
for (const name of readdirSync(DIR)) {
  const path = join(DIR, name);
  if (!statSync(path).isFile()) continue;
  if (!ALLOWED.has(name)) {
    problems.push(`${path}：不在白名单里。文档配图请用 scripts/make-shots.mjs 生成，不要提交桌面截图。`);
    continue;
  }
  if (!name.endsWith(".png")) continue;
  const { width, height } = pngSize(path);
  if (Math.max(width, height) > MAX_EDGE) {
    problems.push(`${path}：${width}×${height} 超过 ${MAX_EDGE}px，像截图不像合成图。`);
  }
}

if (problems.length > 0) {
  console.error(problems.join("\n"));
  process.exit(1);
}
console.log(`docs/assets 检查通过（${ALLOWED.size} 张合成图）`);
