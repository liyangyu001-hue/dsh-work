# 项目说明

> **这份文档是什么**：DSH Work 的**总说明**——它是什么、为什么这样分层、代码怎么组织、按什么规则推进、
> 每个人（用户、AI）各自负责什么、踩过哪些坑。
>
> 它不像 README 那样面向"想用的人"，而是面向"要改它的人"和"要跟它一起工作的人"。
>
> **放什么**：不会每轮都变的东西——角色、规则、结构、决策、教训。
> **不放在这里**：会变的进度（→ [docs/NEXT.md](NEXT.md)）、变更叙事（→ [docs/CHANGELOG.md](CHANGELOG.md)）。
> 别在三个地方各写一份然后互相打架。
>
> **怎么读**：第一次，从头读到尾；要动手，直接跳 [§7 怎么参与开发](#7-怎么参与开发)。

---

## 1. 一分钟了解它是什么

**DSH Work** 是一个 DSH 插件。装上之后，屏幕顶部多一条半透明小条——**它代表"现在这个人在做什么"**。

人和 AI 看同一个任务进程：AI 把任务拆成树、指出当前该做哪一个、在节点上给建议；人负责执行，做完按一下，进度往前走。所有状态落在工作区的 `.dsh-work/` 里，关掉 DSH 也不丢。

它不是"任务管理软件"，也不是"又一个 AI 聊天框"。它押的是一个具体的主张：

> **人与 AI 协作时，缺的不是更强的模型，而是"同一份进度"。**
> 软件没有心跳，进程停下来就死了；但 AI 在任务进行期间可以一直醒着——所以它应该像一种 **人类可用的 skill**，而不是一个要你打开关闭的软件。

一句话记住产品形态：**小条是收起态，面板是展开态，框选是"把屏幕上这块拿给它看"。**

---

## 2. 角色与分工

用户给的原话是"你需要作为优秀的架构师"。落到实际工作里，是四个身份叠在一起，**边界很清楚**：

| 角色 | 我做什么 | 我不做什么 |
|---|---|---|
| **架构师** | 分层、协议、目录结构、技术选型、迁移路径、风险台账。所有"以后会不会变成屎山"的决定在这里定 | 不替用户做产品取舍 |
| **实现者** | 写全部代码、写测试、跑真机验证、修 bug | 不把没验证过的东西说成能用 |
| **守门人** | 测试/类型检查/隐私守卫；提交前自查；发现方向错了先说 | 不为了"看起来完成"而绕过检查 |
| **记录者** | 文档、提交信息、每轮向用户解释"改了什么、为什么、推了什么" | 不写"TODO: 待完善"这种没有信息的行 |

### 分工的硬边界（很重要）

- **产品取舍归用户**：做成什么样、先做什么、要不要为某个体验付出复杂度——这些由用户拍板。我提供选项、代价、我的推荐，然后停手等答复。
- **工程实现归我**：怎么分层、用什么库、目录长什么样、协议怎么定——这些我把理由说清楚就执行，不问"您觉得用 A 还是 B"，除非它牵扯到用户的成本（例如"要装 C++ 工具链吗"）。
- **不确定就问，问到胸有成竹再动手**。这是用户第一条硬要求，原话是"你有完整想法但我没说到的地方，请直接问我，直到胸有成竹，再开工"。宁可多问一轮，不要写错方向再返工。
- **我看不见用户的屏幕，也看不见用户的脸**。任何"现在效果怎么样"的判断，必须靠截图、日志、返回值这些硬证据，不能靠推理。

### 我做错过、已经改过的地方（留着当教训）

1. **把整屏桌面截图推到了公开仓库**。三张 1600×900 的截图里带着浏览器标签、控制台地址、文件管理器路径、桌面图标。用户直接指出"截图别把我桌面隐私东西推到仓库"。现在 `docs/assets/` 只允许放 `scripts/make-shots.mjs` 生成的合成图，`pnpm test` 里有白名单守卫拦着。
2. **一开始误解了 lolPing 的输入模型**。我以为它靠全局热键弹面板，实际上它靠**原生钩子在输入层决定"这个键吃不吃"**——面板自己不拿焦点。这个误解导致第一版面板抢焦点、会打断用户打字。读源码之后重做了。
3. **把用户说的"loop 掉了"理解成"循环进程掉了"**。用户其实说的是"任务本身丢了"。教训：用户描述故障时，先确认是**哪个对象**出了问题，再动手。

---

## 3. 工作的规则

这些规则不是装饰，每一条都是踩过坑之后写下来的。

### 3.1 用户定的规则

1. **开源**，仓库 [liyangyu001-hue/dsh-work](https://github.com/liyangyu001-hue/dsh-work)，MIT 协议，README 要详细、排版要好看。
2. **每次改动，我要自己介绍"改了什么、为什么、推了什么"**——用户不常做这类事，不能让他去翻 commit。
3. **美术**：玻璃质感、iOS 风格、理性克制的动效，遵循动画十二原则的动势与弹性。视觉体系冻结在 `docs/DESIGN-SYSTEM.md`。
4. **有参考找参考，但不生搬硬套**。看到好项目就去看源码里的机制，搬的是原理不是代码。
5. **持久开发，不是一次性任务**。文件和注释要精准，"否则后续变成屎山，我不会开发，只能放弃这个项目"。

### 3.2 工程纪律（我自我要求，也约束后来者）

1. **先取证，再动手**。平台行为（钩子能不能注册、路由是否同源、窗口抢不抢焦点）一律实测，不靠"应该是"。
2. **小步提交，一步一个能验收的事**。提交信息中文，"改了什么 + 为什么"，一行标题 + 空行 + 要点。
3. **测试与类型检查绿才允许提交**：`pnpm test`（73 用例）、`pnpm typecheck`。
4. **不绕过状态机**。任何状态变化都走 `apply(intent)` 这一个入口，没人有权直接改文档对象。
5. **协议冻结**。`docs/PROTOCOL.md` v1 一旦发布，只增不改；要改必须新开版本并写 ADR。
6. **依赖只能向下**：`core` 不认识宿主、不认识窗口、不认识网络；谁往上 import 了，就是结构烂的第一天。
7. **不造轮子，但也不乱引库**。能用系统自带的（WinHTTP、Direct2D、`node:test`）就不用第三方；Windows 上的 C++ 包管理是最容易腐烂的一环。
8. **不新建"临时验证脚本"留在地上**。验证完删掉，或者写进 `scripts/` 并说明用途。
9. **公开仓库里不出现任何个人环境的截图、路径、账号**。
10. **每轮结束改接力板**：`NEXT.md` 的进度、`CHANGELOG.md` 一行。

### 3.3 提交与推送

```powershell
# 测试与类型检查
pnpm test ; pnpm typecheck

# 推送（没有 gh CLI，用 PAT；token 存在仓库外的 %USERPROFILE%\.dsh\dsh-work\gh-token）
$token = (Get-Content "$env:USERPROFILE\.dsh\dsh-work\gh-token" -Raw).Trim()
$b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("x-access-token:$token"))
git -c http.extraHeader="AUTHORIZATION: basic $b64" push https://github.com/liyangyu001-hue/dsh-work.git HEAD:main
```

- **只允许 fast-forward，绝不 force push**。唯一例外：清理误提交的隐私内容，且必须在提交信息里写清原因。
- 提交信息用文件传入（PowerShell 的 heredoc 不可靠）：`git commit -F .commit-msg.tmp`，**提交完立刻删掉这个临时文件**（曾经把它一起提交进去过）。
- 本机 git 走 `127.0.0.1:7897` 代理。**Clash 没开会推不上去**，这时的正确做法是告诉用户"网络不通，不是代码问题"，而不是改配置硬试。

---

## 4. 结构

```
dsh-work/
├─ packages/
│  ├─ core/            纯函数内核：类型、状态机、指针推进、日志重放、协议编解码
│  └─ plugin/          DSH 插件本体（两半）
│     ├─ src/index.ts    host 半边：cordis 插件、注册工具/路由、管存储
│     ├─ src/client/     client 半边：跑在 DSH 网页里的顶部玻璃条（shell.overlay）
│     ├─ src/store.ts    快照 + 追加日志，串行化提交
│     ├─ src/http.ts     /dsh-work/api/* 四个端点，可脱离宿主单测
│     ├─ src/tools.ts    交给模型的四个工具：work_status / work_plan / work_advise / work_handoff_submit
│     └─ src/recognize.ts 截图 → 视觉模型 → 任务树（纯函数解析 + 一次就地重试）
├─ apps/companion/     伴随层（Electron 版，保留可回退）：小条、面板、框选覆盖层、热键
├─ native/             伴随层（C++ 版，正在替代 Electron）
│  ├─ shell/           原生小条：分层窗口 + Direct2D/DirectWrite + WinHTTP 轮询
│  └─ input-helper/    低级键盘钩子
├─ docs/               ARCHITECTURE / PROTOCOL / DATA-MODEL / DESIGN-SYSTEM / ROADMAP / NEXT / CHANGELOG / ADR
└─ scripts/            构建与截图工具（无构建系统，esbuild + cl 直出）
```

### 依赖方向（铁律，画出来是为了别违反）

```
        apps/companion · native/shell          （看得见人的那一层）
                    ↓ HTTP / SSE
        packages/plugin  host 半边             （状态权威、AI 编排）
                    ↓
        packages/core                          （纯函数，谁都不认识）
```

`core` 不 import 任何上层，也不 import `electron` / `cordis` / `node:fs`。它的测试是纯 Node 直接跑的。

---

## 5. 三层，为什么必须是三层

这是整个项目最重要的一次判断，写成了 [ADR 0001](ADR/0001-三进程与协议边界.md)。

| 层 | 跑在哪 | 能不能换 | 职责 |
|---|---|---|---|
| 宿主插件 | DSH 的 Node 子进程 | 不能（DSH 只认这个） | 状态的唯一权威、AI 编排、持久化、对外 HTTP/SSE |
| 页面内半边 | DSH 网页 | 不能（宿主只给这一个槽位） | 窗口内的小条与工作台 |
| 伴随层 | 独立进程 | **能，已经在从 Electron 换到 C++** | 桌面置顶小条、面板、框选、热键、托盘 |

**为什么必须独立进程**：全局热键、屏幕截取、置顶窗口、托盘，这四件事普通插件一件都做不到。取证结论是
`require('electron')` 在插件里抛 `Cannot find module`，宿主只允许三类 IPC，且全库没有 `desktopCapturer`、没有 `globalShortcut`。

**为什么伴随即便换语言，协议不能动**：协议是这三层之间唯一的缝。缝定死了，哪一层都能独立重写——
这次把 UI 从 Electron 换成 C++，`PROTOCOL.md` 一行没改，就是当初留这条缝的回报。

---

## 6. 关键决定（为什么是现在这样）

完整的在 `docs/ADR/`，这里只列"最容易被人重新质疑一遍"的几条：

- **事件日志是真相源**（ADR 0003）：`work.json` 只是快照，`journal.ndjson` 才是真相。好处是崩溃可恢复、可回放、可审计；代价是每次提交多一次 append。
- **状态机是纯函数**：`apply(doc, intent) → {doc, error}`，不改入参、不抛异常、未知名一律拒绝。这让"AI 说要做 X"和"人按了回车"走完全同一条路径，谁都没特权。
- **伴随层用 Electron → 改用 C++**：Electron 版 210 MB / 1.5 s 冷启动 / 4 个进程；C++ 版 318 KB / <100 ms / 单进程，而且能拿到真正的逐像素圆角和亚像素中文排版。代价是改样式要重编译，没有热重载。
- **面板不抢焦点**：抢焦点就会打断用户正在打的字。现在走"打开期间独占按键、关闭立刻释放"（据 lolPing 的机制；实测 `globalShortcut` 在 Windows 上对 Enter/Backspace/Tab/Esc/方向键 6/6 注册成功）。
- **截图识别走视觉模型而非本地 OCR**：一次调用同时完成"认字 + 理解这是什么任务"，少一个会飘的中间层。失败就地重问一次，仍失败就不写文档。
- **轮询而不是 SSE（原生层）**：小条只要几百字节，1 秒延迟感知不到；流式要自己处理分块、断线、半行，风险收益不成比例。

---

## 7. 怎么参与开发

### 动手之前

```powershell
cd D:\work\AI\work\dsh-work
git log --oneline -5
Get-Content docs/NEXT.md | Select-Object -First 40     # 当前任务与硬约束
```

**接力板是唯一权威的"下一步"**。`NEXT.md` 里写着当前任务、验收标准、禁止事项；本文不重复它们。

### 把东西跑起来

```powershell
# 1. 开发用宿主（独立 profile，不碰用户桌面那个）
#    起之前先按端口杀旧进程，否则 dsh 会静默起不来
Get-NetTCPConnection -LocalPort 64001 -State Listen -ErrorAction SilentlyContinue |
  ForEach-Object { Stop-Process -Id $_.OwningProcess -Force }
& "C:\Users\25108\AppData\Local\Programs\DeepSeek Harness\resources\runtime\cli\bin\dsh.cmd" `
  --profile dshwork-dev --port 64001 --no-open

# 2. 插件要真机生效必须先 build
pnpm build

# 3. 原生小条
pnpm build:native
Start-Process "native\shell\build\dshwork-shell.exe"
Stop-Process -Name dshwork-shell -Force      # 它点击穿透，没有 UI 出口

# 4. 看状态（token 在 %USERPROFILE%\.dsh\dsh-work\endpoint.json，每次宿主重启都会换）
```

### 验证一律用硬证据

- 页面类改动：`scripts/shot.mjs`（Playwright + 本机 Chrome）出图看，别靠"应该渲染了"。
- 原生类改动：PowerShell `CopyFromScreen` 截屏，**必须 DPI-aware**（`SetProcessDpiAwarenessContext(-4)`），否则 150% 缩放下坐标全错。
- 状态类改动：直接打 `/dsh-work/api/state` 看 `seq`、`pointer`、节点状态，比看界面可靠。

### 收工前

1. `pnpm test` / `pnpm typecheck` 绿 → `pnpm build`（改过 client 或 companion 必做）。
2. 更新 `docs/NEXT.md`（进度 + 下一个任务 + 新踩的坑）、`docs/CHANGELOG.md`（一行）。
3. 提交并推送；然后**用自己的话告诉用户改了什么、为什么、推了什么**。

---

## 8. 红线

- 不把用户桌面、账号、路径、对话内容推到公开仓库。
- 不改用户正在用的 desktop profile；开发用 `dshwork-dev`。
- 不跳过测试与类型检查提交。
- 不 force push（隐私清理例外，且要写明）。
- 不在没验证的情况下说"跑通了"。
- 不动已经冻结的 `PROTOCOL.md` 语义与 `DESIGN-SYSTEM.md` 的视觉体系，除非用户明确要求。
- 不替用户做产品取舍；做完一件就停，把选择权交回去。

---

## 9. 现在到哪了

时间线（提交号可查）：

| 阶段 | 内容 | 提交 |
|---|---|---|
| P0 | 三端骨架：状态机 + 持久化 + 窗口内玻璃条 + 桌面置顶小条 | `45f0597` `5956f21` `84ff6db` |
| P1-1 | 全局热键可配置（默认 `Control+Alt+Space`；裸 Tab 需显式开启） | `4870b3f` |
| P1-2 | 热键 → 冻结整屏 → 框选 → 区域 PNG 落盘 | `571004d` |
| P1-2b | 面板：卡片带 + 详情列 + 键盘操作；语义改为"小条是收起态、面板是展开态" | `4a0877e` |
| P1-3 | 截图 → 视觉模型 → 任务树（解析失败就地重试一次） | `7e53a78` |
| 隐私 | 移除桌面截图，改合成图，加守卫 | `f7bf0b6` |
| P1-2c | 面板不抢焦点；键盘改为打开期间独占 | `cf77de4` |
| 原生切片一 | C++ 小条替代 Electron：318 KB，DirectWrite 中文，1 s 轮询 | `b381c5a` |

测试 73 用例全绿，源码约 6600 行。

**下一步**：原生切片二（面板）——见 `NEXT.md`。

---

## 10. 还欠着的（别装作没有）

- **P1-3 的真机端到端没补验**：跑着的宿主加载的是旧 bundle，`/api/recognize` 当时返回 404，重启后才算数。
- **原生层没有托盘、没有退出入口**，只能 `Stop-Process`。
- **原生层只有 Windows**。macOS 要另写（`NSWindow` + Core Animation），三片迁移范围内不包含。
- **`remote` / 多会话**：现在一个宿主只服务一个小条，多窗口、多工作区没有设计。
- **AI 接手执行（P2-2）完全没做**：现在节点可以标记"交给你"，但没有真正的执行闭环。
- **学习模式（用户构想里的第 5 条）没做**：`kind:'concept'` 只是打了个标记。
- **仓库尚未删除重建**：旧历史里还躺着他个人桌面截图，等用户在网页上删库后重建。

---

## 11. 一句话原则

> 这个项目真正难的不是写代码，是**守住"人和 AI 共享同一份进度"这一个主张**——
> 所有让它更像"又一个工具"的改动，都值得先停下来问一句：这还算协作吗？
