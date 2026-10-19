/**
 * 伴随层的发现文件。
 *
 * 为什么需要它：DSH 的端口是宿主用 `--port 0` 让系统分配的，每次启动都可能变；
 * 而伴随层（Electron 小条）是**另一条进程**，它必须知道「这次 DSH 在哪个端口、用哪个令牌」。
 * 约定一个固定路径的 JSON，是这两条进程之间最简单、最不脆弱的握手机制。
 *
 * 路径：<DSH_HOME>/dsh-work/endpoint.json   （DSH_HOME 默认 ~/.dsh）
 * 内容：{ version, url, token, root, workspace, pid, at }
 *
 * 安全边界：令牌只用于让伴随层通过本插件的路由鉴权，等价于「本机同一用户的信任」。
 * 文件权限按 0600 创建；不要把这个文件同步到云盘或提交进仓库。
 */

import { randomBytes } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

export const ENDPOINT_SCHEMA_VERSION = 1;

export interface Endpoint {
  version: number;
  /** 例：http://127.0.0.1:63948 —— 不带尾斜杠。 */
  url: string;
  token: string;
  /** 工作区根目录（.dsh-work 的父目录）。 */
  root: string;
  workspace: string;
  pid: number;
  at: number;
}

/** DSH 数据根目录。优先用宿主给的环境变量，保证与宿主同源。 */
export function dshHome(): string {
  const fromEnv = process.env['DSH_HOME'];
  if (typeof fromEnv === 'string' && fromEnv.trim() !== '') return fromEnv.trim();
  return path.join(os.homedir(), '.dsh');
}

export function endpointDir(): string {
  return path.join(dshHome(), 'dsh-work');
}

export function endpointFile(): string {
  return path.join(endpointDir(), 'endpoint.json');
}

/**
 * 读取或创建本机共享令牌。已存在就复用 —— 伴随层在 DSH 重启后不该被迫重新配对。
 */
export async function ensureToken(): Promise<string> {
  const dir = endpointDir();
  await mkdir(dir, { recursive: true });
  const file = path.join(dir, 'token');
  try {
    const existing = (await readFile(file, 'utf8')).trim();
    if (existing.length >= 32) return existing;
  } catch {
    // 不存在或不可读：走下面的创建分支
  }
  const token = randomBytes(24).toString('base64url');
  await writeFile(file, `${token}\n`, { encoding: 'utf8', mode: 0o600 });
  return token;
}

/** 写发现文件。宿主每次启动（端口可能变）都要刷新一次。 */
export async function publishEndpoint(input: {
  port: number | null;
  root: string;
  workspace: string;
}): Promise<Endpoint> {
  const token = await ensureToken();
  const host = '127.0.0.1';
  const url = input.port ? `http://${host}:${input.port}` : `http://${host}`;
  const endpoint: Endpoint = {
    version: ENDPOINT_SCHEMA_VERSION,
    url,
    token,
    root: input.root,
    workspace: input.workspace,
    pid: process.pid,
    at: Date.now(),
  };
  await mkdir(endpointDir(), { recursive: true });
  await writeFile(endpointFile(), `${JSON.stringify(endpoint, null, 2)}\n`, {
    encoding: 'utf8',
    mode: 0o600,
  });
  return endpoint;
}

/** 伴随层侧的同构读取（测试与自检用；伴随层自己实现一份也一样）。 */
export async function readEndpoint(file = endpointFile()): Promise<Endpoint | null> {
  try {
    const parsed = JSON.parse(await readFile(file, 'utf8')) as Partial<Endpoint>;
    if (parsed.version !== ENDPOINT_SCHEMA_VERSION) return null;
    if (typeof parsed.url !== 'string' || typeof parsed.token !== 'string') return null;
    return parsed as Endpoint;
  } catch {
    return null;
  }
}
