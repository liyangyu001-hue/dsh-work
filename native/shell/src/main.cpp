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
#include <string>
#include <thread>

#include "doc.h"
#include "host.h"
#include "panel.h"
#include "json.h"

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
/** 鼠标是否停在小条上。小条是穿透窗口收不到 WM_MOUSEMOVE，只能轮询光标。 */
bool g_bar_hovered = false;
/** 上一次绘制出来的可悬停范围（DIP，相对窗口左上角）。药丸宽度跟着文字变，只能画完才知道。 */
float g_hit_left_dip = 0.0f;
float g_hit_right_dip = 0.0f;
float g_hit_top_dip = 0.0f;
float g_hit_bottom_dip = 0.0f;

void Repaint();
void ShowPanel();
void HidePanel();
int g_focus = -1;  // 面板里选中第几张卡；-1 表示跟随当前任务
dshwork::WorkModel g_panel_model;
IDWriteTextFormat* g_formatPanelTitle = nullptr;
IDWriteTextFormat* g_formatBody = nullptr;
IDWriteTextFormat* g_formatPanelMeta = nullptr;
HHOOK g_keyboard_hook = nullptr;

constexpr wchar_t kPanelClass[] = L"DSHWorkNativePanel";
constexpr UINT kMsgTray = WM_APP + 3;
constexpr UINT kHotkeyId = 0x51;
constexpr int kTrayIconId = 1;

// 这几个函数在窗口过程里就要用，定义在下面，先声明
void RepaintPanel();
void RefreshPanelModel();
void DispatchKey(int action);
void TogglePanel();
void UpdateHover();
void ShowTrayMenu();
LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam);

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
  while (g_polling.load(std::memory_order_relaxed)) {
    // 每次都重读 endpoint.json：宿主重启会换端口和 token，缓存下来就永远连不上
    dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    BarModel model;
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

    for (int i = 0; i < 10 && g_polling.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(100ms);
    }
  }
}

D2D1_COLOR_F ColorOf(UINT32 argb) {
  return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
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
  std::wstring title = model.title.empty() ? L"正在连接 DSH…" : model.title;
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

  // ── 两侧的"邻居"：不做 hover 时挂出来，一旦鼠标上来就收回去让位给面板 ──
  // 左边 = 已经走过的（淡淡地退到后面），右边 = 还没到的（提醒还有多少）。
  // 注意：不管画不画，命中区都按"挂出来"的尺寸算，否则鼠标一进来卡片消失、
  // 命中区跟着缩小，就变成自己把自己踢出去的抖动。
  if (model.current >= 0) {
    const int count = static_cast<int>(model.items.size());
    const float chip_h = pill_h * 0.78f;
    const float chip_top = (height - chip_h) * 0.5f;
    const float chip_w = kSideChipWidth * scale;
    const float gap = kSideChipGap * scale;

    for (int side = -1; side <= 1; side += 2) {
      int drawn = 0;
      for (int step = 1; step <= kSideChipCount; ++step) {
        const int index = model.current + side * step;
        if (index < 0 || index >= count) break;
        // 逐层后退：越远越矮、越淡
        const float shrink = static_cast<float>(drawn) * 6.0f * scale;
        const float left = (side < 0) ? pill_left - gap - chip_w - shrink - static_cast<float>(drawn) * (chip_w * 0.55f)
                                      : pill_left + pill_w + gap + shrink + static_cast<float>(drawn) * (chip_w * 0.55f);
        const D2D1_RECT_F chip = D2D1::RectF(left, chip_top + shrink * 0.5f, left + chip_w,
                                             chip_top + chip_h - shrink * 0.5f);
        const float alpha = 0.055f - static_cast<float>(drawn) * 0.015f;
        g_brush->SetColor(D2D1::ColorF(1, 1, 1, alpha));
        // 鼠标一上来就不画了：不是删掉，是让位。面板会把同样的事讲得更清楚。
        if (!g_bar_hovered) {
          target->FillRoundedRectangle(D2D1::RoundedRect(chip, chip_h * 0.42f, chip_h * 0.42f), g_brush);
        }

        const dshwork::NodeView& node = model.items[static_cast<size_t>(index)];
        if (left < hit_left) hit_left = left;
        if (left + chip_w > hit_right) hit_right = left + chip_w;
        if (g_bar_hovered) {
          ++drawn;
          continue;
        }
        IDWriteTextLayout* chip_layout = nullptr;
        g_dwrite->CreateTextLayout(node.title.c_str(), static_cast<UINT32>(node.title.size()), g_formatMeta,
                                   chip_w - 16.0f * scale, chip_h, &chip_layout);
        if (chip_layout != nullptr) {
          g_brush->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
          target->DrawTextLayout(D2D1::Point2F(left + 10.0f * scale, chip_top + (chip_h - pill_h * 0.34f) * 0.5f),
                                 chip_layout, g_brush);
          chip_layout->Release();
        }
        ++drawn;
      }
    }
  }

  // 阴影：三层同心圆角矩形叠出柔和感（没有 GPU 模糊时的老办法，够用且零依赖）
  const float radius = pill_h * 0.5f;
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

  // 标题
  const float text_x = dot_cx + kDotRadius * scale + kDotGap * scale;
  g_brush->SetColor(D2D1::ColorF(0.96f, 0.96f, 0.97f, 1.0f));
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

/** 贴在工作区顶部正中。多显示器时跟着主屏，缩放变化要重算。 */
void PositionWindow() {
  HMONITOR monitor = MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);

  const int px_w = static_cast<int>(kWindowWidth * g_dpiScale);
  const int px_h = static_cast<int>(kWindowHeight * g_dpiScale);
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - px_w) / 2;
  const int y = info.rcWork.top + static_cast<int>(4 * g_dpiScale);
  SetWindowPos(g_window, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Windows 会悄悄把置顶窗口降级（lolPing issue #70），每次显示都要重申
  SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

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
    case kMsgKey:
      DispatchKey(static_cast<int>(wparam));
      return 0;
    case WM_HOTKEY:
      if (wparam == kHotkeyId) TogglePanel();
      return 0;
    case WM_TIMER:
      if (wparam == kHoverTimerId) {
        UpdateHover();
        return 0;
      }
      break;
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
      if (g_keyboard_hook != nullptr) {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
      }
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

  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN",
                                        &g_formatTitle)))
    return false;
  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN",
                                        &g_formatMeta)))
    return false;

  if (FAILED(g_dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.5f, L"zh-CN",
                                        &g_formatPanelTitle)))
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

