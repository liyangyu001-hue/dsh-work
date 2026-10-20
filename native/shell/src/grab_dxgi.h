// DSH Work · 抓屏（DXGI 桌面复制）
//
// 为什么不用 GDI 的 BitBlt/GetDC：
//   那是窃屏木马的经典指纹。本机卡巴斯基在 BitBlt 整块虚拟桌面的那一刻
//   直接掐掉进程（VHO:Trojan-PSW.Win32.Convagent.gen），把 exe 加进"排除项"
//   也不管用——排除项只跳过扫描，行为拦截照旧。
//
//   IDXGIOutputDuplication 是 OBS / 录屏软件走的正路：不碰 GDI，拿的是
//   DWM 合成后的桌面纹理。杀毒对这条路的容忍度高得多，而且它本来就是
//   "想要屏幕内容"的官方答案。
//
// 代价与约束：
//   - 同一个输出（显示器）同一时间只能有一个 Duplication，被占用时返回失败；
//   - 只能抓"这个输出"，多显示器要逐个抓；
//   - 必须是 D3D11 设备（WARP 也支持，跑在没独显的机器上）。

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <string>
#include <vector>

namespace dshwork::grab {

/** 诊断日志出口，和 capture.h 里那套一致（由宿主 SetLog 注入）。 */
using LogFn = void (*)(const std::string&);
inline LogFn g_log = nullptr;
inline void SetLog(LogFn fn) { g_log = fn; }
inline void GLog(const std::string& text) {
  if (g_log != nullptr) g_log(text);
}

/** 抓到的整块虚拟桌面：BGRA、自上而下、行宽 = width * 4。 */
struct Shot {
  int width = 0;
  int height = 0;
  std::vector<BYTE> pixels;
};

struct ComPtr {
  template <typename T>
  static void Release(T*& p) {
    if (p != nullptr) {
      p->Release();
      p = nullptr;
    }
  }
};

/**
 * 抓整个虚拟桌面（所有显示器拼起来）。
 *
 * virtual_rect 是 GetSystemMetrics(SM_XVIRTUALSCREEN..) 那一组值，允许负数原点。
 * 成功返回 true 并填满 out；失败返回 false 并把原因写进 reason（可读中文）。
 */
inline bool GrabVirtualScreen(const RECT& virtual_rect, Shot* out, std::string* reason) {
  if (out == nullptr) return false;
  const int vw = virtual_rect.right - virtual_rect.left;
  const int vh = virtual_rect.bottom - virtual_rect.top;
  if (vw <= 0 || vh <= 0) {
    if (reason != nullptr) *reason = "虚拟桌面尺寸非法";
    return false;
  }

  out->width = vw;
  out->height = vh;
  out->pixels.assign(static_cast<size_t>(vw) * static_cast<size_t>(vh) * 4, 0xFF);

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level = {};
  const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted, 4, D3D11_SDK_VERSION,
                                 &device, &level, &context);
  if (FAILED(hr)) {
    // 无独显 / 远程桌面回退到 WARP，慢但能出图
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           wanted, 4, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
      if (reason != nullptr) *reason = "创建 D3D11 设备失败";
      return false;
    }
    GLog("grab: 用 WARP 软件渲染");
  }

  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  bool ok = true;
  std::string fail_reason = "没有可用的显示器输出";
  int captured = 0;

  if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device)))) {
    ok = false;
    fail_reason = "拿不到 DXGI 设备";
  } else if (FAILED(dxgi_device->GetAdapter(&adapter))) {
    ok = false;
    fail_reason = "拿不到 DXGI 适配器";
  }

  for (UINT i = 0; ok; ++i) {
    IDXGIOutput* output = nullptr;
    if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
    if (output == nullptr) break;

    DXGI_OUTPUT_DESC desc = {};
    output->GetDesc(&desc);

    // 这个输出在虚拟桌面里的位置；完全在虚拟桌面之外的直接跳过
    const int left = desc.DesktopCoordinates.left - virtual_rect.left;
    const int top = desc.DesktopCoordinates.top - virtual_rect.top;
    const int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    const int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    if (w <= 0 || h <= 0 || left >= vw || top >= vh || left + w <= 0 || top + h <= 0) {
      ComPtr::Release(output);
      continue;
    }

    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* frame_texture = nullptr;
    ID3D11Texture2D* staging = nullptr;

    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1))) ||
        output1 == nullptr) {
      fail_reason = "显示器不支持桌面复制（需要 Win8+）";
      ComPtr::Release(output);
      continue;
    }

    if (FAILED(output1->DuplicateOutput(device, &duplication)) || duplication == nullptr) {
      fail_reason = "桌面复制被占用或不被支持";
      ComPtr::Release(output1);
      ComPtr::Release(output);
      continue;
    }

    // 拿一帧"真画面"。
    //
    // 坑：DuplicateOutput 之后的第一帧经常是全黑的占位帧（桌面在那一刻没有新内容），
    // 直接拿来用就是整块黑屏。所以每拿到一帧都取样看一眼，全黑就丢掉再等，
    // 直到等到有内容的帧或者次数用完。藏起小条本身就是一次桌面变化，
    // 通常第二、三次尝试就能拿到真画面。
    DXGI_OUTDUPL_FRAME_INFO frame_info = {};
    IDXGIResource* resource = nullptr;
    bool got = false;
    for (int attempt = 0; attempt < 8 && !got; ++attempt) {
      resource = nullptr;
      hr = duplication->AcquireNextFrame(attempt == 0 ? 250 : 110, &frame_info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;  // 还没有新帧，再等
      if (FAILED(hr) || resource == nullptr) break;

      // 换一张 CPU 可读的纹理，把这一帧烤下来
      ID3D11Texture2D* probe_texture = nullptr;
      bool usable = false;
      if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                             reinterpret_cast<void**>(&probe_texture))) &&
          probe_texture != nullptr) {
        D3D11_TEXTURE2D_DESC desc2 = {};
        probe_texture->GetDesc(&desc2);
        desc2.Usage = D3D11_USAGE_STAGING;
        desc2.BindFlags = 0;
        desc2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc2.MiscFlags = 0;
        desc2.ArraySize = 1;
        desc2.MipLevels = 1;
        desc2.SampleDesc.Count = 1;
        desc2.SampleDesc.Quality = 0;

        ID3D11Texture2D* staging2 = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&desc2, nullptr, &staging2)) && staging2 != nullptr) {
          context->CopyResource(staging2, probe_texture);
          D3D11_MAPPED_SUBRESOURCE mapped = {};
          if (SUCCEEDED(context->Map(staging2, 0, D3D11_MAP_READ, 0, &mapped))) {
            const auto* base = static_cast<const BYTE*>(mapped.pData);
            const size_t mid_row = static_cast<size_t>(desc2.Height / 2) * mapped.RowPitch;
            const size_t mid_x = static_cast<size_t>(desc2.Width / 2) * 4u;
            const BYTE b = base[mid_row + mid_x];
            const BYTE g2 = base[mid_row + mid_x + 1];
            const BYTE r2 = base[mid_row + mid_x + 2];
            usable = (b != 0 || g2 != 0 || r2 != 0);

            if (usable) {
              const int copy_w = (w < vw - left) ? w : (vw - left);
              const int copy_h = (h < vh - top) ? h : (vh - top);
              for (int y = 0; y < copy_h; ++y) {
                const BYTE* src_row = base + static_cast<size_t>(y) * mapped.RowPitch;
                BYTE* dst_row = out->pixels.data() +
                                (static_cast<size_t>(top + y) * static_cast<size_t>(vw) +
                                 static_cast<size_t>(left)) * 4u;
                memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * 4u);
              }
            }
            context->Unmap(staging2, 0);
          }
          ComPtr::Release(staging2);
        }
        ComPtr::Release(probe_texture);
      }

      duplication->ReleaseFrame();
      ComPtr::Release(resource);
      if (usable) {
        got = true;
        ++captured;
      } else {
        GLog("grab: 第 " + std::to_string(attempt + 1) + " 帧是全黑的，丢掉重等");
        Sleep(40);
      }
    }

    if (false && frame_texture != nullptr) {
      D3D11_TEXTURE2D_DESC desc2 = {};
      frame_texture->GetDesc(&desc2);
      desc2.Usage = D3D11_USAGE_STAGING;
      desc2.BindFlags = 0;
      desc2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc2.MiscFlags = 0;
      desc2.ArraySize = 1;
      desc2.MipLevels = 1;
      desc2.SampleDesc.Count = 1;
      desc2.SampleDesc.Quality = 0;

      if (SUCCEEDED(device->CreateTexture2D(&desc2, nullptr, &staging)) && staging != nullptr) {
        context->CopyResource(staging, frame_texture);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
          // 逐行搬进目标缓冲：源行宽可能大于 w*4（对齐），不能整块 memcpy
          const int copy_w = (w < vw - left) ? w : (vw - left);
          const int copy_h = (h < vh - top) ? h : (vh - top);
          for (int y = 0; y < copy_h; ++y) {
            const BYTE* src_row = static_cast<const BYTE*>(mapped.pData) +
                                  static_cast<size_t>(y) * mapped.RowPitch;
            BYTE* dst_row = out->pixels.data() +
                            (static_cast<size_t>(top + y) * static_cast<size_t>(vw) +
                             static_cast<size_t>(left)) * 4u;
            memcpy(dst_row, src_row, static_cast<size_t>(copy_w) * 4u);
          }
          ++captured;
          context->Unmap(staging, 0);
        }
      }
    }

    if (got) duplication->ReleaseFrame();
    ComPtr::Release(resource);
    ComPtr::Release(frame_texture);
    ComPtr::Release(staging);
    ComPtr::Release(duplication);
    ComPtr::Release(output1);
    ComPtr::Release(output);
  }

  ComPtr::Release(adapter);
  ComPtr::Release(dxgi_device);
  ComPtr::Release(context);
  ComPtr::Release(device);

  if (captured == 0) {
    if (reason != nullptr) *reason = fail_reason;
    return false;
  }
  {
    const size_t mid = (static_cast<size_t>(vw) * static_cast<size_t>(vh) / 2) * 4;
    GLog("grab: 取样 b=" + std::to_string(out->pixels[mid]) + " g=" +
         std::to_string(out->pixels[mid + 1]) + " r=" + std::to_string(out->pixels[mid + 2]));
  }
  GLog("grab: dxgi 抓到 " + std::to_string(captured) + " 个显示器");
  return true;
}

}  // namespace dshwork::grab
