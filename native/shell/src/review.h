// 审阅窗的「节点图」：解构出来的任务树，一个任务一个框，用连线表示父子。
//
// 为什么是图而不是列表：任务之间只有"先后"和"从属"两种关系，可这两件事
// 在缩进列表里要读很久；画成从左往右的流程树，一眼就能看出"从哪开始、
// 走到哪、哪颗还想再拆"。排版用的是经典的 tidy-tree：叶子按顺序占位，
// 父节点垂直居中在自己的孩子之间。
//
// 这个文件只做三件事：排版（Layout/FitView）、命中（NodeAt）、绘制（Draw）。
// 键盘、网络、窗口生命周期都在 main.cpp —— 这里保持成可以被单测的纯计算。
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "panel.h"  // PanelFormats / ui::kAccent / Text 等公共绘制件

namespace dshwork::review {

struct Row {
  std::string id;
  int depth = 0;
  int parent = -1;  // 父行索引；-1 是根
  bool has_children = false;
  bool expanded = false;
  int child_count = 0;
  std::string status;
  std::string title;
  std::string detail;
  std::string acceptance;
  std::string kind;

  // 排版结果（世界坐标，DIP）。父节点用它的孩子算垂直位置。
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

struct Model {
  std::wstring title;
  std::vector<Row> rows;
  int selected = -1;
  int total = 0;
  bool busy = false;
  std::wstring busy_text;
  // 只读模式：看的是已经落库的文档，不是草稿。按钮变成「知道了」，Enter 只展开/收起。
  bool read_only = false;

  // 视图：世界坐标 → 窗口，先乘 zoom 再平移。缩放围绕面板中心，不做鼠标锚定，
  // 因为这个窗口是用来"看清结构"的，不是看图软件。
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  bool fitted = false;  // 第一次排版后自动缩放到合适大小，之后尊重用户的手动视图

