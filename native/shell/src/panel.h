// 面板：一条横贯屏幕顶部的长条。
//
// 为什么不放一个方盒子：这个面板是"随时瞟一眼"的东西，不是工作窗口。
// 方盒子会压住正在看的内容，而长条只占顶部一条带，视线不用离开原来的位置。
//
// 参考 BrimDeck（Windows 灵动岛）：收起是胶囊，展开是一条与其同中心的宽面板，
// 展开过程是同一个表面在变宽变高，不是两个窗口替换。它的收起态也是一排
// 圆环而不是一排卡片——这里照搬这个判断：
//
// **任务用点表示，不用卡片。**
// 卡片有边、有底、有文字，每多一张就多一种"细节要对齐"的负担；
// 而这一层想说的只有"现在在第几件、前后还有几件"。点没有边距、没有字号、
// 没有对齐问题，天然统一。文字只在需要的时候出现一次：鼠标指到哪个点，
// 右边就写哪个任务。
#pragma once

#include <algorithm>
#include <string>

#include "doc.h"

namespace dshwork {

struct PanelFormats {
  IDWriteTextFormat* title = nullptr;  // 任务标题 13px
  IDWriteTextFormat* body = nullptr;   // 详情 12.5px
  IDWriteTextFormat* meta = nullptr;   // 标签/计数 10.5px
  ID2D1SolidColorBrush* brush = nullptr;
};

namespace ui {

// ── 尺寸（DIP）────────────────────────────────────────────
constexpr float kStripHeight = 132.0f;
constexpr float kPadX = 22.0f;
constexpr float kLeftBlock = 210.0f;    // 左侧：工作区标题 + 进度
constexpr float kDetailBlock = 430.0f;  // 右侧：当前/指到的那件事
constexpr float kRightBlock = 92.0f;    // 最右：连接状态与键位
constexpr float kGapLeft = 20.0f;
constexpr float kGapRight = 18.0f;

constexpr float kDotStep = 26.0f;      // 点与点的间距
constexpr float kDotRadius = 4.5f;     // 点的大小
constexpr float kDotRadiusNow = 5.5f;  // 当前任务那颗点稍大

constexpr float kMinWidth = 940.0f;
constexpr float kMaxWidth = 1560.0f;

constexpr UINT32 kText = 0xFFF5F5F7;
constexpr UINT32 kTextDim = 0xFF9A9AA0;
constexpr UINT32 kAccent = 0xFF0A84FF;
constexpr UINT32 kWarn = 0xFFFF453A;

/** 这条长条能排下几颗点。任务更多时只画当前任务附近的一段。 */
inline int VisibleDots(float dip_w) {
  const float usable =
      dip_w - (kPadX * 2 + kLeftBlock + kGapLeft + kGapRight + kDetailBlock + kRightBlock);
  int n = static_cast<int>(usable / kDotStep);
  if (n < 1) n = 1;
  return n;
}

/** 窗口里第一颗点的序号：让当前任务尽量居中。 */
inline int DotStart(int count, int current, int visible) {
  int start = current - visible / 2;
  if (start < 0) start = 0;
  if (start + visible > count) start = count - visible;
  return start < 0 ? 0 : start;
}

/** 长条宽度：把所有点排下，再留出两侧的信息区。 */
inline float StripWidth(int count) {
  const int n = count > 0 ? count : 1;
  const float dots = static_cast<float>(n) * kDotStep;
  const float w = kPadX * 2 + kLeftBlock + kGapLeft + dots + kGapRight + kDetailBlock + kRightBlock;
  if (w < kMinWidth) return kMinWidth;
  if (w > kMaxWidth) return kMaxWidth;
  return w;
}

// ── 小工具 ───────────────────────────────────────────────
inline UINT32 Fade(UINT32 argb, float factor) {
  const float a = static_cast<float>((argb >> 24) & 0xFF) * (factor < 0.0f ? 0.0f : (factor > 1.0f ? 1.0f : factor));
  return (static_cast<UINT32>(a) << 24) | (argb & 0x00FFFFFFu);
}

inline D2D1_COLOR_F Color(UINT32 argb, float alpha = -1.0f) {
  const float a = alpha < 0.0f ? static_cast<float>((argb >> 24) & 0xFF) / 255.0f : alpha;
  return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(argb & 0xFF) / 255.0f, a);
}

inline void FillRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_RECT_F rect, float radius,
                      UINT32 color) {
  brush->SetColor(Color(color));
  target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

inline void StrokeRound(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_RECT_F rect, float radius,
                        UINT32 color, float width = 0.5f) {
  brush->SetColor(Color(color));
  target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, width);
}

