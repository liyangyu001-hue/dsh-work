/**
 * 学习页的数据整理：顺序、兜底、边界。
 *
 * 只测纯函数；页面长什么样靠 scripts/shot.mjs 真机截图。
 * 这里守的是三件容易写错的事：
 * ① 列必须按树的顺序（不是 JSON 的键序）；② 缺字段要有能读的兜底；③ 脏数据不能死循环。
 */

import assert from "node:assert/strict";
import test from "node:test";

import { studyProgress, studyRows, studyTips, statusLabel, type StudyDocLike } from "./study-model.ts";

const doc: StudyDocLike = {
  pointer: "b",
  roots: ["a", "c"],
  nodes: {
    // 故意把 b 放在 a 前面：键序和树序不一致，正是要防的那种数据
    b: { id: "b", title: "第二步", status: "active", children: [] },
    a: { id: "a", title: "第一步", status: "done", children: ["b"] },
    c: { id: "c", title: "另一棵树", status: "todo", detail: " 这里写了点解释 ", children: ["d"] },
    d: { id: "d", title: "子步骤", status: "todo", acceptance: "验收：能跑通" },
  },
};

test("学习页：按树的顺序摊平，子节点缩进一级", () => {
  const rows = studyRows(doc);
  assert.deepEqual(
    rows.map((row) => [row.id, row.depth]),
    [
      ["a", 0],
      ["b", 1],
      ["c", 0],
      ["d", 1],
    ],
  );
  assert.equal(rows[0]?.childCount, 1);
});

test("学习页：没有 doc 就是空列，不抛", () => {
  assert.deepEqual(studyRows(undefined), []);
});

test("学习页：成环/重复挂不会死循环，每颗只出现一次", () => {
  const loop: StudyDocLike = {
    roots: ["x"],
    nodes: {
      x: { id: "x", title: "x", status: "todo", children: ["y"] },
      y: { id: "y", title: "y", status: "todo", children: ["x"] },
    },
  };
  assert.deepEqual(
    studyRows(loop).map((row) => row.id),
    ["x", "y"],
  );
});

test("学习页：基础知识优先 detail，没有就退回 acceptance", () => {
  assert.equal(studyTips(doc, "c")?.body, "这里写了点解释");
  assert.equal(studyTips(doc, "d")?.body, "验收：能跑通");
});

test("学习页：两样都没有时给一句能照着做的话，而不是空白", () => {
  const tips = studyTips(doc, "b");
  assert.match(tips?.body ?? "", /还没写解释/);
});

test("学习页：详解流程按孩子顺序列出来，没有孩子就是空数组", () => {
  assert.deepEqual(studyTips(doc, "a")?.steps, ["第二步"]);
  assert.deepEqual(studyTips(doc, "b")?.steps, []);
});

test("学习页：选了个不存在的 id 返回 null，交给页面显示引导话", () => {
  assert.equal(studyTips(doc, "nope"), null);
  assert.equal(studyTips(doc, null), null);
  assert.equal(studyTips(undefined, "a"), null);
});

test("学习页：进度与顶部小条同口径（skipped 不进分母）", () => {
  const rows = studyRows({
    roots: ["a", "b"],
    nodes: {
      a: { id: "a", title: "a", status: "done" },
      b: { id: "b", title: "b", status: "skipped" },
    },
  });
  assert.deepEqual(studyProgress(rows), { done: 1, total: 1 });
});

test("状态文案与原生面板一致", () => {
  assert.equal(statusLabel("active"), "正在做");
  assert.equal(statusLabel("done"), "已完成");
  assert.equal(statusLabel("skipped"), "已跳过");
  assert.equal(statusLabel("todo"), "待办");
});
