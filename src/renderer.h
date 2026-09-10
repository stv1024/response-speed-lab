// D3D11 + DXGI 翻转模型渲染器。
// 存在的唯一理由：拿到 DXGI_FRAME_STATISTICS::SyncQPCTime —— 也就是
// 「你提交的这一帧，是在哪个 vblank 真正被扫出到屏幕的」。
// 这是浏览器 requestAnimationFrame 永远给不了的东西。
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <cstdint>

class Renderer
{
public:
    bool Init(HWND hwnd);
    void Shutdown();

    void OnResize(UINT width, UINT height);

    // 等待上一帧被消费（SetMaximumFrameLatency(1) 的可等待对象）。
    // 放在「读输入」之前，让输入尽可能新鲜。
    // timeoutMs 用于封顶：有延迟样本待触发时，不能在这里睡过它的到期时刻。
    void WaitForFrameLatency(DWORD timeoutMs = 1000);

    void BeginFrame();
    // 返回本次 Present 的序号；用它去认领 SyncQPCTime。0 表示失败。
    UINT64 Present();

    // 每帧调一次，回读 DXGI 的帧统计。
    // 若某个 present 序号刚刚上屏，写出它的 vblank QPC 时刻并返回 true。
    bool PollPresentedFrame(UINT64* outPresentCount, int64_t* outSyncQpc);

    bool  TearingSupported() const { return tearingSupported_; }
    bool& TearingEnabled()         { return tearingEnabled_; }
    bool  TearingEnabled() const   { return tearingEnabled_; }

    // 显示器 vblank 间隔（ms），由连续的 SyncQPCTime 差值实测得出。0 表示尚未测出。
    double MeasuredVblankMs() const { return vblankMs_; }

    // 帧统计是否还在推进。开启撕裂 / 某些合成路径下 DXGI 会停止更新它，
    // 此时「到屏幕上」无法测量，UI 必须如实说明而不是显示旧值。
    bool FrameStatsHealthy() const { return statsHealthy_; }
    void NoteStatsPollFailed();

    ID3D11Device*        Device()  { return device_; }
    ID3D11DeviceContext* Context() { return context_; }

private:
    void CreateRenderTarget();
    void ReleaseRenderTarget();

    HWND                    hwnd_        = nullptr;
    ID3D11Device*           device_      = nullptr;
    ID3D11DeviceContext*    context_     = nullptr;
    IDXGISwapChain2*        swapChain_   = nullptr;
    ID3D11RenderTargetView* rtv_         = nullptr;
    HANDLE                  waitable_    = nullptr;

    bool   tearingSupported_ = false;
    bool   tearingEnabled_   = false;

    UINT64 lastSeenPresentCount_ = 0;
    int64_t lastSyncQpc_ = 0;
    double  vblankMs_    = 0.0;
    int     staleStatsFrames_ = 0;
    bool    statsHealthy_     = true;
};
