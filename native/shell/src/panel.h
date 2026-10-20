// 面板：一条横贯屏幕顶部的长条。
//
// 为什么不放一个方盒子：这个面板是"随时瞟一眼"的东西，不是工作窗口。
// 方盒子会压住正在看的内容，而长条只占顶部一条带，视线不用离开原来的位置。
// 参考 BrimDeck（Windows 灵动岛）：收起是胶囊，展开是一条与其同中心的宽面板，
// 展开过程是同一个表面在变宽变高，不是两个窗口替换。
//
// 一列任务平铺在长条里，用**透明度**区分远近：当前这张完全不透明，
// 越往外越淡。不用边框、不用色块——视线先落在最实的那个，其余自然退后。
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
constexpr float kStripHeight = 132.0f;  // 长条高度
constexpr float kPadX = 20.0f;          // 左右内边距
constexpr float kLeftBlock = 240.0f;    // 左侧：工作区标题 + 进度
constexpr float kRightBlock = 116.0f;   // 右侧：连接状态 + 键位提示
constexpr float kGapLeft = 18.0f;
constexpr float kGapRight = 12.0f;

constexpr float kCardW = 196.0f;
constexpr float kCardH = 84.0f;
constexpr float kCardGap = 12.0f;
constexpr float kCardRadius = 16.0f;

constexpr float kMinWidth = 720.0f;
constexpr float kMaxWidth = 1560.0f;

constexpr UINT32 kText = 0xFFF5F5F7;
constexpr UINT32 kTextDim = 0xFF9A9AA0;
constexpr UINT32 kAccent = 0xFF0A84FF;
constexpr UINT32 kWarn = 0xFFFF453A;

/** 这条长条能排下几张卡。任务更多时只画当前任务附近的一段。 */
inline int VisibleCards(float dip_w) {
  const float usable = dip_w - (kPadX * 2 + kLeftBlock + kGapLeft + kGapRight + kRightBlock);
  int n = static_cast<int>((usable + kCardGap) / (kCardW + kCardGap));
  if (n < 1) n = 1;
  return n;
}

