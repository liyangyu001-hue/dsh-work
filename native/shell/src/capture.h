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
#include "grab_dxgi.h"
#include <objidl.h>
#include <wincodec.h>

#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace dshwork::capture {

/**
 * 诊断日志出口。
 *
 * 为什么不在这个头里直接写文件：capture 要能在任何宿主里跑（包括单测），
 * 不该自己决定日志去哪。宿主启动时 SetLog 一下即可。
 * 切片三被杀软掐掉的那几次，就是靠这几行日志定位到"死在 BitBlt 之后"的。
 */
using LogFn = void (*)(const std::string&);
inline LogFn g_log = nullptr;
inline void SetLog(LogFn fn) { g_log = fn; }
inline void CapLog(const std::string& text) {
  if (g_log != nullptr) g_log(text);
}

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
  // 框选的第二级：松开鼠标后不立刻收摊，先把"这块要拿去做什么"摆出来。
  // 0 = 正在拖框，1 = 选中了、菜单在等你选。
  int stage = 0;
  int menu_index = 2;
  int pending_action = 2;  // 1=解构 2=直接保存，随结果一起回给宿主
  RECT menu = {};  // 菜单在虚拟屏幕里的位置（物理像素）
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
// ── 第二级：选中之后的动作菜单 ────────────────────────────────────────────────
//
// 用户对 Tab 的期望是"按下去会有菜单"，不是"按下去直接截图"：
// 框完一块，得先问这块要拿去做什么。菜单是浮在冻结帧上的，不另开窗口——
// 另开窗口会把底下的选区抢走焦点，也会让"再按一次 Tab"失效。
//
// 为什么这一块用 GDI 画：整条覆盖层是纯像素操作（UpdateLayeredWindow），
// 没有 D2D/DirectWrite 可用；而菜单是唯一需要文字的地方，量小。
// GDI 写字不写 alpha 通道，所以菜单底做成**不透明**的一块，字就正常了。

inline int MenuItems() { return 4; }

inline const wchar_t* MenuLabel(int index, const State& s) {
  switch (index) {
    case 0:
      return L"重新框选";
    case 1:
      return L"解构这块";
    case 2:
      return L"直接保存";
    default:
      return L"取消";
  }
}

/** 菜单放在选区右下角，放不下就翻到左边/上边——永远不越出屏幕。 */
inline void LayoutMenu() {
  State& s = S();
  const float scale = s.window ? static_cast<float>(GetDpiForWindow(s.window)) / 96.0f : 1.0f;
  const int w = static_cast<int>(188.0f * scale);
  const int row = static_cast<int>(36.0f * scale);
  const int h = row * MenuItems() + static_cast<int>(12.0f * scale);

  int left = s.selection.right + static_cast<int>(8.0f * scale);
  int top = s.selection.bottom + static_cast<int>(8.0f * scale);
  if (left + w > s.vw) left = s.selection.left - w - static_cast<int>(8.0f * scale);
  if (left < 0) left = 0;
  if (top + h > s.vh) top = s.selection.top - h - static_cast<int>(8.0f * scale);
  if (top < 0) top = 0;
  s.menu = {left, top, left + w, top + h};
}

inline void FillRectAlpha(RECT r, BYTE rr, BYTE gg, BYTE bb, BYTE aa, float radius) {
  State& s = S();
  for (int y = r.top; y < r.bottom; ++y) {
    if (y < 0 || y >= s.vh) continue;
    auto* row = static_cast<BYTE*>(s.work_bits) + static_cast<size_t>(y) * static_cast<size_t>(s.vw) * 4;
    for (int x = r.left; x < r.right; ++x) {
      if (x < 0 || x >= s.vw) continue;
      // 圆角：角落那几像素直接留空，才有卡片感
      if (radius > 0.5f) {
        const float fx = static_cast<float>(x - r.left);
        const float fy = static_cast<float>(y - r.top);
        const float fx2 = static_cast<float>(r.right - 1 - x);
        const float fy2 = static_cast<float>(r.bottom - 1 - y);
        const float dx = fx < radius ? radius - fx : (fx2 < radius ? radius - fx2 : radius);
        const float dy = fy < radius ? radius - fy : (fy2 < radius ? radius - fy2 : radius);
        if (dx < radius && dy < radius) {
          const float ddx = radius - dx;
          const float ddy = radius - dy;
          if (ddx * ddx + ddy * ddy > radius * radius) continue;
        }
      }
      BYTE* px = row + static_cast<size_t>(x) * 4;
      px[0] = bb;
      px[1] = gg;
      px[2] = rr;
      px[3] = aa;
    }
  }
}

