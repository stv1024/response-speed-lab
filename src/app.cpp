#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiButtonFlags_PressedOnClick
#include "fonts.h"

// ---------------------------------------------------------------- stats

double RollingStats::Min() const
{
    if (v.empty()) return 0.0;
    return *std::min_element(v.begin(), v.end());
}
double RollingStats::Max() const
{
    if (v.empty()) return 0.0;
    return *std::max_element(v.begin(), v.end());
}
double RollingStats::Avg() const
{
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}
double RollingStats::Percentile(double p) const
{
    if (v.empty()) return 0.0;
    std::vector<double> t(v.begin(), v.end());
    std::sort(t.begin(), t.end());
    const size_t i = static_cast<size_t>(p * (t.size() - 1) + 0.5);
    return t[i];
}

// ---------------------------------------------------------------- consts

namespace {

struct SampleDef { int ms; const char* tag; };
const SampleDef kSamples[] = {
    { 0,   "即时"       },
    { 5,   ""           },
    { 10,  ""           },
    { 16,  "≈1帧@60Hz"  },
    { 33,  "≈2帧@60Hz"  },
    { 50,  ""           },
    { 100, ""           },
    { 200, ""           },
    { 400, ""           },
    { 800, ""           },
};
constexpr int kSampleCount = static_cast<int>(sizeof(kSamples) / sizeof(kSamples[0]));
constexpr int kManualSlot  = kSampleCount;         // 手动滑块占一个槽位
constexpr int kSlotCount   = kSampleCount + 1;
constexpr int kReactSlot   = -2;                   // 反应测试：测「绿色真正上屏」的时刻

const ImVec4 kAccent  = ImVec4(0.133f, 0.827f, 0.933f, 1.0f);
const ImVec4 kMuted   = ImVec4(0.545f, 0.596f, 0.647f, 1.0f);
const ImVec4 kInk     = ImVec4(0.024f, 0.133f, 0.173f, 1.0f);

void TextMuted(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// 等宽大号读数
void BigValue(const char* text, ImVec4 col, float scale = 1.7f)
{
    ImGui::PushFont(fonts::g_mono, ImGui::GetStyle().FontSizeBase * scale);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void MsText(char* buf, size_t n, double ms)
{
    if (ms < 0.0) std::snprintf(buf, n, "—");
    else          std::snprintf(buf, n, "%.2f ms", ms);
}

} // namespace

// ---------------------------------------------------------------- lifecycle

bool App::Init(HWND hwnd, Renderer* renderer)
{
    hwnd_     = hwnd;
    renderer_ = renderer;
    slotLitUntil_.assign(kSlotCount, 0);
    reactSeed_ = static_cast<unsigned>(timing::Now() & 0xFFFFFFFF) | 1u;
    return true;
}

void App::Shutdown() {}

// ---------------------------------------------------------------- input

void App::OnPointerDown(int64_t inputQpc, int64_t dispatchQpc)
{
    // WM_POINTERDOWN 与 WM_LBUTTONDOWN 会成对到达（EnableMouseInPointer 下
    // 系统仍会合成传统鼠标消息）。有硬件时间戳的那条优先，另一条丢弃。
    if (inputQpc != 0)
    {
        pointerApiActive_ = true;
    }
    else
    {
        if (pointerApiActive_) return;   // 已经由 WM_POINTERDOWN 记过了
        inputQpc = dispatchQpc;          // 退路：只有消息到达时刻
    }

    // 同一帧内多次按下只保留第一次（ImGui 一帧也只会认领一次点击）。
    if (hasPendingInput_) return;

    pendingInputQpc_    = inputQpc;
    pendingDispatchQpc_ = dispatchQpc;
    hasPendingInput_    = true;
}

bool App::ConsumeInput(int64_t* inputQpc, int64_t* dispatchQpc)
{
    if (!hasPendingInput_) return false;

    // 落在空白处的按下没人认领，会一直挂着。若不设有效期，下一次真正按到按钮时
    // 就会取到这个陈旧时间戳，算出荒谬的延迟。超过 100ms 视为过期丢弃。
    if (timing::Ms(pendingDispatchQpc_, timing::Now()) > 100.0)
    {
        hasPendingInput_ = false;
        return false;
    }

    *inputQpc    = pendingInputQpc_;
    *dispatchQpc = pendingDispatchQpc_;
    hasPendingInput_ = false;
    return true;
}

// ---------------------------------------------------------------- probes

void App::ArmProbe(int slot)
{
    Probe p;
    p.slot = slot;
    if (slot == kReactSlot)
    {
        // 反应测试只关心「绿色何时上屏」，没有输入端。
        p.inputQpc = p.dispatchQpc = p.uiQpc = timing::Now();
    }
    inFlight_.push_back(p);
}

void App::TagPresent(UINT64 presentCount)
{
    if (presentCount == 0) return;
    for (Probe& p : inFlight_)
        if (!p.tagged) { p.presentTag = presentCount; p.tagged = true; }
}

void App::OnPresented(UINT64 presentCount, int64_t syncQpc)
{
    // DXGI 的 PresentCount 可能一次跳过好几帧（我们每帧只回读一次，中间的看不到）。
    // 若探针挂的是被跳过的那一帧，直接拿当前 syncQpc 会高估好几个刷新周期。
    // 垂直同步下一个 present 对应一个 vblank，因此可以按实测帧间隔回推。
    const double vb = renderer_->MeasuredVblankMs();

    for (size_t i = 0; i < inFlight_.size(); )
    {
        Probe& p = inFlight_[i];
        if (!p.tagged || presentCount < p.presentTag) { ++i; continue; }

        int64_t hitQpc = syncQpc;
        if (presentCount > p.presentTag && vb > 0.0 && !renderer_->TearingEnabled())
        {
            const UINT64 skipped = presentCount - p.presentTag;
            hitQpc = syncQpc - timing::MsToTicks(static_cast<double>(skipped) * vb);
        }
        if (p.slot == kReactSlot)
        {
            // 绿色真正被扫出到屏幕的时刻 —— 从这里开始计人的反应时间。
            reactGreenQpc_ = hitQpc;
        }
        else if (p.slot == -1)
        {
            lastDispatchMs_ = timing::Ms(p.inputQpc, p.dispatchQpc);
            lastUiMs_       = timing::Ms(p.inputQpc, p.uiQpc);
            lastE2eMs_      = timing::Ms(p.inputQpc, hitQpc);
            dispatchStats_.Push(lastDispatchMs_);
            e2eStats_.Push(lastE2eMs_);
        }
        else
        {
            lastSampleActualMs_ = timing::Ms(p.inputQpc, hitQpc);
            lastSampleResolved_ = true;
        }
        inFlight_.erase(inFlight_.begin() + static_cast<ptrdiff_t>(i));
    }
}

// ---------------------------------------------------------------- deadlines

void App::DropStaleProbes(int64_t now)
{
    for (size_t i = 0; i < inFlight_.size(); )
    {
        if (timing::Ms(inFlight_[i].uiQpc, now) > 500.0)
            inFlight_.erase(inFlight_.begin() + static_cast<ptrdiff_t>(i));
        else
            ++i;
    }
}

int64_t App::NextDeadlineQpc() const
{
    int64_t best = 0;
    for (const Reveal& r : pendingReveals_)
    {
        if (!r.active) continue;
        if (best == 0 || r.deadlineQpc < best) best = r.deadlineQpc;
    }
    if (reactState_ == React::Waiting && reactArmAtQpc_ != 0)
        if (best == 0 || reactArmAtQpc_ < best) best = reactArmAtQpc_;
    return best;
}

void App::ResolveDeadlines(int64_t now)
{
    for (size_t i = 0; i < pendingReveals_.size(); )
    {
        Reveal& r = pendingReveals_[i];
        if (!r.active || now < r.deadlineQpc) { ++i; continue; }

        // 到点了：本帧点亮这个按钮，并挂一个探针去测它真正上屏的时刻。
        if (r.slot >= 0 && r.slot < static_cast<int>(slotLitUntil_.size()))
            slotLitUntil_[r.slot] = now + timing::MsToTicks(220.0);

        Probe p;
        p.slot        = r.slot;
        p.inputQpc    = r.inputQpc;
        p.dispatchQpc = r.inputQpc;
        p.uiQpc       = now;
        inFlight_.push_back(p);

        lastSampleTarget_   = r.targetMs;
        lastSampleResolved_ = false;

        pendingReveals_.erase(pendingReveals_.begin() + static_cast<ptrdiff_t>(i));
    }

    if (reactState_ == React::Waiting && reactArmAtQpc_ != 0 && now >= reactArmAtQpc_)
    {
        reactState_    = React::Ready;
        reactArmAtQpc_ = 0;
        reactGreenQpc_ = 0;      // 等 vblank 回读填入真实上屏时刻
        ArmProbe(kReactSlot);
    }
}

// ---------------------------------------------------------------- UI

void App::BuildUI()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawHeader();
    DrawInstantPanel();
    DrawSamplePanel();
    DrawManualPanel();
    DrawReactionPanel();
    DrawReferencePanel();

    ImGui::End();
}

void App::DrawHeader()
{
    ImGui::PushFont(fonts::g_text, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("响应速度实验室");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    TextMuted("原生 Win32 + D3D11 翻转链。测量的是「输入硬件时间戳 → 画面真正扫出到屏幕」的闭环，"
              "而不是回调被调用的时刻。");
    ImGui::Spacing();

    const double vb = renderer_->MeasuredVblankMs();
    char hz[64] = "检测中…";
    char fr[64] = "—";
    if (vb > 0.0)
    {
        std::snprintf(hz, sizeof(hz), "%.1f Hz", 1000.0 / vb);
        std::snprintf(fr, sizeof(fr), "%.2f ms", vb);
    }

    auto chip = [](const char* label, const char* value, ImVec4 col) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 6);
        ImGui::PushFont(fonts::g_mono, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(value);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    };

    chip("刷新率（实测 vblank）", hz, kAccent);
    ImGui::SameLine(0, 24); chip("每帧", fr, kAccent);
    ImGui::SameLine(0, 24); chip("输入时间戳",
        pointerApiActive_ ? "硬件 QPC" : "消息到达",
        pointerApiActive_ ? ImVec4(0.204f, 0.827f, 0.600f, 1.0f) : ImVec4(0.984f, 0.749f, 0.141f, 1.0f));
    ImGui::SameLine(0, 24); chip("高精度定时器",
        waiter_.HighResolution() ? "可用" : "不可用",
        waiter_.HighResolution() ? ImVec4(0.204f, 0.827f, 0.600f, 1.0f) : ImVec4(0.984f, 0.749f, 0.141f, 1.0f));

    if (renderer_->TearingSupported())
    {
        ImGui::SameLine(0, 24);
        bool& t = renderer_->TearingEnabled();
        if (ImGui::Checkbox("不等 vsync 立即上屏（会撕裂）", &t)) {}
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("开：Present(0, ALLOW_TEARING)，画面立刻扫出，端到端延迟少约半帧到一帧，代价是撕裂。\n"
                              "关：Present(1)，对齐 vblank，画面完整。\n"
                              "打开后对比「到屏幕上」那个数字 —— 这就是一帧的真实价格。");
    }
    else
    {
        ImGui::SameLine(0, 24);
        TextMuted("(本机不支持 ALLOW_TEARING)");
    }

    ImGui::Spacing();
}

void App::DrawInstantPanel()
{
    ImGui::SeparatorText("1 · 即时响应：这台机器最快能多快");
    TextMuted("按下大按钮。它在收到按下的那一帧就改变外观，然后我们回读 DXGI，问「承载这个变化的那一帧，"
              "是在哪个 vblank 被扫出去的」。");
    ImGui::Spacing();

    const int64_t now = timing::Now();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float btnW  = avail * 0.42f;
    const float btnH  = 236.0f * ImGui::GetStyle().FontScaleDpi;
    // 读数面板比按钮高一些：统计那两行加上「清空统计」要放得下，不能被裁掉。
    const float panelH = btnH + 66.0f * ImGui::GetStyle().FontScaleDpi;

    // 自绘按钮：先判定按下，再绘制。这样「按下」和「变亮」发生在同一帧，
    // 中间没有任何动画曲线或状态机延后。
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##instant", ImVec2(btnW, btnH),
                                                ImGuiButtonFlags_PressedOnClick);
    if (pressed)
    {
        int64_t in = 0, disp = 0;
        if (ConsumeInput(&in, &disp))
        {
            Probe p;
            p.slot = -1;
            p.inputQpc    = in;
            p.dispatchQpc = disp;
            p.uiQpc       = timing::Now();
            inFlight_.push_back(p);
        }
        instantLitUntil_ = now + timing::MsToTicks(120.0);
    }