  // 每颗节点「第一次被看到」的时刻，用来让新长出来的框错开淡入。
  std::unordered_map<std::string, ULONGLONG> appear;
};

struct Anim {
  float open = 1.0f;
  float highlight_y = -1.0f;  // 旧版列表的高亮位置；图模式不再用它，留着接口不折腾调用方
  ULONGLONG now = 0;
};

constexpr float kHeader = 58.0f;
constexpr float kFooter = 48.0f;
constexpr float kPad = 26.0f;

constexpr float kNodeW = 214.0f;
constexpr float kNodeH = 86.0f;
constexpr float kGapX = 74.0f;   // 列间距：够画一条明显的连线
constexpr float kGapY = 26.0f;   // 行间距
constexpr float kNodeRadius = 14.0f;

constexpr float kMinZoom = 0.28f;  // 展开一层后内容会很宽，允许缩得更小才装得下
constexpr float kMaxZoom = 1.60f;

constexpr float kWindowW = 980.0f;
constexpr float kWindowH = 640.0f;
constexpr float kCommitW = 108.0f;
constexpr float kCommitH = 32.0f;

inline float WindowWidth(const Model&) { return kWindowW; }
inline float WindowHeight(const Model&) { return kWindowH; }
inline float BodyWidth(const Model&) { return kWindowW; }
inline float BodyHeight(const Model&) { return kWindowH - kHeader - kFooter; }

inline float EaseOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

/** 状态点的颜色：做完绿、在做强调蓝、跳过暗、待办按层级分两档。 */
inline UINT32 StatusInk(const std::string& status, bool child) {
  if (status == "done") return 0xFF30D158;
  if (status == "active") return ui::kAccent;
  if (status == "skipped") return 0xFF5C5C63;
  return child ? 0xFF7C7C85 : 0xFFA0A0A8;
}

/**
 * 排版：叶子按出现顺序占位，父节点垂直居中在孩子中间。
 *
 * 两趟递归：先量每棵子树需要多高，再按这个高度往下摆。
 * 行数组本身是 DFS 先序（main.cpp 摊平出来的），所以子节点在父节点后面，
 * 用 parent 索引就能把树建起来。
 */
inline void Layout(Model& model) {
  const int count = static_cast<int>(model.rows.size());
  if (count == 0) return;

  std::vector<std::vector<int>> kids(static_cast<size_t>(count));
  std::vector<int> roots;
  for (int i = 0; i < count; ++i) {
    const int parent = model.rows[static_cast<size_t>(i)].parent;
    if (parent >= 0 && parent < count) {
      kids[static_cast<size_t>(parent)].push_back(i);
    } else {
      roots.push_back(i);
    }
  }

  // 第一趟：子树高度（叶子 = kNodeH）
  std::vector<float> height(static_cast<size_t>(count), kNodeH);
  // 逆序处理即可保证孩子先于父亲算完：DFS 先序里孩子一定排在父亲后面。
  for (int i = count - 1; i >= 0; --i) {
    const std::vector<int>& children = kids[static_cast<size_t>(i)];
    if (children.empty()) continue;
    float sum = 0.0f;
    for (size_t c = 0; c < children.size(); ++c) {
      sum += height[static_cast<size_t>(children[c])];
      if (c + 1 < children.size()) sum += kGapY;
    }
    height[static_cast<size_t>(i)] = sum;
  }

  // 第二趟：给每棵子树一个纵向游标
  std::vector<float> top(static_cast<size_t>(count), 0.0f);
  float cursor = 0.0f;
  for (size_t r = 0; r < roots.size(); ++r) {
    const int root = roots[r];
    top[static_cast<size_t>(root)] = cursor;
    cursor += height[static_cast<size_t>(root)] + kGapY * 2.0f;
  }
  // 顺着 DFS 顺序把孩子摆进父亲的地盘
  for (int i = 0; i < count; ++i) {
    const std::vector<int>& children = kids[static_cast<size_t>(i)];
    if (children.empty()) continue;
    const float children_total = height[static_cast<size_t>(i)];
    float y = top[static_cast<size_t>(i)] + (height[static_cast<size_t>(i)] - children_total) * 0.5f;
    for (size_t c = 0; c < children.size(); ++c) {
      const int child = children[c];
      top[static_cast<size_t>(child)] = y;
      y += height[static_cast<size_t>(child)] + kGapY;
    }
  }

  // 落到行上：横向流程 + 分层。
  //
  // 为什么不是"按深度排成一列"（上一版）：根节点会全部竖着堆在左边，
  // 读起来像清单而不是流程。现在 x 按 DFS 顺序一格一格往右排、y 按层级下沉，
  // 展开谁就在它右边长出它的下一步，方向感来自坐标本身，不靠箭头。
  for (int i = 0; i < count; ++i) {
    Row& row = model.rows[static_cast<size_t>(i)];
    row.w = kNodeW;
    row.h = kNodeH;
    row.x = static_cast<float>(i) * (kNodeW + kGapX);
    row.y = static_cast<float>(row.depth) * (kNodeH + kGapY);
  }
}

/** 世界坐标的包围盒（用来做自动适配视图）。 */
inline void Bounds(const Model& model, float& min_x, float& min_y, float& max_x, float& max_y) {
  min_x = 0.0f;
  min_y = 0.0f;
  max_x = kNodeW;
  max_y = kNodeH;
  if (model.rows.empty()) return;
  min_x = model.rows[0].x;
  max_x = model.rows[0].x + model.rows[0].w;
  min_y = model.rows[0].y;
  max_y = model.rows[0].y + model.rows[0].h;
  for (const Row& row : model.rows) {
    min_x = row.x < min_x ? row.x : min_x;
    min_y = row.y < min_y ? row.y : min_y;
    max_x = row.x + row.w > max_x ? row.x + row.w : max_x;
    max_y = row.y + row.h > max_y ? row.y + row.h : max_y;
  }
}

/**
 * 把整张图缩到面板里。
 *
 * 只在第一次排版时自动跑一次（model.fitted），之后用户拖过、缩过就尊重用户，
 * 否则每次展开一层视图都会自己跳一下，很难受。
 */
inline void FitView(Model& model) {
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  Bounds(model, min_x, min_y, max_x, max_y);
  const float content_w = max_x - min_x;
  const float content_h = max_y - min_y;
  const float avail_w = BodyWidth(model) - kPad * 2.0f;
  const float avail_h = BodyHeight(model) - kPad * 2.0f;

  float zoom = 1.0f;
  if (content_w > 0.0f && content_h > 0.0f) {
    const float zx = avail_w / content_w;
    const float zy = avail_h / content_h;
    zoom = zx < zy ? zx : zy;  // 手写比较：windows.h 的 min 宏会吃掉 std::min
  }
  if (zoom > 1.0f) zoom = 1.0f;
  if (zoom < kMinZoom) zoom = kMinZoom;
  model.zoom = zoom;

  // 居中：把内容中心搬到面板中心
  const float content_cx = (min_x + max_x) * 0.5f;
  const float content_cy = (min_y + max_y) * 0.5f;
  model.pan_x = BodyWidth(model) * 0.5f - content_cx * zoom;
  model.pan_y = BodyHeight(model) * 0.5f - content_cy * zoom;
  model.fitted = true;
}

/** 把平移量夹住，别让用户把整张图拖出视野找不回来。 */
inline void ClampPan(Model& model) {
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  Bounds(model, min_x, min_y, max_x, max_y);
  const float slack = 120.0f;  // 允许露一点空白，手感更松
  const float left = -max_x * model.zoom - slack;
  const float right = BodyWidth(model) + slack;
  const float top = -max_y * model.zoom - slack;
  const float bottom = BodyHeight(model) + slack;
  if (model.pan_x < left) model.pan_x = left;
  if (model.pan_x > right) model.pan_x = right;
  if (model.pan_y < top) model.pan_y = top;
  if (model.pan_y > bottom) model.pan_y = bottom;
}

/** 鼠标点在哪个框上；点在空白返回 -1。坐标是窗口 DIP。 */
inline int NodeAt(const Model& model, float x, float y) {
  if (!model.fitted) return -1;
  const float wx = (x - model.pan_x) / model.zoom;
  const float wy = (y - kHeader - model.pan_y) / model.zoom;
  for (int i = static_cast<int>(model.rows.size()) - 1; i >= 0; --i) {
    const Row& row = model.rows[static_cast<size_t>(i)];
    if (wx >= row.x && wx <= row.x + row.w && wy >= row.y && wy <= row.y + row.h) return i;
  }
  return -1;
}

inline D2D1_RECT_F CommitRectAt(float width_dip, float height_dip) {
  return D2D1::RectF(width_dip - kPad - kCommitW, height_dip - kFooter + (kFooter - kCommitH) * 0.5f,
                     width_dip - kPad, height_dip - kFooter + (kFooter + kCommitH) * 0.5f);
}

inline bool CommitHit(const Model& model, float x, float y) {
  const D2D1_RECT_F rect = CommitRectAt(WindowWidth(model), WindowHeight(model));
  return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}


/** UTF-8 → UTF-16。节点标题从 JSON 进来，窗口这边只认宽字符。 */
inline std::wstring WidenNode(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (need <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), need);
  return out;
}

/** 一行字放不下就拆成两行：第一行尽量长，第二行给省略号。 */
inline void SplitTwoLines(const std::wstring& text, float max_w, float font_dip, std::wstring& line1,
                          std::wstring& line2) {
  const std::wstring fitted = ui::FitText(text, max_w, font_dip);
  if (fitted.size() >= text.size()) {
    line1 = fitted;
    line2.clear();
    return;
  }
  size_t cut = fitted.size();
  if (cut > 0 && fitted[cut - 1] == L'…') --cut;
  if (cut == 0) cut = 1;
  line1 = text.substr(0, cut);
  line2 = ui::FitText(text.substr(cut), max_w, font_dip);
}

/**
 * 画整张节点图。
 *
 * 顺序是刻意的：先连线再画框，线头就被框压住；选中框最后画，永远在最上层。
 * 世界坐标的换算只有一处（SetTransform），命中测试用的是同一组参数，
 * 所以"看到的位置"和"点到的位置"不会分叉。
 */
inline void Draw(ID2D1RenderTarget* target, const Model& model, const PanelFormats& fmt, float scale,
                 float width_dip, float height_dip, float alpha, const Anim& anim) {
  if (target == nullptr) return;
  const float a = alpha * EaseOut(anim.open * 1.15f);  // 稍微提前收尾，读起来更利落
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));

  const UINT32 bg = ui::Fade(0xFF14161C, a);
  ui::FillRound(target, fmt.brush, D2D1::RectF(0.0f, 0.0f, width_dip, height_dip), 22.0f, bg);
  ui::StrokeRound(target, fmt.brush, D2D1::RectF(0.5f, 0.5f, width_dip - 0.5f, height_dip - 0.5f), 22.0f,
                  ui::Fade(0xFFFFFFFF, 0.08f * a), 0.5f);

  // ---- 标题栏 ----
  const std::wstring heading = model.title.empty() ? L"任务树" : model.title;
  ui::Text(target, heading, fmt.title, D2D1::RectF(kPad, 16.0f, width_dip - kPad - 90.0f, 42.0f), fmt.brush,
           ui::Fade(ui::kText, a));
  const std::wstring count = std::to_wstring(model.total) + L" 件事";
  ui::Text(target, count, fmt.meta, D2D1::RectF(width_dip - kPad - 130.0f, 20.0f, width_dip - kPad, 40.0f),
           fmt.brush, ui::Fade(ui::kTextDim, a));

  // ---- 图区 ----
  const D2D1_RECT_F body = D2D1::RectF(0.0f, kHeader, width_dip, height_dip - kFooter);
  target->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
  target->SetTransform(D2D1::Matrix3x2F::Scale(model.zoom, model.zoom) *
                       D2D1::Matrix3x2F::Translation(model.pan_x, kHeader + model.pan_y) *
                       D2D1::Matrix3x2F::Scale(scale, scale));

  // 连线：父的右边中点 → 子的左边中点，三次贝塞尔，角度按水平切线收。
  //
  // 线上跑一颗「光点」：把曲线采样成折线、按长度归一化，再让一段亮弧随时间
  // 平移。方向是父→子，所以光是朝思路前进的方向流的，扫一眼就知道下一步在哪。
  // 采样 24 段足够顺滑，且比求贝塞尔弧长公式便宜得多。
  static ID2D1Factory* factory = nullptr;
  if (factory == nullptr) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
  const float flow_phase = static_cast<float>((anim.now % 100000) * 0.00022);
  for (size_t i = 0; i < model.rows.size(); ++i) {
    const Row& row = model.rows[i];
    if (row.parent < 0 || row.parent >= static_cast<int>(model.rows.size())) continue;
    const Row& parent = model.rows[static_cast<size_t>(row.parent)];
    const D2D1_POINT_2F from = D2D1::Point2F(parent.x + parent.w, parent.y + parent.h * 0.5f);
    const D2D1_POINT_2F to = D2D1::Point2F(row.x, row.y + row.h * 0.5f);
    const float bend = (to.x - from.x) * 0.5f;

    // 采样并累计长度
    constexpr int kSamples = 24;
    D2D1_POINT_2F points[kSamples + 1];
    float lengths[kSamples + 1] = {};
    float total = 0.0f;
    for (int s = 0; s <= kSamples; ++s) {
      const float t = static_cast<float>(s) / kSamples;
      const float u = 1.0f - t;
      // 三次贝塞尔，控制点按水平切线摆
      const D2D1_POINT_2F c1 = D2D1::Point2F(from.x + bend, from.y);
      const D2D1_POINT_2F c2 = D2D1::Point2F(to.x - bend, to.y);
      points[s].x = u * u * u * from.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * to.x;
      points[s].y = u * u * u * from.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * to.y;
      if (s > 0) {
        const float dx = points[s].x - points[s - 1].x;
        const float dy = points[s].y - points[s - 1].y;
        total += std::sqrt(dx * dx + dy * dy);
      }
      lengths[s] = total;
    }
    if (total <= 0.01f) continue;

    // 底线
    fmt.brush->SetColor(ui::Color(ui::Fade(0xFFFFFFFF, 0.22f * a)));
    for (int s = 0; s < kSamples; ++s) target->DrawLine(points[s], points[s + 1], fmt.brush, 1.6f);

    // 流光：一段 18% 长度的亮弧，每条边错开相位，整体朝前流
    const float phase = flow_phase + static_cast<float>(i) * 0.11f;
    const float head = phase - std::floor(phase);
    for (int s = 0; s < kSamples; ++s) {
      const float p = (lengths[s] / total) - head;
      float u = p - std::floor(p);              // 0..1 的循环位置
      const float tail_len = 0.18f;
      float d = head - (lengths[s] / total);    // 当前段相对光头的距离（负=在光头之后）
      d -= std::floor(d);
      (void)u;
      if (d > 1.0f - tail_len) {
        const float k = (d - (1.0f - tail_len)) / tail_len;  // 0..1，越接近光头越亮
        fmt.brush->SetColor(ui::Color(ui::Fade(ui::kAccent, (0.10f + 0.55f * k) * a)));
        target->DrawLine(points[s], points[s + 1], fmt.brush, 1.9f);
      }
    }
    (void)factory;
  }

  // 节点
  for (size_t i = 0; i < model.rows.size(); ++i) {
    const Row& row = model.rows[i];
    const bool selected = static_cast<int>(i) == model.selected;
    const D2D1_RECT_F box = D2D1::RectF(row.x, row.y, row.x + row.w, row.y + row.h);
    const UINT32 fill = selected ? ui::Fade(0xFF1B2A44, a) : ui::Fade(0xFF1B1D24, a);
    ui::FillRound(target, fmt.brush, box, kNodeRadius, fill);
    ui::StrokeRound(target, fmt.brush, box, kNodeRadius,
                    selected ? ui::Fade(ui::kAccent, 0.95f * a) : ui::Fade(0xFFFFFFFF, 0.09f * a), selected ? 1.6f : 0.5f);

    // 「正在做」的框：iOS 那种「一圈在转」的效果。
    //
    // 做法：把圆角矩形周长采样成 96 段，按「离光头的距离」算强度衰减
    // （三次方，尾巴长、头部亮），同一段画两遍——宽而淡的一遍当辉光，
    // 窄而亮的一遍当芯。光头从正上方出发顺时针走，和 iOS 活动环同向。
    // 底色边框还在，所以就算动画被系统节流也不会看起来像坏了。
    if (row.status == "active") {
      ui::StrokeRound(target, fmt.brush, box, kNodeRadius, ui::Fade(ui::kAccent, 0.50f * a), 1.5f);

      constexpr int kN = 168;  // 采样密到圆点能互相压住，尾巴才是连续的一根锥形
      D2D1_POINT_2F pt[kN + 1];
      const float r = kNodeRadius;
      const float x0 = row.x, y0 = row.y, x1 = row.x + row.w, y1 = row.y + row.h;
      const float sx = (x1 - x0) - 2.0f * r;
      const float sy = (y1 - y0) - 2.0f * r;
      const float arc = 1.5707963f * r;
      const float total = 2.0f * sx + 2.0f * sy + 4.0f * arc;
      // 周长参数化：从左上角接缝开始，先走顶边，顺时针。
      for (int s = 0; s <= kN; ++s) {
        float d = static_cast<float>(s) / kN * total;
        if (d < sx) { pt[s] = D2D1::Point2F(x0 + r + d, y0); continue; }
        d -= sx;
        if (d < arc) { const float k = d / arc * 1.5707963f; pt[s] = D2D1::Point2F(x1 - r + r * std::sin(k), y0 + r - r * std::cos(k)); continue; }
        d -= arc;
        if (d < sy) { pt[s] = D2D1::Point2F(x1, y0 + r + d); continue; }
        d -= sy;
        if (d < arc) { const float k = d / arc * 1.5707963f; pt[s] = D2D1::Point2F(x1 - r + r * std::cos(k), y1 - r + r * std::sin(k)); continue; }
        d -= arc;
        if (d < sx) { pt[s] = D2D1::Point2F(x1 - r - d, y1); continue; }
        d -= sx;
        if (d < arc) { const float k = d / arc * 1.5707963f; pt[s] = D2D1::Point2F(x0 + r - r * std::sin(k), y1 - r + r * std::cos(k)); continue; }
        d -= arc;
        if (d < sy) { pt[s] = D2D1::Point2F(x0, y1 - r - d); continue; }
        d -= sy;
        const float k = d / arc * 1.5707963f;
        pt[s] = D2D1::Point2F(x0 + r - r * std::cos(k), y0 + r - r * std::sin(k));
      }

      // 0.55 圈/秒；光头在最前、尾巴往回收。
      //
      // 上一版画反了：亮的一端跑在后面，看起来像被拖着走。
      // 现在用「离光头多远」当自变量：0 就是光头，越大越靠后、越淡越细。
      // 每一段不画线、画圆点：DrawLine 默认是平头，一段段接起来会出现明显缺口
      // （就是之前看到的"断层"），圆点叠起来才是连续的锥形。
      const float head = static_cast<float>(anim.now % 100000) * 0.00055f;
      const float hl = head - std::floor(head);
      constexpr float kTail = 0.40f;
      for (int s = 0; s <= kN; ++s) {
        float d = hl - static_cast<float>(s) / kN;
        d -= std::floor(d);                    // 0 = 光头，越大越靠后
        if (d > kTail) continue;
        const float k = 1.0f - d / kTail;      // 光头 1，尾端 0
        const float ease = k * k;              // 二次就够，三次会显得头特别短
        // 外层辉光：宽、淡，像光从边框后面泄出来
        fmt.brush->SetColor(ui::Color(ui::Fade(ui::kAccent, (0.03f + 0.20f * ease) * a)));
        target->FillEllipse(D2D1::Ellipse(pt[s], 3.2f + 2.2f * ease, 3.2f + 2.2f * ease), fmt.brush);
        // 芯：白，头部最亮最粗，尾巴细成一根线
        fmt.brush->SetColor(ui::Color(ui::Fade(0xFFFFFFFF, (0.10f + 0.90f * ease) * a)));
        target->FillEllipse(D2D1::Ellipse(pt[s], 0.9f + 1.5f * ease, 0.9f + 1.5f * ease), fmt.brush);
      }
    }
    // 状态点
    fmt.brush->SetColor(ui::Color(ui::Fade(StatusInk(row.status, row.depth > 0), a)));
    const D2D1_POINT_2F dot = D2D1::Point2F(row.x + 16.0f, row.y + 20.0f);
    const float dot_r = row.status == "active" ? 4.6f : 3.6f;
    target->FillEllipse(D2D1::Ellipse(dot, dot_r, dot_r), fmt.brush);

    // 标题：最多两行
    std::wstring line1;
    std::wstring line2;
    SplitTwoLines(WidenNode(row.title), row.w - 44.0f, 12.5f, line1, line2);
    ui::Text(target, line1, fmt.body, D2D1::RectF(row.x + 28.0f, row.y + 11.0f, row.x + row.w - 16.0f, row.y + 32.0f),
             fmt.brush, ui::Fade(ui::kText, a));
    if (!line2.empty()) {
      ui::Text(target, line2, fmt.body,
               D2D1::RectF(row.x + 28.0f, row.y + 31.0f, row.x + row.w - 16.0f, row.y + 52.0f), fmt.brush,
               ui::Fade(ui::kText, a));
    }

    // 右下角徽标：拆过就显示子节点数，没收起用强调色
    if (row.child_count > 0) {
      const std::wstring badge = (row.expanded ? L"−" : L"+") + std::to_wstring(row.child_count);
      const D2D1_RECT_F chip = D2D1::RectF(row.x + row.w - 44.0f, row.y + row.h - 26.0f, row.x + row.w - 10.0f,
                                          row.y + row.h - 8.0f);
      ui::FillRound(target, fmt.brush, chip, 6.0f,
                    row.expanded ? ui::Fade(0x260A84FF, a) : ui::Fade(0x14FFFFFF, a));
      ui::Text(target, badge, fmt.meta, D2D1::RectF(chip.left + 6.0f, chip.top + 1.0f, chip.right, chip.bottom),
               fmt.brush, row.expanded ? ui::Fade(ui::kAccent, a) : ui::Fade(ui::kTextDim, a));
    }
  }

  target->PopAxisAlignedClip();
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));

  // ---- 页脚 ----
  ui::StrokeRound(target, fmt.brush,
                  D2D1::RectF(kPad, height_dip - kFooter + 6.0f, width_dip - kPad, height_dip - kFooter + 6.5f),
                  0.0f, ui::Fade(0xFFFFFFFF, 0.06f * a), 0.5f);
  const std::wstring hint =
      model.busy ? model.busy_text
                 : (model.read_only
                        ? L"↑↓ 选 · Enter 展开/收起 · 拖空白平移 · 滚轮缩放 · Esc 关闭"
                        : L"↑↓ 选 · Enter 再拆一层 · 拖空白平移 · 滚轮缩放 · Ctrl+Enter 就这样 · Esc 不要了");
  ui::Text(target, hint, fmt.meta,
           D2D1::RectF(kPad, height_dip - kFooter + 14.0f, width_dip - kPad - kCommitW - 20.0f, height_dip - 8.0f),
           fmt.brush, ui::Fade(ui::kTextDim, a));

  const D2D1_RECT_F commit = CommitRectAt(width_dip, height_dip);
  ui::FillRound(target, fmt.brush, commit, kCommitH * 0.5f,
                model.busy ? ui::Fade(0xFF3A3A40, a) : ui::Fade(ui::kAccent, a));
  ui::TextCentered(target, model.read_only ? L"知道了" : L"就这样", fmt.body, commit, fmt.brush,
                  ui::Fade(0xFFFFFFFF, a));

  target->SetTransform(D2D1::Matrix3x2F::Identity());
}
}  // namespace dshwork::review
