/**
 * 打包客户端半边。
 *
 * 为什么需要这一层壳：宿主用 <script src="plugins/??<pkg>/client.js"> 加载
 * 客户端模块，文件必须**自己**调用 __ModuleLoader__.load 注册，
 * 而不是 ESM 的 export。esbuild 只负责把 TSX/依赖压成一个 CJS 体，
 * 壳由本脚本套上（与 @michengai/dsh-codex-pet 的产物形态一致）。
 *
 * react / react-dom 必须 external —— 宿主已在自己的种子里提供，
 * 打进来会造成两份 React，hooks 立刻报 "Invalid hook call"。
 */

import { build } from "esbuild";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");

/** 必须与 package.json 的 name 完全一致：宿主按 id 对账 entries。 */
const PACKAGE_ID = "@dshwork/plugin";

const entry = resolve(root, "packages/plugin/src/client/index.tsx");
const outfile = resolve(root, "packages/plugin/lib/client.js");

const result = await build({
  entryPoints: [entry],
  bundle: true,
  format: "cjs",
  platform: "browser",
  target: "chrome120",
  jsx: "automatic",
  external: ["react", "react-dom", "react/jsx-runtime", "react-dom/client"],
  write: false,
  logLevel: "warning",
  // 开发期保留可读的函数名，生产也不做压缩：
  // 客户端代码总量很小，可读性对"半年后回来改"更重要
  minify: false,
  sourcemap: false,
});

const body = result.outputFiles[0].text;

const wrapped = `window.__ModuleLoader__.load({id:${JSON.stringify(PACKAGE_ID)},factory:function(require){var module={exports:{}};var exports=module.exports;
${body}
return module.exports;}});
`;

await mkdir(dirname(outfile), { recursive: true });
await writeFile(outfile, wrapped, "utf8");

console.log(`[build-client] ${PACKAGE_ID} → ${outfile} (${(wrapped.length / 1024).toFixed(1)} kB)`);
