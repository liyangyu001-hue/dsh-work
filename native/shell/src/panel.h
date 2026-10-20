// 面板的绘制
//
// 块状设计：头部一行（状态点 / 文档标题 / 进度 / 关闭），左边一张卡带，
// 右边是"当前这件事的目标与验收"，底部一行键位。所有尺寸都以 DIP 记，
// 绘制前统一乘 g_dpiScale —— 150% 缩放下手算像素是 bug 的温床。
//
// 这里只画，不知道任何输入。键盘在 main.cpp 里由低级钩子收走，
// 面板窗口本身是 WS_EX_NOACTIVATE（不抢焦点），所以它永远收不到 WM_KEYDOWN。

#pragma once

#include <algorithm>
#include <cmath>
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

constexpr float kPanelWidth = 660.0f;
constexpr float kPanelHeight = 318.0f;
constexpr float kPad = 18.0f;
constexpr float kHeaderH = 46.0f;
constexpr float kFooterH = 40.0f;
constexpr float kCardW = 228.0f;
constexpr float kCardH = 92.0f;
// 卡片是叠起来的：层与层之间只露出这么多，不是并排
constexpr float kStackStep = 26.0f;
constexpr int kStackDepth = 3;
constexpr float kRadius = 12.0f;

constexpr UINT32 kText = 0xFFF5F5F7;
constexpr UINT32 kTextDim = 0xFF9A9AA0;
constexpr UINT32 kAccent = 0xFF0A84FF;
constexpr UINT32 kSurface = 0xFF1C1C20;

/** 按比例压暗一个颜色（含 alpha）——完成的任务靠这个往前退。 */
inline UINT32 Fade(UINT32 argb, float factor) {
  const UINT32 alpha = static_cast<UINT32>(static_cast<float>((argb >> 24) & 0xFF) * factor);
  return (alpha << 24) | (argb & 0x00FFFFFFu);
}

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
  for (int i = 5; i >= 1; --i) {
    const float spread = static_cast<float>(i) * 1.6f * scale;
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
  const float split = w * 0.62f;
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
    const int current = model.current >= 0 ? model.current : 0;
    const int focus_index = focus >= 0 && focus < count ? focus : current;
    const float cx = split / 2.0f;

    target->PushAxisAlignedClip(
        D2D1::RectF(kPad * scale, body_top - 10.0f * scale, split - 10.0f * scale, body_bottom + 10.0f * scale),
        D2D1_ANTIALIAS_MODE_ALIASED);

    // 一叠卡片：当前这张在最上面、百分百显示；
    // 左右两侧是叠在它下面的同尺寸卡，只露出一条边，而且越远越淡。
    // 这样眼睛只会被"现在这件事"占住，两侧仍然提示"前面做过什么、后面还有什么"。
    for (int step = kStackDepth; step >= 1; --step) {
      for (int side = -1; side <= 1; side += 2) {
        const int index = current + side * step;
        if (index < 0 || index >= count) continue;
        const D2D1_RECT_F card = D2D1::RectF(cx - (kCardW / 2.0f) * scale + static_cast<float>(side * step) * kStackStep * scale,
                                             card_cy - (kCardH / 2.0f) * scale,
                                             cx + (kCardW / 2.0f) * scale + static_cast<float>(side * step) * kStackStep * scale,
                                             card_cy + (kCardH / 2.0f) * scale);
        const float fade = 0.30f - 0.05f * static_cast<float>(step - 1);
        FillRound(target, fmt.brush, card, kRadius * scale, Fade(0x12FFFFFF, fade));
      }
    }

    // 最上面这张：当前任务
    const D2D1_RECT_F card = D2D1::RectF(cx - (kCardW / 2.0f) * scale, card_cy - (kCardH / 2.0f) * scale,
                                         cx + (kCardW / 2.0f) * scale, card_cy + (kCardH / 2.0f) * scale);
    FillRound(target, fmt.brush, card, kRadius * scale, 0x24FFFFFF);
    StrokeRound(target, fmt.brush, card, kRadius * scale, kAccent);

    const NodeView& node = model.items[static_cast<size_t>(current)];
    Text(target, L"进行中", fmt.meta,
         D2D1::RectF(card.left + 14.0f * scale, card.top + 10.0f * scale, card.right - 14.0f * scale,
                     card.top + 30.0f * scale),
         fmt.brush, kAccent);
    Text(target, node.title, fmt.title,
         D2D1::RectF(card.left + 14.0f * scale, card.top + 32.0f * scale, card.right - 14.0f * scale,
                     card.bottom - 10.0f * scale),
         fmt.brush, kText);

    // 光标落在侧边卡上时，在它露出的那条边上加一点高亮，提示"你现在指着它"
    if (focus_index != current) {
      const int side = focus_index > current ? 1 : -1;
      const int distance = focus_index > current ? focus_index - current : current - focus_index;
      const int step = (distance < kStackDepth) ? distance : kStackDepth;
      const int index = current + side * step;
      if (index >= 0 && index < count) {
        const float edge = cx + static_cast<float>(side) * (kCardW / 2.0f) * scale +
                           static_cast<float>(side * step) * kStackStep * scale;
        const float x = side > 0 ? edge - 3.0f * scale : edge;
        FillRound(target, fmt.brush,
                  D2D1::RectF(x, card_cy - 18.0f * scale, x + 3.0f * scale, card_cy + 18.0f * scale), 1.5f * scale,
                  kAccent);
      }
    }
    target->PopAxisAlignedClip();
  }
  // 竖向分隔线
  FillRound(target, fmt.brush,
            D2D1::RectF(split, body_top, split + 0.5f * scale, body_bottom), 0.0f, 0x14FFFFFF);

  // ── 右侧详情 ──────────────────────────────────────────
  const int detail_index = focus >= 0 && focus < static_cast<int>(model.items.size())
                             ? focus
                             : (model.current >= 0 && model.current < static_cast<int>(model.items.size()) ? model.current : -1);
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
