#pragma once
#include <imgui.h>

namespace Widgets {

    struct FontGuard {
        explicit FontGuard(ImFont* font) {
            if (font) {
                ImGui::PushFont(font);
                m_active = true;
            }
        }
        ~FontGuard() {
            if (m_active) {
                ImGui::PopFont();
            }
        }
        FontGuard(const FontGuard&) = delete;
        FontGuard& operator=(const FontGuard&) = delete;

      private:
        bool m_active = false;
    };

    struct StyleVarGuard {
        explicit StyleVarGuard(ImGuiStyleVar idx, float val) {
            ImGui::PushStyleVar(idx, val);
            m_count++;
        }
        explicit StyleVarGuard(ImGuiStyleVar idx, const ImVec2& val) {
            ImGui::PushStyleVar(idx, val);
            m_count++;
        }
        ~StyleVarGuard() {
            if (m_count > 0) {
                ImGui::PopStyleVar(m_count);
            }
        }
        void Push(ImGuiStyleVar idx, float val) {
            ImGui::PushStyleVar(idx, val);
            m_count++;
        }
        void Push(ImGuiStyleVar idx, const ImVec2& val) {
            ImGui::PushStyleVar(idx, val);
            m_count++;
        }
        StyleVarGuard(const StyleVarGuard&) = delete;
        StyleVarGuard& operator=(const StyleVarGuard&) = delete;

      private:
        int m_count = 0;
    };

} // namespace Widgets
