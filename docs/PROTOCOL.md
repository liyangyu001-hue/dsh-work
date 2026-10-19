# 协议 v1（冻结）

> 本文件是三端之间**唯一**的契约。任何一端可以重写，只要它遵守这里。
> 修改规则见 §8：小改动只允许「增加」，不允许「改变含义」。

---

## 1. 传输

| 通道 | 用途 | 地址 |
|---|---|---|
| WebSocket | 主通道：状态、意图、采集、提示 | `ws://127.0.0.1:<hostPort>/dsh-work/channel` |
| HTTP GET | 静态资源：截图、缩略图 | `/dsh-work/asset/<assetId>` |
| HTTP GET | 健康检查 | `/dsh-work/health` |
| HTTP POST | 提交识别作业（一张 PNG） | `/dsh-work/api/recognize` |
| HTTP GET | 查询识别作业 | `/dsh-work/api/recognize/<jobId>` |

- `hostPort` 由 DSH 宿主分配（`ctx.webServer.port`），**不得硬编码**。
- 连接方向：**Companion 主动反连** Host。Host 不主动拨号，避免防火墙与端口发现问题。
- 鉴权：查询参数 `token`，由 Host 在启动时生成一次性令牌，写入伴随层的启动参数；令牌不过期但随 Host 进程生命周期失效。

## 2. 信封

所有报文共用一个信封：

```jsonc
{
  "v": 1,                 // 协议主版本
  "id": "a3f1…",          // 报文唯一 id（UUIDv7）
  "ts": 1760000000000,    // 发送方时钟（毫秒），仅用于诊断，不参与逻辑
  "type": "state.patch",  // 类型，见 §4
  "corr": "a3e0…",        // 可选：请求-响应关联 id
  "payload": { }          // 类型专属负载
}
```

**兼容规则**（三端必须全部实现）：

1. 收到未知 `type` → 记录一条 WARN，**忽略**，不得断开连接。
2. 收到未知 `payload` 字段 → **忽略**，不得报错。
3. 收到 `v` 大于自身支持的主版本 → 回 `error {code:"version/incompatible"}` 并断开。

## 3. 握手

```
Companion                               Host
    │ ── hello ─────────────────────────► │
    │    {clientVersion, platform,         │
    │     capabilities[], snapshotSeq}     │
    │ ◄───────────────────────── welcome ─ │
    │    {serverVersion, stateSeq, config} │
    │ ◄───────────────────────── snapshot ─│   （仅当 snapshotSeq < stateSeq）
    │ ◄────────────── state.patch 流 ──────│
    │ ── ping ────────（每 20s）─────────► │
    │ ◄──────────────────────────── pong ─ │
```

- `capabilities` 由 Companion 自报（`["capture","hotkey","tray","alwaysOnTop"]`）。Host 据此决定是否下发 `capture.request`——**不要在一方不具备能力时发送该类消息**。
- 心跳：Companion 每 20s 发 `ping`；Host 45s 未收到视为断线，标记伴随层离线。Companion 断线后按 1s/2s/5s/10s/30s 退避重连，封顶 30s。

## 4. 消息目录

### 4.1 生命期

| type | 方向 | payload |
|---|---|---|
| `hello` | C→H | `{clientVersion, platform:"windows"\|"macos"\|"linux", capabilities:string[], snapshotSeq:number}` |
| `welcome` | H→C | `{serverVersion, stateSeq:number, config:WorkConfig}` |
| `ping` / `pong` | 双向 | `{seq:number}` |
| `bye` | 双向 | `{reason:string}` |
| `error` | 双向 | `{code:string, message:string}` |

### 4.2 状态同步

| type | 方向 | payload |
|---|---|---|
| `snapshot` | H→C | `{seq:number, doc:WorkDoc}` |
| `state.patch` | H→C | `{seq:number, ops:PatchOp[]}` |

`PatchOp` 为最小变更描述，View 端据此增量更新；若 `seq` 不连续（跳号），必须**丢弃本地状态并请求 snapshot**，不得自行猜测。

### 4.3 意图

| type | 方向 | payload |
|---|---|---|
| `intent` | C→H | `{intent:Intent}` |
| `intent.ack` | H→C | `{corr, seq, applied:boolean, reason?:string}` |

`Intent` 的完整列表见 [DATA-MODEL.md §4](DATA-MODEL.md)。Host 是唯一裁判：

