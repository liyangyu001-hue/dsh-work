// DSH Work · 原生小条（切片一）
//
// 这里只做一件事：把宿主的"当前节点 + 进度"画成屏幕顶部那条玻璃小条。
//
// 为什么是分层窗口（WS_EX_LAYERED + UpdateLayeredWindow）而不是普通窗口：
//   小条要有个真正圆润的边缘和柔和阴影。普通窗口的圆角在 Win11 上只能拿到
//   系统那个固定半径，而且边缘没有抗锯齿；分层窗口是逐像素 alpha，圆角、
//   发丝描边、阴影全由我们自己控制，这是"像 iOS 而不是像控件"的关键。
//
// 为什么不用 DWM 的毛玻璃：那套（SetWindowCompositionAttribute）是未公开接口，
//   行为随系统版本漂移；而小条的面积太小，模糊不模糊几乎看不出差别。
//   所以这里用"半透明深色 + 顶部发丝高光 + 双层阴影"的配方（见 DESIGN-SYSTEM.md）。
//
// 线程模型：一个轮询线程负责 HTTP，算好的模型用互斥量交接，再 PostMessage
// 通知 UI 线程重绘；UI 线程只碰 D2D/DWrite。

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <chrono>
#include <mutex>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "doc.h"
#include "host.h"
#include "panel.h"
#include "prompt.h"
#include "capture.h"
#include "json.h"
#include "review.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winhttp.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"DSHWorkNativeBar";
constexpr UINT kMsgModel = WM_APP + 1;
constexpr UINT kMsgPaint = WM_APP + 2;
constexpr UINT kMsgKey = WM_APP + 4;

// 几何（DIP，绘制时乘 dpi/96）
// 窗口比药丸宽得多，是为了让"左边已完成的、右边还没做的"能挂在两侧；
// 窗口本身是穿透的，多出来的透明区域不会挡住任何点击。
constexpr float kWindowWidth = 1040.0f;
constexpr float kWindowHeight = 96.0f;
constexpr float kPillHeight = 44.0f;
constexpr float kPaddingX = 18.0f;
constexpr float kDotRadius = 4.0f;
constexpr float kDotGap = 10.0f;
constexpr float kProgressGap = 12.0f;
constexpr float kSideChipWidth = 132.0f;  // 两侧那张半透明的卡片宽度
constexpr float kSideChipGap = 8.0f;
constexpr int kSideChipCount = 2;         // 每侧最多挂两张：再多就糊成一片
constexpr UINT kHoverTimerId = 0x77;
constexpr UINT kAnimTimerId = 0x78;
// Tab 是「长按手势」：按下先等一会儿，按住够久才召唤覆盖层，松手才落子。
// 用 GetAsyncKeyState 轮询来判断松手 —— RegisterHotKey 只给按下的通知，没有抬起。
constexpr UINT_PTR kTabWatchTimerId = 0x7a;
constexpr ULONGLONG kTabHoldMs = 220;
ULONGLONG g_tab_press = 0;      // 这次按下是什么时候（0 = 没在按）
bool g_tab_gesture = false;     // 这一轮是不是「按住 Tab」唤起的框选
bool g_tab_watch = false;       // 轮询定时器在不在跑
constexpr UINT kOpenTimerId = 0x79;
// 动效时长（毫秒）：改手感只改这里，别散落到各处
constexpr DWORD kSquashMs = 420;    // 药丸挤压回弹的整段时长
constexpr DWORD kOpenDelayMs = 150; // 弹起来之后再展开面板
// 面板展开/收起：从药丸的矩形长成整条，或缩回去。
// 参考 BrimDeck——表面先变形，内容随后淡入，而不是两个窗口硬切。
constexpr DWORD kPanelOpenMs = 300;
constexpr DWORD kPanelCloseMs = 220;
constexpr float kPanelTopDip = 10.0f;
constexpr DWORD kChipFadeMs = 140;   // 邻居卡收进/挂出的过渡

/**
 * 界面模型来自 doc.h：小条和面板看的是同一份结构。
 * 早先小条自己解析一遍 JSON，面板又要再解析一遍，两处口径迟早会漂；
 * 现在只解析一次，两个窗口各取所需。
 */
using BarModel = dshwork::WorkModel;

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
std::mutex g_model_mutex;
BarModel g_model;
std::atomic<bool> g_polling{true};

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_formatTitle = nullptr;
IDWriteTextFormat* g_formatMeta = nullptr;

float g_dpiScale = 1.0f;

// 绘制用资源随窗口尺寸变化，缓存起来免得每帧重建
ID2D1DCRenderTarget* g_target = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;

// ── 面板 ─────────────────────────────────────────────────
// 面板是第二个窗口，不是小条的放大版：小条只报"现在这一件事"，
// 面板承载全部任务与详情，两者共用同一份 WorkModel。
HWND g_panel = nullptr;
bool g_panel_open = false;
/** 面板是被鼠标悬停带出来的（而不是热键按出来的）——离开时要跟着收回去 */
bool g_panel_by_hover = false;
// 「完成」按钮的按压动画：按下后 260ms 内压下去再弹回，停表后归零。
constexpr UINT kBtnPulseTimerId = 0x7C;
ULONGLONG g_btn_pulse_at = 0;
bool g_btn_pulse_on = false;
/** 鼠标是否停在小条上。小条是穿透窗口收不到 WM_MOUSEMOVE，只能轮询光标。 */
bool g_bar_hovered = false;
/** 上一次绘制出来的可悬停范围（DIP，相对窗口左上角）。药丸宽度跟着文字变，只能画完才知道。 */
/** 上一次绘制出来的可悬停范围（DIP，相对窗口左上角）。药丸宽度跟着文字变，只能画完才知道。 */
float g_hit_left_dip = 0.0f;
float g_hit_right_dip = 0.0f;
float g_hit_top_dip = 0.0f;
float g_hit_bottom_dip = 0.0f;

/**
 * 动效状态。
 *
 * 按下/悬停那一下不是直接"变大小"，而是先被压扁、再弹回来、余震收敛——
 * 这就是动画十二原则里的挤压拉伸 + 跟随动作。全部用毫秒时间戳现算，
 * 没有补间表也没有依赖，120fps 都撑得住。
 */
bool g_squash_active = false;
ULONGLONG g_squash_start = 0;
float g_squash_scale_x = 1.0f;
float g_squash_scale_y = 1.0f;
float g_squash_min_y = 1.0f;  // 这一趟弹动的最小/最大缩放，收尾时写日志自检
float g_squash_max_y = 1.0f;

/**
 * 动效整体放慢倍数，只给调试和审查用（DSHWORK_SLOWMO=8 就是八倍慢放）。
 * 正常运行时恒为 1；所有时长都乘它，方便逐帧看清曲线。
 */
float g_motion_scale = 1.0f;

/** 最近一次框选的结果，成功后由 kMsgCaptureDone 读出来做提示。 */
std::wstring g_capture_path;

/* 解构链路的跨线程共享：消息号、结果、以及 UI 线程的入口声明。
   声明放这里是因为 WndProc 在上面，定义在下面。 */
constexpr UINT kMsgRecognize = WM_APP + 6;
std::mutex g_recognize_mutex;
std::wstring g_recognize_text;
std::wstring g_recognize_job;  // 作业号：审阅窗要靠它去拉草稿

// 审阅窗的全局与前置声明：WindowProc 在定义之前就要用到它们
constexpr UINT kMsgReview = WM_APP + 10;
constexpr UINT kMsgDocGraph = WM_APP + 11;  // 面板 → 主窗口：打开已落库文档的节点图
constexpr UINT kMsgOpenPanel = WM_APP + 12; // 宿主里的「打开面板」按钮 → 主窗口
constexpr int kReviewKeyBase = 0x90;  // 0 上 · 1 下 · 2 回车 · 3 Ctrl+回车 · 4 Esc

bool g_review_open = false;
HWND g_review = nullptr;
/** 唤出审阅窗之前谁在前台；关窗要还回去，别把人家的输入光标抢走。 */
HWND g_review_prev_focus = nullptr;
std::wstring g_review_job;
dshwork::review::Model g_review_model;
std::wstring g_review_note = L"正在再拆一层…";
bool g_review_busy = false;
std::set<std::string> g_review_expanded;  // 记住拆开过哪些，刷新后不塌回去
/** 刚点了「再拆一层」的那颗：拆完自动摊开，免得人再去按一次。 */
std::string g_review_pending_expand;
// 最近一次构建用的 JSON 根（草稿或文档）。本地展开/收起只要重建模型，
// 不该为此再跑一趟 HTTP —— 那是白白多一次往返。
dshwork::JValue g_review_root;

void ShowReview(const std::wstring& job);
void ShowReviewDoc();
void EnsureReviewWindow();
void ForceForeground(HWND hwnd);
bool g_review_read_only = false;
bool ReloadReviewDraft();
float EaseInOutCubic(float t);
// 审阅窗的动画状态：打开进度 + 选中块的当前位置（DIP，body 顶部起算）。
// 逐帧在这里推进，review.h 的 Draw 只负责按这两个值画。
float g_review_anim_open = 1.0f;
float g_review_hl_y = -1.0f;
ULONGLONG g_review_anim_at = 0;
constexpr UINT kReviewAnimTimerId = 0x7B;
// 拖动平移（中键）与「鼠标移出去就收窗」的状态。放在这里是因为
// ReviewProc 和 TickReviewAnim 都要用，而两者都在下面。
bool g_review_panning = false;
float g_review_pan_lx = 0.0f;
float g_review_pan_ly = 0.0f;
bool g_review_closing = false;
bool g_review_tracking_leave = false;
constexpr float kReviewCloseMs = 190.0f;
// 动画帧间隔：固定 60fps。跟屏幕刷新率绑定过一版，240Hz 下等于 4ms 一帧，
// 画不完就堆积，反而卡。动画本身是按时间算的，帧率低也不会变慢。
constexpr UINT kReviewFrameMs = 16;
// 悬停多久自动展开。太短会误触，太长又像没反应。
constexpr ULONGLONG kHoverExpandMs = 260;
int g_review_hover_row = -1;
// 双击判定：同一颗节点两次点击的间隔。
int g_review_last_click_row = -1;
ULONGLONG g_review_last_click_at = 0;
ULONGLONG g_review_hover_at = 0;

void RepaintReview();
void ReviewKey(int which);
void LayoutReview();
void ReviewExpandSelected();
// 定义在文件后半段（意图通道），审阅窗的双击要先用一小段。
void PostIntent(const std::string& type, const std::string& node_id);
void ReviewCollapseSelected();
void ReviewCommit();
void CloseReview();
// 定义在 ReviewProc 之后：「讲讲它」点下去要起后台线程，这里只先声明。
void StartLesson(const std::string& node_id);
// 后台拿回结果后由 kMsgReview 6 在 UI 线程贴到模型上（定义在 StartLesson 之后）。
void ApplyLessonOutcome();
// 「讲讲它」的结果：网络在后台线程跑，回来后 PostMessage(kMsgReview, 6) 交给 UI 线程。
// 和「再拆一层」同款——后台线程碰窗口是这个文件里踩过的坑。
std::mutex g_lesson_mutex;
struct LessonOutcome {
  std::string node_id;
  bool ok = false;        // 宿主给了 2xx
  int status = 0;         // HTTP 状态；0 = 连不上 / 超时
  std::string title;      // 卡片标题（宿主 view.title）
  std::string text;       // 正文
  std::string hint;       // 提示（怎么关掉 / 下一步）
  bool unsupported = false;  // 宿主没有这个接口
  std::string fallback;   // 没有 view 时的兜底原因（旧的宿主、或不是 JSON）
} g_lesson_outcome;
bool g_review_hover_chip = false;
bool g_recognize_ok = false;
void StartRecognize(const std::wstring& path);
void OfferHandoffToAi();

int g_capture_w = 0;
int g_capture_h = 0;

/**
 * 小条上的一次性提示（"已截取 800×600"这类）。
 *
 * 不做独立窗口：小条本来就常驻顶部，借它显示 2 秒最省事，
 * 也不会多一个窗口要管理位置和层级。
 */
std::wstring g_notice;
ULONGLONG g_notice_until = 0;
constexpr UINT kNoticeTimerId = 0x7A;

/** 重绘整条小条（分层窗口走 UpdateLayeredWindow，见 Repaint 定义）。 */
void Repaint();

void ShowNotice(const std::wstring& text, DWORD ms) {
  g_notice = text;
  g_notice_until = GetTickCount64() + ms;
  SetTimer(g_window, kNoticeTimerId, ms + 40, nullptr);
  Repaint();
}

/**
 * 邻居卡的显隐进度（0=全藏着，1=全挂出）。
 *
 * 为什么要它：悬停瞬间卡片如果"啪"地消失，药丸的挤压就被这个突变盖住了。
 * 让卡片在 140ms 里收进去，挤压叠在这段收拢之上，动作才连成一句。
 */
float EaseOutCubic(float t);  // 定义在下面，这里先用
float g_chip_fade = 1.0f;
float g_chip_fade_from = 1.0f;
float g_chip_fade_to = 1.0f;
ULONGLONG g_chip_fade_start = 0;
bool g_chip_fade_active = false;

void UpdateChipFade() {
  if (!g_chip_fade_active) return;
  const float t = static_cast<float>(GetTickCount64() - g_chip_fade_start) /
                  static_cast<float>(kChipFadeMs);
  if (t >= 1.0f) {
    g_chip_fade = g_chip_fade_to;
    g_chip_fade_active = false;
    return;
  }
  g_chip_fade = g_chip_fade_from + (g_chip_fade_to - g_chip_fade_from) * EaseOutCubic(t);
}

void StartChipFade(float target) {
  if (target == g_chip_fade_to && g_chip_fade_active) return;
  g_chip_fade_from = g_chip_fade;
  g_chip_fade_to = target;
  g_chip_fade_start = GetTickCount64();
  g_chip_fade_active = true;
}

/** 面板的一次变形：从 from 矩形长到 to 矩形，矩形本身在每一帧被插值。 */
struct PanelMorph {
  bool active = false;
  bool opening = false;
  ULONGLONG start = 0;
  RECT from = {};
  RECT to = {};
  float ms = static_cast<float>(kPanelOpenMs);
};
PanelMorph g_morph;
float g_panel_dip_w = 0.0f;   // 长条的逻辑宽度（随任务数变）
float g_panel_dip_h = dshwork::ui::kStripHeight;

void Repaint();
void ShowPanel();
void HidePanel(bool animated = true);
void UpdatePanelMorph();
float PanelContentAlpha();
int g_focus = -1;  // 面板里选中第几张卡；-1 表示跟随当前任务
dshwork::WorkModel g_panel_model;
IDWriteTextFormat* g_formatPanelTitle = nullptr;
IDWriteTextFormat* g_formatNode = nullptr;
IDWriteTextFormat* g_formatBody = nullptr;
IDWriteTextFormat* g_formatPanelMeta = nullptr;

