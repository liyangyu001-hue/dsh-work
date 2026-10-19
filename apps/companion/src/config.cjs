/**
 * 伴随层 · 配置
 *
 * 只有一个来源：`~/.dsh/dsh-work/config.json`（可选文件）。这一轮不做设置页，
 * 但"热键别写死"是硬要求 —— 用户换键只该改文件，不该改代码。
 *
 * 两条设计底线：
 *   1. **坏配置不能让伴随层起不来**：任何解析失败都退回默认值，并把原因交给调用方写日志。
 *   2. **危险开关必须显式打开**：裸 Tab 是输入框与键盘导航的公共键，只有布尔 true 才算数
 *      （写成字符串 "true" 一律当没开，避免"看起来开了其实没开"）。
 *
 * 为什么是 .cjs：Electron 主进程直接 require，没有编译步骤（同 main.cjs）。
 */

"use strict";

const { readFileSync } = require("node:fs");
const path = require("node:path");

const { workDir } = require("./paths.cjs");

const DEFAULT_HOTKEY = "Control+Alt+Space";

/** @typedef {object} CompanionConfig */
/** @property {string} hotkey Electron accelerator 写法 */
/** @property {boolean} allowBareTab 是否允许把裸 Tab 注册成全局热键 */

/** @returns {CompanionConfig} */
function defaults() {
  return { hotkey: DEFAULT_HOTKEY, allowBareTab: false };
}

function configFile() {
  return path.join(workDir(), "config.json");
}

/**
 * 把热键写法归一化：去掉各段空白、拒绝空段与非字母数字字符。
 * 只做"形状"校验，键名本身是否被 Electron 认识留给 globalShortcut.register 判定。
 *
 * @param {unknown} value
 * @returns {string | null} 归一化后的组合键；非法返回 null
 */
function normalizeHotkey(value) {
  if (typeof value !== "string") return null;
  const parts = value.split("+").map((part) => part.trim());
  if (parts.some((part) => !/^[A-Za-z0-9]+$/.test(part))) return null;
  return parts.join("+");
}

/**
 * 是否"裸 Tab"：只有一个键位且就是 Tab（大小写不敏感）。
 * 带任何修饰键（Control+Tab）都不算。
 *
 * @param {string} combo
 */
function isBareTab(combo) {
  const parts = String(combo)
    .split("+")
    .map((part) => part.trim())
    .filter(Boolean);
  return parts.length === 1 && parts[0].toLowerCase() === "tab";
}

/**
 * 解析配置文本。不抛异常。
 *
 * @param {string} text
 * @returns {{ config: CompanionConfig, problems: string[] }}
 */
function parseConfig(text) {
  const config = defaults();
  const problems = [];

  let raw;
  try {
    // Windows 编辑器可能写 BOM，JSON.parse 不认
    raw = JSON.parse(String(text).replace(/^\uFEFF/, ""));
  } catch {
    return { config, problems: ["config.json 不是合法 JSON，使用默认配置"] };
  }

  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) {
    return { config, problems: ["config.json 顶层必须是对象，使用默认配置"] };
  }

  if ("hotkey" in raw) {
    const hotkey = normalizeHotkey(raw.hotkey);
    if (hotkey === null) {
      problems.push(`hotkey 不是合法的组合键，使用默认值 ${DEFAULT_HOTKEY}`);
    } else {
      config.hotkey = hotkey;
    }
  }

  if ("allowBareTab" in raw) {
    if (typeof raw.allowBareTab === "boolean") {
      config.allowBareTab = raw.allowBareTab;
    } else {
      problems.push("allowBareTab 必须是布尔值（字符串 \"true\" 不算），按关闭处理");
    }
  }

  return { config, problems };
}

/**
 * 读配置文件。文件不存在是正常情况（用默认值），不算问题。
 *
 * @param {string} [file]
 * @returns {{ config: CompanionConfig, problems: string[], file: string, exists: boolean }}
 */
function readConfig(file = configFile()) {
  let text;
  try {
    text = readFileSync(file, "utf8");
  } catch (err) {
    if (err && err.code === "ENOENT") {
      return { config: defaults(), problems: [], file, exists: false };
    }
    return {
      config: defaults(),
      problems: [`读不到 config.json（${err && err.message ? err.message : String(err)}），使用默认配置`],
      file,
      exists: false,
    };
  }
  return { ...parseConfig(text), file, exists: true };
}

/**
 * 决定这次要不要注册，以及注册什么。
 * 裸 Tab 且未显式放行时**不注册**（也不替换成默认键，免得用户以为改生效了）。
 *
 * @param {CompanionConfig} config
 * @returns {{ ok: true, combo: string } | { ok: false, combo: string, reason: string }}
 */
function resolveHotkey(config) {
  const combo = config && typeof config.hotkey === "string" ? config.hotkey : DEFAULT_HOTKEY;
  if (isBareTab(combo) && !(config && config.allowBareTab === true)) {
    return {
      ok: false,
      combo,
      reason: "裸 Tab 默认不注册：它是输入框与键盘导航的公共键，确需占用请在 config.json 写 allowBareTab: true",
    };
  }
  return { ok: true, combo };
}

module.exports = {
  DEFAULT_HOTKEY,
  configFile,
  isBareTab,
  normalizeHotkey,
  parseConfig,
  readConfig,
  resolveHotkey,
};
