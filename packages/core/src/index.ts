/**
 * @dshwork/core —— 三端共用的纯逻辑层。
 *
 * 对外只暴露这一组符号；任何端都不允许绕过这里直接 import 内部文件
 * （测试除外）。这样将来要拆分/重构内部实现时，只有这一个文件会动。
 */

export * from './types.ts';
export {
  emptyDoc,
  linearOrder,
  isOpen,
  progress,
  validateDoc,
  apply,
  applyMany,
} from './state.ts';
export type { ApplyResult } from './state.ts';
export { diffDoc, applyPatch } from './patch.ts';
export type { PatchOp, PatchResult } from './patch.ts';
export { encodeEntry, parseJournal, replay, isJournalEntry } from './journal.ts';
export type { JournalEntry, ParseResult, ReplayResult } from './journal.ts';
export {
  PROTOCOL_VERSION,
  MessageTypes,
  ErrorCodes,
  buildEnvelope,
  encodeEnvelope,
  decodeEnvelope,
  isKnownMessageType,
} from './protocol.ts';
export type {
  Envelope,
  MessageType,
  HelloPayload,
  WelcomePayload,
  WorkConfig,
  SnapshotPayload,
  IntentPayload,
  IntentAckPayload,
  CaptureRequestPayload,
  CaptureResultPayload,
  NoticePayload,
  ErrorPayload,
  PingPayload,
  DecodeResult,
  BuildOptions,
  PlainPlatform,
} from './protocol.ts';
export { materializePlan } from './plan.ts';
export type { Materialized } from './plan.ts';
export { createId, createSequentialIdFactory } from './ids.ts';
