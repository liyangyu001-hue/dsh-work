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

#include "host.h"
#include "json.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winhttp.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"DSHWorkNativeBar";
constexpr UINT kMsgModel = WM_APP + 1;
constexpr UINT kMsgPaint = WM_APP + 2;

// 几何（DIP，绘制时乘 dpi/96）
constexpr float kWindowWidth = 640.0f;
constexpr float kWindowHeight = 96.0f;
constexpr float kPillHeight = 44.0f;
constexpr float kPaddingX = 18.0f;
constexpr float kDotRadius = 4.0f;
constexpr float kDotGap = 10.0f;
constexpr float kProgressGap = 12.0f;

/** 宿主状态 → 小条要显示的东西。UI 线程只认这个结构。 */
struct BarModel {
  enum class State { Offline, Connecting, Online } state = State::Connecting;
  std::wstring title;
  int done = 0;
  int total = 0;
  std::string note;  // 离线原因，排版时优先显示
};

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

/** 从宿主文档里算出小条要的三样东西。取不到就返回 false，交给上层显示离线。 */
bool ModelFromDoc(const std::string& body, BarModel& out) {
  dshwork::JValue root;
  if (!dshwork::ParseJson(body, root) || !root.IsObject()) return false;

  // /api/state 的外层是 { doc: {...} }，真正的文档在 doc 里；
  // 但别写死——直接给文档的形态也认，免得宿主改一层就得跟着改。
  const dshwork::JValue* doc = root.Get("doc");
  if (doc == nullptr || !doc->IsObject()) doc = &root;

  const dshwork::JValue* nodes = doc->Get("nodes");
  if (nodes == nullptr || !nodes->IsObject()) return false;

  int done = 0;
  for (const auto& entry : nodes->object) {
    const dshwork::JValue* status = entry.second.Get("status");
    if (status != nullptr && status->IsString() && status->text == "done") ++done;
  }
  out.done = done;
  out.total = static_cast<int>(nodes->object.size());

  const std::string pointer = doc->GetString("pointer");
  if (pointer.empty()) {
    out.title = L"全部已完成";
    out.state = BarModel::State::Online;
    return true;
  }

  const dshwork::JValue* node = nodes->Get(pointer);
  if (node == nullptr) {
    out.title = L"当前节点不见了";
    out.state = BarModel::State::Online;
    return true;
  }
  out.title = dshwork::Widen(node->GetString("title"));
  out.state = BarModel::State::Online;
  return true;
}

void PollLoop() {
  using namespace std::chrono_literals;
  std::string last_reason;
  while (g_polling.load(std::memory_order_relaxed)) {
    // 每次都重读 endpoint.json：宿主重启会换端口和 token，缓存下来就永远连不上
    dshwork::Endpoint endpoint = dshwork::ReadEndpoint();
    BarModel model;
    if (!endpoint.ok) {
      model.state = BarModel::State::Offline;
      model.note = endpoint.error;
      model.title = L"DSH 未连接";
    } else {
      std::string body;
      std::string reason;
      if (dshwork::HttpGetJson(endpoint, L"/dsh-work/api/state", body, reason)) {
        if (!ModelFromDoc(body, model)) {
          model.state = BarModel::State::Offline;
          model.note = "状态文档解析失败";
          model.title = L"DSH 数据异常";
        }
        last_reason.clear();
      } else {
        model.state = BarModel::State::Offline;
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
  if (model.state == BarModel::State::Offline) dot_color = 0xFFFF453A;
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
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
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

  PositionWindow();
  Repaint();
  LogToFile("原生小条启动");

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