constexpr wchar_t kPanelClass[] = L"DSHWorkNativePanel";
constexpr UINT kMsgTray = WM_APP + 3;
constexpr UINT kMsgPlanTextDone = WM_APP + 9;
constexpr int kPromptHotkeyId = 0x76;

/** 后台线程算出来的提示语，回到 UI 线程才拿去 ShowNotice。 */
std::wstring g_plan_note;
std::mutex g_plan_mutex;

void OpenPrompt();
void SubmitPrompt(const std::wstring& text);
void RefreshPanelModel();
void ShowPanel();
constexpr UINT kHotkeyId = 0x51;
constexpr UINT kTabHotkeyId = 0x70;   // Tab 常驻热键：开着 DSH Work 就一直监听
bool g_hover_tab_armed = false;
constexpr int kTrayIconId = 1;

// 这几个函数在窗口过程里就要用，定义在下面，先声明
void RepaintPanel();
int HitTestDot(int px, int py);  // 定义在后面，前面先用
void RefreshPanelModel();
void DispatchKey(int action);
void TogglePanel();
void UpdateHover();
void ShowTrayMenu();
void Repaint();
void StartRegionCapture();
void ArmHoverTab();
void DisarmHoverTab();

void LogToFile(const std::string& message) {
  wchar_t profile[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return;
  std::wstring dir = std::wstring(profile) + L"\\.dsh\\dsh-work";
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path = dir + L"\\native-shell.log";
  std::ofstream file(path, std::ios::app);
  if (!file) return;
  SYSTEMTIME now;
  GetLocalTime(&now);
  char stamp[32];
  snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
  file << "[" << stamp << "] " << message << "\n";
}

void PollLoop() {
  using namespace std::chrono_literals;
  std::string last_reason;
  // 宿主重启后 ui.seq 会归零，所以按时间戳判新旧；只认 15 秒内的指令，
  // 免得伴随层晚启动时把一个陈年请求又弹一遍。
  double last_ui_at = 0.0;
  while (g_polling.load(std::memory_order_relaxed)) {
    // 每次都重读 endpoint.json：宿主重启会换端口和 token，缓存下来就永远连不上
    dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    BarModel model;
    std::string ui_target;  // 非空 = 宿主按钮刚留下一条指令
    if (!endpoint.ok) {
      model.state = dshwork::WorkModel::State::Offline;
      model.note = endpoint.error;
      model.title = L"DSH 未连接";
    } else {
      std::string body;
      std::string reason;
      if (dshwork::HttpGetJson(endpoint, L"/dsh-work/api/state", body, reason)) {
        model = dshwork::ModelFromDoc(body);
        if (model.state != dshwork::WorkModel::State::Online) {
          model.state = dshwork::WorkModel::State::Offline;
          model.note = "状态文档解析失败";
          model.title = L"DSH 数据异常";
        } else {
          // 宿主按钮把「打开面板 / 节点图」指令挂在 state 上，这里取走。
          dshwork::JValue root;
          if (dshwork::ParseJson(body, root)) {
            const dshwork::JValue* ui = root.Get("ui");
            const dshwork::JValue* at = (ui != nullptr) ? ui->Get("at") : nullptr;
            const dshwork::JValue* target = (ui != nullptr) ? ui->Get("target") : nullptr;
            if (at != nullptr && target != nullptr && target->IsString()) {
              const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
              const double stamp = at->number;
              if (stamp > last_ui_at && static_cast<double>(now_ms) - stamp <= 15000.0) {
                if (target->text == "graph") {
                  ui_target = "graph";
                  last_ui_at = stamp;
                } else if (target->text == "panel") {
                  ui_target = "panel";
                  last_ui_at = stamp;
                }
              }
            }
          }
        }
        last_reason.clear();
      } else {
        model.state = dshwork::WorkModel::State::Offline;
        model.note = reason;
        model.title = L"DSH 未连接";
        if (reason != last_reason) {
          LogToFile("轮询失败：" + reason);
          last_reason = reason;
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(g_model_mutex);
      g_model = model;
    }
    if (g_window != nullptr) PostMessageW(g_window, kMsgModel, 0, 0);
    if (!ui_target.empty() && g_window != nullptr) {
      LogToFile(std::string("宿主按钮：打开") + (ui_target == "graph" ? "节点图" : "面板"));
      PostMessageW(g_window, ui_target == "graph" ? kMsgDocGraph : kMsgOpenPanel, 0, 0);
    }

    for (int i = 0; i < 10 && g_polling.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(100ms);
    }
  }
}

/** 缓出曲线：先快后慢，收尾不拖泥带水。t 已经是 0..1。 */
float EaseOutCubic(float t) {
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

/**
 * 审阅窗的一帧动画。
 *
 * 三件事同时推进：窗口淡入（200ms）、选中块滑到新位置（跟手，约 120ms 收敛）、
 * 新行的淡入由每行自己的 appear 时间决定（在 Draw 里算，不在这里）。
 * 全部收敛后停表，静止时不占 CPU。
 */
void TickReviewAnim() {
  if (g_review == nullptr) {
    KillTimer(g_window, kReviewAnimTimerId);
    return;
  }
  const ULONGLONG now = GetTickCount64();
  // 收窗也是一段动效：open 从 1 走到 0（同时整体往下沉、淡出），走完才真正 HideWindow。
  if (g_review_closing) {
    const float k = static_cast<float>(now - g_review_anim_at) / kReviewCloseMs;
    g_review_anim_open = k >= 1.0f ? 0.0f : (1.0f - (k <= 0.0f ? 0.0f : EaseInOutCubic(k)));
    RepaintReview();
    if (k >= 1.0f) {
      g_review_closing = false;
      g_review_anim_open = 1.0f;
      CloseReview();
    }
    return;
  }
  const float k = static_cast<float>(now - g_review_anim_at) / 200.0f;
  g_review_anim_open = k >= 1.0f ? 1.0f : (k <= 0.0f ? 0.0f : EaseOutCubic(k));
  RepaintReview();
  // 悬停自动展开：指针在某一颗上停够 kHoverExpandMs、它有子节点且还没展开，就展开。
  // 放在定时器里而不是鼠标消息里，是因为"停够久"需要一个时钟 —— 一次扫过整排
  // 不该连着开好几颗。
  if (!g_review_closing && !g_review_panning && g_review_hover_row >= 0 &&
      g_review_hover_row < static_cast<int>(g_review_model.rows.size())) {
    const dshwork::review::Row& hovered =
        g_review_model.rows[static_cast<size_t>(g_review_hover_row)];
    if (hovered.has_children && !hovered.expanded && now - g_review_hover_at >= kHoverExpandMs) {
      g_review_model.selected = g_review_hover_row;
      g_review_hover_row = -1;  // 先清掉：展开后行会被重建，索引立刻失效
      g_review_hover_at = now;
      ReviewExpandSelected();
      return;
    }
  }

  // 打开动画收尾后不立刻停表：连线上的流光还在走，只要窗口开着就按刷新率跑。
  // 这是唯一一个"长期运行"的动画，代价是几颗方框 + 几条折线，可接受；
  // 窗口一收（CloseReview）就把表停掉，静止时不留任何后台开销。
  if (g_review_anim_open >= 1.0f && now - g_review_anim_at > 320) {
    g_review_anim_open = 1.0f;
  }

  // 键盘：只"看"，不"抢"。
  //
  // 这个窗口是从一个 WS_EX_NOACTIVATE 的面板点开的，系统经常不肯把前台权交过来，
  // 于是 WM_KEYDOWN 根本到不了这里（踩过两次）。但 RegisterHotKey 会真的把键吃掉，
  // 用户在任何输入框打字都会被吞——也不行。
  // 折中：定时器里用 GetAsyncKeyState 观测按下沿，只处理我们自己关心的那五个键，
  // 其它键概不干预，用户该打什么打什么。
  struct Watch { UINT vk; int code; };
  static const Watch kWatched[] = {{VK_UP, 0}, {VK_DOWN, 1}, {VK_RETURN, 2}, {VK_BACK, 3}, {VK_ESCAPE, 4}};
  static bool was_down[5] = {};
  for (int i = 0; i < 5; ++i) {
    const bool down = (GetAsyncKeyState(static_cast<int>(kWatched[i].vk)) & 0x8000) != 0;
    if (down && !was_down[i]) {
      const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      // Ctrl+Enter = 就这样；单独 Enter = 展开/收起
      const int code = (kWatched[i].code == 2 && ctrl) ? 5 : kWatched[i].code;
      ReviewKey(code);
    }
    was_down[i] = down;
  }
}

/** 两头慢中间快：收起用它，像被吸回去，而不是"啪"地消失。 */
float EaseInOutCubic(float t) {
  if (t < 0.5f) return 4.0f * t * t * t;
  const float f = -2.0f * t + 2.0f;
  return 1.0f - f * f * f * 0.5f;
}

inline float LerpF(float a, float b, float t) { return a + (b - a) * t; }

inline int LerpI(int a, int b, float t) { return static_cast<int>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t); }

/**
 * 挤压回弹。
 *
 * 一条衰减振荡：先把药丸压扁（横向略微撑开，保持体积感），再弹过头一点，
 * 幅度按 e^(-4.5t) 收掉，约 420ms 静止。这是"预备 → 挤压 → 跟随动作"的
 * 最小实现，一条曲线同时给出速度和停止感。
 */
void UpdateSquash() {
  if (!g_squash_active) {
    g_squash_scale_x = 1.0f;
    g_squash_scale_y = 1.0f;
    return;
  }
  const float span = static_cast<float>(kSquashMs) * g_motion_scale;
  const float t = static_cast<float>(GetTickCount64() - g_squash_start) / span;
  if (t >= 1.0f) {
    // 收尾时把这一趟的极值记下来：没有过冲（<1 再 >1）就说明曲线写坏了
    LogToFile("挤压回弹 y∈[" + std::to_string(g_squash_min_y).substr(0, 6) + "," +
              std::to_string(g_squash_max_y).substr(0, 6) + "]");
    g_squash_active = false;
    g_squash_min_y = 1.0f;
    g_squash_max_y = 1.0f;
    g_squash_scale_x = 1.0f;
    g_squash_scale_y = 1.0f;
    return;
  }
  const float amplitude = 0.15f * std::exp(-4.5f * t);
  const float wave = std::sin(18.0f * t);
  g_squash_scale_y = 1.0f - amplitude * wave;
  g_squash_scale_x = 1.0f + amplitude * wave * 0.75f;  // 压扁时横向补偿
  if (g_squash_scale_y < g_squash_min_y) g_squash_min_y = g_squash_scale_y;
  if (g_squash_scale_y > g_squash_max_y) g_squash_max_y = g_squash_scale_y;
}

void StartSquash() {
  g_squash_active = true;
  g_squash_start = GetTickCount64();
}

D2D1_COLOR_F ColorOf(UINT32 argb) {  return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(argb & 0xFF) / 255.0f,
                      static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
}

void DrawPill(ID2D1RenderTarget* target, const BarModel& model) {
  const float scale = g_dpiScale;
  const float width = kWindowWidth * scale;
  const float height = kWindowHeight * scale;
  const float pill_h = kPillHeight * scale;
  const float pill_top = (height - pill_h) * 0.5f;

  // 先量文字，长度决定药丸多宽——小条要跟着内容走，不能写死
  const bool showing_notice = !g_notice.empty() && GetTickCount64() < g_notice_until;
  std::wstring title = showing_notice ? g_notice : (model.title.empty() ? L"正在连接 DSH…" : model.title);
  wchar_t progress[64] = {};
  if (model.total > 0) {
    swprintf_s(progress, L"%d/%d", model.done, model.total);
  } else {
    wcscpy_s(progress, L"—");
  }

  IDWriteTextLayout* title_layout = nullptr;
  const float max_title_width = 420.0f * scale;
  g_dwrite->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()), g_formatTitle,
                             max_title_width, pill_h, &title_layout);
  IDWriteTextLayout* progress_layout = nullptr;
  g_dwrite->CreateTextLayout(progress, static_cast<UINT32>(wcslen(progress)), g_formatMeta, 200.0f * scale,
                             pill_h, &progress_layout);

  DWRITE_TEXT_METRICS title_metrics = {};
  DWRITE_TEXT_METRICS progress_metrics = {};
  if (title_layout != nullptr) title_layout->GetMetrics(&title_metrics);
  if (progress_layout != nullptr) progress_layout->GetMetrics(&progress_metrics);

  const float text_w = (title_metrics.widthIncludingTrailingWhitespace > 0)
                           ? title_metrics.widthIncludingTrailingWhitespace
                           : 40.0f * scale;
  const float progress_w = progress_metrics.widthIncludingTrailingWhitespace;

  const float content_w = kPaddingX * 2 * scale + kDotRadius * 2 * scale + kDotGap * scale + text_w +
                          kProgressGap * scale + progress_w;
  const float pill_w = content_w;
  const float pill_left = (width - pill_w) * 0.5f;

  // 悬停命中区：药丸本身 + 两侧挂出来的卡片。画的时候顺手记下来，
  // 因为药丸宽度是量完文字才知道的，外面没法预先算。
  float hit_left = pill_left;
  float hit_right = pill_left + pill_w;

  // ── 两侧：一串半透明白点，代表排在前面/后面的事 ──
  // 不用卡片：卡片有边、有底、有文字，每多一张就多一种"细节要统一"的负担，
  // 而这里想说的只有"还有几件事、分别在前后"。一个点就够了。
  // 参照 BrimDeck 的收起态：一排圆环，不排文字。
  if (model.current >= 0) {
    const int count = static_cast<int>(model.items.size());
    const float dot_r = 4.5f * scale;
    const float dot_gap = 17.0f * scale;
    const float dot_cy = pill_top + pill_h * 0.5f;
    const float pad = 14.0f * scale;

    for (int side = -1; side <= 1; side += 2) {
      for (int step = 1; step <= kSideChipCount; ++step) {
        const int index = model.current + side * step;
        if (index < 0 || index >= count) break;
        const float cx = (side < 0)
                             ? pill_left - pad - dot_r - static_cast<float>(step - 1) * dot_gap
                             : pill_left + pill_w + pad + dot_r + static_cast<float>(step - 1) * dot_gap;
        const float alpha = (0.46f - 0.14f * static_cast<float>(step - 1)) * g_chip_fade;
        if (alpha <= 0.01f) continue;
        // 已完成的那侧再淡一点：它是"走过的路"，不是"待办"
        const dshwork::NodeView& node = model.items[static_cast<size_t>(index)];
        float a = alpha;
        if (node.status == "done" || node.status == "skipped") a *= 0.55f;
        g_brush->SetColor(D2D1::ColorF(1, 1, 1, a));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_cy), dot_r, dot_r), g_brush);
        if (cx - dot_r < hit_left) hit_left = cx - dot_r;
        if (cx + dot_r > hit_right) hit_right = cx + dot_r;
      }
    }
  }
  // 阴影：三层同心圆角矩形叠出柔和感（没有 GPU 模糊时的老办法，够用且零依赖）
  const float radius = pill_h * 0.5f;

  // 挤压回弹：围绕药丸中心做各向异性缩放。只影响观感，不影响命中区。
  UpdateSquash();
  const D2D1_POINT_2F squash_center = D2D1::Point2F(pill_left + pill_w * 0.5f, pill_top + pill_h * 0.5f);
  target->SetTransform(D2D1::Matrix3x2F::Scale(g_squash_scale_x, g_squash_scale_y, squash_center));
  for (int i = 6; i >= 1; --i) {
    const float spread = static_cast<float>(i) * 1.15f * scale;
    g_brush->SetColor(D2D1::ColorF(0, 0, 0, 0.016f));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(pill_left - spread, pill_top - spread + 2.0f * scale,
                                      pill_left + pill_w + spread, pill_top + pill_h + spread + 2.0f * scale),
                          radius + spread, radius + spread),
        g_brush);
  }

  // 玻璃主体：半透明深色
  g_brush->SetColor(D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.78f));
  target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pill_left, pill_top, pill_left + pill_w, pill_top + pill_h), radius, radius),
                               g_brush);

  // 发丝描边：0.5 DIP
  g_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.14f));
  target->DrawRoundedRectangle(
      D2D1::RoundedRect(D2D1::RectF(pill_left, pill_top, pill_left + pill_w, pill_top + pill_h), radius, radius), g_brush,
      0.5f * scale);

  // 状态点
  UINT32 dot_color = 0xFF8E8E93;
  if (model.state == BarModel::State::Online) dot_color = 0xFF0A84FF;
  if (model.state == dshwork::WorkModel::State::Offline) dot_color = 0xFFFF453A;
  g_brush->SetColor(ColorOf(dot_color));
  const float dot_cx = pill_left + kPaddingX * scale + kDotRadius * scale;
  const float dot_cy = pill_top + pill_h * 0.5f;
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_cx, dot_cy), kDotRadius * scale, kDotRadius * scale),
                      g_brush);

  // 标题（提示态用强调色，让人一眼看出这不是任务名）
  const float text_x = dot_cx + kDotRadius * scale + kDotGap * scale;
  if (showing_notice) {
    g_brush->SetColor(ColorOf(0xFF0A84FF));
  } else {
    g_brush->SetColor(D2D1::ColorF(0.96f, 0.96f, 0.97f, 1.0f));
  }
  if (title_layout != nullptr) {
    target->DrawTextLayout(D2D1::Point2F(text_x, pill_top), title_layout,
                           g_brush);
  }

  // 进度
  g_brush->SetColor(D2D1::ColorF(0.60f, 0.60f, 0.64f, 1.0f));
  if (progress_layout != nullptr) {
    const float progress_x = text_x + text_w + kProgressGap * scale;
    target->DrawTextLayout(D2D1::Point2F(progress_x, pill_top),
                           progress_layout, g_brush);
  }

  if (title_layout != nullptr) title_layout->Release();
  if (progress_layout != nullptr) progress_layout->Release();

  target->SetTransform(D2D1::Matrix3x2F::Identity());

  g_hit_left_dip = hit_left / scale;
  g_hit_right_dip = hit_right / scale;
  g_hit_top_dip = pill_top / scale;
  g_hit_bottom_dip = (pill_top + pill_h) / scale;
}