    const bool lit = (now < instantLitUntil_);
    const ImVec2 p1 = ImVec2(p0.x + btnW, p0.y + btnH);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (lit)
    {
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.133f, 0.827f, 0.933f, 1.0f)), 16.0f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.404f, 0.910f, 0.976f, 1.0f)), 16.0f, 0, 3.0f);
    }
    else
    {
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(
            hov ? ImVec4(0.102f, 0.153f, 0.196f, 1.0f) : ImVec4(0.078f, 0.118f, 0.153f, 1.0f)), 16.0f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.141f, 0.188f, 0.235f, 1.0f)), 16.0f, 0, 2.0f);
    }

    {
        const char* t1 = "点我 · 立即响应";
        const char* t2 = "按下即变色，零动画、零缓动";
        ImGui::PushFont(fonts::g_text, ImGui::GetStyle().FontSizeBase * 1.5f);
        const ImVec2 s1 = ImGui::CalcTextSize(t1);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p0.x + (btnW - s1.x) * 0.5f, p0.y + btnH * 0.5f - s1.y),
                    ImGui::GetColorU32(lit ? kInk : ImVec4(0.902f, 0.929f, 0.953f, 1.0f)), t1);
        ImGui::PopFont();
        const ImVec2 s2 = ImGui::CalcTextSize(t2);
        dl->AddText(ImVec2(p0.x + (btnW - s2.x) * 0.5f, p0.y + btnH * 0.5f + s2.y * 0.6f),
                    ImGui::GetColorU32(lit ? ImVec4(0.047f, 0.290f, 0.369f, 1.0f) : kMuted), t2);
    }

    ImGui::SameLine(0, 18);

    ImGui::BeginChild("##readout", ImVec2(0, panelH), ImGuiChildFlags_Borders);
    {
        char buf[64];

        const bool statsOk = renderer_->FrameStatsHealthy();

        TextMuted("到屏幕上（输入 → vblank 扫出）");
        if (statsOk)
        {
            MsText(buf, sizeof(buf), lastE2eMs_);
            BigValue(buf, fonts::LatencyColor(lastE2eMs_), 1.9f);
        }
        else
        {
            BigValue("不可测", ImVec4(0.984f, 0.749f, 0.141f, 1.0f), 1.5f);
            TextMuted("当前显示路径下 DXGI 停止上报帧统计（常见于开启撕裂后）。"
                      "关掉「不等 vsync 立即上屏」即可恢复。");
        }

        ImGui::Spacing();
        ImGui::Columns(2, nullptr, false);
        TextMuted("到程序（输入 → 消息到手）");
        MsText(buf, sizeof(buf), lastDispatchMs_);
        BigValue(buf, fonts::LatencyColor(lastDispatchMs_), 1.15f);
        ImGui::NextColumn();
        TextMuted("到逻辑（输入 → UI 认领）");
        MsText(buf, sizeof(buf), lastUiMs_);
        BigValue(buf, fonts::LatencyColor(lastUiMs_), 1.15f);
        ImGui::Columns(1);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (e2eStats_.Empty())
        {
            TextMuted("还没有样本，按几次上面的按钮。");
        }
        else
        {
            ImGui::PushFont(fonts::g_mono, 0.0f);
            ImGui::Text("到屏幕  min %.2f   avg %.2f   p95 %.2f   max %.2f",
                        e2eStats_.Min(), e2eStats_.Avg(), e2eStats_.Percentile(0.95), e2eStats_.Max());
            ImGui::Text("到程序  min %.2f   avg %.2f   p95 %.2f   max %.2f",
                        dispatchStats_.Min(), dispatchStats_.Avg(),
                        dispatchStats_.Percentile(0.95), dispatchStats_.Max());
            ImGui::PopFont();
            ImGui::Spacing();
            TextMuted("最近 %d 次", static_cast<int>(e2eStats_.v.size()));
            ImGui::SameLine(0, 16);
            if (ImGui::SmallButton("清空统计"))
            {
                e2eStats_.Clear();
                dispatchStats_.Clear();
                lastE2eMs_ = lastDispatchMs_ = lastUiMs_ = -1.0;
            }
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();
}

void App::DrawSamplePanel()
{
    ImGui::SeparatorText("2 · 延迟样本：亲手对比不同毫秒数");
    TextMuted("点击后延迟指定毫秒才给视觉反馈。延迟由高分辨率定时器 + 自旋补齐实现，"
              "5ms 就是 5ms —— 浏览器的 setTimeout 在这个量级上做不到。");
    ImGui::Spacing();

    const int64_t now = timing::Now();
    const float btnW = 118.0f * ImGui::GetStyle().FontScaleDpi;
    const float btnH = 76.0f  * ImGui::GetStyle().FontScaleDpi;
    const float availW = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, static_cast<int>(availW / (btnW + ImGui::GetStyle().ItemSpacing.x)));

    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < kSampleCount; ++i)
    {
        if (i % perRow != 0) ImGui::SameLine();

        char id[32];
        std::snprintf(id, sizeof(id), "##sample%d", i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(btnW, btnH),
                                                    ImGuiButtonFlags_PressedOnClick);
        if (pressed)
        {
            int64_t in = 0, disp = 0;
            if (!ConsumeInput(&in, &disp)) { in = disp = timing::Now(); }

            lastSampleTarget_   = kSamples[i].ms;
            lastSampleResolved_ = false;
            lastSampleActualMs_ = -1.0;

            // 短目标必须在「本帧」内完成，不能推到下一帧再点亮 ——
            // 否则会白白多花一整帧（60Hz 下 16.7ms），把 5ms 的样本
            // 变成 5ms + 一帧，比目标本身还大。这里直接自旋到到期时刻。
            const double kSameFrameMaxMs = 20.0;
            if (kSamples[i].ms == 0)
            {
                slotLitUntil_[i] = now + timing::MsToTicks(220.0);
                Probe p;
                p.slot = i;
                p.inputQpc = in; p.dispatchQpc = disp; p.uiQpc = timing::Now();
                inFlight_.push_back(p);
            }
            else if (kSamples[i].ms <= kSameFrameMaxMs)
            {
                waiter_.WaitUntil(in + timing::MsToTicks(kSamples[i].ms));
                slotLitUntil_[i] = timing::Now() + timing::MsToTicks(220.0);
                Probe p;
                p.slot = i;
                p.inputQpc = in; p.dispatchQpc = disp; p.uiQpc = timing::Now();
                inFlight_.push_back(p);
            }
            else
            {
                Reveal r;
                r.active      = true;
                r.slot        = i;
                r.targetMs    = kSamples[i].ms;
                r.inputQpc    = in;
                // 从硬件输入时刻起算，而不是从「现在」起算。
                r.deadlineQpc = in + timing::MsToTicks(kSamples[i].ms);
                pendingReveals_.push_back(r);
            }
        }

        const bool lit = (now < slotLitUntil_[i]);
        const ImVec2 p1 = ImVec2(p0.x + btnW, p0.y + btnH);
        if (lit)
        {
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.133f, 0.827f, 0.933f, 1.0f)), 12.0f);
            dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.404f, 0.910f, 0.976f, 1.0f)), 12.0f, 0, 2.5f);
        }
        else
        {
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(
                hov ? ImVec4(0.129f, 0.176f, 0.220f, 1.0f) : ImVec4(0.090f, 0.125f, 0.161f, 1.0f)), 12.0f);
            dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.141f, 0.188f, 0.235f, 1.0f)), 12.0f, 0, 1.5f);
        }

        char label[32];
        std::snprintf(label, sizeof(label), "%d ms", kSamples[i].ms);
        ImGui::PushFont(fonts::g_mono, ImGui::GetStyle().FontSizeBase * 1.15f);
        const ImVec2 ls = ImGui::CalcTextSize(label);
        const bool hasTag = kSamples[i].tag[0] != '\0';
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p0.x + (btnW - ls.x) * 0.5f,
                           p0.y + btnH * 0.5f - ls.y * (hasTag ? 0.85f : 0.5f)),
                    ImGui::GetColorU32(lit ? kInk : ImVec4(0.902f, 0.929f, 0.953f, 1.0f)), label);
        ImGui::PopFont();
        if (hasTag)
        {
            const ImVec2 ts = ImGui::CalcTextSize(kSamples[i].tag);
            dl->AddText(ImVec2(p0.x + (btnW - ts.x) * 0.5f, p0.y + btnH * 0.5f + ts.y * 0.25f),
                        ImGui::GetColorU32(lit ? ImVec4(0.047f, 0.290f, 0.369f, 1.0f) : kMuted),
                        kSamples[i].tag);
        }
    }

    ImGui::Spacing();
    ImGui::BeginChild("##sampleReadout", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (lastSampleTarget_ < 0)
    {
        TextMuted("点击上方样本，这里显示目标延迟与实测的端到端延迟。");
    }
    else
    {
        ImGui::PushFont(fonts::g_mono, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::Text("目标 %d ms", lastSampleTarget_);
        ImGui::SameLine(0, 20);
        ImGui::TextUnformatted("·");
        ImGui::SameLine(0, 20);
        if (lastSampleResolved_)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, fonts::LatencyColor(lastSampleActualMs_));
            ImGui::Text("实测到屏幕 %.2f ms", lastSampleActualMs_);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 20);
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::Text("(其中 %.2f ms 是显示管线自己的开销)",
                        lastSampleActualMs_ - lastSampleTarget_);
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextUnformatted("等待上屏…");
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::Spacing();
}

