# 设计系统

> 目标观感：**iOS 的克制 + 玻璃的通透 + 动效的物理感**。
> 三条底线：不炫技、不抢焦点、不打断人。

---

## 1. 设计 token 三层

```
原始层 primitive   色阶、间距、圆角、字号、时长      ─┐
语义层 semantic     surface / text / border / motion ─┼─► 组件层 component
组件层              具体组件的私有变量                 ─┘
```

- 单一来源：`packages/tokens/src/tokens.ts`（TS 常量）→ 构建时产出 `tokens.css`（CSS 变量）。
- **任何组件不许写颜色字面量**，只能引用变量。这条在 CI 里用 lint 卡住。
- 在 DSH 窗口内，语义层**优先映射到宿主 token**（`--dsw-*`），映射不到的一律用本插件 `--dswork-*` 前缀兜底；在伴随层里全部用 `--dswork-*`。这样两处观感一致，又不与宿主打架。

```css
:root {
  /* 明暗由 data-theme 驱动 */
  --dswork-surface-glass: light-dark(rgb(255 255 255 / .72), rgb(28 28 30 / .72));
  --dswork-surface-panel: light-dark(rgb(255 255 255 / .86), rgb(32 32 34 / .86));
  --dswork-hairline: rgba(0, 0, 0, .08);
}
```

## 2. 玻璃配方

**真的背景模糊**优先，CSS 兜底：

| 环境 | 做法 |
|---|---|
| Windows 11 + Electron | 窗口级 `backgroundMaterial: 'acrylic'` + 内容区 `background: transparent` |
| 其它 | `backdrop-filter: blur(24px) saturate(180%)` + 半透明填充 + 1px 内发丝高光 |

统一叠加（三条缺一不可，缺了就"塑料"）：

1. **填充**：半透明表面色，不透明度 0.72 左右；
2. **描边**：顶部 `inset 0 1px 0 rgba(255,255,255,.5)` 发丝高光，外框 `0.5px` 极淡描边；
3. **阴影**：`0 8px 32px rgba(0,0,0,.12), 0 1px 2px rgba(0,0,0,.06)` —— 短软影负责"浮起"，长淡影负责"存在"。

噪声纹理 `3%` 可选叠加，用于抑制大面积纯色带来的廉价感。

## 3. 几何

| 元素 | 规格 |
|---|---|
| 顶栏高度 | 44px（收起态）/ 自适应（展开任务卡时） |
| 顶栏宽度 | `min(520px, 60vw)`，居中 |
| 顶栏圆角 | 22px（全圆角胶囊） |
| 任务卡 | 320 × 112，圆角 18，卡间距 12 |
| 卡片导航 | 当前卡居中，左右各露 96px 边缘；`scroll-snap-type: x mandatory` |
| 安全区 | 顶部 = `--dsh-frame-top-clearance`（Windows 标题栏高）+ 8px |
| 层级 | 顶栏 z-index 2147483000 以下，避免与宿主弹层打架 |

**Windows 特有坑**：窗口顶部是 `-webkit-app-region: drag` 拖拽带。顶栏若贴 `top:0` 会落进拖拽区，按钮点不动。所以要么下移，要么给交互元素显式 `-webkit-app-region: no-drag`——两条都做。

## 4. 动效

### 4.1 参数

```
--dswork-dur-fast:   120ms
--dswork-dur-base:   200ms
--dswork-dur-slow:   320ms
--dswork-ease-out:   cubic-bezier(.16, 1, .3, 1)     /* 进场 */
--dswork-ease-in:    cubic-bezier(.4, 0, 1, 1)        /* 退场 */
--dswork-ease-soft:  cubic-bezier(.32, .72, 0, 1)     /* 位移 */
--dswork-spring:     cubic-bezier(.34, 1.4, .64, 1)   /* 微弱回弹，只用于卡片 */
```

### 4.2 十二原则在本项目里的落点

不是背诵，是**只取用得上的五条**：

| 原则 | 落在哪 | 具体做法 |
|---|---|---|
| **预备（Anticipation）** | 唤起顶栏 | 按下热键先出现 20ms 的 40×40 圆点，再横向展开成胶囊 |
| **跟随与重叠（Follow Through）** | 任务卡滑动 | 卡片到位后允许 3% 过冲再回弹；相邻卡有 40ms 延迟跟随 |
| **缓入缓出（Slow In/Out）** | 所有位移 | 一律用 `ease-out` 进场、`ease-in` 退场，**禁止 linear** |
| **挤压与拉伸（Squash & Stretch）** | 完成一个节点 | 卡片在 120ms 内 `scaleY .94 → 1` 再淡出，模拟"被收走" |
| **次要动作（Secondary Action）** | 采集中 | 框选层出现时，顶栏轻微下沉 2px 并降低饱和度 |

**明确不做**：粒子、光效、渐变流动、无限循环装饰动画。低频、短促、有因果——这是"理性克制"的具体含义。

### 4.3 无障碍

- 所有动效必须包在 `@media (prefers-reduced-motion: reduce)` 的降级分支里，降级后仅保留透明度变化。
- 焦点环 `2px solid var(--dswork-focus)`，不依赖颜色单独传达状态。
- 顶栏是 `role="toolbar"`，任务卡是 `role="button"`，状态变化通过 `aria-live="polite"` 播报。

## 5. 组件状态机（视觉侧）

```
Idle ──热键──► Capturing ──选区完成──► Recognizing ──完成──► Active
  ▲                                                          │
  └────────────────── 全部节点完成 / 手动收起 ─────────────────┘

Active ──交接中──► Handoff（卡片显示"AI 正在做"，进度脉动）──► Review
Active ──宿主断线──► Offline（灰态 + 上次快照 + 重连提示）
```

每个状态都有对应的最小可用表现，不允许出现"空白小条"。

## 6. 文案

- 中文优先，短句，动词开头：「拍一张，告诉我你在做什么」。
- 不用感叹号，不用「啦」「哦」这类拟人化尾巴。
- 失败文案必须包含**下一步动作**：「识别失败了。再拍一次，或者直接打字告诉我。」
- 所有文案集中放在 `packages/ui/src/copy/zh-CN.ts`，为将来多语言留好结构。

## 7. 禁止清单

- ❌ 纯黑 `#000` 与纯白 `#fff`（永远带一点色相）
- ❌ `border: 1px solid #ddd` 式的硬描边
- ❌ `linear` 缓动
- ❌ 超过 400ms 的界面动效
- ❌ 同时出现三种以上阴影
- ❌ 任何依赖外部图片资源的装饰（全部用 CSS 画）
