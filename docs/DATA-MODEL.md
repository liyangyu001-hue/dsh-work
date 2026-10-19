# 数据模型

> 目标只有一条：**人可读、git 可 diff、崩溃可回放、模型可手改**。
> 所以没有数据库，只有两个文件加两个目录。

---

## 1. 磁盘布局

工作区根目录下：

```
.dsh-work/
├─ work.json              # 当前快照（可整份删除，靠日志重建）
├─ journal.ndjson         # 只追加的事件日志（真相源）
├─ nodes/
│   └─ <nodeId>.md        # 每个节点的人读档案：目标 / 验收 / 建议 / 备注
├─ captures/
│   └─ <yyyymmdd>-<id>.png
└─ .gitkeep
```

- 全局索引（跨工作区）放在 `~/.dsh/work/index.json`，只记录「哪个工作区有哪些任务」，不含正文。
- `captures/`、`journal.ndjson` 默认进 `.gitignore`；`work.json` 与 `nodes/*.md` **建议入库**——任务树是资产，截图不是。

## 2. `work.json` —— 快照

```jsonc
{
  "schema": 1,
  "id": "wrk_01J…",
  "title": "把 DSH Work 做到能自己走路",
  "createdAt": 1760000000000,
  "updatedAt": 1760000123000,
  "seq": 42,                     // 快照对应的日志序号
  "pointer": "nd_01J…",          // 当前节点（可为 null）
  "roots": ["nd_01J…"],          // 根节点顺序
  "nodes": {
    "nd_01J…": {
      "id": "nd_01J…",
      "parentId": null,
      "children": ["nd_02…"],
      "title": "冻结三端协议",
      "detail": "把 WS 消息目录与意图列表定下来",
      "acceptance": "docs/PROTOCOL.md 合入且无 TODO",
      "kind": "task",            // task | concept
      "status": "active",        // todo | active | done | skipped
      "advice": null,            // 见 §3
      "handoff": null,           // 见 §3
      "evidence": [],            // [{assetId, note, at}]
      "createdAt": 1760000000000,
      "updatedAt": 1760000000000
    }
  }
}
```

### 字段约束

| 字段 | 约束 |
|---|---|
| `schema` | 整数。加载时若不等于当前版本且无迁移路径 → 拒绝启动并提示备份 |
| `seq` | 单调递增。与 `journal.ndjson` 最后一行对齐 |
| `pointer` | 必须是 `nodes` 里存在的 id，或 `null` |
| `roots` / `children` | 每个 id 必须存在，且**不能**同时出现在两处；禁止成环 |
| `status` | 只允许四值；`active` 全树最多一个 |
| `kind` | `task` = 要做的事；`concept` = 要理解的东西（学习模式入口） |

## 3. 三个挂件

### `advice` —— AI 建议 / 更优解

```jsonc
{ "text": "先跑通空壳再谈动效", "alt": "或者直接用 Electron 的 backgroundMaterial", "at": 176…, "model": "…" }
```

- 由 `advisor` 异步写入，**不阻塞**节点推进。
- 按 `title + detail` 的 hash 缓存，内容没变不重新请求。

### `handoff` —— 交给 AI

```jsonc
{ "state": "offered|running|review|accepted|returned", "agentSessionId": "…", "since": 176…, "note": "…" }
```

- 只允许两类迁移：`offered → running → review → (accepted|returned)`。
- `review` 态意味着**AI 说做完了但人还没确认**——此时节点的 `status` 仍是 `active`。**完成判定权永远在人不在此结构里**，这是产品原则，不是技术选择。

### `evidence` —— 证据

截图或笔记，`assetId` 指向 `captures/` 或外部引用。节点的「凭什么说这件事做完了」全靠它。

## 4. Intent 目录

`Intent` 是**唯一合法的状态变更描述**，纯数据、可序列化、可回放。