void App::DrawManualPanel()
{
    ImGui::SeparatorText("3 · 手动调节");
    TextMuted("任意设定 0–500ms，精细体会某个具体毫秒数的手感。");
    ImGui::Spacing();

    const int64_t now = timing::Now();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::SliderInt("##manual", &manualMs_, 0, 500, "%d ms");
    ImGui::SameLine(0, 18);

    const bool lit = (now < slotLitUntil_[kManualSlot]);
    if (lit)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.133f, 0.827f, 0.933f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.133f, 0.827f, 0.933f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.133f, 0.827f, 0.933f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          kInk);
    }

    bool pressed = false;
    {
        // 用 ButtonEx 拿到 PressedOnClick 语义
        const ImVec2 sz = ImVec2(0, 0);
        pressed = ImGui::ButtonEx("感受这个延迟", sz, ImGuiButtonFlags_PressedOnClick);
    }
    if (lit) ImGui::PopStyleColor(4);

    if (pressed)
    {
        int64_t in = 0, disp = 0;
        if (!ConsumeInput(&in, &disp)) { in = disp = timing::Now(); }

        lastSampleTarget_   = manualMs_;
        lastSampleResolved_ = false;
        lastSampleActualMs_ = -1.0;

        if (manualMs_ == 0)
        {
            slotLitUntil_[kManualSlot] = now + timing::MsToTicks(220.0);
            Probe p;
            p.slot = kManualSlot;
            p.inputQpc = in; p.dispatchQpc = disp; p.uiQpc = timing::Now();
            inFlight_.push_back(p);
        }
        else if (manualMs_ <= 20)
        {
            // 同上：短目标本帧内自旋兑现，避免多花一整帧。
            waiter_.WaitUntil(in + timing::MsToTicks(manualMs_));
            slotLitUntil_[kManualSlot] = timing::Now() + timing::MsToTicks(220.0);
            Probe p;
            p.slot = kManualSlot;
            p.inputQpc = in; p.dispatchQpc = disp; p.uiQpc = timing::Now();
            inFlight_.push_back(p);
        }
        else
        {
            Reveal r;
            r.active      = true;
            r.slot        = kManualSlot;
            r.targetMs    = manualMs_;
            r.inputQpc    = in;
            r.deadlineQpc = in + timing::MsToTicks(manualMs_);
            pendingReveals_.push_back(r);
        }
    }
    ImGui::Spacing();
}