inline void Text(ID2D1RenderTarget* target, const std::wstring& text, IDWriteTextFormat* format,
                 D2D1_RECT_F rect, ID2D1SolidColorBrush* brush, UINT32 color) {
  brush->SetColor(Color(color));
  target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

/**
 * 按字符宽度硬截断。
 *
 * 分层窗口里 DWrite 的 SetTrimming 不生效（实测会溢出到界外），
 * 所以宁可自己砍字：中文一个字约等于字号宽度，按格子数估就够用了。
 */
inline std::wstring FitText(const std::wstring& text, float max_dip, float font_dip) {
  const float cells = max_dip / (font_dip * 0.98f);
  const size_t max_chars = static_cast<size_t>(cells < 1.0f ? 1.0f : cells);
  if (text.size() <= max_chars) return text;
  if (max_chars <= 1) return L"…";
  return text.substr(0, max_chars - 1) + L"…";
}

inline const wchar_t* StatusLabel(const std::string& status) {
  if (status == "done") return L"已完成";
  if (status == "skipped") return L"已跳过";
  if (status == "active") return L"进行中";
  return L"待进行";
}

/**
 * 一颗点的透明度：以当前任务为中心向外衰减。
 *
 * 这就是"用透明度区分"的全部规则。当前那颗不透明，邻座还认得出，
 * 再远只剩一个影子——眼睛不会去数它，但余光知道那边还有事。
 */
inline float DotAlpha(int distance, const std::string& status) {
  float a = 1.0f;
  if (distance == 1) {
    a = 0.46f;
  } else if (distance == 2) {
    a = 0.28f;
  } else if (distance >= 3) {
    a = 0.16f;
  }
  // 已经做过的事再淡一档：它只需要"存在过"的证据，不需要再被读。
  if (status == "done" || status == "skipped") a *= 0.72f;
  return a;
}

/**
 * 画整条面板。
 *
 * dip_w / dip_h 是当前的逻辑尺寸——展开动画的每一帧都在变，
 * 所以布局一律按传进来的尺寸算，不读常量，否则中间帧会撕裂。
 * alpha 是内容整体的淡入淡出（表面已经先长出来，内容后跟上来），
 * zoom 是内容从 0.96 长到 1 的缩放（BrimDeck 的展开里也有这一手）。
 */
inline void DrawPanel(ID2D1RenderTarget* target, const WorkModel& model, int focus, float scale,
                      const PanelFormats& fmt, float dip_w, float dip_h, float alpha, float zoom) {
  const float w = dip_w * scale;
  const float h = dip_h * scale;
  // 收起来时高度接近药丸，圆角跟着收到一半高，才是一条胶囊
  const float half = h / 2.0f;
  const float radius = half < 26.0f * scale ? half : 26.0f * scale;

  for (int i = 5; i >= 1; --i) {
    const float spread = static_cast<float>(i) * 1.6f * scale;
    fmt.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.02f));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(spread, spread * 1.4f, w - spread, h - spread * 0.6f), radius, radius),
        fmt.brush);
  }

  const D2D1_RECT_F panel = D2D1::RectF(0.0f, 0.0f, w, h);
  FillRound(target, fmt.brush, panel, radius, 0xFA1A1A1E);
  StrokeRound(target, fmt.brush, D2D1::RectF(0.25f, 0.25f, w - 0.25f, h - 0.25f), radius, 0x1AFFFFFF);

  if (alpha <= 0.01f) return;
  // 太窄的时候还摆不下内容：等表面长开再画，避免文字在中间帧里挤成一团
  if (dip_w < kMinWidth * 0.80f || dip_h < kStripHeight * 0.72f) return;

  // 内容整体缩放：围绕中心从 0.96 长到 1
  const D2D1_POINT_2F center = D2D1::Point2F(w / 2.0f, h / 2.0f);
  target->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, center));

  const UINT32 text = Fade(kText, alpha);
  const UINT32 dim = Fade(kTextDim, alpha);

  // ── 左侧：工作区标题 + 进度 ──────────────────────────
  const float dot_y = 44.0f * scale;
  fmt.brush->SetColor(Color(model.state == WorkModel::State::Online ? kAccent : kWarn, alpha));
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F((kPadX + 5.0f) * scale, dot_y), 4.5f * scale, 4.5f * scale),
                      fmt.brush);

  const float title_x = (kPadX + 18.0f) * scale;
  const std::wstring doc_title = FitText(model.doc_title.empty() ? L"未命名工作区" : model.doc_title,
                                         kLeftBlock - 26.0f, 12.5f);
  Text(target, doc_title, fmt.title,
       D2D1::RectF(title_x, 30.0f * scale, title_x + (kLeftBlock - 24.0f) * scale, 56.0f * scale), fmt.brush,
       text);

  if (model.total > 0) {
    const std::wstring progress =
        std::to_wstring(model.done) + L"/" + std::to_wstring(model.total) + L" 已完成";
    Text(target, progress, fmt.meta,
         D2D1::RectF(title_x, 72.0f * scale, title_x + (kLeftBlock - 10.0f) * scale, 94.0f * scale), fmt.brush,
         dim);
  }

  // ── 中间：一排点，一颗一件事 ─────────────────────────
  const int count = static_cast<int>(model.items.size());
  const float dots_x = (kPadX + kLeftBlock + kGapLeft) * scale;
  const float dots_cy = dip_h * 0.5f * scale;
  const int current = model.current >= 0 ? model.current : 0;
  const int focus_index = (focus >= 0 && focus < count) ? focus : current;

  if (count == 0) {
    Text(target, L"还没有任务 —— 按 Tab 框选屏幕上的活", fmt.body,
         D2D1::RectF(dots_x, dots_cy - 12.0f * scale, w - (kDetailBlock + kRightBlock) * scale,
                     dots_cy + 18.0f * scale),
         fmt.brush, dim);
  }

  int visible = VisibleDots(dip_w);
  if (visible > count) visible = count;
  const int first = DotStart(count, current, visible);

  for (int i = first; i < first + visible; ++i) {
    const float cx = dots_x + (static_cast<float>(i - first) + 0.5f) * kDotStep * scale;
    const NodeView& node = model.items[static_cast<size_t>(i)];
    const int distance = i > current ? i - current : current - i;
    const bool is_current = i == current;
    const bool is_focus = i == focus_index;

    if (is_current) {
      // 光晕先画：当前这颗点自己会"发热"，不用再套一个框去强调它
      fmt.brush->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.18f * alpha));
      target->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(cx, dots_cy), kDotRadiusNow * 2.1f * scale, kDotRadiusNow * 2.1f * scale),
          fmt.brush);
      fmt.brush->SetColor(Color(kAccent, alpha));
      target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dots_cy), kDotRadiusNow * scale, kDotRadiusNow * scale),
                          fmt.brush);
    } else {
      const float a = DotAlpha(distance, node.status) * alpha;
      fmt.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, a));
      target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dots_cy), kDotRadius * scale, kDotRadius * scale),
                          fmt.brush);
      // 指到哪颗，哪颗外面套一圈：这是"我要看这个"的最小提示
      if (is_focus) {
        fmt.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f * alpha));
        target->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, dots_cy), (kDotRadius + 4.5f) * scale, (kDotRadius + 4.5f) * scale),
            fmt.brush, 1.2f * scale);
      }
    }
  }

  // ── 右侧：指到的那件事（没指就写当前这件）────────────────
  const float detail_left = w - (kDetailBlock + kRightBlock + kPadX) * scale;
  if (focus_index >= 0 && focus_index < count) {
    const NodeView& node = model.items[static_cast<size_t>(focus_index)];
    const float status_w = 56.0f * scale;
    Text(target, StatusLabel(node.status), fmt.meta,
         D2D1::RectF(detail_left, 28.0f * scale, detail_left + status_w, 48.0f * scale), fmt.brush,
         node.status == "active" ? Fade(kAccent, alpha) : dim);

    const std::wstring title = FitText(node.title, kDetailBlock - status_w - 20.0f, 13.0f);
    Text(target, title, fmt.title,
         D2D1::RectF(detail_left + status_w, 26.0f * scale, w - (kRightBlock + kPadX) * scale, 54.0f * scale),
         fmt.brush, text);

    const std::wstring acceptance =
        node.acceptance.empty() ? L"（还没写验收）" : (L"验收 · " + node.acceptance);
    Text(target, FitText(acceptance, kDetailBlock - 10.0f, 12.0f), fmt.body,
         D2D1::RectF(detail_left, 58.0f * scale, w - (kRightBlock + kPadX) * scale, 84.0f * scale), fmt.brush,
         dim);

    // 建议是"AI 替你想过一手的下一步"，所以给它强调色；没有就整行不画，
    // 不摆一句"暂无建议"占地方。
    if (!node.advice.empty()) {
      const std::wstring line = L"建议 · " + node.advice;
      Text(target, FitText(line, kDetailBlock - 10.0f, 12.0f), fmt.body,
           D2D1::RectF(detail_left, 88.0f * scale, w - (kRightBlock + kPadX) * scale, 112.0f * scale),
           fmt.brush, Fade(kAccent, alpha));
      if (!node.advice_alt.empty()) {
        const std::wstring alt = L"更优解 · " + node.advice_alt;
        Text(target, FitText(alt, kDetailBlock - 10.0f, 11.5f), fmt.meta,
             D2D1::RectF(detail_left, 114.0f * scale, w - (kRightBlock + kPadX) * scale, 136.0f * scale),
             fmt.brush, dim);
      }
    }
  }

  // ── 最右：键位与连接状态 ─────────────────────────────
  const float right_x = w - (kPadX + kRightBlock) * scale;
  const std::wstring status = model.state == WorkModel::State::Online ? L"已连接" : L"未连接";
  Text(target, status, fmt.meta, D2D1::RectF(right_x, 30.0f * scale, w - kPadX * scale, 52.0f * scale), fmt.brush,
       model.state == WorkModel::State::Online ? dim : Fade(kWarn, alpha));
  Text(target, L"Esc 收起", fmt.meta, D2D1::RectF(right_x, 72.0f * scale, w - kPadX * scale, 94.0f * scale),
       fmt.brush, dim);
  Text(target, L"×", fmt.title, D2D1::RectF(w - 30.0f * scale, 8.0f * scale, w - 10.0f * scale, 32.0f * scale),
       fmt.brush, dim);

  target->SetTransform(D2D1::Matrix3x2F::Identity());
}

}  // namespace ui
}  // namespace dshwork
