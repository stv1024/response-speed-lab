#pragma once
#include <windows.h>
#include <cstdint>
#include <deque>
#include <vector>

#include "renderer.h"
#include "timing.h"

// 一次按下的完整生命周期：
//   硬件时间戳 -> WndProc 收到 -> UI 认领 -> 提交某一帧 -> 该帧在某个 vblank 上屏
struct Probe
{
    int64_t inputQpc    = 0;   // POINTER_INFO.PerformanceCount，最接近硬件的时刻
    int64_t dispatchQpc = 0;   // WndProc 里读到消息的时刻
    int64_t uiQpc       = 0;   // UI 逻辑认领这次按下的时刻
    UINT64  presentTag  = 0;   // 承载视觉变化的那一帧的 present 序号
    bool    tagged      = false;
    int     slot        = -1;  // -1 = 即时响应按钮；>=0 = 延迟样本/手动
};

struct RollingStats
{
    std::deque<double> v;
    void Push(double x, size_t cap = 100)
    {
        v.push_back(x);
        while (v.size() > cap) v.pop_front();
    }
    void Clear() { v.clear(); }
    bool Empty() const { return v.empty(); }
    double Min() const;
    double Max() const;
    double Avg() const;
    double Percentile(double p) const; // p in [0,1]
};

class App
{
public:
    bool Init(HWND hwnd, Renderer* renderer);
    void Shutdown();

    // 从 WndProc 传入的原始输入时刻。
    void OnPointerDown(int64_t inputQpc, int64_t dispatchQpc);

    // 帧循环钩子
    int64_t NextDeadlineQpc() const;      // 最近的一个待触发延迟样本，0 = 无
    void    ResolveDeadlines(int64_t now);
    void    BuildUI();
    void    TagPresent(UINT64 presentCount);
    void    OnPresented(UINT64 presentCount, int64_t syncQpc);
    // 帧统计不可用时清理永远等不到结果的探针。
    void    DropStaleProbes(int64_t now);

private:
    void DrawInstantPanel();
    void DrawSamplePanel();
    void DrawManualPanel();
    void DrawReactionPanel();
    void DrawReferencePanel();
    void DrawHeader();

    // 认领一次按下：返回本帧是否真的有一次新的输入可用
    bool ConsumeInput(int64_t* inputQpc, int64_t* dispatchQpc);

    void ArmProbe(int slot);

    HWND      hwnd_     = nullptr;
    Renderer* renderer_ = nullptr;
    timing::PreciseWaiter waiter_;

    // --- 输入 ---
    int64_t pendingInputQpc_    = 0;
    int64_t pendingDispatchQpc_ = 0;
    bool    hasPendingInput_    = false;
    bool    pointerApiActive_   = false;

    // --- 待解算的探针 ---
    std::vector<Probe> inFlight_;

    // --- 1. 即时响应 ---
    RollingStats dispatchStats_;   // 硬件 -> JS/WndProc
    RollingStats e2eStats_;        // 硬件 -> vblank 上屏
    double lastDispatchMs_ = -1.0;
    double lastE2eMs_      = -1.0;
    double lastUiMs_       = -1.0;
    int64_t instantLitUntil_ = 0;  // 大按钮高亮到期时刻

    // --- 2/3. 延迟样本 ---
    struct Reveal
    {
        int64_t deadlineQpc = 0;
        int64_t inputQpc    = 0;
        int     slot        = 0;
        int     targetMs    = 0;
        bool    active      = false;
    };
    std::vector<Reveal> pendingReveals_;
    std::vector<int64_t> slotLitUntil_;   // 每个样本按钮的高亮到期时刻
    int     lastSampleTarget_  = -1;
    double  lastSampleActualMs_ = -1.0;
    bool    lastSampleResolved_ = false;
    int     manualMs_ = 100;

    // --- 4. 反应时间 ---
    enum class React { Idle, Waiting, Ready };
    React   reactState_    = React::Idle;
    int64_t reactGreenQpc_ = 0;
    int64_t reactArmAtQpc_ = 0;
    double  lastReactMs_   = -1.0;
    bool    reactTooEarly_ = false;
    RollingStats reactStats_;
    unsigned reactSeed_ = 1;

    friend class AppAccess;
};
