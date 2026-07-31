// SettingsPanel.h — ImGui 设置面板
//
// 在 PoeFixer 的插件设置窗口里绘制所有可配置项。
// 分为：筛选配置（预设+按类型独立配置）、合成物品种类、启动控制、时序、仓库、安全门控、策略、标定数据。
#pragma once

#include "../config/CalibData.h"
#include "../config/Settings.h"
#include "../flow/StateMachine.h"
#include "../game/TabletBonusCatalog.h"
#include "../game/TabletFilter.h"
#include "../game/TabletRanges.h"
#include "../game/VisionRecognizer.h"
#include "../game/StashItemMapper.h"
#include "StashMappingPanel.h"
#include "../input/Win32Input.h"

#include <imgui.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <fstream>
#include <sstream>

namespace TabletReforgeUi {

// —— 辅助：词缀类别颜色 ——
inline ImVec4 CategoryColor(const char* category) {
    if (category == TabletReforgeGame::detail::kUnique)
        return ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
    if (category == TabletReforgeGame::detail::kMechanic)
        return ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
    return ImVec4(0.7f, 0.9f, 0.7f, 1.0f);
}

// —— 辅助：获取某类型的全部词缀列表 ——
inline std::vector<TabletReforgeGame::Bonus> GetAllBonusesForType(
    const std::string& typeKey)
{
    return TabletReforgeGame::detail::GetBonusesForType(typeKey);
}

// —— 辅助：安全格式化字符串 ——
inline std::string FormatString(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

// —— 预设选择器 ——
inline void DrawPresetSelector(TabletReforgeConfig::Settings& s) {
    ImGui::TextUnformatted("筛选配置预设");
    ImGui::SameLine();

    int curIdx = s.activePresetIndex;
    if (curIdx > (int)s.filterPresets.size()) curIdx = (int)s.filterPresets.size();

    if (ImGui::BeginCombo("##preset_combo",
        (curIdx < 0 || curIdx >= (int)s.filterPresets.size()) ? "当前配置（未保存）" :
         s.filterPresets[curIdx].name.c_str())) {

        if (ImGui::Selectable("当前配置（未保存）", curIdx < 0 || curIdx >= (int)s.filterPresets.size())) {
            s.activePresetIndex = -1;
        }

        ImGui::Separator();

        for (int i = 0; i < (int)s.filterPresets.size(); ++i) {
            bool isSelected = (i == s.activePresetIndex);
            std::string label = FormatString("%s (%d词缀 × %d类型)%s",
                s.filterPresets[i].name.c_str(),
                (int)s.filterPresets[i].bonusIds.size(),
                (int)s.filterPresets[i].typeConfigs.size(),
                isSelected ? " ✓" : "");
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                s.ApplyPreset(i);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%d)", (int)s.filterPresets.size());

    // 名称输入
    static char nameBuf[256] = {};
    if (nameBuf[0] == 0 && !s.filterPresets.empty() && curIdx >= 0 && curIdx < (int)s.filterPresets.size()) {
        std::strncpy(nameBuf, s.filterPresets[curIdx].name.c_str(), sizeof(nameBuf) - 1);
    }
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputTextWithHint("名称", "配置名称...", nameBuf, sizeof(nameBuf))) {
        s.newPresetName = nameBuf;
    }
    s.newPresetName = nameBuf;

    // 新建/复制/更新/删除按钮
    int presetCount = (int)s.filterPresets.size();
    bool atCap = presetCount >= TabletReforgeConfig::kMaxPresets;
    bool canEdit = curIdx >= 0 && curIdx < presetCount;

    if (atCap) ImGui::BeginDisabled();
    if (ImGui::Button("新建")) {
        TabletReforgeConfig::FilterPreset p;
        p.name = (nameBuf[0] ? nameBuf : ("配置 " + std::to_string(presetCount + 1)));
        p.subCategories = s.selectedSubCategories;
        p.bonusIds = s.selectedBonusIds;
        p.typeConfigs = s.typeConfigs;
        s.filterPresets.push_back(std::move(p));
        s.activePresetIndex = (int)s.filterPresets.size() - 1;
        nameBuf[0] = '\0';
    }
    if (atCap) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!canEdit) ImGui::BeginDisabled();
    if (ImGui::Button("复制")) {
        TabletReforgeConfig::FilterPreset p = s.filterPresets[curIdx];
        p.name += " 副本";
        s.filterPresets.push_back(std::move(p));
        s.activePresetIndex = (int)s.filterPresets.size() - 1;
        std::strncpy(nameBuf, s.filterPresets[curIdx].name.c_str(), sizeof(nameBuf) - 1);
    }
    if (!canEdit) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!canEdit) ImGui::BeginDisabled();
    if (ImGui::Button("更新")) {
        s.UpdatePreset(curIdx);
    }
    if (!canEdit) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!canEdit || presetCount <= 1) ImGui::BeginDisabled();
    if (ImGui::Button("删除")) {
        s.DeletePreset(curIdx);
        if (!s.filterPresets.empty()) {
            std::strncpy(nameBuf, s.filterPresets[s.activePresetIndex].name.c_str(), sizeof(nameBuf) - 1);
        } else {
            nameBuf[0] = '\0';
        }
    }
    if (!canEdit || presetCount <= 1) ImGui::EndDisabled();

    if (atCap) ImGui::TextDisabled("已达配置数量上限（32）。");
}

