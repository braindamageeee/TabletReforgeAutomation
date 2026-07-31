// CalibOverlay.h — 标定辅助工具（完整实现）
//
// 五步式标定向导，自动扫描实体和 UI 节点，支持 F7 热键捕获鼠标坐标：
//   1. 标定重铸台实体（扫描附近可交互实体，选一个）
//   2. 标定仓库实体（可选，扫描附近实体）
//   3. 标定合成按钮（扫描 UI 树 或 F7 捕获坐标）
//   4. 标定产物槽（扫描 UI 树 或 F7 捕获坐标）
//   5. 检查并保存
//
// 使用方式：
//   - 在设置面板点"打开标定向导"
//   - 按向导提示操作
//   - 标定完成后自动写入 calib.json
#pragma once

#include "../config/CalibData.h"
#include "../game/ReformatoryFinder.h"
#include "../game/StashOps.h"
#include "../game/UiTreeWalker.h"
#include "../sdk/PluginSDK.h"

#include <imgui.h>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace TabletReforgeUi {

// 宽字符串转 UTF-8 窄字符串（用于显示实体 Path）
inline std::string WStringToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                           static_cast<int>(w.size()),
                           s.data(), needed, nullptr, nullptr);
    return s;
}

// 实体类型显示名
inline const char* EntityTypeLabel(PluginSDK::EntityType t) {
    switch (t) {
        case PluginSDK::EntityType::Unidentified:      return "未识别";
        case PluginSDK::EntityType::Chest:              return "箱子";
        case PluginSDK::EntityType::NPC:                return "NPC";
        case PluginSDK::EntityType::Player:             return "玩家";
        case PluginSDK::EntityType::Shrine:             return "神龛";
        case PluginSDK::EntityType::Monster:            return "怪物";
        case PluginSDK::EntityType::DeliriumBomb:       return "谵妄炸弹";
        case PluginSDK::EntityType::DeliriumSpawner:    return "谵妄生成器";
        case PluginSDK::EntityType::OtherImportant:     return "重要物";
        case PluginSDK::EntityType::Item:               return "物品";
        case PluginSDK::EntityType::Renderable:         return "可渲染";
        case PluginSDK::EntityType::AreaTransition:     return "传送";
        case PluginSDK::EntityType::ExpeditionMarker:   return "远征标记";
        case PluginSDK::EntityType::ExpeditionRemnant:  return "远征遗物";
        default:                                        return "未知";
    }
}

// F7 捕获目标
enum class CaptureTarget {
    None,
    CombineButton,
    OutputSlot,
    IdentifyButton,  // NPC 鉴定按钮（多利亚尼）
};

// 标定向导状态
struct CalibWizardState {
    int step = 0; // 0=欢迎, 1=重铸台, 2=仓库, 3=合成按钮, 4=产物槽, 5=NPC鉴定, 6=完成
    bool active = false;

    // 实体扫描结果
    std::vector<TabletReforgeGame::NearbyEntity> entities;
    int selectedEntityIdx = -1;
    bool scanEntitiesRequested = false;
    int entityFilterType = 0;  // 0=全部, 1=箱子, 2=NPC, 3=重要物, 4=神龛, 5=物品, 6=可渲染, 7=传送

    // UI 节点扫描结果
    std::vector<TabletReforgeGame::UiNodeInfo> uiNodes;
    int selectedNodeIdx = -1;
    bool scanUiRequested = false;
    char searchFilter[256] = {};

    // F7 鼠标捕获
    CaptureTarget captureTarget = CaptureTarget::None;
    int captureCountdown = 0; // 倒计时帧数（给用户时间移动鼠标）

    // 实时鼠标坐标
    int cursorX = 0;
    int cursorY = 0;

    // 保存标志
    bool calibSaved = false;

    // 测试请求（主入口检测后调用 StartTest）
    bool testRequested = false;

    // 仓库物品诊断面板
    bool diagPanelOpen = false;
};