void Repaint() {
  if (g_target == nullptr) return;

  const int px_w = static_cast<int>(kWindowWidth * g_dpiScale);
  const int px_h = static_cast<int>(kWindowHeight * g_dpiScale);

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = px_w;
  info.bmiHeader.biHeight = -px_h;  // 自上而下，和 D2D 的坐标一致
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);

  BarModel model;
  {
    std::lock_guard<std::mutex> lock(g_model_mutex);
    model = g_model;
  }

  RECT bounds{0, 0, px_w, px_h};
  g_target->BindDC(memory, &bounds);
  g_target->BeginDraw();
  g_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));  // 全透明
  DrawPill(g_target, model);
  g_target->EndDraw();

  POINT source = {0, 0};
  SIZE size = {px_w, px_h};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(g_window, nullptr, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA);

  SelectObject(memory, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

/**
 * 贴在工作区顶部正中。
 * monitor 传 nullptr 就留在原处；传了就把小条搬到那块屏（双屏时按热键唤起到光标所在的屏）。
 * 必须先重算 DPI：两块屏缩放不同（150% + 100%）是常态，用旧值搬过去会错位。
 */
void UpdateDpi();  // 定义在下面，这里先声明

void PositionWindow(HMONITOR monitor = nullptr) {
  if (monitor == nullptr) monitor = MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);

  // 先把窗口挪进目标显示器的范围，GetDpiForWindow 才会报那块屏的 DPI
  const int probe_w = 8;
  const int probe_x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - probe_w) / 2;
  const int probe_y = info.rcWork.top + 4;
  SetWindowPos(g_window, HWND_TOPMOST, probe_x, probe_y, probe_w, probe_w, SWP_NOACTIVATE);
  UpdateDpi();

  const int px_w = static_cast<int>(kWindowWidth * g_dpiScale);
  const int px_h = static_cast<int>(kWindowHeight * g_dpiScale);
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - px_w) / 2;
  const int y = info.rcWork.top + static_cast<int>(4 * g_dpiScale);
  SetWindowPos(g_window, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Windows 会悄悄把置顶窗口降级（lolPing issue #70），每次显示都要重申
  SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/**
 * 当前显示器刷新率。用 EnumDisplaySettings 而不是 DWM：前者只要 user32，
 * 后者要额外链 dwmapi.lib，而我们只需要一个量级正确的数字。
 * 拿不到按 60 算；夹在 [60, 240]，免得某些虚拟显示器报 1 或 360。
 */
UINT RefreshHzOf(HMONITOR monitor) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) return 60;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 60;
  UINT hz = mode.dmDisplayFrequency;
  if (hz == 0 || hz == 1) return 60;  // 1 表示「硬件默认」，不可信
  if (hz < 60) hz = 60;
  if (hz > 240) hz = 240;
  return hz;
}

/** 动画定时器一跳多少毫秒：按当前屏刷新率算。 */
UINT FrameIntervalMs() {
  const UINT hz = RefreshHzOf(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY));
  const UINT ms = 1000u / (hz == 0 ? 60u : hz);
  return ms < 4u ? 4u : ms;
}

/** 光标所在的显示器。热键唤起时把小条搬过去——双屏用户不该被钉在第一块屏上。 */
HMONITOR MonitorUnderCursor() {
  POINT point = {};
  if (!GetCursorPos(&point)) return MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY);
  return MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
}
// 动画帧间隔：跟着屏幕的高刷能力走，但不让主线程被它拖垮。
// 120Hz 以上给 8ms（约 120fps），普通屏 16ms；动画一律按真实时间算，
// 所以两档跑出来是同一个时长，只是细腻程度不同。
UINT kReviewFrameMsForScreen() {
  const UINT hz = RefreshHzOf(MonitorUnderCursor());
  return hz >= 120 ? 8 : 16;
}



void UpdateDpi();  // 前向声明：PositionWindow 里要先按目标屏重算 DPI

void UpdateDpi() {
  const UINT dpi = GetDpiForWindow(g_window);
  g_dpiScale = (dpi == 0 ? 96 : static_cast<float>(dpi)) / 96.0f;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      return 0;
    case kMsgModel:
      Repaint();
      if (g_panel_open) {
        RefreshPanelModel();
        RepaintPanel();
      }
      return 0;
    case WM_LBUTTONDBLCLK:
      // 双击药丸 = 把眼下这件标记完成（单击是开面板，见 WM_LBUTTONDOWN）
      {
        std::thread([]() {
          const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
          std::string body, reason;
          if (!dshwork::HttpPostJson(endpoint, L"/dsh-work/api/intent",
                                     "{\"intent\":{\"type\":\"node.advance\"},\"by\":\"human\"}", body,
                                     reason)) {
            LogToFile("双击小条失败：" + reason);
          }
        }).detach();
        ShowNotice(L"已标记完成", 1600);
      }
      return 0;
    case WM_LBUTTONDOWN:
      // 单击药丸 = 展开/收起面板（悬停已经能开，这一下是给"想用鼠标点"的人）
      if (g_panel_open) HidePanel(); else ShowPanel();
      return 0;
    case dshwork::capture::kMsgCaptureDone:
      ArmHoverTab();  // 覆盖层收摊了，Tab 还给主窗口
      ShowWindow(g_window, SW_SHOWNOACTIVATE);
      if (wparam != 0) {
        wchar_t text[96];
        const int action = static_cast<int>(lparam);
        if (action == 7) {
          // 双击 Tab：不截图，只把眼下这件标记完成
          std::thread([]() {
            const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
            std::string body, reason;
            if (!dshwork::HttpPostJson(endpoint, L"/dsh-work/api/intent",
                                       "{\"intent\":{\"type\":\"node.advance\"},\"by\":\"human\"}", body,
                                       reason)) {
              LogToFile("双击 Tab 失败：" + reason);
            }
          }).detach();
          ShowNotice(L"已标记完成", 1600);
          return 0;
        }        if (action == 2) {
          // 解构：截图已经落盘，这里把它交给宿主模型去拆任务树
          StartRecognize(g_capture_path);
          return 0;
        } else if (action == 4) {
          // 交给 AI：不拆解，只把当前节点标成「已交给 AI」等它接。
          // 真正的执行在宿主那一侧（会话/模型），这里只把意愿落到状态机。
          OfferHandoffToAi();
          swprintf_s(text, L"已交给 AI · 等它接手");
        } else if (action == 6) {
          swprintf_s(text, L"已截取 %d × %d · 自动后续待接", g_capture_w, g_capture_h);
        } else if (action == 3) {
          swprintf_s(text, L"已收下 %d × %d", g_capture_w, g_capture_h);
        } else {
          swprintf_s(text, L"已截取 %d × %d", g_capture_w, g_capture_h);
        }
        ShowNotice(text, 2400);
        LogToFile(std::string("框选完成 action=") + std::to_string(static_cast<int>(lparam)));
      } else {
        ShowNotice(L"已取消", 1600);
        LogToFile("框选取消");
      }
      return 0;
    case kMsgRecognize: {
      std::wstring text;
      {
        std::lock_guard<std::mutex> lock(g_recognize_mutex);
        text = g_recognize_text;
      }
      ShowNotice(text, 3600);

      // 拆完了先开审阅窗：让人看过、再拆过，才落库。
      std::wstring job;
      {
        std::lock_guard<std::mutex> lock(g_recognize_mutex);
        job = g_recognize_job;
      }
      if (wparam != 0 && !job.empty()) {
        ShowReview(job);
        return 0;
      }
      if (wparam != 0 && !g_panel_open) ShowPanel();
      return 0;
    }
    case kMsgOpenPanel:
      // 宿主里的「打开面板」：只负责摊开，不跟药丸单击一样来回折叠。
      if (!g_panel_open) ShowPanel();
      return 0;
    case kMsgDocGraph:
      // 面板里点了当前任务那段文字：把人送进整张地图。
      ShowReviewDoc();
      return 0;
    case kMsgReview: {
      switch (wparam) {
        case 1: {  // 再拆一层回来了：重拉草稿；刚拆的那颗由 pending 负责摊开
          g_review_busy = false;
          if (ReloadReviewDraft()) {
            // 摊开新子树会把图撑宽：跟本地展开一样重新自适应，别让新节点落到窗户外。
            dshwork::review::FitView(g_review_model);
            dshwork::review::ClampPan(g_review_model);
            LayoutReview();
            RepaintReview();
            ShowNotice(L"又拆细了一层", 1600);
          } else {
            RepaintReview();
          }
          return 0;
        }
        case 2:
          ReviewExpandSelected();
          return 0;
        case 3:
          ReviewCommit();
          return 0;
        case 4:
          g_review_busy = false;
          ShowNotice(L"这一步没成，看看日志", 2400);
          RepaintReview();
          return 0;
        case 5: {  // 提交成功：关窗、把面板推出来看新进度
          CloseReview();
          ShowNotice(L"收下了，开工", 2000);
          if (!g_panel_open) ShowPanel();
          return 0;
        }
        case 6:  // 「讲讲它」回来了
          ApplyLessonOutcome();
          return 0;
        default:
          return 0;
      }
    }
    case kMsgKey:
      DispatchKey(static_cast<int>(wparam));
      return 0;
    case WM_HOTKEY:
      // 审阅窗自己的一组键，先认掉，别落到下面的全局分支里
      if (wparam >= static_cast<WPARAM>(kReviewKeyBase) && wparam < static_cast<WPARAM>(kReviewKeyBase + 8)) {
        ReviewKey(static_cast<int>(wparam) - kReviewKeyBase);
        return 0;
      }
      if (wparam == kPromptHotkeyId) {
      OpenPrompt();
      return 0;
    }
    if (wparam == kHotkeyId) {
        // 双屏：唤起到光标所在的那块屏，而不是永远粘在主屏
        PositionWindow(MonitorUnderCursor());
        TogglePanel();
      } else if (wparam == kTabHotkeyId) {
        // Tab 是「按一下进入框选、然后停在那儿」的开关，不是按住手势：
        // 之前把「松手」当成手势结束，结果手指一抬覆盖层就被关掉了（看起来就是闪一下）。
        // 覆盖层开着的时候 Tab 归它自己管（它重新注册了这个键），这里直接让过。
        if (dshwork::capture::IsActive()) return 0;
        // 同一波自动重复：MOD_NOREPEAT 通常够了，但换键盘/远程桌面时偶有漏网，再兜一层
        const ULONGLONG now = GetTickCount64();
        if (now - g_tab_press < 400) return 0;
        g_tab_press = now;
        StartRegionCapture();
      }
      return 0;
    case WM_TIMER:
      if (wparam == kReviewAnimTimerId) {
        TickReviewAnim();
        return 0;
      }
      if (wparam == kBtnPulseTimerId) {
        const float kk = static_cast<float>(GetTickCount64() - g_btn_pulse_at) / 260.0f;
        // 0→1 压下去，1→2 弹回来；用一条简谐曲线，末了就停在常态
        dshwork::g_btn_pulse = kk >= 1.0f ? 0.0f : (1.0f - std::cos(kk * 3.14159265f));
        RepaintPanel();
        if (kk >= 1.0f) {
          g_btn_pulse_on = false;
          KillTimer(g_window, kBtnPulseTimerId);
        }
        return 0;
      }
      if (wparam == kHoverTimerId) {
        UpdateHover();
        return 0;
      }
      if (wparam == kAnimTimerId) {
        UpdateChipFade();
        Repaint();
        if (g_morph.active) UpdatePanelMorph();
        if (!g_squash_active && !g_chip_fade_active && !g_morph.active) {
          KillTimer(g_window, kAnimTimerId);
        }
        return 0;
      }
      if (wparam == kNoticeTimerId) {
        KillTimer(g_window, kNoticeTimerId);
        g_notice.clear();
        Repaint();
        return 0;
      }
      if (wparam == kOpenTimerId) {
        KillTimer(g_window, kOpenTimerId);
        if (g_bar_hovered && !g_panel_open) {
          ShowPanel();
          g_panel_by_hover = true;
        }
        return 0;
      }
      break;
    case dshwork::ui::prompt::kMsgPromptSubmit: {
      const std::wstring typed = dshwork::ui::prompt::Text();
      dshwork::ui::prompt::Close();
      SubmitPrompt(typed);
      return 0;
    }
    case dshwork::ui::prompt::kMsgPromptCancel:
      dshwork::ui::prompt::Close();
      ShowWindow(g_window, SW_SHOWNOACTIVATE);
      return 0;
    case kMsgPlanTextDone: {
      std::wstring note;
      {
        std::lock_guard<std::mutex> guard(g_plan_mutex);
        note.swap(g_plan_note);
      }
      if (!note.empty()) ShowNotice(note, wparam == 1 ? 3600 : 4000);
      if (wparam == 1) {
        RefreshPanelModel();
        ShowPanel();
      }
      return 0;
    }
    case kMsgTray:
      if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
        ShowTrayMenu();
      } else if (lparam == WM_LBUTTONUP) {
        TogglePanel();
      }
      return 0;
    case WM_DPICHANGED: {
      UpdateDpi();
      PositionWindow();
      Repaint();
      return 0;
    }
    case WM_DISPLAYCHANGE:
      PositionWindow();
      Repaint();
      return 0;
    case WM_DESTROY: {
      NOTIFYICONDATAW data = {};
      data.cbSize = sizeof(data);
      data.hWnd = g_window;
      data.uID = kTrayIconId;
      Shell_NotifyIconW(NIM_DELETE, &data);
      UnregisterHotKey(g_window, kHotkeyId);
      PostQuitMessage(0);
      return 0;
    }
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

