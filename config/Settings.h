// Settings.h — 用户偏好配置（settings.json）
//
// 照搬 QuickStash 的 crash-safe JSON 模式：每个字段用 j.value(key, default) 读取，
// 数值字段额外用 std::clamp 限制范围，避免错误配置导致行为异常。
//
// 设计要点：
//   - 所有时序参数都有上下限 clamp，避免用户填 0 或负数导致 SendInput 风暴
//   - maxLoops=0 表示无限循环（用户手动停止）
//   - targetStashTabId=0 表示自动选第一个有先行者碑牌的仓库页
//   - toggleKey 用 Win32 VK_* 宏，默认 F6
#pragma once

#include "../third_party/json.hpp"
#include "AtomicWrite.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TabletReforgeConfig {

// —— 常量 ——
inline constexpr int   kMinUsesMax = 10;   // 最少剩余次数上限
inline constexpr int   kMinMatchedMax = 8;  // 最少匹配词缀数上限
inline constexpr int   kMaxPresets = 32;     // 最大预设数量
inline constexpr std::size_t kMaxPresetNameLen = 63;

// 每类石板的词缀值阈值（0=不限制）
struct ValueRange {
    int min = 0;
    int max = 0;
};

// 仓库页配置类型
enum class StashTabRole : int {
    None = 0,         // 未分配
    Material,         // 原料仓库页（从此页取出原料）
    Special,          // 特殊仓库页（存回产物或其他用途）
    Ignore,           // 忽略此页
};

// 仓库页手动配置（用户手动添加的仓库页）
struct StashTabConfig {
    int inventoryId = 0;               // 仓库页的InventoryId
    std::string name;                   // 显示名称
    StashTabRole role = StashTabRole::None;  // 角色：原料/特殊/忽略
    StashTabRole autoRole = StashTabRole::None;  // 自动归类的角色（仅参考，不覆盖手动设置）
    bool enabled = true;                // 是否启用
    std::string detectedText;           // 从UI树识别到的文本标签（用于验证）
    float clickX = 0;                   // 点击坐标
    float clickY = 0;
    bool hasSubTabs = false;            // 是否有子页
    std::vector<StashTabConfig> subTabs; // 子页配置
    std::set<std::string> keywordHints; // 关键词提示（用于辅助识别）
    
    // 快捷方法
    bool IsMaterial() const { return role == StashTabRole::Material && enabled; }
    bool IsSpecial() const { return role == StashTabRole::Special && enabled; }
    bool IsAutoMaterial() const { return autoRole == StashTabRole::Material && enabled; }
    bool IsAutoSpecial() const { return autoRole == StashTabRole::Special && enabled; }
    
    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["inventory_id"] = inventoryId;
        j["name"] = name;
        j["role"] = static_cast<int>(role);
        j["auto_role"] = static_cast<int>(autoRole);
        j["enabled"] = enabled;
        j["detected_text"] = detectedText;
        j["click_x"] = clickX;
        j["click_y"] = clickY;
        j["has_sub_tabs"] = hasSubTabs;
        
        nlohmann::json kwArr = nlohmann::json::array();
        for (const auto& k : keywordHints) kwArr.push_back(k);
        j["keyword_hints"] = kwArr;
        
        nlohmann::json subArr = nlohmann::json::array();
        for (const auto& sub : subTabs) {
            subArr.push_back(sub.ToJson());
        }
        j["sub_tabs"] = subArr;
        return j;
    }
    
    void FromJson(const nlohmann::json& j) {
        inventoryId = j.value("inventory_id", 0);
        name = j.value("name", std::string(""));
        role = static_cast<StashTabRole>(std::clamp(j.value("role", 0), 0, 3));
        autoRole = static_cast<StashTabRole>(std::clamp(j.value("auto_role", 0), 0, 3));
        enabled = j.value("enabled", true);
        detectedText = j.value("detected_text", std::string(""));
        clickX = j.value("click_x", 0.f);
        clickY = j.value("click_y", 0.f);
        hasSubTabs = j.value("has_sub_tabs", false);
        
        keywordHints.clear();
        if (j.contains("keyword_hints") && j["keyword_hints"].is_array()) {
            for (const auto& v : j["keyword_hints"]) {
                keywordHints.insert(v.get<std::string>());
            }
        }
        
        subTabs.clear();
        if (j.contains("sub_tabs") && j["sub_tabs"].is_array()) {
            for (const auto& sj : j["sub_tabs"]) {
                StashTabConfig sub;
                sub.FromJson(sj);
                subTabs.push_back(std::move(sub));
            }
        }
    }
};

// 常量 - 仓库页角色显示
inline const char* StashTabRoleName(StashTabRole role) {
    switch (role) {
        case StashTabRole::Material: return "原料";
        case StashTabRole::Special:  return "特殊";
        case StashTabRole::Ignore:   return "忽略";
        default:                     return "未分配";
    }
}

// 合成目标（多合成物依次合成）
// 当 activeTargetIndex 指向的目标材料不足3时，自动切换到下一个目标
struct SynthesisTarget {
    std::string name;                  // 目标名称（如 "碑牌合成1"）
    int  itemType = 0;                 // ReforgeItemType
    int  subCategoryId = 0;            // 子类ID
    std::set<std::string> modifierKeys; // 词缀筛选关键词
    bool enabled = true;

    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["name"] = name;
        j["item_type"] = itemType;
        j["sub_category_id"] = subCategoryId;
        j["enabled"] = enabled;
        nlohmann::json kArr = nlohmann::json::array();
        for (const auto& k : modifierKeys) kArr.push_back(k);
        j["modifier_keys"] = kArr;
        return j;
    }

    void FromJson(const nlohmann::json& j) {
        name = j.value("name", name);
        itemType = j.value("item_type", itemType);
        subCategoryId = j.value("sub_category_id", subCategoryId);
        enabled = j.value("enabled", enabled);
        modifierKeys.clear();
        if (j.contains("modifier_keys") && j["modifier_keys"].is_array()) {
            for (const auto& v : j["modifier_keys"]) {
                modifierKeys.insert(v.get<std::string>());
            }
        }
    }
};

// 每类石板的独立配置
struct TypeConfig {
    std::string key;                  // 类型键（如 "Breach"）
    std::string displayName;          // 显示名称
    bool  enabled = true;
    float color[4] = {0.0f, 0.749f, 1.0f, 1.0f};  // 高亮颜色
    int   minUsesLeft = 0;            // 最少剩余次数
    int   minMatchedBonuses = 1;      // 最少可选词缀数
    int   minRequiredBonuses = 1;     // 最少必须词缀数
    std::vector<std::string> selectedBonusIds;  // 选中的词缀（可选池）
    std::vector<std::string> requiredBonusIds;  // 必须词缀（必须池）
    std::unordered_map<std::string, ValueRange> valueFilters;  // 词缀数值范围

    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["key"] = key;
        j["enabled"] = enabled;
        j["color"] = {color[0], color[1], color[2], color[3]};
        j["min_uses_left"] = minUsesLeft;
        j["min_matched_bonuses"] = minMatchedBonuses;
        j["min_required_bonuses"] = minRequiredBonuses;