inline void OpaqueRect(RECT r) {
  State& s = S();
  for (int y = r.top; y < r.bottom; ++y) {
    if (y < 0 || y >= s.vh) continue;
    auto* row = static_cast<BYTE*>(s.work_bits) + static_cast<size_t>(y) * static_cast<size_t>(s.vw) * 4;
    for (int x = r.left; x < r.right; ++x) {
      if (x < 0 || x >= s.vw) continue;
      row[x * 4 + 3] = 255;
    }
  }
}

inline void DrawMenu() {
  State& s = S();
  if (s.stage != 1 || s.menu.right <= s.menu.left) return;
  const float scale = s.window ? static_cast<float>(GetDpiForWindow(s.window)) / 96.0f : 1.0f;
  const int row_h = static_cast<int>(36.0f * scale);
  const float radius = 12.0f * scale;

  // 底：不透明的深色卡（GDI 写字不写 alpha，底必须实心）
  // 直角：圆角那几个像素会留空, 会露出后面的桌面，像四个亮角
  FillRectAlpha(s.menu, 26, 26, 30, 255, 0.0f);

  // 当前项：一条强调色高亮
  if (s.menu_index >= 0 && s.menu_index < MenuItems()) {
    RECT hl = {s.menu.left + 6, s.menu.top + 6 + s.menu_index * row_h, s.menu.right - 6,
               s.menu.top + 6 + (s.menu_index + 1) * row_h};
    FillRectAlpha(hl, 38, 78, 128, 255, radius * 0.6f);
  }

  // 字：直接写进 work 那张位图已经挂着的 DC。
  // 位图同时只能被一个 DC 选中，再 CreateCompatibleDC 去选一次会失败，
  // 结果就是菜单出来了但没有字。
  HDC dc = s.work;
  SetBkMode(dc, TRANSPARENT);
  HFONT font = CreateFontW(-static_cast<int>(14.0f * scale), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HGDIOBJ old_font = SelectObject(dc, font);
  SetTextColor(dc, RGB(245, 245, 247));
  for (int i = 0; i < MenuItems(); ++i) {
    RECT r = {s.menu.left + static_cast<int>(18.0f * scale), s.menu.top + 6 + i * row_h,
              s.menu.right - static_cast<int>(12.0f * scale), s.menu.top + 6 + (i + 1) * row_h};
    DrawTextW(dc, MenuLabel(i, s), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  SelectObject(dc, old_font);
  DeleteObject(font);

  // GDI 往 32bpp DIB 上写字不会碰 alpha 字节，结果就是字写了但透明。
  // 菜单底本来就不透明，画完统一把这一块的 alpha 拉满。
  OpaqueRect(s.menu);
}

inline void Compose() {
  State& s = S();
  const size_t bytes = static_cast<size_t>(s.vw) * static_cast<size_t>(s.vh) * 4;
  CopyPixels(s.work_bits, s.dim_bits, bytes);

  if (s.dragging || s.stage == 1) {
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

  if (s.stage == 1) DrawMenu();
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
  const bool selection_ok = (s.selection.right - s.selection.left) >= kMinSide &&
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
  UnregisterHotKey(s.window, 0x72);
  UnregisterHotKey(s.window, 0x73);
  UnregisterHotKey(s.window, 0x74);
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
  PostMessageW(s.owner, kMsgCaptureDone, static_cast<WPARAM>(result), static_cast<LPARAM>(s.pending_action));
}

/** 菜单选中某项之后要做的事。 */
inline void ActivateMenu(int index) {
  State& s = S();
  switch (index) {
    case 0:  // 重新框选：留在覆盖层里，下次按下重新拖
      s.stage = 0;
      s.dragging = false;
      s.selection = {0, 0, 0, 0};
      s.menu = {0, 0, 0, 0};
      Paint();
      break;
    case 1:  // 解构这块
      s.pending_action = 1;
      Finish();
      break;
    case 2:  // 直接保存
      s.pending_action = 2;
      Finish();
      break;
    default:  // 取消
      s.dragging = false;
      Teardown(0);
      break;
  }
}

// ── 窗口过程 ─────────────────────────────────────────────────────────────────

inline LRESULT CALLBACK CaptureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  State& s = S();
  switch (message) {
    case WM_LBUTTONDOWN: {
      // 菜单开着的时候，点菜单就是选动作；点别处等于"重来一次"
      if (s.stage == 1) {
        POINT now = {};
        GetCursorPos(&now);
        now.x -= s.vx;
        now.y -= s.vy;
        if (now.x >= s.menu.left && now.x <= s.menu.right && now.y >= s.menu.top && now.y <= s.menu.bottom) {
          const float scale = window ? static_cast<float>(GetDpiForWindow(window)) / 96.0f : 1.0f;
          const int row_h = static_cast<int>(36.0f * scale);
          const int index = (now.y - s.menu.top - 6) / row_h;
          ActivateMenu(index < 0 ? 0 : (index >= MenuItems() ? MenuItems() - 1 : index));
          return 0;
        }
        s.stage = 0;
        s.selection = {0, 0, 0, 0};
        s.menu = {0, 0, 0, 0};
      }
      s.dragging = true;
      GetCursorPos(&s.anchor);
      s.anchor.x -= s.vx;
      s.anchor.y -= s.vy;
      s.selection = {s.anchor.x, s.anchor.y, s.anchor.x, s.anchor.y};
      SetCapture(window);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (s.stage == 1) {
        POINT now = {};
        GetCursorPos(&now);
        now.x -= s.vx;
        now.y -= s.vy;
        if (now.x >= s.menu.left && now.x <= s.menu.right && now.y >= s.menu.top && now.y <= s.menu.bottom) {
          const float scale = window ? static_cast<float>(GetDpiForWindow(window)) / 96.0f : 1.0f;
          const int row_h = static_cast<int>(36.0f * scale);
          int index = (now.y - s.menu.top - 6) / row_h;
          if (index < 0) index = 0;
          if (index >= MenuItems()) index = MenuItems() - 1;
          if (index != s.menu_index) {
            s.menu_index = index;
            Paint();
          }
        }
        return 0;
      }
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
      s.dragging = false;
      const bool ok = (s.selection.right - s.selection.left) >= kMinSide &&
                      (s.selection.bottom - s.selection.top) >= kMinSide;
      if (!ok) {
        // 手滑点了一下：不算选中，回到拖框状态
        s.selection = {0, 0, 0, 0};
        Paint();
        return 0;
      }
      // 选中了就先问"这块拿去做什么"，不直接落盘
      s.stage = 1;
      s.menu_index = 2;
      LayoutMenu();
      Paint();
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
      } else if (s.stage == 1 && wparam == 0x72) {  // Tab：换一项
        s.menu_index = (s.menu_index + 1) % MenuItems();
        Paint();
      } else if (s.stage == 1 && (wparam == 0x73 || wparam == 0x74)) {  // Enter / 空格：执行
        ActivateMenu(s.menu_index);
      }
      return 0;
    case WM_DESTROY:
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

/** 抓屏的方式，可用 DSHWORK_CAPTURE_MODE 切（默认 bitblt）。 */
inline int CaptureMode() {
  static int mode = -1;
  if (mode >= 0) return mode;
  char buf[32] = {};
  mode = 0;  // 0 = dxgi（默认），3 = bitblt
  if (GetEnvironmentVariableA("DSHWORK_CAPTURE_MODE", buf, 32) > 0) {
    if (lstrcmpiA(buf, "plain") == 0) mode = 1;
    else if (lstrcmpiA(buf, "printwindow") == 0) mode = 2;
    else if (lstrcmpiA(buf, "bitblt") == 0) mode = 3;
  }
  return mode;
}

/**
 * 取一整块虚拟桌面。
 *
 * 为什么写成可切换：BitBlt + CAPTUREBLT 是窃屏木马的典型指纹，杀软会当场掐掉
 * 进程（本机卡巴斯基：VHO:Trojan-PSW.Win32.Convagent.gen）。三种取法留一手，
 * 出了事能立刻换一条路验证，而不是改代码重编。
 */
inline bool GrabScreen(HDC src, int vx, int vy, int vw, int vh, HDC dst) {
  switch (CaptureMode()) {
    case 0: {
      // 默认走 DXGI 桌面复制：不碰 GDI，杀软不把它当窃屏木马
      CapLog("capture: grab=dxgi");
      dshwork::grab::SetLog(g_log);
      RECT virtual_rect = {vx, vy, vx + vw, vy + vh};
      dshwork::grab::Shot shot;
      std::string reason;
      if (!dshwork::grab::GrabVirtualScreen(virtual_rect, &shot, &reason)) {
        CapLog("capture: dxgi 失败 → " + reason);
        return false;
      }
      // 搬到 GDI 的位图上，后面绘制逻辑一行都不用改
      BITMAPINFO bmi = {};
      bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth = shot.width;
      bmi.bmiHeader.biHeight = -shot.height;  // 负数 = 自上而下
      bmi.bmiHeader.biPlanes = 1;
      bmi.bmiHeader.biBitCount = 32;
      bmi.bmiHeader.biCompression = BI_RGB;
      CapLog("capture: pixels=" + std::to_string(shot.pixels.size()));
      const int blit = SetDIBitsToDevice(dst, 0, 0, static_cast<DWORD>(shot.width),
                                         static_cast<DWORD>(shot.height), 0, 0, 0,
                                         static_cast<UINT>(shot.height), shot.pixels.data(), &bmi,
                                         DIB_RGB_COLORS);
      CapLog("capture: blit=" + std::to_string(blit));
      return blit != 0;
    }
    case 1:
      CapLog("capture: grab=plain");
      return BitBlt(dst, 0, 0, vw, vh, src, vx, vy, SRCCOPY) != 0;
    case 2:
      CapLog("capture: grab=printwindow");
      return PrintWindow(GetDesktopWindow(), dst, 2 /* PW_RENDERFULLCONTENT */) != 0;
    default:
      CapLog("capture: grab=bitblt+captureblt");
      return BitBlt(dst, 0, 0, vw, vh, src, vx, vy, SRCCOPY | CAPTUREBLT) != 0;
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

  CapLog("capture: begin");
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

  // CreateSurface 返回的是 DIB 的像素指针——必须接住，否则后面的压暗/裁剪
  // 全是空指针解引用（这个坑让切片三的进程每次框选都段错误，还一度被当成杀软拦截）
  s.shot_bits = CreateSurface(s.screen, s.vw, s.vh, &s.shot_bitmap, &s.shot);
  s.dim_bits = CreateSurface(s.screen, s.vw, s.vh, &s.dim_bitmap, &s.dim);
  s.work_bits = CreateSurface(s.screen, s.vw, s.vh, &s.work_bitmap, &s.work);
  if (s.shot_bits == nullptr || s.dim_bits == nullptr || s.work_bits == nullptr) {
    Teardown(0);
    return false;
  }

  // 冻结帧：一次 BitBlt 把整块虚拟桌面拿到手
  CapLog("capture: surfaces ready " + std::to_string(s.vw) + "x" + std::to_string(s.vh));
  if (!GrabScreen(s.screen, s.vx, s.vy, s.vw, s.vh, s.shot)) {
    Teardown(0);
    return false;
  }

  // 抓到的画面是不是全黑？三个取样点够判断了（调试用，正常路径开销可忽略）
  {
    const auto* probe = static_cast<const BYTE*>(s.shot_bits);
    const size_t total = static_cast<size_t>(s.vw) * static_cast<size_t>(s.vh);
    const size_t at = (total / 2) * 4;
    CapLog("capture: probe b=" + std::to_string(probe[at]) + " g=" + std::to_string(probe[at + 1]) +
           " r=" + std::to_string(probe[at + 2]) + " a=" + std::to_string(probe[at + 3]));
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

  CapLog("capture: snapshot ok");
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
  // Esc 用热键接：覆盖层不抢焦点，收不到键盘。
  // Tab/Enter/空格 同理——菜单里要用它们，一样得走热键。
  RegisterHotKey(s.window, 0x71, MOD_NOREPEAT, VK_ESCAPE);
  RegisterHotKey(s.window, 0x72, MOD_NOREPEAT, VK_TAB);
  RegisterHotKey(s.window, 0x73, MOD_NOREPEAT, VK_RETURN);
  RegisterHotKey(s.window, 0x74, MOD_NOREPEAT, VK_SPACE);
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