bool CreateGraphics() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&g_dwrite))))
    return false;

  dshwork::g_ui_dwrite = g_dwrite;
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN",
                                        &g_formatTitle)))
    return false;
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN",
                                        &g_formatMeta)))
    return false;

  dshwork::g_ui_dwrite = g_dwrite;
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.5f, L"zh-CN",
                                        &g_formatPanelTitle)))
    return false;
  // 节点图标题：16 DIP。和面板标题视觉等大，缩放到 100% 也清楚。
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-CN",
                                      &g_formatNode)))
    return false;
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN",
                                        &g_formatBody)))
    return false;
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"zh-CN",
                                        &g_formatPanelMeta)))
    return false;
  g_formatBody->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  g_formatPanelTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  g_formatPanelMeta->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  g_formatPanelTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  g_formatPanelMeta->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  g_formatBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  g_formatTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  g_formatMeta->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  g_formatTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  g_formatMeta->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(g_d2d->CreateDCRenderTarget(&properties, &g_target))) return false;
  if (FAILED(g_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_brush))) return false;
  return true;
}

// ── 面板：位置 / 绘制 / 生命周期 ─────────────────────────

/** 药丸当前占的矩形（物理像素）：展开动画的起点，也是收起的终点。 */
RECT PillRectPx() {
  RECT window_rect = {};
  GetWindowRect(g_window, &window_rect);
  RECT r = {};
  r.left = window_rect.left + static_cast<LONG>(g_hit_left_dip * g_dpiScale);
  r.top = window_rect.top + static_cast<LONG>(g_hit_top_dip * g_dpiScale);
  r.right = window_rect.left + static_cast<LONG>(g_hit_right_dip * g_dpiScale);
  r.bottom = window_rect.top + static_cast<LONG>(g_hit_bottom_dip * g_dpiScale);
  return r;
}

/** 长条落位：横向居中，贴在屏幕上沿往下一点。 */
RECT PanelTargetRect() {
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &info);
  const int px_w = static_cast<int>(g_panel_dip_w * g_dpiScale);
  const int px_h = static_cast<int>(g_panel_dip_h * g_dpiScale);
  RECT r = {};
  r.left = info.rcWork.left + (info.rcWork.right - info.rcWork.left - px_w) / 2;
  r.top = info.rcWork.top + static_cast<int>(kPanelTopDip * g_dpiScale);
  r.right = r.left + px_w;
  r.bottom = r.top + px_h;
  return r;
}

/**
 * 动画时间轴：一段动画里每个元素只跑自己的那一段。
 *
 * 这是从 BrimDeck 学来的一手（`Track(Start, End)`，按总时长的比例分段）：
 * 所有东西一起动会糊成一团，"表面先走、内容后跟"才是活的。
 * 展开：表面 0→0.8，内容 0.25→0.9，药丸残影 0→0.15，阴影 0→0.3。
 * 收起反过来：内容先走（0→0.3），表面后收（0.12→1）。
 */
struct TimeTrack {
  float start;
  float end;
};

TimeTrack TrackOf(bool expanding, int which) {
  switch (which) {
    case 0:  // surface
      return expanding ? TimeTrack{0.0f, 0.80f} : TimeTrack{0.12f, 1.0f};
    case 1:  // content
      return expanding ? TimeTrack{0.25f, 0.90f} : TimeTrack{0.0f, 0.30f};
    case 2:  // compact（药丸内容的残影）
      return expanding ? TimeTrack{0.0f, 0.15f} : TimeTrack{0.62f, 1.0f};
    default:  // shadow
      return expanding ? TimeTrack{0.0f, 0.30f} : TimeTrack{0.50f, 1.0f};
  }
}

/** 当前进度落在某段轨道上的 0..1。 */
float TrackProgress(bool expanding, int which) {
  if (!g_morph.active) return 1.0f;
  const float t = static_cast<float>(GetTickCount64() - g_morph.start) / (g_morph.ms * g_motion_scale);
  const float p = t >= 1.0f ? 1.0f : t;
  const TimeTrack track = TrackOf(expanding, which);
  const float span = track.end - track.start;
  if (span <= 0.0001f) return p >= track.start ? 1.0f : 0.0f;
  const float v = (p - track.start) / span;
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/** 内容的不透明度：表面先长出来，字后跟上来（跟随动作，别一起糊上去）。 */
float PanelContentAlpha() {
  if (!g_morph.active) return 1.0f;
  return TrackProgress(g_morph.opening, 1);
}

/** 内容从 0.96 长到 1（BrimDeck 的 ContentScale）。 */
float PanelZoom() {
  if (!g_morph.active) return 1.0f;
  return 0.96f + 0.04f * TrackProgress(g_morph.opening, 1);
}

/** 推进变形：每一帧把窗口矩形朝目标插值，并重画（尺寸变了，布局得跟着重排）。 */
void UpdatePanelMorph() {
  if (!g_morph.active || g_panel == nullptr) return;
  const float t = static_cast<float>(GetTickCount64() - g_morph.start) /
                  (g_morph.ms * g_motion_scale);
  const float p = t >= 1.0f ? 1.0f : t;
  const float e = g_morph.opening ? EaseOutCubic(p) : EaseInOutCubic(p);

  const int left = LerpI(g_morph.from.left, g_morph.to.left, e);
  const int top = LerpI(g_morph.from.top, g_morph.to.top, e);
  const int right = LerpI(g_morph.from.right, g_morph.to.right, e);
  const int bottom = LerpI(g_morph.from.bottom, g_morph.to.bottom, e);
  SetWindowPos(g_panel, HWND_TOPMOST, left, top, right - left, bottom - top, SWP_NOACTIVATE);
  RepaintPanel();

  if (p < 1.0f) return;
  g_morph.active = false;
  if (!g_morph.opening) {
    // 缩回药丸大小 → 面板让位，小条重新出现
    ShowWindow(g_panel, SW_HIDE);
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    g_panel_open = false;
    LogToFile("面板收起");
  } else {
    LogToFile("面板展开完成");
  }
}

void RepaintPanel() {
  if (g_panel == nullptr) return;
  RECT panel_rect = {};
  GetWindowRect(g_panel, &panel_rect);
  const int px_w = panel_rect.right - panel_rect.left;
  const int px_h = panel_rect.bottom - panel_rect.top;
  if (px_w <= 0 || px_h <= 0) return;

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);

  // 必须用 DIB 段：CreateCompatibleBitmap 没有 alpha 通道，
  // D2D 在上面画会直接返回错误，UpdateLayeredWindow 也拿不到透明度。
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = px_w;
  info.bmiHeader.biHeight = -px_h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);

  RECT bounds = {0, 0, px_w, px_h};
  g_target->BindDC(memory, &bounds);
  g_target->BeginDraw();
  g_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

  dshwork::WorkModel model;
  {
    std::lock_guard<std::mutex> lock(g_model_mutex);
    model = g_panel_model;
  }
  dshwork::PanelFormats formats{g_formatPanelTitle, g_formatBody, g_formatPanelMeta, g_formatNode, g_brush};
  dshwork::ui::DrawPanel(g_target, model, g_focus, g_dpiScale, formats,
                         static_cast<float>(px_w) / g_dpiScale, static_cast<float>(px_h) / g_dpiScale,
                         PanelContentAlpha(), PanelZoom());
  const HRESULT draw_result = g_target->EndDraw();
  if (FAILED(draw_result)) LogToFile("面板绘制失败 hr=" + std::to_string(static_cast<long>(draw_result)));

  POINT source = {0, 0};
  SIZE size = {px_w, px_h};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  if (!UpdateLayeredWindow(g_panel, nullptr, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA)) {
    LogToFile("面板 UpdateLayeredWindow 失败 err=" + std::to_string(GetLastError()));
  }

  SelectObject(memory, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

/* ─────────────── 解构审阅窗：先看，再决定要不要 ─────────────── */

constexpr wchar_t kReviewClass[] = L"DSHWorkNativeReview";

// 审阅窗的兜底宽度：工作区够宽就用 kWindowW（1600），不够就退到「工作区宽 − 两侧留白」。
// 640 是再窄就没法读的底线（内容还能横向拖）。这里全部手写比较：windows.h 的
// std::min / std::max 宏会把它们吃掉。
constexpr float kReviewMargin = 16.0f;
constexpr float kReviewMinWidth = 640.0f;

float ReviewAvailWidthDip() {
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &info);
  const float scale = g_dpiScale > 0.1f ? g_dpiScale : 1.0f;
  float avail = static_cast<float>(info.rcWork.right - info.rcWork.left) / scale - kReviewMargin * 2.0f;
  if (avail < kReviewMinWidth) avail = kReviewMinWidth;
  return avail;
}

void RepaintReview();
void TickReviewAnim();

/** 把草稿 JSON 摊平成行：结构折进 depth，绘制与命中只认行。 */
void BuildReviewModel(const dshwork::JValue& root) {
  using dshwork::JValue;
  g_review_root = root;
  dshwork::review::Model model;
  model.title = dshwork::Widen(root.GetString("title"));
  // 宽度先定下来：Layout/FitView 都要按它能给多少宽来排版，等开窗时再改就晚了。
  model.avail_w = ReviewAvailWidthDip();
  // 注意：这里不要清 g_review_expanded —— 它是「哪几颗摊开着」的跨次记忆，
  // 每次重建都清掉的话，子行刚放出来就会在下一帧塌回去。
  model.selected = g_review_model.selected;

  const JValue* nodes = root.Get("nodes");
  const JValue* roots = root.Get("roots");
  if (nodes == nullptr || roots == nullptr || !roots->IsArray()) {
    g_review_model = model;
    return;
  }

  // 两种形态都要认：
  //   · 草稿（/api/plan/<job>）的 nodes 是数组，每项自带 id
  //   · 落库文档（/api/state 的 doc）的 nodes 是「id → 节点」的对象
  // 只认数组的话，从面板进文档地图会看到一张空图（已踩过一次）。
  std::map<std::string, const JValue*> by_id;
  if (nodes->IsArray()) {
    for (const JValue& node : nodes->array) by_id[node.GetString("id")] = &node;
  } else if (nodes->IsObject()) {
    for (const auto& kv : nodes->object) by_id[kv.first] = &kv.second;
  } else {
    g_review_model = model;
    return;
  }
  model.total = static_cast<int>(roots->array.size());

  // 递归展开：只把「已经点开过」的那一层放出来，其余收着。
  std::function<void(const std::string&, int, int)> walk = [&](const std::string& id, int depth, int parent) {
    const auto it = by_id.find(id);
    if (it == by_id.end()) return;
    const JValue& node = *it->second;
    dshwork::review::Row row;
    row.id = id;
    row.depth = depth;
    row.parent = parent;
    row.title = node.GetString("title");
    row.detail = node.GetString("detail");
    row.acceptance = node.GetString("acceptance");
    row.status = node.GetString("status", "todo");
    // 概念节点靠它区分：读不到就当普通任务，绝不凭空标成「要学」。
    row.kind = node.GetString("kind", "task");
    if (const JValue* children = node.Get("children"); children != nullptr && children->IsArray()) {
      row.child_count = static_cast<int>(children->array.size());
      row.has_children = row.child_count > 0;
    }
    row.expanded = row.has_children && g_review_expanded.count(id) > 0;
    model.rows.push_back(row);
    const int my_index = static_cast<int>(model.rows.size()) - 1;
    if (!row.expanded) return;
    const JValue* children = node.Get("children");
    for (const JValue& child : children->array) walk(child.text, depth + 1, my_index);
  };

  for (const JValue& id : roots->array) walk(id.text, 0, -1);
  // 选中项必须落在行内。刚打开时 selected 是 −1（没有行被选过），这时按 Enter 会
  // 静默什么都不做，底栏却写着「Enter 展开/收起」——真机上就是"按了没反应"（踩过）。
  if (model.rows.empty()) {
    model.selected = -1;
  } else if (model.selected < 0 || model.selected >= static_cast<int>(model.rows.size())) {
    model.selected = 0;
  }

  // 记住每一行「第一次被看到」的时间：老行沿用之前的时间戳（不重播动画），
  // 新行（比如刚展开出来的子节点）从此刻开始淡入，并错开一点形成瀑布感。
  const ULONGLONG now = GetTickCount64();
  ULONGLONG stagger = 0;
  for (const dshwork::review::Row& row : model.rows) {
    const auto old = g_review_model.appear.find(row.id);
    if (old != g_review_model.appear.end()) {
      model.appear[row.id] = old->second;
    } else {
      model.appear[row.id] = now + stagger;
      stagger += 26;  // 每条错开 26ms
    }
  }

  g_review_model = model;
  dshwork::review::Layout(g_review_model);
  if (!g_review_model.fitted) dshwork::review::FitView(g_review_model);
}