        nlohmann::json selArr = nlohmann::json::array();
        for (const auto& id : selectedBonusIds) selArr.push_back(id);
        j["selected_bonus_ids"] = selArr;

        nlohmann::json reqArr = nlohmann::json::array();
        for (const auto& id : requiredBonusIds) reqArr.push_back(id);
        j["required_bonus_ids"] = reqArr;

        nlohmann::json vf = nlohmann::json::object();
        for (const auto& kv : valueFilters) {
            if (kv.second.min != 0 || kv.second.max != 0) {
                vf[kv.first] = {kv.second.min, kv.second.max};
            }
        }
        j["value_filters"] = vf;
        return j;
    }

    void FromJson(const nlohmann::json& j) {
        if (j.contains("enabled") && j["enabled"].is_boolean())
            enabled = j["enabled"].get<bool>();
        if (j.contains("min_uses_left") && j["min_uses_left"].is_number_integer())
            minUsesLeft = std::clamp(j["min_uses_left"].get<int>(), 0, kMinUsesMax);
        if (j.contains("min_matched_bonuses") && j["min_matched_bonuses"].is_number_integer())
            minMatchedBonuses = std::clamp(j["min_matched_bonuses"].get<int>(), 1, kMinMatchedMax);
        if (j.contains("min_required_bonuses") && j["min_required_bonuses"].is_number_integer())
            minRequiredBonuses = std::clamp(j["min_required_bonuses"].get<int>(), 1, kMinMatchedMax);
        if (j.contains("color") && j["color"].is_array()) {
            const auto& c = j["color"];
            for (int i = 0; i < 4 && i < static_cast<int>(c.size()); ++i)
                if (c[i].is_number())
                    color[i] = std::clamp(c[i].get<float>(), 0.0f, 1.0f);
        }
        if (j.contains("selected_bonus_ids") && j["selected_bonus_ids"].is_array()) {
            selectedBonusIds.clear();
            for (const auto& id : j["selected_bonus_ids"]) {
                if (!id.is_string()) continue;
                std::string s = id.get<std::string>();
                if (std::find(selectedBonusIds.begin(), selectedBonusIds.end(), s) == selectedBonusIds.end())
                    selectedBonusIds.push_back(std::move(s));
            }
        }
        if (j.contains("required_bonus_ids") && j["required_bonus_ids"].is_array()) {
            requiredBonusIds.clear();
            for (const auto& id : j["required_bonus_ids"]) {
                if (!id.is_string()) continue;
                std::string s = id.get<std::string>();
                if (std::find(selectedBonusIds.begin(), selectedBonusIds.end(), s) != selectedBonusIds.end()
                    && std::find(requiredBonusIds.begin(), requiredBonusIds.end(), s) == requiredBonusIds.end())
                    requiredBonusIds.push_back(std::move(s));
            }
        }
        if (j.contains("value_filters") && j["value_filters"].is_object()) {
            valueFilters.clear();
            for (auto vf = j["value_filters"].begin(); vf != j["value_filters"].end(); ++vf) {
                if (!vf.value().is_array() || vf.value().size() < 2) continue;
                ValueRange r;
                if (vf.value()[0].is_number_integer()) r.min = vf.value()[0].get<int>();
                if (vf.value()[1].is_number_integer()) r.max = vf.value()[1].get<int>();
                if (r.min != 0 || r.max != 0) valueFilters[vf.key()] = r;
            }
        }
    }

    bool HasSelected() const { return !selectedBonusIds.empty(); }
    bool IsRequired(const std::string& id) const {
        return std::find(requiredBonusIds.begin(), requiredBonusIds.end(), id) != requiredBonusIds.end();
    }
    bool IsSelected(const std::string& id) const {
        return std::find(selectedBonusIds.begin(), selectedBonusIds.end(), id) != selectedBonusIds.end();
    }
    void ToggleRequired(const std::string& id) {
        auto it = std::find(requiredBonusIds.begin(), requiredBonusIds.end(), id);
        if (it != requiredBonusIds.end()) {
            requiredBonusIds.erase(it);
        } else {
            if (IsSelected(id))
                requiredBonusIds.push_back(id);
        }
    }
    void ToggleSelected(const std::string& id) {
        auto it = std::find(selectedBonusIds.begin(), selectedBonusIds.end(), id);
        if (it != selectedBonusIds.end()) {
            selectedBonusIds.erase(it);
            auto rit = std::find(requiredBonusIds.begin(), requiredBonusIds.end(), id);
            if (rit != requiredBonusIds.end())
                requiredBonusIds.erase(rit);
        } else {
            selectedBonusIds.push_back(id);
        }
    }
};

// 常量 - 类型展示
inline const std::vector<std::pair<std::string, std::string>>& GetTypeDisplayNames() {
    static const std::vector<std::pair<std::string, std::string>> names = {
        {"Irradiated", "輻照碑牌"},
        {"Breach", "裂痕碑牌"},
        {"Expedition", "探險碑牌"},
        {"Delirium", "譫妄碑牌"},
        {"Abyss", "深淵碑牌"},
        {"Ritual", "祭祀碑牌"},
        {"Overseer", "總督碑牌"},
        {"Temple", "神廟碑牌"},
    };
    return names;
}

inline TypeConfig MakeDefaultType(const std::string& key, const std::string& display) {
    TypeConfig t;
    t.key = key;
    t.displayName = display;
    t.enabled = true;
    t.minUsesLeft = 0;
    t.minMatchedBonuses = 1;
    t.minRequiredBonuses = 1;
    if (key == "Irradiated")   { t.color[0]=0.85f; t.color[1]=0.85f; t.color[2]=0.85f; }
    else if (key == "Breach")  { t.color[0]=0.90f; t.color[1]=0.20f; t.color[2]=0.20f; }
    else if (key == "Expedition") { t.color[0]=0.20f; t.color[1]=0.60f; t.color[2]=0.95f; }
    else if (key == "Delirium"){ t.color[0]=0.20f; t.color[1]=0.80f; t.color[2]=0.80f; }
    else if (key == "Abyss")   { t.color[0]=0.80f; t.color[1]=0.15f; t.color[2]=0.15f; }
    else if (key == "Ritual")  { t.color[0]=0.95f; t.color[1]=0.35f; t.color[2]=0.20f; }
    else if (key == "Overseer"){ t.color[0]=0.95f; t.color[1]=0.80f; t.color[2]=0.20f; }
    else if (key == "Temple")  { t.color[0]=0.30f; t.color[1]=0.80f; t.color[2]=0.30f; }
    return t;
}

inline std::vector<TypeConfig> DefaultTypeConfigs() {
    std::vector<TypeConfig> result;
    for (const auto& [key, display] : GetTypeDisplayNames()) {
        result.push_back(MakeDefaultType(key, display));
    }
    return result;
}