void PositionPanel() {
  if (g_panel == nullptr) return;
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromWindow(g_panel, MONITOR_DEFAULTTOPRIMARY), &info);

  const int px_w = static_cast<int>(dshwork::ui::kPanelWidth * g_dpiScale);
  const int px_h = static_cast<int>(dshwork::ui::kPanelHeight * g_dpiScale);
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - px_w) / 2;
  // 小条窗口高 96，药丸占中间 44 左右；62 让面板上沿正好接在药丸下缘
  const int y = info.rcWork.top + static_cast<int>(82 * g_dpiScale);
  SetWindowPos(g_panel, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE);
}

void RepaintPanel() {
  if (g_panel == nullptr) return;
  const int px_w = static_cast<int>(dshwork::ui::kPanelWidth * g_dpiScale);
  const int px_h = static_cast<int>(dshwork::ui::kPanelHeight * g_dpiScale);

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
  dshwork::PanelFormats formats{g_formatPanelTitle, g_formatBody, g_formatPanelMeta, g_brush};
  dshwork::ui::DrawPanel(g_target, model, g_focus, g_dpiScale, formats);
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

void RefreshPanelModel() {
  std::lock_guard<std::mutex> lock(g_model_mutex);
  g_panel_model = g_model;
  if (g_focus < 0 && g_panel_model.current >= 0) g_focus = g_panel_model.current;
}

void ShowPanel() {
  if (g_panel == nullptr) return;
  RefreshPanelModel();
  PositionPanel();
  ShowWindow(g_panel, SW_SHOWNOACTIVATE);
  // 每次显示都重申置顶：Windows 会在一段时间后把置顶窗口降到普通层（lolPing #70）
  SetWindowPos(g_panel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  g_panel_open = true;
  if (g_keyboard_hook == nullptr) {
    g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_instance, 0);
    if (g_keyboard_hook == nullptr) LogToFile("键盘钩子安装失败，面板将收不到按键");
  }
  RepaintPanel();
  RECT panel_rect = {};
  GetWindowRect(g_panel, &panel_rect);
  LogToFile("面板打开 rect=" + std::to_string(panel_rect.left) + "," + std::to_string(panel_rect.top) + " " +
            std::to_string(panel_rect.right - panel_rect.left) + "x" +
            std::to_string(panel_rect.bottom - panel_rect.top));
}

void HidePanel() {
  if (g_panel == nullptr || !g_panel_open) return;
  ShowWindow(g_panel, SW_HIDE);
  g_panel_open = false;
  if (g_keyboard_hook != nullptr) {
    UnhookWindowsHookEx(g_keyboard_hook);
    g_keyboard_hook = nullptr;
  }
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
  if (g_panel_open) {
    RECT panel_rect = {};
    GetWindowRect(g_panel, &panel_rect);
    on_panel = cursor.x >= panel_rect.left && cursor.x <= panel_rect.right && cursor.y >= panel_rect.top &&
               cursor.y <= panel_rect.bottom;
  }

  const bool hovered = on_bar || on_panel;
  static int tick = 0;
  if (++tick % 20 == 0) {
    LogToFile("hover 探测 tick=" + std::to_string(tick) + " cursor=" + std::to_string(cursor.x) + "," +
              std::to_string(cursor.y) + " bar=[" + std::to_string(left) + "," + std::to_string(top) + "-" +
              std::to_string(right) + "," + std::to_string(bottom) + "] on_bar=" + std::to_string(on_bar));
  }
  if (hovered == g_bar_hovered) return;
  g_bar_hovered = hovered;
  LogToFile(std::string("hover 变化 → ") + (hovered ? "进入" : "离开"));
  Repaint();

  if (hovered && !g_panel_open) {
    ShowPanel();
    g_panel_by_hover = true;
  } else if (!hovered && g_panel_open && g_panel_by_hover) {
    HidePanel();
    g_panel_by_hover = false;
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
      // 只认右上角那个 ×，别的一律忽略（面板不抢焦点，也不需要拖动）
      const int x = GET_X_LPARAM(lparam);
      const int width = static_cast<int>(dshwork::ui::kPanelWidth * g_dpiScale);
      if (x > width - static_cast<int>(70 * g_dpiScale) && GET_Y_LPARAM(lparam) < static_cast<int>(60 * g_dpiScale)) {
        HidePanel();
      }
      return 0;
    }
    case WM_DESTROY:
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

/** 托盘：右键菜单里至少要有"退出"，否则这进程只能去任务管理器杀。 */
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
    default:
      break;
  }
}