void LayoutReview() {
  if (g_review == nullptr) return;
  // 每开一次都按当前屏幕重算：换了显示器 / 改了缩放也能落回可用范围。
  g_review_model.avail_w = ReviewAvailWidthDip();
  const float width_dip = dshwork::review::WindowWidth(g_review_model);
  const float height_dip = dshwork::review::WindowHeight(g_review_model);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &info);
  const int px_w = static_cast<int>(width_dip * g_dpiScale);
  const int px_h = static_cast<int>(height_dip * g_dpiScale);
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - px_w) / 2;
  const int y = info.rcWork.top + static_cast<int>(96 * g_dpiScale);
  SetWindowPos(g_review, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE);
}

void RepaintReview() {
  if (g_review == nullptr) return;
  // 图模式没有滚动位置：把平移量夹住，别让整张图被拖出视野。
  dshwork::review::ClampPan(g_review_model);

  RECT rect = {};
  GetWindowRect(g_review, &rect);
  const int px_w = rect.right - rect.left;
  const int px_h = rect.bottom - rect.top;
  if (px_w <= 0 || px_h <= 0) return;

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = px_w;
  info.bmiHeader.biHeight = -px_h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);

  RECT bounds = {0, 0, px_w, px_h};
  g_target->BindDC(memory, &bounds);
  g_target->BeginDraw();
  g_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

  dshwork::PanelFormats formats{g_formatPanelTitle, g_formatBody, g_formatPanelMeta, g_formatNode, g_brush};
  dshwork::review::Model model = g_review_model;
  model.read_only = g_review_read_only;
  if (g_review_busy) {
    model.busy = true;
    model.busy_text = g_review_note;
  }
  dshwork::review::Anim anim;
  anim.open = g_review_anim_open;
  anim.highlight_y = g_review_hl_y;
  anim.now = GetTickCount64();
  dshwork::review::Draw(g_target, model, formats, g_dpiScale, static_cast<float>(px_w) / g_dpiScale,
                        static_cast<float>(px_h) / g_dpiScale, 1.0f, anim);
  if (FAILED(g_target->EndDraw())) LogToFile("审阅窗绘制失败");

  POINT source = {0, 0};
  SIZE size = {px_w, px_h};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(g_review, nullptr, nullptr, &size, memory, &source, 0, &blend, ULW_ALPHA);

  SelectObject(memory, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

/** 拉一次草稿并重画。失败只写日志 + 一句提示，不关窗。 */
bool ReloadReviewDraft() {
  const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
  if (!endpoint.ok) {
    g_review_note = L"没连上宿主";
    return false;
  }
  std::string body;
  std::string reason;
  if (!dshwork::HttpGetJson(endpoint, L"/dsh-work/api/plan/" + g_review_job, body, reason)) {
    g_review_note = L"读不到草稿：" + dshwork::Widen(reason);
    LogToFile("读草稿失败：" + reason);
    return false;
  }
  dshwork::JValue root;
  if (!dshwork::ParseJson(body, root)) {
    g_review_note = L"草稿解析失败";
    return false;
  }
  BuildReviewModel(root);
  // 刚拆开的那颗：草稿里已经有子节点了，就顺手摊开，省掉一次多余按键。
  if (!g_review_pending_expand.empty()) {
    for (const dshwork::review::Row& row : g_review_model.rows) {
      if (row.id == g_review_pending_expand && row.has_children) {
        g_review_expanded.insert(row.id);
        break;
      }
    }
    g_review_pending_expand.clear();
    BuildReviewModel(root);  // 摊开后行数变了，重建一次
    LayoutReview();
  }
  g_review_note.clear();
  return true;
}


LRESULT CALLBACK ReviewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_KEYDOWN) {
    // 键盘走这个窗口自己的消息，不注册任何全局热键：
    // 全局热键会在用户打字时把键吞掉（Tab 是唯一例外，那是我们自己唤出的手势键）。
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (wparam) {
      case VK_UP:
        ReviewKey(0);
        return 0;
      case VK_DOWN:
        ReviewKey(1);
        return 0;
      case VK_RETURN:
        ReviewKey(ctrl ? 3 : 2);
        return 0;
      case VK_BACK:
        ReviewCollapseSelected();
        return 0;
      case VK_ESCAPE:
        ReviewKey(4);
        return 0;
      default:
        break;
    }
    return 0;
  }
  if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && g_review_open) {
    // 指针落在能点的地方才变手型：「讲讲它」和右下角那颗「知道了」。
    // 这张图整体是只读的，别的区域不该给出"能点"的错觉。
    POINT pt = {};
    GetCursorPos(&pt);
    ScreenToClient(window, &pt);
    const float dpi = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    const float k = dpi > 0.1f ? dpi : 1.0f;
    const float x = static_cast<float>(pt.x) / k;
    const float y = static_cast<float>(pt.y) / k;
    const bool hand =
        dshwork::review::LessonChipAt(g_review_model, x, y) >= 0 || dshwork::review::CommitHit(g_review_model, x, y);
    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
    return TRUE;
  }
  if (message == WM_MOUSEWHEEL) {
    // 滚轮缩放：以面板中心为锚，一次 12%，夹在 0.45~1.6 之间。
    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    const float step = delta > 0 ? 1.12f : (1.0f / 1.12f);
    float next = g_review_model.zoom * step;
    if (next > dshwork::review::kMaxZoom) next = dshwork::review::kMaxZoom;
    if (next < dshwork::review::kMinZoom) next = dshwork::review::kMinZoom;
    if (next != g_review_model.zoom) {
      // 保持中心那个点不动：先算出中心对应的世界坐标，缩放后再把它放回中心。
      const float cx = (dshwork::review::BodyWidth(g_review_model) * 0.5f - g_review_model.pan_x) / g_review_model.zoom;
      const float cy = (dshwork::review::BodyHeight(g_review_model) * 0.5f - g_review_model.pan_y) / g_review_model.zoom;
      g_review_model.zoom = next;
      g_review_model.pan_x = dshwork::review::BodyWidth(g_review_model) * 0.5f - cx * next;
      g_review_model.pan_y = dshwork::review::BodyHeight(g_review_model) * 0.5f - cy * next;
      dshwork::review::ClampPan(g_review_model);
      RepaintReview();
    }
    return 0;
  }
  if (message == WM_MBUTTONDOWN) {
    // 中键拖动平移。坐标同样要先换算成 DIP（理由见 WM_LBUTTONDOWN 那段）。
    const float dpi = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    const float k = dpi > 0.1f ? dpi : 1.0f;
    g_review_panning = true;
    g_review_pan_lx = static_cast<float>(GET_X_LPARAM(lparam)) / k;
    g_review_pan_ly = static_cast<float>(GET_Y_LPARAM(lparam)) / k;
    SetCapture(window);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return 0;
  }
  if (message == WM_MBUTTONUP) {
    if (g_review_panning) {
      g_review_panning = false;
      ReleaseCapture();
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    return 0;
  }
  if (message == WM_MOUSEMOVE) {
    if (!g_review_tracking_leave) {
      // 只登记一次；之后指针离开窗口时系统会发 WM_MOUSELEAVE。
      TRACKMOUSEEVENT tme = {};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = window;
      TrackMouseEvent(&tme);
      g_review_tracking_leave = true;
    }
    const float dpi = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    const float k = dpi > 0.1f ? dpi : 1.0f;
    const float mx = static_cast<float>(GET_X_LPARAM(lparam)) / k;
    const float my = static_cast<float>(GET_Y_LPARAM(lparam)) / k;
    if (g_review_panning) {
      g_review_model.pan_x += mx - g_review_pan_lx;
      g_review_model.pan_y += my - g_review_pan_ly;
      g_review_pan_lx = mx;
      g_review_pan_ly = my;
      dshwork::review::ClampPan(g_review_model);
      RepaintReview();
      return 0;
    }
    // 记住指针停在哪一颗上；"停够久了没有"由动画定时器判断，
    // 而不是在这里直接展开 —— 一次扫过整排会连开好几颗。
    const int hover = dshwork::review::NodeAt(g_review_model, mx, my);
    if (hover != g_review_hover_row) {
      g_review_hover_row = hover;
      g_review_hover_at = GetTickCount64();
    }
    // 手型的判定单独记一份：WM_SETCURSOR 会频繁进来，这里只是缓存。
    g_review_hover_chip = dshwork::review::LessonChipAt(g_review_model, mx, my) >= 0;
    return 0;
  }
  if (message == WM_MOUSELEAVE) {
    g_review_tracking_leave = false;
    // 指针离开这张图 = 人已经不看它了，收起来；但必须是淡出，不能啪一下消失。
    // 中键拖拽时偶尔会甩出边界，那不算"离开"。
    if (!g_review_panning && g_review_open && !g_review_closing) {
      g_review_closing = true;
      g_review_anim_at = GetTickCount64();
      SetTimer(g_window, kReviewAnimTimerId, kReviewFrameMsForScreen(), nullptr);
    }
    return 0;
  }
  if (message == WM_LBUTTONDOWN) {
    // 鼠标坐标是物理像素，而模型与布局全是 DIP。不换算就永远命中不到任何一颗
    // （实测 click y=488 实际落在 DIP 325，差 1.5 倍，被算到节点下方去了）。
    const float dpi_scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
    const float scale_ok = dpi_scale > 0.1f ? dpi_scale : 1.0f;
    const float x = static_cast<float>(GET_X_LPARAM(lparam)) / scale_ok;
    const float y = static_cast<float>(GET_Y_LPARAM(lparam)) / scale_ok;
    if (dshwork::review::CommitHit(g_review_model, x, y)) {
      if (g_window != nullptr) PostMessageW(g_window, kMsgReview, 3, 0);
      return 0;
    }
    // 讲解卡开着时，点卡片以外的地方 = 收起它。卡片本身只有字，没有可点的东西，
    // 所以"点外面收掉"是唯一的手势，和挂在上面的那颗按钮对称。
    if (dshwork::review::LessonCardOn(g_review_model) && !dshwork::review::LessonCardHit(g_review_model, x, y)) {
      g_review_model.lesson_phase = 0;
      LayoutReview();
      RepaintReview();
    }
    // 「讲讲它」在节点矩形的右下角，和节点重叠，必须先判它：
    // 否则永远命中到下面那颗节点，按钮等于画的。
    const int chip = dshwork::review::LessonChipAt(g_review_model, x, y);
    if (chip >= 0) {
      StartLesson(g_review_model.rows[static_cast<size_t>(chip)].id);
      return 0;
    }
    const int index = dshwork::review::NodeAt(g_review_model, x, y);
    if (index >= 0) {
      // 双击（同一颗 400ms 内点两次）= 把这一颗设成「当前任务」。
      // 判定要放在改选中项之前：第一次点只是选中（again=false），
      // 若只在 again 分支里记时间，双击就得等到第三次点击才成立。
      const ULONGLONG now_ms = GetTickCount64();
      const bool is_double = g_review_last_click_row == index && now_ms - g_review_last_click_at < 400;
      g_review_last_click_row = index;
      g_review_last_click_at = now_ms;
      const bool again = index == g_review_model.selected;
      g_review_model.selected = index;
      if (is_double) {
        const std::string id = g_review_model.rows[static_cast<size_t>(index)].id;
        std::thread([id]() { PostIntent("node.focus", id); }).detach();
        LogToFile("审阅：设为当前任务 " + id);
        g_review_last_click_row = -1;
        return 0;
      }
      SetTimer(g_window, kReviewAnimTimerId, kReviewFrameMsForScreen(), nullptr);
      // 再点一次同一颗 = 拆开 / 收起：不额外教用户记快捷键
      if (again) {
        const auto& row = g_review_model.rows[static_cast<size_t>(index)];
        if (row.has_children && g_review_expanded.count(row.id) > 0) {
          g_review_expanded.erase(row.id);
        } else if (g_window != nullptr) {
          PostMessageW(g_window, kMsgReview, 2, 0);
        }
      }
      RepaintReview();
      RepaintReview();
    }
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/* ───────────────────── 「讲讲它」：让宿主的模型当场讲一段 ───────────────────── */

/**
 * 点下「讲讲它」之后发生的事。
 *
 * 只做三件：先把卡片切成"正在讲"、起后台线程 POST、回来后由 UI 线程收尾。
 * 模型写一段要二三十秒，这段等待必须让界面先动起来——所以这里是先画再发，
 * 而不是等 HTTP 回来才刷新（那样看起来就是"点了没反应"，这个项目已经踩过）。
 */
void StartLesson(const std::string& node_id) {
  if (node_id.empty()) return;
  if (g_review_model.lesson_unsupported) {
    // 已经知道这个宿主没有讲解接口：别再烧一次请求，直接把原因摆出来。
    g_review_model.lesson_node = node_id;
    g_review_model.lesson_phase = 3;
    g_review_model.lesson_title = "讲讲它";
    g_review_model.lesson_text = "这个宿主还不认识讲解接口。";
    g_review_model.lesson_hint = "拉最新代码重启宿主；按钮变回蓝色再点。";
    LayoutReview();
    RepaintReview();
    return;
  }
  // 同一颗正在讲就不要再发一遍：模型调用贵，而且两张结果会打架。
  if (g_review_model.lesson_phase == 1 && g_review_model.lesson_node == node_id) return;
  g_review_model.lesson_node = node_id;
  g_review_model.lesson_phase = 1;
  g_review_model.lesson_title = "讲讲它";
  g_review_model.lesson_text.clear();
  g_review_model.lesson_hint.clear();
  RepaintReview();
  LogToFile("讲解：请求 " + node_id);

  std::thread([node_id]() {
    LessonOutcome outcome;
    outcome.node_id = node_id;
    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    if (!endpoint.ok) {
      outcome.title = "没连上宿主";
      outcome.text = "宿主没在跑，或者刚刚重启过。";
      outcome.hint = "把 dsh 重新拉起（--profile dshwork-dev），再点一次";
      outcome.fallback = endpoint.error;
    } else {
      std::string body;
      std::string reason;
      const std::string payload = "{\"nodeId\":\"" + node_id + "\"}";
      // 接收超时放到 60s：宿主自己的模型上限是 45s，30s 会正好卡在最后几秒。
      const bool ok = dshwork::HttpPostJson(endpoint, L"/dsh-work/api/learn", payload, body, reason, 60000,
                                           &outcome.status);
      outcome.ok = ok;
      outcome.fallback = reason;
      dshwork::JValue root;
      const dshwork::JValue* view = nullptr;
      if (dshwork::ParseJson(body, root)) view = root.Get("view");
      if (view != nullptr && view->IsObject()) {
        outcome.title = view->GetString("title");
        outcome.text = view->GetString("text");
        outcome.hint = view->GetString("hint");
        const dshwork::JValue* unsupported = view->Get("unsupported");
        outcome.unsupported = unsupported != nullptr && unsupported->type == dshwork::JValue::Type::Bool &&
                              unsupported->boolean;
      } else {
        // 没有 view：要么跑着的宿主比节点图旧（旧路由回的是 {error:'not found'}），
        // 要么回来的根本不是 DSH Work。两种情况都别硬猜，明说接口不认识。
        outcome.unsupported = true;
        outcome.title = "这个宿主还不认识讲解";
        outcome.text = "跑着的宿主比节点图旧，接口还没长出来。";
        outcome.hint = "拉最新代码重启宿主；按钮恢复前别重复点。";
      }
    }
    {
      std::lock_guard<std::mutex> lock(g_lesson_mutex);
      g_lesson_outcome = outcome;
    }
    LogToFile(std::string("讲解：") + (outcome.ok ? "成功 " : "失败 ") + outcome.fallback);
    if (g_window != nullptr) PostMessageW(g_window, kMsgReview, 6, 0);
  }).detach();
}

/** 收起讲解卡（也就取消了正在讲的那次）：只动界面上的这张卡。 */
void DismissLesson() {
  if (g_review_model.lesson_phase == 0) return;
  g_review_model.lesson_phase = 0;
  g_review_model.lesson_node.clear();
  LayoutReview();
  RepaintReview();
}

/** UI 线程收尾：把后台拿回来的那张卡贴到模型上，再按新高度重排。 */
void ApplyLessonOutcome() {
  LessonOutcome outcome;
  {
    std::lock_guard<std::mutex> lock(g_lesson_mutex);
    outcome = g_lesson_outcome;
  }
  // 用户已经 Esc 掉了、或者又点了别的节点：这张迟到的结果就不弹了。
  if (g_review_model.lesson_phase != 1 || g_review_model.lesson_node != outcome.node_id) return;
  g_review_model.lesson_phase = outcome.ok ? 2 : 3;
  g_review_model.lesson_title = outcome.title;
  g_review_model.lesson_text = outcome.text;
  g_review_model.lesson_hint = outcome.hint;
  if (outcome.unsupported) g_review_model.lesson_unsupported = true;
  LayoutReview();
  RepaintReview();
}

void CloseReview() {
  g_review_open = false;
  g_review_busy = false;
  // 窗口收掉，卡片跟着消失；下次打开从干净的模型开始（不然会看到上一轮的讲解）。
  g_review_model.lesson_phase = 0;
  g_review_model.lesson_node.clear();
  g_review_hover_chip = false;
  if (g_review != nullptr) ShowWindow(g_review, SW_HIDE);
  g_review_job.clear();
  // 关窗之后把前台还给唤出之前那个窗口：抢一次焦点可以，赖着不走不行。
  if (g_review_prev_focus != nullptr && IsWindow(g_review_prev_focus)) {
    SetForegroundWindow(g_review_prev_focus);
  }
  g_review_prev_focus = nullptr;
  if (g_window != nullptr) ArmHoverTab();
  if (!g_panel_open) ShowWindow(g_window, SW_SHOWNOACTIVATE);
}

/* 动作都在后台线程做，回来后用消息让 UI 线程收尾。 */
void ReviewExpandSelected() {
  const int index = g_review_model.selected;
  if (index < 0 || index >= static_cast<int>(g_review_model.rows.size())) {
    LogToFile("审阅：没有选中的行，忽略展开");
    return;
  }
  const dshwork::review::Row row = g_review_model.rows[static_cast<size_t>(index)];

  // 已经拆过的：本地展开/收起就够了，不必再花一次模型调用。
  if (row.has_children) {
    if (g_review_expanded.count(row.id) > 0) {
      g_review_expanded.erase(row.id);
    } else {
      g_review_expanded.insert(row.id);
    }
    // 光改这个集合界面是不会动的：行是 BuildReviewModel 摊平出来的，
    // 必须拿同一份 JSON 重建一次模型，再重新布局。（这一步漏过，表现就是点了没反应。）
    BuildReviewModel(g_review_root);
    // 展开会把内容撑宽，不重新自适应的话新节点会跑到窗口外（踩过）。
    dshwork::review::FitView(g_review_model);
    dshwork::review::ClampPan(g_review_model);
    LayoutReview();
    RepaintReview();
    LogToFile(std::string("审阅：本地") + (g_review_expanded.count(row.id) > 0 ? "展开 " : "收起 ") + row.id);
    return;
  }

  const std::string node_id = row.id;
  const std::wstring job = g_review_job;
  g_review_busy = true;
  g_review_note = L"正在再拆一层…";
  g_review_pending_expand = node_id;  // 拆完自动摊开这颗，不用再按一次
  RepaintReview();
  std::thread([job, node_id]() {
    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    if (!endpoint.ok) {
      if (g_window != nullptr) PostMessageW(g_window, kMsgReview, 4, 0);
      return;
    }
    std::string body;
    std::string reason;
    const std::string payload = "{\"jobId\":\"" + dshwork::Narrow(job) + "\",\"nodeId\":\"" + node_id + "\"}";
    const bool ok = dshwork::HttpPostJson(endpoint, L"/dsh-work/api/plan/expand", payload, body, reason);
    LogToFile(std::string("审阅再拆 ") + (ok ? "成功：" : "失败：") + body);
    if (g_window != nullptr) PostMessageW(g_window, kMsgReview, ok ? 1 : 4, 0);
  }).detach();
}

/**
 * 撤销这一层：把这颗节点刚拆出来的子树整棵删掉，退回没拆过的样子。
 *
 * 只对"拆过"的行有效。撤销是就地完成的（宿主那边同步删），不需要作业轮询；
 * 撤销后同一次会话里可以再拆一次，结果不会越滚越脏。
 */
void ReviewCollapseSelected() {
  const int index = g_review_model.selected;
  if (index < 0 || index >= static_cast<int>(g_review_model.rows.size())) return;
  const dshwork::review::Row row = g_review_model.rows[static_cast<size_t>(index)];
  if (!row.has_children) {
    g_review_note = L"这颗还没拆过";
    RepaintReview();
    return;
  }

  const std::wstring job = g_review_job;
  const std::string node_id = row.id;
  g_review_busy = true;
  g_review_note = L"正在撤掉这一层…";
  RepaintReview();
  std::thread([job, node_id]() {
    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    if (!endpoint.ok) {
      if (g_window != nullptr) PostMessageW(g_window, kMsgReview, 4, 0);
      return;
    }
    std::string body;
    std::string reason;
    const std::string payload = "{\"jobId\":\"" + dshwork::Narrow(job) + "\",\"nodeId\":\"" + node_id + "\"}";
    const bool ok = dshwork::HttpPostJson(endpoint, L"/dsh-work/api/plan/collapse", payload, body, reason);
    LogToFile(std::string("审阅撤销 ") + (ok ? "成功：" : "失败：") + body);
    if (g_window != nullptr) PostMessageW(g_window, kMsgReview, ok ? 1 : 4, 0);
  }).detach();
}

void ReviewCommit() {
  if (g_review_read_only) {
    CloseReview();
    return;
  }
  if (g_review_busy) return;  const std::wstring job = g_review_job;
  g_review_busy = true;
  g_review_note = L"正在收下…";
  RepaintReview();
  std::thread([job]() {
    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    if (!endpoint.ok) {
      if (g_window != nullptr) PostMessageW(g_window, kMsgReview, 4, 0);
      return;
    }
    std::string body;
    std::string reason;
    const std::string payload = "{\"jobId\":\"" + dshwork::Narrow(job) + "\"}";
    const bool ok = dshwork::HttpPostJson(endpoint, L"/dsh-work/api/plan/commit", payload, body, reason);
    LogToFile(std::string("审阅提交 ") + (ok ? "成功：" : "失败：") + body);
    if (g_window != nullptr) PostMessageW(g_window, kMsgReview, ok ? 5 : 4, 0);
  }).detach();
}

void ShowReview(const std::wstring& job) {
  g_review_job = job;
  g_review_read_only = false;
  g_review_busy = false;
  g_review_note = L"正在再拆一层…";
  g_review_expanded.clear();
  if (!ReloadReviewDraft()) LogToFile("审阅窗：草稿拉不下来");

  EnsureReviewWindow();
  RepaintReview();
  LogToFile("审阅窗打开 job=" + dshwork::Narrow(job) + " 行数=" + std::to_string(g_review_model.rows.size()));
}

/**
 * 打开"已经落库的那份文档"的节点图。
 *
 * 和审阅窗共用同一个窗口和同一套排版，区别只有两点：数据来自 /api/state 的 doc，
 * 以及它是只读的（没有"就这样"，不能拆）。入口是面板右侧那段当前任务的描述——
 * 人在"我做到哪了"和"整张地图长什么样"之间来回切，这是唯一自然的通道。
 */
void ShowReviewDoc() {
  const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
  if (!endpoint.ok) {
    ShowNotice(L"没连上宿主", 2000);
    return;
  }
  std::string body;
  std::string reason;
  if (!dshwork::HttpGetJson(endpoint, L"/dsh-work/api/state", body, reason)) {
    ShowNotice(L"读不到任务：" + dshwork::Widen(reason), 2600);
    return;
  }
  dshwork::JValue root;
  if (!dshwork::ParseJson(body, root)) {
    ShowNotice(L"任务解析失败", 2400);
    return;
  }
  const dshwork::JValue* doc = root.Get("doc");
  if (doc == nullptr) return;

  g_review_read_only = true;
  g_review_job.clear();
  g_review_busy = false;
  g_review_note.clear();
  g_review_expanded.clear();
  BuildReviewModel(*doc);
  // 必须做一次自适应：NodeAt 以 model.fitted 为前置条件，
  // 没 fit 过的模型点哪都返回 -1 —— 表现就是「图能看，点不动」。
  dshwork::review::FitView(g_review_model);
  dshwork::review::ClampPan(g_review_model);

  // 面板是"点了一下就进地图"的门，进门之后它该退场：
  // 两个窗口叠着会互相挡，而且面板不抢焦点，留着它也接不到键。
  HidePanel(false);
  EnsureReviewWindow();
  // 刚点完我们自己的面板，前台权还在本进程手上，这里可以直接把焦点交给地图窗口；
  // 少了这一步，地图显示出来了但 Enter/↑↓ 全被系统发给别的窗口（踩过）。
  ForceForeground(g_review);
  RepaintReview();
  LogToFile("节点图打开（文档） 行数=" + std::to_string(g_review_model.rows.size()));
}

/**
 * 把键盘焦点硬塞给我们的窗口。
 *
 * 为什么不只是一句 SetForegroundWindow：面板是 WS_EX_NOACTIVATE 的，
 * 点它不会把前台权交给本进程，于是紧接着的 SetForegroundWindow 会被系统
 * 静默拒绝——窗口显示出来了，按键却全发给别的窗口（踩过）。
 * 标准解法是把输入队列临时挂到当前前台线程上，做完再摘开。
 */
void ForceForeground(HWND hwnd) {
  if (hwnd == nullptr) return;
  const DWORD target = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  const DWORD self = GetCurrentThreadId();
  const bool attached = (target != 0 && target != self && AttachThreadInput(self, target, TRUE));
  SetForegroundWindow(hwnd);
  SetActiveWindow(hwnd);
  SetFocus(hwnd);
  if (attached) AttachThreadInput(self, target, FALSE);
}

/** 两个入口共用的开窗动作：建窗口、定位、抢焦点、放动画。 */
void EnsureReviewWindow() {
  if (g_review == nullptr) {
    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = ReviewProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kReviewClass;
    RegisterClassExW(&cls);
    // 审阅窗是个要动手做决定的窗口，可以抢焦点、直接从 WM_KEYDOWN 收键。
    // 不去注册全局热键：那会把用户正在打的字一起吞掉（这是踩过的坑）。
    g_review = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kReviewClass, L"DSH Work 审阅",
                               WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  LayoutReview();
  // 记住唤出之前谁在前台，关窗时还回去。
  g_review_prev_focus = GetForegroundWindow();
  ShowWindow(g_review, SW_SHOW);
  SetForegroundWindow(g_review);
  SetFocus(g_review);
  g_review_open = true;
  // 从头放一遍淡入。
  g_review_anim_open = 0.0f;
  g_review_anim_at = GetTickCount64();
  g_review_hl_y = -1.0f;
  SetTimer(g_window, kReviewAnimTimerId, kReviewFrameMsForScreen(), nullptr);
}

/** 审阅窗的按键：0 上 · 1 下 · 2 回车 · 3 Ctrl+回车 · 4 Esc */
void ReviewKey(int which) {
  if (!g_review_open) return;
  // 一次按下可能被投递两遍：窗口有焦点时 WM_KEYDOWN 来一次，动画定时器里的
  // GetAsyncKeyState 又观测到同一个按下沿再来一次。截图验证时一次 Esc 就把
  // 整张图关了——第一遍收讲解卡，第二遍看到卡已收起就收窗口。
  // 同一个键 220ms 内只认一次；人手不会以 4.5Hz 连按同一个键。
  static int last_code = -1;
  static ULONGLONG last_at = 0;
  const ULONGLONG now_ms = GetTickCount64();
  if (which == last_code && now_ms - last_at < 220) return;
  last_code = which;
  last_at = now_ms;
  const int count = static_cast<int>(g_review_model.rows.size());
  switch (which) {
    case 0:
      if (count > 0) g_review_model.selected = (g_review_model.selected + count - 1) % count;
      RepaintReview();
      return;
    case 1:
      if (count > 0) g_review_model.selected = (g_review_model.selected + 1) % count;
      RepaintReview();
      return;
    case 2:
      ReviewExpandSelected();
      return;
    case 3:
      ReviewCommit();
      return;
    default:
      // Esc（4）先收讲解卡，卡关着才收窗口；Ctrl+Enter（5）也落在这里 ——
      // 旧键位映射本来就把 5 归到 Esc 一档（见 NEXT.md「已知的坑」），本轮不顺手改。
      if (g_review_model.lesson_phase != 0) {
        DismissLesson();
        return;
      }
      CloseReview();
      return;
  }
}

/** 意图一律交给宿主判定：本地不改状态，下一次轮询自然会把新状态带回来。 */
void PostIntent(const std::string& type, const std::string& node_id = std::string()) {
  dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
  if (!endpoint.ok) {
    LogToFile("意图 " + type + " 未提交：" + endpoint.error);
    return;
  }
  std::string intent = "{\"type\":\"" + type + "\"";
  if (!node_id.empty()) intent += ",\"nodeId\":\"" + node_id + "\"";
  intent += "}";
  const std::string payload = "{\"intent\":" + intent + ",\"by\":\"human\"}";
  std::string body;
  std::string reason;
  if (dshwork::HttpPostJson(endpoint, L"/dsh-work/api/intent", payload, body, reason)) {
    LogToFile("意图已提交：" + type);
  } else {
    LogToFile("意图失败：" + type + " " + reason);
  }
}

/* ───────────────────── 交给 AI：只落意愿，不冒充执行 ───────────────────── */

/**
 * 把「当前节点交给 AI」记进状态机。
 *
 * 这里刻意什么都不启动：真正的执行属于宿主那一侧（会话、模型、工具），
 * 原生层只负责把「人已经把这块交出去了」这件事写进文档，
 * 面板据此显示「已交给 AI · 等它接手」，AI 接手后状态会变 running。
 */
void OfferHandoffToAi() {
  std::lock_guard<std::mutex> lock(g_model_mutex);
  if (g_model.current < 0 || static_cast<size_t>(g_model.current) >= g_model.items.size()) {
    LogToFile("交接未提交：没有当前节点");
    return;
  }
  const std::string node_id = g_model.items[static_cast<size_t>(g_model.current)].id;
  std::thread([node_id]() { PostIntent("handoff.offer", node_id); }).detach();
}

/* ───────────────────── 解构：把截图交给宿主的模型 ───────────────────── */

/**
 * 识别是异步的：宿主受理后回 202 + jobId，真正的结果要再去查。
 *
 * 所以这条链路全在后台线程里跑，结果只通过 PostMessage 回 UI 线程，
 * 网络等待期间小条照样能响应悬停和热键。
 */


/** 后台线程写、UI 线程读，只在这两处加锁。 */
/** 把结果放到全局，再敲一下 UI 线程的门——不要在后台线程里碰窗口。 */
void FinishRecognize(bool ok, const std::wstring& text) {
  {
    std::lock_guard<std::mutex> lock(g_recognize_mutex);
    g_recognize_ok = ok;
    g_recognize_text = text;
  }
  LogToFile(std::string("识别结束 ") + (ok ? "成功：" : "失败：") + dshwork::Narrow(text));
  if (g_window != nullptr) PostMessageW(g_window, kMsgRecognize, ok ? 1 : 0, 0);
}

/** 整份读进内存。截图只有几百 KB，流式读没有意义。 */
bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out, std::string& reason) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    reason = "打不开截图文件";
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (out.empty()) {
    reason = "截图是空的";
    return false;
  }
  return true;
}

/**
 * 提交 → 轮询 → 回报。
 *
 * 超时给到 150 秒：模型拆一棵树通常十几秒，卡住时我们宁可明确报“超时”，
 * 也不要让小条一直停在“识别中”让人猜。
 */
void StartRecognize(const std::wstring& path) {
  ShowNotice(L"正在识别这块…", 150000);
  std::thread([path]() {
    std::vector<unsigned char> png;
    std::string reason;
    if (!ReadWholeFile(path, png, reason)) {
      FinishRecognize(false, L"读不到截图：" + dshwork::Widen(reason));
      return;
    }

    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    if (!endpoint.ok) {
      FinishRecognize(false, L"没连上宿主（DSH 没开？）");
      return;
    }

    std::string body;
    if (!dshwork::HttpPostPng(endpoint, L"/dsh-work/api/recognize", png, body, reason)) {
      FinishRecognize(false, L"提交失败：" + dshwork::Widen(reason));
      return;
    }

    dshwork::JValue accepted;
    const std::string job_id = dshwork::ParseJson(body, accepted) ? accepted.GetString("jobId") : std::string();
    if (job_id.empty()) {
      FinishRecognize(false, L"宿主没有返回作业号");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_recognize_mutex);
      g_recognize_job = dshwork::Widen(job_id);
    }
    LogToFile("识别作业 " + job_id);

    const std::wstring poll_path = L"/dsh-work/api/recognize/" + dshwork::Widen(job_id);
    for (int attempt = 0; attempt < 190; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
      std::string polled;
      if (!dshwork::HttpGetJson(endpoint, poll_path, polled, reason)) continue;  // 抖动不算失败，接着等

      dshwork::JValue job;
      if (!dshwork::ParseJson(polled, job)) continue;
      const std::string state = job.GetString("state");
      if (state == "done") {
        const std::wstring title = dshwork::Widen(job.GetString("title"));
        const dshwork::JValue* count = job.Get("count");
        const int n = count == nullptr ? 0 : static_cast<int>(count->number);
        wchar_t text[128];
        if (n > 0 && !title.empty()) {
          swprintf_s(text, L"已拆成 %d 个任务 · %s", n, title.c_str());
        } else if (n > 0) {
          swprintf_s(text, L"已拆成 %d 个任务", n);
        } else {
          swprintf_s(text, L"识别完成，但没拆出任务");
        }
        FinishRecognize(true, text);
        return;
      }
      if (state == "error") {
        const std::wstring why = dshwork::Widen(job.GetString("reason", "模型没有给出可用的任务"));
        FinishRecognize(false, L"识别失败：" + why);
        return;
      }
    }
    FinishRecognize(false, L"识别超时（150 秒没结果）");
  }).detach();
}
void RefreshPanelModel() {
  std::lock_guard<std::mutex> lock(g_model_mutex);
  g_panel_model = g_model;
  if (g_focus < 0 && g_panel_model.current >= 0) g_focus = g_panel_model.current;
  // 长条宽度取决于任务数：开着的当口任务变了，窗口要跟着变宽变窄
  const float wanted = dshwork::ui::StripWidth(static_cast<int>(g_panel_model.items.size()));
  if (wanted != g_panel_dip_w) {
    g_panel_dip_w = wanted;
    if (g_panel_open && g_panel != nullptr && !g_morph.active) {
      const RECT target = PanelTargetRect();
      SetWindowPos(g_panel, HWND_TOPMOST, target.left, target.top, target.right - target.left,
                   target.bottom - target.top, SWP_NOACTIVATE);
    }
  }
}

