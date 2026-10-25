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
  // 这一颗是不是文档的「当前任务」（doc.pointer 指的正是它）。
  // 和「选中」是两件事：选中是手里拿着的，当前是文档里站着的那一步。
  bool current = false;
  // 交给 AI 的那条线走到哪了：offered / running / review / accepted / returned，
  // 空 = 没交。右侧「分工」栏靠它分「AI 在做」和「人来做」两摞。
  std::string handoff;
  // 这一步该谁做：human / ai / either；空 = 还没定（三态之外还有"没想过"）。
  std::string executor;

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
  // 文档里的当前任务 id（doc.pointer）。空 = 不知道 / 草稿没有这个概念，
  // 这时一颗都不标 —— 猜错比不标更难看。
  std::string current_id;
  bool busy = false;
  std::wstring busy_text;
  // 页脚上的一句临时说明（「这一步已经做完了」这类），几秒后自己消失。
  // 和 busy 分开：busy 会把按钮一起置灰，一句话的提示不该有那种副作用。
  std::wstring note;
  // 只读模式：看的是已经落库的文档，不是草稿。按钮变成「知道了」，Enter 只展开/收起。
  bool read_only = false;
  // 图里有没有 AI 会话正在跑（节点 handoff.state == running）。
  // 有的话整扇窗描一圈呼吸蓝边 + 流光，让"它在自动做"这件事一眼可见。
  bool ai_running = false;

  // 视图：世界坐标 → 窗口，先乘 zoom 再平移。缩放围绕面板中心，不做鼠标锚定，
  // 因为这个窗口是用来"看清结构"的，不是看图软件。
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  bool fitted = false;  // 第一次排版后自动缩放到合适大小，之后尊重用户的手动视图
  float content_h = 0.0f;  // 内容实际高度（DIP），窗口按它长个
  // 屏幕工作区能给的宽度（DIP）；0 = 不设限，用 kWindowW。屏幕不够宽时窗口退到这里，
  // 免得整条冲出屏幕。由 main.cpp 在排版前按 rcWork 填。
  float avail_w = 0.0f;

  // 「讲讲它」：概念节点右下角那颗按钮 + 它下面浮出来的讲解卡。
  // 卡片内容由宿主给（/api/learn 的 view 字段），这里只存要画的三行字。
  std::string lesson_node;      // 正在讲的是哪一颗（id）；空 = 没开过
  int lesson_phase = 0;         // 0 没开 · 1 正在讲 · 2 讲完了 · 3 没讲成
  std::string lesson_title;     // 卡片标题（「讲讲它」/「这颗节点不在了」…）
  std::string lesson_text;      // 正文一行
  std::string lesson_hint;      // 提示：怎么关掉 / 下一步做什么
  bool lesson_unsupported = false;  // 宿主没有这个接口：按钮置灰，别让人白点

  // 每颗节点「第一次被看到」的时刻，用来让新长出来的框错开淡入。
  std::unordered_map<std::string, ULONGLONG> appear;

  // 右侧那一栏。用户原话：「在基础面板的按钮上有个切换选项，切到学习」「现在的流程
  // 其实就是办公界面」——两者都是"把眼下这张图换个读法"，所以用同一栏、两个页签：
  // 学习 = 这颗在讲什么 + 它拆出来的流程；分工 = 哪些交给 AI、哪些留给人。
  enum class Rail { Off, Learn, Office };
  Rail rail = Rail::Off;
};

struct Anim {
  float open = 1.0f;
  float highlight_y = -1.0f;  // 旧版列表的高亮位置；图模式不再用它，留着接口不折腾调用方
  ULONGLONG now = 0;
  int hover_row = -1;  // 鼠标停在哪一行上（-1 = 没停）：概念节点据此把「讲讲它」提亮
};

constexpr float kHeader = 58.0f;
constexpr float kFooter = 48.0f;
constexpr float kPad = 26.0f;

constexpr float kNodeW = 176.0f;  // 紧凑：一屏能放 6 颗，像 Blender 的节点
constexpr float kNodeH = 58.0f;   // 一行标题 + 一行副标题，压掉大半空白
constexpr float kGapX = 48.0f;   // 列间距：够画连线就行，太宽一屏放不下几颗
constexpr float kGapY = 20.0f;   // 行间距
constexpr float kNodeRadius = 14.0f;
// 整扇窗的圆角。2026-10-25 用户看彩虹边说「边角更圆滑一点」，从 22 加到 30：
// 窗面、描边、AI 彩虹边都用同一个值，三者才不会在拐角处错开。
constexpr float kWindowRadius = 30.0f;

constexpr float kMinZoom = 0.28f;      // 手动缩放的下限：内容再宽也让人自己决定
constexpr float kMinFitZoom = 1.0f;   // 自动适配也不缩小：字就是设计尺寸，放不下就让人自己拖/滚
constexpr float kMaxZoom = 1.60f;

// 一长条是一行排开：一颗占 kNodeW + kGapX = 224 DIP。1600 正好放得下 7 颗
// （7*224-48+2*kPad = 1568），又不用 100% 屏宽——两侧留着桌面，才看得出这是浮层。
// 屏幕工作区比这窄时退回 avail_w（见 WindowWidth），不硬塞出屏。
constexpr float kWindowW = 1600.0f;
constexpr float kWindowH = 720.0f;
constexpr float kCommitW = 108.0f;
constexpr float kCommitH = 32.0f;

// 右侧「学习 / 分工」栏：宽度固定，图区让出这一段。
// 开栏时窗口整体加宽同样的宽度 —— 不然挤窄的只会是节点，字还是那么大，白白换掉一屏节点。
constexpr float kRailW = 300.0f;
constexpr float kRailGap = 16.0f;
constexpr float kRailPad = 18.0f;
// 头带上那两颗切换按钮（学习 / 分工）
constexpr float kRailChipW = 52.0f;
constexpr float kRailChipH = 22.0f;
constexpr float kRailChipGap = 8.0f;

inline bool RailOn(const Model& m) { return m.rail != Model::Rail::Off; }

inline float WindowWidth(const Model& m) {
  float base = (m.avail_w > 0.0f && m.avail_w < kWindowW) ? m.avail_w : kWindowW;
  if (RailOn(m)) base += kRailW + kRailGap;
  // 栏开了也不能冲出屏幕：屏幕比这还窄时，宁可挤图区。
  if (m.avail_w > 0.0f && base > m.avail_w) base = m.avail_w;
  return base;
}
// 窗口高度跟着内容长：一层节点就是一条扁条，展开子层再长高。
// 夹在 [kMinWindowH, kWindowH] 之间：太小放不下页脚，太大就失去浮层的轻快感。
constexpr float kMinWindowH = 260.0f;