| Intent | 语义 | 副作用 |
|---|---|---|
| `plan.set {roots, nodes}` | 整体设定任务树（首次拆解 / 重拆） | 替换树；`pointer` 指向第一个 `todo` |
| `plan.add {parentId, node, index?}` | 插入节点 | 更新父子关系与顺序 |
| `plan.remove {nodeId}` | 删除子树 | 若删掉 `pointer`，指针退到父节点 |
| `plan.rename {nodeId, title?, detail?, acceptance?}` | 修改文字 | — |
| `node.advance` | 当前完成并前进 | `pointer` 节点 → `done`；指针移到下一个 `todo`（深度优先） |
| `node.back` | 回到上一个任务 | 当前节点保持 `active` 不变，指针退到上一个未完成节点 |
| `node.complete {nodeId}` | 标记某节点完成 | 该节点 → `done`；若正是指针则自动前进 |
| `node.skip {nodeId}` | 跳过 | → `skipped`；若正是指针则前进 |
| `node.focus {nodeId}` | 直接把指针挪到某节点 | 仅当该节点为 `todo`；原 `active` 保持（允许多线并行意识） |
| `node.advise {nodeId, advice}` | 写入建议 | — |
| `node.evidence {nodeId, evidence}` | 追加证据 | — |
| `handoff.offer {nodeId}` | 提议交给 AI | `handoff.state = offered` |
| `handoff.start {nodeId, agentSessionId}` | AI 开始执行 | → `running` |
| `handoff.submit {nodeId, note}` | AI 交回 | → `review` |
| `handoff.accept {nodeId}` | 人认可 | → `accepted`，且 `status = done` |
| `handoff.return {nodeId, reason}` | 人打回 | → `returned`，`status` 回到 `active` |

**指针前进规则**（`advance` 的核心，必须有单测覆盖）：

1. 深度优先：当前节点有未完成子节点 → 进入第一个；
2. 否则找同级下一个未完成兄弟；
3. 否则回溯到父级的下一个兄弟；
4. 全树无 `todo` → `pointer = null`，任务完成。

## 5. `journal.ndjson` —— 事件日志

每行一条 JSON，**只追加，永不改写**：

```jsonc
{"seq":41,"at":1760000123000,"by":"human","intent":{"type":"node.complete","nodeId":"nd_02…"}}
{"seq":42,"at":1760000124000,"by":"ai","intent":{"type":"node.advise", …}}
```

- `by`：`human | ai | system`，用于事后归因（「这棵树是谁拆的」）。
- 写入策略：`appendFile` 后不强制 fsync（崩溃最多丢最后一行，代价可接受）；每 50 条或每 30s 折叠一次快照。
- 校验：行尾必须是 `\n`；解析失败的行视为**损坏尾部**，截断并记警告，不影响前面的历史。

## 6. 恢复流程

```
启动
 ├─ 读 work.json
 │   ├─ 成功 → doc = snapshot, seq = doc.seq
 │   └─ 失败/不存在 → doc = 空文档, seq = 0
 ├─ 扫 journal.ndjson，取 seq > doc.seq 的条目，逐条 apply
 ├─ 重放完成 → doc 写入 work.json（折叠）
 └─ 打开 WS，广播 snapshot
```

**折叠（compaction）**：日志超过 2000 行时，把当前快照写入 `work.json`，随后把 `journal.ndjson` 重命名为 `journal.<seq>.ndjson` 归档，新日志从 `seq+1` 开始。

## 7. 迁移

`work.json` 的 `schema` 每升一级，`packages/core/src/migrate.ts` 里必须加一个纯函数 `migrate<N>(doc) → doc`。

规则：

- 迁移函数**只增不改**，从 v1 一路链式升到当前版本。
- 每次迁移前自动备份为 `work.v<N>.bak.json`。
- 迁移失败 → 拒绝启动，**绝不**用默认值覆盖用户数据。

## 8. 人读档案 `nodes/<nodeId>.md`

每个节点落一份 Markdown，格式固定（AI 与人都按这个格式读写）：

```markdown
---
id: nd_01J…
status: active
kind: task
---

# 冻结三端协议

## 目标
把 WS 消息目录与意图列表定下来。

## 验收
docs/PROTOCOL.md 合入且无 TODO。

## AI 建议
- 先跑通空壳，再谈动效。
- 备选：直接复用 Electron 的 backgroundMaterial。

## 记录
- 2026-10-19 21:40 人：截图识别出 6 个子任务。
```

它的作用是：**当人想绕过界面直接看/改任务时，有东西可看可改**。
