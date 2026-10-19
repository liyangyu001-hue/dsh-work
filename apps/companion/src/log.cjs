/**
 * 伴随层 · 日志
 *
 * 为什么单独一个文件而不是 console：伴随层是没有终端的常驻进程，
 * 出问题时用户唯一能提供的就是日志。所以所有可诊断事件都落到
 * `%USERPROFILE%\.dsh\dsh-work\companion.log`（与本机令牌同目录，但不含令牌）。
 *
 * 只追加、只保留最近 N 行：常驻进程写日志最怕无限增长。
 */

"use strict";

const { appendFile, mkdir, readFile, writeFile } = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");

const MAX_LINES = 800;

function logFile() {
  const home = typeof process.env["DSH_HOME"] === "string" && process.env["DSH_HOME"].trim() !== ""
    ? process.env["DSH_HOME"].trim()
    : path.join(os.homedir(), ".dsh");
  return path.join(home, "dsh-work", "companion.log");
}

/** 串行化写入：并发 append 在 Windows 上偶发 EBUSY。 */
let chain = Promise.resolve();

function stamp() {
  const d = new Date();
  return d.toISOString().replace("T", " ").slice(0, 19);
}

/**
 * @param {string} level
 * @param {string} message
 */
function write(level, message) {
  const line = `${stamp()} [${level}] ${message}\n`;
  chain = chain
    .then(async () => {
      const file = logFile();
      await mkdir(path.dirname(file), { recursive: true });
      await appendFile(file, line, "utf8");
      await trimIfNeeded(file);
    })
    .catch(() => {
      // 日志失败不能影响主流程
    });
  return chain;
}

async function trimIfNeeded(file) {
  const text = await readFile(file, "utf8");
  const lines = text.split("\n");
  if (lines.length <= MAX_LINES) return;
  await writeFile(file, lines.slice(-MAX_LINES).join("\n"), "utf8");
}

module.exports = {
  log: (message) => write("info", message),
  warn: (message) => write("warn", message),
  error: (message) => write("error", message),
  logFile,
};