inline TypeConfig* FindTypeConfig(std::vector<TypeConfig>& types, const std::string& key) {
    for (auto& t : types) if (t.key == key) return &t;
    return nullptr;
}

inline const TypeConfig* FindTypeConfig(const std::vector<TypeConfig>& types, const std::string& key) {
    for (const auto& t : types) if (t.key == key) return &t;
    return nullptr;
}

// 重铸台合成支持的物品类型（POE2 重铸台有 89 种配方，这里分大类选择）
// 说明：POE2 重铸台 X of a Kind 规则：3 个完全相同的物品 → 1 个更高阶/更稀有版本
enum class ReforgeItemType : int {
    WaystonesOnly         = 0,   // 地图钥匙（Waystone / 引路石）
    TabletsOnly           = 1,   // 所有碑牌（含 8 种 Augment + 传奇 Mastered Domain）
    AllTablets            = 2,   // 所有碑牌类物品
    JewelsOnly            = 3,   // 仅珠宝（Jewel）
    RunesOnly             = 4,   // 仅符文（Rune / SoulCore）
    EssencesOnly          = 5,   // 仅精髓（Essence）
    LiquidsOnly           = 6,   // 仅情感蒸馏液（Distilled Emotion）
    CatalystsOnly         = 7,   // 仅催化剂（Catalyst / 催化劑，含 Jewellery + Jewel 两种共 26 种）
    CustomKeywords        = 8,   // 自定义关键词（匹配 Path/BaseType 中任意关键词在 customKeywords 里）
};

// 筛选预设：保存一组筛选配置（子类+词缀+类型配置）
struct FilterPreset {
    std::string name;
    std::set<int> subCategories;
    std::set<std::string> bonusIds;
    std::vector<TypeConfig> typeConfigs;  // 各类型的完整配置

    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["name"] = name;
        nlohmann::json subArr = nlohmann::json::array();
        for (int id : subCategories) subArr.push_back(id);
        j["sub_categories"] = subArr;
        nlohmann::json bonusArr = nlohmann::json::array();
        for (auto& id : bonusIds) bonusArr.push_back(id);
        j["bonus_ids"] = bonusArr;
        nlohmann::json typeArr = nlohmann::json::array();
        for (const auto& tc : typeConfigs) {
            nlohmann::json tj = tc.ToJson();
            typeArr.push_back(std::move(tj));
        }
        j["type_configs"] = typeArr;
        return j;
    }

    void FromJson(const nlohmann::json& j) {
        name = j.value("name", std::string("未命名"));
        subCategories.clear();
        if (j.contains("sub_categories") && j["sub_categories"].is_array()) {
            for (const auto& v : j["sub_categories"]) {
                subCategories.insert(v.get<int>());
            }
        }
        bonusIds.clear();
        if (j.contains("bonus_ids") && j["bonus_ids"].is_array()) {
            for (const auto& v : j["bonus_ids"]) {
                bonusIds.insert(v.get<std::string>());
            }
        }
        typeConfigs = DefaultTypeConfigs();
        if (j.contains("type_configs") && j["type_configs"].is_array()) {
            for (const auto& tj : j["type_configs"]) {
                if (!tj.is_object() || !tj.contains("key")) continue;
                std::string key = tj["key"].get<std::string>();
                TypeConfig* tc = FindTypeConfig(typeConfigs, key);
                if (tc) tc->FromJson(tj);
            }
        }
    }
};

struct Settings {
    // —— 总开关与启动 ——
    bool enabled = false;          // 总开关（默认关，避免一加载就跑）
    int  toggleKey = VK_F6;        // 启动/停止热键，默认 F6
    int  maxLoops = 0;             // 最大循环次数，0=无限

    // —— 合成目标物品种类（旧版：按大类+子类选择）——
    int  itemType = static_cast<int>(ReforgeItemType::TabletsOnly);
    int  subCategoryId = 0;
    bool requireIdentified = false;
    bool matchExactItem = true;
    std::string customKeywords = "tablet,waystone,jewel";

    // —— 新版：按子类直接选择 ——
    bool useSubCategoryMode = true;
    std::set<int> selectedSubCategories;
    int  minUnwantedBeforeStop = 3;
    bool autoDepositWanted = true;
    std::set<std::string> selectedBonusIds;

    // —— 词缀筛选模式（核心：基于词缀匹配原料/产物）——
    bool useModifierFilterMode = true;  // 词缀筛选总开关
    std::set<std::string> selectedModifierKeys;  // 用户勾选的词缀关键词（snake_case POE2 mod id）
    bool requireIdentifiedForMaterial = true;  // 原料必须已鉴定（未鉴定→先NPC鉴定）
    int  minRarityForMaterial = 1;  // 原料最低稀有度（0=白, 1=魔法, 2=稀有）

    // ★【方案 B v1.3】合规词缀 Id 读取开关（宪法修正案 v1.3）
    // - false（默认）：方案 A，ExtractModIds 不调用，匹配走 4 参数版 MatchesDesiredReforgeType
    // - true：方案 B，ExtractModIds 调用（只读 Mod.Id+Hash32，禁读 Name/AffixName/StatKey），
    //         匹配走 8 参数版。静默压力测试期间配合 bonusMatchSilent=true
    bool enableBonusMatch = false;
    // ★【方案 B v1.3】静默压力测试模式（仅在 enableBonusMatch=true 时生效）
    // - false：完整启用（含 isMaterial/isProductType 判定变化）
    // - true：ExtractModIds 仍调用并输出日志，但判定走 4 参数版（不影响行为）
    bool bonusMatchSilent = false;

    // —— 各石板类型的独立配置 ——
    std::vector<TypeConfig> typeConfigs = DefaultTypeConfigs();

    // —— 筛选预设管理 ——
    std::vector<FilterPreset> filterPresets;
    int activePresetIndex = -1;
    std::string newPresetName;

    TypeConfig* FindType(const std::string& key) {
        return FindTypeConfig(typeConfigs, key);
    }
    const TypeConfig* FindType(const std::string& key) const {
        return FindTypeConfig(typeConfigs, key);
    }

    FilterPreset GetCurrentPreset() const {
        FilterPreset p;
        p.name = "当前配置";
        p.subCategories = selectedSubCategories;
        p.bonusIds = selectedBonusIds;
        p.typeConfigs = typeConfigs;
        return p;
    }

    void ApplyPreset(int idx) {
        if (idx < 0 || idx >= (int)filterPresets.size()) return;
        activePresetIndex = idx;
        selectedSubCategories = filterPresets[idx].subCategories;
        selectedBonusIds = filterPresets[idx].bonusIds;
        typeConfigs = filterPresets[idx].typeConfigs;
    }

    void AddPreset(const std::string& name) {
        FilterPreset p;
        p.name = name.empty() ? "未命名" : name;
        p.subCategories = selectedSubCategories;
        p.bonusIds = selectedBonusIds;
        p.typeConfigs = typeConfigs;
        filterPresets.push_back(std::move(p));
        activePresetIndex = (int)filterPresets.size() - 1;
    }

