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

#include <cmath>
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
constexpr UINT_PTR kWheelTimerId = 0x81;      // 轮盘张开动画的定时器

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
  int wheel_cx = 0;  // 轮盘圆心（覆盖层坐标系，物理像素）
  int wheel_cy = 0;
  float wheel_t = 0.0f;     // 张开动画进度 0→1
  bool wheel_anim = false;
  POINT anchor = {};
  RECT selection = {};
  ULONGLONG last_paint = 0;

  // 最近一次截取的 PNG：有它，Tab 之后摆出来的就是"这块拿去做什么"那一层
  std::wstring pending_path;
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
// ── 第二级：环形轮盘 ─────────────────────────────────────────────────────────
// 形态照 lolPing：框选松手后光标处张开一个环，鼠标指哪一瓣亮哪一瓣，
// 再点一下（或回车）才执行；指回中心死区就是放弃，Esc 直接退。
// 全程像素运算——覆盖层只有 UpdateLayeredWindow，没有 D2D；文字最后用 GDI 补。

inline float ScaleOf() {
  State& s = S();
  return s.window ? static_cast<float>(GetDpiForWindow(s.window)) / 96.0f : 1.0f;
}

// ── 像素工具 ───────────────────────────────────────────────────────────────────
// 这个覆盖层没有 D2D，只有一张 32bpp 的 DIB。所以每个像素都得自己对，
// 好处是不用引任何渲染库，坏处是抗锯齿要自己数——圆环那边用 2x2 采样。

/** 把源色按 a 叠到 (x,y)，结果 alpha 记 255。 */
inline void BlendPixel(int x, int y, int r, int g, int b, float a) {
  State& s = S();
  if (s.vw <= 0 || s.vh <= 0) return;
  if (x < 0 || y < 0 || x >= s.vw || y >= s.vh) return;
  if (a <= 0.0f) return;
  if (a > 1.0f) a = 1.0f;
  const long stride = static_cast<long>(s.vw) * 4;
  BYTE* p = static_cast<BYTE*>(s.work_bits) + static_cast<size_t>(y) * static_cast<size_t>(stride) +
            static_cast<size_t>(x) * 4;
  const float inv = 1.0f - a;
  p[0] = static_cast<BYTE>(p[0] * inv + b * a);
  p[1] = static_cast<BYTE>(p[1] * inv + g * a);
  p[2] = static_cast<BYTE>(p[2] * inv + r * a);
  p[3] = 255;
}

/**
 * GDI 往 DIB 上写字时会把 alpha 通道一起写坏（全 0 → 字变成透明洞）。
 * 所以每块文字画完，把它占的矩形补回不透明。
 */
inline void OpaqueRect(const RECT& r) {
  State& s = S();
  if (s.work_bits == nullptr) return;
  const long stride = static_cast<long>(s.vw) * 4;
  const int x0 = r.left < 0 ? 0 : r.left;
  const int x1 = r.right > s.vw ? s.vw : r.right;
  const int y0 = r.top < 0 ? 0 : r.top;
  const int y1 = r.bottom > s.vh ? s.vh : r.bottom;
  for (int y = y0; y < y1; ++y) {
    BYTE* row = static_cast<BYTE*>(s.work_bits) + static_cast<size_t>(y) * static_cast<size_t>(stride);
    for (int x = x0; x < x1; ++x) row[x * 4 + 3] = 255;
  }
}
// ── 轮盘条目 ───────────────────────────────────────────────────────────────────
// Tab 先进框选，框好了才摆这一圈——它问的是同一件事："这块拿去做什么"。
// 顺序照最初构思：解构 / 继续框选 / 自动后续 / 直接收下。
enum MenuKind { kMenuCancel = 0, kMenuCrop = 1, kMenuDeconstruct = 2, kMenuAccept = 3, kMenuAuto = 6 };

inline int MenuItems() { return 4; }

inline const wchar_t* MenuLabel(int index) {
  static const wchar_t* items[] = {L"解构这块", L"继续框选", L"自动后续", L"直接收下"};
  if (index < 0 || index >= 4) return L"";
  return items[index];
}

inline int MenuAction(int index) {
  static const int actions[] = {kMenuDeconstruct, kMenuCrop, kMenuAuto, kMenuAccept};
  if (index < 0 || index >= 4) return kMenuCancel;
  return actions[index];
}
inline float SegDist(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay;
  const float wx = px - ax, wy = py - ay;
  const float len2 = vx * vx + vy * vy;
  float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const float dx = px - (ax + vx * t), dy = py - (ay + vy * t);
  return std::sqrt(dx * dx + dy * dy);
}