- 结构非法 → `intent.ack {applied:false, reason:"intent/invalid"}`，**不落盘**。
- 业务非法（如节点不存在、状态迁移不合法）→ `applied:false, reason:"intent/rejected"`，落一条 `rejected` 诊断日志但**不改状态**。
- 幂等：Host 保留最近 256 个已处理 `id`，重复收到直接回放上次 ack，不重复应用。

### 4.4 采集

| type | 方向 | payload |
|---|---|---|
| `capture.request` | H→C | `{requestId, mode:"full"\|"region"\|"window"}` |
| `capture.result` | C→H | `{requestId, assetId, width, height, display:{id, scale}}` |
| `capture.cancel` | 双向 | `{requestId, reason}` |
| `capture.failed` | C→H | `{requestId, reason:string}` |

截图**不走 WS 二进制**：Companion 先把 PNG POST 到 `/dsh-work/asset/`，再在 `capture.result` 里回 `assetId`。这样 WS 只承载小而快的控制面，大对象走 HTTP 便于缓存与重试。

### 4.5 呈现

| type | 方向 | payload |
|---|---|---|
| `overlay.show` | H→C | `{mode:"bar"\|"select"\|"ask", payload:unknown}` |
| `overlay.hide` | H→C | `{mode}` |
| `notice` | H→C | `{level:"info"\|"warn"\|"error", text, ttlMs?}` |

### 4.6 识别（P1-3，HTTP 侧，不进信封）

识别是**异步作业**：模型调用要几十秒，长响应会被任何一层超时掐断，
因此请求只受理、结果另取。两条路由都要求伴随层令牌。

```
POST /dsh-work/api/recognize          Content-Type: image/png，body 为 PNG 原始字节
  202 {ok:true, jobId:"rc_…"}
  409 {ok:false, error:"上一张截图还在识别中"}     同一时刻只允许一个作业
  413 / 400 / 503                                 过大 / 不是 PNG / 宿主没有识别能力

GET  /dsh-work/api/recognize/<jobId>
  200 {ok:true, state:"running"}
  200 {ok:true, state:"done", title:"…", count:12}
  200 {ok:true, state:"error", reason:"可读原因"}
  404 {ok:false, error:"没有这个识别作业（可能已过期）"}
```

- 成功的判定标准是**状态文档真的变了**：宿主先解析模型输出，再走 `plan.set`
  的常规 `apply` 通道落库（`by:"ai"`），失败时文档一个字节都不改。
- 作业在内存里保留 5 分钟、最多 8 个（在跑的不回收）；过期后再问就是 404。
- 识别进度通过伴随层自己的 IPC 呈现（小条「识别中…」/「已拆成 N 个任务」），
  不属于协议层。

## 5. 错误码

| code | 含义 | 处理 |
|---|---|---|
| `auth/denied` | 令牌无效 | 断开，提示重装伴随层 |
| `version/incompatible` | 主版本不兼容 | 断开，提示升级 |
| `intent/invalid` | 结构非法 | 回 ack，不落盘 |
| `intent/rejected` | 业务规则拒绝 | 回 ack，落诊断日志 |
| `capture/failed` | 截图失败（权限/显示器） | 回 `capture.failed`，界面提示 |
| `internal` | 宿主内部错误 | 回 error，记 ERROR 日志 |

## 6. 时间与顺序

- 逻辑顺序唯一由 `seq`（Host 侧单调整数）定义，**不得**用 `ts` 排序。
- `ts` 只用于诊断与日志。三端时钟可能漂移，任何依赖 `ts` 做业务判断的代码都是 bug。

## 7. 安全

- 只监听回环地址。
- 令牌只出现在启动参数与 WS 查询串，不写日志（日志中一律打码）。
- 截图可能含敏感信息：默认落在工作区 `.dsh-work/captures/`，默认 7 天自动清理，可在设置中关闭留存。

## 8. 版本策略

- **主版本**（`v`）：破坏性变更。新版本需同时支持 `vN` 与 `vN-1` 至少一个发布周期。
- **次版本**：只允许新增 `type` 与新增可选字段。接收方必须忽略未知。
- 任何对已有字段含义的修改，都视为破坏性变更——**宁可新开一个 type，也不要改老的**。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v1 | 2026-10 | 初版冻结：握手、状态同步、意图、采集、呈现 |
| v1.1 | 2026-10 | 新增 §4.6 识别作业（两条 HTTP 路由，仅新增，不改旧语义） |