    void UpdatePreset(int idx) {
        if (idx < 0 || idx >= (int)filterPresets.size()) return;
        filterPresets[idx].subCategories = selectedSubCategories;
        filterPresets[idx].bonusIds = selectedBonusIds;
        filterPresets[idx].typeConfigs = typeConfigs;
    }

    void DeletePreset(int idx) {
        if (idx < 0 || idx >= (int)filterPresets.size()) return;
        filterPresets.erase(filterPresets.begin() + idx);
        if (activePresetIndex >= (int)filterPresets.size())
            activePresetIndex = (int)filterPresets.size() - 1;
    }

    int PresetCount() const { return (int)filterPresets.size(); }

    // —— 多合成物目标管理 ——
    bool HasActiveTarget() const {
        return activeTargetIndex >= 0 && activeTargetIndex < (int)synthesisTargets.size();
    }

    SynthesisTarget* GetActiveTarget() {
        if (!HasActiveTarget()) return nullptr;
        return &synthesisTargets[activeTargetIndex];
    }

    const SynthesisTarget* GetActiveTarget() const {
        if (!HasActiveTarget()) return nullptr;
        return &synthesisTargets[activeTargetIndex];
    }

    // 将当前激活目标的设置应用到全局（用于状态机读取）
    void ApplyTargetToGlobal() {
        SynthesisTarget* t = GetActiveTarget();
        if (!t || !t->enabled) return;
        itemType = t->itemType;
        subCategoryId = t->subCategoryId;
        selectedModifierKeys = t->modifierKeys;
    }

    // 切换到下一个启用的目标；返回是否切换成功（false=已到末尾）
    bool AdvanceToNextTarget() {
        if (synthesisTargets.empty()) return false;
        int startIdx = activeTargetIndex;
        if (startIdx < 0) startIdx = -1;
        for (int i = startIdx + 1; i < (int)synthesisTargets.size(); ++i) {
            if (synthesisTargets[i].enabled) {
                activeTargetIndex = i;
                ApplyTargetToGlobal();
                return true;
            }
        }
        // 从头开始再扫一遍
        for (int i = 0; i < (int)synthesisTargets.size(); ++i) {
            if (i == startIdx) break;
            if (synthesisTargets[i].enabled) {
                activeTargetIndex = i;
                ApplyTargetToGlobal();
                return true;
            }
        }
        return false;
    }

    void AddSynthesisTarget(const std::string& name) {
        SynthesisTarget t;
        t.name = name.empty() ? "合成目标" + std::to_string(synthesisTargets.size() + 1) : name;
        t.itemType = itemType;
        t.subCategoryId = subCategoryId;
        t.modifierKeys = selectedModifierKeys;
        synthesisTargets.push_back(std::move(t));
        if (activeTargetIndex < 0) activeTargetIndex = 0;
    }

    void DeleteSynthesisTarget(int idx) {
        if (idx < 0 || idx >= (int)synthesisTargets.size()) return;
        synthesisTargets.erase(synthesisTargets.begin() + idx);
        if (activeTargetIndex >= (int)synthesisTargets.size())
            activeTargetIndex = (int)synthesisTargets.size() - 1;
    }

    int SynthesisTargetCount() const { return (int)synthesisTargets.size(); }

    // —— 时序（毫秒）v4：配合批处理架构，目标单步点击 100-250ms ——
    // 单次点击循环 = (clickDelayMs 节流) + WindMouse(相邻~5ms/远距离~20ms) + cursorSettleMs + postClickDelayMs
    // v4 默认总: 30 + ~5 + 3 + 15 = ~53ms / 单步(相邻格)，批处理4个= ~220ms < 目标 300ms
    // 用户设置为极速档: 13+5+1+3 = ~22ms/单步, 4个=~92ms (接近极限人体 300ms)
    int  clickDelayMs = 30;        // 点击节流：距离上次点击的最小间隔 v3=50→v4=30
    int  postClickDelayMs = 15;    // 点击后等待：给 UI 物品转移落地 v3=40→v4=15
    int  cursorSettleMs = 3;       // 光标稳定：命中检测稳定 v3=10→v4=3（Win32 SendInput 有队列）
    int  uiWaitMs = 200;           // 等待 UI 面板出现/消失的轮询间隔
    int  combineWaitMs = 800;      // 点合成按钮后等待产物生成
    int  scanSettleMs = 100;       // Scan(-1) 后等待数据返回
    int  stateTimeoutMs = 30000;   // 单状态最大停留时间，超时则 Abort

    // —— 模拟玩家鼠标轨迹（WindMouse v4：近距离跳过 + 三档距离策略）——
    // 仓库1格约 50-55px，相邻格<80px走简化2-3步，<18px直接瞬移
    // 四档预设（v4）：
    //   极速(追求极限吞吐量)  : gravity=14, wind=1, maxStep=32, wait=0ms  相邻格总耗时~2ms
    //   快速(流水线取物推荐)  : gravity=13, wind=2, maxStep=28, wait=1ms  相邻格总耗时~5ms
    //   平衡(默认，自然感)    : gravity=9,  wind=3, maxStep=15, wait=2ms  远距离原版
    //   精准(防检测远距离)    : gravity=7,  wind=4, maxStep=10, wait=3ms  远距离精调
    bool enableHumanMouse = true;  // 启用模拟玩家鼠标轨迹（从左至右相邻点击）
    int  mouseGravity = 13;        // v4默认：快速档，适合流水线 gravity=13
    int  mouseWind = 2;            // v4默认：wind=2，不太抖但有自然感
    int  mouseMaxStep = 28;        // v4默认：maxStep=28，远距离大步数少
    int  mouseStepWaitMs = 1;      // v4默认：1ms，相邻格约 1*2step=2ms 总耗时

    // —— 品质筛选控制 ——
    bool filterByRarity = true;    // 启用按品质筛选原料（只取出 ≥ minRarityForMaterial 的物品）

    // —— 仓库与背包 ——
    int  targetStashTabId = 0;     // 目标仓库页 ID，0=自动查找（兼容旧版）
    std::set<int> selectedStashTabIds;  // 多选仓库页ID列表（空=自动选第一个）
    std::vector<StashTabConfig> stashTabConfigs;  // 手动配置的仓库页列表（按inventoryId索引，兼容旧版）
    int  reservedBagSlots = 1;     // 背包预留格数（不取碑牌到这里，给产物留位）
    
    // —— 仓库识别模式 ——
    bool autoDetectStashType = true;  // 自动检测仓库类型（视觉识别+默认映射）
    bool enableVisionStashRecognition = true;  // 启用地视觉识别仓库Tab
    bool autoClickScannedStash = true;  // 自动点击扫描到的仓库页（点击切换按钮时使用扫描到的坐标）
    bool preferUiTreeOverVision = true;  // 优先使用UI树识别结果（视觉识别失败时Fallback到UI树）
    bool autoClassifyOnScan = true;       // 扫描仓库时自动执行图标归类（识别每个Tab的仓库类型）
    bool useClassifiedClick = true;     // 切换仓库时优先使用图标归类得到的点击坐标
    