/** 圆头线段：轮盘上的图标全用这一种笔触拼，和圆环共用同一套 2x2 采样，所以一样干净。 */
inline void StampCapsule(float ax, float ay, float bx, float by, float radius, BYTE rr, BYTE gg, BYTE bb) {
  const float lo_x = (ax < bx ? ax : bx) - radius - 1.0f;
  const float hi_x = (ax > bx ? ax : bx) + radius + 1.0f;
  const float lo_y = (ay < by ? ay : by) - radius - 1.0f;
  const float hi_y = (ay > by ? ay : by) + radius + 1.0f;
  for (int y = static_cast<int>(lo_y); y <= static_cast<int>(hi_y); ++y) {
    for (int x = static_cast<int>(lo_x); x <= static_cast<int>(hi_x); ++x) {
      int covered = 0;
      for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
          const float px = static_cast<float>(x) + (sx + 0.5f) * 0.5f;
          const float py = static_cast<float>(y) + (sy + 0.5f) * 0.5f;
          if (SegDist(px, py, ax, ay, bx, by) <= radius) ++covered;
        }
      }
      if (covered > 0) BlendPixel(x, y, rr, gg, bb, static_cast<float>(covered) * 0.25f);
    }
  }
}

/** 四个图标都是用圆头线段拼的：好写、抗锯齿一致、不依赖任何字体或图片资源。 */
inline void StampIcon(int kind, float cx, float cy, float size, BYTE rr, BYTE gg, BYTE bb) {
  const float u = size * 0.5f;
  const float th = u * 0.155f;
  switch (kind) {
    case 0: {  // 框选：四个直角括号
      const float o = u * 0.74f;
      const float arm = u * 0.58f;
      StampCapsule(cx - o, cy - o, cx - o + arm, cy - o, th, rr, gg, bb);
      StampCapsule(cx - o, cy - o, cx - o, cy - o + arm, th, rr, gg, bb);
      StampCapsule(cx + o, cy - o, cx + o - arm, cy - o, th, rr, gg, bb);
      StampCapsule(cx + o, cy - o, cx + o, cy - o + arm, th, rr, gg, bb);
      StampCapsule(cx - o, cy + o, cx - o + arm, cy + o, th, rr, gg, bb);
      StampCapsule(cx - o, cy + o, cx - o, cy + o - arm, th, rr, gg, bb);
      StampCapsule(cx + o, cy + o, cx + o - arm, cy + o, th, rr, gg, bb);
      StampCapsule(cx + o, cy + o, cx + o, cy + o - arm, th, rr, gg, bb);
      break;
    }
    case 1: {  // 面板：一条长的 + 一条短的，像侧栏里那两行
      StampCapsule(cx - u * 0.78f, cy - u * 0.34f, cx + u * 0.16f, cy - u * 0.34f, th * 1.5f, rr, gg, bb);
      StampCapsule(cx - u * 0.78f, cy + u * 0.34f, cx + u * 0.66f, cy + u * 0.34f, th * 1.5f, rr, gg, bb);
      break;
    }
    case 2: {  // 解构：四射的星芒，中心实心
      const float r = u * 0.86f;
      StampCapsule(cx, cy - r, cx, cy + r, th * 0.9f, rr, gg, bb);
      StampCapsule(cx - r, cy, cx + r, cy, th * 0.9f, rr, gg, bb);
      StampCapsule(cx - r * 0.62f, cy - r * 0.62f, cx + r * 0.62f, cy + r * 0.62f, th * 0.55f, rr, gg, bb);
      StampCapsule(cx - r * 0.62f, cy + r * 0.62f, cx + r * 0.62f, cy - r * 0.62f, th * 0.55f, rr, gg, bb);
      break;
    }
    case 3: {  // 文件夹：扁圆角块当身子，左上压一个小把手
      StampCapsule(cx - u * 0.52f, cy - u * 0.44f, cx - u * 0.02f, cy - u * 0.44f, u * 0.15f, rr, gg, bb);
      StampCapsule(cx - u * 0.78f, cy + u * 0.12f, cx + u * 0.78f, cy + u * 0.12f, u * 0.30f, rr, gg, bb);
      break;
    }
    default: {  // 叉
      const float r = u * 0.60f;
      StampCapsule(cx - r, cy - r, cx + r, cy + r, th * 1.2f, rr, gg, bb);
      StampCapsule(cx - r, cy + r, cx + r, cy - r, th * 1.2f, rr, gg, bb);
      break;
    }
  }
}

