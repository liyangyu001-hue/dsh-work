import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  CHAT_PROMPT,
  MAX_ANSWER_CHARS,
  MAX_HISTORY_TURNS,
  MAX_QUESTION_CHARS,
  buildChatPrompt,
  clipText,
  tidyAnswer,
} from './chat.ts';

const node = {
  title: '区分幂等与重试安全',
  detail: '写出三个反例',
  acceptance: '三条判据都能自圆其说',
};

test('clipText 把换行和多余空白压成一行', () => {
  assert.equal(clipText('  第一行\n\n第二行\t带制表符  ', 100), '第一行 第二行 带制表符');
});

test('clipText 超长时截断并留可见省略号', () => {
  const clipped = clipText('字'.repeat(50), 10);
  assert.equal(clipped.length, 10);
  assert.ok(clipped.endsWith('…'));
});

test('buildChatPrompt 带上这一步的标题、目标、验收', () => {
  const prompt = buildChatPrompt(node, '那第二步呢？');
  assert.ok(prompt.startsWith(CHAT_PROMPT));
  assert.ok(prompt.includes('这一步：区分幂等与重试安全'));
  assert.ok(prompt.includes('他自己写的目标：写出三个反例'));
  assert.ok(prompt.includes('做到什么算完：三条判据都能自圆其说'));
  assert.ok(prompt.endsWith('他现在问：那第二步呢？'));
});

test('buildChatPrompt 不塞空字段', () => {
  const prompt = buildChatPrompt({ title: '只有标题', detail: '', acceptance: '' }, '这是什么');
  assert.ok(!prompt.includes('他自己写的目标'));
  assert.ok(!prompt.includes('做到什么算完'));
  assert.ok(!prompt.includes('已经给他讲过的要点'));
});

test('buildChatPrompt 会把上次讲解带上，避免答重复话', () => {
  const prompt = buildChatPrompt({ ...node, advice: '幂等是同一请求重复执行结果一致' }, '再举个例子');
  assert.ok(prompt.includes('已经给他讲过的要点：幂等是同一请求重复执行结果一致'));
});

test('buildChatPrompt 只留最近几轮问答，且压成一行', () => {
  const history = Array.from({ length: MAX_HISTORY_TURNS + 3 }, (_, i) => ({
    question: `问题 ${i}\n第二行`,
    answer: `答案 ${i}`,
  }));
  const prompt = buildChatPrompt(node, '继续', history);
  assert.ok(!prompt.includes('问题 0'));
  assert.ok(prompt.includes(`问题 ${MAX_HISTORY_TURNS + 2} 第二行`));
  assert.ok(prompt.includes(`答案 ${MAX_HISTORY_TURNS + 2}`));
});

test('buildChatPrompt 把超长问题截到上限', () => {
  const prompt = buildChatPrompt(node, '问'.repeat(MAX_QUESTION_CHARS + 80));
  const asked = prompt.slice(prompt.lastIndexOf('他现在问：') + '他现在问：'.length);
  assert.equal(asked.length, MAX_QUESTION_CHARS);
  assert.ok(asked.endsWith('…'));
});

test('tidyAnswer 去掉三反引号围栏和整段引号', () => {
  assert.equal(tidyAnswer('```text\n第一句。\n```'), '第一句。');
  assert.equal(tidyAnswer('"他说：这样就行"'), '他说：这样就行');
});

test('tidyAnswer 折叠多余空行并按上限截断', () => {
  assert.equal(tidyAnswer('第一段\n\n\n第二段'), '第一段\n第二段');
  const long = tidyAnswer('字'.repeat(MAX_ANSWER_CHARS + 50));
  assert.equal(long.length, MAX_ANSWER_CHARS);
  assert.ok(long.endsWith('…'));
});