// 「讲讲它」：按钮贴在概念节点右下角内侧，讲解卡浮在图区底部。
// 两者都占地方：按钮要挤掉第二行标题的右端，卡片要让窗口临时长高。
constexpr float kLessonChipW = 50.0f;
constexpr float kLessonChipH = 17.0f;
constexpr float kLessonChipInset = 8.0f;   // 距节点右/下边
constexpr float kLessonCardH = 78.0f;
constexpr float kLessonCardGap = 10.0f;    // 卡片与图区底边的间距

inline bool LessonCardOn(const Model& m) { return m.lesson_phase == 2 || m.lesson_phase == 3; }
/** 开卡时窗口要长出来的高度：不预留就会把最下面一排节点盖掉。 */
inline float LessonReserve(const Model& m) { return LessonCardOn(m) ? kLessonCardH + kLessonCardGap : 0.0f; }

inline float WindowHeight(const Model& m) {
  if (m.content_h <= 0.0f) return kWindowH;
  // 开右栏时窗口至少要够"标题 + 四行说明 + 三条流程"——不然那一栏变成一条缝，
  // 字挤在一起还不如不开。
  const float min_h = (RailOn(m) ? 400.0f : kMinWindowH) + LessonReserve(m);
  const float want = kHeader + kFooter + m.content_h + kPad * 2.0f + LessonReserve(m);
  return want < min_h ? min_h : (want > kWindowH ? kWindowH : want);
}
// 图区宽度：开了右栏就把那一段让出去（排版、缩放、命中都用这一个数）。
inline float BodyWidth(const Model& m) {
  const float full = WindowWidth(m);
  return RailOn(m) ? (full - kRailW - kRailGap) : full;
}
inline float BodyHeight(const Model& m) { return WindowHeight(m) - kHeader - kFooter; }

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

  // 一长条：x 按深度优先顺序一路往右排，y 按层级下沉。
  // 折行会打断「一条线走到头」的读法，宁可让人横向拖。
  for (int i = 0; i < count; ++i) {
    Row& row = model.rows[static_cast<size_t>(i)];
    row.w = kNodeW;
    row.h = kNodeH;
    row.x = static_cast<float>(i) * (kNodeW + kGapX);
    row.y = static_cast<float>(row.depth) * (kNodeH + kGapY);
  }

  // 记下内容真实高度：窗口按它长个（一层节点 = 一条扁条，展开子层再长高）。
  float max_bottom = 0.0f;
  for (const Row& row : model.rows) {
    const float bottom = row.y + row.h;
    if (bottom > max_bottom) max_bottom = bottom;
  }
  model.content_h = max_bottom;
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
  // 自动适配时别把字缩到看不清：够不着就让人自己拖，不硬塞
  if (zoom < kMinFitZoom) zoom = kMinFitZoom;
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
  // 开右栏时按钮跟着图区右缘走，别钻到栏底下。
  const D2D1_RECT_F rect = CommitRectAt(BodyWidth(model), WindowHeight(model));
  return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

/**
 * 「讲讲它」按钮的命中区。
 *
 * 用世界坐标算、单独一遍倒序循环：按钮在节点矩形的右下角，和节点本身重叠，
 * 所以点按钮必须先于「选中这颗节点」判定——不然永远点不中。
 * 命中区比画出来的框四周各放大 3 DIP：一个 17 DIP 高的小按钮，
 * 差一个像素就点空，比视觉精确更重要。
 */
inline D2D1_RECT_F LessonChipRect(const Row& row) {
  const float right = row.x + row.w - kLessonChipInset;
  const float bottom = row.y + row.h - 5.0f;
  return D2D1::RectF(right - kLessonChipW, bottom - kLessonChipH, right, bottom);
}

inline int LessonChipAt(const Model& model, float x, float y) {
  if (!model.fitted) return -1;
  const float wx = (x - model.pan_x) / model.zoom;
  const float wy = (y - kHeader - model.pan_y) / model.zoom;
  for (int i = static_cast<int>(model.rows.size()) - 1; i >= 0; --i) {
    const Row& row = model.rows[static_cast<size_t>(i)];
    if (row.kind != "concept") continue;
    const D2D1_RECT_F chip = LessonChipRect(row);
    constexpr float kSlop = 3.0f;
    if (wx >= chip.left - kSlop && wx <= chip.right + kSlop && wy >= chip.top - kSlop &&
        wy <= chip.bottom + kSlop) {
      return i;
    }
  }
  return -1;
}

/**
 * 右上角那颗「+N / −N」徽标的命中区（展开/收起）。
 *
 * 为什么要有它：单击节点现在会被拿去看详解，而「再点同一颗 = 拆开」是个没写在
 * 任何地方的隐式手势，而且开卡时窗口会变高、图可能重新自适应，同一屏幕坐标第二次
 * 点下去往往已经不在这颗节点上（实测会点到「讲讲它」）。给徽标一个真的命中区，
 * 「拆」就有了一个看得见、点得着、不会被别的手势抢走的入口。
 * 命中区比画出来的框四周各放大 3 DIP，理由同 LessonChipRect。
 */
inline D2D1_RECT_F ExpandBadgeRect(const Row& row) {
  return D2D1::RectF(row.x + row.w - 44.0f, row.y + 8.0f, row.x + row.w - 10.0f, row.y + 26.0f);
}

inline int ExpandBadgeAt(const Model& model, float x, float y) {
  if (!model.fitted) return -1;
  const float wx = (x - model.pan_x) / model.zoom;
  const float wy = (y - kHeader - model.pan_y) / model.zoom;
  for (int i = static_cast<int>(model.rows.size()) - 1; i >= 0; --i) {
    const Row& row = model.rows[static_cast<size_t>(i)];
    if (row.child_count <= 0) continue;
    const D2D1_RECT_F badge = ExpandBadgeRect(row);
    constexpr float kSlop = 3.0f;
    if (wx >= badge.left - kSlop && wx <= badge.right + kSlop && wy >= badge.top - kSlop &&
        wy <= badge.bottom + kSlop) {
      return i;
    }
  }
  return -1;
}

