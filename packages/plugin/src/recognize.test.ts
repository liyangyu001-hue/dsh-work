import assert from 'node:assert/strict';
import test from 'node:test';

import type { LlmChunk } from './recognize.ts';
import { draftFromSpecs, parseRecognizedPlan, planSetFromDraft, planSetFromSpecs, recognizePlan, REPAIR_PROMPT } from './recognize.ts';

/* ─────────────────────────── 解析 ─────────────────────────── */

test('解析：带代码块的 JSON → 标题、节点、验收齐全', () => {
  const raw = [
    '```json',
    JSON.stringify({
      title: '修好登录页',
      nodes: [
        {
          title: '复现问题',
          detail: '用测试账号登录三次',
          acceptance: '能稳定复现 500',
          children: [{ title: '看控制台报错', detail: '打开 devtools', acceptance: '拿到堆栈' }],
        },
      ],
    }),
    '```',
  ].join('\n');

  const plan = parseRecognizedPlan(raw);
  assert.equal(plan.title, '修好登录页');
  assert.equal(plan.specs.length, 1);
  assert.equal(plan.specs[0]!.acceptance, '能稳定复现 500');
  assert.equal(plan.specs[0]!.children?.[0]?.title, '看控制台报错');
});

test('解析：前后带解释文字也能抠出对象', () => {
  const raw = '好的，这是任务树：\n{"title":"发布","nodes":[{"title":"写发布说明"}]}\n希望对你有帮助。';
  const plan = parseRecognizedPlan(raw);
  assert.equal(plan.title, '发布');
  assert.equal(plan.specs[0]!.title, '写发布说明');
});

test('解析：直接给数组也接受', () => {
  const plan = parseRecognizedPlan('[{"title":"A"},{"title":"B"}]');
  assert.equal(plan.specs.length, 2);
  assert.equal(plan.title, '');
});

test('解析：不是 JSON 时退回缩进大纲，不整条丢弃', () => {
  const plan = parseRecognizedPlan('- 第一步\n  - 子步骤\n- 第二步');
  assert.equal(plan.specs.length, 2);
  assert.equal(plan.specs[0]!.children?.[0]?.title, '子步骤');
});

test('解析：空输入与纯废话 → 空计划（由调用方报错，不写文档）', () => {
  assert.deepEqual(parseRecognizedPlan(''), { title: '', specs: [] });
  assert.deepEqual(parseRecognizedPlan('抱歉，我无法识别这张图片。').specs, []);
});

test('解析：kind 原样保留（截图是概念时不丢标记）', () => {
  const plan = parseRecognizedPlan('{"nodes":[{"title":"理解 TCP 三次握手","kind":"concept"}]}');
  assert.equal(plan.specs[0]!.kind, 'concept');
});

/* ─────────────────────────── 调用 ─────────────────────────── */

interface Recorded {
  provider?: unknown;
  model?: unknown;
  messages?: unknown;
  signal?: unknown;
}

/** 假模型：按顺序吐出预设文本，第二次调用起用第二段。 */
function fakeLlm(outputs: Array<string | { fail: string }>) {
  const calls: Recorded[] = [];
  const llm = {
    calls,
    stream(options: Recorded): AsyncIterable<LlmChunk> {
      calls.push(options);
      const output = outputs[Math.min(calls.length - 1, outputs.length - 1)]!;
      return (async function* generate() {
        if (typeof output !== 'string') {
          yield { type: 'text-delta', text: '半句' };
          yield { type: 'finish', reason: { kind: 'error', failure: { code: 'RATE_LIMIT', message: output.fail } } };
          return;
        }
        yield { type: 'block-start' };
        for (const piece of output.match(/.{1,8}/gs) ?? []) {
          yield { type: 'text-delta', text: piece };
        }
        yield { type: 'finish', reason: { kind: 'stop' } };
      })();
    },
  };
  return llm;
}

function fakeAttachments() {
  const parts: unknown[][] = [];
  return {
    parts,
    async admitPromptContent(input: unknown[]): Promise<unknown[]> {
      parts.push(input);
      return [
        { type: 'text', text: 'prompt' },
        { type: 'image', attachment: { attachmentId: 'sha256:abc', mediaType: 'image/png', width: 10, height: 10 } },
      ];
    },
  };
}

