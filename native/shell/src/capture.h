// ─────────────────────────────────────────────────────────────────────────────
// 框选覆盖层：把整块虚拟桌面拍下来 → 全屏盖一层压暗的图 → 拖框 → 亮出框内 → 存 PNG
//
// 为什么不是"实时截屏"：拖框时每一帧都去 BitBlt 会把下面的窗口动画拍进来，
// 选框里的人头像会跟着动，像在给活的屏幕拍照。先拍一张冻结帧，后面全在这张
// 图上操作，柜子里的东西才不会跑。
//
// 为什么压暗而不是画半透明遮罩：分层窗口上再叠一层半透明黑，Window 会把
// 两张图层都做 alpha 合成，边缘会出现半透明的双影；直接改像素的 RGB 更干净，
// 而且能做到"框内完全透亮、框外只剩 45%"这种硬边界。
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace dshwork::capture {

constexpr wchar_t kCaptureClass[] = L"DSHWorkNativeCapture";
constexpr UINT kMsgCaptureDone = WM_APP + 5;  // 发给主窗口：0=取消，1=完成

/** 压暗多少。0.55 保留 55% 亮度，框内原样。 */
constexpr float kDim = 0.55f;
/** 少于这个边长当成误触，不放行。 */
constexpr int kMinSide = 8;
/** 两次重绘之间至少隔这么久，鼠标再快也不至于把 CPU 打满。 */
constexpr ULONGLONG kPaintIntervalMs = 8;

struct State {
  bool active = false;
  HWND owner = nullptr;
  HWND window = nullptr;

  int vx = 0, vy = 0, vw = 0, vh = 0;  // 虚拟屏幕矩形（物理像素）

  HDC screen = nullptr;
  HBITMAP shot_bitmap = nullptr;
  void* shot_bits = nullptr;
  HDC shot = nullptr;  // 冻结帧

  HBITMAP dim_bitmap = nullptr;
  void* dim_bits = nullptr;
  HDC dim = nullptr;  // 压暗后的底图（每次拖框从它复制，避免逐像素重算）

  HBITMAP work_bitmap = nullptr;
  void* work_bits = nullptr;
  HDC work = nullptr;  // 每帧要交给 UpdateLayeredWindow 的那张

  bool dragging = false;
  POINT anchor = {};
  RECT selection = {};
  ULONGLONG last_paint = 0;

  std::wstring* out_path = nullptr;
  int* out_w = nullptr;
  int* out_h = nullptr;
};

inline State& S() {
  static State state;
  return state;
}

inline BITMAPINFO MakeBitmapInfo(int width, int height) {
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // 负数 = 自上而下，和屏幕坐标同向
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  return info;
}

/** 建一张和自己的 DC 无关的 32bpp 位图，返回位；失败返回 nullptr。 */
inline void* CreateSurface(HDC reference, int width, int height, HBITMAP* bitmap, HDC* dc) {
  BITMAPINFO info = MakeBitmapInfo(width, height);
  void* bits = nullptr;
  *bitmap = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (*bitmap == nullptr) return nullptr;
  *dc = CreateCompatibleDC(reference);
  if (*dc == nullptr) return nullptr;
  SelectObject(*dc, *bitmap);
  return bits;
}

inline void CopyPixels(void* dst, const void* src, size_t bytes) {
  memcpy(dst, src, bytes);
}

// ── PNG 落盘（WIC）────────────────────────────────────────────────────────────

/** 把 BGRX 的像素写成 PNG。返回是否成功。 */
inline bool SavePng(const std::wstring& path, const void* pixels, int width, int height) {
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) {
    return false;
  }

  bool ok = false;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IWICBitmap* bitmap = nullptr;

  do {
    if (FAILED(factory->CreateStream(&stream))) break;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) break;
    if (FAILED(frame->Initialize(nullptr))) break;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) break;
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) break;

    const UINT stride = static_cast<UINT>(width) * 4;
    const UINT size = stride * static_cast<UINT>(height);
    if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                               GUID_WICPixelFormat32bppBGR, stride, size,
                                               static_cast<BYTE*>(const_cast<void*>(pixels)), &bitmap))) {
      break;
    }
    if (FAILED(frame->WriteSource(bitmap, nullptr))) break;
    if (FAILED(frame->Commit())) break;
    if (FAILED(encoder->Commit())) break;
    ok = true;
  } while (false);

  if (bitmap != nullptr) bitmap->Release();
  if (frame != nullptr) frame->Release();
  if (encoder != nullptr) encoder->Release();
  if (stream != nullptr) stream->Release();
  factory->Release();
  return ok;
}