/** 讲解卡：图区最下面那一条，左右各让 kPad，压在节点之上。 */
inline D2D1_RECT_F LessonCardRect(float width_dip, float height_dip) {
  const float bottom = height_dip - kFooter - kLessonCardGap;
  return D2D1::RectF(kPad, bottom - kLessonCardH, width_dip - kPad, bottom);
}

/** 右侧那栏的矩形（定义在下面 LessonCardHit 之后；这里先声明，画栏要用）。 */
inline D2D1_RECT_F RailRect(float width_dip, float height_dip);
/** UTF-8 → UTF-16（定义在文件后半段；画栏先用到）。 */
inline std::wstring WidenNode(const std::string& text);

/** 把一段中文按字数硬折行：这个分层窗口里 DWrite 不做自动换行（见 FitText 的注释）。 */
inline std::vector<std::wstring> WrapRough(const std::wstring& text, size_t per_line, size_t max_lines = 4) {
  std::vector<std::wstring> lines;
  if (text.empty() || per_line == 0 || max_lines == 0) return lines;
  size_t i = 0;
  while (i < text.size() && lines.size() < max_lines) {
    lines.push_back(text.substr(i, per_line));
    i += per_line;
  }
  if (i < text.size() && !lines.empty()) lines.back() += L"…";
  return lines;
}

/** 右侧那栏的矩形（定义在下面 LessonCardHit 之后；这里先声明，画栏要用）。 */
inline D2D1_RECT_F RailRect(float width_dip, float height_dip);

/**
 * 右侧「学习 / 分工」栏。
 *
 * 一栏两用：学习页签回答"这颗在讲什么、它拆出什么步骤"，分工页签回答"哪些交给 AI、
 * 哪些留给人"。两者都是把同一张图换个读法，所以共用宽度、共用位置，只是内容不同。
 * 栏开了以后图区会变窄（BodyWidth 让出 kRailW + kRailGap），节点跟着重排。
 */
inline void DrawRail(ID2D1RenderTarget* target, const Model& model, const PanelFormats& fmt, float width_dip,
                     float height_dip, float alpha) {
  if (!RailOn(model)) return;
  const D2D1_RECT_F rail = RailRect(width_dip, height_dip);
  ui::FillRound(target, fmt.brush, rail, 16.0f, ui::Fade(0xFF191B22, alpha));
  ui::StrokeRound(target, fmt.brush, rail, 16.0f, ui::Fade(0xFFFFFFFF, 0.07f * alpha), 0.5f);

  const float left = rail.left + kRailPad;
  const float right = rail.right - kRailPad;
  const float limit = rail.bottom - 6.0f;
  float y = rail.top + 14.0f;

  const bool learn = model.rail == Model::Rail::Learn;
  ui::Text(target, learn ? L"学习" : L"分工", fmt.title, D2D1::RectF(left, y, right, y + 20.0f), fmt.brush,
           ui::Fade(ui::kText, alpha));
  y += 28.0f;

  const Row* sel = nullptr;
  if (model.selected >= 0 && static_cast<size_t>(model.selected) < model.rows.size()) {
    sel = &model.rows[static_cast<size_t>(model.selected)];
  }

  if (learn) {
    // 学哪一颗：Tab / 上下键换，标题跟着走。
    const std::wstring heading = sel != nullptr ? WidenNode(sel->title) : L"先点一颗节点";
    ui::Text(target, ui::FitText(heading, right - left, 11.5f), fmt.body, D2D1::RectF(left, y, right, y + 18.0f),
             fmt.brush, ui::Fade(sel != nullptr ? ui::kText : ui::kTextDim, alpha));
    y += 26.0f;

    ui::Text(target, L"基础知识", fmt.meta, D2D1::RectF(left, y, right, y + 16.0f), fmt.brush,
             ui::Fade(ui::kAccent, alpha));
    y += 20.0f;
    std::string body;
    if (sel != nullptr) body = sel->detail.empty() ? sel->acceptance : sel->detail;
    const std::wstring wide = body.empty() ? std::wstring(L"（这颗没写基础说明）") : WidenNode(body);
    for (const std::wstring& line : WrapRough(wide, 20, 4)) {
      if (y + 16.0f > limit) break;
      ui::Text(target, line, fmt.meta, D2D1::RectF(left, y, right, y + 16.0f), fmt.brush,
               ui::Fade(ui::kTextDim, alpha));
      y += 17.0f;
    }

    y += 10.0f;
    if (y + 16.0f <= limit) {
      ui::Text(target, L"详解流程", fmt.meta, D2D1::RectF(left, y, right, y + 16.0f), fmt.brush,
               ui::Fade(ui::kAccent, alpha));
      y += 20.0f;
    }
    int shown = 0;
    if (model.selected >= 0) {
      for (size_t i = 0; i < model.rows.size() && shown < 3; ++i) {
        if (model.rows[i].parent != model.selected) continue;
        if (y + 16.0f > limit) break;
        const std::wstring line = std::to_wstring(shown + 1) + L". " + WidenNode(model.rows[i].title);
        ui::Text(target, ui::FitText(line, right - left, 11.0f), fmt.meta, D2D1::RectF(left, y, right, y + 16.0f),
                 fmt.brush, ui::Fade(ui::kTextDim, alpha));
        y += 17.0f;
        shown += 1;
      }
    }
    if (shown == 0 && y + 16.0f <= limit) {
      ui::Text(target, L"（还没拆出下一步：点右上 +N 拆一颗）", fmt.meta, D2D1::RectF(left, y, right, y + 16.0f), fmt.brush,
               ui::Fade(ui::kTextDim, alpha));
    }
    return;
  }

  // 分工：同一棵树按「谁来做、做到哪一步」重排。做完/跳过的先不算 —— 它们已经不在盘子里了。
  //
  // 第一版只分「AI 来做 / 人来做」两摞，靠 handoff 在不在来推测归属；那是过程痕迹不是意图：
  // 一颗没派的节点和一颗明确要人做的节点长得一模一样。用户给了显式 executor 字段之后，
  // 改成按归属分四摞，AI 那摞里再缀上这条线走到哪了（等开工 / 正在做 / 等你验收 / 退回来了）。
  struct Bucket {
    const char* executor;   // ai / human / either；空串 = 还没标
    const wchar_t* head;    // 小节标题（不含数量）
    bool accent;            // 标题用蓝色（AI 那头）
  };
  static const Bucket kBuckets[] = {
      {"ai", L"AI 做", true},
      {"human", L"人做", false},
      {"either", L"都可以", false},
      {"", L"还没定谁做", false},
  };

  // AI 那摞里的小尾巴：handoff.state 翻成人话，空表示还没派出去。
  const auto stateTag = [](const std::string& handoff) -> std::wstring {
    if (handoff == "running") return L" · 正在做";
    if (handoff == "review") return L" · 等你验收";
    if (handoff == "offered") return L" · 等开工";
    if (handoff == "returned") return L" · 退回来了";
    return L"";
  };

  for (const Bucket& bucket : kBuckets) {
    // 归属判定：标了谁就归谁；标了 AI 但还没派出去也归 AI 那摞；
    // 没标的里面，凡是已经派给 AI 的（有 handoff）也算 AI，剩下的才是「还没定」。
    const auto inBucket = [&bucket](const Row& row) -> bool {
      if (bucket.executor[0] == '\0') return row.executor.empty() && row.handoff.empty();
      if (row.executor == bucket.executor) return true;
      return std::string(bucket.executor) == "ai" && !row.handoff.empty();
    };
    int total = 0;
    for (const Row& row : model.rows) {
      if (row.status == "done" || row.status == "skipped") continue;
      if (inBucket(row)) total += 1;
    }
    if (total == 0) continue;  // 空的那摞不占地方：四摞全画出来，右栏会被撑爆
    if (y + 16.0f > limit) break;
    const std::wstring head = std::wstring(bucket.head) + L"（" + std::to_wstring(total) + L"）";
    ui::Text(target, head, fmt.meta, D2D1::RectF(left, y, right, y + 16.0f), fmt.brush,
             ui::Fade(bucket.accent ? ui::kAccent : ui::kText, alpha));
    y += 20.0f;
    for (const Row& row : model.rows) {
      if (row.status == "done" || row.status == "skipped") continue;
      if (!inBucket(row)) continue;
      if (y + 16.0f > limit) break;
      const std::wstring line =
          L"· " + WidenNode(row.title) + (bucket.accent ? stateTag(row.handoff) : std::wstring());
      ui::Text(target, ui::FitText(line, right - left, 11.0f), fmt.meta,
               D2D1::RectF(left, y, right, y + 16.0f), fmt.brush, ui::Fade(ui::kTextDim, alpha));
      y += 17.0f;
    }
    y += 8.0f;
  }
  if (y + 16.0f <= limit) {
    // 五摞全空 = 盘子里只剩收尾的了；说清楚，别让人以为栏坏了。
    bool any = false;
    for (const Row& row : model.rows) {
      if (row.status != "done" && row.status != "skipped") {
        any = true;
        break;
      }
    }
    if (!any) {
      ui::Text(target, L"（都收尾了，没有要派的）", fmt.meta, D2D1::RectF(left, y, right, y + 16.0f), fmt.brush,
               ui::Fade(ui::kTextDim, alpha));
    }
  }
}