test('调用：把图和提示交给模型，拼回 JSON 并解析成计划', async () => {
  const llm = fakeLlm(['{"title":"整理需求","nodes":[{"title":"列出约束","acceptance":"写下来"}]}']);
  const attachments = fakeAttachments();

  const plan = await recognizePlan(
    { llm, attachments, provider: 'deepseek-official', model: 'deepseek-v4-flash-vision-exp' },
    'aGVsbG8=',
  );

  assert.equal(plan.title, '整理需求');
  assert.equal(plan.specs[0]!.title, '列出约束');
  assert.equal(plan.attempts, 1);
  assert.equal(plan.raw.includes('整理需求'), true);

  assert.equal(llm.calls.length, 1);
  assert.equal(llm.calls[0]!.provider, 'deepseek-official');
  assert.equal(llm.calls[0]!.model, 'deepseek-v4-flash-vision-exp');
  assert.ok(llm.calls[0]!.signal, '必须把超时信号交给模型');

  const image = attachments.parts[0]!.find(
    (part) => (part as { type?: string }).type === 'image',
  ) as { data?: string; mediaType?: string };
  assert.equal(image.data, 'aGVsbG8=');
  assert.equal(image.mediaType, 'image/png');
});

test('调用：第一次不可解析时，用同一张图就地修格式再问一次', async () => {
  const llm = fakeLlm(['抱歉，我看不清。', '{"title":"重来","nodes":[{"title":"再做一次"}]}']);
  const attachments = fakeAttachments();

  const plan = await recognizePlan({ llm, attachments, provider: 'p', model: 'm' }, 'aGVsbG8=');

  assert.equal(plan.attempts, 2);
  assert.equal(plan.title, '重来');
  assert.equal(llm.calls.length, 2);

  const second = llm.calls[1]!.messages as Array<{ content: Array<{ type: string; text?: string }> }>;
  const lastPart = second[0]!.content.at(-1);
  assert.equal(lastPart?.text, REPAIR_PROMPT);
});

test('调用：流里的失败原因原样变成可读错误', async () => {
  const llm = fakeLlm([{ fail: '额度用完了' }]);
  const attachments = fakeAttachments();

  await assert.rejects(
    () => recognizePlan({ llm, attachments, provider: 'p', model: 'm' }, 'aGVsbG8='),
    /RATE_LIMIT：额度用完了/,
  );
});

test('落库：计划树变成一条 plan.set，id 由调用方给', () => {
  let next = 0;
  const intent = planSetFromSpecs(
    [{ title: '父', children: [{ title: '子' }] }],
    { now: 1_700_000_000_000, newId: () => `nd_${++next}`, title: '标题' },
  );

  assert.equal(intent.type, 'plan.set');
  assert.equal(intent.title, '标题');
  assert.deepEqual(intent.roots, ['nd_1']);
  assert.equal(intent.nodes.length, 2);
  assert.equal(intent.nodes[0]!.parentId, null);
  assert.equal(intent.nodes[1]!.parentId, 'nd_1');
});

test('落库：没有标题时不带 title 字段', () => {
  const intent = planSetFromSpecs([{ title: 'A' }], { now: 0, newId: () => 'nd_1' });
  assert.equal('title' in intent, false);
});


test('草稿：先生成不落库，确认后才变成 plan.set', () => {
  let n = 0;
  const specs = [{ title: '父', children: [{ title: '子一', acceptance: '看得见' }] }];
  const draft = draftFromSpecs(specs, { now: 1000, newId: () => `nd_${++n}`, title: '母题' });
  assert.equal(draft.title, '母题');
  assert.equal(draft.nodes.length, 2);
  assert.equal(draft.roots.length, 1);

  const intent = planSetFromDraft(draft);
  assert.equal(intent.type, 'plan.set');
  assert.equal(intent.nodes.length, 2);
  assert.equal(intent.title, '母题');
  // 草稿本身不含任何「已提交」痕迹：它只是节点数组。
  assert.equal(Object.prototype.hasOwnProperty.call(draft, 'committed'), false);
});

test('草稿：没有标题时不带多余字段', () => {
  const draft = draftFromSpecs([{ title: '一件事' }], { now: 1, newId: () => 'nd_1' });
  const intent = planSetFromDraft(draft);
  assert.equal(Object.prototype.hasOwnProperty.call(intent, 'title'), false);
});