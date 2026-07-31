// StatusOverlay.h — 左上角状态显示
//
// 在游戏画面上绘制当前状态、循环计数、错误信息和最近日志。
// 用 ImGui 的后台 overlay 模式（半透明背景）。
#pragma once

#include "../flow/Diagnostics.h"
#include "../flow/StateMachine.h"

#include <imgui.h>
#include <string>

namespace TabletReforgeUi {

inline void DrawStatusOverlay(TabletReforgeFlow::StateMachine& sm) {
    // 左上角 overlay
    const float dist = 10.0f;
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;

    ImGui::SetNextWindowPos(ImVec2(dist, dist), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("TabletReforgeStatus", nullptr, flags)) {
        ImGui::TextUnformatted("重铸台合成自动化");

        // 状态
        const char* stateName = TabletReforgeFlow::StateName(sm.CurrentState());
        const bool running = sm.IsRunning();
        if (running) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        } else if (!sm.LastError().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        }
        ImGui::Text("状态: %s", stateName);
        ImGui::PopStyleColor();

        // 循环计数
        if (running) {
            ImGui::Text("已完成循环: %d", sm.LoopCount());
        }

        // 测试模式指示
        if (running && sm.IsTestMode()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            ImGui::TextUnformatted("[测试模式] 跑 1 轮后自动停止");
            ImGui::PopStyleColor();
        }

        // 错误信息
        if (!sm.LastError().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("错误: %s", sm.LastError().c_str());
            ImGui::PopStyleColor();
            if (sm.CurrentState() == TabletReforgeFlow::State::ErrorWait) {
                if (ImGui::SmallButton("清除错误##clear_err")) {
                    sm.ClearError();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("  或按 F6 重新启动");
            }
        }

        // 最近日志（最多 8 条）
        ImGui::Separator();
        ImGui::TextUnformatted("最近日志:");
        auto logs = sm.diag.Recent(8);
        for (const auto& entry : logs) {
            ImVec4 color(0.7f, 0.7f, 0.7f, 1.0f);
            if (entry.severity == 1) color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
            else if (entry.severity == 2) color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("[%lldms] %s", entry.timestampMs, entry.message.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

} // namespace TabletReforgeUi