void App::DrawReactionPanel()
{
    ImGui::SeparatorText("4 · 你的反应时间（参考系）");
    TextMuted("点击开始，方块变绿后立刻点击。绿色的起算时刻取自它真正上屏的那个 vblank，"
              "所以这个数字不含显示延迟，是干净的人类反应时间。");
    ImGui::Spacing();

    const float h = 150.0f * ImGui::GetStyle().FontScaleDpi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    const bool pressed = ImGui::InvisibleButton("##react", ImVec2(w, h),
                                                ImGuiButtonFlags_PressedOnClick);
    if (pressed)
    {
        int64_t in = 0, disp = 0;
        if (!ConsumeInput(&in, &disp)) { in = disp = timing::Now(); }

        if (reactState_ == React::Idle)
        {
            reactState_    = React::Waiting;
            reactTooEarly_ = false;
            lastReactMs_   = -1.0;
            // xorshift，避免引入 <random> 的开销与不确定性
            reactSeed_ ^= reactSeed_ << 13;
            reactSeed_ ^= reactSeed_ >> 17;
            reactSeed_ ^= reactSeed_ << 5;
            const double waitMs = 900.0 + (reactSeed_ % 2100u);
            reactArmAtQpc_ = timing::Now() + timing::MsToTicks(waitMs);
        }
        else if (reactState_ == React::Waiting)
        {
            reactState_    = React::Idle;
            reactArmAtQpc_ = 0;
            reactTooEarly_ = true;
        }
        else if (reactState_ == React::Ready)
        {
            if (reactGreenQpc_ != 0)
            {
                lastReactMs_ = timing::Ms(reactGreenQpc_, in);
                if (lastReactMs_ > 0.0) reactStats_.Push(lastReactMs_, 20);
            }
            reactState_    = React::Idle;
            reactTooEarly_ = false;
        }
    }

    const ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg, fg;
    const char* line1;
    char line2[128] = "";

    switch (reactState_)
    {
    case React::Waiting:
        bg = ImVec4(0.102f, 0.129f, 0.188f, 1.0f);
        fg = kMuted;
        line1 = "等待变绿……";
        break;
    case React::Ready:
        bg = ImVec4(0.063f, 0.725f, 0.506f, 1.0f);
        fg = ImVec4(0.020f, 0.180f, 0.133f, 1.0f);
        line1 = "变绿了！快点击！";
        break;
    default:
        bg = ImVec4(0.090f, 0.125f, 0.161f, 1.0f);
        fg = ImVec4(0.902f, 0.929f, 0.953f, 1.0f);
        if (reactTooEarly_)        line1 = "太早了！等变绿再点";
        else if (lastReactMs_ > 0) line1 = "";
        else                       line1 = "点击开始测试";
        break;
    }

    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(bg), 14.0f);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.141f, 0.188f, 0.235f, 1.0f)), 14.0f, 0, 2.0f);

    if (reactState_ == React::Idle && lastReactMs_ > 0.0)
    {
        char big[64];
        std::snprintf(big, sizeof(big), "%.0f ms", lastReactMs_);
        ImGui::PushFont(fonts::g_mono, ImGui::GetStyle().FontSizeBase * 2.6f);
        const ImVec2 bs = ImGui::CalcTextSize(big);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p0.x + (w - bs.x) * 0.5f, p0.y + h * 0.5f - bs.y * 0.75f),
                    ImGui::GetColorU32(kAccent), big);
        ImGui::PopFont();
        std::snprintf(line2, sizeof(line2), "你的反应时间 · 点击重新开始");
        const ImVec2 s2 = ImGui::CalcTextSize(line2);
        dl->AddText(ImVec2(p0.x + (w - s2.x) * 0.5f, p0.y + h * 0.5f + s2.y * 1.1f),
                    ImGui::GetColorU32(kMuted), line2);
    }
    else
    {
        ImGui::PushFont(fonts::g_text, ImGui::GetStyle().FontSizeBase * 1.5f);
        const ImVec2 s1 = ImGui::CalcTextSize(line1);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p0.x + (w - s1.x) * 0.5f, p0.y + h * 0.5f - s1.y * 0.5f),
                    ImGui::GetColorU32(fg), line1);
        ImGui::PopFont();
    }

    if (!reactStats_.Empty())
    {
        ImGui::PushFont(fonts::g_mono, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("最近 %d 次   best %.0f   avg %.0f",
                    static_cast<int>(reactStats_.v.size()),
                    reactStats_.Min(), reactStats_.Avg());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine(0, 16);
        if (ImGui::SmallButton("清空##react")) reactStats_.Clear();
    }

    ImGui::Spacing();
}