    // —— 图色识别相似度阈值 ——
    // 0.0 = 0% (可能误识别)  1.0 = 100% (严格匹配)
    // 推荐范围: 0.3 - 0.7，过高可能无法识别，过低可能错误识别
    double visionMatchThreshold = 0.5;

    // —— 按排列顺序编号的仓库页配置（用户视角：1..N 号仓库页）——
    // 用户根据仓库页排列顺序（1=最左/第一个，2=第二个...）来指定角色，
    // 与仓库页实际名称无关。点击时根据扫描到的 UI 顺序，将编号映射到 Tab 按钮坐标。
    struct NumberedStashTabConfig {
        int  slotIndex = 0;              // 1-based 编号（第几个仓库页，左→右、上→下）
        StashTabRole role = StashTabRole::None;  // 角色
        bool enabled = true;            // 是否启用（参与扫描/点击）
        // 扫描时填入（只读回显）：
        int  inventoryId = 0;           // 扫描到的 InventoryId
        std::string detectedLabel;      // 扫描到的 Tab 文本标签（中文/英文）
        float clickX = 0;               // 扫描到的 Tab 点击坐标
        float clickY = 0;
        bool hasSubTabs = false;
        std::vector<NumberedStashTabConfig> subTabs; // 子页（如碎片仓库页下的小页）

        bool IsMaterial() const { return role == StashTabRole::Material && enabled; }
        bool IsSpecial()  const { return role == StashTabRole::Special  && enabled; }
        bool IsIgnore()   const { return role == StashTabRole::Ignore   && enabled; }

        nlohmann::json ToJson() const {
            nlohmann::json j;
            j["slot_index"] = slotIndex;
            j["role"] = static_cast<int>(role);
            j["enabled"] = enabled;
            j["inventory_id"] = inventoryId;
            j["detected_label"] = detectedLabel;
            j["click_x"] = clickX;
            j["click_y"] = clickY;
            j["has_sub_tabs"] = hasSubTabs;
            nlohmann::json subArr = nlohmann::json::array();
            for (const auto& s : subTabs) subArr.push_back(s.ToJson());
            j["sub_tabs"] = subArr;
            return j;
        }

        void FromJson(const nlohmann::json& j) {
            slotIndex = j.value("slot_index", slotIndex);
            role = static_cast<StashTabRole>(std::clamp(j.value("role", 0), 0, 3));
            enabled = j.value("enabled", true);
            inventoryId = j.value("inventory_id", 0);
            detectedLabel = j.value("detected_label", std::string(""));
            clickX = j.value("click_x", 0.f);
            clickY = j.value("click_y", 0.f);
            hasSubTabs = j.value("has_sub_tabs", false);
            subTabs.clear();
            if (j.contains("sub_tabs") && j["sub_tabs"].is_array()) {
                for (const auto& sj : j["sub_tabs"]) {
                    NumberedStashTabConfig sub;
                    sub.FromJson(sj);
                    subTabs.push_back(std::move(sub));
                }
            }
        }
    };

    int  numberedStashTabCount = 5;  // 用户可配置的仓库页数量（默认 5，可通过 + 增加）
    std::vector<NumberedStashTabConfig> numberedStashTabs; // 按 slotIndex 顺序存储

    // —— 仓库页配置管理（按编号）——
    void EnsureNumberedStashTabs() {
        // 保证 numberedStashTabs 大小 == numberedStashTabCount
        if ((int)numberedStashTabs.size() < numberedStashTabCount) {
            for (int i = (int)numberedStashTabs.size(); i < numberedStashTabCount; ++i) {
                NumberedStashTabConfig c;
                c.slotIndex = i + 1;
                numberedStashTabs.push_back(std::move(c));
            }
        } else if ((int)numberedStashTabs.size() > numberedStashTabCount) {
            numberedStashTabs.resize(numberedStashTabCount);
        }
        // 强制每一项的 slotIndex == i+1
        for (size_t i = 0; i < numberedStashTabs.size(); ++i) {
            numberedStashTabs[i].slotIndex = (int)i + 1;
        }
    }

    NumberedStashTabConfig* FindNumberedStashTabBySlot(int slotIndex) {
        EnsureNumberedStashTabs();
        for (auto& c : numberedStashTabs) {
            if (c.slotIndex == slotIndex) return &c;
            for (auto& sub : c.subTabs) {
                if (sub.slotIndex == slotIndex) return &sub;
            }
        }
        return nullptr;
    }

    const NumberedStashTabConfig* FindNumberedStashTabBySlot(int slotIndex) const {
        for (const auto& c : numberedStashTabs) {
            if (c.slotIndex == slotIndex) return &c;
            for (auto& sub : c.subTabs) {
                if (sub.slotIndex == slotIndex) return &sub;
            }
        }
        return nullptr;
    }

    // 旧接口（保留兼容，基于 inventoryId 反查编号配置）
    NumberedStashTabConfig* FindNumberedStashTabByInventory(int inventoryId) {
        EnsureNumberedStashTabs();
        for (auto& c : numberedStashTabs) {
            if (c.inventoryId == inventoryId) return &c;
            for (auto& sub : c.subTabs) {
                if (sub.inventoryId == inventoryId) return &sub;
            }
        }
        return nullptr;
    }

    bool IsMaterialTabBySlot(int slotIndex) const {
        const auto* cfg = FindNumberedStashTabBySlot(slotIndex);
        return cfg && cfg->IsMaterial();
    }

    bool IsSpecialTabBySlot(int slotIndex) const {
        const auto* cfg = FindNumberedStashTabBySlot(slotIndex);
        return cfg && cfg->IsSpecial();
    }

    // 获取所有原料仓库页的 slotIndex（包括子页）
    std::set<int> GetMaterialSlotIndices() const {
        std::set<int> ids;
        for (const auto& c : numberedStashTabs) {
            if (c.IsMaterial()) ids.insert(c.slotIndex);
            for (const auto& sub : c.subTabs) {
                if (sub.IsMaterial()) ids.insert(sub.slotIndex);
            }
        }
        return ids;
    }

    // 旧接口（基于 inventoryId）—— 委托给新接口
    StashTabConfig* FindStashTabConfig(int inventoryId) {
        auto* numbered = FindNumberedStashTabByInventory(inventoryId);
        if (!numbered) return nullptr;
        static StashTabConfig tmp;
        tmp.inventoryId = numbered->inventoryId;
        tmp.name = numbered->detectedLabel;
        tmp.role = numbered->role;
        tmp.enabled = numbered->enabled;
        tmp.clickX = numbered->clickX;
        tmp.clickY = numbered->clickY;
        return &tmp;
    }
    const StashTabConfig* FindStashTabConfig(int inventoryId) const {
        auto* self = const_cast<Settings*>(this);
        return self->FindStashTabConfig(inventoryId);
    }

