/**
 * 生成文档配图（小条 + 面板）。
 *
 * 为什么不让脚本直接截桌面：仓库是公开的，桌面截图会把用户其他窗口、标签页、
 * 账号名一起带出去。所以文档图一律用**合成状态 + 合成底图**渲染真实 HTML ——
 * 控件、样式、排版都是真的，只是喂进去的数据是假的。
 *
 *   node scripts/make-shots.mjs
 */
import { chromium } from "playwright-core";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

const OUT_BAR = resolve("docs/assets/bar.png");
const OUT_PANEL = resolve("docs/assets/panel.png");

const barHtml = pathToFileURL(resolve("apps/companion/src/bar/index.html")).href;
const panelHtml = pathToFileURL(resolve("apps/companion/src/panel/index.html")).href;

/** 合成底图：真实运行时这里是桌面截图，文档里换成柔光渐变。 */
const BACKDROP =
  "data:image/svg+xml;charset=utf-8," +
  encodeURIComponent(
    '<svg xmlns="http://www.w3.org/2000/svg" width="760" height="396">' +
      '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">' +
      '<stop offset="0" stop-color="#1d2b4a"/><stop offset=".45" stop-color="#3a2a55"/>' +
      '<stop offset="1" stop-color="#122033"/></linearGradient></defs>' +
      '<rect width="760" height="396" fill="url(#g)"/>' +
      '<circle cx="150" cy="330" r="220" fill="#0a84ff" opacity=".18"/>' +
      '<circle cx="640" cy="60" r="180" fill="#ff375f" opacity=".12"/></svg>',
  );

/** 够满的合成状态：同时演示 已完成 / 当前 / 待进行 / 概念节点。 */
const SNAPSHOT = {
  doc: {
    schema: 1,
    id: "wk_demo",
    title: "把落地页的注册流程改完",
    createdAt: "2026-01-01T00:00:00.000Z",
    updatedAt: "2026-01-01T00:00:00.000Z",
    seq: 7,
    pointer: "nd_form",
    roots: ["nd_audit", "nd_form", "nd_cases", "nd_idem", "nd_qa"],
    nodes: {
      nd_audit: {
        id: "nd_audit",
        parent: null,
        children: [],
        title: "复核注册流程的阻塞点",
        kind: "task",
        status: "done",
        detail: "把用户实际走一遍流程时的卡点列出来",
        acceptance: "每个卡点都有复现步骤",
      },
      nd_form: {
        id: "nd_form",
        parent: null,
        children: [],
        title: "把表单校验抽成纯函数",
        kind: "task",
        status: "active",
        detail: "校验逻辑现在散在三个组件里，同一套规则写了两遍",
        acceptance: "校验只依赖入参，组件里不再有重复判断",
      },
      nd_cases: {
        id: "nd_cases",
        parent: null,
        children: [],
        title: "补三条边界用例",
        kind: "task",
        status: "todo",
        detail: "空值、超长、重复提交",
      },
      nd_idem: {
        id: "nd_idem",
        parent: null,
        children: [],
        title: "幂等提交",
        kind: "concept",
        status: "todo",
        detail: "同一份表单提交两次，第二次不该产生副作用",
      },
      nd_qa: {
        id: "nd_qa",
        parent: null,
        children: [],
        title: "交接给 QA 复测",
        kind: "task",
        status: "todo",
      },
    },
  },
};

const browser = await chromium.launch({ channel: "chrome", headless: true });

/** 小条：只有一个状态点，尺寸就按窗口实际大小裁。 */
{
  const page = await browser.newPage({
    viewport: { width: 564, height: 88 },
    deviceScaleFactor: 2,
    colorScheme: "dark",
  });
  await page.addInitScript((payload) => {
    const noop = () => () => {};
    // 接口名以 apps/companion/src/preload.cjs 为准：小条与面板是两套 API。
    window.dshwork = {
      onHostState: (cb) => {
        queueMicrotask(() => cb(payload));
        return noop();
      },
      onHotkey: noop,
      onCaptureResult: noop,
      onCaptureCancel: noop,
      onRecognize: noop,
      setPointerOver: () => {},
      openPanel: () => {},
      ready: () => {},
    };
  }, { state: "online", snapshot: SNAPSHOT });
  await page.goto(barHtml, { waitUntil: "load" });
  await page.waitForTimeout(500);
  await page.screenshot({ path: OUT_BAR, omitBackground: true });
  await page.close();
}

/** 面板：760×396，是主进程里写死的窗口尺寸。 */
{
  const page = await browser.newPage({
    viewport: { width: 760, height: 396 },
    deviceScaleFactor: 2,
    colorScheme: "dark",
  });
  await page.addInitScript((payload) => {
    const noop = () => () => {};
    window.dshworkPanel = {
      onState: (cb) => {
        queueMicrotask(() => cb(payload));
        return noop();
      },
      onCapture: noop,
      onRecognize: noop,
      onBackdrop: (cb) => {
        queueMicrotask(() => cb(BACKDROP));
        return noop();
      },
      intent: () => {},
      capture: () => {},
      close: () => {},
      ready: () => {},
    };
  }, { state: "online", snapshot: SNAPSHOT });
  await page.goto(panelHtml, { waitUntil: "load" });
  await page.waitForTimeout(700); // 等入场动效跑完，别截到过渡中间帧
  await page.screenshot({ path: OUT_PANEL, omitBackground: true });
  await page.close();
}

await browser.close();
console.log(`已生成 ${OUT_BAR}\n已生成 ${OUT_PANEL}`);
