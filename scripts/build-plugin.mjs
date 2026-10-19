/**
 * 构建 host 半边：把 packages/plugin/src 打成一个 ESM 单文件 lib/index.js。
 *
 * 三条硬约束：
 * 1. `@deepseek-ai/dsh-tools` 必须 external —— 它由宿主在运行期解析，npm 上没有可装的真身。
 * 2. 产物是 ESM：DSH 的插件加载走 import()。
 * 3. 产物里不留任何 `require`/`__dirname` 假设，路径一律用 import.meta.url 推导。
 *
 * 用法：node scripts/build-plugin.mjs [--watch]
 */

import { build, context } from 'esbuild';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const packageDir = path.resolve(here, '..', 'packages', 'plugin');

/** 宿主提供、绝不打进产物的模块。 */
const HOST_MODULES = ['@deepseek-ai/*', 'cordis', 'schemastery', '@deepseek-ai/schemastery'];

/** @type {import('esbuild').BuildOptions} */
const options = {
  entryPoints: [path.join(packageDir, 'src', 'index.ts')],
  outfile: path.join(packageDir, 'lib', 'index.js'),
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: ['node22'],
  sourcemap: false,
  legalComments: 'none',
  logLevel: 'info',
  external: HOST_MODULES,
  banner: {
    js: '// 由 scripts/build-plugin.mjs 生成，请勿手改。源码在 packages/plugin/src/。',
  },
};

if (process.argv.includes('--watch')) {
  const ctx = await context(options);
  await ctx.watch();
  console.log('[build-plugin] watching…');
} else {
  await build(options);
  console.log('[build-plugin] 完成 →', path.relative(process.cwd(), options.outfile));
}
