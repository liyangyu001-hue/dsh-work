/**
 * 协议 v1 的编解码与兼容性检查。
 *
 * 这里只做「信封」层的事：校验形状、判版本、给出可读的失败原因。
 * 具体的 payload 语义由各端自己解释——因为兼容规则要求
 * **未知字段与未知类型必须被忽略**，所以这里刻意不做严格校验。
 */

import type { Intent, WorkDoc } from './types.ts';

export const PROTOCOL_VERSION = 1;

export interface Envelope<P = unknown> {
  v: number;
  id: string;
  ts: number;
  type: string;
  corr?: string;
  payload: P;
}

/** 消息类型常量。用 as const 而非 enum，保证可以被类型擦除。 */
export const MessageTypes = {
  hello: 'hello',
  welcome: 'welcome',
  ping: 'ping',
  pong: 'pong',
  bye: 'bye',
  error: 'error',
  snapshot: 'snapshot',
  statePatch: 'state.patch',
  intent: 'intent',
  intentAck: 'intent.ack',
  captureRequest: 'capture.request',
  captureResult: 'capture.result',
  captureCancel: 'capture.cancel',
  captureFailed: 'capture.failed',
  overlayShow: 'overlay.show',
  overlayHide: 'overlay.hide',
  notice: 'notice',
} as const;

export type MessageType = (typeof MessageTypes)[keyof typeof MessageTypes];

const KNOWN_TYPES = new Set<string>(Object.values(MessageTypes));

export function isKnownMessageType(type: string): type is MessageType {
  return KNOWN_TYPES.has(type);
}

/* ─────────────────────────── payload ─────────────────────────── */

export type PlainPlatform = 'windows' | 'macos' | 'linux';

export interface HelloPayload {
  clientVersion: string;
  platform: PlainPlatform;
  capabilities: string[];
  /** 客户端手上的快照序号；落后于 Host 时 Host 会补一个 snapshot。 */
  snapshotSeq: number;
}

export interface WorkConfig {
  /** 全局热键，例如 `Ctrl+Alt+Space`。 */
  globalHotkey: string;
  /** 是否允许注册裸 Tab（危险：会吞掉全系统 Tab）。 */
  allowBareTab: boolean;
  /** 截图留存天数；0 表示不落盘。 */
  captureRetentionDays: number;
  /** 建议所用模型标识。 */
  advisorModel: string;
  /** 是否开机自启伴随层。 */
  autoLaunch: boolean;
}

export interface WelcomePayload {
  serverVersion: string;
  stateSeq: number;
  config: WorkConfig;
}

export interface SnapshotPayload {
  seq: number;
  doc: WorkDoc;
}

export interface IntentPayload {
  intent: Intent;
}

export interface IntentAckPayload {
  /** 原封不动回传请求的 `id`，用于关联。 */
  corr: string;
  seq: number;
  applied: boolean;
  reason?: string;
}

export interface CaptureRequestPayload {
  requestId: string;
  mode: 'full' | 'region' | 'window';
}

export interface CaptureResultPayload {
  requestId: string;
  assetId: string;
  width: number;
  height: number;
  display: { id: string; scale: number };
}

export interface NoticePayload {
  level: 'info' | 'warn' | 'error';
  text: string;
  ttlMs?: number;
}

export interface ErrorPayload {
  code: string;
  message: string;
}

export interface PingPayload {
  seq: number;
}

/* ─────────────────────────── 识别码 ─────────────────────────── */

export const ErrorCodes = {
  authDenied: 'auth/denied',
  versionIncompatible: 'version/incompatible',
  intentInvalid: 'intent/invalid',
  intentRejected: 'intent/rejected',
  captureFailed: 'capture/failed',
  internal: 'internal',
} as const;

/* ─────────────────────────── 编解码 ─────────────────────────── */

export interface BuildOptions {
  /** 报文 id。由调用方生成，保证 core 无随机性。 */
  id: string;
  ts: number;
  corr?: string;
}

export function buildEnvelope<P>(type: string, payload: P, opts: BuildOptions): Envelope<P> {
  const envelope: Envelope<P> = {
    v: PROTOCOL_VERSION,
    id: opts.id,
    ts: opts.ts,
    type,
    payload,
  };
  if (opts.corr !== undefined) envelope.corr = opts.corr;
  return envelope;
}

export function encodeEnvelope(envelope: Envelope): string {
  return JSON.stringify(envelope);
}

export type DecodeResult =
  | { ok: true; envelope: Envelope; known: boolean }
  | { ok: false; reason: string; code: string };

/**
 * 解码一条入站报文。
 *
 * 注意两种「失败」的区别：
 * - `ok:false` = 结构非法或版本不兼容 → 应当断开或回 error；
 * - `ok:true, known:false` = 未知类型 → 按协议必须**忽略**，不断开。
 */
export function decodeEnvelope(raw: string): DecodeResult {
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { ok: false, reason: '不是合法 JSON', code: ErrorCodes.intentInvalid };
  }

  if (typeof parsed !== 'object' || parsed === null) {
    return { ok: false, reason: '报文不是对象', code: ErrorCodes.intentInvalid };
  }

  const candidate = parsed as Record<string, unknown>;
  if (typeof candidate['v'] !== 'number') {
    return { ok: false, reason: '缺少协议版本 v', code: ErrorCodes.intentInvalid };
  }
  if (candidate['v'] > PROTOCOL_VERSION) {
    return {
      ok: false,
      reason: `对端协议版本 ${candidate['v']} 高于本端 ${PROTOCOL_VERSION}`,
      code: ErrorCodes.versionIncompatible,
    };
  }
  if (typeof candidate['id'] !== 'string' || candidate['id'] === '') {
    return { ok: false, reason: '缺少报文 id', code: ErrorCodes.intentInvalid };
  }
  if (typeof candidate['ts'] !== 'number') {
    return { ok: false, reason: '缺少 ts', code: ErrorCodes.intentInvalid };
  }
  if (typeof candidate['type'] !== 'string' || candidate['type'] === '') {
    return { ok: false, reason: '缺少 type', code: ErrorCodes.intentInvalid };
  }
  if (!('payload' in candidate)) {
    return { ok: false, reason: '缺少 payload', code: ErrorCodes.intentInvalid };
  }

  const envelope = candidate as unknown as Envelope;
  return { ok: true, envelope, known: isKnownMessageType(envelope.type) };
}