    // 更新或插入仓库页配置（按 slotIndex）
    void UpsertStashTabConfig(const NumberedStashTabConfig& cfg) {
        EnsureNumberedStashTabs();
        auto* existing = FindNumberedStashTabBySlot(cfg.slotIndex);
        if (existing) {
            *existing = cfg;
        } else {
            numberedStashTabs.push_back(cfg);
            std::sort(numberedStashTabs.begin(), numberedStashTabs.end(),
                [](const NumberedStashTabConfig& a, const NumberedStashTabConfig& b) {
                    return a.slotIndex < b.slotIndex;
                });
        }
    }

    // 兼容旧接口：从StashTabConfig转换并插入
    void UpsertStashTabConfig(const StashTabConfig& oldCfg) {
        EnsureNumberedStashTabs();
        // 查找已有编号配置（通过inventoryId匹配）
        NumberedStashTabConfig* existing = FindNumberedStashTabByInventory(oldCfg.inventoryId);
        if (existing) {
            existing->inventoryId = oldCfg.inventoryId;
            existing->detectedLabel = oldCfg.name;
            existing->role = oldCfg.role;
            existing->clickX = oldCfg.clickX;
            existing->clickY = oldCfg.clickY;
        }
    }

    // 兼容旧接口：删除仓库页配置（通过inventoryId）
    bool DeleteStashTabConfig(int inventoryId) {
        EnsureNumberedStashTabs();
        for (auto it = numberedStashTabs.begin(); it != numberedStashTabs.end(); ++it) {
            if (it->inventoryId == inventoryId) {
                numberedStashTabs.erase(it);
                return true;
            }
            // 检查子页
            for (auto sit = it->subTabs.begin(); sit != it->subTabs.end(); ++sit) {
                if (sit->inventoryId == inventoryId) {
                    it->subTabs.erase(sit);
                    return true;
                }
            }
        }
        return false;
    }
    bool IsMaterialTab(int inventoryId) const {
        auto* c = const_cast<Settings*>(this)->FindNumberedStashTabByInventory(inventoryId);
        return c && c->IsMaterial();
    }
    bool IsSpecialTab(int inventoryId) const {
        auto* c = const_cast<Settings*>(this)->FindNumberedStashTabByInventory(inventoryId);
        return c && c->IsSpecial();
    }
    std::set<int> GetMaterialTabIds() const {
        std::set<int> ids;
        for (const auto& c : numberedStashTabs) {
            if (c.IsMaterial() && c.inventoryId != 0) ids.insert(c.inventoryId);
            for (const auto& sub : c.subTabs) {
                if (sub.IsMaterial() && sub.inventoryId != 0) ids.insert(sub.inventoryId);
            }
        }
        return ids;
    }

    int StashTabConfigCount() const {
        int count = 0;
        for (const auto& c : numberedStashTabs) {
            count++;
            count += (int)c.subTabs.size();
        }
        return count;
    }

    // —— 多合成物依次合成 ——
    std::vector<SynthesisTarget> synthesisTargets;  // 合成目标队列
    int activeTargetIndex = -1;    // 当前激活的合成目标索引（-1=使用全局设置）

    // —— 安全门控 ——
    bool gateTownHideout = true;   // 必须在城镇/藏身处
    bool gateEnemyNear = true;     // 附近不能有敌人
    int  enemyRange = 30;          // 敌人检测范围（游戏单位）
    bool gateMenu = false;         // 没有菜单/对话遮挡（默认关闭：插件UI面板会触发此门控）
    bool gateNotForeground = true; // 游戏窗口必须在前台
    bool cancelOnRightClick = true;// 用户右键立即取消
    bool cancelOnEsc = true;       // 用户 Esc 立即取消

    // —— 策略 ——
    bool withdrawRequireIdentified = false;   // 取出只取已鉴定碑牌（原料）——默认 OFF
    bool depositOnlyUnidentified = true;     // 存回只存未鉴定（合成产物）
    bool ctrlSessionMode = true;             // Ctrl 会话级保持（连续操作只按一次）

    // —— 性能与调试 ——
    bool verboseLogging = false;             // 详细日志（默认关闭，开启后记录每个物品的处理步骤）
    bool autoIdentifyOutput = true;          // 合成后自动鉴定产物（魔法/稀有品质）
    int  identifyMinMaterials = 3;           // 背包原料低于此数量时触发鉴定流程

    // —— 文件路径 ——
    std::filesystem::path SettingsPath(const std::filesystem::path& pluginDir) const {
        return pluginDir / "config" / "settings.json";
    }

