#include "ToggleWidget.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include "core/Config.hpp" // For theme colors if needed

namespace Widgets {

bool ToggleButton(const char* str_id, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float height = ImGui::GetFrameHeight();
    float width = height * 1.55f;
    float radius = height * 0.50f;

    ImGui::InvisibleButton(str_id, ImVec2(width, height));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    float t = *v ? 1.0f : 0.0f;
    ImGuiContext& g = *GImGui;
    float ANIM_SPEED = 0.08f;
    if (g.LastActiveId == g.CurrentWindow->GetID(str_id)) {
        float t_anim = ImGui::GetStateStorage()->GetFloat(ImGui::GetID(str_id), t);
        t_anim += (*v ? 1.0f : -1.0f) * g.IO.DeltaTime * ANIM_SPEED * 100.0f;
        if (t_anim < 0.0f) t_anim = 0.0f;
        if (t_anim > 1.0f) t_anim = 1.0f;
        ImGui::GetStateStorage()->SetFloat(ImGui::GetID(str_id), t_anim);
        t = t_anim;
    }

    ImU32 col_bg;
    if (ImGui::IsItemHovered())
        col_bg = ImGui::GetColorU32(ImLerp(ImVec4(0.78f, 0.78f, 0.78f, 1.0f), ImVec4(0.40f, 0.90f, 0.40f, 1.0f), t));
    else
        col_bg = ImGui::GetColorU32(ImLerp(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), ImVec4(0.56f, 0.83f, 0.26f, 1.0f), t));

    draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg, height * 0.5f);
    draw_list->AddCircleFilled(ImVec2(p.x + radius + t * (width - radius * 2.0f), p.y + radius), radius - 1.5f, IM_COL32(255, 255, 255, 255));

    // Render text label
    const char* label_end = ImGui::FindRenderedTextEnd(str_id);
    if (label_end != str_id) {
        ImGui::SameLine();
        ImVec2 text_pos = ImGui::GetCursorScreenPos();
        // Center text vertically with the button
        text_pos.y += (height - ImGui::GetTextLineHeight()) * 0.5f;
        draw_list->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), str_id, label_end);
        ImGui::Dummy(ImVec2(ImGui::CalcTextSize(str_id, label_end).x, 0.0f)); // Advance cursor horizontally
    }

    return clicked;
}

} // namespace Widgets