// —— 单个类型配置面板 ——
inline void DrawTypeConfigPanel(TabletReforgeConfig::TypeConfig& tc,
    const std::vector<TabletReforgeGame::Bonus>& allBonuses)
{
    ImGui::PushID(tc.key.c_str());

    const int selCount = static_cast<int>(tc.selectedBonusIds.size());
    const int reqCount = static_cast<int>(tc.requiredBonusIds.size());
    const int optCount = selCount - reqCount;

    if (reqCount > 0) {
        ImGui::Text("必须词缀 %d, 可选词缀 %d", reqCount, optCount);
    } else if (selCount > 0) {
        ImGui::Text("已选 %d 个词缀", selCount);
    } else {
        ImGui::TextDisabled("未选择词缀");
    }

    ImGui::Checkbox("启用", &tc.enabled);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderInt("最少剩余次数", &tc.minUsesLeft, 0, TabletReforgeConfig::kMinUsesMax);

    if (reqCount > 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("最少必须词缀数", &tc.minRequiredBonuses, 0, reqCount);
    }

    if (optCount > 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("最少可选词缀数", &tc.minMatchedBonuses, 0, optCount);
    }

    ImGui::Text("匹配词缀 (共 %zu 个):", allBonuses.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("清空选择")) {
        tc.selectedBonusIds.clear();
        tc.requiredBonusIds.clear();
        tc.valueFilters.clear();
    }
    ImGui::TextDisabled("勾选词缀后，在右侧点击'必须'设为必须条件");

    static char searchBuf[256] = {};
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##searchfilter", searchBuf, sizeof(searchBuf));
    std::string searchFilter = searchBuf;
    std::transform(searchFilter.begin(), searchFilter.end(), searchFilter.begin(),
        [](unsigned char c) { return std::tolower(c); });

    ImGui::BeginChild("##bonus_list", ImVec2(-FLT_MIN, 200), true);

    std::string lastCategory;
    int shown = 0;

    for (const auto& b : allBonuses) {
        if (!searchFilter.empty()) {
            std::string labelLower = b.Label;
            std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(),
                [](unsigned char c) { return std::tolower(c); });
            std::string idLower = b.NormId;
            if (labelLower.find(searchFilter) == std::string::npos &&
                idLower.find(searchFilter) == std::string::npos) continue;
        }

        if (b.Category != lastCategory) {
            lastCategory = b.Category;
            ImGui::SeparatorText(b.Category.c_str());
        }

        ImGui::PushID(b.NormId.c_str());

        bool isSel = tc.IsSelected(b.NormId);
        bool isReq = tc.IsRequired(b.NormId);

        if (ImGui::Checkbox(b.Label.c_str(), &isSel)) {
            tc.ToggleSelected(b.NormId);
        }

        if (isSel) {
            ImGui::SameLine();
            if (ImGui::Checkbox("必须", &isReq)) {
                tc.ToggleRequired(b.NormId);
            }

            const auto* rg = TabletReforgeGame::FindRange(b.NormId);
            if (!rg && !b.NormIdStripped.empty())
                rg = TabletReforgeGame::FindRange(b.NormIdStripped);

            if (rg && rg->min != rg->max) {
                auto it = tc.valueFilters.find(b.NormId);
                bool hasFilter = (it != tc.valueFilters.end());
                int minVal = hasFilter ? it->second.min : 0;
                int maxVal = hasFilter ? it->second.max : 0;
                bool showRange = false;

                int lo = rg->min < 0 ? rg->min : 0;
                int hi = rg->max;
                const bool pct = rg->unit == "percent";

                ImGui::SameLine();
                ImGui::TextUnformatted("\xE2\x89\xA5");
                ImGui::SameLine(0.f, 2.f);

                char vminBuf[8];
                std::snprintf(vminBuf, sizeof(vminBuf), "%d", minVal);
                ImGui::SetNextItemWidth(46.f);
                if (ImGui::InputText("##vmin", vminBuf, sizeof(vminBuf),
                    ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll)) {
                    int nv = (vminBuf[0] == '\0' || (vminBuf[0] == '-' && vminBuf[1] == '\0'))
                        ? 0 : std::atoi(vminBuf);
                    if (nv < lo) nv = lo;
                    if (nv > hi) nv = hi;
                    minVal = nv;
                    showRange = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("最小值. 范围 %d-%d%s. 0 = 不限.",
                        rg->min, rg->max, pct ? "%" : "");

                ImGui::SameLine();
                ImGui::TextUnformatted("\xE2\x89\xA4");
                ImGui::SameLine(0.f, 2.f);

                char vmaxBuf[8];
                std::snprintf(vmaxBuf, sizeof(vmaxBuf), "%d", maxVal);
                ImGui::SetNextItemWidth(46.f);
                if (ImGui::InputText("##vmax", vmaxBuf, sizeof(vmaxBuf),
                    ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll)) {
                    int nv = (vmaxBuf[0] == '\0' || (vmaxBuf[0] == '-' && vmaxBuf[1] == '\0'))
                        ? 0 : std::atoi(vmaxBuf);
                    if (nv < lo) nv = lo;
                    if (nv > hi) nv = hi;
                    maxVal = nv;
                    showRange = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("最大值. 范围 %d-%d%s. 0 = 不限.",
                        rg->min, rg->max, pct ? "%" : "");

                if (showRange || hasFilter) {
                    if (minVal == 0 && maxVal == 0) {
                        tc.valueFilters.erase(b.NormId);
                    } else {
                        tc.valueFilters[b.NormId] = {minVal, maxVal};
                    }
                }
            }
        }

        ImGui::PopID();
        shown++;
    }

    if (shown == 0) {
        ImGui::TextDisabled("没有匹配的词缀");
    }

    ImGui::EndChild();
    ImGui::PopID();
}

// —— 石板类型折叠区域 ——
inline void DrawTabletTypeSection(TabletReforgeConfig::Settings& s) {
    ImGui::TextUnformatted("— 石板类型独立配置 —");
    ImGui::TextDisabled("每种石板可独立设置启用状态、最少次数要求和词缀筛选");

    // 全部类型通用词缀
    if (ImGui::CollapsingHeader("通用词缀（所有类型共享）")) {
        auto common = TabletReforgeGame::detail::CommonBonuses();
        ImGui::TextDisabled("共 %zu 个通用词缀", common.size());

        ImGui::BeginChild("##common_list", ImVec2(-FLT_MIN, 120), true);
        for (const auto& b : common) {
            ImGui::PushID(b.NormId.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
            ImGui::TextUnformatted(b.Label.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetCursorPosX() + 350);
            ImGui::TextDisabled("[%s]", b.Category.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // 各石板类型独立配置
    const auto& typeNames = TabletReforgeConfig::GetTypeDisplayNames();

    for (const auto& [key, display] : typeNames) {
        auto* tc = s.FindType(key);
        if (!tc) continue;

        auto allBonuses = GetAllBonusesForType(key);

        std::string headerLabel;
        int selCount = (int)tc->selectedBonusIds.size();
        int reqCount = (int)tc->requiredBonusIds.size();
        int optCount = selCount - reqCount;

        if (reqCount > 0) {
            headerLabel = FormatString("%s  (%d必须 + %d可选)###%s_hdr",
                display.c_str(), reqCount, optCount, key.c_str());
        } else if (selCount > 0) {
            headerLabel = FormatString("%s  (%d词缀)###%s_hdr",
                display.c_str(), selCount, key.c_str());
        } else {
            headerLabel = FormatString("%s###%s_hdr", display.c_str(), key.c_str());
        }

        if (ImGui::CollapsingHeader(headerLabel.c_str())) {
            DrawTypeConfigPanel(*tc, allBonuses);
        }
    }
}

// —— 调试：导出词缀目录 ——
inline void DrawDebugExport() {
    if (ImGui::CollapsingHeader("调试: 导出石板词缀目录")) {
        ImGui::TextDisabled("将当前所有石板词缀导出为 JSON 文件，用于调试或备份。");

        static char exportPath[512] = "tablet_bonus_catalog.json";
        ImGui::InputText("导出路径", exportPath, sizeof(exportPath));

        if (ImGui::Button("导出为 JSON")) {
            nlohmann::json j;
            nlohmann::json types = nlohmann::json::array();

            auto addType = [&](const std::string& key, const char* display) {
                const auto& bonuses = TabletReforgeGame::detail::GetBonusesForType(key);
                nlohmann::json tj;
                tj["type"] = key;
                tj["display_name"] = display;
                nlohmann::json bj = nlohmann::json::array();
                for (const auto& b : bonuses) {
                    nlohmann::json entry;
                    entry["id"] = b.Id;
                    entry["label"] = b.Label;
                    entry["category"] = b.Category;
                    entry["norm_id"] = b.NormId;
                    bj.push_back(entry);
                }
                tj["bonuses"] = bj;
                types.push_back(tj);
            };

            addType("Breach", "裂隙石板");
            addType("Delirium", "谵妄石板");
            addType("Ritual", "祭礼石板");
            addType("Overseer", "督军石板");
            addType("Abyss", "深渊石板");
            addType("Temple", "神殿石板");
            addType("Irradiated", "辐照石板");

            j["tablet_types"] = types;

            nlohmann::json common = nlohmann::json::array();
            for (const auto& b : TabletReforgeGame::detail::CommonBonuses()) {
                nlohmann::json entry;
                entry["id"] = b.Id;
                entry["label"] = b.Label;
                entry["category"] = b.Category;
                entry["norm_id"] = b.NormId;
                common.push_back(entry);
            }
            j["common_bonuses"] = common;

            std::ofstream out(exportPath);
            if (out.is_open()) {
                out << j.dump(2);
                out.close();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                ImGui::TextUnformatted("导出成功!");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextUnformatted("导出失败!");
                ImGui::PopStyleColor();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("打印到日志")) {
            auto printType = [&](const std::string& key) {
                const auto& bonuses = TabletReforgeGame::detail::GetBonusesForType(key);
                TabletReforgeGame::Log("类型 [%s] 共 %zu 个词缀:", key.c_str(), bonuses.size());
                for (const auto& b : bonuses) {
                    TabletReforgeGame::Log("  %s [%s] norm=%s",
                        b.Label.c_str(), b.Category.c_str(), b.NormId.c_str());
                }
            };

            printType("Breach");
            printType("Delirium");
            printType("Ritual");
            printType("Overseer");
            printType("Abyss");
            printType("Temple");
            printType("Irradiated");
            TabletReforgeGame::Log("通用词缀: %zu 个", TabletReforgeGame::detail::CommonBonuses().size());
        }
    }
}

// —— 合成物品种类选择 ——
inline void DrawSynthesisConfig(TabletReforgeConfig::Settings& s) {
    if (ImGui::CollapsingHeader("合成物品种类选择")) {
        ImGui::TextDisabled("勾选要保留的子类（未勾选的将被合成为新物品）");
        ImGui::TextDisabled("白色品质物品（催化剂/精髓/液态情感/符文等）：选中=产物, 未选中=原料, 无需鉴定");
        ImGui::Checkbox("使用子类选择模式", &s.useSubCategoryMode);

        ImGui::SameLine();
        ImGui::Checkbox("自动将保留物品存入仓库", &s.autoDepositWanted);
        ImGui::SameLine();
        ImGui::SliderInt("非选择物品低于N个时停止", &s.minUnwantedBeforeStop, 0, 20);

        ImGui::Separator();

        // —— 快捷: 按品质筛选原料（合成物品栏中的品质筛选 UI）——
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "☑ 原料品质筛选（仓库取出时生效）");
        ImGui::Checkbox("启用原料品质筛选", &s.filterByRarity);
        ImGui::SameLine();
        if (!s.filterByRarity) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
            ImGui::TextDisabled("(关闭: 白色/魔法/稀有 全部取出，不限制品质)");
            ImGui::PopStyleColor();
        }
        if (s.filterByRarity) {
            ImGui::PushItemWidth(220);
            const char* rarityItems = "白色 (普通 Rarity=0, 催化剂/符文)\0魔法 (蓝色 Rarity=1)\0稀有 (黄色 Rarity=2)\0传奇 (橙色 Rarity=3)\0";
            ImGui::Combo("原料最低品质", &s.minRarityForMaterial, rarityItems);
            ImGui::PopItemWidth();
            // 快捷按钮
            ImGui::SameLine();
            if (ImGui::SmallButton("全部品质(白)")) s.minRarityForMaterial = 0;
            ImGui::SameLine();
            if (ImGui::SmallButton("魔法+")) s.minRarityForMaterial = 1;
            ImGui::SameLine();
            if (ImGui::SmallButton("稀有+")) s.minRarityForMaterial = 2;
            if (s.minRarityForMaterial == 0) {
                ImGui::TextDisabled("当前: 取出所有可合成物品(白色催化剂/符文 也会被取出)");
            } else if (s.minRarityForMaterial == 1) {
                ImGui::TextDisabled("当前: 取出魔法(蓝)及以上, 跳过白色(催化剂/符文/白色物品)");
            } else if (s.minRarityForMaterial == 2) {
                ImGui::TextDisabled("当前: 只取出稀有(黄)及以上物品, 适合只重铸稀有碑牌");
            } else {
                ImGui::TextDisabled("当前: 只取出传奇(橙)物品");
            }
        }

        ImGui::Separator();

        if (s.useSubCategoryMode) {
            struct SubCatOption {
                int id;
                std::string typeKey;
                std::string displayName;
            };

            static const SubCatOption subOptions[] = {
                {0,  "",  "Waystone（地图钥匙）"},
                {101, "Irradiated", "Irradiated Precursor Tablet（輻照碑牌）"},
                {102, "Breach",  "Breach Precursor Tablet（裂痕碑牌）"},
                {103, "Expedition", "Expedition Precursor Tablet（探險碑牌）"},
                {104, "Delirium", "Delirium Precursor Tablet（譫妄碑牌）"},
                {105, "Abyss",   "Abyss Precursor Tablet（深淵碑牌）"},
                {106, "Ritual",  "Ritual Precursor Tablet（祭祀碑牌）"},
                {107, "Overseer", "Overseer Precursor Tablet（總督碑牌）"},
                {108, "Temple",  "Temple Precursor Tablet（神廟碑牌）"},
                {201, "Jewel-Ruby", "Ruby Jewel（紅玉·力量）"},
                {202, "Jewel-Emerald", "Emerald Jewel（翠綠碧雲·敏捷）"},
                {203, "Jewel-Sapphire", "Sapphire Jewel（藍玉·智力）"},
                {204, "Jewel-Diamond", "Diamond Jewel（鑽石·全属性）"},
                {205, "Jewel-Timeless", "Timeless Jewel（永恆珠寶）"},
                {206, "Jewel-TimeLost", "Time-Lost Jewel（時迭珠寶）"},
                {301, "Rune-Desert", "Desert Rune（沙漠符文·火）"},
                {302, "Rune-Glacial", "Glacial Rune（冰川符文·冰）"},
                {303, "Rune-Storm", "Storm Rune（暴風符文·电）"},
                {304, "Rune-Iron", "Iron Rune（鍛鐵符文·护甲）"},
                {305, "Rune-Body", "Body Rune（肉體符文·生命）"},
                {306, "Rune-Mind", "Mind Rune（心靈符文·魔力）"},
                {401, "Essence-Life", "Life Essence（肉體精髓）"},
                {402, "Essence-Mind", "Mana Essence（心智精髓）"},
                {403, "Essence-Defences", "Defences Essence（強化精髓）"},
                {404, "Essence-Physical", "Physical Essence（磨礪精髓）"},
                {405, "Essence-Fire", "Fire Essence（烈焰精髓）"},
                {406, "Essence-Cold", "Cold Essence（寒冰精髓）"},
                {407, "Essence-Lightning", "Lightning Essence（閃電精髓）"},
                {408, "Essence-Chaos", "Chaos Essence（渾沌精髓）"},
                {409, "Essence-Attack", "Attack Essence（銳利精髓）"},
                {410, "Essence-Caster", "Caster Essence（施法精髓）"},
                {411, "Essence-Speed", "Speed Essence（迅捷精髓）"},
                {412, "Essence-Attribute", "Attribute Essence（都會精髓）"},
                {501, "Liquid-Ire", "Distilled Ire（液態憤怒）"},
                {502, "Liquid-Guilt", "Distilled Guilt（液態罪孽）"},
                {503, "Liquid-Greed", "Distilled Greed（液態貪婪）"},
                {504, "Liquid-Paranoia", "Distilled Paranoia（液態偏執）"},
                {505, "Liquid-Envy", "Distilled Envy（液態忌妒）"},
                {506, "Liquid-Disgust", "Distilled Disgust（液態厭惡）"},
                {507, "Liquid-Despair", "Distilled Despair（液態絕望）"},
                {508, "Liquid-Fear", "Distilled Fear（液態恐懼）"},
                {509, "Liquid-Suffering", "Distilled Suffering（液態苦難）"},
                {510, "Liquid-Isolation", "Distilled Isolation（液態孤立）"},
                {601, "Catalyst-Life", "Flesh Catalyst（血肉催化劑·生命）"},
                {602, "Catalyst-Mana", "Mana Catalyst（魔力催化劑·魔力）"},
                {603, "Catalyst-Defences", "Defences Catalyst（護衛催化劑·防御）"},
                {604, "Catalyst-Physical", "Physical Catalyst（利刃催化劑·物理）"},
                {605, "Catalyst-Fire", "Fire Catalyst（烈焰催化劑·火）"},
                {606, "Catalyst-Cold", "Cold Catalyst（寒冰催化劑·冰）"},
                {607, "Catalyst-Lightning", "Lightning Catalyst（閃電催化劑·电）"},
                {608, "Catalyst-Chaos", "Chaos Catalyst（渾沌催化劑·混沌）"},
                {609, "Catalyst-Attack", "Attack Catalyst（銳利催化劑·攻击）"},
                {610, "Catalyst-Caster", "Caster Catalyst（施法催化劑·施法）"},
                {611, "Catalyst-Speed", "Speed Catalyst（迅捷催化劑·速度）"},
                {612, "Catalyst-Attribute", "Attribute Catalyst（都會催化劑·属性）"},
                {613, "Catalyst-Necrotic", "Necrotic Catalyst（死靈催化劑·死灵）"},
                {701, "RefinedCatalyst-Life", "Refined Flesh Catalyst（精製血肉催化劑·生命）"},
                {702, "RefinedCatalyst-Mana", "Refined Mana Catalyst（精製魔力催化劑·魔力）"},
                {703, "RefinedCatalyst-Defences", "Refined Defences Catalyst（精製護衛催化劑·防御）"},
                {704, "RefinedCatalyst-Physical", "Refined Physical Catalyst（精製利刃催化劑·物理）"},
                {705, "RefinedCatalyst-Fire", "Refined Fire Catalyst（精製烈焰催化劑·火）"},
                {706, "RefinedCatalyst-Cold", "Refined Cold Catalyst（精製寒冰催化劑·冰）"},
                {707, "RefinedCatalyst-Lightning", "Refined Lightning Catalyst（精製閃電催化劑·电）"},
                {708, "RefinedCatalyst-Chaos", "Refined Chaos Catalyst（精製渾沌催化劑·混沌）"},
                {709, "RefinedCatalyst-Attack", "Refined Attack Catalyst（精製銳利催化劑·攻击）"},
                {710, "RefinedCatalyst-Caster", "Refined Caster Catalyst（精製施法催化劑·施法）"},
                {711, "RefinedCatalyst-Speed", "Refined Speed Catalyst（精製迅捷催化劑·速度）"},
                {712, "RefinedCatalyst-Attribute", "Refined Attribute Catalyst（精製都會催化劑·属性）"},
                {713, "RefinedCatalyst-Necrotic", "Refined Necrotic Catalyst（精製死靈催化劑·死灵）"},
            };

            int totalOptions = sizeof(subOptions) / sizeof(subOptions[0]);

            ImGui::BeginChild("##subCategoryList", ImVec2(-FLT_MIN, 18 * ImGui::GetTextLineHeightWithSpacing()));

            for (int i = 0; i < totalOptions; ++i) {
                const auto& opt = subOptions[i];
                bool isSelected = s.selectedSubCategories.count(opt.id) > 0;

                std::string checkLabel = FormatString("%s##subcat_%d", opt.displayName.c_str(), i);

                bool newVal = isSelected;
                if (ImGui::Checkbox(checkLabel.c_str(), &newVal)) {
                    if (newVal) {
                        s.selectedSubCategories.insert(opt.id);
                    } else {
                        s.selectedSubCategories.erase(opt.id);
                    }
                }

                if (!opt.typeKey.empty()) {
                    auto allB = GetAllBonusesForType(opt.typeKey);
                    if (!allB.empty()) {
                        ImGui::SameLine(ImGui::GetCursorPosX() + 280);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
                        ImGui::TextDisabled("(%d词缀)", (int)allB.size());
                        ImGui::PopStyleColor();
                    }
                }
            }

            ImGui::EndChild();

            ImGui::TextDisabled("已选中 %zu / %d 个子类别", s.selectedSubCategories.size(), totalOptions);

            if (ImGui::Button("全选")) {
                for (int i = 0; i < totalOptions; ++i) {
                    s.selectedSubCategories.insert(subOptions[i].id);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("清空")) {
                s.selectedSubCategories.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("反选")) {
                std::set<int> newSet;
                for (int i = 0; i < totalOptions; ++i) {
                    if (s.selectedSubCategories.count(subOptions[i].id) == 0) {
                        newSet.insert(subOptions[i].id);
                    }
                }
                s.selectedSubCategories = std::move(newSet);
            }
        } else {
            int typeCount = 0;
            const auto* craftList = TabletReforgeGame::GetCraftableItemList(typeCount);
            int cur = std::clamp(s.itemType, 0, typeCount - 1);

            std::vector<std::string> displayNames;
            displayNames.reserve(typeCount);
            for (int i = 0; i < typeCount; ++i) {
                std::string dn = craftList[i].displayName;
                int pCount = 0;
                const auto* patterns = TabletReforgeGame::GetPoe2Patterns(pCount);
                for (int j = 0; j < pCount; ++j) {
                    if (static_cast<int>(patterns[j].category) == i && patterns[j].chineseName) {
                        dn += " / ";
                        dn += patterns[j].chineseName;
                        break;
                    }
                }
                displayNames.push_back(dn);
            }

            int itemTypeIdx = s.itemType;
            if (ImGui::Combo("##itemType", &itemTypeIdx,
                [](void* data, int idx, const char** out_text) -> bool {
                    auto* names = static_cast<std::vector<std::string>*>(data);
                    if (idx < 0 || idx >= (int)names->size()) return false;
                    *out_text = (*names)[idx].c_str();
                    return true;
                }, &displayNames, (int)displayNames.size())) {
                s.itemType = itemTypeIdx;
                s.subCategoryId = 0;
                cur = itemTypeIdx;
            }

            if (cur >= 0 && cur < typeCount) {
                const auto& info = craftList[cur];
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
                ImGui::TextWrapped("📋 %s", info.description);
                ImGui::PopStyleColor();
                ImGui::TextDisabled("🔍 识别规则: %s", info.pathPattern);
            }

            if (cur == 7) {
                static char kwBuf[512] = {};
                if (s.customKeywords.size() < sizeof(kwBuf) - 1) {
                    std::memcpy(kwBuf, s.customKeywords.c_str(), s.customKeywords.size());
                    kwBuf[s.customKeywords.size()] = 0;
                }
                ImGui::TextUnformatted("自定义关键词 (英文逗号分隔):");
                if (ImGui::InputText("##customKws", kwBuf, sizeof(kwBuf))) {
                    s.customKeywords = kwBuf;
                }
            }
        }

        ImGui::Separator();
        ImGui::Checkbox("原料要求已鉴定 (需满足)", &s.requireIdentified);
        ImGui::SameLine();
        ImGui::Checkbox("3 槽必须完全相同物品 (推荐)", &s.matchExactItem);
    }
}

// —— 主设置面板 ——
inline void DrawSettingsPanel(TabletReforgeConfig::Settings& s,
                               TabletReforgeConfig::CalibData& calib,
                               TabletReforgeFlow::StateMachine& sm,
                               const PluginSDK::Context* ctx = nullptr,
                               const std::filesystem::path& pluginDir = {}) {
    // —— 筛选配置 ——
    if (ImGui::CollapsingHeader("筛选配置", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawPresetSelector(s);
        ImGui::Separator();
        DrawTabletTypeSection(s);
        ImGui::Separator();
        DrawDebugExport();
    }

    ImGui::Separator();

    // —— 合成物品种类 ——
    DrawSynthesisConfig(s);

    ImGui::Separator();

    // —— 启动控制 ——
    if (ImGui::CollapsingHeader("启动控制", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用插件", &s.enabled);
        ImGui::SameLine();
        if (sm.IsRunning()) {
            if (ImGui::Button("停止 (Stop)")) sm.Stop();
        } else if (!sm.LastError().empty()) {
            if (ImGui::Button("清除错误")) sm.ClearError();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(按热键启动)");

        const char* keyNames[] = {"F5", "F6", "F7", "F8", "F9"};
        const int keyValues[] = {VK_F5, VK_F6, VK_F7, VK_F8, VK_F9};
        int keyIdx = 1;
        for (int i = 0; i < 5; ++i) {
            if (s.toggleKey == keyValues[i]) { keyIdx = i; break; }
        }
        if (ImGui::Combo("启动热键", &keyIdx, keyNames, 5)) {
            s.toggleKey = keyValues[keyIdx];
        }

        ImGui::SliderInt("最大循环数 (0=无限)", &s.maxLoops, 0, 100);
    }

    ImGui::Separator();

    // —— 时序 ——
    if (ImGui::CollapsingHeader("时序设置 (毫秒)")) {
        ImGui::SliderInt("点击延迟", &s.clickDelayMs, 10, 1000);
        ImGui::SliderInt("点击后延迟", &s.postClickDelayMs, 0, 500);
        ImGui::SliderInt("光标稳定延迟", &s.cursorSettleMs, 0, 200);
        ImGui::SliderInt("UI 等待间隔", &s.uiWaitMs, 50, 1000);
        ImGui::SliderInt("合成等待", &s.combineWaitMs, 100, 3000);
        ImGui::SliderInt("扫描稳定延迟", &s.scanSettleMs, 0, 500);
        ImGui::SliderInt("状态超时", &s.stateTimeoutMs, 1000, 30000);

        ImGui::Separator();
        // —— 模拟玩家鼠标轨迹（WindMouse v4：近距离跳过 + 三档距离策略）——
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "模拟玩家鼠标轨迹 v4（WindMouse + 近距离跳过 + 批处理取出）");
        ImGui::Checkbox("启用模拟玩家鼠标轨迹", &s.enableHumanMouse);
        ImGui::TextDisabled("v4新特性: <18px瞬移, <80px简化2-3步, >80px完整WindMouse弧; 取出物品单帧批处理4个消除跨帧等待");
        if (s.enableHumanMouse) {
            // —— 四档预设按钮（v4：极速档解决 300ms 内目标）——
            ImGui::TextUnformatted("快速选择预设:");
            ImGui::SameLine();
            if (ImGui::SmallButton("极速(吞吐量极限)")) {
                s.mouseGravity = 14;
                s.mouseWind = 1;
                s.mouseMaxStep = 32;
                s.mouseStepWaitMs = 0;
                s.clickDelayMs = 10;
                s.cursorSettleMs = 1;
                s.postClickDelayMs = 3;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("快速(流水线取物★)")) {
                s.mouseGravity = 13;
                s.mouseWind = 2;
                s.mouseMaxStep = 28;
                s.mouseStepWaitMs = 1;
                s.clickDelayMs = 30;
                s.cursorSettleMs = 3;
                s.postClickDelayMs = 15;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("平衡(自然感 ben.land)")) {
                s.mouseGravity = 9;
                s.mouseWind = 3;
                s.mouseMaxStep = 15;
                s.mouseStepWaitMs = 2;
                s.clickDelayMs = 50;
                s.cursorSettleMs = 5;
                s.postClickDelayMs = 25;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("精准(远距离)")) {
                s.mouseGravity = 7;
                s.mouseWind = 4;
                s.mouseMaxStep = 10;
                s.mouseStepWaitMs = 3;
                s.clickDelayMs = 80;
                s.cursorSettleMs = 10;
                s.postClickDelayMs = 40;
            }

            ImGui::PushItemWidth(240);
            ImGui::SliderInt("重力强度 Gravity", &s.mouseGravity, 1, 50);
            ImGui::SameLine();
            ImGui::TextDisabled("越大轨迹越直");
            ImGui::SliderInt("风力强度 Wind", &s.mouseWind, 0, 20);
            ImGui::SameLine();
            ImGui::TextDisabled("越大轨迹越抖");
            ImGui::SliderInt("最大步长像素 MaxStep", &s.mouseMaxStep, 1, 100);
            ImGui::SameLine();
            ImGui::TextDisabled("越大移动越快");
            ImGui::SliderInt("每步等待毫秒 StepWait", &s.mouseStepWaitMs, 0, 50);
            ImGui::SameLine();
            ImGui::TextDisabled("每步Sleep基准 0-3 最快");
            ImGui::PopItemWidth();
            if (ImGui::SmallButton("恢复默认推荐值（快速档）")) {
                s.mouseGravity = 13;
                s.mouseWind = 2;
                s.mouseMaxStep = 28;
                s.mouseStepWaitMs = 1;
                s.clickDelayMs = 30;
                s.cursorSettleMs = 3;
                s.postClickDelayMs = 15;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("v4默认: 快速档(快速流水线取物)");
            ImGui::Separator();
            // v4: 批处理架构预期总节奏（相邻格 ~60px 距离）
            int estWind = (s.mouseStepWaitMs <= 0) ? 1 : (s.mouseStepWaitMs * 2); // 相邻简化 2-3步
            int estPerClick = s.clickDelayMs + estWind + s.cursorSettleMs + s.postClickDelayMs;
            int estBatch4 = estPerClick * 4;
            ImGui::TextDisabled("批处理节奏预测: 单步点击~%dms (相邻格) | 单帧批4个~%dms | 目标 < 300ms/单物品 用户体验",
                estPerClick, estBatch4);
            if (estBatch4 <= 280) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "★ 达标");
            } else if (estPerClick <= 500) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Δ 可接受");
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "✗ 较慢");
            }
        }

        ImGui::Separator();
        // —— 按品质筛选原料 ——
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "按品质筛选原料（仓库取出时）");
        ImGui::Checkbox("启用按品质筛选原料", &s.filterByRarity);
        ImGui::TextDisabled("开启后，只取出大于等于最低品质的物品作为原料");
        if (s.filterByRarity) {
            ImGui::PushItemWidth(240);
            const char* rarityItems = "白色 (普通, Rarity=0)\0魔法 (蓝色, Rarity=1)\0稀有 (黄色, Rarity=2)\0传奇 (橙色, Rarity=3)\0";
            ImGui::Combo("原料最低品质", &s.minRarityForMaterial, rarityItems);
            ImGui::PopItemWidth();
            ImGui::TextDisabled("说明: 催化剂/符文等白色物品是 Rarity=0，如需取出请选择'白色'");
        }
    }

    ImGui::Separator();

    // —— 仓库 ——
    if (ImGui::CollapsingHeader("仓库设置")) {
        ImGui::SliderInt("背包预留格数", &s.reservedBagSlots, 0, 30);

        ImGui::Separator();

        // —— 仓库设置标签页 ——
        if (ImGui::BeginTabBar("stash_tabs", ImGuiTabBarFlags_None)) {
            
            // === 标签1: 仓库映射配置 ===
            if (ImGui::BeginTabItem("仓库映射配置")) {
                TabletReforgeUi::DrawStashMappingConfigPanel(s, ctx, pluginDir);
                ImGui::EndTabItem();
            }
            
            // === 标签2: 视觉识别测试 ===
            if (ImGui::BeginTabItem("视觉识别测试")) {
                TabletReforgeUi::DrawVisionRecognitionTestPanel(s, ctx, pluginDir);
                ImGui::EndTabItem();
            }
            
            ImGui::EndTabBar();
        }
    }

    ImGui::Separator();

    // —— 安全门控 ——
    if (ImGui::CollapsingHeader("安全门控")) {
        ImGui::Checkbox("必须在城镇/藏身处", &s.gateTownHideout);
        ImGui::Checkbox("检测附近敌人", &s.gateEnemyNear);
        ImGui::SliderInt("敌人检测范围", &s.enemyRange, 5, 100);
        ImGui::Checkbox("检测菜单遮挡", &s.gateMenu);
        ImGui::Checkbox("游戏必须在前台", &s.gateNotForeground);
        ImGui::Checkbox("右键取消", &s.cancelOnRightClick);
        ImGui::Checkbox("Esc 取消", &s.cancelOnEsc);
    }

    ImGui::Separator();

    // —— 策略 ——
    if (ImGui::CollapsingHeader("策略")) {
        ImGui::Checkbox("取出只取已鉴定 (旧设置，等效上面)", &s.withdrawRequireIdentified);
        ImGui::Checkbox("存回只存未鉴定碑牌", &s.depositOnlyUnidentified);
        ImGui::Checkbox("Ctrl 会话级保持", &s.ctrlSessionMode);
    }

    ImGui::Separator();

    // —— 方案 B v1.3：合规词缀 Id 读取（宪法修正案 v1.3 熔断开关）——
    if (ImGui::CollapsingHeader("方案B 词缀Id合规读取 (熔断开关)")) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        ImGui::TextWrapped("【宪法修正案 v1.3】以下开关控制 ReadItemMods 的词缀 Id 读取行为。");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("关闭时(默认): 方案A, 仅靠 Path/Rarity/IsIdentified 筛选（零风险）");
        ImGui::TextDisabled("开启时: 方案B, 读取 Mod.Id+Hash32 做白名单匹配（合规但需观察）");
        ImGui::Separator();

        // 主开关：enableBonusMatch
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        if (ImGui::Checkbox("启用合规词缀Id读取 (EnableBonusMatch)", &s.enableBonusMatch)) {
            // 熔断开关：用户可一键关闭，立即降级为方案A
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (s.enableBonusMatch) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            ImGui::TextUnformatted("[方案B 已启用]");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::TextUnformatted("[方案A 已降级]");
            ImGui::PopStyleColor();
        }

        // 静默压力测试开关：bonusMatchSilent
        if (s.enableBonusMatch) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
            if (ImGui::Checkbox("静默压力测试模式 (BonusMatchSilent)", &s.bonusMatchSilent)) {
                // 静默模式：ExtractModIds 仍调用并输出日志，但判定走 4 参数版（不影响行为）
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (s.bonusMatchSilent) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
                ImGui::TextUnformatted("[静默测试中]");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                ImGui::TextUnformatted("[完整启用]");
                ImGui::PopStyleColor();
            }
            ImGui::TextDisabled("静默模式: 仅记录日志不做 UI 高亮, 用于 1-2 周观察期验证安全");
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextWrapped("说明: 此开关同步保存到 settings.json, 紧急情况一键关闭即可降级, 无需重新编译。");
        ImGui::TextWrapped("绝对红线: Mod.Name / Mod.AffixName / Mod.StatKey 在任何路径下均禁止读取。");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // —— NPC 鉴定 ——
    if (ImGui::CollapsingHeader("NPC 鉴定 (多利亚尼)")) {
        ImGui::Checkbox("自动鉴定未鉴定产物", &s.autoIdentifyOutput);
        ImGui::SameLine();
        ImGui::TextDisabled("(魔法/稀有品质碑牌需要鉴定，白色物品不需要)");
        ImGui::SliderInt("触发鉴定的原料阈值", &s.identifyMinMaterials, 1, 10);
        ImGui::TextDisabled("当背包原料数低于此值且仓库无料时，自动前往 NPC 鉴定");
    }

    ImGui::Separator();

    // —— 标定数据 ——
    if (ImGui::CollapsingHeader("标定数据 (calib.json)")) {
        if (calib.IsComplete()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            ImGui::TextUnformatted("标定完整 ✓");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextUnformatted("标定不完整:");
            ImGui::TextWrapped("%s", calib.MissingDescription().c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        char benchPath[512] = {};
        std::strncpy(benchPath, calib.benchEntityPath.c_str(), sizeof(benchPath) - 1);
        if (ImGui::InputText("重铸台实体 Path", benchPath, sizeof(benchPath))) {
            calib.benchEntityPath = benchPath;
        }

        char stashPath[512] = {};
        std::strncpy(stashPath, calib.stashEntityPath.c_str(), sizeof(stashPath) - 1);
        if (ImGui::InputText("仓库实体 Path (可选)", stashPath, sizeof(stashPath))) {
            calib.stashEntityPath = stashPath;
        }

        ImGui::Separator();

        ImGui::TextUnformatted("合成按钮:");
        char combineId[256] = {};
        std::strncpy(combineId, calib.combineButtonStringId.c_str(), sizeof(combineId) - 1);
        if (ImGui::InputText("StringId", combineId, sizeof(combineId))) {
            calib.combineButtonStringId = combineId;
        }
        ImGui::SliderInt("坐标 X", &calib.combineButtonX, -1, 3840);
        ImGui::SliderInt("坐标 Y", &calib.combineButtonY, -1, 2160);
        ImGui::Checkbox("强制使用坐标模式", &calib.useManualCoords);

        ImGui::Separator();

        ImGui::TextUnformatted("产物槽:");
        char outputId[256] = {};
        std::strncpy(outputId, calib.outputSlotStringId.c_str(), sizeof(outputId) - 1);
        if (ImGui::InputText("StringId##output", outputId, sizeof(outputId))) {
            calib.outputSlotStringId = outputId;
        }
        ImGui::SliderInt("坐标 X##output", &calib.outputSlotX, -1, 3840);
        ImGui::SliderInt("坐标 Y##output", &calib.outputSlotY, -1, 2160);

        ImGui::Separator();
        ImGui::TextDisabled("—— NPC 鉴定（可选，未标定则不触发自动鉴定）——");

        char npcPath[512] = {};
        std::strncpy(npcPath, calib.npcEntityPath.c_str(), sizeof(npcPath) - 1);
        if (ImGui::InputText("NPC 实体 Path (多利亚尼)", npcPath, sizeof(npcPath))) {
            calib.npcEntityPath = npcPath;
        }

        char dialogId[256] = {};
        std::strncpy(dialogId, calib.npcDialogStringId.c_str(), sizeof(dialogId) - 1);
        if (ImGui::InputText("对话面板 StringId##dialog", dialogId, sizeof(dialogId))) {
            calib.npcDialogStringId = dialogId;
        }

        ImGui::TextUnformatted("鉴定按钮（实时扫描）:");
        char identifyId[256] = {};
        std::strncpy(identifyId, calib.identifyButtonStringId.c_str(), sizeof(identifyId) - 1);
        if (ImGui::InputText("StringId##identify", identifyId, sizeof(identifyId))) {
            calib.identifyButtonStringId = identifyId;
        }
        ImGui::TextDisabled("留空则自动搜索 \"鑑定\" / \"Identify\" 等文字关键词");
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "使用说明：1. 先完成标定（填好重铸台 Path、NPC Path 等）。"
        "2. 鉴定按钮使用实时扫描（自动搜索\"鑑定\"/\"Identify\"文字），无需坐标标定。"
        "3. 确保在藏身处，仓库有可合成的物品。"
        "4. 按 F6 启动自动流程，右键或 Esc 取消。");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
    ImGui::TextWrapped(
        "手动模式：把鼠标移到物品上按 F7，自动执行 Ctrl+右键"
        "（仓库物品→背包，背包物品→仓库）。标定向导中 F7 用于坐标捕获（鉴定按钮已无需捕获）。");
    ImGui::PopStyleColor();
}

} // namespace TabletReforgeUi