/**
 * 伴随层 · 宿主连接器（CommonJS）
 *
 * 职责：找到 DSH 里那个插件的端口与令牌，订阅状态流，断线自动重连。
 * 这一层**不含任何 UI 逻辑**，只把"最新快照"和"连接状态"交给窗口。
 *
 * 传输选择见 docs/ADR/0004-伴随层传输走HTTP与SSE.md：
 * 服务端推 = SSE，客户端发 = POST，语义与 WebSocket 等价但零依赖。
 *
 * 为什么是纯 JS 而不是 TS：这个文件由 Electron 主进程直接执行，
 * 中间没有编译步骤。用 JSDoc 保留类型（tsc 会检查），避免构建链。
 */

"use strict";

const { readFile } = require("node:fs/promises");
const path = require("node:path");

const { workDir } = require("./paths.cjs");

/** 与 packages/plugin/src/endpoint.ts 保持同构；改一处必须改两处（有测试兜底）。 */
const ENDPOINT_VERSION = 1;

/**
 * @typedef {object} Endpoint
 * @property {number} version
 * @property {string} url      宿主根地址，例如 http://127.0.0.1:64001
 * @property {string} token    本机回环令牌
 * @property {string} root     工作区根目录
 * @property {string} workspace
 * @property {number} pid      写这个文件时的宿主进程号（用来判断僵死）
 * @property {number} at
 */

/**
 * @typedef {"offline" | "connecting" | "online"} ConnState
 * @typedef {{ id: string, title: string, status: string, kind: string, parentId: string | null, children: string[] }} NodeView
 * @typedef {{ title: string, pointer: string | null, roots: string[], nodes: Record<string, NodeView> }} DocView
 * @typedef {{ seq: number, doc: DocView | null }} HostSnapshot
 * @typedef {(payload: { state: ConnState, snapshot: HostSnapshot | null }) => void} Listener
 */

function endpointFile() {
  return path.join(workDir(), "endpoint.json");
}

/**
 * 读发现文件。文件不存在/格式不对/版本不符都返回 null —— 调用方按"DSH 未就绪"处理。
 * @param {string} [file]
 * @returns {Promise<Endpoint | null>}
 */
async function readEndpoint(file = endpointFile()) {
  try {
    const parsed = JSON.parse(await readFile(file, "utf8"));
    if (parsed.version !== ENDPOINT_VERSION) return null;
    if (typeof parsed.url !== "string" || typeof parsed.token !== "string") return null;
    return parsed;
  } catch {
    return null;
  }
}

/**
 * 长连接订阅器。
 *
 * 为什么把重连放在这里而不是交给 EventSource：Node 端没有 EventSource，
 * 我们用 fetch 的流式响应手写 SSE 解析，顺带就能拿到"响应中断"这个信号，
 * 比浏览器的 error 事件更明确。
 */
class HostLink {
  /** @type {ConnState} */
  #state = "offline";
  /** @type {HostSnapshot | null} */
  #snapshot = null;
  /** @type {Listener} */
  #listener;
  /** @type {AbortController | null} */
  #abort = null;
  /** @type {NodeJS.Timeout | null} */
  #timer = null;
  /** @type {(msg: string) => void} */
  #log;
  #stopped = false;
  /** 重连退避档位：1s → 2s → 5s → 10s 封顶，成功后归零。 */
  #backoff = 0;

  /** @param {Listener} listener */
  /** @param {Listener} listener @param {(msg: string) => void} [onLog] */
  constructor(listener, onLog) {
    this.#listener = listener;
    this.#log = onLog ?? (() => {});
  }

  start() {
    this.#stopped = false;
    void this.#connect();
  }

  stop() {
    this.#stopped = true;
    this.#abort?.abort();
    if (this.#timer) clearTimeout(this.#timer);
  }

  #emit() {
    this.#listener({ state: this.#state, snapshot: this.#snapshot });
  }

  async #connect() {
    if (this.#stopped) return;
    this.#state = "connecting";
    this.#emit();

    const endpoint = await readEndpoint();
    if (!endpoint) {
      this.#log('未找到 endpoint.json（DSH 未启动或插件未加载）');
      this.#scheduleRetry();
      return;
    }

    const controller = new AbortController();
    this.#abort = controller;

    try {
      this.#log(`连接 ${endpoint.url}/dsh-work/api/events`);
      const res = await fetch(`${endpoint.url}/dsh-work/api/events`, {
        headers: { "x-dsh-work-token": endpoint.token, accept: "text/event-stream" },
        signal: controller.signal,
      });
      if (!res.ok || !res.body) throw new Error(`HTTP ${res.status}`);

      this.#log("已连接，开始接收状态流");
      this.#state = "online";
      this.#backoff = 0;
      this.#emit();
      await this.#consume(res.body);
      this.#log("状态流结束（对端关闭）");
    } catch (err) {
      // 连接失败/断开：走重连，不当成致命错误
      if (!this.#stopped) this.#log(`连接中断：${err && err.message ? err.message : String(err)}`);
    } finally {
      if (this.#abort === controller) this.#abort = null;
    }

    if (!this.#stopped) {
      this.#state = "offline";
      this.#emit();
      this.#scheduleRetry();
    }
  }

  #scheduleRetry() {
    const ladder = [1_000, 2_000, 5_000, 10_000];
    const delay = ladder[Math.min(this.#backoff, ladder.length - 1)];
    this.#backoff += 1;
    this.#timer = setTimeout(() => void this.#connect(), delay);
  }

  /**
   * 逐行读 SSE：`event: <name>` / `data: <json>` / 空行分隔。
   * @param {ReadableStream<Uint8Array>} body
   */
  async #consume(body) {
    const decoder = new TextDecoder();
    const reader = body.getReader();
    let buffer = "";
    let event = "message";

    for (;;) {
      const { value, done } = await reader.read();
      if (done) return;
      buffer += decoder.decode(value, { stream: true });

      let idx;
      while ((idx = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, idx).replace(/\r$/, "");
        buffer = buffer.slice(idx + 1);

        if (line === "") {
          event = "message";
          continue;
        }
        if (line.startsWith("event:")) {
          event = line.slice(6).trim();
          continue;
        }
        if (!line.startsWith("data:")) continue;

        const data = line.slice(5).trim();
        if (event !== "snapshot" && event !== "patch") continue;
        try {
          const parsed = JSON.parse(data);
          this.#snapshot = { seq: parsed.seq, doc: parsed.doc ?? this.#snapshot?.doc ?? null };
          this.#emit();
        } catch {
          // 坏帧忽略：SSE 是尽力而为的推送，状态最终仍以重连时的快照为准
        }
      }
    }
  }
}

module.exports = { HostLink, readEndpoint, endpointFile };
