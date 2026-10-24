import assert from 'node:assert/strict';
import { test } from 'node:test';

import { listWorkspaces, pickRoot, pickWorkspaceRoot } from './root.ts';

test('显式配置压过一切，且会去掉首尾空白', () => {
  const decision = pickRoot({
    configured: '  D:\\work\\AI\\work  ',
    workspaces: [{ path: 'C:\\elsewhere', sessionIds: ['s1'] }],
    cwd: 'C:\\Users\\someone\\.dsh\\profiles\\desktop',
  });
  assert.deepEqual(decision, { root: 'D:\\work\\AI\\work', source: 'config' });
});

test('配置是空串 / 纯空白时不算配置，继续往下走', () => {
  const decision = pickRoot({
    configured: '   ',
    workspaces: [{ path: 'D:\\work\\AI\\work', sessionIds: ['s1'] }],
    cwd: 'C:\\profiles\\desktop',
  });
  assert.deepEqual(decision, { root: 'D:\\work\\AI\\work', source: 'workspace' });
});

test('没有配置时跟随宿主登记的工作区，而不是进程 cwd', () => {
  const decision = pickRoot({
    workspaces: [{ path: 'D:\\work\\AI\\work', sessionIds: ['s1'] }],
    cwd: 'C:\\Users\\25108\\.dsh\\profiles\\desktop',
  });
  assert.deepEqual(decision, { root: 'D:\\work\\AI\\work', source: 'workspace' });
});

test('有会话的工作区优先于登记顺序更靠前的空工作区', () => {
  const decision = pickRoot({
    workspaces: [
      { path: 'C:\\empty', sessionIds: [] },
      { path: 'D:\\work\\AI\\work', sessionIds: ['s1', 's2'] },
    ],
    cwd: 'C:\\profiles\\desktop',
  });
  assert.deepEqual(decision, { root: 'D:\\work\\AI\\work', source: 'workspace' });
});

test('全都空着的工作区仍然可以用来决定根目录（登记顺序第一个）', () => {
  const decision = pickRoot({
    workspaces: [{ path: 'C:\\a', sessionIds: [] }, { path: 'C:\\b', sessionIds: [] }],
    cwd: 'C:\\profiles\\desktop',
  });
  assert.deepEqual(decision, { root: 'C:\\a', source: 'workspace' });
});

test('登记表里路径为空的条目被跳过', () => {
  assert.equal(pickWorkspaceRoot([{ path: '   ' }, { path: 'D:\\work\\AI\\work' }]), 'D:\\work\\AI\\work');
});

test('没有登记表 / 登记表为空时退回 cwd', () => {
  const cwd = 'D:\\work\\AI\\work';
  assert.deepEqual(pickRoot({ cwd }), { root: cwd, source: 'cwd' });
  assert.deepEqual(pickRoot({ workspaces: [], cwd }), { root: cwd, source: 'cwd' });
});

test('listWorkspaces：服务缺失、名字不存在、list 抛错都只是「不知道」', () => {
  assert.equal(listWorkspaces(() => undefined), undefined);
  assert.equal(
    listWorkspaces(() => {
      throw new Error('no service');
    }),
    undefined,
  );
  assert.equal(listWorkspaces(() => ({ list: 'not a function' })), undefined);
  assert.equal(
    listWorkspaces(() => ({
      list: () => {
        throw new Error('registry not ready');
      },
    })),
    undefined,
  );
  assert.equal(listWorkspaces(() => ({ list: () => 'nope' })), undefined);
});

test('listWorkspaces：只留带 path 字符串的对象', () => {
  const got = listWorkspaces(() => ({
    list: () => [{ path: 'D:\\work\\AI\\work', sessionIds: ['s1'] }, { path: 42 }, null, 'x'],
  }));
  assert.deepEqual(got, [{ path: 'D:\\work\\AI\\work', sessionIds: ['s1'] }]);
});