inline bool LessonCardHit(const Model& model, float x, float y) {
  if (!LessonCardOn(model)) return false;
  const D2D1_RECT_F card = LessonCardRect(BodyWidth(model), WindowHeight(model));
  return x >= card.left && x <= card.right && y >= card.top && y <= card.bottom;
}

/** 右侧那栏的矩形：贴着右边缘，从标题栏下方一直到底部页脚上方。 */
inline D2D1_RECT_F RailRect(float width_dip, float height_dip) {
  const float right = width_dip - kRailPad;
  const float left = right - kRailW;
  return D2D1::RectF(left, kHeader + 10.0f, right, height_dip - kFooter - 10.0f);
}

/**
 * 头带上那两颗切换按钮（0 = 学习，1 = 分工）。
 *
 * 用户要的是「在基础面板的按钮上有个切换选项」——网页玻璃条那边也有入口，
 * 但人一旦站在图里，最顺手的还是就地切，所以头带上再放一对。
 */
inline D2D1_RECT_F RailChipRect(const Model& model, int which) {
  const float width_dip = WindowWidth(model);
  // 「N 件事」占着最右 130 DIP，两个按钮排在它左边（再留 10 DIP 空隙，
  // 否则按钮的圆角会压在计数文字上 —— 第一版就是这么糊在一起的）。
  const float right = width_dip - kPad - 140.0f - static_cast<float>(1 - which) * (kRailChipW + kRailChipGap);
  return D2D1::RectF(right - kRailChipW, kHeader * 0.5f - kRailChipH * 0.5f + 4.0f, right,
                     kHeader * 0.5f + kRailChipH * 0.5f + 4.0f);
}

/** 点在哪个切换按钮上：0 = 学习，1 = 分工，-1 = 都没点。 */
inline int RailChipAt(const Model& model, float x, float y) {
  for (int i = 0; i < 2; ++i) {
    const D2D1_RECT_F chip = RailChipRect(model, i);
    if (x >= chip.left && x <= chip.right && y >= chip.top && y <= chip.bottom) return i;
  }
  return -1;
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

/**
 * 一行字放不下就拆成两行：第一行尽量长，第二行给省略号。
 *
 * second_w 是给概念节点留的：右下角那颗「讲讲它」会占掉第二行的右边，
 * 不给它让位，字就会从按钮底下钻过去。
 */
inline void SplitTwoLines(const std::wstring& text, float max_w, float font_dip, std::wstring& line1,
                          std::wstring& line2, float second_w = -1.0f) {
  const float tail_w = second_w < 0.0f ? max_w : second_w;
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
  line2 = ui::FitText(text.substr(cut), tail_w, font_dip);
}

/* ─────────────────────────── 连线的几何 ─────────────────────────── */

// 一条边：短横 → 竖 → 横。
//
// 斜切贝塞尔在「父在上、子在下」时只是一条缓坡，跨行时到底谁连谁得顺着看；
// 直角折线把「出门 → 落下 → 进门」三段讲清楚，一眼就能对上号。同行相邻
// 还是直线，不为了统一把它也掰弯。
//
// 拐角用二次曲线切个小圆角：半径小到看不出是圆角，但缩放后不会有折线的硬刺。
// 采样按弧长均分——流光的位置是「走了百分之几」，采样点不匀就会在长段上一跳
// 一跳、过拐角时还闪；这里直线段每 ~8 DIP 一个点，拐角固定 5 个点。
constexpr float kCornerR = 4.0f;    // 拐角半径（DIP）
constexpr float kStubX = 14.0f;     // 出父节点后先横着走的一小段
constexpr int kMaxEdgePoints = 72;  // 一条边的采样上限：>300 DIP 的竖直段也够平滑
constexpr float kEdgeStep = 8.0f;   // 直线段的采样步长（DIP）

struct EdgePath {
  D2D1_POINT_2F points[kMaxEdgePoints + 1];
  float lengths[kMaxEdgePoints + 1] = {};  // 累计弧长，流光按它定位
  int count = 0;
  float total = 0.0f;
  D2D1_POINT_2F arrow = {};  // 方向标落点：由 BuildEdge 定，竖线上画「>」会指错方向
};

inline void EdgePush(EdgePath& path, float x, float y) {
  if (path.count > kMaxEdgePoints) return;
  const int s = path.count;
  path.points[s] = D2D1::Point2F(x, y);
  if (s > 0) {
    const float dx = x - path.points[s - 1].x;
    const float dy = y - path.points[s - 1].y;
    path.total += std::sqrt(dx * dx + dy * dy);
  }
  path.lengths[s] = path.total;
  path.count = s + 1;
}

/** 直线段：长段多采几个点，流光在同一段上的速度才不靠运气。 */
inline void EdgeLine(EdgePath& path, float x0, float y0, float x1, float y1) {
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);
  int steps = static_cast<int>(len / kEdgeStep) + 1;
  if (steps > 40) steps = 40;
  for (int s = 1; s <= steps; ++s) {
    const float t = static_cast<float>(s) / static_cast<float>(steps);
    EdgePush(path, x0 + dx * t, y0 + dy * t);
  }
}