// —— 实体列表渲染（步骤 1/2 共用）——
// targetBench=true 标定重铸台，false 标定仓库
inline void DrawEntityScanStep(CalibWizardState& wizard,
                                TabletReforgeConfig::CalibData& calib,
                                const PluginSDK::Context* ctx,
                                bool targetBench) {
    const char* title = targetBench ? "步骤 1：标定重铸台实体"
                                     : "步骤 2：标定仓库实体（可选）";
    ImGui::TextUnformatted(title);

    if (targetBench) {
        ImGui::TextWrapped("请站在藏身处重铸台附近，点击扫描。"
                           "列表会显示附近所有可交互实体，选一个作为重铸台。");
    } else {
        ImGui::TextWrapped("仓库实体标定可选。如果留空，每次循环回仓库时需手动打开。"
                           "要标定的话，走到仓库附近扫描。");
    }

    // 当前已标定的值
    const std::string& currentPath = targetBench ? calib.benchEntityPath : calib.stashEntityPath;
    if (!currentPath.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("当前: %s", currentPath.c_str());
        ImGui::PopStyleColor();
    }

    // 类型过滤下拉
    const char* filterItems[] = {
        "全部类型", "箱子", "NPC", "重要物", "神龛", "物品", "可渲染", "传送"
    };
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("类型过滤##entity_filter", &wizard.entityFilterType, filterItems,
                 IM_ARRAYSIZE(filterItems));
    ImGui::SameLine();

    // 扫描按钮
    if (ImGui::Button("扫描附近实体##scan_entities")) {
        wizard.scanEntitiesRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("清空列表##clear_entities")) {
        wizard.entities.clear();
        wizard.selectedEntityIdx = -1;
    }

    // 实体列表
    if (!wizard.entities.empty()) {
        ImGui::Separator();
        ImGui::Text("找到 %zu 个实体（按距离排序）:", wizard.entities.size());

        ImGui::BeginChild("entity_list", ImVec2(0, 200), true);
        for (int i = 0; i < static_cast<int>(wizard.entities.size()); ++i) {
            const auto& e = wizard.entities[static_cast<size_t>(i)];
            char label[1024];
            std::snprintf(label, sizeof(label), "[%s] %s (距离 %.1f)",
                          EntityTypeLabel(e.type),
                          WStringToUtf8(e.path).c_str(),
                          e.distance);
            if (ImGui::Selectable(label, wizard.selectedEntityIdx == i)) {
                wizard.selectedEntityIdx = i;
            }
        }
        ImGui::EndChild();

        // 确认选择
        if (wizard.selectedEntityIdx >= 0 && wizard.selectedEntityIdx < static_cast<int>(wizard.entities.size())) {
            const auto& sel = wizard.entities[static_cast<size_t>(wizard.selectedEntityIdx)];
            if (ImGui::Button("确认选择##confirm_entity")) {
                std::string pathUtf8 = WStringToUtf8(sel.path);
                if (targetBench) {
                    calib.benchEntityPath = pathUtf8;
                } else {
                    calib.stashEntityPath = pathUtf8;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("已选: %s", WStringToUtf8(sel.path).c_str());
        }
    }

    // 导航按钮
    ImGui::Separator();
    int nextStep = targetBench ? 2 : 3;
    int prevStep = targetBench ? 0 : 1;
    if (ImGui::Button("下一步##entity_next")) wizard.step = nextStep;
    ImGui::SameLine();
    if (ImGui::Button("上一步##entity_prev")) wizard.step = prevStep;
    if (!targetBench) {
        ImGui::SameLine();
        if (ImGui::Button("跳过（不标定仓库）##skip_stash")) {
            calib.stashEntityPath.clear();
            wizard.step = 3;
        }
    }
}

// —— UI 节点扫描渲染（步骤 3/4 共用）——
// targetCombine=true 标定合成按钮，false 标定产物槽
inline void DrawUiScanStep(CalibWizardState& wizard,
                            TabletReforgeConfig::CalibData& calib,
                            const PluginSDK::Context* ctx,
                            bool targetCombine) {
    const char* title = targetCombine ? "步骤 3：标定合成按钮"
                                       : "步骤 4：标定产物槽";
    ImGui::TextUnformatted(title);

    if (targetCombine) {
        ImGui::TextWrapped("请先手动打开重铸台合成界面，放 3 个碑牌让合成按钮亮起，"
                           "然后点扫描。从列表找合成按钮并确认。");
    } else {
        ImGui::TextWrapped("请先合成一次让产物出现，然后扫描。从列表找产物槽并确认。");
    }

    // 当前已标定的值
    const auto& currentId = targetCombine ? calib.combineButtonStringId : calib.outputSlotStringId;
    const int curX = targetCombine ? calib.combineButtonX : calib.outputSlotX;
    const int curY = targetCombine ? calib.combineButtonY : calib.outputSlotY;
    if (!currentId.empty() || curX >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        if (!currentId.empty())
            ImGui::Text("当前 StringId: %s", currentId.c_str());
        if (curX >= 0)
            ImGui::Text("当前坐标: (%d, %d)", curX, curY);
        ImGui::PopStyleColor();
    }

    // 扫描 UI 按钮
    if (ImGui::Button("扫描 UI 节点##scan_ui")) {
        wizard.scanUiRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("清空##clear_ui")) {
        wizard.uiNodes.clear();
        wizard.selectedNodeIdx = -1;
    }

    // F7 捕获坐标
    ImGui::Separator();
    ImGui::TextUnformatted("或者用 F7 热键捕获鼠标坐标：");
    ImGui::SameLine();
    if (wizard.captureTarget == (targetCombine ? CaptureTarget::CombineButton : CaptureTarget::OutputSlot)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        ImGui::TextUnformatted("正在捕获... 移动鼠标到目标位置后按 F7");
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("开始 F7 捕获##start_capture")) {
            wizard.captureTarget = targetCombine ? CaptureTarget::CombineButton : CaptureTarget::OutputSlot;
        }
    }

    // 实时鼠标坐标
    POINT pt;
    if (::GetCursorPos(&pt)) {
        wizard.cursorX = pt.x;
        wizard.cursorY = pt.y;
    }
    ImGui::TextDisabled("当前鼠标坐标: (%d, %d)", wizard.cursorX, wizard.cursorY);

    // UI 节点列表
    if (!wizard.uiNodes.empty()) {
        ImGui::Separator();
        // 节点数过多时提示
        if (wizard.uiNodes.size() >= 5000) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            ImGui::TextWrapped("⚠ 节点数较多（%zu），滚动可能较慢。请用下方过滤框缩小范围。", wizard.uiNodes.size());
            ImGui::PopStyleColor();
        }
        ImGui::Text("找到 %zu 个 UI 节点:", wizard.uiNodes.size());

        // 搜索过滤
        ImGui::InputText("过滤##ui_filter", wizard.searchFilter, sizeof(wizard.searchFilter));

        ImGui::BeginChild("ui_node_list", ImVec2(0, 200), true);
        int displayIdx = 0;
        for (int i = 0; i < static_cast<int>(wizard.uiNodes.size()); ++i) {
            const auto& n = wizard.uiNodes[static_cast<size_t>(i)];

            // 过滤
            if (wizard.searchFilter[0] != '\0') {
                std::string filter = wizard.searchFilter;
                std::string idStr = n.stringId;
                std::string textStr = n.text;
                // 简单子串匹配（大小写不敏感）
                auto toLower = [](std::string s) {
                    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return s;
                };
                std::string fl = toLower(filter);
                std::string il = toLower(idStr);
                std::string tl = toLower(textStr);
                if (il.find(fl) == std::string::npos && tl.find(fl) == std::string::npos)
                    continue;
            }
            // 无过滤词时只显示前 200 个，防止卡死
            if (wizard.searchFilter[0] == '\0' && displayIdx >= 200) continue;

            char label[1024];
            std::snprintf(label, sizeof(label), "[%s] %s (%.0f,%.0f %.0fx%.0f)",
                          n.stringId.empty() ? "(无ID)" : n.stringId.c_str(),
                          n.text.empty() ? "(无文字)" : n.text.c_str(),
                          n.x, n.y, n.w, n.h);
            if (ImGui::Selectable(label, wizard.selectedNodeIdx == i)) {
                wizard.selectedNodeIdx = i;
            }
            ++displayIdx;
        }
        ImGui::EndChild();

        if (wizard.searchFilter[0] == '\0' && wizard.uiNodes.size() > 200) {
            ImGui::TextDisabled("（无过滤词时只显示前 200 个，请输入关键词缩小范围）");
        }

        // 确认选择
        if (wizard.selectedNodeIdx >= 0 && wizard.selectedNodeIdx < static_cast<int>(wizard.uiNodes.size())) {
            const auto& sel = wizard.uiNodes[static_cast<size_t>(wizard.selectedNodeIdx)];
            if (ImGui::Button("确认选择##confirm_ui")) {
                int cx = static_cast<int>(sel.x + sel.w * 0.5f);
                int cy = static_cast<int>(sel.y + sel.h * 0.5f);
                if (targetCombine) {
                    if (!sel.stringId.empty()) calib.combineButtonStringId = sel.stringId;
                    calib.combineButtonX = cx;
                    calib.combineButtonY = cy;
                } else {
                    if (!sel.stringId.empty()) calib.outputSlotStringId = sel.stringId;
                    calib.outputSlotX = cx;
                    calib.outputSlotY = cy;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("中心点: (%d, %d)",
                                static_cast<int>(sel.x + sel.w * 0.5f),
                                static_cast<int>(sel.y + sel.h * 0.5f));
        }
    }

    // 导航
    ImGui::Separator();
    int nextStep = targetCombine ? 4 : 5;
    int prevStep = targetCombine ? 2 : 3;
    if (ImGui::Button("下一步##ui_next")) wizard.step = nextStep;
    ImGui::SameLine();
    if (ImGui::Button("上一步##ui_prev")) wizard.step = prevStep;
}

// —— NPC 鉴定标定（步骤 5：多利亚尼）——
// 一次性标定三个字段：
//   1. NPC 实体 Path（扫描附近 NPC 实体）
//   2. NPC 对话面板 StringId（可选，UI 扫描或手动输入）
//   3. 鉴定按钮 StringId + 坐标（UI 扫描 或 F7 捕获）
inline void DrawNpcCalibStep(CalibWizardState& wizard,
                              TabletReforgeConfig::CalibData& calib,
                              const PluginSDK::Context* ctx) {
    ImGui::TextUnformatted("步骤 5：标定 NPC 鉴定（多利亚尼，可选）");
    ImGui::TextWrapped("当背包原料不足 3 个且有未鉴定的魔法/稀有碑牌时，"
                       "插件会自动找多利亚尼鉴定。本步全部可选，"
                       "不标定则不会触发自动鉴定流程。");

    // ========== 5.1 NPC 实体 Path ==========
    ImGui::Separator();
    ImGui::TextUnformatted("5.1 NPC 实体 Path");
    if (!calib.npcEntityPath.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("当前: %s", calib.npcEntityPath.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("未设置（自动鉴定将不可用）");
    }

    // 类型过滤强制为 NPC（提示用户）
    ImGui::TextDisabled("提示：类型过滤已自动设为 NPC，扫描会列出附近所有 NPC");
    ImGui::SameLine();
    if (ImGui::Button("扫描附近 NPC##scan_npc")) {
        wizard.entityFilterType = 2;  // NPC
        wizard.scanEntitiesRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("清空##clear_npc_entities")) {
        wizard.entities.clear();
        wizard.selectedEntityIdx = -1;
    }

    // 实体列表（复用 wizard.entities，但只显示 NPC 类型）
    if (!wizard.entities.empty()) {
        ImGui::BeginChild("npc_entity_list", ImVec2(0, 150), true);
        for (int i = 0; i < static_cast<int>(wizard.entities.size()); ++i) {
            const auto& e = wizard.entities[static_cast<size_t>(i)];
            // 只显示 NPC 类型
            if (e.type != PluginSDK::EntityType::NPC) continue;
            char label[1024];
            std::snprintf(label, sizeof(label), "[%s] %s (距离 %.1f)",
                          EntityTypeLabel(e.type),
                          WStringToUtf8(e.path).c_str(),
                          e.distance);
            if (ImGui::Selectable(label, wizard.selectedEntityIdx == i)) {
                wizard.selectedEntityIdx = i;
            }
        }
        ImGui::EndChild();

        if (wizard.selectedEntityIdx >= 0
            && wizard.selectedEntityIdx < static_cast<int>(wizard.entities.size())) {
            const auto& sel = wizard.entities[static_cast<size_t>(wizard.selectedEntityIdx)];
            if (sel.type == PluginSDK::EntityType::NPC) {
                if (ImGui::Button("确认选择为多利亚尼##confirm_npc")) {
                    calib.npcEntityPath = WStringToUtf8(sel.path);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("已选: %s", WStringToUtf8(sel.path).c_str());
            }
        }
    }

    // 手动输入 NPC Path
    char npcPathBuf[512] = {};
    std::strncpy(npcPathBuf, calib.npcEntityPath.c_str(), sizeof(npcPathBuf) - 1);
    if (ImGui::InputText("手动输入 NPC Path##npc_path_input",
                          npcPathBuf, sizeof(npcPathBuf))) {
        calib.npcEntityPath = npcPathBuf;
    }

    // ========== 5.2 NPC 对话面板 StringId（可选）==========
    ImGui::Separator();
    ImGui::TextUnformatted("5.2 NPC 对话面板 StringId（可选）");
    ImGui::TextWrapped("先在游戏里点击 NPC 打开对话界面，再扫描 UI 节点。"
                       "从列表中找对话面板的 StringId。");
    if (!calib.npcDialogStringId.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::Text("当前: %s", calib.npcDialogStringId.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("未设置（将使用兜底等待模式）");
    }

    if (ImGui::Button("扫描 UI 节点##scan_npc_dialog")) {
        wizard.scanUiRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("清空 UI 列表##clear_npc_dialog_ui")) {
        wizard.uiNodes.clear();
        wizard.selectedNodeIdx = -1;
    }

    // 手动输入对话 StringId
    char dialogIdBuf[256] = {};
    std::strncpy(dialogIdBuf, calib.npcDialogStringId.c_str(), sizeof(dialogIdBuf) - 1);
    if (ImGui::InputText("手动输入对话 StringId##dialog_id_input",
                          dialogIdBuf, sizeof(dialogIdBuf))) {
        calib.npcDialogStringId = dialogIdBuf;
    }

    // UI 节点列表（仅用于参考选择对话面板）—— 带搜索过滤，防止节点过多卡死
    if (!wizard.uiNodes.empty()) {
        // 节点数过多时提示
        if (wizard.uiNodes.size() >= 2000) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            ImGui::TextWrapped("⚠ 已达节点上限 2000（可能被截断）。请用下方过滤框缩小范围。");
            ImGui::PopStyleColor();
        }
        ImGui::Text("找到 %zu 个 UI 节点（输入关键词过滤后选择）:", wizard.uiNodes.size());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("过滤 (StringId 或文字)##npc_dialog_filter",
                          wizard.searchFilter, sizeof(wizard.searchFilter));

        // 过滤辅助
        auto toLower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::string fl = toLower(wizard.searchFilter);

        ImGui::BeginChild("npc_dialog_ui_list", ImVec2(0, 150), true);
        int shownCount = 0;
        for (int i = 0; i < static_cast<int>(wizard.uiNodes.size()); ++i) {
            const auto& n = wizard.uiNodes[static_cast<size_t>(i)];
            // 有过滤词时只显示匹配的节点
            if (!fl.empty()) {
                std::string il = toLower(n.stringId);
                std::string tl = toLower(n.text);
                if (il.find(fl) == std::string::npos && tl.find(fl) == std::string::npos)
                    continue;
            }
            // 无过滤词时只显示前 200 个，防止卡死
            if (fl.empty() && shownCount >= 200) continue;

            char label[1024];
            std::snprintf(label, sizeof(label), "[%s] %s (%.0f,%.0f %.0fx%.0f)",
                          n.stringId.empty() ? "(无ID)" : n.stringId.c_str(),
                          n.text.empty() ? "(无文字)" : n.text.c_str(),
                          n.x, n.y, n.w, n.h);
            if (ImGui::Selectable(label, wizard.selectedNodeIdx == i)) {
                wizard.selectedNodeIdx = i;
            }
            ++shownCount;
        }
        ImGui::EndChild();

        if (fl.empty() && wizard.uiNodes.size() > 200) {
            ImGui::TextDisabled("（无过滤词时只显示前 200 个，请输入关键词缩小范围）");
        }

        if (wizard.selectedNodeIdx >= 0
            && wizard.selectedNodeIdx < static_cast<int>(wizard.uiNodes.size())) {
            const auto& sel = wizard.uiNodes[static_cast<size_t>(wizard.selectedNodeIdx)];
            if (ImGui::Button("确认选择为对话面板##confirm_dialog")) {
                if (!sel.stringId.empty()) {
                    calib.npcDialogStringId = sel.stringId;
                }
            }
        }
    }

    // ========== 5.3 鉴定按钮 StringId（实时扫描） ==========
    ImGui::Separator();
    ImGui::TextUnformatted("5.3 鉴定按钮（实时扫描模式，无需坐标标定）");
    ImGui::TextWrapped("运行时会自动搜索 \"鑑定\" / \"Identify\" 等文字关键词。"
                       "如需更精确，可在 NPC 对话界面扫描 UI 节点选择 StringId。");
    if (!calib.identifyButtonStringId.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::Text("当前 StringId: %s", calib.identifyButtonStringId.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("未设置 StringId（将使用文字关键词自动搜索）");
    }

    // 扫描 UI 按钮（用于查找 StringId）
    ImGui::TextUnformatted("在 NPC 对话界面扫描 UI 节点以获取 StringId（可选）:");
    if (ImGui::Button("扫描 UI 节点##scan_identify_btn")) {
        wizard.scanUiRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("清空##clear_identify_ui")) {
        wizard.uiNodes.clear();
        wizard.selectedNodeIdx = -1;
    }

    // UI 节点列表（用于选鉴定按钮）—— 带搜索过滤
    if (!wizard.uiNodes.empty()) {
        if (wizard.uiNodes.size() >= 2000) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            ImGui::TextWrapped("⚠ 已达节点上限 2000（可能被截断）。请用下方过滤框缩小范围。");
            ImGui::PopStyleColor();
        }
        ImGui::Text("找到 %zu 个 UI 节点（输入关键词过滤后选择）:", wizard.uiNodes.size());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("过滤 (StringId 或文字)##npc_identify_filter",
                          wizard.searchFilter, sizeof(wizard.searchFilter));

        auto toLower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::string fl = toLower(wizard.searchFilter);

        ImGui::BeginChild("identify_btn_ui_list", ImVec2(0, 150), true);
        int shownCount = 0;
        for (int i = 0; i < static_cast<int>(wizard.uiNodes.size()); ++i) {
            const auto& n = wizard.uiNodes[static_cast<size_t>(i)];
            if (!fl.empty()) {
                std::string il = toLower(n.stringId);
                std::string tl = toLower(n.text);
                if (il.find(fl) == std::string::npos && tl.find(fl) == std::string::npos)
                    continue;
            }
            if (fl.empty() && shownCount >= 200) continue;

            char label[1024];
            std::snprintf(label, sizeof(label), "[%s] %s",
                          n.stringId.empty() ? "(无ID)" : n.stringId.c_str(),
                          n.text.empty() ? "(无文字)" : n.text.c_str());
            if (ImGui::Selectable(label, wizard.selectedNodeIdx == i)) {
                wizard.selectedNodeIdx = i;
            }
            ++shownCount;
        }
        ImGui::EndChild();

        if (fl.empty() && wizard.uiNodes.size() > 200) {
            ImGui::TextDisabled("（无过滤词时只显示前 200 个，请输入关键词缩小范围）");
        }

        if (wizard.selectedNodeIdx >= 0
            && wizard.selectedNodeIdx < static_cast<int>(wizard.uiNodes.size())) {
            const auto& sel = wizard.uiNodes[static_cast<size_t>(wizard.selectedNodeIdx)];
            if (ImGui::Button("确认选择为鉴定按钮##confirm_identify_btn")) {
                if (!sel.stringId.empty()) calib.identifyButtonStringId = sel.stringId;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("文字: %s", sel.text.empty() ? "(无)" : sel.text.c_str());
        }
    }

    // 手动输入鉴定按钮 StringId
    char identifyIdBuf[256] = {};
    std::strncpy(identifyIdBuf, calib.identifyButtonStringId.c_str(), sizeof(identifyIdBuf) - 1);
    if (ImGui::InputText("手动输入鉴定按钮 StringId##identify_id_input",
                          identifyIdBuf, sizeof(identifyIdBuf))) {
        calib.identifyButtonStringId = identifyIdBuf;
    }

    // ========== 导航 ==========
    ImGui::Separator();
    if (ImGui::Button("下一步##npc_next")) wizard.step = 6;
    ImGui::SameLine();
    if (ImGui::Button("上一步##npc_prev")) wizard.step = 4;
    ImGui::SameLine();
    if (ImGui::Button("跳过（不标定 NPC 鉴定）##skip_npc")) {
        calib.npcEntityPath.clear();
        calib.npcDialogStringId.clear();
        calib.identifyButtonStringId.clear();
        wizard.step = 6;
    }
}

// —— 主向导窗口 ——
inline void DrawCalibWizard(CalibWizardState& wizard,
                             TabletReforgeConfig::CalibData& calib,
                             const PluginSDK::Context* ctx) {
    if (!wizard.active) return;

    // 执行扫描请求（在渲染前执行，这样结果能立即显示）
    if (wizard.scanEntitiesRequested && ctx) {
        using ET = PluginSDK::EntityType;
        ET filterType = ET::Unidentified;
        switch (wizard.entityFilterType) {
            case 1: filterType = ET::Chest; break;
            case 2: filterType = ET::NPC; break;
            case 3: filterType = ET::OtherImportant; break;
            case 4: filterType = ET::Shrine; break;
            case 5: filterType = ET::Item; break;
            case 6: filterType = ET::Renderable; break;
            case 7: filterType = ET::AreaTransition; break;
            default: filterType = ET::Unidentified; break; // 全部
        }
        wizard.entities = TabletReforgeGame::ListNearbyEntities(ctx, 80.f, filterType);
        wizard.selectedEntityIdx = -1;
        wizard.scanEntitiesRequested = false;
    }
    if (wizard.scanUiRequested && ctx) {
        // 全量扫描：深度 20、无节点上限、最小尺寸 4px、要求有 ID 或文字
        wizard.uiNodes = TabletReforgeGame::CollectVisible(ctx, 20, 0, 4.f, true);
        wizard.selectedNodeIdx = -1;
        wizard.scanUiRequested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("标定辅助工具##CalibWizard", &wizard.active)) {
        // 顶部进度指示
        const char* stepNames[] = {"欢迎", "重铸台", "仓库", "合成按钮", "产物槽", "NPC鉴定", "完成"};
        for (int i = 0; i <= 6; ++i) {
            if (i > 0) ImGui::SameLine(0, 5);
            if (i == wizard.step) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                ImGui::Text("[%d. %s]", i, stepNames[i]);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("%d. %s", i, stepNames[i]);
            }
            if (i < 6) {
                ImGui::SameLine(0, 5);
                ImGui::TextDisabled("->");
            }
        }
        ImGui::Separator();

        switch (wizard.step) {
            case 0:
                ImGui::TextUnformatted("欢迎使用标定辅助工具");
                ImGui::TextWrapped("本工具会自动扫描游戏内的实体和 UI 节点，"
                                   "帮你完成标定。共 6 步：");
                ImGui::BulletText("1. 标定重铸台实体（扫描附近实体）");
                ImGui::BulletText("2. 标定仓库实体（可选，扫描附近实体）");
                ImGui::BulletText("3. 标定合成按钮（扫描 UI 或 F7 捕获）");
                ImGui::BulletText("4. 标定产物槽（扫描 UI 或 F7 捕获）");
                ImGui::BulletText("5. 标定 NPC 鉴定（可选，多利亚尼）");
                ImGui::BulletText("6. 检查并保存");
                ImGui::Spacing();
                ImGui::TextWrapped("提示：标定前请确保游戏已加载，"
                                   "角色在藏身处。每步都可以随时返回上一步修改。");
                ImGui::Spacing();
                if (ImGui::Button("开始标定##calib_start")) wizard.step = 1;
                ImGui::SameLine();
                if (ImGui::Button("诊断工具##diag_open")) {
                    wizard.diagPanelOpen = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(查看仓库物品路径)");
                break;

            case 1:
                DrawEntityScanStep(wizard, calib, ctx, true);
                break;

            case 2:
                DrawEntityScanStep(wizard, calib, ctx, false);
                break;

            case 3:
                DrawUiScanStep(wizard, calib, ctx, true);
                break;

            case 4:
                DrawUiScanStep(wizard, calib, ctx, false);
                break;

            case 5:
                DrawNpcCalibStep(wizard, calib, ctx);
                break;

            case 6:
                ImGui::TextUnformatted("步骤 6：检查并保存");
                ImGui::Separator();

                // 标定完整性
                if (calib.IsComplete()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                    ImGui::TextUnformatted("标定完整 ✓ 可以使用了");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextWrapped("标定不完整: %s", calib.MissingDescription().c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Separator();
                ImGui::TextUnformatted("当前标定值：");
                ImGui::Text("重铸台 Path: %s",
                            calib.benchEntityPath.empty() ? "(未设置)" : calib.benchEntityPath.c_str());
                ImGui::Text("仓库 Path: %s",
                            calib.stashEntityPath.empty() ? "(未设置)" : calib.stashEntityPath.c_str());
                ImGui::Text("合成按钮 StringId: %s",
                            calib.combineButtonStringId.empty() ? "(无)" : calib.combineButtonStringId.c_str());
                ImGui::Text("合成按钮坐标: (%d, %d)", calib.combineButtonX, calib.combineButtonY);
                ImGui::Text("产物槽 StringId: %s",
                            calib.outputSlotStringId.empty() ? "(无)" : calib.outputSlotStringId.c_str());
                ImGui::Text("产物槽坐标: (%d, %d)", calib.outputSlotX, calib.outputSlotY);
                ImGui::Separator();
                ImGui::TextDisabled("—— NPC 鉴定（可选）——");
                ImGui::Text("NPC Path: %s",
                            calib.npcEntityPath.empty() ? "(未设置)" : calib.npcEntityPath.c_str());
                ImGui::Text("对话 StringId: %s",
                            calib.npcDialogStringId.empty() ? "(无)" : calib.npcDialogStringId.c_str());
                ImGui::Text("鉴定按钮 StringId: %s",
                            calib.identifyButtonStringId.empty() ? "(无，使用文字搜索)" : calib.identifyButtonStringId.c_str());
                ImGui::TextDisabled("（鉴定按钮使用实时扫描，无需固定坐标）");

                ImGui::Separator();
                if (ImGui::Button("保存标定##save_calib")) {
                    // 保存由主插件处理（通过 SaveSettings），这里只标记
                    wizard.calibSaved = true;
                }
                ImGui::SameLine();
                if (wizard.calibSaved) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                    ImGui::TextUnformatted("已保存 ✓ (关闭向导时会写入 calib.json)");
                    ImGui::PopStyleColor();
                }

                // 测试标定按钮
                ImGui::Separator();
                if (calib.IsComplete()) {
                    ImGui::TextWrapped("标定完整，可以测试。点击下方按钮会跑 1 轮完整循环验证。");
                    if (ImGui::Button("测试标定（跑 1 轮）##test_calib")) {
                        wizard.testRequested = true;
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::TextWrapped("(标定不完整，无法测试。请先完成所有必填项)");
                    ImGui::PopStyleColor();
                }

                ImGui::Separator();
                if (ImGui::Button("完成并关闭##calib_done")) {
                    wizard.step = 0;
                    wizard.active = false;
                    wizard.calibSaved = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("上一步##calib_prev_final")) wizard.step = 5;
                break;
        }
    }
    ImGui::End();

    // —— 诊断面板 ——
    if (wizard.diagPanelOpen && ctx) {
        ImGui::SetNextWindowSize(ImVec2(650, 550), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("诊断工具##Diag", &wizard.diagPanelOpen)) {
            static int diagTab = 0;
            if (ImGui::BeginTabBar("diag_tabs")) {
                if (ImGui::BeginTabItem("全部 Inventory")) {
                    ImGui::TextWrapped("列出所有 Inventory 面板，用于确认主背包和仓库的真实名称。");
                    ImGui::Spacing();
                    ImGui::TextUnformatted("名称 | 尺寸 | Grid.Valid | 物品数");
                    ImGui::Separator();
                    ImGui::BeginChild("inv_list", ImVec2(0, 0), true);
                    auto allInvs = ctx->Inventory.GetAll();
                    for (const auto& inv : allInvs) {
                        const char* name = ctx->Inventory.GetName(inv.InventoryId);
                        const char* displayName = name ? name : "(无名)";
                        const char* validStr = inv.Grid.Valid ? "✓" : "✗";
                        // 标记是否被识别为主背包
                        bool isMain = false;
                        auto main = TabletReforgeGame::FindMainInventory(ctx);
                        if (main && main->InventoryId == inv.InventoryId) isMain = true;

                        ImGui::Text("%s%s", isMain ? "★ " : "  ", displayName);
                        ImGui::SameLine();
                        ImGui::TextDisabled(" | %dx%d", inv.TotalBoxesX, inv.TotalBoxesY);
                        ImGui::SameLine();
                        ImGui::TextDisabled(" | Valid=%s", validStr);
                        ImGui::SameLine();
                        ImGui::TextDisabled(" | 物品=%zu", inv.Items.size());
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("仓库物品")) {
                    ImGui::TextWrapped("浏览当前可见仓库页的所有物品，用于确认碑牌的真实 Path/BaseTypeName。");
                    ImGui::Spacing();

                    static char itemFilter[256] = {};
                    ImGui::InputText("过滤关键词 (小写)", itemFilter, sizeof(itemFilter));

                    auto inv = TabletReforgeGame::FindVisibleStash(ctx);
                    if (!inv) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                        ImGui::TextUnformatted("未检测到仓库面板，请先打开仓库");
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::Text("仓库页: %d x %d 格, 物品数: %zu",
                                    inv->TotalBoxesX, inv->TotalBoxesY, inv->Items.size());

                        ImGui::Separator();
                        ImGui::TextUnformatted("物品列表 (Path | BaseType | Rarity | Identified):");
                        ImGui::BeginChild("item_list", ImVec2(0, 0), true);
                        for (const auto& item : inv->Items) {
                            std::string path = item.Path;
                            std::string base = item.BaseTypeName;
                            std::string filter = itemFilter;
                            if (!filter.empty()) {
                                bool match = false;
                                std::string pathLower, baseLower;
                                for (char c : path) pathLower += std::tolower(static_cast<unsigned char>(c));
                                for (char c : base) baseLower += std::tolower(static_cast<unsigned char>(c));
                                if (pathLower.find(filter) != std::string::npos) match = true;
                                if (baseLower.find(filter) != std::string::npos) match = true;
                                if (!match) continue;
                            }
                            ImGui::Text("[%d] %s", item.Rarity, path.c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled("  | %s", base.c_str());
                            ImGui::SameLine();
                            ImGui::TextDisabled("  | %s", item.IsIdentified ? "已鉴定" : "未鉴定");
                        }
                        ImGui::EndChild();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }
}

} // namespace TabletReforgeUi
