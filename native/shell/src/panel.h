// 面板的绘制
//
// 块状设计：头部一行（状态点 / 文档标题 / 进度 / 关闭），左边一张卡带，
// 右边是"当前这件事的目标与验收"，底部一行键位。所有尺寸都以 DIP 记，
// 绘制前统一乘 g_dpiScale —— 150% 缩放下手算像素是 bug 的温床。
//
// 这里只画，不知道任何输入。键盘在 main.cpp 里由低级钩子收走，
// 面板窗口本身是 WS_EX_NOACTIVATE（不抢焦点），所以它永远收不到 WM_KEYDOWN。

#pragma once

#include <string>

#include "doc.h"

namespace dshwork {

struct PanelFormats {
  IDWriteTextFormat* title = nullptr;   // 文档标题
  IDWriteTextFormat* body = nullptr;    // 正文（会自动换行）
  IDWriteTextFormat* meta = nullptr;    // 标签、进度、页脚
  ID2D1SolidColorBrush* brush = nullptr;
};

namespace ui {

constexpr float kPanelWidth = 760.0f;
constexpr float kPanelHeight = 396.0f;
constexpr float kPad = 20.0f;
constexpr float kHeaderH = 50.0f;
constexpr float kFooterH = 44.0f;
constexpr float kCardW = 132.0f;
constexpr float kCardH = 76.0f;
constexpr float kCardGap = 10.0f;
constexpr float kRadius = 14.0f;

constexpr UINT32 kText = 0xFFF5F5F7;
constexpr UINT32 kTextDim = 0xFF9A9AA0;
constexpr UINT32 kAccent = 0xFF0A84FF;
constexpr UINT32 kSurface = 0xFF1C1C20;

inline D2D1_COLOR_F Color(UINT32 argb, float alpha = -1.0f) {
  return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(argb & 0xFF) / 255.0f,
                      alpha < 0.0f ? static_cast<float>((argb >> 24) & 0xFF) / 255.0f : alpha);
}

inline void FillRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_RECT_F rect,
                      float radius, UINT32 color) {
  brush->SetColor(Color(color));
  target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

inline void StrokeRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_RECT_F rect,
                        float radius, UINT32 color) {
  brush->SetColor(Color(color));
  target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, 0.5f);
}

