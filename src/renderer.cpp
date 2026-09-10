#include "renderer.h"
#include "timing.h"

bool Renderer::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    IDXGIFactory2* factory2 = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory2))))
        return false;

    // 撕裂支持（ALLOW_TEARING）：允许不等 vsync 立刻上屏。
    {
        IDXGIFactory5* factory5 = nullptr;
        if (SUCCEEDED(factory2->QueryInterface(IID_PPV_ARGS(&factory5))))
        {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                tearingSupported_ = (allow == TRUE);
            factory5->Release();
        }
    }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, _countof(levels), D3D11_SDK_VERSION,
        &device_, &got, &context_);
    if (FAILED(hr))
    {
        factory2->Release();
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width       = static_cast<UINT>(rc.right - rc.left);
    sd.Height      = static_cast<UINT>(rc.bottom - rc.top);
    sd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc  = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling     = DXGI_SCALING_NONE;
    // FLIP_DISCARD：翻转模型，命中 independent flip 时可绕过 DWM 桌面合成那一跳。
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingSupported_)
        sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    IDXGISwapChain1* sc1 = nullptr;
    hr = factory2->CreateSwapChainForHwnd(device_, hwnd_, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr))
    {
        // 有些环境不支持 SCALING_NONE，退一步再试。
        sd.Scaling = DXGI_SCALING_STRETCH;
        hr = factory2->CreateSwapChainForHwnd(device_, hwnd_, &sd, nullptr, nullptr, &sc1);
    }
    if (SUCCEEDED(hr))
    {
        hr = sc1->QueryInterface(IID_PPV_ARGS(&swapChain_));
        sc1->Release();
    }

    // 屏蔽 Alt+Enter 全屏切换，避免打乱测量。
    factory2->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    factory2->Release();

    if (FAILED(hr) || !swapChain_)
        return false;

    // 帧延迟 1：CPU 最多领先 GPU 一帧，减少排队带来的额外延迟。
    swapChain_->SetMaximumFrameLatency(1);
    waitable_ = swapChain_->GetFrameLatencyWaitableObject();

    // 默认关闭：先给出对齐 vsync 的诚实基线，让用户自己打开去看省下的那一帧。
    tearingEnabled_ = false;
    CreateRenderTarget();
    return rtv_ != nullptr;
}

void Renderer::Shutdown()
{
    ReleaseRenderTarget();
    if (waitable_)  { CloseHandle(waitable_);  waitable_  = nullptr; }
    if (swapChain_) { swapChain_->Release();   swapChain_ = nullptr; }
    if (context_)   { context_->Release();     context_   = nullptr; }
    if (device_)    { device_->Release();      device_    = nullptr; }
}

void Renderer::CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))))
    {
        device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
}

void Renderer::ReleaseRenderTarget()
{
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void Renderer::OnResize(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0)
        return;

    ReleaseRenderTarget();

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingSupported_)
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
    CreateRenderTarget();
}

void Renderer::WaitForFrameLatency(DWORD timeoutMs)
{
    if (waitable_)
        WaitForSingleObjectEx(waitable_, timeoutMs, TRUE);
}

void Renderer::BeginFrame()
{
    if (!rtv_) return;
    const float clear[4] = { 0.043f, 0.059f, 0.078f, 1.0f };
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear);
}

UINT64 Renderer::Present()
{
    if (!swapChain_) return 0;

    UINT syncInterval = 1;
    UINT flags = 0;
    if (tearingEnabled_ && tearingSupported_)
    {
        // 不等 vsync，立刻上屏（会撕裂）。ALLOW_TEARING 只在 syncInterval==0 时合法。
        syncInterval = 0;
        flags = DXGI_PRESENT_ALLOW_TEARING;
    }

    if (FAILED(swapChain_->Present(syncInterval, flags)))
        return 0;

    // Present 返回后立刻问 DXGI「我刚提交的是第几帧」，用于后续认领。
    UINT lastPresent = 0;
    if (FAILED(swapChain_->GetLastPresentCount(&lastPresent)))
        return 0;
    return lastPresent;
}

bool Renderer::PollPresentedFrame(UINT64* outPresentCount, int64_t* outSyncQpc)
{
    if (!swapChain_) return false;

    DXGI_FRAME_STATISTICS fs{};
    // DXGI_ERROR_FRAME_STATISTICS_DISJOINT 等情况下直接跳过本次回读。
    if (FAILED(swapChain_->GetFrameStatistics(&fs)))
        return false;
    if (fs.PresentCount == 0 || fs.SyncQPCTime.QuadPart == 0)
        return false;
    if (fs.PresentCount == lastSeenPresentCount_)
        return false;

    // 用连续 vblank 时刻差实测刷新间隔。
    if (lastSyncQpc_ != 0)
    {
        const double dt = timing::Ms(lastSyncQpc_, fs.SyncQPCTime.QuadPart);
        // 可能跨了多帧；折算回单帧，并做指数平滑。
        if (dt > 0.5 && dt < 200.0)
        {
            const int frames = (vblankMs_ > 0.0)
                ? static_cast<int>(dt / vblankMs_ + 0.5) : 1;
            const double per = dt / (frames > 0 ? frames : 1);
            if (per > 0.5 && per < 60.0)
                vblankMs_ = (vblankMs_ > 0.0) ? (vblankMs_ * 0.9 + per * 0.1) : per;
        }
    }

    lastSeenPresentCount_ = fs.PresentCount;
    lastSyncQpc_ = fs.SyncQPCTime.QuadPart;

    staleStatsFrames_ = 0;
    statsHealthy_     = true;

    *outPresentCount = fs.PresentCount;
    *outSyncQpc      = fs.SyncQPCTime.QuadPart;
    return true;
}

void Renderer::NoteStatsPollFailed()
{
    // 判据必须按时间算，不能按帧数：开启撕裂后帧率不设上限，
    // 一个刷新周期内会轮询很多次，连续「没有新数据」是正常的。
    ++staleStatsFrames_;
    const int64_t now = timing::Now();
    if (lastSyncQpc_ == 0)
        return;
    if (timing::Ms(lastSyncQpc_, now) > 500.0)
        statsHealthy_ = false;
}