/** 窗口里第一张卡的序号：让当前任务尽量居中。 */
inline int CardStart(int count, int current, int visible) {
  int start = current - visible / 2;
  if (start < 0) start = 0;
  if (start + visible > count) start = count - visible;
  return start < 0 ? 0 : start;
}
/** 长条宽度：让所有任务卡刚好排下，两侧留出信息区。 */
inline float StripWidth(int card_count) {
  const int n = card_count > 0 ? card_count : 1;
  const float cards = static_cast<float>(n) * kCardW + static_cast<float>(n - 1) * kCardGap;
  const float w = kPadX * 2 + kLeftBlock + kGapLeft + cards + kGapRight + kRightBlock;
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
 * 分层窗口里 DWrite 的 SetTrimming 不生效（实测会溢出到卡片外面），
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
 * 卡片透明度：以当前任务为中心向外衰减。
 *
 * 这张表就是"用透明度区分"的全部规则。当前任务永远 1.0，
 * 邻座 0.42 还看得清标题，再往外只剩一个轮廓——眼睛不会去读它，
 * 但余光知道"那边还有事"。
 */
inline float CardAlpha(int distance, const std::string& status) {
  float a = 1.0f;
  if (distance == 1) {
    a = 0.50f;
  } else if (distance == 2) {
    a = 0.32f;
  } else if (distance >= 3) {
    a = 0.19f;
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
 * alpha 是内容整体的淡入淡出（表面已经先长出来，内容后跟上来）。
 */
inline void DrawPanel(ID2D1RenderTarget* target, const WorkModel& model, int focus, float scale,
                      const PanelFormats& fmt, float dip_w, float dip_h, float alpha) {
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
  if (dip_w < kMinWidth * 0.86f || dip_h < kStripHeight * 0.72f) return;

  const UINT32 text = Fade(kText, alpha);
  const UINT32 dim = Fade(kTextDim, alpha);

  // ── 左侧：工作区标题 + 进度 ──────────────────────────
  const float dot_y = 40.0f * scale;
  fmt.brush->SetColor(Color(model.state == WorkModel::State::Online ? kAccent : kWarn, alpha));
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F((kPadX + 5.0f) * scale, dot_y), 4.5f * scale, 4.5f * scale),
                      fmt.brush);

  const float title_x = (kPadX + 18.0f) * scale;
  // 单行标题：这个字体格式的行距是按小条调的，折行会叠字，
  // 宁可在左边多留点地方让它排得下
  const std::wstring doc_title = FitText(model.doc_title.empty() ? L"未命名工作区" : model.doc_title,
                                         kLeftBlock - 26.0f, 12.5f);
  Text(target, doc_title, fmt.title,
       D2D1::RectF(title_x, 28.0f * scale, title_x + (kLeftBlock - 24.0f) * scale, 54.0f * scale), fmt.brush,
       text);

  if (model.total > 0) {
    const std::wstring progress =
        std::to_wstring(model.done) + L"/" + std::to_wstring(model.total) + L" 已完成";
    Text(target, progress, fmt.meta,
         D2D1::RectF(title_x, 70.0f * scale, title_x + (kLeftBlock - 10.0f) * scale, 92.0f * scale), fmt.brush,
         dim);
  }

  // ── 中间：一列任务卡，按距离淡出 ─────────────────────
  const int count = static_cast<int>(model.items.size());
  const float card_y = (dip_h - kCardH) / 2.0f * scale;
  const float cards_x = (kPadX + kLeftBlock + kGapLeft) * scale;

  if (count == 0) {
    Text(target, L"还没有任务 —— 按 Tab 框选屏幕上的活，或让我帮你拆", fmt.body,
         D2D1::RectF(cards_x, card_y + 18.0f * scale, w - kRightBlock * scale, card_y + 60.0f * scale), fmt.brush,
         dim);
  }

  const int current = model.current >= 0 ? model.current : 0;
  int visible = VisibleCards(dip_w);
  if (visible > count) visible = count;
  const int first = CardStart(count, current, visible);
  for (int i = first; i < first + visible; ++i) {
    const float x = cards_x + static_cast<float>(i - first) * (kCardW + kCardGap) * scale;
    const D2D1_RECT_F card =
        D2D1::RectF(x, card_y, x + kCardW * scale, card_y + kCardH * scale);
    const NodeView& node = model.items[static_cast<size_t>(i)];
    const int distance = i > current ? i - current : current - i;
    const float a = CardAlpha(distance, node.status) * alpha;
    const bool is_current = i == current;
    const bool is_focus = i == focus;

    // 填充也吃透明度：同一张卡在不同距离上"厚薄"不同，
    // 比换颜色更安静，也更像玻璃叠在桌面上。
    FillRound(target, fmt.brush, card, kCardRadius * scale, Fade(0xFFFFFFFF, 0.055f * a + (is_current ? 0.02f : 0.0f)));
    if (is_current) {
      StrokeRound(target, fmt.brush, card, kCardRadius * scale, Fade(kAccent, 0.85f * alpha), 1.4f * scale);
    } else if (is_focus) {
      StrokeRound(target, fmt.brush, card, kCardRadius * scale, Fade(0xFFFFFFFF, 0.28f * alpha), 1.0f * scale);
    } else {
      StrokeRound(target, fmt.brush, card, kCardRadius * scale, Fade(0xFFFFFFFF, 0.10f * a));
    }

    Text(target, StatusLabel(node.status), fmt.meta,
         D2D1::RectF(x + 14.0f * scale, card_y + 12.0f * scale, x + kCardW * scale - 12.0f * scale,
                     card_y + 30.0f * scale),
         fmt.brush, is_current ? Fade(kAccent, alpha) : Fade(kTextDim, a));

    const std::wstring title = FitText(node.title, kCardW - 28.0f, 13.0f);
    Text(target, title, fmt.title,
         D2D1::RectF(x + 14.0f * scale, card_y + 34.0f * scale, x + kCardW * scale - 12.0f * scale,
                     card_y + kCardH * scale - 10.0f * scale),
         fmt.brush, Fade(kText, is_current ? alpha : a));
  }

  // ── 右侧：键位与连接状态 ─────────────────────────────
  const float right_x = w - (kPadX + kRightBlock) * scale;
  const std::wstring status = model.state == WorkModel::State::Online ? L"已连接" : L"未连接";
  Text(target, status, fmt.meta, D2D1::RectF(right_x, 40.0f * scale, w - kPadX * scale, 62.0f * scale), fmt.brush,
       model.state == WorkModel::State::Online ? dim : Fade(kWarn, alpha));
  Text(target, L"Esc 收起", fmt.meta, D2D1::RectF(right_x, 62.0f * scale, w - kPadX * scale, 84.0f * scale),
       fmt.brush, dim);
  Text(target, L"×", fmt.title, D2D1::RectF(w - 30.0f * scale, 6.0f * scale, w - 8.0f * scale, 34.0f * scale),
       fmt.brush, dim);
}

}  // namespace ui
}  // namespace dshwork
