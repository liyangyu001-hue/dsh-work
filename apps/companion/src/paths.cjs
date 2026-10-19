/**
 * 伴随层 · 路径
 *
 * endpoint.json / config.json / companion.log 三个文件都躺在同一个目录。
 * 这个口径原先在 host-link 与 log 里各写了一遍，再加一个 config 就会三处漂移，
 * 所以收成一处：目录只由 DSH_HOME（或 ~/.dsh）决定。
 *
 * 为什么不是 TS：这几个文件由 Electron 主进程直接执行，没有编译步骤。
 */

"use strict";

const os = require("node:os");
const path = require("node:path");

function dshHome() {
  const fromEnv = process.env["DSH_HOME"];
  if (typeof fromEnv === "string" && fromEnv.trim() !== "") return fromEnv.trim();
  return path.join(os.homedir(), ".dsh");
}

function workDir() {
  return path.join(dshHome(), "dsh-work");
}

module.exports = { dshHome, workDir };