    // —— 加载（crash-safe：任何字段异常都不影响其余字段）——
    void Load(const std::filesystem::path& pluginDir) {
        const auto path = SettingsPath(pluginDir);
        if (!std::filesystem::exists(path)) return;
        std::ifstream in(path);
        if (!in.is_open()) return;
        try {
            nlohmann::json j;
            in >> j;

            // 总开关
            enabled    = j.value("enabled", enabled);
            toggleKey  = std::clamp(j.value("toggle_key", toggleKey), 0x01, 0xFE);
            maxLoops   = std::clamp(j.value("max_loops", maxLoops), 0, 10000);

            // 时序
            clickDelayMs      = std::clamp(j.value("click_delay_ms", clickDelayMs), 10, 2000);
            postClickDelayMs  = std::clamp(j.value("post_click_delay_ms", postClickDelayMs), 0, 1000);
            cursorSettleMs    = std::clamp(j.value("cursor_settle_ms", cursorSettleMs), 0, 500);
            uiWaitMs          = std::clamp(j.value("ui_wait_ms", uiWaitMs), 50, 2000);
            combineWaitMs     = std::clamp(j.value("combine_wait_ms", combineWaitMs), 100, 5000);
            scanSettleMs      = std::clamp(j.value("scan_settle_ms", scanSettleMs), 0, 1000);
            stateTimeoutMs    = std::clamp(j.value("state_timeout_ms", stateTimeoutMs), 1000, 60000);

            // 模拟玩家鼠标轨迹
            enableHumanMouse  = j.value("enable_human_mouse", enableHumanMouse);
            mouseGravity      = std::clamp(j.value("mouse_gravity", mouseGravity), 1, 50);
            mouseWind         = std::clamp(j.value("mouse_wind", mouseWind), 0, 20);
            mouseMaxStep      = std::clamp(j.value("mouse_max_step", mouseMaxStep), 1, 100);
            mouseStepWaitMs   = std::clamp(j.value("mouse_step_wait_ms", mouseStepWaitMs), 0, 50);

            // 品质筛选控制
            filterByRarity    = j.value("filter_by_rarity", filterByRarity);

            // 仓库与背包
            targetStashTabId  = std::clamp(j.value("target_stash_tab_id", targetStashTabId), 0, 9999);
            reservedBagSlots  = std::clamp(j.value("reserved_bag_slots", reservedBagSlots), 0, 60);
            
            // 仓库识别模式
            autoDetectStashType     = j.value("auto_detect_stash_type", autoDetectStashType);
            enableVisionStashRecognition = j.value("enable_vision_stash_recognition", enableVisionStashRecognition);
            autoClickScannedStash   = j.value("auto_click_scanned_stash", autoClickScannedStash);
            preferUiTreeOverVision  = j.value("prefer_ui_tree_over_vision", preferUiTreeOverVision);
            autoClassifyOnScan = j.value("auto_classify_on_scan", autoClassifyOnScan);
            useClassifiedClick = j.value("use_classified_click", useClassifiedClick);
            visionMatchThreshold = std::clamp(j.value("vision_match_threshold", visionMatchThreshold), 0.0, 1.0);

            selectedStashTabIds.clear();
            if (j.contains("selected_stash_tab_ids") && j["selected_stash_tab_ids"].is_array()) {
                for (const auto& v : j["selected_stash_tab_ids"]) {
                    selectedStashTabIds.insert(v.get<int>());
                }
            }

            // 仓库页手动配置
            stashTabConfigs.clear();
            if (j.contains("stash_tab_configs") && j["stash_tab_configs"].is_array()) {
                for (const auto& tc : j["stash_tab_configs"]) {
                    StashTabConfig cfg;
                    cfg.FromJson(tc);
                    stashTabConfigs.push_back(std::move(cfg));
                }
            }

            // 编号仓库页配置（新系统）
            numberedStashTabCount = std::clamp(j.value("numbered_stash_tab_count", numberedStashTabCount), 1, 50);
            numberedStashTabs.clear();
            if (j.contains("numbered_stash_tabs") && j["numbered_stash_tabs"].is_array()) {
                for (const auto& nt : j["numbered_stash_tabs"]) {
                    NumberedStashTabConfig cfg;
                    cfg.FromJson(nt);
                    numberedStashTabs.push_back(std::move(cfg));
                }
            }
            EnsureNumberedStashTabs();

            // 多合成物目标
            synthesisTargets.clear();
            if (j.contains("synthesis_targets") && j["synthesis_targets"].is_array()) {
                for (const auto& tj : j["synthesis_targets"]) {
                    SynthesisTarget t;
                    t.FromJson(tj);
                    synthesisTargets.push_back(std::move(t));
                }
            }
            activeTargetIndex = std::clamp(j.value("active_target_index", -1), -1, (int)synthesisTargets.size() - 1);

            // 安全门控
            gateTownHideout   = j.value("gate_town_hideout", gateTownHideout);
            gateEnemyNear     = j.value("gate_enemy_near", gateEnemyNear);
            enemyRange        = std::clamp(j.value("enemy_range", enemyRange), 5, 200);
            gateMenu          = j.value("gate_menu", gateMenu);
            gateNotForeground = j.value("gate_not_foreground", gateNotForeground);
            cancelOnRightClick= j.value("cancel_on_right_click", cancelOnRightClick);
            cancelOnEsc       = j.value("cancel_on_esc", cancelOnEsc);

            // 合成物品种类
            itemType            = std::clamp(j.value("item_type", itemType), 0, 8);
            subCategoryId       = j.value("sub_category_id", subCategoryId);
            
            // 验证 subCategoryId 是否在有效范围内
            if (subCategoryId > 0) {
                bool validSubId = false;
                // 检查是否在已知的有效范围内
                if (subCategoryId >= 101 && subCategoryId <= 108) validSubId = true; // Tablets
                else if (subCategoryId >= 201 && subCategoryId <= 206) validSubId = true; // Jewels
                else if (subCategoryId >= 301 && subCategoryId <= 306) validSubId = true; // Runes
                else if (subCategoryId >= 401 && subCategoryId <= 412) validSubId = true; // Essences
                else if (subCategoryId >= 501 && subCategoryId <= 510) validSubId = true; // Emotions
                else if ((subCategoryId >= 601 && subCategoryId <= 613) || (subCategoryId >= 701 && subCategoryId <= 713)) validSubId = true; // Catalysts
                
                if (!validSubId) {
                    subCategoryId = 0; // 重置为0（所有子类）
                }
            }
            
            requireIdentified   = j.value("require_identified", requireIdentified);
            matchExactItem      = j.value("match_exact_item", matchExactItem);
            customKeywords      = j.value("custom_keywords", customKeywords);

            // 新版子类选择
            useSubCategoryMode  = j.value("use_sub_category_mode", useSubCategoryMode);
            minUnwantedBeforeStop = std::clamp(j.value("min_unwanted_before_stop", minUnwantedBeforeStop), 0, 100);
            autoDepositWanted   = j.value("auto_deposit_wanted", autoDepositWanted);
            
            selectedSubCategories.clear();
            if (j.contains("selected_sub_categories") && j["selected_sub_categories"].is_array()) {
                for (const auto& v : j["selected_sub_categories"]) {
                    selectedSubCategories.insert(v.get<int>());
                }
            }

            selectedBonusIds.clear();
            if (j.contains("selected_bonus_ids") && j["selected_bonus_ids"].is_array()) {
                for (const auto& v : j["selected_bonus_ids"]) {
                    selectedBonusIds.insert(v.get<std::string>());
                }
            }

            // 加载各类型配置
            typeConfigs = DefaultTypeConfigs();
            if (j.contains("type_configs") && j["type_configs"].is_array()) {
                for (const auto& tj : j["type_configs"]) {
                    if (!tj.is_object() || !tj.contains("key")) continue;
                    std::string key = tj["key"].get<std::string>();
                    TypeConfig* tc = FindTypeConfig(typeConfigs, key);
                    if (tc) tc->FromJson(tj);
                }
            }

            filterPresets.clear();
            if (j.contains("filter_presets") && j["filter_presets"].is_array()) {
                for (const auto& pj : j["filter_presets"]) {
                    FilterPreset p;
                    p.FromJson(pj);
                    filterPresets.push_back(std::move(p));
                }
            }
            activePresetIndex = std::clamp(j.value("active_preset_index", 0), 0, (int)filterPresets.size() - 1);

            // 策略
            withdrawRequireIdentified = j.value("withdraw_require_identified", withdrawRequireIdentified);
            depositOnlyUnidentified   = j.value("deposit_only_unidentified", depositOnlyUnidentified);
            ctrlSessionMode           = j.value("ctrl_session_mode", ctrlSessionMode);

            // 词缀筛选模式
            useModifierFilterMode      = j.value("use_modifier_filter_mode", useModifierFilterMode);
            requireIdentifiedForMaterial = j.value("require_identified_for_material", requireIdentifiedForMaterial);
            minRarityForMaterial       = std::clamp(j.value("min_rarity_for_material", minRarityForMaterial), 0, 3);
            // 【方案 B v1.3】合规词缀 Id 读取开关
            enableBonusMatch   = j.value("enable_bonus_match", enableBonusMatch);
            bonusMatchSilent   = j.value("bonus_match_silent", bonusMatchSilent);
            
            selectedModifierKeys.clear();
            if (j.contains("selected_modifier_keys") && j["selected_modifier_keys"].is_array()) {
                for (const auto& v : j["selected_modifier_keys"]) {
                    selectedModifierKeys.insert(v.get<std::string>());
                }
            }

            // 性能与调试
            verboseLogging = j.value("verbose_logging", verboseLogging);
            autoIdentifyOutput = j.value("auto_identify_output", autoIdentifyOutput);
            identifyMinMaterials = std::clamp(j.value("identify_min_materials", identifyMinMaterials), 1, 10);
        } catch (...) {
            // JSON 解析失败：保留默认值，不崩
        }
    }

