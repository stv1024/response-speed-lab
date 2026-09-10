// QPC 计时基元。全程使用 QueryPerformanceCounter，不做任何单位转换上的偷懒。
#pragma once
#include <windows.h>
#include <cstdint>

namespace timing {

inline int64_t Freq()
{
    static const int64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return li.QuadPart;
    }();
    return f;
}

inline int64_t Now()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// a -> b 的毫秒数（可为负）
inline double Ms(int64_t a, int64_t b)
{
    return static_cast<double>(b - a) * 1000.0 / static_cast<double>(Freq());
}

inline int64_t MsToTicks(double ms)
{
    return static_cast<int64_t>(ms * static_cast<double>(Freq()) / 1000.0);
}

// 高精度等待：先用高分辨率可等待定时器睡到目标点前 0.6ms，再自旋补齐。
// 定时器不可用时退化为 Sleep(0) 自旋。
class PreciseWaiter
{
public:
    PreciseWaiter()
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        highRes_ = (timer_ != nullptr);
        if (!timer_)
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    ~PreciseWaiter()
    {
        if (timer_) CloseHandle(timer_);
    }

    PreciseWaiter(const PreciseWaiter&) = delete;
    PreciseWaiter& operator=(const PreciseWaiter&) = delete;

    bool HighResolution() const { return highRes_; }

    void WaitUntil(int64_t targetQpc) const
    {
        const double kSpinMs = 0.6;
        if (timer_)
        {
            const double remain = Ms(Now(), targetQpc);
            if (remain > kSpinMs)
            {
                // 负值 = 相对时间，单位 100ns
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((remain - kSpinMs) * 10000.0);
                if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject(timer_, INFINITE);
            }
        }
        while (Now() < targetQpc)
            YieldProcessor();
    }

private:
    HANDLE timer_ = nullptr;
    bool   highRes_ = false;
};

} // namespace timing
