// 响应速度实验室 · Win32 + D3D11 原生版
//
// 与旧 HTML 版的根本区别：这里能拿到「输入 -> 像素上屏」的真实闭环时间。
//   输入端：WM_POINTERDOWN 的 POINTER_INFO.PerformanceCount（硬件侧 QPC 时间戳）
//   输出端：DXGI_FRAME_STATISTICS::SyncQPCTime（承载该变化的那一帧的 vblank 时刻）
#include <windows.h>
#include <mmsystem.h>          // timeBeginPeriod（WIN32_LEAN_AND_MEAN 下需显式包含）
#include <shellscalingapi.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "renderer.h"
#include "app.h"
#include "timing.h"
#include "fonts.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

Renderer g_renderer;
App      g_app;
bool     g_running   = true;
bool     g_occluded  = false;
UINT     g_pendingW  = 0;
UINT     g_pendingH  = 0;
bool     g_needResize = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 先抓时间戳，再交给 ImGui —— 顺序不能反，否则测的是 ImGui 的处理时间。
    switch (msg)
    {
    case WM_POINTERDOWN:
    {
        const int64_t recv = timing::Now();
        const UINT32 pid = GET_POINTERID_WPARAM(wParam);
        POINTER_INFO pi{};
        int64_t hw = 0;
        if (GetPointerInfo(pid, &pi) && pi.PerformanceCount != 0)
            hw = static_cast<int64_t>(pi.PerformanceCount);
        g_app.OnPointerDown(hw, recv);
        break;
    }
    case WM_LBUTTONDOWN:
    {
        // WM_POINTERDOWN 不可用时的退路：没有硬件时间戳，只有消息到达时刻。
        const int64_t recv = timing::Now();
        g_app.OnPointerDown(0, recv);
        break;
    }
    default:
        break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_pendingW = LOWORD(lParam);
            g_pendingH = HIWORD(lParam);
            g_needResize = true;
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // 屏蔽 Alt 菜单
            return 0;
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { g_running = false; return 0; }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT)
            g_running = false;
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 1ms 定时器分辨率 + 高进程优先级：让调度抖动尽量不污染测量。
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ResponseSpeedLabWindow";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const float scale = static_cast<float>(dpi) / 96.0f;
    const int winW = static_cast<int>(1180 * scale);
    const int winH = static_cast<int>(900 * scale);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"响应速度实验室 · Response Speed Lab",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
        return 1;

    // 让鼠标也走 WM_POINTER* 管线，才能拿到 PerformanceCount。
    // 传统鼠标消息仍会照常投递，ImGui 不受影响。
    EnableMouseInPointer(TRUE);

    if (!g_renderer.Init(hwnd))
    {
        MessageBoxW(hwnd, L"D3D11 / DXGI 初始化失败。", L"启动失败", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // 不写 imgui.ini
    io.ConfigInputTrickleEventQueue = false;  // 同一帧内的按下立刻生效，不拖到下一帧

    fonts::Load(scale);
    fonts::ApplyStyle(scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_renderer.Device(), g_renderer.Context());

    g_app.Init(hwnd, &g_renderer);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    timing::PreciseWaiter waiter;

    while (g_running)
    {
        // 等上一帧被消费。必须等满：交换链没就绪就 Present 的话，
        // Present 自己会阻塞，白白多花一帧 —— 那一帧会直接算进测量结果里。
        // 这个等待最多一帧，之后再自旋到样本的到期时刻，两者不冲突。
        g_renderer.WaitForFrameLatency();

        PumpMessages();
        if (!g_running) break;

        if (g_needResize)
        {
            g_renderer.OnResize(g_pendingW, g_pendingH);
            g_needResize = false;
        }

        // 若有延迟样本即将到期，精确睡到那一刻再出这一帧，
        // 这样「目标 5ms」真的就是 5ms，而不是浏览器里的 5~20ms。
        const int64_t deadline = g_app.NextDeadlineQpc();
        if (deadline != 0)
        {
            const double remain = timing::Ms(timing::Now(), deadline);
            if (remain > 0.0 && remain < 25.0)
            {
                waiter.WaitUntil(deadline);
                PumpMessages();
            }
        }

        const int64_t now = timing::Now();
        g_app.ResolveDeadlines(now);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        g_app.BuildUI();
        ImGui::Render();

        g_renderer.BeginFrame();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const UINT64 tag = g_renderer.Present();
        g_app.TagPresent(tag);

        UINT64  presented = 0;
        int64_t syncQpc   = 0;
        bool    got       = false;
        while (g_renderer.PollPresentedFrame(&presented, &syncQpc))
        {
            got = true;
            g_app.OnPresented(presented, syncQpc);
        }
        if (!got)
        {
            g_renderer.NoteStatsPollFailed();
            // 帧统计不可用时，别让探针无限堆积。
            if (!g_renderer.FrameStatsHealthy())
                g_app.DropStaleProbes(timing::Now());
        }
    }

    g_app.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_renderer.Shutdown();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    timeEndPeriod(1);
    return 0;
}
