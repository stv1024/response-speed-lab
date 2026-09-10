// 字体与配色。ImGui 1.92 起支持动态字形加载，中文无需预声明 glyph ranges。
#pragma once
#include <windows.h>
#include <cstdio>
#include "imgui.h"

namespace fonts {

// 全局字体句柄，供 UI 切换字号/等宽使用。
inline ImFont* g_text = nullptr;
inline ImFont* g_mono = nullptr;

inline bool FileExists(const char* p)
{
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline void Load(float dpiScale)
{
    ImGuiIO& io = ImGui::GetIO();

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);

    auto sysFont = [&](const char* name, char* out, size_t n) -> bool {
        std::snprintf(out, n, "%s\\Fonts\\%s", winDir, name);
        return FileExists(out);
    };

    const float base = 17.0f;
    char path[MAX_PATH]{};

    // 正文：优先微软雅黑，退到等线，再退到 ImGui 内置（无中文）。
    if (sysFont("msyh.ttc", path, sizeof(path)) || sysFont("Deng.ttf", path, sizeof(path)))
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        g_text = io.Fonts->AddFontFromFileTTF(path, base, &cfg);
    }
    if (!g_text)
        g_text = io.Fonts->AddFontDefault();

    // 等宽：数字读数专用，避免毫秒数跳动时宽度抖动。
    if (sysFont("consola.ttf", path, sizeof(path)))
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        g_mono = io.Fonts->AddFontFromFileTTF(path, base, &cfg);

        // Consolas 没有中文字形。把中文字体并进来做兜底，
        // 否则等宽读数里夹带的中文会渲染成问号。
        if (g_mono && (sysFont("msyh.ttc", path, sizeof(path)) ||
                       sysFont("Deng.ttf", path, sizeof(path))))
        {
            ImFontConfig merge;
            merge.MergeMode   = true;
            merge.OversampleH = 2;
            merge.OversampleV = 1;
            io.Fonts->AddFontFromFileTTF(path, base, &merge);
        }
    }
    if (!g_mono)
        g_mono = g_text;

    io.FontDefault = g_text;
    ImGui::GetStyle().FontSizeBase = base;
    ImGui::GetStyle().FontScaleDpi = dpiScale;
}

inline void ApplyStyle(float dpiScale)
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    s.WindowRounding    = 0.0f;
    s.FrameRounding     = 8.0f;
    s.GrabRounding      = 8.0f;
    s.ChildRounding     = 12.0f;
    s.PopupRounding     = 8.0f;
    s.WindowPadding     = ImVec2(20, 16);
    s.FramePadding      = ImVec2(12, 8);
    s.ItemSpacing       = ImVec2(10, 9);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.CellPadding       = ImVec2(10, 7);
    s.ScrollbarSize     = 13.0f;
    s.ScrollbarRounding = 8.0f;
    s.SeparatorTextBorderSize = 2.0f;
    s.SeparatorTextPadding    = ImVec2(18, 6);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = ImVec4(0.043f, 0.059f, 0.078f, 1.00f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.071f, 0.094f, 0.122f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.071f, 0.094f, 0.122f, 0.98f);
    c[ImGuiCol_Border]              = ImVec4(0.141f, 0.188f, 0.235f, 1.00f);
    c[ImGuiCol_FrameBg]             = ImVec4(0.090f, 0.125f, 0.161f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.129f, 0.176f, 0.220f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.157f, 0.212f, 0.263f, 1.00f);
    c[ImGuiCol_TitleBg]             = ImVec4(0.043f, 0.059f, 0.078f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.043f, 0.059f, 0.078f, 1.00f);
    c[ImGuiCol_Text]                = ImVec4(0.902f, 0.929f, 0.953f, 1.00f);
    c[ImGuiCol_TextDisabled]        = ImVec4(0.545f, 0.596f, 0.647f, 1.00f);
    c[ImGuiCol_Button]              = ImVec4(0.102f, 0.145f, 0.184f, 1.00f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.145f, 0.200f, 0.251f, 1.00f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.180f, 0.243f, 0.302f, 1.00f);
    c[ImGuiCol_Header]              = ImVec4(0.102f, 0.145f, 0.184f, 1.00f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.145f, 0.200f, 0.251f, 1.00f);
    c[ImGuiCol_Separator]           = ImVec4(0.141f, 0.188f, 0.235f, 1.00f);
    c[ImGuiCol_SliderGrab]          = ImVec4(0.133f, 0.827f, 0.933f, 1.00f);
    c[ImGuiCol_SliderGrabActive]    = ImVec4(0.400f, 0.910f, 0.976f, 1.00f);
    c[ImGuiCol_CheckMark]           = ImVec4(0.133f, 0.827f, 0.933f, 1.00f);
    c[ImGuiCol_TableHeaderBg]       = ImVec4(0.090f, 0.125f, 0.161f, 1.00f);
    c[ImGuiCol_TableBorderStrong]   = ImVec4(0.141f, 0.188f, 0.235f, 1.00f);
    c[ImGuiCol_TableBorderLight]    = ImVec4(0.114f, 0.153f, 0.192f, 1.00f);
    c[ImGuiCol_TableRowBg]          = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1.000f, 1.000f, 1.000f, 0.018f);
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.043f, 0.059f, 0.078f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.141f, 0.188f, 0.235f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.200f, 0.251f, 0.302f, 1.00f);

    s.ScaleAllSizes(dpiScale);
}

// 语义色：按毫秒数分档
inline ImVec4 LatencyColor(double ms)
{
    if (ms < 0.0)  return ImVec4(0.545f, 0.596f, 0.647f, 1.0f); // 无数据
    if (ms < 5.0)  return ImVec4(0.204f, 0.827f, 0.600f, 1.0f); // fast
    if (ms < 16.7) return ImVec4(0.133f, 0.827f, 0.933f, 1.0f); // good
    if (ms < 33.4) return ImVec4(0.984f, 0.749f, 0.141f, 1.0f); // ok
    if (ms < 100.) return ImVec4(0.984f, 0.573f, 0.235f, 1.0f); // slow
    return ImVec4(0.973f, 0.443f, 0.443f, 1.0f);                // bad
}

} // namespace fonts