inline void Text(ID2D1RenderTarget* target, const std::wstring& text, IDWriteTextFormat* format,
                 D2D1_RECT_F rect, ID2D1SolidColorBrush* brush, UINT32 color) {
  brush->SetColor(Color(color));
  target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

/** 卡片宽度随焦点变化：当前卡略大，邻卡略小——视线自然落在中间。 */
inline D2D1_RECT_F CardRect(float center_x, float center_y, bool current, float scale) {
  const float w = (current ? kCardW + 14.0f : kCardW) * scale;
  const float h = (current ? kCardH + 8.0f : kCardH) * scale;
  return D2D1::RectF(center_x - w / 2.0f, center_y - h / 2.0f, center_x + w / 2.0f,
                     center_y + h / 2.0f);
}

inline void DrawPanel(ID2D1RenderTarget* target, const WorkModel& model, int focus, float scale,
                      const PanelFormats& fmt) {
  const float w = kPanelWidth * scale;
  const float h = kPanelHeight * scale;

  // 面板下面的阴影：分层叠出柔边（D2D 没有高斯模糊）
  for (int i = 6; i >= 1; --i) {
    const float spread = static_cast<float>(i) * 2.2f * scale;
    fmt.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.02f));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(spread, spread * 1.4f, w - spread, h - spread * 0.6f), 22.0f * scale,
                          22.0f * scale),
        fmt.brush);
  }

  const D2D1_RECT_F panel = D2D1::RectF(0.0f, 0.0f, w, h);
  FillRound(target, fmt.brush, panel, 22.0f * scale, 0xFA1A1A1E);
  StrokeRound(target, fmt.brush, D2D1::RectF(0.25f, 0.25f, w - 0.25f, h - 0.25f), 22.0f * scale,
              0x1AFFFFFF);

  // ── 头部 ─────────────────────────────────────────────
  const float head_cy = (kPad + kHeaderH / 2.0f) * scale;
  fmt.brush->SetColor(Color(model.state == WorkModel::State::Online ? kAccent : 0xFFFF453A));
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPad * scale, head_cy), 4.5f * scale, 4.5f * scale),
                      fmt.brush);

  const float title_x = (kPad + 16.0f) * scale;
  Text(target, model.doc_title.empty() ? L"未命名工作区" : model.doc_title, fmt.title,
       D2D1::RectF(title_x, (kPad - 2.0f) * scale, w - 130.0f * scale, (kPad + 34.0f) * scale),
       fmt.brush, kText);

  if (model.total > 0) {
    const std::wstring progress = std::to_wstring(model.done) + L"/" + std::to_wstring(model.total);
    Text(target, progress, fmt.meta,
         D2D1::RectF(w - 118.0f * scale, head_cy - 12.0f * scale, w - 62.0f * scale, head_cy + 12.0f * scale),
         fmt.brush, kTextDim);
  }
  Text(target, L"×", fmt.title,
       D2D1::RectF(w - 48.0f * scale, head_cy - 16.0f * scale, w - 18.0f * scale, head_cy + 16.0f * scale),
       fmt.brush, kTextDim);

  FillRound(target, fmt.brush, D2D1::RectF(kPad * scale, (kPad + kHeaderH) * scale, w - kPad * scale,
                                           (kPad + kHeaderH + 0.5f) * scale),
            0.0f, 0x14FFFFFF);

  // ── 主体 ─────────────────────────────────────────────
  const float body_top = (kPad + kHeaderH + 14.0f) * scale;
  const float body_bottom = h - (kPad + kFooterH) * scale;
  const float split = w * 0.58f;
  const float card_cy = (body_top + body_bottom) / 2.0f;

  int center = model.current >= 0 ? model.current : 0;
  if (model.items.empty()) {
    Text(target, L"还没有任务", fmt.title,
         D2D1::RectF(kPad * scale, card_cy - 40.0f * scale, split, card_cy - 4.0f * scale), fmt.brush,
         kTextDim);
    Text(target, L"按 Tab 框选屏幕上的活，或者让我帮你拆", fmt.meta,
         D2D1::RectF(kPad * scale, card_cy, split, card_cy + 40.0f * scale), fmt.brush, kTextDim);
  } else {
    const int count = static_cast<int>(model.items.size());
    const int center = focus >= 0 && focus < count ? focus : (model.current >= 0 ? model.current : 0);
    // 卡片带只在自己的列里画：相邻卡露半张是设计，但绝不能压进右边的详情栏
    target->PushAxisAlignedClip(
        D2D1::RectF(kPad * scale, body_top - 8.0f * scale, split - 8.0f * scale, body_bottom + 8.0f * scale),
        D2D1_ANTIALIAS_MODE_ALIASED);
    // 只画光标附近三张：再远的看不见，画了只是浪费
    for (int offset = -1; offset <= 1; ++offset) {
      const int index = center + offset;
      if (index < 0 || index >= count) continue;
      const NodeView& node = model.items[static_cast<size_t>(index)];
      const bool selected = index == center;
      // split 已经是像素，别再乘 scale —— 之前这里乘了两次，卡片被推到右边缘
      const float cx = split / 2.0f + static_cast<float>(offset) * (kCardW + kCardGap) * scale;
      const D2D1_RECT_F card = CardRect(cx, card_cy, selected, scale);

      FillRound(target, fmt.brush, card, kRadius * scale, selected ? 0x2EFFFFFF : 0x10FFFFFF);
      // 选中框跟着光标走，和"进行中"这个状态分开：状态说的是任务，框说的是你现在指着谁
      if (selected) StrokeRound(target, fmt.brush, card, kRadius * scale, kAccent);

      const D2D1_RECT_F pad_rect = D2D1::RectF(card.left + 12.0f * scale, card.top + 10.0f * scale,
                                               card.right - 12.0f * scale, card.top + 30.0f * scale);
      const wchar_t* label = L"待进行";
      UINT32 label_color = kTextDim;
      if (node.status == "done") {
        label = L"已完成";
      } else if (node.status == "active") {
        label = L"进行中";
        label_color = kAccent;
      } else if (node.status == "skipped") {
        label = L"已跳过";
      }
      Text(target, label, fmt.meta, pad_rect, fmt.brush, label_color);

      const D2D1_RECT_F title_rect = D2D1::RectF(card.left + 12.0f * scale, card.top + 30.0f * scale,
                                                 card.right - 12.0f * scale, card.bottom - 8.0f * scale);
      Text(target, node.title, fmt.body, title_rect, fmt.brush,
           node.status == "done" ? kTextDim : kText);
    }
    target->PopAxisAlignedClip();
  }

  // 竖向分隔线
  FillRound(target, fmt.brush,
            D2D1::RectF(split, body_top, split + 0.5f * scale, body_bottom), 0.0f, 0x14FFFFFF);

  // ── 右侧详情 ──────────────────────────────────────────
  const int detail_index = center >= 0 && center < static_cast<int>(model.items.size()) ? center : -1;
  const float dx = split + 18.0f * scale;
  if (detail_index >= 0) {
    const NodeView& node = model.items[static_cast<size_t>(detail_index)];
    float y = body_top + 2.0f * scale;
    const std::wstring detail = node.detail.empty() ? L"（还没写目标）" : node.detail;
    const std::wstring acceptance = node.acceptance.empty() ? L"（还没写验收）" : node.acceptance;

    Text(target, L"目标", fmt.meta, D2D1::RectF(dx, y, w - kPad * scale, y + 18.0f * scale), fmt.brush,
         kTextDim);
    y += 22.0f * scale;
    const float column_dip = (w - kPad * scale - dx) / scale;
    // 中文字宽约等于字号，按字符数估行数就够了；要精确得用 IDWriteTextLayout，
    // 但那点误差不值得给每个标签建布局对象
    int lines = static_cast<int>(detail.size() / (column_dip / 13.0f)) + 1;
    if (lines > 4) lines = 4;
    Text(target, detail, fmt.body, D2D1::RectF(dx, y, w - kPad * scale, y + lines * 20.0f * scale), fmt.brush,
         kText);
    y += (lines * 20.0f + 18.0f) * scale;

    Text(target, L"验收", fmt.meta, D2D1::RectF(dx, y, w - kPad * scale, y + 18.0f * scale), fmt.brush,
         kTextDim);
    y += 20.0f * scale;
    Text(target, acceptance, fmt.body, D2D1::RectF(dx, y, w - kPad * scale, body_bottom), fmt.brush,
         kText);
  }

  // ── 页脚 ─────────────────────────────────────────────
  const float foot_y = h - (kPad + kFooterH) * scale + 8.0f * scale;
  const std::wstring keys = L"Tab 框选    Enter 完成    Backspace 上一个    Esc 收起";
  Text(target, keys, fmt.meta, D2D1::RectF(kPad * scale, foot_y, w - 120.0f * scale, foot_y + 24.0f * scale),
       fmt.brush, kTextDim);

  const std::wstring status = model.state == WorkModel::State::Online ? L"已连接" : L"未连接";
  Text(target, status, fmt.meta,
       D2D1::RectF(w - 110.0f * scale, foot_y, w - kPad * scale, foot_y + 24.0f * scale), fmt.brush,
       model.state == WorkModel::State::Online ? kTextDim : 0xFFFF453A);
}

}  // namespace ui
}  // namespace dshwork