/**
 * 进入框选：先把我们自己藏干净，再让覆盖层拍屏。
 *
 * 顺序不能反——先把小条和面板收起来，冻结帧里才不会有自己的影子。
 * 结束后由 kMsgCaptureDone 把该露出来的露回来。
 */
void StartRegionCapture() {
  DisarmHoverTab();  // 覆盖层要独占 Tab 进菜单，先把悬停期的那个还掉
  HidePanel(false);
  g_panel_by_hover = false;
  ShowWindow(g_window, SW_HIDE);
  dshwork::capture::SetLog(&LogToFile);
  if (!dshwork::capture::Begin(g_window, &g_capture_path, &g_capture_w, &g_capture_h)) {
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    ShowNotice(L"框选启动失败", 2000);
    LogToFile("框选启动失败");
    return;
  }
  LogToFile("进入框选");
}


/* ───────────────────── 建议：给当前节点要一句下一步 ───────────────────── */

/*
 * 什么时候要：面板打开、且当前节点还没有建议的时候。
 *
 * 三个约束：
 * 1. 同一个节点同一时刻只发一次（g_advice_pending）；
 * 2. 失败后不追着打——同一节点 60 秒内不再重试，否则每秒一次的轮询会变成
 *    每秒一次的模型调用；
 * 3. 全程在后台线程，结果靠 1 秒轮询自然带回来，不额外造一条消息通道。
 */