/** 0 表示不拦，其余是动作号。 */
int ActionOf(UINT vk) {
  switch (vk) {
    case VK_RETURN: return 1;
    case VK_BACK: return 2;
    case VK_ESCAPE: return 3;
    case VK_LEFT: return 4;
    case VK_RIGHT: return 5;
    default: return 0;
  }
}

/**
 * 低级键盘钩子：只在这几个键、且面板开着时把它们吃掉。
 * 这是 lolPing 的做法——面板窗口 WS_EX_NOACTIVATE 收不到键盘，
 * 与其抢焦点（会打断用户正在打的字），不如在系统层截走并立刻转给面板。
 */
LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_panel_open) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const int action = ActionOf(key->vkCode);
    if (action != 0) {
      if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) PostMessageW(g_window, kMsgKey, action, 0);
      return 1;  // 按下与抬起都吃掉，避免下层程序收到半个手势
    }
  }
  return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
}
}  // namespace

int main() {
  // 逐显示器 DPI 感知：不然 150% 缩放下整条会画成模糊的
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  g_instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = g_instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
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
                            static_cast<int>(dshwork::ui::kPanelWidth),
                            static_cast<int>(dshwork::ui::kPanelHeight), nullptr, nullptr, g_instance, nullptr);
  if (g_panel == nullptr) LogToFile("面板窗口创建失败");

  PositionWindow();
  Repaint();
  CreateTrayIcon();
  if (!RegisterHotKey(g_window, kHotkeyId, MOD_CONTROL | MOD_ALT, VK_SPACE)) {
    LogToFile("全局热键 Ctrl+Alt+Space 注册失败（可能被别的软件占了）");
  }
  LogToFile("原生小条启动");

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
