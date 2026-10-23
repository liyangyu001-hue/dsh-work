// 状态文档 → 界面模型
//
// 这一层只做"翻译"：把宿主给的 WorkDoc（对象图）摊平成线性列表，算出当前是第几个。
// 摊平规则必须和 core 的 linearOrder 完全一致（深度优先先序），否则小条说"第 2 个"、
// 面板却高亮另一个，用户会立刻发现两个界面在说不同的故事。

#pragma once

#include <string>
#include <vector>

#include "host.h"  // Widen：UTF-8 → UTF-16
#include "json.h"

namespace dshwork {

struct NodeView {
  std::string id;
  std::wstring title;
  std::wstring detail;      // 目标
  std::wstring acceptance;  // 验收
  std::wstring advice;      // 模型给的一句话建议（可能没有）
  std::wstring advice_alt;  // 更优解（可选）
  std::string handoff;      // 交接状态：offered / running / review / accepted / returned，空=没交给谁
  std::string agent_session;  // 正在跑这颗节点的 AI 会话 id；和 handoff=running 一起由宿主写入
  std::string status;       // todo / active / done / skipped
  std::string kind;         // task / concept
};

struct WorkModel {
  enum class State { Offline, Online } state = State::Offline;
  std::string note;  // 离线原因
  std::wstring title;      // 当前节点标题：小条只显示这一行
  std::wstring doc_title;  // 文档标题：面板头部用
  std::vector<NodeView> items;
  int current = -1;  // items 下标；-1 表示全部完成或没有任务
  int done = 0;
  int total = 0;
};

/**
 * 有没有 AI 会话正在跑。
 *
 * 只认宿主写下的 handoff.state == running：这是「AI 真的在干活」的唯一权威信号，
 * 不去猜 status=active 或标题里有没有「AI」。窗口边缘的蓝框由它点亮。
 */
inline bool HasRunningAgent(const WorkModel& model) {
  for (const NodeView& item : model.items) {
    if (item.handoff == "running") return true;
  }
  return false;
}

/** 正在跑的那个会话 id（可能为空：旧文档只写了 running）。日志里用它核对是哪一次。 */
inline std::string RunningAgentSession(const WorkModel& model) {
  for (const NodeView& item : model.items) {
    if (item.handoff == "running") return item.agent_session;
  }
  return std::string();
}

/** 深度优先先序，和 packages/core/src/state.ts 的 linearOrder 同一套规则。 */
inline void WalkNodes(const JValue& nodes, const JValue& roots, const std::string& id, std::vector<NodeView>& out,
                      int depth) {
  if (depth > 64) return;  // 防畸形数据把栈吃穿
  const JValue* node = nodes.Get(id);
  if (node == nullptr || !node->IsObject()) return;

  NodeView view;
  view.id = id;
  view.title = Widen(node->GetString("title"));
  view.detail = Widen(node->GetString("detail"));
  view.acceptance = Widen(node->GetString("acceptance"));
  view.status = node->GetString("status", "todo");
  view.kind = node->GetString("kind", "task");
  // advice 是可选的挂件：没有就留空，面板会自动省掉那一行
  if (const JValue* advice = node->Get("advice")) {
    view.advice = Widen(advice->GetString("text"));
    view.advice_alt = Widen(advice->GetString("alt"));
  }
  // handoff 同理：没有就是没交给任何人
  if (const JValue* handoff = node->Get("handoff")) {
    view.handoff = handoff->GetString("state");
    view.agent_session = handoff->GetString("agentSessionId");
  }
  out.push_back(std::move(view));

  const JValue* children = node->Get("children");
  if (children != nullptr && children->IsArray()) {
    for (const JValue& child : children->array) {
      if (child.IsString()) WalkNodes(nodes, roots, child.text, out, depth + 1);
    }
  }
}

inline WorkModel ModelFromDoc(const std::string& body) {
  WorkModel model;
  JValue packet;
  if (!ParseJson(body, packet) || !packet.IsObject()) {
    model.note = "状态文档解析失败";
    return model;
  }

  // 端点回的是 {doc: {...}, seq: n}；直接给 doc 也认，省得两个版本都写一遍
  const JValue* doc = packet.Get("doc");
  if (doc == nullptr || !doc->IsObject()) doc = &packet;

  const JValue* nodes = doc->Get("nodes");
  if (nodes == nullptr || !nodes->IsObject()) {
    model.note = "文档里没有 nodes";
    return model;
  }

  const JValue* roots = doc->Get("roots");
  if (roots != nullptr && roots->IsArray()) {
    for (const JValue& root : roots->array) {
      if (root.IsString()) WalkNodes(*nodes, *roots, root.text, model.items, 0);
    }
  }
  // roots 缺失时退化成按 id 排序，至少不会显示成空
  if (model.items.empty()) {
    for (const auto& entry : nodes->object) {
      WalkNodes(*nodes, *roots, entry.first, model.items, 0);
    }
  }

  model.doc_title = Widen(doc->GetString("title"));
  if (model.doc_title.empty()) model.doc_title = L"未命名工作区";
  const std::string pointer = doc->GetString("pointer");
  model.total = static_cast<int>(model.items.size());
  for (size_t i = 0; i < model.items.size(); ++i) {
    if (model.items[i].status == "done") ++model.done;
    if (!pointer.empty() && model.items[i].id == pointer) model.current = static_cast<int>(i);
  }
  // 小条只显示"当前这一件事"，全完成或空文档时给一句人话
  if (model.current >= 0) {
    model.title = model.items[static_cast<size_t>(model.current)].title;
  } else {
    model.title = model.total > 0 ? L"全部已完成" : L"还没有任务";
  }
  model.state = WorkModel::State::Online;
  return model;
}

}  // namespace dshwork