/** 相对圆心的角度落在哪一瓣；-1 = 中心死区或环外。0 号瓣在正上方，顺时针均分。 */
inline int PickSector(int rel_x, int rel_y) {
  const float sc = ScaleOf();
  const float inner = 44.0f * sc;
  const float outer = 104.0f * sc;
  const float dist2 = static_cast<float>(rel_x) * rel_x + static_cast<float>(rel_y) * rel_y;
  if (dist2 < inner * inner || dist2 > outer * outer) return -1;
  float deg = static_cast<float>(
      std::atan2(static_cast<double>(rel_y), static_cast<double>(rel_x)) * 180.0 / 3.14159265358979);
  deg += 90.0f;  // 从正上方量
  while (deg < 0.0f) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  const int n = MenuItems();
  const float step = 360.0f / static_cast<float>(n);
  int index = static_cast<int>((deg + step * 0.5f) / step);
  if (index >= n) index -= n;
  if (index < 0) index = 0;
  return index;
}

/** 轮盘永远开在当前鼠标位置——想点哪一瓣，手不用先跑过去。 */
inline void PrepareWheelAt(int px, int py) {
  State& s = S();
  s.stage = 1;
  s.wheel_cx = px;
  s.wheel_cy = py;
  s.menu_index = -1;
  s.wheel_t = 0;
  s.wheel_anim = true;
  if (s.window != nullptr) SetTimer(s.window, kWheelTimerId, 16, nullptr);
}
inline void DrawWheel() {
  State& s = S();
  if (s.stage != 1) return;

  const float sc = ScaleOf();
  const float t = s.wheel_anim ? s.wheel_t : 1.0f;
  const float grow = 0.86f + 0.14f * (1.0f - (1.0f - t) * (1.0f - t));
  const float pi = 3.14159265358979f;

  const int n = MenuItems();
  const float step = 360.0f / static_cast<float>(n);
  const float inner = 44.0f * sc * grow;
  const float outer = 104.0f * sc * grow;
  const float cx = static_cast<float>(s.wheel_cx);
  const float cy = static_cast<float>(s.wheel_cy);

  const int x0 = static_cast<int>(cx - outer) - 2;
  const int x1 = static_cast<int>(cx + outer) + 2;
  const int y0 = static_cast<int>(cy - outer) - 2;
  const int y1 = static_cast<int>(cy + outer) + 2;

  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      float a_disc = 0, a_hot = 0, a_sep = 0, a_ring_o = 0, a_ring_i = 0, a_arc = 0, a_hub = 0;
      int total = 0;
      for (int sy = 0; sy < 2; ++sy) {
        for (int sx = 0; sx < 2; ++sx) {
          const float dx = static_cast<float>(x) + (sx + 0.5f) * 0.5f - cx;
          const float dy = static_cast<float>(y) + (sy + 0.5f) * 0.5f - cy;
          const float d = std::sqrt(dx * dx + dy * dy);
          ++total;
          if (d > outer) continue;

          if (d < inner) {
            a_hub += 1.0f;
            if (std::fabs(d - inner) < 0.60f * sc) a_ring_i += 1.0f;
            continue;
          }

          a_disc += 1.0f;
          float deg = static_cast<float>(std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * 180.0 / pi);
          deg += 90.0f;
          while (deg < 0.0f) deg += 360.0f;
          while (deg >= 360.0f) deg -= 360.0f;
          int sec = static_cast<int>((deg + step * 0.5f) / step);
          if (sec >= n) sec -= n;
          if (sec < 0) sec = 0;
          if (s.menu_index >= 0 && sec == s.menu_index) a_hot += 1.0f;

          float rel = std::fmod(deg + step * 0.5f, step);
          if (rel < 0.0f) rel += step;
          const float da = rel < step * 0.5f ? rel : step - rel;
          if (da * pi / 180.0f * d < 0.55f * sc) a_sep += 1.0f;

          if (std::fabs(d - outer) < 0.60f * sc) a_ring_o += 1.0f;
          if (s.menu_index >= 0 && sec == s.menu_index && d > outer - 2.2f * sc) a_arc += 1.0f;
        }
      }
      if (total == 0) continue;
      const float inv = 1.0f / static_cast<float>(total);

      if (a_disc > 0.0f) {
        const float g = (static_cast<float>(y) - (cy - outer)) / (2.0f * outer);
        const float k = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
        BlendPixel(x, y, static_cast<int>(27.0f - 9.0f * k), static_cast<int>(29.0f - 9.0f * k),
                   static_cast<int>(37.0f - 11.0f * k), a_disc * inv * 0.97f);
      }
      if (a_hot > 0.0f) BlendPixel(x, y, 10, 132, 255, a_hot * inv * 0.22f);
      if (a_sep > 0.0f) BlendPixel(x, y, 255, 255, 255, a_sep * inv * 0.075f);
      if (a_ring_o + a_ring_i > 0.0f) {
        BlendPixel(x, y, 255, 255, 255, (a_ring_o + a_ring_i) * inv * 0.09f);
      }
      if (a_arc > 0.0f) BlendPixel(x, y, 10, 132, 255, a_arc * inv * 0.95f);
      if (a_hub > 0.0f) BlendPixel(x, y, 14, 16, 21, a_hub * inv * 0.98f);
    }
  }

  HDC dc = s.work;
  SetBkMode(dc, TRANSPARENT);
  HFONT font = CreateFontW(-static_cast<int>(13.0f * sc), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");
  HFONT font_hot = CreateFontW(-static_cast<int>(14.0f * sc), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HGDIOBJ old_font = SelectObject(dc, font);

  // 名字直接写在每一瓣的中间半径上，横排——四个词而已，转着读反而慢
  const float label_r = (inner + outer) * 0.5f;
  for (int i = 0; i < n; ++i) {
    const float mid = -90.0f + static_cast<float>(i) * step;
    const float rad = mid * pi / 180.0f;
    const int lx = static_cast<int>(cx + label_r * std::cos(rad));
    const int ly = static_cast<int>(cy + label_r * std::sin(rad));
    const int half_w = static_cast<int>(48.0f * sc);
    RECT r = {lx - half_w, ly - static_cast<int>(11.0f * sc), lx + half_w, ly + static_cast<int>(11.0f * sc)};
    const bool is_hot = (i == s.menu_index);
    SelectObject(dc, is_hot ? font_hot : font);
    SetTextColor(dc, is_hot ? RGB(255, 255, 255) : RGB(176, 182, 196));
    DrawTextW(dc, MenuLabel(i), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    OpaqueRect(r);
  }

  SelectObject(dc, font_hot);
  SetTextColor(dc, s.menu_index >= 0 ? RGB(255, 255, 255) : RGB(158, 164, 178));
  RECT hub_r = {static_cast<int>(cx - inner * 0.92f), static_cast<int>(cy - 13.0f * sc),
                static_cast<int>(cx + inner * 0.92f), static_cast<int>(cy + 13.0f * sc)};
  const wchar_t* hub_text = s.menu_index >= 0 ? MenuLabel(s.menu_index) : L"取消";
  DrawTextW(dc, hub_text, -1, &hub_r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  OpaqueRect(hub_r);

  SelectObject(dc, old_font);
  DeleteObject(font);
  DeleteObject(font_hot);
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

  if (s.stage == 1) DrawWheel();
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
  const BOOL ok = UpdateLayeredWindow(s.window, nullptr, &dest, &size, s.work, &source, 0, &blend, ULW_ALPHA);
  CapLog("paint stage=" + std::to_string(s.stage) + " ok=" + std::to_string(static_cast<int>(ok)) +
         " t=" + std::to_string(s.wheel_t));
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

  // 只把选框那一块抠出来另存；整张桌面不落盘
  void* crop_bits = nullptr;
  bool saved = false;
  BITMAPINFO info = MakeBitmapInfo(w, h);
  HBITMAP crop_bitmap = CreateDIBSection(s.screen, &info, DIB_RGB_COLORS, &crop_bits, nullptr, 0);
  if (crop_bitmap != nullptr && crop_bits != nullptr) {
    const size_t row_bytes = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y) {
      const auto* src = static_cast<const BYTE*>(s.shot_bits) +
                        (static_cast<size_t>(s.selection.top + y) * static_cast<size_t>(s.vw) +
                         s.selection.left) * 4;
      auto* dst = static_cast<BYTE*>(crop_bits) + static_cast<size_t>(y) * row_bytes;
      memcpy(dst, src, row_bytes);
    }
    saved = SavePng(path, crop_bits, w, h);
  }
  if (crop_bitmap != nullptr) DeleteObject(crop_bitmap);

  if (!saved) {
    Teardown(0);
    return;
  }

  s.pending_path = path;
  if (s.out_path != nullptr) *s.out_path = path;
  if (s.out_w != nullptr) *s.out_w = w;
  if (s.out_h != nullptr) *s.out_h = h;

  // 图已经落到磁盘了，别急着收摊：就地把它"能干什么"摆出来
  POINT p = {};
  GetCursorPos(&p);
  CapLog("showwheel cursor=" + std::to_string(p.x) + "," + std::to_string(p.y) + " n=" + std::to_string(MenuItems()));
  PrepareWheelAt(p.x - s.vx, p.y - s.vy);
  Paint();
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
/** 菜单选中某项之后要做的事。 */
inline void ActivateMenu(int index) {
  State& s = S();
  switch (MenuAction(index)) {
    case kMenuCrop:  // 回去重框：只清选区，图还在
      s.stage = 0;
      s.dragging = false;
      s.selection = {0, 0, 0, 0};
      s.wheel_anim = false;
      Paint();
      break;
    case kMenuDeconstruct:  // 交给宿主拆任务树（宿主那侧还没接上）
      s.pending_action = 2;
      Teardown(1);
      break;
    case kMenuAuto:  // 自动后续：同样是宿主的活，先占位
      s.pending_action = 6;
      Teardown(1);
      break;
    case kMenuAccept:  // 直接收下：只留文件
      s.pending_action = 3;
      Teardown(1);
      break;
    default:
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

      // 轮盘开着：点在某一瓣上就执行；点回中心＝放弃
      if (s.stage == 1) {
        if (s.menu_index >= 0) {
          ActivateMenu(s.menu_index);
        } else {
          s.dragging = false;
          Teardown(0);
        }
        return 0;
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
        const int index = PickSector(now.x - s.wheel_cx, now.y - s.wheel_cy);
        if (index != s.menu_index) {
          s.menu_index = index;
          Paint();
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
      // 框好了：先落盘，再把"这块拿去做什么"摆在当前指针上
      Finish();
      return 0;
    }
    case WM_RBUTTONDOWN:
      s.dragging = false;
      Teardown(0);
      return 0;
    case WM_HOTKEY:
      if (wparam == 0x71) {  // Esc：随时收摊
        s.dragging = false;
        Teardown(0);
      } else if (s.stage == 1) {
        // 菜单开着：Enter / 空格 / Tab 都是"就它了"
        if ((wparam == 0x73 || wparam == 0x74 || wparam == 0x72) && s.menu_index >= 0) {
          ActivateMenu(s.menu_index);
        }
      } else {
        // 还在框选：Tab / Enter / 空格 = "框好了，下一步"；没框就当作取消
        const bool ready = (s.selection.right - s.selection.left) >= kMinSide &&
                           (s.selection.bottom - s.selection.top) >= kMinSide;
        if (ready) {
          Finish();
        } else if (wparam == 0x72) {
          Teardown(0);
        }
      }
      return 0;    case WM_TIMER:
      if (wparam == kWheelTimerId) {
        // 张开动画：走完就停表，别一直空转
        s.wheel_t += 0.09f;
        if (s.wheel_t >= 1.0f) {
          s.wheel_t = 1.0f;
          s.wheel_anim = false;
          KillTimer(window, kWheelTimerId);
        }
        Paint();
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

/** 轮盘是不是正开着（鼠标事件该按"选瓣"处理，而不是开始拖框）。 */
inline bool WheelOpen() { return S().stage == 1; }

/** 在当前鼠标位置摆出轮盘：没有截图就是一级（框选/面板/取消），有截图就是二级。 */
inline void ShowWheelAtCursor() {
  State& s = S();
  if (!s.active) return;
  POINT p = {};
  GetCursorPos(&p);
  CapLog("showwheel cursor=" + std::to_string(p.x) + "," + std::to_string(p.y) + " n=" + std::to_string(MenuItems()));
  PrepareWheelAt(p.x - s.vx, p.y - s.vy);
  Paint();
}

inline void Cancel() {
  State& s = S();
  if (!s.active) return;
  s.dragging = false;
  Teardown(0);
}

}  // namespace dshwork::capture