import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/**
 * 伴随层的配置模块是 .cjs（Electron 主进程直接 require，没有编译步骤），
 * 所以这里用 createRequire 载入，而不是 import —— import 会因缺少声明文件而报 TS7016。
 */
const require = createRequire(import.meta.url);
const {
  DEFAULT_HOTKEY,
  configFile,
  isBareTab,
  normalizeHotkey,
  parseConfig,
  readConfig,
  resolveHotkey,
} = require('./config.cjs') as {
  DEFAULT_HOTKEY: string;
  configFile: () => string;
  isBareTab: (combo: string) => boolean;
  normalizeHotkey: (value: unknown) => string | null;
  parseConfig: (text: string) => ParsedConfig;
  readConfig: (file?: string) => ReadConfigResult;
  resolveHotkey: (config: CompanionConfig) => ResolvedHotkey;
};

type CompanionConfig = { hotkey: string; allowBareTab: boolean };
type ParsedConfig = { config: CompanionConfig; problems: string[] };
type ReadConfigResult = ParsedConfig & { file: string; exists: boolean };
type ResolvedHotkey = { ok: boolean; combo: string; reason?: string };

test('默认配置：Control+Alt+Space，裸 Tab 关闭', () => {
  const { config, problems } = parseConfig('{}');
  assert.deepEqual(config, { hotkey: 'Control+Alt+Space', allowBareTab: false });
  assert.deepEqual(problems, []);
});

test('热键可从配置改，且写法做归一化（去空白）', () => {
  const { config, problems } = parseConfig('{"hotkey":" Control + Shift + W "}');
  assert.equal(config.hotkey, 'Control+Shift+W');
  assert.deepEqual(problems, []);
  assert.equal(resolveHotkey(config).ok, true);
});

test('坏 JSON / 非对象 / 字段类型不对：回退默认并给出可读原因', () => {
  const cases: Array<[string, string]> = [
    ['{oops', 'JSON'],
    ['[]', '对象'],
    ['{"hotkey":42}', 'hotkey'],
    ['{"hotkey":"Control++Space"}', 'hotkey'],
    ['{"allowBareTab":"true"}', 'allowBareTab'],
  ];
  for (const [text, fragment] of cases) {
    const { config, problems } = parseConfig(text);
    assert.equal(config.hotkey, DEFAULT_HOTKEY, text);
    assert.equal(config.allowBareTab, false, text);
    assert.ok(
      problems.some((problem) => problem.includes(fragment)),
      `${text} 应提示 ${fragment}，实际：${problems.join(' / ')}`,
    );
  }
});

test('裸 Tab 默认拒绝注册，allowBareTab: true 才放行', () => {
  const off = parseConfig('{"hotkey":"Tab"}').config;
  const denied = resolveHotkey(off);
  assert.equal(denied.ok, false);
  assert.equal(denied.combo, 'Tab');
  assert.match(denied.reason ?? '', /allowBareTab/);

  const on = parseConfig('{"hotkey":"Tab","allowBareTab":true}').config;
  assert.equal(resolveHotkey(on).ok, true);

  // 带修饰键的 Tab 是普通组合键，不触发危险开关
  const chord = parseConfig('{"hotkey":"Control+Tab"}').config;
  assert.equal(resolveHotkey(chord).ok, true);
  assert.equal(isBareTab('Control+Tab'), false);
});

test('裸 Tab 判定大小写不敏感，且只有单键才算', () => {
  assert.equal(isBareTab('Tab'), true);
  assert.equal(isBareTab('tab'), true);
  assert.equal(isBareTab('Control+Tab'), false);
  assert.equal(isBareTab(''), false);
  assert.equal(normalizeHotkey('Tab'), 'Tab');
  assert.equal(normalizeHotkey(''), null);
  assert.equal(normalizeHotkey(undefined), null);
});

test('配置文件缺失时静默用默认值；存在时读入并容忍 BOM', (t) => {
  const dir = mkdtempSync(join(tmpdir(), 'dshwork-config-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));

  const missing = readConfig(join(dir, 'config.json'));
  assert.equal(missing.exists, false);
  assert.deepEqual(missing.problems, []);
  assert.equal(missing.config.hotkey, DEFAULT_HOTKEY);

  const file = join(dir, 'config.json');
  writeFileSync(file, '\uFEFF{"hotkey":"Alt+Space","allowBareTab":false}', 'utf8');
  const loaded = readConfig(file);
  assert.equal(loaded.exists, true);
  assert.equal(loaded.config.hotkey, 'Alt+Space');
  assert.equal(loaded.file, file);
});

test('配置文件路径跟随 DSH_HOME', (t) => {
  const previous = process.env['DSH_HOME'];
  t.after(() => {
    if (previous === undefined) delete process.env['DSH_HOME'];
    else process.env['DSH_HOME'] = previous;
  });

  process.env['DSH_HOME'] = join(tmpdir(), 'dshwork-home');
  assert.equal(configFile(), join(tmpdir(), 'dshwork-home', 'dsh-work', 'config.json'));
});
