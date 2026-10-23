/**
 * 「讲讲它」的展示文案：模型文本的归一化 + 每档失败要说的那一句。
 *
 * 为什么值得单测：这块东西不参与状态机，却决定用户点了以后是「知道下一步」
 * 还是「对着一行英文栈发呆」。卡片只有三个位置（标题/正文/提示），
 * 文案写歪了没有任何一层会兜住。
 */

import assert from 'node:assert/strict';
import test from 'node:test';

import { LESSON_REASON_MAX, LESSON_TEXT_MAX, lessonFailure, lessonSuccess, lessonText } from './lesson.ts';

test('lessonText：剥掉 markdown 记号，只留能读的字', () => {
  const raw = '**幂等**是「做一次」和「做十次」结果一样。\n\n见 [RFC 7231](https://example.com)，`retry` 不改变结果。';
  assert.equal(lessonText(raw), '幂等是「做一次」和「做十次」结果一样。 见 RFC 7231，retry 不改变结果。');
});

test('lessonText：代码块整块丢掉（卡片上放不下，留着只会被截断成乱码）', () => {
  assert.equal(lessonText('看这个\n```ts\nconst a = 1;\n```\n就够了'), '看这个 就够了');
});

test('lessonText：列表符号和多余空白都压成一行', () => {
  assert.equal(lessonText('- 先看门\n- 再看锁\n\n• 最后看单据'), '先看门 再看锁 最后看单据');
});

test('lessonText：超过上限就截断并补省略号', () => {
  const long = '重'.repeat(LESSON_TEXT_MAX + 20);
  const out = lessonText(long);
  assert.equal(Array.from(out).length, LESSON_TEXT_MAX);
  assert.equal(out.endsWith('…'), true);
});

test('lessonText：短文本原样返回，不补省略号', () => {
  assert.equal(lessonText('一句就够'), '一句就够');
});

test('lessonSuccess：正文归一化，副行提示怎么关掉', () => {
  const view = lessonSuccess('**要点**只有一个：重试要幂等。');
  assert.equal(view.phase, 'done');
  assert.equal(view.title, '讲讲它');
  assert.equal(view.text, '要点只有一个：重试要幂等。');
  assert.equal(view.hint, '按 Esc 关掉');
  assert.equal(view.unsupported, false);
});

test('lessonSuccess：有练习时副行放「试一下」，不再提示关窗口', () => {
  const view = lessonSuccess('要点。', '把 `POST` 重发一次');
  assert.equal(view.hint, '试一下：把 POST 重发一次');
});

test('lessonSuccess：模型回空也要给人看得懂的一行', () => {
  const view = lessonSuccess('   ');
  assert.equal(view.phase, 'done');
  assert.notEqual(view.text.trim(), '');
  assert.notEqual(view.hint.trim(), '');
});

test('lessonFailure 404：节点不在了，但宿主是认识的（不置灰）', () => {
  const view = lessonFailure(404, '没有这个节点');
  assert.equal(view.phase, 'error');
  assert.equal(view.title, '这颗节点不在了');
  assert.equal(view.text, '没有这个节点');
  assert.equal(view.unsupported, false);
  assert.notEqual(view.hint.trim(), '');
});

test('lessonFailure 405：宿主没这个接口，按钮该置灰', () => {
  const view = lessonFailure(405, 'method not allowed');
  assert.equal(view.unsupported, true);
  assert.equal(view.title, '这个宿主还不认识讲解');
});

test('lessonFailure 断网：原因说得具体，提示给的是重启宿主', () => {
  for (const status of [null, 0]) {
    const view = lessonFailure(status, '');
    assert.equal(view.title, '没连上宿主');
    assert.match(view.hint, /dsh/);
    assert.equal(view.unsupported, false);
  }
});

test('lessonFailure 503：宿主在，只是没接模型', () => {
  const view = lessonFailure(503, '宿主没有提供 llm 服务');
  assert.equal(view.title, '宿主没接模型');
  assert.equal(view.unsupported, false);
  assert.match(view.text, /llm/);
});

test('lessonFailure 500：原因太长时截断，不把整段栈塞进卡片', () => {
  const view = lessonFailure(500, 'Error: '.repeat(40));
  assert.equal(view.title, '讲的时候出错了');
  assert.ok(Array.from(view.text).length <= LESSON_REASON_MAX);
  assert.equal(view.text.endsWith('…'), true);
});

test('lessonFailure：每一档失败都必须给出去处（标题/正文/提示都不能空）', () => {
  for (const status of [null, 0, 400, 404, 405, 409, 500, 501, 503, 418]) {
    const view = lessonFailure(status, '');
    assert.notEqual(view.title.trim(), '', `status=${String(status)} 缺标题`);
    assert.notEqual(view.text.trim(), '', `status=${String(status)} 缺正文`);
    assert.notEqual(view.hint.trim(), '', `status=${String(status)} 缺提示`);
  }
});