std::mutex g_advice_mutex;
std::string g_advice_pending;
std::string g_advice_tried_id;
ULONGLONG g_advice_tried_at = 0;

/**
 * 这颗节点是不是「要懂的东西」而不是「要做的事」。
 *
 * 只读模型快照，不碰网络：Worker 线程里调，所以要拿同一把锁。
 * 找不到就当普通节点 —— 宁可按任务处理，也别把讲解塞进任务里。
 */
bool IsConceptNodeId(const std::string& id) {
  std::lock_guard<std::mutex> lock(g_model_mutex);
  for (const dshwork::NodeView& item : g_model.items) {
    if (item.id == id) return item.kind == "concept";
  }
  return false;
}
void RequestAdvice(const std::string& node_id) {
  {
    std::lock_guard<std::mutex> lock(g_advice_mutex);
    if (g_advice_pending == node_id) return;
    if (g_advice_tried_id == node_id && GetTickCount64() - g_advice_tried_at < 60000) return;
    g_advice_pending = node_id;
    g_advice_tried_id = node_id;
    g_advice_tried_at = GetTickCount64();
  }
  std::thread([node_id]() {
    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    if (endpoint.ok) {
      const std::string payload = "{\"nodeId\":\"" + node_id + "\"}";
      std::string body;
      std::string reason;
      // 概念节点问的是"这是什么"，不是"下一步做什么"：走 /api/learn。
      // 两者返回形状一样，所以这里只换路由，取回后的处理完全共用。
      const bool is_concept = IsConceptNodeId(node_id);
      const wchar_t* route = is_concept ? L"/dsh-work/api/learn" : L"/dsh-work/api/advise";
      if (dshwork::HttpPostJson(endpoint, route, payload, body, reason)) {
        LogToFile(std::string(is_concept ? "讲解已取回：" : "建议已取回：") + node_id);
      } else {
        LogToFile("建议失败：" + reason);
      }
    } else {
      LogToFile("建议失败：没连上宿主");
    }
    std::lock_guard<std::mutex> lock(g_advice_mutex);
    if (g_advice_pending == node_id) g_advice_pending.clear();
  }).detach();
}

/** 当前节点还没有建议时去要一条。没当前节点、或者已经有了，就什么都不做。 */
void MaybeRequestAdvice() {
  std::string node_id;
  {
    std::lock_guard<std::mutex> lock(g_model_mutex);
    if (g_model.current < 0 || static_cast<size_t>(g_model.current) >= g_model.items.size()) return;
    const dshwork::NodeView& node = g_model.items[static_cast<size_t>(g_model.current)];
    if (!node.advice.empty()) return;
    node_id = node.id;
  }
  if (node_id.empty()) return;
  RequestAdvice(node_id);
}
void ShowPanel() {
  if (g_panel == nullptr) return;
  RefreshPanelModel();
  MaybeRequestAdvice();
  const RECT target = PanelTargetRect();
  const RECT from = PillRectPx();

  // 药丸让位：展开动画从它自己的矩形长出去，两个窗口同时可见就会看到两坨东西
  ShowWindow(g_window, SW_HIDE);
  ShowWindow(g_panel, SW_SHOWNOACTIVATE);
  // 每次显示都重申置顶：Windows 会在一段时间后把置顶窗口降到普通层（lolPing #70）
  SetWindowPos(g_panel, HWND_TOPMOST, from.left, from.top, from.right - from.left, from.bottom - from.top,
               SWP_NOACTIVATE);

  g_morph.active = true;
  g_morph.opening = true;
  g_morph.start = GetTickCount64();
  g_morph.from = from;
  g_morph.to = target;
  g_morph.ms = static_cast<float>(kPanelOpenMs);
  g_panel_open = true;
  SetTimer(g_window, kAnimTimerId, FrameIntervalMs(), nullptr);
  RepaintPanel();
  LogToFile("面板打开 from=" + std::to_string(from.right - from.left) + "x" +
            std::to_string(from.bottom - from.top) + " to=" + std::to_string(target.right - target.left) + "x" +
            std::to_string(target.bottom - target.top));
}

void HidePanel(bool animated) {
  if (g_panel == nullptr || !g_panel_open) return;
  if (animated) {
    if (g_morph.active && !g_morph.opening) return;  // 正在收，别打断
    RECT from = {};
    GetWindowRect(g_panel, &from);
    g_morph.active = true;
    g_morph.opening = false;
    g_morph.start = GetTickCount64();
    g_morph.from = from;
    g_morph.to = PillRectPx();
    g_morph.ms = static_cast<float>(kPanelCloseMs);
    ArmHoverTab();  // 面板拿走的那几个键要还回去，否则关掉面板后 Tab 就哑了
    SetTimer(g_window, kAnimTimerId, FrameIntervalMs(), nullptr);
    return;
  }
  ShowWindow(g_panel, SW_HIDE);
  ShowWindow(g_window, SW_SHOWNOACTIVATE);
  g_morph.active = false;
  g_panel_open = false;
  ArmHoverTab();  // 同上：把 Tab 还给小条，随时按都能进框选
  LogToFile("面板收起");
}

/**
 * 鼠标是否停在药丸（或面板）上。
 *
 * 小条是 WS_EX_TRANSPARENT 的穿透窗口：它不接收任何鼠标消息，所以"悬停"只能
 * 轮询光标自己算。120ms 一次，人察觉不到，CPU 也几乎不花。
 *
 * 命中区用"画出来的尺寸"而不是窗口尺寸——窗口比药丸宽得多，拿窗口算的话
 * 鼠标飘到屏幕中间偏上一点就会莫名其妙展开面板。
 */
