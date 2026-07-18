#include "ThemeManager.hpp"
#include "core/Config.hpp"
#include "ui/Formatting.hpp"
#include <imgui.h>

void ThemeManager::Apply(const ConfigData& cfg, float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    float scale = dpiScale;
    if (scale < 0.5f) scale = 1.0f;

    style.WindowRounding = 12.0f * scale;
    style.FrameRounding = 6.0f * scale;
    style.PopupRounding = 8.0f * scale;
    style.ChildRounding = 8.0f * scale;
    style.ScrollbarRounding = 12.0f * scale;
    style.GrabRounding = 6.0f * scale;

    style.WindowPadding = ImVec2(16.0f * scale, 16.0f * scale);
    style.FramePadding = ImVec2(8.0f * scale, 4.0f * scale);
    style.ItemSpacing = ImVec2(8.0f * scale, 6.0f * scale);

    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f * scale;
    style.ChildBorderSize = 1.0f * scale;
    style.CellPadding = ImVec2(4.0f * scale, 2.0f * scale);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(Format::C(cfg.themeBg));
    colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.70f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.95f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.25f, 0.50f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.28f, 0.33f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(Format::C(cfg.themeAccent));
    colors[ImGuiCol_SliderGrab] = ImVec4(Format::C(cfg.themeAccent));
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.70f, 1.00f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(Format::C(cfg.themeAccent));
    colors[ImGuiCol_Header] = ImVec4(0.15f, 0.18f, 0.22f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.25f, 0.30f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(Format::C(cfg.themeAccent));
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.22f, 0.25f, 0.50f);
    colors[ImGuiCol_Text] = ImVec4(Format::C(cfg.themeText));
    colors[ImGuiCol_TextDisabled] = ImVec4(Format::C(cfg.themeMuted));

    auto& t = cfg;
    ImVec4 bg = Format::C(t.themeBg);
    ImVec4 text = Format::C(t.themeText);
    ImVec4 accent = Format::C(t.themeAccent);
    ImVec4 dim = Format::C(t.themeDim);
    ImVec4 muted = Format::C(t.themeMuted);

    auto brighten = [](const ImVec4& c, float f) -> ImVec4 {
        return ImVec4(fminf(c.x * f, 1.0f), fminf(c.y * f, 1.0f), fminf(c.z * f, 1.0f), c.w);
    };
    auto darken = [](const ImVec4& c, float f) -> ImVec4 {
        return ImVec4(c.x * f, c.y * f, c.z * f, c.w);
    };

    style.Colors[ImGuiCol_WindowBg] = bg;
    style.Colors[ImGuiCol_PopupBg] = ImVec4(bg.x, bg.y, bg.z, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(muted.x, muted.y, muted.z, 0.3f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    style.Colors[ImGuiCol_Text] = text;
    style.Colors[ImGuiCol_TextDisabled] = dim;

    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.8f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(accent.x * 0.2f, accent.y * 0.2f, accent.z * 0.2f, 0.5f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(accent.x * 0.3f, accent.y * 0.3f, accent.z * 0.3f, 0.7f);

    style.Colors[ImGuiCol_CheckMark] = accent;

    style.Colors[ImGuiCol_SliderGrab] = accent;
    style.Colors[ImGuiCol_SliderGrabActive] = brighten(accent, 1.3f);

    style.Colors[ImGuiCol_Button] = ImVec4(accent.x * 0.2f, accent.y * 0.2f, accent.z * 0.2f, 0.7f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(accent.x * 0.4f, accent.y * 0.4f, accent.z * 0.4f, 0.8f);
    style.Colors[ImGuiCol_ButtonActive] = accent;

    style.Colors[ImGuiCol_Header] = ImVec4(accent.x * 0.2f, accent.y * 0.2f, accent.z * 0.2f, 0.5f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x * 0.35f, accent.y * 0.35f, accent.z * 0.35f, 0.7f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(accent.x * 0.5f, accent.y * 0.5f, accent.z * 0.5f, 0.9f);

    style.Colors[ImGuiCol_Separator] = ImVec4(muted.x, muted.y, muted.z, 0.4f);
    style.Colors[ImGuiCol_SeparatorHovered] = accent;
    style.Colors[ImGuiCol_SeparatorActive] = accent;

    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.4f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(muted.x, muted.y, muted.z, 0.5f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = dim;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = accent;

    style.Colors[ImGuiCol_TitleBg] = darken(bg, 0.8f);
    style.Colors[ImGuiCol_TitleBgActive] = darken(bg, 0.9f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = darken(bg, 0.6f);

    style.Colors[ImGuiCol_Tab] = darken(accent, 0.4f);
    style.Colors[ImGuiCol_TabHovered] = accent;
    style.Colors[ImGuiCol_TabActive] = brighten(accent, 0.8f);
    style.Colors[ImGuiCol_TabUnfocused] = darken(accent, 0.3f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = darken(accent, 0.5f);
}
