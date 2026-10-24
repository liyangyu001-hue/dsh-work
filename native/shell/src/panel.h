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
#include <cmath>
#include <string>
#include "doc.h"

namespace dshwork {

/**
 * DWrite 工厂在这里存一份指针（由 main.cpp 在初始化时赋值）。
 *
 * 不直接 extern 去引 main.cpp 里那个全局量：它在匿名 namespace 里，
 * 外面再声明一个同名的就是「不明确的符号」（实测 C2872）。
 */
inline IDWriteFactory* g_ui_dwrite = nullptr;
// 完成按钮的按压进度：0=常态、1=按到底。main 在点击后驱动它，
// 画的时候做一次「按下去再弹回来」，点下去才有手感。
inline float g_btn_pulse = 0.0f;

struct PanelFormats {
  IDWriteTextFormat* title = nullptr;  // 任务标题 13px
  IDWriteTextFormat* body = nullptr;   // 详情 12.5px
  IDWriteTextFormat* meta = nullptr;   // 标签/计数 10.5px
  IDWriteTextFormat* node = nullptr;   // 节点图里的节点标题 16px：比面板正文大一号，缩放到 100% 也看得清
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

/**
 * 「完成」按钮的矩形（像素坐标）。
 *
 * 面板不抢焦点、也不注册全局热键——键位那种做法会在用户打字时把字吞掉。
 * 所以动作只能落在鼠标上，这个矩形同时被绘制和命中测试用到，两处必须是同一个函数，
 * 否则按钮看着在哪、点着不在哪。
 */
inline D2D1_RECT_F AdvanceRectAt(float width_dip, float height_dip, float scale) {
  (void)height_dip;
  const float right = (width_dip - kPadX) * scale;
  const float wide = 64.0f * scale;
  const float top = 54.0f * scale;
  const float tall = 26.0f * scale;
  return D2D1::RectF(right - wide, top, right, top + tall);
}

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
 * 文字在一个矩形里精确居中（水平 + 垂直）。
 *
 * 按钮上的字不能用「左对齐 + 手工加个 padding」凑——字宽随字体和缩放变，
 * 手调的偏移一定会偏。这里交给 DWrite 排一次版，居中是它算的。
 */
inline void TextCentered(ID2D1RenderTarget* target, const std::wstring& text, IDWriteTextFormat* format,
                         D2D1_RECT_F rect, ID2D1SolidColorBrush* brush, UINT32 color) {
  if (!g_ui_dwrite || text.empty()) return;
  IDWriteTextLayout* layout = nullptr;
  const float w = rect.right - rect.left;
  const float h = rect.bottom - rect.top;
  if (w <= 0.0f || h <= 0.0f) return;
  if (FAILED(g_ui_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, w, h, &layout)) ||
      !layout) {
    return;
  }
  layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  brush->SetColor(Color(color));
  target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  layout->Release();
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

inline const wchar_t* HandoffLabel(const std::string& state) {
  if (state == "running") return L"AI 正在做";
  if (state == "review") return L"AI 做完了";
  if (state == "accepted") return L"已验收";
  if (state == "returned") return L"已退回";
  return L"已交给 AI";
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
 * 圆角矩形周长上的一点：t 从 0 到 1，从左上角接缝出发顺时针。
 *
 * 和节点上「一圈在转」用的是同一套参数化（直线段 + 四段 1/4 圆），
 * 只是这里的矩形是整扇窗。t 越界会自动绕回，流光可以一直跑下去。
 */
inline D2D1_POINT_2F FramePoint(const D2D1_RECT_F& box, float radius, float t) {
  const float x0 = box.left;
  const float y0 = box.top;
  const float x1 = box.right;
  const float y1 = box.bottom;
  const float limit = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 0.5f;
  float r = radius;
  if (r > limit) r = limit;
  if (r < 0.0f) r = 0.0f;
  const float sx = (x1 - x0) - 2.0f * r;
  const float sy = (y1 - y0) - 2.0f * r;
  const float arc = 1.5707963f * r;
  const float total = 2.0f * sx + 2.0f * sy + 4.0f * arc;
  if (total <= 0.01f) return D2D1::Point2F(x0, y0);
  float d = t * total;
  d -= std::floor(d / total) * total;
  if (d < sx) return D2D1::Point2F(x0 + r + d, y0);
  d -= sx;
  if (d < arc) {
    const float k = d / arc * 1.5707963f;
    return D2D1::Point2F(x1 - r + r * std::sin(k), y0 + r - r * std::cos(k));
  }
  d -= arc;
  if (d < sy) return D2D1::Point2F(x1, y0 + r + d);
  d -= sy;
  if (d < arc) {
    const float k = d / arc * 1.5707963f;
    return D2D1::Point2F(x1 - r + r * std::cos(k), y1 - r + r * std::sin(k));
  }
  d -= arc;
  if (d < sx) return D2D1::Point2F(x1 - r - d, y1);
  d -= sx;
  if (d < arc) {
    const float k = d / arc * 1.5707963f;
    return D2D1::Point2F(x0 + r - r * std::sin(k), y1 - r + r * std::cos(k));
  }
  d -= arc;
  if (d < sy) return D2D1::Point2F(x0, y1 - r - d);
  d -= sy;
  const float k = d / arc * 1.5707963f;
  return D2D1::Point2F(x0 + r - r * std::cos(k), y0 + r - r * std::sin(k));
}

/**
 * 「AI 会话正在跑」的窗边：一圈随呼吸变亮的蓝边 + 一颗沿边顺时针跑的流光。
 *
 * 只画边，不碰内容：用户要的是「这窗口里有东西在自动运行」的余光信号，
 * 不是再读一段说明。坐标是 DIP，调用方保证当前变换是 Scale(scale)。
 * alpha 跟着窗口的开合淡入淡出；没有运行中的会话时调用方根本不会调这里。
 */
inline void DrawAiFrame(ID2D1RenderTarget* target, const PanelFormats& fmt, float width_dip, float height_dip,
                        float radius_dip, ULONGLONG now_ms, float alpha) {
  if (target == nullptr || width_dip <= 4.0f || height_dip <= 4.0f || alpha <= 0.01f) return;
  const float seconds = static_cast<float>(now_ms % 100000) * 0.001f;
  const float breath = 0.5f + 0.5f * std::sin(seconds * 2.2f);  // 约 0.35Hz，比节点环慢
  const D2D1_RECT_F edge = D2D1::RectF(1.0f, 1.0f, width_dip - 1.0f, height_dip - 1.0f);
  float r = radius_dip - 1.0f;
  if (r < 0.0f) r = 0.0f;

  // 外晕三圈：宽而淡，像蓝光从窗边后面漏出来；主边跟着呼吸变亮。
  // 分层窗口会把画到窗外的部分裁掉，所以看起来正好是"从里面透出去"。
  const float glow_w[3] = {10.0f, 5.0f, 2.2f};
  const float glow_a[3] = {0.05f, 0.11f, 0.34f + 0.30f * breath};
  for (int i = 0; i < 3; ++i) {
    fmt.brush->SetColor(Color(Fade(kAccent, glow_a[i] * alpha)));
    target->DrawRoundedRectangle(D2D1::RoundedRect(edge, r, r), fmt.brush, glow_w[i]);
  }

  // 流光：亮头在前，尾巴在后；6.4 秒一圈，和节点环同向（顺时针）。
  // 点阵而不是折线：分层窗下 DrawLine 的平头会露出缺口，圆点叠起来才是连续锥形。
  constexpr int kN = 480;
  constexpr float kTail = 0.14f;
  const float head = static_cast<float>(now_ms % 6400) / 6400.0f;
  for (int s = 0; s <= kN; ++s) {
    float d = head - static_cast<float>(s) / kN;
    d -= std::floor(d);
    if (d > kTail) continue;
    const float k = 1.0f - d / kTail;
    const float ease = k * k;
    const D2D1_POINT_2F p = FramePoint(edge, r, static_cast<float>(s) / kN);
    fmt.brush->SetColor(Color(Fade(kAccent, (0.02f + 0.20f * ease) * alpha)));
    target->FillEllipse(D2D1::Ellipse(p, 4.2f + 2.6f * ease, 4.2f + 2.6f * ease), fmt.brush);
    fmt.brush->SetColor(Color(Fade(0xFFFFFFFF, (0.06f + 0.82f * ease) * alpha)));
    target->FillEllipse(D2D1::Ellipse(p, 1.0f + 1.5f * ease, 1.0f + 1.5f * ease), fmt.brush);
  }
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
                      const PanelFormats& fmt, float dip_w, float dip_h, float alpha, float zoom,
                      bool ai_running, ULONGLONG now_ms) {
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
  //
  // 排版规则（上一版乱在这）：标签占固定 46 DIP 一列、值从固定位置起排，
  // 三行等距（每行 20 DIP + 4 DIP 间距），每行只给一行字、超出用省略号。
  // 这样无论哪件事、有没有建议，右列都长得一样，眼睛不用重新找位置。
  const float detail_left = w - (kDetailBlock + kRightBlock + kPadX) * scale;
  const float detail_right = w - (kRightBlock + kPadX) * scale;
  const float label_w = 46.0f * scale;
  const float value_left = detail_left + label_w;
  const float value_w = (detail_right - value_left) / scale;

  if (focus_index >= 0 && focus_index < count) {
    const NodeView& node = model.items[static_cast<size_t>(focus_index)];

    // 第一行：状态标签 + 标题。状态用小药丸，标题最多一行。
    const std::wstring status_text = StatusLabel(node.status);
    const bool running = node.status == "active";
    const float chip_w = 44.0f * scale;
    const D2D1_RECT_F chip =
        D2D1::RectF(detail_left, 22.0f * scale, detail_left + chip_w, 40.0f * scale);
    FillRound(target, fmt.brush, chip, 5.0f * scale, running ? Fade(0x260A84FF, alpha) : Fade(0x12FFFFFF, alpha));
    TextCentered(target, status_text, fmt.meta, chip, fmt.brush, running ? Fade(kAccent, alpha) : dim);

    const std::wstring title = FitText(node.title, value_w + 60.0f, 13.0f);
    Text(target, title, fmt.title,
         D2D1::RectF(value_left, 20.0f * scale, detail_right, 44.0f * scale), fmt.brush, text);

    // 之后三行：标签 + 值，行高固定。
    const float row1 = 50.0f * scale;
    const float row2 = 74.0f * scale;
    const float row3 = 98.0f * scale;
    const float row_h = 20.0f * scale;

    // 验收
    Text(target, L"验收", fmt.meta, D2D1::RectF(detail_left, row1, value_left, row1 + row_h), fmt.brush, dim);
    const std::wstring acceptance =
        node.acceptance.empty() ? L"（还没写验收）" : FitText(node.acceptance, value_w, 12.0f);
    Text(target, acceptance, fmt.body, D2D1::RectF(value_left, row1, detail_right, row1 + row_h), fmt.brush, dim);

    // 建议（有才画，强调色）
    if (!node.advice.empty()) {
      Text(target, L"建议", fmt.meta, D2D1::RectF(detail_left, row2, value_left, row2 + row_h), fmt.brush,
           Fade(kAccent, alpha * 0.85f));
      Text(target, FitText(node.advice, value_w, 12.0f), fmt.body,
           D2D1::RectF(value_left, row2, detail_right, row2 + row_h), fmt.brush, Fade(kAccent, alpha));
    }

    // 更优解（有才画，最弱的一档）
    if (!node.advice_alt.empty()) {
      Text(target, L"更优解", fmt.meta, D2D1::RectF(detail_left, row3, value_left, row3 + row_h), fmt.brush, dim);
      Text(target, FitText(node.advice_alt, value_w, 11.5f), fmt.meta,
           D2D1::RectF(value_left, row3, detail_right, row3 + row_h), fmt.brush, Fade(0xB0B0B6FF, alpha * 0.9f));
    }
  }

  // ── 最右：完成按钮与连接状态 ─────────────────────────
  const float right_x = w - (kPadX + kRightBlock) * scale;
  const std::wstring status = model.state == WorkModel::State::Online ? L"已连接" : L"未连接";
  Text(target, status, fmt.meta, D2D1::RectF(right_x, 26.0f * scale, w - kPadX * scale, 46.0f * scale), fmt.brush,
       model.state == WorkModel::State::Online ? dim : Fade(kWarn, alpha));
  // 「完成」是个真按钮：面板不抢焦点、也不注册全局键，所以动作只能靠点。
  D2D1_RECT_F done = AdvanceRectAt(w / scale, h / scale, scale);
  if (g_btn_pulse > 0.0f) {
    const float k = 1.0f - 0.10f * g_btn_pulse;  // 最多缩到九成
    const float pcx = (done.left + done.right) * 0.5f;
    const float pcy = (done.top + done.bottom) * 0.5f;
    done.left = pcx + (done.left - pcx) * k;
    done.right = pcx + (done.right - pcx) * k;
    done.top = pcy + (done.top - pcy) * k;
    done.bottom = pcy + (done.bottom - pcy) * k;
  }
  FillRound(target, fmt.brush, done, 13.0f * scale, Fade(kAccent, alpha * 0.92f));
  TextCentered(target, L"完成", fmt.meta,
               D2D1::RectF(done.left, done.top, done.right, done.bottom), fmt.brush, Fade(0xFFFFFFFF, alpha));

  // 交给 AI 之后，最右列常驻一行交接状态（面板只有 132 DIP 高，别处放不下）
  std::wstring handoff_line;
  for (const NodeView& item : model.items) {
    if (!item.handoff.empty()) { handoff_line = HandoffLabel(item.handoff); break; }
  }
  if (!handoff_line.empty()) {
    Text(target, FitText(handoff_line, kRightBlock, 11.0f), fmt.meta,
         D2D1::RectF(right_x, 104.0f * scale, w - kPadX * scale, 124.0f * scale),
         fmt.brush, Fade(kAccent, alpha * 0.9f));
  }
  Text(target, L"×", fmt.title, D2D1::RectF(w - 30.0f * scale, 8.0f * scale, w - 10.0f * scale, 32.0f * scale),
       fmt.brush, dim);

  target->SetTransform(D2D1::Matrix3x2F::Identity());
  // 运行中的会话给整条面板描一圈蓝边。画在最后 = 压在所有内容上；
  // 单独换回 Scale 变换，是因为内容那层有 zoom，边框不该跟着缩。
  if (ai_running) {
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    const float ring_radius = dip_h * 0.5f < 26.0f ? dip_h * 0.5f : 26.0f;
    DrawAiFrame(target, fmt, dip_w, dip_h, ring_radius, now_ms, alpha);
    target->SetTransform(D2D1::Matrix3x2F::Identity());
  }
}


/**
 * 事件流那一行：单独一个函数，由调用方在 DrawPanel 之后画。
 *
 * 为什么不做进 DrawPanel：面板是先铺底再画内容，插在函数开头会被底盖住；
 * 单独画一层最省事，也不影响 DrawPanel 内部的层级。
 */
inline void DrawFeed(ID2D1RenderTarget* target, const WorkModel& model, const PanelFormats& fmt,
                     float scale, float dip_w, float dip_h, float alpha) {
  if (model.feed.empty()) return;
  const float w = dip_w * scale;
  const float h = dip_h * scale;
  const D2D1_RECT_F box = D2D1::RectF(20.0f * scale, h - 30.0f * scale, w - 20.0f * scale, h - 8.0f * scale);
  target->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  /*
   * 颜色必须给不透明值（0xFF......）。面板是分层窗口：D2D 把颜色写进 DIB 时
   * 已经按预乘 alpha 处理一次，UpdateLayeredWindow(ULW_ALPHA) 再乘一次 —— 任何
   * alpha < 1 的颜色都会被压暗近一倍，alpha≈0.4 的文字在深色底上直接看不见。
   * 2026-10-25 实测同一串字：0xFF9AA0A6 清楚、0x9AA0A6 只剩零星噪点、0x659AA0A6 全无。
   * 所以「暗」用不透明暗色表达（和面板里其他次要文字同一套颜色），
   * 整体淡出仍旧交给 alpha —— 面板稳态 alpha=1，只有收起动画里才会变小。
   */
  Text(target, model.feed.front(), fmt.meta, box, fmt.brush, Fade(kTextDim, alpha));
  target->PopAxisAlignedClip();
}

}  // namespace ui
}  // namespace dshwork