void App::DrawReferencePanel()
{
    ImGui::SeparatorText("5 · 毫秒参考表");

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;

    if (ImGui::CollapsingHeader("体感对照", ImGuiTreeNodeFlags_DefaultOpen))
    {
        struct Row { const char* ms; const char* desc; double v; };
        static const Row rows[] = {
            { "0–1 ms",  "即时。本地内存/总线级操作，物理上几乎为零", 0.5 },
            { "5 ms",    "几乎即时，人类通常无法单独察觉", 5 },
            { "8.3 ms",  "120Hz 显示器刷新一帧的时间", 8.3 },
            { "16.7 ms", "60Hz 显示器刷新一帧的时间", 16.7 },
            { "33 ms",   "60Hz 下两帧，轻微迟滞；也是 30 FPS 的单帧预算", 33 },
            { "50 ms",   "反馈依然很快，但竞技游戏与直接操控中已可能被察觉", 50 },
            { "100 ms",  "普通点击仍显得流畅；连续操控会明显感觉到滞后", 100 },
            { "200 ms",  "明显延迟，接近人类简单视觉反应时间", 200 },
            { "400 ms",  "交互明显「不跟手」，用户容易重复点击", 400 },
            { "800 ms",  "等待感强，通常需要加载状态或进度反馈", 800 },
            { "1 s+",    "思路容易被打断；必须明确告知系统仍在工作", 1000 },
        };
        if (ImGui::BeginTable("##feel", 2, flags))
        {
            ImGui::TableSetupColumn("延迟", ImGuiTableColumnFlags_WidthFixed,
                                    110.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::TableSetupColumn("直观感受 / 现实参照");
            ImGui::TableHeadersRow();
            for (const Row& r : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushFont(fonts::g_mono, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, fonts::LatencyColor(r.v));
                ImGui::TextUnformatted(r.ms);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                ImGui::TextUnformatted(r.desc);
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("常见技术栈 / 场景延迟参考"))
    {
        TextMuted("「优秀 / 可接受」是经验目标，不代表某种框架的固定性能。真正应比较的是"
                  "同一设备、同一网络、同一测量起止点。");
        ImGui::Spacing();

        struct SRow { const char* kind; const char* scope; const char* good;
                      const char* ok; const char* note; };
        static const SRow rows[] = {
            { "Web",  "原生 / React / Vue 本地点击反馈\n输入 → 下一帧可见", "≤ 50 ms", "50–100 ms",
              "> 100 ms 开始显迟；框架通常不是主要瓶颈，长任务和重渲染更关键" },
            { "Web",  "SPA 路由切换\n点击 → 主要内容可交互", "≤ 200 ms", "200–500 ms",
              "> 500 ms 应显示骨架屏；含数据请求时需单独观察网络与服务端" },
            { "Web",  "网页首次打开\n导航 → 主要内容可见", "≤ 1.0 s", "1.0–2.5 s",
              "> 2.5 s 体感偏慢；更适合用 LCP、INP 等指标评估" },
            { "App",  "iOS / Android 原生 UI\n触摸 → 画面反馈", "≤ 50 ms", "50–100 ms",
              "> 100 ms 可感知；动画需稳定满足 60/90/120Hz 帧预算" },
            { "App",  "Flutter / RN 页面交互\n触摸 → 下一帧可见", "≤ 70 ms", "70–120 ms",
              "> 120 ms 显迟；跨端本身不等于慢，JS/UI 线程阻塞更关键" },
            { "App",  "App 冷启动\n点击图标 → 首屏可用", "≤ 1.0 s", "1–2 s",
              "> 2 s 等待感明显；低端机通常需要单独设定基线" },
            { "桌面", "Win32 / Cocoa / Qt / WPF\n输入 → 本地 UI 反馈", "≤ 30 ms", "30–80 ms",
              "> 100 ms 不再像原生即时反馈；Electron/Tauri 也可进入优秀区间" },
            { "游戏", "本地游戏端到端输入延迟\n按键 → 屏幕出现结果", "≤ 40 ms", "40–80 ms",
              "> 100 ms 明显影响操控；设备、引擎、帧率、垂直同步和显示器都会参与" },
            { "游戏", "在线竞技游戏 Ping\n客户端 ↔ 服务器 RTT", "≤ 30 ms", "30–80 ms",
              "> 100 ms 明显吃亏；Ping 不是按键到画面的完整延迟" },
            { "游戏", "云游戏\n输入 → 编码传输 → 屏幕结果", "≤ 80 ms", "80–150 ms",
              "> 150 ms 多数动作游戏手感较差；画质和抖动同样重要" },
            { "网络", "同城 API 请求\n发出请求 → 收到完整响应", "≤ 100 ms", "100–300 ms",
              "> 500 ms 需加载反馈；包含 DNS/TLS、网络 RTT 和服务端处理" },
            { "实时", "语音通话单向延迟\n说话 → 对方听到", "≤ 150 ms", "150–300 ms",
              "> 300 ms 容易抢话；不要与网络 RTT 混用" },
        };

        if (ImGui::BeginTable("##stacks", 5, flags))
        {
            const float dpi = ImGui::GetStyle().FontScaleDpi;
            ImGui::TableSetupColumn("类别",  ImGuiTableColumnFlags_WidthFixed, 56.0f * dpi);
            ImGui::TableSetupColumn("场景与测量口径", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("优秀",  ImGuiTableColumnFlags_WidthFixed, 84.0f * dpi);
            ImGui::TableSetupColumn("可接受", ImGuiTableColumnFlags_WidthFixed, 96.0f * dpi);
            ImGui::TableSetupColumn("偏慢 / 说明", ImGuiTableColumnFlags_WidthStretch, 2.6f);
            ImGui::TableHeadersRow();
            for (const SRow& r : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                ImGui::TextUnformatted(r.kind);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.scope);
                ImGui::TableSetColumnIndex(2);
                ImGui::PushFont(fonts::g_mono, 0.0f); ImGui::TextUnformatted(r.good); ImGui::PopFont();
                ImGui::TableSetColumnIndex(3);
                ImGui::PushFont(fonts::g_mono, 0.0f); ImGui::TextUnformatted(r.ok); ImGui::PopFont();
                ImGui::TableSetColumnIndex(4);
                ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
                ImGui::TextWrapped("%s", r.note);
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("测量口径与已知边界"))
    {
        TextMuted(
            "起点：WM_POINTERDOWN 的 POINTER_INFO.PerformanceCount —— 由输入栈在硬件事件到达时打的 QPC 时间戳，"
            "早于消息投递。若该 API 不可用，退化为 WM_LBUTTONDOWN 的到达时刻（会偏小）。\n\n"
            "终点：DXGI_FRAME_STATISTICS.SyncQPCTime —— 承载这次视觉变化的那一帧所对应的 vblank 时刻。"
            "这是操作系统能提供的、最接近「像素真的出现在屏幕上」的时刻。\n\n"
            "仍然测不到的部分：鼠标自身的采样与去抖（1000Hz 鼠标约 1ms，125Hz 约 8ms）、"
            "USB 传输、以及显示器内部的 overdrive / 缩放 / 面板响应（通常还有 3–15ms）。"
            "任何纯软件方案都到不了真正的 photon —— 要 ground truth 只能用 LDAT 或高速摄像机。\n\n"
            "交叉验证：可用 Intel PresentMon 2.x 对照，它的 Click-to-Photon 指标口径与此接近。\n\n"
            "「不等 vsync 立即上屏」打开后，SyncQPCTime 仍报告所属的 vblank，因此该模式下"
            "读数会略微高估真实的撕裂式上屏时刻 —— 但两种模式之间的差值仍然是有意义的对比。");
    }

    ImGui::Spacing();
    ImGui::Spacing();
}
