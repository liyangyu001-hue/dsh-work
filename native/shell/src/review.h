#pragma once
/**
 * 解构审阅窗：模型拆出来的计划先在这里给人看，满意了才落库。
 *
 * 为什么要有这一步：解构原来是一把梭 —— 模型给出七八个节点就直接进文档，
 * 人只能接受或者重来。审阅窗把「拆」变成可以商量的事：哪颗看懂了就过，
 * 哪颗太粗就当场再拆一层，全部满意再按 Enter。
 *
 * 形态取舍：做成**带缩进的竖向流程树**而不是自由布局节点图。
 *   - 我们要表达的是先后顺序，不是依赖关系，没有需要汇合的边；
 *   - 任意深度都能读（二次、三次解构会越来越深），自由布局越深越乱；
 *   - 每行等高，键盘上下选和鼠标命中都是 O(1)。
 * 左侧的树线 + 每行那颗状态点，始终在告诉人「你走到第几步了」。
 */

#include <string>
#include <vector>
#include <unordered_map>

#include "doc.h"
#include "panel.h"

namespace dshwork::review {

/** 扁平化之后的一行：树的结构已经折进 depth，绘制只认这一层。 */
struct Row {
  std::string id;  // 草稿里的节点 id：二次解构与确认都要用它
  int depth = 0;
  bool has_children = false;
  bool expanded = false;
  int child_count = 0;
  std::string status;  // 'todo' | 'active' | 'done' | 'skipped'
  std::wstring title;
  std::wstring detail;
  std::wstring acceptance;
};

struct Model {
  std::wstring title;
  std::vector<Row> rows;
  int selected = 0;
  int total = 0;
  int scroll = 0;  // 第一行可见的索引；窗口只露 kMaxRowsVisible 行
  bool busy = false;
  std::wstring busy_text;
  // 每个行 id 第一次出现的时间：用来做「新行逐条淡入」，已存在的老行不会重播。
  std::unordered_map<std::string, ULONGLONG> appear;
};

/** 一帧的动画状态。所有值都由 main.cpp 每帧算好，这里只负责画。 */
struct Anim {
  float open = 1.0f;         // 窗口打开进度 0..1
  float highlight_y = -1.0f; // 选中块的 y（body 顶部起算，DIP）；<0 表示还没初始化
  ULONGLONG now = 0;         // 本帧时间，用来算每行 reveal
};

constexpr float kHeader = 56.0f;
constexpr float kFooter = 46.0f;
constexpr float kRowH = 48.0f;
constexpr float kIndentStep = 22.0f;
constexpr float kPad = 24.0f;
constexpr float kRightW = 250.0f;  // 右侧详情列
constexpr float kMinW = 740.0f;
constexpr float kMaxW = 1120.0f;
constexpr float kMaxRowsVisible = 9.0f;
constexpr float kCommitW = 96.0f;
constexpr float kCommitH = 30.0f;

inline float VisibleRows(const Model& model) {
  const float rows = static_cast<float>(model.rows.size());
  if (rows < 1.0f) return 1.0f;
  return rows > kMaxRowsVisible ? kMaxRowsVisible : rows;
}

inline float BodyHeight(const Model& model) { return VisibleRows(model) * kRowH; }

/** 宽度按最长的一行估：中文 1 字 ≈ 字号，英文约一半，宁可宽一点也不要折行。 */
inline float WindowWidth(const Model& model) {
  float widest = kMinW;
  for (const Row& row : model.rows) {
    float text = 0.0f;
    for (const wchar_t ch : row.title) text += (ch < 0x2E80 ? 7.0f : 13.5f);
    const float estimate = kPad * 2 + kRightW + 78.0f + text + static_cast<float>(row.depth) * kIndentStep;
    if (estimate > widest) widest = estimate;
  }
  return widest > kMaxW ? kMaxW : widest;
}

inline float WindowHeight(const Model& model) { return kHeader + BodyHeight(model) + kFooter; }

/** 命中：点到第几行（-1 = 没点到行）。 */
inline int RowAt(const Model& model, float x, float y, float scale) {
  const float top = kHeader * scale;
  const float bottom = top + BodyHeight(model) * scale;
  if (y < top || y >= bottom) return -1;
  if (x < kPad * scale) return -1;
  const int index = model.scroll + static_cast<int>((y - top) / (kRowH * scale));
  return index >= 0 && index < static_cast<int>(model.rows.size()) ? index : -1;
}

inline D2D1_RECT_F CommitRectAt(float width_dip, float height_dip, float scale) {
  const float w = kCommitW * scale;
  const float h = kCommitH * scale;
  const float right = (width_dip - kPad) * scale;
  const float bottom = height_dip * scale - ((kFooter - kCommitH) * 0.5f) * scale;
  return D2D1::RectF(right - w, bottom - h, right, bottom);
}

inline bool CommitHit(const Model& model, float x, float y, float scale) {
  const D2D1_RECT_F r = CommitRectAt(WindowWidth(model), WindowHeight(model), scale);
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

/** 缓出：开头快、收尾慢，用于行的淡入。 */
inline float EaseOut(float t) {
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

/**
 * 状态点颜色：做完是绿的、正在做是强调蓝且稍大、跳过是暗灰、待办是浅灰。
 *
 * 之前只有一种灰，一屏树看下去分不出"这行到底做没做"——颜色就是最省字的标注。
 */
inline UINT32 StatusInk(const std::string& status, bool child) {
  if (status == "active") return ui::kAccent;
  if (status == "done") return 0xFF30D158;
  if (status == "skipped") return 0xFF5C5C63;
  return child ? 0xFF8A8A92 : 0xFFB9B9BE;
}

inline void Draw(ID2D1RenderTarget* target, const Model& model, const PanelFormats& fmt, float scale,
                 float width_dip, float height_dip, float alpha, const Anim& anim) {
  using namespace ui;
  const float w = width_dip * scale;
  const float h = height_dip * scale;
  // 打开时整体从下方 10 DIP 浮上来；alpha 也跟着走，避免"砰"地出现。
  const float open = anim.open < 0.0f ? 0.0f : (anim.open > 1.0f ? 1.0f : anim.open);
  const float body_dy = (1.0f - open) * 10.0f * scale;
  alpha *= open;
  if (alpha <= 0.0f) return;

  // 底：近乎不透明的深色压住桌面，顶边一道发丝高光。
  FillRound(target, fmt.brush, D2D1::RectF(0, 0, w, h), 22.0f * scale, Fade(0xFF14161C, alpha));
  StrokeRound(target, fmt.brush, D2D1::RectF(0.5f, 0.5f, w - 1.0f, h - 1.0f), 21.0f * scale, Fade(0x22FFFFFF, alpha),
              1.0f);

  // 头部
  const std::wstring head = model.title.empty() ? L"解构结果" : model.title;
  Text(target, FitText(head, width_dip - kPad * 2 - 90.0f, 14.0f), fmt.title,
       D2D1::RectF(kPad * scale, 14.0f * scale, w - kPad * scale - 80.0f * scale, 42.0f * scale), fmt.brush,
       Fade(kText, alpha));
  const std::wstring count = std::to_wstring(model.total) + L" 件事";
  Text(target, count, fmt.meta,
       D2D1::RectF(w - kPad * scale - 78.0f * scale, 14.0f * scale, w - kPad * scale, 42.0f * scale), fmt.brush,
       Fade(kTextDim, alpha));

  FillRound(target, fmt.brush, D2D1::RectF(kPad * scale, kHeader * scale - 0.5f, w - kPad * scale, kHeader * scale + 0.5f),
            0.5f, Fade(0x1AFFFFFF, alpha));

  // 行（只画可见的那一段，scroll 是窗口顶端对应的索引）
  const int first = model.scroll < 0 ? 0 : model.scroll;
  const int last = static_cast<int>(model.rows.size()) < first + static_cast<int>(kMaxRowsVisible)
                       ? static_cast<int>(model.rows.size())
                       : first + static_cast<int>(kMaxRowsVisible);

  // 选中块单独画在最底层：它会在行之间滑动，不能被行的淡入淡出切碎。
  {
    const float target_y = (kHeader + static_cast<float>(model.selected - first) * kRowH + 3.0f) * scale;
    const float hl_y = anim.highlight_y >= 0.0f ? (kHeader + anim.highlight_y + 3.0f) * scale : target_y;
    if (model.selected >= first && model.selected < last) {
      FillRound(target, fmt.brush,
                D2D1::RectF(kPad * scale, hl_y + body_dy, w - (kPad + kRightW) * scale, hl_y + kRowH * scale - 3.0f * scale + body_dy),
                10.0f * scale, Fade(0x1F0A84FF, alpha));
      FillRound(target, fmt.brush,
                D2D1::RectF(kPad * scale, hl_y + 12.0f * scale + body_dy, kPad * scale + 3.0f * scale,
                            hl_y + kRowH * scale - 12.0f * scale + body_dy),
                1.5f * scale, Fade(kAccent, alpha));
    }
  }

  for (int i = first; i < last; ++i) {
    const Row& row = model.rows[static_cast<size_t>(i)];
    const float row_h = kRowH * scale;

    // 每行自己的出现进度：新行从 0 淡入并右移 8 DIP，老行直接是 1。
    float reveal = 1.0f;
    const auto found = model.appear.find(row.id);
    if (found != model.appear.end() && anim.now > found->second) {
      const float k = static_cast<float>(anim.now - found->second) / 170.0f;
      reveal = k >= 1.0f ? 1.0f : (k <= 0.0f ? 0.0f : k);
      reveal = review::EaseOut(reveal);
    }
    if (reveal <= 0.001f) continue;
    const float row_alpha = alpha * reveal;
    const float slide = (1.0f - reveal) * -8.0f * scale;

    const float top = (kHeader + static_cast<float>(i - first) * kRowH) * scale + body_dy + slide;
    const bool picked = i == model.selected;
    const float indent = (kPad + static_cast<float>(row.depth) * kIndentStep) * scale;

    // 树线：竖线 + 到底部直角折向右边，像真正的一棵树，而不是几条孤立竖线。
    if (row.depth > 0) {
      const float lx = (kPad + static_cast<float>(row.depth) * kIndentStep - 12.0f) * scale;
      const bool last_sibling = (i + 1 >= last) || (model.rows[static_cast<size_t>(i + 1)].depth < row.depth);
      const float bottom = last_sibling ? top + row_h * 0.5f : top + row_h;
      FillRound(target, fmt.brush, D2D1::RectF(lx, top, lx + 1.0f, bottom), 0.5f, Fade(0x1FFFFFFF, row_alpha));
      if (last_sibling) {
        FillRound(target, fmt.brush, D2D1::RectF(lx, top + row_h * 0.5f - 0.5f, lx + 10.0f * scale, top + row_h * 0.5f + 0.5f),
                  0.5f, Fade(0x1FFFFFFF, row_alpha));
      }
    }

    const UINT32 ink = StatusInk(row.status, row.depth > 0);
    const float cx = indent + 6.0f * scale;
    const float cy = top + row_h * 0.5f;
    const float dr = 3.6f * scale;
    FillRound(target, fmt.brush, D2D1::RectF(cx - dr, cy - dr, cx + dr, cy + dr), dr, Fade(ink, row_alpha));

    // 拆过的行右边有个小胶囊：收起是 +N，展开是 −N（颜色也跟着变）。
    const float badge_w = row.has_children ? 34.0f * scale : 0.0f;
    const float text_left = indent + 18.0f * scale;
    const float text_right = w - kPad * scale - kRightW * scale - badge_w;
    Text(target, FitText(row.title, (text_right - text_left) / scale, picked ? 13.5f : 13.0f),
         picked ? fmt.title : fmt.body, D2D1::RectF(text_left, top + 12.0f * scale, text_right, top + row_h - 8.0f * scale),
         fmt.brush, Fade(picked ? kText : (row.depth > 0 ? 0xFFC8C8CE : 0xFFE2E2E6), row_alpha));

    if (row.has_children) {
      const std::wstring badge = (row.expanded ? L"−" : L"+") + std::to_wstring(row.child_count);
      const float bx = w - kPad * scale - kRightW * scale - badge_w + 4.0f * scale;
      FillRound(target, fmt.brush, D2D1::RectF(bx, cy - 9.0f * scale, bx + 28.0f * scale, cy + 9.0f * scale),
                9.0f * scale, Fade(row.expanded ? 0x260A84FF : 0x14FFFFFF, row_alpha));
      Text(target, badge, fmt.meta, D2D1::RectF(bx + 6.0f * scale, cy - 8.0f * scale, bx + 28.0f * scale, cy + 8.0f * scale),
           fmt.brush, Fade(row.expanded ? kAccent : kTextDim, row_alpha));
    }
  }

  // 滚动条：只在装不下的时候出现。没有它，用户不知道下面还有东西。
  {
    const int total = static_cast<int>(model.rows.size());
    const int visible = static_cast<int>(kMaxRowsVisible);
    if (total > visible) {
      // 放在标题列右缘、右列分隔线之前：滚动条在那儿最不抢戏，也不会挡到缩进线。
      const float track_x = w - (kPad + kRightW + 9.0f) * scale;
      const float top = kHeader * scale + 6.0f * scale;
      const float bottom = (kHeader + static_cast<float>(visible) * kRowH) * scale - 6.0f * scale;
      const float track_h = bottom - top;
      FillRound(target, fmt.brush, D2D1::RectF(track_x, top, track_x + 2.5f * scale, bottom), 1.25f * scale,
                Fade(0x14FFFFFF, alpha));
      const float ratio = static_cast<float>(visible) / static_cast<float>(total);
      const float thumb_h = track_h * ratio < 18.0f * scale ? 18.0f * scale : track_h * ratio;
      const float max_scroll = static_cast<float>(total - visible);
      const float t = max_scroll > 0.0f ? static_cast<float>(model.scroll) / max_scroll : 0.0f;
      const float thumb_top = top + (track_h - thumb_h) * t;
      FillRound(target, fmt.brush, D2D1::RectF(track_x, thumb_top, track_x + 2.5f * scale, thumb_top + thumb_h),
                1.25f * scale, Fade(0x55FFFFFF, alpha));
    }
  }

  // 右列：指到那一颗的目标与验收
  {
    const float left = w - kPad * scale - kRightW * scale;
    // 右列自己有底色，才像一块独立的详情面板，而不是飘在行标题右边。
    FillRound(target, fmt.brush, D2D1::RectF(left, kHeader * scale, w - kPad * scale, h - kFooter * scale),
              10.0f * scale, Fade(0x0DFFFFFF, alpha));
    FillRound(target, fmt.brush, D2D1::RectF(left - 1.0f, kHeader * scale, left, h - kFooter * scale), 0.5f,
              Fade(0x14FFFFFF, alpha));
    if (model.selected >= 0 && model.selected < static_cast<int>(model.rows.size())) {
      const Row& row = model.rows[static_cast<size_t>(model.selected)];
      const float tx = left + 18.0f * scale;
      const float tw = (kRightW - 36.0f) * scale;
      float y = (kHeader + 16.0f) * scale;
      Text(target, L"目标", fmt.meta, D2D1::RectF(tx, y, tx + tw, y + 18.0f * scale), fmt.brush, Fade(kTextDim, alpha));
      y += 20.0f * scale;
      Text(target, row.detail.empty() ? L"—" : row.detail, fmt.body,
           D2D1::RectF(tx, y, tx + tw, y + 52.0f * scale), fmt.brush, Fade(0xFFE4E4E8, alpha));
      y += 64.0f * scale;
      Text(target, L"验收", fmt.meta, D2D1::RectF(tx, y, tx + tw, y + 18.0f * scale), fmt.brush, Fade(kTextDim, alpha));
      y += 20.0f * scale;
      Text(target, row.acceptance.empty() ? L"—" : row.acceptance, fmt.body,
           D2D1::RectF(tx, y, tx + tw, y + 52.0f * scale), fmt.brush, Fade(0xFFE4E4E8, alpha));
    }
  }

  // 页脚：键位提示 + 「就这样」
  {
    const float top = h - kFooter * scale;
    FillRound(target, fmt.brush, D2D1::RectF(kPad * scale, top, w - kPad * scale, top + 1.0f), 0.5f,
              Fade(0x1AFFFFFF, alpha));

    // 先算按钮矩形，再让左边的提示文字与它同一套基线——
    // 之前两边各自"看着差不多"给 y，结果字比按钮低了一截。
    const D2D1_RECT_F cr = CommitRectAt(width_dip, height_dip, scale);
    const float text_top = cr.top + 5.0f * scale;
    const float text_bottom = cr.top + 21.0f * scale;
    const std::wstring hint =
        model.busy ? model.busy_text : L"↑↓ 选 · Enter 再拆一层 · Ctrl+Enter 就这样 · Esc 不要了";
    Text(target, FitText(hint, width_dip - kPad * 2 - kCommitW - 20.0f, 11.5f), fmt.meta,
         D2D1::RectF(kPad * scale, text_top, cr.left - 16.0f * scale, text_bottom), fmt.brush,
         Fade(model.busy ? kAccent : kTextDim, alpha));

    FillRound(target, fmt.brush, cr, 15.0f * scale, Fade(model.busy ? 0x33FFFFFF : kAccent, alpha));
    // 居中交给 DWrite 算：不同缩放下字宽会变，手调偏移一定偏。
    TextCentered(target, L"就这样", fmt.body,
                 D2D1::RectF(cr.left, cr.top, cr.right, cr.bottom), fmt.brush, Fade(0xFFFFFFFF, alpha));
  }
}

}  // namespace dshwork::review