    // —— 保存 ——
    void Save(const std::filesystem::path& pluginDir) const {
        std::error_code ec;
        std::filesystem::create_directories(pluginDir / "config", ec);

        nlohmann::json j;
        j["enabled"]    = enabled;
        j["toggle_key"] = toggleKey;
        j["max_loops"]  = maxLoops;

        j["click_delay_ms"]      = clickDelayMs;
        j["post_click_delay_ms"] = postClickDelayMs;
        j["cursor_settle_ms"]    = cursorSettleMs;
        j["ui_wait_ms"]          = uiWaitMs;
        j["combine_wait_ms"]     = combineWaitMs;
        j["scan_settle_ms"]      = scanSettleMs;
        j["state_timeout_ms"]    = stateTimeoutMs;

        j["enable_human_mouse"]   = enableHumanMouse;
        j["mouse_gravity"]        = mouseGravity;
        j["mouse_wind"]           = mouseWind;
        j["mouse_max_step"]       = mouseMaxStep;
        j["mouse_step_wait_ms"]   = mouseStepWaitMs;

        j["filter_by_rarity"]    = filterByRarity;

        j["target_stash_tab_id"] = targetStashTabId;
        j["reserved_bag_slots"]  = reservedBagSlots;
        
        // 仓库识别模式
        j["auto_detect_stash_type"]      = autoDetectStashType;
        j["enable_vision_stash_recognition"] = enableVisionStashRecognition;
        j["auto_click_scanned_stash"]    = autoClickScannedStash;
        j["prefer_ui_tree_over_vision"]  = preferUiTreeOverVision;
        j["auto_classify_on_scan"]       = autoClassifyOnScan;
        j["use_classified_click"]        = useClassifiedClick;
        j["vision_match_threshold"]      = visionMatchThreshold;

        nlohmann::json stashArr = nlohmann::json::array();
        for (int id : selectedStashTabIds) {
            stashArr.push_back(id);
        }
        j["selected_stash_tab_ids"] = stashArr;

        nlohmann::json tabCfgArr = nlohmann::json::array();
        for (const auto& tc : stashTabConfigs) {
            tabCfgArr.push_back(tc.ToJson());
        }
        j["stash_tab_configs"] = tabCfgArr;

        // 编号仓库页配置保存
        j["numbered_stash_tab_count"] = numberedStashTabCount;
        nlohmann::json numberedArr = nlohmann::json::array();
        const_cast<Settings*>(this)->EnsureNumberedStashTabs();
        for (const auto& nt : numberedStashTabs) {
            numberedArr.push_back(nt.ToJson());
        }
        j["numbered_stash_tabs"] = numberedArr;

        nlohmann::json targetArr = nlohmann::json::array();
        for (const auto& t : synthesisTargets) {
            targetArr.push_back(t.ToJson());
        }
        j["synthesis_targets"] = targetArr;
        j["active_target_index"] = activeTargetIndex;

        j["gate_town_hideout"]    = gateTownHideout;
        j["gate_enemy_near"]      = gateEnemyNear;
        j["enemy_range"]          = enemyRange;
        j["gate_menu"]            = gateMenu;
        j["gate_not_foreground"]  = gateNotForeground;
        j["cancel_on_right_click"]= cancelOnRightClick;
        j["cancel_on_esc"]        = cancelOnEsc;

        j["item_type"]            = itemType;
        j["sub_category_id"]      = subCategoryId;
        j["require_identified"]   = requireIdentified;
        j["match_exact_item"]     = matchExactItem;
        j["custom_keywords"]      = customKeywords;

        j["use_sub_category_mode"]   = useSubCategoryMode;
        j["min_unwanted_before_stop"] = minUnwantedBeforeStop;
        j["auto_deposit_wanted"]     = autoDepositWanted;
        nlohmann::json selArr = nlohmann::json::array();
        for (int id : selectedSubCategories) {
            selArr.push_back(id);
        }
        j["selected_sub_categories"] = selArr;

        nlohmann::json bonusArr = nlohmann::json::array();
        for (auto& id : selectedBonusIds) {
            bonusArr.push_back(id);
        }
        j["selected_bonus_ids"] = bonusArr;

        // 保存各类型配置
        nlohmann::json typeArr = nlohmann::json::array();
        for (const auto& tc : typeConfigs) {
            typeArr.push_back(tc.ToJson());
        }
        j["type_configs"] = typeArr;

        nlohmann::json presetArr = nlohmann::json::array();
        for (const auto& p : filterPresets) {
            presetArr.push_back(p.ToJson());
        }
        j["filter_presets"] = presetArr;
        j["active_preset_index"] = activePresetIndex;

        j["withdraw_require_identified"] = withdrawRequireIdentified;
        j["deposit_only_unidentified"]   = depositOnlyUnidentified;
        j["ctrl_session_mode"]           = ctrlSessionMode;

        // 词缀筛选模式
        j["use_modifier_filter_mode"]       = useModifierFilterMode;
        j["require_identified_for_material"] = requireIdentifiedForMaterial;
        j["min_rarity_for_material"]        = minRarityForMaterial;
        // 【方案 B v1.3】合规词缀 Id 读取开关
        j["enable_bonus_match"]             = enableBonusMatch;
        j["bonus_match_silent"]             = bonusMatchSilent;
        nlohmann::json modArr = nlohmann::json::array();
        for (const auto& k : selectedModifierKeys) {
            modArr.push_back(k);
        }
        j["selected_modifier_keys"] = modArr;

        j["verbose_logging"]             = verboseLogging;
        j["auto_identify_output"]        = autoIdentifyOutput;
        j["identify_min_materials"]      = identifyMinMaterials;

        // 原子写入：tmp → rename，避免半写损坏（参考 AtomicWrite.h）
        const auto target = SettingsPath(pluginDir);
        const std::string content = j.dump(2);
        if (!AtomicWriteText(target, content)) {
            // 原子写失败时也尽量保证不崩，写日志的责任在调用方（无 diag 句柄可用）
            // 兜底直写一次（AtomicWriteText 内部已尝试过，这里不重复，仅留注释）
        }
    }
};

} // namespace TabletReforgeConfig