/** 小圆角：二次贝塞尔，控制点就是那个尖角，曲线自然贴着两边走。 */
inline void EdgeCorner(EdgePath& path, float x0, float y0, float cx, float cy, float x1, float y1) {
  constexpr int kSteps = 5;
  for (int s = 1; s <= kSteps; ++s) {
    const float t = static_cast<float>(s) / kSteps;
    const float u = 1.0f - t;
    EdgePush(path, u * u * x0 + 2.0f * u * t * cx + t * t * x1,
             u * u * y0 + 2.0f * u * t * cy + t * t * y1);
  }
}

/**
 * 铺一条边的采样点：起点由这里写，调用方只管拿 points/lengths。
 *
 * 同一行（纵向差 < 0.5）就是直线；跨行才走「短横 → 竖 → 横」。
 * 手写比较：这个 TU 里有 windows.h，std::min/max/clamp 会被宏吃掉。
 */
inline void BuildEdge(EdgePath& path, float fx, float fy, float tx, float ty) {
  path = EdgePath();  // 聚合初始化，points/lengths 一起清零
  EdgePush(path, fx, fy);
  path.arrow = D2D1::Point2F((fx + tx) * 0.5f, (fy + ty) * 0.5f);

  const float dy = ty - fy;
  if (dy > -0.5f && dy < 0.5f) {
    EdgeLine(path, fx, fy, tx, ty);  // 同行：还是直线
    return;
  }

  const float dx = tx - fx;
  float stub = kStubX;
  if (stub > dx * 0.6f) stub = dx * 0.6f;
  // 间距窄到摆不下「短横 → 竖 → 横」时退回直连：画出回勾比斜线更难读。
  if (stub < 1.0f || dx <= kCornerR * 2.0f) {
    EdgeLine(path, fx, fy, tx, ty);
    return;
  }

  const float corner_x = fx + stub;
  const float down = dy > 0.0f ? 1.0f : -1.0f;
  const float vlen = dy > 0.0f ? dy : -dy;

  // 半径别把竖直段和短横吃光：窄间距时自动缩，缩没了就画硬拐角。
  float r = kCornerR;
  if (r > vlen * 0.4f) r = vlen * 0.4f;
  if (r > stub * 0.5f) r = stub * 0.5f;
  if (r < 0.5f) r = 0.0f;

  if (r <= 0.0f) {
    EdgeLine(path, fx, fy, corner_x, fy);
    EdgeLine(path, corner_x, fy, corner_x, ty);
    EdgeLine(path, corner_x, ty, tx, ty);
  } else {
    EdgeLine(path, fx, fy, corner_x - r, fy);
    EdgeCorner(path, corner_x - r, fy, corner_x, fy, corner_x, fy + down * r);
    EdgeLine(path, corner_x, fy + down * r, corner_x, ty - down * r);
    EdgeCorner(path, corner_x, ty - down * r, corner_x, ty, corner_x + r, ty);
    EdgeLine(path, corner_x + r, ty, tx, ty);
  }

  // 方向标落在最后那段横线的中间：靠近子节点但不贴框，指向「进门」的方向。
  path.arrow = D2D1::Point2F((corner_x + r + tx) * 0.5f, ty);
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
  ui::FillRound(target, fmt.brush, D2D1::RectF(0.0f, 0.0f, width_dip, height_dip), kWindowRadius, bg);
  ui::StrokeRound(target, fmt.brush, D2D1::RectF(0.5f, 0.5f, width_dip - 0.5f, height_dip - 0.5f), kWindowRadius,
                  ui::Fade(0xFFFFFFFF, 0.08f * a), 0.5f);

  // ---- 标题栏 ----
  const std::wstring heading = model.title.empty() ? L"任务树" : model.title;
  ui::Text(target, heading, fmt.title, D2D1::RectF(kPad, 16.0f, width_dip - kPad - 200.0f, 42.0f), fmt.brush,
           ui::Fade(ui::kText, a));
  const std::wstring count = std::to_wstring(model.total) + L" 件事";
  ui::Text(target, count, fmt.meta, D2D1::RectF(width_dip - kPad - 130.0f, 20.0f, width_dip - kPad, 40.0f),
           fmt.brush, ui::Fade(ui::kTextDim, a));

  // 头带上的「学习 / 分工」切换：网页玻璃条上也有入口，但人在图里时最顺手的是就地切。
  // 开着的那个填色、字变白，另一个只留描边 —— 一眼看得出现在是哪种读法。
  for (int i = 0; i < 2; ++i) {
    const bool on = (i == 0 && model.rail == Model::Rail::Learn) || (i == 1 && model.rail == Model::Rail::Office);
    const D2D1_RECT_F chip = RailChipRect(model, i);
    ui::FillRound(target, fmt.brush, chip, 7.0f, ui::Fade(on ? 0x330A84FF : 0x10FFFFFF, a));
    ui::StrokeRound(target, fmt.brush, chip, 7.0f, ui::Fade(on ? ui::kAccent : 0xFFFFFFFF, (on ? 0.9f : 0.14f) * a),
                    0.7f);
    ui::TextCentered(target, i == 0 ? L"学习" : L"分工", fmt.meta, chip, fmt.brush,
                     ui::Fade(on ? 0xFFFFFFFF : ui::kTextDim, a));
  }

  // ---- 图区 ----
  const D2D1_RECT_F body = D2D1::RectF(0.0f, kHeader, width_dip, height_dip - kFooter);
  target->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
  target->SetTransform(D2D1::Matrix3x2F::Scale(model.zoom, model.zoom) *
                       D2D1::Matrix3x2F::Translation(model.pan_x, kHeader + model.pan_y) *
                       D2D1::Matrix3x2F::Scale(scale, scale));

  // 连线：父的右边中点 → 子的左边中点。几何由 BuildEdge 铺：同行是直线，
  // 跨行走「短横 → 竖 → 横」，拐角带 4 DIP 小圆角。
  //
  // 线上跑一颗「光点」：把折线按弧长归一化，再让一段亮弧随时间平移。方向是
  // 父→子，所以光是朝思路前进的方向流的，扫一眼就知道下一步在哪。
  static ID2D1Factory* factory = nullptr;
  if (factory == nullptr) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);

  // ---- 轮动：一次只亮一条边，从第一颗轮到最后一颗 ----
  //
  // 用户原话是「轮动展示」（追问后选的正是「节点图里节点间的流光线节奏」）。
  // 原来是每条边各自错相位（phase = flow_phase + i * 0.11），所有边同时在流，
  // 看上去像一条并行流水线，反而看不出"现在走到哪一步"。
  // 现在整张图共用一个时间槽：每条边占「走完 0.9s + 空档 0.1s」，轮完最后一条回第一条。
  constexpr double kEdgeTravelMs = 900.0;  // 一条边从头走到尾
  constexpr double kEdgeGapMs = 100.0;     // 交接空档：整张图暗一下，眼睛才分得清换了条边
  constexpr double kEdgeSlotMs = kEdgeTravelMs + kEdgeGapMs;
  constexpr float kTailLen = 0.18f;        // 亮弧占整条边的比例
  const size_t edge_count = model.rows.size() > 1 ? model.rows.size() - 1 : 0;
  size_t active_edge = 0;
  float flow_head = -1.0f;  // < 0 表示此刻不画流光（空档期）
  if (edge_count > 0) {
    const double clock = static_cast<double>(anim.now);
    const double slot = std::floor(clock / kEdgeSlotMs);
    active_edge = static_cast<size_t>(slot) % edge_count;
    const double in_slot = clock - slot * kEdgeSlotMs;
    if (in_slot < kEdgeTravelMs) {
      // 出发前先让尾巴探出来、到达后让尾巴走完：t 0→1 映射到 head −尾巴→1。
      const float t = static_cast<float>(in_slot / kEdgeTravelMs);
      flow_head = t * (1.0f + kTailLen) - kTailLen;
    }
  }
  // 连线按「深度优先的先后」串起来：每颗接下一颗，而不是只连父子。
  // 这样一屏就是一条从头走到尾的流水线，而不是几段互不相连的树枝。
  for (size_t i = 0; i + 1 < model.rows.size(); ++i) {
    const Row& row = model.rows[i];
    const Row& next = model.rows[i + 1];
    const D2D1_POINT_2F from = D2D1::Point2F(row.x + row.w, row.y + row.h * 0.5f);
    const D2D1_POINT_2F to = D2D1::Point2F(next.x - 2.0f, next.y + next.h * 0.5f);

    EdgePath edge;
    BuildEdge(edge, from.x, from.y, to.x, to.y);
    if (edge.count < 2 || edge.total <= 0.01f) continue;

    // 底线
    fmt.brush->SetColor(ui::Color(ui::Fade(0xFFFFFFFF, 0.22f * a)));
    for (int s = 0; s + 1 < edge.count; ++s) {
      target->DrawLine(edge.points[s], edge.points[s + 1], fmt.brush, 1.6f);
    }

    // 流光：只画当前轮到的这条边，其余边保持底线、静止。
    // 位置按累计弧长算，拐角只是折线上的普通一点，所以不会重来一段、也不掉速。
    if (flow_head >= 0.0f && active_edge == i) {
      for (int s = 0; s + 1 < edge.count; ++s) {
        const float behind = flow_head - (edge.lengths[s] / edge.total);  // >0 = 这段在光头后面
        if (behind >= 0.0f && behind <= kTailLen) {
          const float k = 1.0f - behind / kTailLen;  // 0..1，越接近光头越亮
          fmt.brush->SetColor(ui::Color(ui::Fade(ui::kAccent, (0.10f + 0.55f * k) * a)));
          target->DrawLine(edge.points[s], edge.points[s + 1], fmt.brush, 1.9f);
        }
      }
      // 光头再点一颗小圆点：18% 的尾巴在长边上只是一小段，轮动时不够扎眼。
      if (flow_head <= 1.0f) {
        for (int s = 0; s + 1 < edge.count; ++s) {
          const float a0 = edge.lengths[s] / edge.total;
          const float a1 = edge.lengths[s + 1] / edge.total;
          if (flow_head >= a0 && flow_head <= a1) {
            const float span = a1 - a0;
            const float t = span > 0.0001f ? (flow_head - a0) / span : 0.0f;
            const D2D1_POINT_2F p = D2D1::Point2F(
                edge.points[s].x + (edge.points[s + 1].x - edge.points[s].x) * t,
                edge.points[s].y + (edge.points[s + 1].y - edge.points[s].y) * t);
            fmt.brush->SetColor(ui::Color(ui::Fade(0xFFFFFFFF, 0.80f * a)));
            target->FillEllipse(D2D1::Ellipse(p, 2.2f, 2.2f), fmt.brush);
            break;
          }
        }
      }
    }
    // 方向标：一颗很小的 ">"，落在 BuildEdge 选好的位置（同行是中段，
    // 折线是最后那段横线），不贴框、也不在竖线上指错方向。
    {
      const D2D1_POINT_2F c = edge.arrow;
      fmt.brush->SetColor(ui::Color(ui::Fade(0xFFFFFFFF, 0.20f * a)));
      target->DrawLine(D2D1::Point2F(c.x - 3.2f, c.y - 3.4f), D2D1::Point2F(c.x + 1.4f, c.y), fmt.brush, 1.3f);
      target->DrawLine(D2D1::Point2F(c.x - 3.2f, c.y + 3.4f), D2D1::Point2F(c.x + 1.4f, c.y), fmt.brush, 1.3f);
    }
    (void)factory;
  }

  // 节点
  for (size_t i = 0; i < model.rows.size(); ++i) {
    const Row& row = model.rows[i];
    const bool selected = static_cast<int>(i) == model.selected;
    const D2D1_RECT_F box = D2D1::RectF(row.x, row.y, row.x + row.w, row.y + row.h);
    // 概念节点不改颜色：试过紫色，实际看起来比原来的冷灰底脏，撤回。
    const UINT32 fill = selected ? ui::Fade(0xFF1B2A44, a) : ui::Fade(0xFF1B1D24, a);
    ui::FillRound(target, fmt.brush, box, kNodeRadius, fill);
    ui::StrokeRound(target, fmt.brush, box, kNodeRadius,
                    selected ? ui::Fade(ui::kAccent, 0.95f * a) : ui::Fade(0xFFFFFFFF, 0.09f * a),
                    selected ? 1.6f : 0.5f);

    // 「当前任务」：左侧一条强调色竖标。
    //
    // 为什么另起一样东西：状态点变色和那圈转动都在"动"，静止的一帧里看不出
    // 人此刻站在哪一颗上；而且 pointer 也可能指着颗 todo，那时没有转圈可看。
    // 这一条不动、不闪、不抢标题的位置（左边那条 24 DIP 的空槽就是给它留的）。
    if (row.current) {
      ui::FillRound(target, fmt.brush,
                    D2D1::RectF(row.x + 5.0f, row.y + 11.0f, row.x + 8.5f, row.y + row.h - 11.0f), 1.75f,
                    ui::Fade(ui::kAccent, 0.95f * a));
    }

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
    const D2D1_POINT_2F dot = D2D1::Point2F(row.x + 16.0f, row.y + 18.0f);
    const float dot_r = row.status == "active" ? 4.6f : 3.6f;
    target->FillEllipse(D2D1::Ellipse(dot, dot_r, dot_r), fmt.brush);

    // 标题：最多两行。
    // 这里必须按节点矩形裁剪：DWrite 排版高度比框高时会把字画到框外，
    // 看着就像「字被切掉一半」。节点是圆角矩形，用矩形裁剪够用了。
    target->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    std::wstring line1;
    std::wstring line2;
    const bool concept = row.kind == "concept";
    const float text_w = row.w - 44.0f - (row.child_count > 0 ? 40.0f : 0.0f);
    // 概念节点的第二行要给右下角的「讲讲它」让位，否则最后几个字会压在按钮底下。
    const float tail_w = concept ? text_w - (kLessonChipW + 8.0f) : text_w;
    SplitTwoLines(WidenNode(row.title), text_w, 16.0f, line1, line2, tail_w);
    ui::Text(target, line1, fmt.node, D2D1::RectF(row.x + 28.0f, row.y + 7.0f, row.x + row.w - (row.child_count > 0 ? 58.0f : 14.0f), row.y + 31.0f),
             fmt.brush, ui::Fade(ui::kText, a));
    if (!line2.empty()) {
      // 第二行也是标题的一部分，用同一个字号；不要降成正文，否则标题读起来像正文带注释。
      ui::Text(target, line2, fmt.node,
               D2D1::RectF(row.x + 28.0f, row.y + 31.0f, row.x + row.w - (row.child_count > 0 ? 58.0f : 14.0f), row.y + 59.0f), fmt.brush,
               ui::Fade(ui::kText, a));
    }

    // 右下角徽标：拆过就显示子节点数，没收起用强调色
    if (row.child_count > 0) {
      const std::wstring badge = (row.expanded ? L"−" : L"+") + std::to_wstring(row.child_count);
      const D2D1_RECT_F chip = D2D1::RectF(row.x + row.w - 44.0f, row.y + 8.0f, row.x + row.w - 10.0f,
                                          row.y + 26.0f);
      ui::FillRound(target, fmt.brush, chip, 6.0f,
                    row.expanded ? ui::Fade(0x260A84FF, a) : ui::Fade(0x14FFFFFF, a));
      ui::Text(target, badge, fmt.meta, D2D1::RectF(chip.left + 6.0f, chip.top + 1.0f, chip.right, chip.bottom),
               fmt.brush, row.expanded ? ui::Fade(ui::kAccent, a) : ui::Fade(ui::kTextDim, a));
    }
    // 概念节点：右下角一颗「讲讲它」。
    // 之前这里只有一个「学」字——看得出来是概念，但没有一个地方能点，
    // 用户原话是「不然不知道怎么打开」。现在它是一颗真的按钮：
    // 悬停提亮、正在讲时换字、宿主没有这个接口时置灰（不装成能用）。
    if (concept) {
      const bool busy = model.lesson_phase == 1 && model.lesson_node == row.id;
      const bool hot = static_cast<int>(i) == anim.hover_row;
      const bool ready = !model.lesson_unsupported;
      const D2D1_RECT_F lesson_pill = LessonChipRect(row);
      const float radius = kLessonChipH * 0.5f;
      UINT32 pill_fill = 0x240A84FF;
      float edge = 0.45f;
      if (!ready) {
        pill_fill = 0x12FFFFFF;
        edge = 0.16f;
      } else if (busy) {
        pill_fill = 0x330A84FF;
        edge = 0.85f;
      } else if (hot) {
        pill_fill = 0x3A0A84FF;
        edge = 0.75f;
      }
      ui::FillRound(target, fmt.brush, lesson_pill, radius, ui::Fade(pill_fill, a));
      ui::StrokeRound(target, fmt.brush, lesson_pill, radius,
                      ui::Fade(ready ? ui::kAccent : 0xFFFFFFFF, edge * a), 0.7f);
      const std::wstring label = !ready ? L"讲不了" : (busy ? L"讲解中" : L"讲讲它");
      const UINT32 ink = !ready ? ui::kTextDim : ((busy || hot) ? 0xFFFFFFFF : ui::kAccent);
      ui::TextCentered(target, label, fmt.meta, lesson_pill, fmt.brush, ui::Fade(ink, a));
    }
    target->PopAxisAlignedClip();
  }

  target->PopAxisAlignedClip();
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));

  // ---- 右侧栏 ----
  // 画在图区裁剪之外：栏本身是固定的，不跟着节点的缩放/平移走。
  DrawRail(target, model, fmt, width_dip, height_dip, a);

  // ---- 讲解卡 ----
  //
  // 讲的内容是宿主的模型现写的，长度不可控，所以它是"浮"在图区底部的一条，
  // 而不是长在节点里：关掉之后节点还是原来那个样子。
  if (LessonCardOn(model)) {
    const bool failed = model.lesson_phase == 3;
    const D2D1_RECT_F card = LessonCardRect(BodyWidth(model), height_dip);
    ui::FillRound(target, fmt.brush, card, 14.0f, ui::Fade(0xF21B1D24, a));
    ui::StrokeRound(target, fmt.brush, card, 14.0f, ui::Fade(failed ? 0xFFFF9F0A : ui::kAccent, 0.45f * a), 1.0f);

    const float inner_l = card.left + 16.0f;
    const float inner_r = card.right - 16.0f;
    fmt.brush->SetColor(ui::Color(ui::Fade(failed ? 0xFFFF9F0A : 0xFF30D158, a)));
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(inner_l + 4.0f, card.top + 16.0f), 3.5f, 3.5f), fmt.brush);
    ui::Text(target, ui::FitText(WidenNode(model.lesson_title), inner_r - inner_l - 110.0f, 10.5f), fmt.meta,
             D2D1::RectF(inner_l + 14.0f, card.top + 7.0f, inner_r - 90.0f, card.top + 25.0f), fmt.brush,
             ui::Fade(failed ? 0xFFFFB340 : ui::kAccent, a));
    ui::Text(target, L"Esc 关掉 · 点空白处也行", fmt.meta,
             D2D1::RectF(inner_r - 200.0f, card.top + 7.0f, inner_r, card.top + 25.0f), fmt.brush,
             ui::Fade(ui::kTextDim, a));
    ui::Text(target, ui::FitText(WidenNode(model.lesson_text), inner_r - inner_l, 13.0f), fmt.body,
             D2D1::RectF(inner_l, card.top + 27.0f, inner_r, card.top + 50.0f), fmt.brush, ui::Fade(ui::kText, a));
    ui::Text(target, ui::FitText(WidenNode(model.lesson_hint), inner_r - inner_l, 10.5f), fmt.meta,
             D2D1::RectF(inner_l, card.top + 52.0f, inner_r, card.top + 71.0f), fmt.brush,
             ui::Fade(ui::kTextDim, a));
  }

  // ---- 页脚 ----
  ui::StrokeRound(target, fmt.brush,
                  D2D1::RectF(kPad, height_dip - kFooter + 6.0f, width_dip - kPad, height_dip - kFooter + 6.5f),
                  0.0f, ui::Fade(0xFFFFFFFF, 0.06f * a), 0.5f);
  // 页脚一行话：平时教人怎么用（「讲讲它」必须在这里露一次脸，否则没人知道能点），
  // 卡开着时换成怎么关掉，忙的时候显示进度。
  std::wstring hint;
  UINT32 hint_ink = ui::kTextDim;
  if (model.busy) {
    hint = model.busy_text;
  } else if (!model.note.empty()) {
    // 刚点了一颗不能当当前任务的节点：这句是回话，比平时的说明亮一档。
    hint = model.note;
    hint_ink = ui::kText;
  } else if (LessonCardOn(model)) {
    // 卡片有两种来源（左键的本地详解 / 「讲讲它」的模型讲解），这行不偏向任何一边。
    hint = L"卡片开着 · Esc 关掉 · ↑↓ 换一颗 · 拖空白平移 · 滚轮缩放";
  } else if (model.lesson_phase == 1) {
    hint = L"正在讲…（一颗概念节点讲一段，讲完会浮在这下面）· Esc 关掉";
  } else if (anim.hover_row >= 0 && static_cast<size_t>(anim.hover_row) < model.rows.size()) {
    // 鼠标停在哪颗上，就把那颗能做的几件事直接说出来。
    // 悬停是唯一能让「右键 = 完成」「点 +N = 拆开」被发现的时机——图上没有右键菜单，
    // 光标在有子节点的那颗上也只变个手型，不说出来就没人试。
    const bool has_children = model.rows[static_cast<size_t>(anim.hover_row)].child_count > 0;
    if (model.read_only) {
      hint = L"右键这颗 = 标记完成 · 左键 = 看详解 · 双击 = 设为当前任务";
      if (has_children) hint += L" · 点 +N = 拆开/收起";
    } else {
      hint = has_children ? L"左键这颗 = 看详解 · 点 +N = 再拆一层" : L"左键这颗 = 看详解";
    }
    hint_ink = ui::kText;
  } else if (model.read_only) {
    hint = L"左键 = 看详解 · 右键 = 标记完成 · 双击 = 设为当前任务 · Enter 展开/收起 · Esc 关闭";
  } else {
    hint = L"左键 = 看详解 · Enter 或点 +N 再拆一层 · 点概念节点「讲讲它」听讲解 · Ctrl+Enter 就这样 · Esc 不要了";
  }
  ui::Text(target, hint, fmt.meta,
           D2D1::RectF(kPad, height_dip - kFooter + 14.0f, width_dip - kPad - kCommitW - 20.0f, height_dip - 8.0f),
           fmt.brush, ui::Fade(hint_ink, a));

  const D2D1_RECT_F commit = CommitRectAt(BodyWidth(model), height_dip);
  ui::FillRound(target, fmt.brush, commit, kCommitH * 0.5f,
                model.busy ? ui::Fade(0xFF3A3A40, a) : ui::Fade(ui::kAccent, a));
  ui::TextCentered(target, model.read_only ? L"知道了" : L"就这样", fmt.body, commit, fmt.brush,
                  ui::Fade(0xFFFFFFFF, a));

  // AI 会话在跑：整扇窗的边缘亮一圈蓝边，流光沿边跑。只做余光提示，不挡任何内容；
  // 放在最后画，边框压在标题栏与页脚之上，窗口一开一合时跟着 a 一起淡。
  if (model.ai_running) {
    ui::DrawAiFrame(target, fmt, width_dip, height_dip, kWindowRadius, anim.now, a);
  }

  target->SetTransform(D2D1::Matrix3x2F::Identity());
}
}  // namespace dshwork::review