/** captures 目录，不存在就建。 */
inline std::wstring CapturesDir() {
  wchar_t profile[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return L".";
  std::wstring dir = std::wstring(profile) + L"\\.dsh\\dsh-work\\captures";
  CreateDirectoryW((std::wstring(profile) + L"\\.dsh\\dsh-work").c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

inline std::wstring TimestampedPath() {
  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t name[64];
  swprintf_s(name, L"\\%04d%02d%02d-%02d%02d%02d-%03d.png", now.wYear, now.wMonth, now.wDay, now.wHour,
             now.wMinute, now.wSecond, now.wMilliseconds);
  return CapturesDir() + name;
}

// ── 绘制 ─────────────────────────────────────────────────────────────────────

/**
 * 把 this frame 合成到 work 上：
 * 1) 先整块复制压暗底图（这步是 memcpy，比逐像素算快两个数量级）
 * 2) 把选框里的原始像素贴回去
 * 3) 画选框边和内圈的一像素白线
 */
inline void Compose() {
  State& s = S();
  const size_t bytes = static_cast<size_t>(s.vw) * static_cast<size_t>(s.vh) * 4;
  CopyPixels(s.work_bits, s.dim_bits, bytes);

  if (s.dragging) {
    const RECT& r = s.selection;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w > 0 && h > 0) {
      // 选框内的原始像素：逐行从冻结帧搬，一次一行 memcpy
      const size_t row_bytes = static_cast<size_t>(w) * 4;
      for (int y = 0; y < h; ++y) {
        const auto* src = static_cast<const BYTE*>(s.shot_bits) +
                          (static_cast<size_t>(r.top + y) * static_cast<size_t>(s.vw) + r.left) * 4;
        auto* dst = static_cast<BYTE*>(s.work_bits) +
                    (static_cast<size_t>(r.top + y) * static_cast<size_t>(s.vw) + r.left) * 4;
        memcpy(dst, src, row_bytes);
      }

      // 边框：外圈两像素白 + 内圈一像素半透明白，看着像"揭开的纸边"
      auto put = [&](int x, int y, BYTE a, BYTE rr, BYTE gg, BYTE bb) {
        if (x < 0 || y < 0 || x >= s.vw || y >= s.vh) return;
        auto* px = static_cast<BYTE*>(s.work_bits) +
                   (static_cast<size_t>(y) * static_cast<size_t>(s.vw) + x) * 4;
        px[0] = static_cast<BYTE>(px[0] * (1.0f - a / 255.0f) + bb * (a / 255.0f));
        px[1] = static_cast<BYTE>(px[1] * (1.0f - a / 255.0f) + gg * (a / 255.0f));
        px[2] = static_cast<BYTE>(px[2] * (1.0f - a / 255.0f) + rr * (a / 255.0f));
      };
      for (int x = r.left; x < r.right; ++x) {
        put(x, r.top, 255, 255, 255, 255);
        put(x, r.top - 1, 255, 255, 255, 255);
        put(x, r.bottom - 1, 255, 255, 255, 255);
        put(x, r.bottom, 255, 255, 255, 255);
      }
      for (int y = r.top; y < r.bottom; ++y) {
        put(r.left, y, 255, 255, 255, 255);
        put(r.left - 1, y, 255, 255, 255, 255);
        put(r.right - 1, y, 255, 255, 255, 255);
        put(r.right, y, 255, 255, 255, 255);
      }
    }
  }
}

inline void Paint() {
  State& s = S();
  if (!s.active || s.window == nullptr) return;

  const ULONGLONG now = GetTickCount64();
  if (now - s.last_paint < kPaintIntervalMs) return;
  s.last_paint = now;

  Compose();

  POINT dest = {s.vx, s.vy};
  SIZE size = {s.vw, s.vh};
  POINT source = {0, 0};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(s.window, nullptr, &dest, &size, s.work, &source, 0, &blend, ULW_ALPHA);
}

// ── 收尾 ─────────────────────────────────────────────────────────────────────

inline void Teardown(int result);

inline void Finish() {
  State& s = S();
  if (!s.active) return;
  const bool selection_ok = s.dragging && (s.selection.right - s.selection.left) >= kMinSide &&
                            (s.selection.bottom - s.selection.top) >= kMinSide;

  if (!selection_ok) {
    Teardown(0);
    return;
  }

  const int w = s.selection.right - s.selection.left;
  const int h = s.selection.bottom - s.selection.top;
  const std::wstring path = TimestampedPath();

  // 只把选框那一块抠出来另存；整张桌面不落盘（用户只框了一小块，
  // 旁边可能就开着不该进仓库的窗口）
  HBITMAP crop_bitmap = nullptr;
  HDC crop_dc = nullptr;
  void* crop_bits = nullptr;
  bool saved = false;
  if (CreateSurface(s.screen, w, h, &crop_bitmap, &crop_dc) != nullptr) {
    crop_bits = nullptr;
    BITMAPINFO info = MakeBitmapInfo(w, h);
    DeleteObject(crop_bitmap);
    DeleteDC(crop_dc);
    crop_bitmap = CreateDIBSection(s.screen, &info, DIB_RGB_COLORS, &crop_bits, nullptr, 0);
    if (crop_bitmap != nullptr) {
      const size_t row_bytes = static_cast<size_t>(w) * 4;
      for (int y = 0; y < h; ++y) {
        const auto* src = static_cast<const BYTE*>(s.shot_bits) +
                          (static_cast<size_t>(s.selection.top + y) * static_cast<size_t>(s.vw) +
                           s.selection.left) * 4;
        auto* dst = static_cast<BYTE*>(crop_bits) + static_cast<size_t>(y) * row_bytes;
        memcpy(dst, src, row_bytes);
      }
      saved = SavePng(path, crop_bits, w, h);
      DeleteObject(crop_bitmap);
    }
  }

  if (saved) {
    if (s.out_path != nullptr) *s.out_path = path;
    if (s.out_w != nullptr) *s.out_w = w;
    if (s.out_h != nullptr) *s.out_h = h;
  }
  Teardown(saved ? 1 : 0);
}

inline void Teardown(int result) {
  State& s = S();
  UnregisterHotKey(s.window, 0x71);
  if (s.window != nullptr) {
    ShowWindow(s.window, SW_HIDE);
    DestroyWindow(s.window);
    s.window = nullptr;
  }
  if (s.screen != nullptr) {
    ReleaseDC(nullptr, s.screen);
    s.screen = nullptr;
  }
  auto drop = [](HBITMAP bitmap, HDC dc) {
    if (dc != nullptr) DeleteDC(dc);
    if (bitmap != nullptr) DeleteObject(bitmap);
  };
  drop(s.shot_bitmap, s.shot);
  drop(s.dim_bitmap, s.dim);
  drop(s.work_bitmap, s.work);
  s.shot_bitmap = nullptr; s.shot = nullptr;
  s.dim_bitmap = nullptr; s.dim = nullptr;
  s.work_bitmap = nullptr; s.work = nullptr;
  s.active = false;
  s.dragging = false;
  PostMessageW(s.owner, kMsgCaptureDone, static_cast<WPARAM>(result), 0);
}

// ── 窗口过程 ─────────────────────────────────────────────────────────────────

inline LRESULT CALLBACK CaptureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  State& s = S();
  switch (message) {
    case WM_LBUTTONDOWN: {
      s.dragging = true;
      GetCursorPos(&s.anchor);
      s.anchor.x -= s.vx;
      s.anchor.y -= s.vy;
      s.selection = {s.anchor.x, s.anchor.y, s.anchor.x, s.anchor.y};
      SetCapture(window);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!s.dragging) return 0;
      POINT now = {};
      GetCursorPos(&now);
      now.x -= s.vx;
      now.y -= s.vy;
      RECT r = {s.anchor.x < now.x ? s.anchor.x : now.x, s.anchor.y < now.y ? s.anchor.y : now.y,
                s.anchor.x > now.x ? s.anchor.x : now.x, s.anchor.y > now.y ? s.anchor.y : now.y};
      // 夹到屏幕里，拖出边界也不产生负数或越界
      if (r.left < 0) r.left = 0;
      if (r.top < 0) r.top = 0;
      if (r.right > s.vw) r.right = s.vw;
      if (r.bottom > s.vh) r.bottom = s.vh;
      s.selection = r;
      Paint();
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!s.dragging) return 0;
      ReleaseCapture();
      Finish();
      return 0;
    }
    case WM_RBUTTONDOWN:
      s.dragging = false;
      Teardown(0);
      return 0;
    case WM_HOTKEY:
      if (wparam == 0x71) {  // Esc
        s.dragging = false;
        Teardown(0);
      }
      return 0;
    case WM_DESTROY:
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

/**
 * 开始一次框选。返回 false 表示没起来（拍屏失败 / 建窗失败），调用方不应改状态。
 * 结果异步：结束时会向 owner 发 kMsgCaptureDone，wparam=1 成功、0 取消。
 */
inline bool Begin(HWND owner, std::wstring* out_path, int* out_w, int* out_h) {
  State& s = S();
  if (s.active) return false;

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  s.owner = owner;
  s.out_path = out_path;
  s.out_w = out_w;
  s.out_h = out_h;
  s.dragging = false;
  s.selection = {};
  s.last_paint = 0;

  // 虚拟屏幕 = 所有显示器合起来的矩形。多屏时左上角可能是负数。
  s.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  s.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  s.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  s.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (s.vw <= 0 || s.vh <= 0) return false;

  s.screen = GetDC(nullptr);
  if (s.screen == nullptr) return false;

  if (CreateSurface(s.screen, s.vw, s.vh, &s.shot_bitmap, &s.shot) == nullptr) return false;
  if (CreateSurface(s.screen, s.vw, s.vh, &s.dim_bitmap, &s.dim) == nullptr) return false;
  if (CreateSurface(s.screen, s.vw, s.vh, &s.work_bitmap, &s.work) == nullptr) return false;

  // 冻结帧：一次 BitBlt 把整块虚拟桌面拿到手
  if (!BitBlt(s.shot, 0, 0, s.vw, s.vh, s.screen, s.vx, s.vy, SRCCOPY | CAPTUREBLT)) {
    Teardown(0);
    return false;
  }

  // 压暗底图：逐像素乘 kDim，alpha 保持 255（分层窗口要预乘，255 时两者相等）
  const auto* src = static_cast<const BYTE*>(s.shot_bits);
  auto* dst = static_cast<BYTE*>(s.dim_bits);
  const size_t pixels = static_cast<size_t>(s.vw) * static_cast<size_t>(s.vh);
  for (size_t i = 0; i < pixels; ++i) {
    const size_t at = i * 4;
    dst[at + 0] = static_cast<BYTE>(src[at + 0] * kDim);
    dst[at + 1] = static_cast<BYTE>(src[at + 1] * kDim);
    dst[at + 2] = static_cast<BYTE>(src[at + 2] * kDim);
    dst[at + 3] = 255;
  }

  WNDCLASSEXW cls = {};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = CaptureProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
  cls.lpszClassName = kCaptureClass;
  RegisterClassExW(&cls);

  s.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                             kCaptureClass, L"DSH Work · 框选", WS_POPUP, s.vx, s.vy, s.vw, s.vh,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (s.window == nullptr) {
    Teardown(0);
    return false;
  }

  s.active = true;
  Paint();

  ShowWindow(s.window, SW_SHOWNOACTIVATE);
  SetWindowPos(s.window, HWND_TOPMOST, s.vx, s.vy, s.vw, s.vh, SWP_NOACTIVATE);
  // Esc 用热键接：覆盖层不抢焦点，收不到键盘
  RegisterHotKey(s.window, 0x71, MOD_NOREPEAT, VK_ESCAPE);
  return true;
}

inline bool IsActive() { return S().active; }

inline void Cancel() {
  State& s = S();
  if (!s.active) return;
  s.dragging = false;
  Teardown(0);
}

}  // namespace dshwork::capture
