/**
 * 玻璃条入口的契约：什么时候能点、点完说哪句。
 *
 * 只测纯函数；按钮长什么样、点了有没有真的弹窗，靠 scripts/shot.mjs 真机截图。
 */

import assert from "node:assert/strict";
import test from "node:test";

import { graphEntry, openFeedback } from "./open-entry.ts";

test("节点图入口：状态还没到时先别点，也别误说成没有任务", () => {
  const entry = graphEntry(null);
  assert.equal(entry.enabled, false);
  assert.match(entry.hint, /正在读取/);
});

test("节点图入口：没有任务树时置灰，并说清先去拆任务", () => {
  const entry = graphEntry(0);
  assert.equal(entry.enabled, false);
  assert.match(entry.hint, /拆一次任务/);
});

test("节点图入口：有任务树时能点，说明它是只读地图", () => {
  const entry = graphEntry(7);
  assert.equal(entry.enabled, true);
  assert.match(entry.hint, /只读/);
});

test("反馈：打开了 / 正在起 / 没接上，三种说法别混", () => {
  assert.equal(openFeedback("panel", { companion: true }), "桌面面板已打开");
  assert.equal(openFeedback("graph", { companion: true }), "节点图已打开");
  assert.equal(openFeedback("graph", { launched: true }), "正在启动节点图…");
  assert.equal(openFeedback("graph", {}), "桌面伴随层没在运行，节点图没打开");
});

test("反馈：宿主给了 hint 就用它，别拿 HTTP 码糊人", () => {
  assert.equal(
    openFeedback("graph", { status: 400, hint: "target 只能是 panel 或 graph" }),
    "target 只能是 panel 或 graph",
  );
  assert.equal(
    openFeedback("panel", { status: 503, error: "宿主没有提供打开面板的通道" }),
    "宿主没有提供打开面板的通道",
  );
  assert.equal(openFeedback("graph", { status: 502 }), "打开失败（HTTP 502）");
});

test("反馈：fetch 失败单独报，不说成宿主拒绝", () => {
  assert.equal(openFeedback("panel", { offline: true }), "连不上宿主，桌面面板没打开");
  assert.equal(openFeedback("graph", { offline: true }), "连不上宿主，节点图没打开");
});