void UpdateHover() {
  if (g_window == nullptr) return;
  // 节点图开着的时候，整条小条一律不认鼠标。
  //
  // 不然会连环触发：鼠标落在图上 → 小条以为要展开面板 → 面板一开又去开图 →
  // 图叠图，节点越堆越多。图上操作期间，小条就当自己不在。
  if (g_review != nullptr && IsWindowVisible(g_review)) {
    if (g_bar_hovered) {
      g_bar_hovered = false;
      Repaint();
    }
    if (g_panel_by_hover && g_panel_open) HidePanel();
    return;
  }
  RECT window_rect = {};
  GetWindowRect(g_window, &window_rect);
  POINT cursor = {};
  GetCursorPos(&cursor);

  const int left = window_rect.left + static_cast<int>(g_hit_left_dip * g_dpiScale);
  const int right = window_rect.left + static_cast<int>(g_hit_right_dip * g_dpiScale);
  const int top = window_rect.top + static_cast<int>(g_hit_top_dip * g_dpiScale);
  const int bottom = window_rect.top + static_cast<int>(g_hit_bottom_dip * g_dpiScale);
  const bool on_bar = cursor.x >= left && cursor.x <= right && cursor.y >= top && cursor.y <= bottom;

  bool on_panel = false;
  RECT panel_rect = {};
  if (g_panel_open) {
    GetWindowRect(g_panel, &panel_rect);
    on_panel = cursor.x >= panel_rect.left && cursor.x <= panel_rect.right && cursor.y >= panel_rect.top &&
               cursor.y <= panel_rect.bottom;
  }

  const bool hovered = on_bar || on_panel;
  static int tick = 0;
  if (++tick % 20 == 0) {
    LogToFile("panel_open=" + std::to_string(g_panel_open ? 1 : 0) + " hit_panel=" + std::to_string(on_panel ? 1 : 0) + " panel_rect=" + std::to_string(panel_rect.left) + "," + std::to_string(panel_rect.top) + "-" + std::to_string(panel_rect.right) + "," + std::to_string(panel_rect.bottom) + " hover 探测 tick=" + std::to_string(tick) + " cursor=" + std::to_string(cursor.x) + "," +
              std::to_string(cursor.y) + " bar=[" + std::to_string(left) + "," + std::to_string(top) + "-" +
              std::to_string(right) + "," + std::to_string(bottom) + "] on_bar=" + std::to_string(on_bar));
  }
  if (hovered == g_bar_hovered) return;
  g_bar_hovered = hovered;
  // 悬停期间把 WS_EX_TRANSPARENT 摘掉：穿透窗口收不到点击，
  // 而单击（开面板）和双击（标记完成）都得靠真的鼠标消息。
  {
    LONG_PTR ex = GetWindowLongPtrW(g_window, GWL_EXSTYLE);
    ex = hovered ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
                 : (ex | static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowLongPtrW(g_window, GWL_EXSTYLE, ex);
  }
  LogToFile(std::string("hover 变化 → ") + (hovered ? "进入" : "离开"));
  Repaint();

  if (hovered) {
    if (!g_panel_open) {
      // 指针一落到小条上，Tab 就属于我们——这是 lolPing 的手势思路：
      // 只在"手已经搭在上面"的那几秒吞键，不去抢走全系统的 Tab。
      ArmHoverTab();
      // 先弹一下（预备），弹完再展开（跟随动作）——
      // 两件事分开做，才有"先有反应、再出东西"的层次。
      StartSquash();
      StartChipFade(0.0f);
      SetTimer(g_window, kAnimTimerId, 16, nullptr);
      SetTimer(g_window, kOpenTimerId, static_cast<UINT>(kOpenDelayMs * g_motion_scale), nullptr);
    }
  } else {
    // 还没弹完就走了：把待开的那一下撤掉，别让面板自己跳出来
    // Tab 不跟着收回（用户要随时能按），这里只撤待开的面板
    KillTimer(g_window, kOpenTimerId);
    StartChipFade(1.0f);
    SetTimer(g_window, kAnimTimerId, 16, nullptr);
    if (g_panel_open && g_panel_by_hover) {
      HidePanel();
      g_panel_by_hover = false;
    }
  }
}

void TogglePanel() {
  if (g_panel_open) {
    HidePanel();
  } else {
    ShowPanel();
  }
  g_panel_by_hover = false;  // 手动开关一次，就归手动管
}

void MoveFocus(int delta) {  if (g_panel_model.items.empty()) return;
  const int count = static_cast<int>(g_panel_model.items.size());
  const int base = g_focus >= 0 ? g_focus : (g_panel_model.current >= 0 ? g_panel_model.current : 0);
  g_focus = (base + delta + count) % count;
  RepaintPanel();
}

LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_LBUTTONDOWN: {
      // 整条都是几块可点的区域，点空白处不做事（面板不抢焦点，也不需要拖动）
      RECT rect = {};
      GetWindowRect(window, &rect);
      const int width = rect.right - rect.left;
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      // 右上角 × 收起
      if (x > width - static_cast<int>(34 * g_dpiScale) && y < static_cast<int>(34 * g_dpiScale)) {
        HidePanel();
        return 0;
      }
      // 「完成」按钮：这一颗做完了，指针前进一格
      {
        const D2D1_RECT_F done =
            dshwork::ui::AdvanceRectAt(g_panel_dip_w, g_panel_dip_h, g_dpiScale);
        if (x >= done.left && x <= done.right && y >= done.top && y <= done.bottom) {
          g_btn_pulse_at = GetTickCount64();
          g_btn_pulse_on = true;
          SetTimer(g_window, kBtnPulseTimerId, 16, nullptr);
          PostIntent("node.advance");
          return 0;
        }
      }
      // 一排点：点哪颗就把焦点挪过去，右边就写那件事
      const int count = static_cast<int>(g_panel_model.items.size());
      if (count > 0) {
        const float scale = g_dpiScale;
        const float dots_x = (dshwork::ui::kPadX + dshwork::ui::kLeftBlock + dshwork::ui::kGapLeft) * scale;
        const int current = g_panel_model.current >= 0 ? g_panel_model.current : 0;
        int visible = dshwork::ui::VisibleDots(g_panel_dip_w);
        if (visible > count) visible = count;
        const int first = dshwork::ui::DotStart(count, current, visible);
        const int hit = HitTestDot(x, y);
        if (hit >= 0) {
          g_focus = hit;
          RepaintPanel();
          return 0;
        }
        (void)dots_x;
        (void)first;
      }
      // 右侧那一段就是「当前这件事」的描述。点它 = 进节点图看全貌。
      // 这是人从"我做到哪了"切到"整张地图长什么样"的入口。
      if (x > static_cast<int>(g_panel_dip_w * 0.42 * g_dpiScale)) {
        if (g_window != nullptr) PostMessageW(g_window, kMsgDocGraph, 0, 0);
        return 0;
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      // 指到哪颗点，右边就写哪件事；移开就回到当前任务
      const int hit = HitTestDot(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit != g_focus) {
        g_focus = hit;
        RepaintPanel();
      }
      // 鼠标在面板上就不要再触发"离开自动收起"
      return 0;
    }
    case WM_DESTROY:
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

/** 面板底部：点阵的命中测试，返回绝对序号（没命中返回 -1）。 */
int HitTestDot(int px, int py) {
  const int count = static_cast<int>(g_panel_model.items.size());
  if (count <= 0) return -1;
  const float scale = g_dpiScale;
  const float dots_x = (dshwork::ui::kPadX + dshwork::ui::kLeftBlock + dshwork::ui::kGapLeft) * scale;
  RECT rect = {};
  GetWindowRect(g_panel, &rect);
  const float dots_cy = static_cast<float>(rect.bottom - rect.top) * 0.5f;
  if (py < dots_cy - 16.0f * scale || py > dots_cy + 16.0f * scale) return -1;

  const int current = g_panel_model.current >= 0 ? g_panel_model.current : 0;
  int visible = dshwork::ui::VisibleDots(g_panel_dip_w);
  if (visible > count) visible = count;
  const int first = dshwork::ui::DotStart(count, current, visible);
  for (int slot = 0; slot < visible; ++slot) {
    const float cx = dots_x + (static_cast<float>(slot) + 0.5f) * dshwork::ui::kDotStep * scale;
    if (px >= cx - 13.0f * scale && px <= cx + 13.0f * scale) return first + slot;
  }
  return -1;
}

/**
 * Tab 常驻：只要伴随层活着就注册着，随时按都能进框选。
 *
 * 只有覆盖层自己开着时才让给它（覆盖层要用 Tab 当『就它了』），收摊后立刻还回来。
 *
 * 代价说清楚：RegisterHotKey 是独占的，注册期间别的程序收不到 Tab。
 */
void ArmHoverTab() {
  if (g_hover_tab_armed) return;
  if (!RegisterHotKey(g_window, kTabHotkeyId, MOD_NOREPEAT, VK_TAB)) {
    LogToFile("Tab 常驻注册失败（多半被别的程序占了）");
    return;
  }
  g_hover_tab_armed = true;
}

void DisarmHoverTab() {
  if (!g_hover_tab_armed) return;
  UnregisterHotKey(g_window, kTabHotkeyId);
  g_hover_tab_armed = false;
}

/** 托盘：右键菜单里至少要有"退出"，否则这进程只能去任务管理器杀。 */
/** 打开「说一件事」输入框（Ctrl+Alt+N）。 */
void OpenPrompt() {
  HidePanel();
  ShowWindow(g_window, SW_HIDE);
  if (dshwork::ui::prompt::Open(g_window) == nullptr) {
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    ShowNotice(L"输入框打不开", 2000);
  }
}

std::string JsonEscape(const std::string& utf8) {
  std::string out;
  out.reserve(utf8.size() + 16);
  for (const char ch : utf8) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (byte < 0x20) {
      char buffer[8] = {};
      sprintf_s(buffer, "\\u%04x", byte);
      out += buffer;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

/** 提交一句话：后台请求 /api/plan-text，结果用消息回 UI 线程。 */
void SubmitPrompt(const std::wstring& text) {
  if (text.empty()) return;
  ShowWindow(g_window, SW_SHOWNOACTIVATE);
  ShowNotice(L"正在拆…", 120000);
  const std::string utf8 = dshwork::Narrow(text);
  std::thread([utf8]() {
    const dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    std::string body;
    std::string reason;
    if (!endpoint.ok) {
      PostMessageW(g_window, kMsgPlanTextDone, 0, 0);
      return;
    }
    const std::string payload = "{\"text\":\"" + JsonEscape(utf8) + "\"}";
    const bool ok = dshwork::HttpPostJson(endpoint, L"/dsh-work/api/plan-text", payload, body, reason);
    std::wstring note;
    if (ok) {
      // {"ok":true,"title":"…","count":7}
      const std::string marker = "\"count\":";
      const size_t at = body.find(marker);
      int count = 0;
      if (at != std::string::npos) count = std::atoi(body.c_str() + at + marker.size());
      note = L"已拆成 " + std::to_wstring(count) + L" 个任务";
    } else {
      note = L"规划失败：" + dshwork::Widen(reason.empty() ? std::string("未知原因") : reason);
    }
    {
      std::lock_guard<std::mutex> guard(g_plan_mutex);
      g_plan_note = note;
    }
    PostMessageW(g_window, kMsgPlanTextDone, ok ? 1 : 0, 0);
  }).detach();
}

void CreateTrayIcon() {
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = g_window;
  data.uID = kTrayIconId;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kMsgTray;
  data.hIcon = LoadIconW(g_instance, IDI_APPLICATION);
  wcscpy_s(data.szTip, L"DSH Work");
  if (!Shell_NotifyIconW(NIM_ADD, &data)) LogToFile("托盘图标添加失败");
}

void ShowTrayMenu() {
  POINT cursor = {};
  GetCursorPos(&cursor);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, g_panel_open ? L"收起面板" : L"打开面板");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 2, L"退出");
  SetForegroundWindow(g_window);
  const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window, nullptr);
  DestroyMenu(menu);
  if (command == 1) TogglePanel();
  if (command == 2) {
    PostQuitMessage(0);
    PostMessageW(g_window, WM_CLOSE, 0, 0);
  }
}

void DispatchKey(int action) {
  switch (action) {
    case 1: {  // Enter：选中的就是当前任务就完成它，否则先把指针挪过去
      if (g_panel_model.items.empty()) break;
      const int count = static_cast<int>(g_panel_model.items.size());
      const int index = g_focus >= 0 && g_focus < count ? g_focus : g_panel_model.current;
      if (index < 0) break;
      const dshwork::NodeView& node = g_panel_model.items[static_cast<size_t>(index)];
      if (index == g_panel_model.current) {
        std::thread(PostIntent, std::string("node.advance"), std::string()).detach();
      } else if (node.status == "todo") {
        std::thread(PostIntent, std::string("node.focus"), node.id).detach();
      }
      break;
    }
    case 2:  // Backspace
      std::thread(PostIntent, std::string("node.back"), std::string()).detach();
      break;
    case 3:  // Esc
      HidePanel();
      break;
    case 4:
      MoveFocus(-1);
      break;
    case 5:
      MoveFocus(1);
      break;
    case 6:  // Tab：进入框选
      StartRegionCapture();
      break;
    default:
      break;
  }
}



}  // namespace

/**
 * 崩溃兜底：把异常地址写进日志。
 *
 * 被杀软掐掉和"自己段错误"在外部看起来一模一样（进程无声消失），
 * 有这一句就能分清是哪一种：有日志=我们的 bug，没日志=被杀。
 */
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  char text[160] = {};
  const void* address = (info != nullptr && info->ExceptionRecord != nullptr)
                            ? info->ExceptionRecord->ExceptionAddress
                            : nullptr;
  const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
                         ? info->ExceptionRecord->ExceptionCode
                         : 0;
  sprintf_s(text, "崩溃 code=0x%08lX addr=%p", code, address);
  LogToFile(text);
  return EXCEPTION_EXECUTE_HANDLER;
}

int main() {
  SetUnhandledExceptionFilter(CrashFilter);
  // 逐显示器 DPI 感知：不然 150% 缩放下整条会画成模糊的
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // 调试用：把动效整体放慢，方便逐帧看曲线（正常运行时这个环境变量不存在）
  {
    wchar_t slowmo[16] = {};
    if (GetEnvironmentVariableW(L"DSHWORK_SLOWMO", slowmo, 16) > 0) {
      const int factor = _wtoi(slowmo);
      if (factor > 0) g_motion_scale = static_cast<float>(factor);
    }
  }

  g_instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW window_class = {};
  // CS_DBLCLKS：小条要区分单击（开面板）和双击（标记完成）
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = g_instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  window_class.style |= CS_DBLCLKS;
  if (RegisterClassExW(&window_class) == 0) return 1;

  g_window = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kWindowClass,
      L"DSH Work", WS_POPUP, 0, 0, static_cast<int>(kWindowWidth), static_cast<int>(kWindowHeight), nullptr, nullptr,
      g_instance, nullptr);
  if (g_window == nullptr) return 1;

  UpdateDpi();
  if (!CreateGraphics()) {
    LogToFile("Direct2D/DirectWrite 初始化失败");
    return 1;
  }

  // 面板先建好但藏着：热键按下时才 ShowWindow，省掉每次创建窗口的抖动
  WNDCLASSEXW panel_class = window_class;
  panel_class.lpfnWndProc = PanelProc;
  panel_class.lpszClassName = kPanelClass;
  if (RegisterClassExW(&panel_class) == 0) LogToFile("面板窗口类注册失败");
  g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kPanelClass,
                            L"DSH Work 面板", WS_POPUP, 0, 0,
                            static_cast<int>(dshwork::ui::StripWidth(1) * 1.0),
                            static_cast<int>(dshwork::ui::kStripHeight), nullptr, nullptr, g_instance, nullptr);
  if (g_panel == nullptr) LogToFile("面板窗口创建失败");

  PositionWindow();
  Repaint();
  CreateTrayIcon();
  ArmHoverTab();  // Tab 常驻：随时按都该能进框选／菜单
  if (!RegisterHotKey(g_window, kPromptHotkeyId, MOD_CONTROL | MOD_ALT, 'N')) {
    LogToFile("说一件事热键注册失败（Ctrl+Alt+N 可能被占用）");
  }
  if (!RegisterHotKey(g_window, kHotkeyId, MOD_CONTROL | MOD_ALT, VK_SPACE)) {
    LogToFile("全局热键 Ctrl+Alt+Space 注册失败（可能被别的软件占了）");
  }
  {
    wchar_t fps[96];
    const UINT hz = RefreshHzOf(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY));
    swprintf_s(fps, L"原生小条启动 刷新率=%uHz 帧间隔=%ums", hz, FrameIntervalMs());
    LogToFile(dshwork::Narrow(fps));
  }

  // 悬停轮询：小条是穿透窗口，收不到鼠标消息，只能自己看光标
  SetTimer(g_window, kHoverTimerId, 120, nullptr);

  std::thread poller(PollLoop);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  g_polling.store(false);
  poller.join();
  if (g_brush != nullptr) g_brush->Release();
  if (g_target != nullptr) g_target->Release();
  if (g_formatTitle != nullptr) g_formatTitle->Release();
  if (g_formatMeta != nullptr) g_formatMeta->Release();
  if (g_dwrite != nullptr) g_dwrite->Release();
  if (g_d2d != nullptr) g_d2d->Release();
  return 0;
}
