// StashItemMapper.h — 物品类型与仓库页的映射系统
//
// 核心功能：
//   1. 建立【合成物品类型】→【仓库页类型】的映射关系
//   2. 支持自动模式（默认映射）、半自动模式（视觉识别+用户确认）、手动模式（用户指定）
//   3. 运行时根据当前合成目标自动定位正确的仓库页
//
// 映射规则：
//   - 碑牌类 (Tablet) → 碎片仓库 (FragmentStash) / 普通仓库 (NormalStash)
//   - 地图钥匙 (Waystone) → 地图仓库 (MapStash)
//   - 货币类 (Currency) → 货币仓库 (CurrencyStash)
//   - 珠宝 (Jewel) → 普通仓库 (NormalStash) / 四方格 (QuadStash)
//   - 精髓 (Essence) → 精髓仓库 (EssenceStash)
//   - 催化剂 (Catalyst) → 货币仓库 (CurrencyStash) / 碎片仓库 (FragmentStash)
//
#pragma once

#include "StashTypeTable.h"
#include "TabletFilter.h"
#include "../sdk/PluginSDK.h"
#include "../config/Settings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// JSON库
#include "../third_party/json.hpp"

namespace TabletReforgeGame {

using TabletReforgeConfig::ReforgeItemType;
using TabletReforgeConfig::StashTabRole;

// ============================================================
// 1. 物品类型枚举（用于映射）
// ============================================================

// 仓库物品分类（比 ReforgeItemType 更细，用于精确映射）
enum class StashItemCategory : int {
    Unknown = 0,
    Tablets,          // 碑牌类
    Waystones,        // 地图钥匙
    Jewels,           // 珠寶 (紅玉/翠綠碧雲/藍玉/鑽石/永恆珠寶/時迭珠寶)
    Runes,            // 符文
    Essences,         // 精髓 (肉體/心智/強化/折磨/烈焰/寒冰/電能/毀滅/戰鬥/巫術/迅捷/無限)
    Liquids,          // 液態情感 (Distilled Emotions - 10种)
    Catalysts,        // 催化劑 (Catalyst - 13种, 影響珠寶飾品)
    RefinedCatalysts, // 精製催化劑 (Refined Catalyst - 13种, 影響珠寶)
    Currency,         // 通用货币（Chaos, Exa 等）
    Fragments,        // 碎片类（Scouring, Regret 等）
    Maps,             // 地图
    Divination,       // 预言卡
    Jewellery,        // 珠寶飾品 (戒指/項鍊)
    Gems,             // 技能宝石
    Flasks,           // 药剂
    Socketable,       // 可镶嵌物品
};

// 物品分类名称（中英文对照）
inline const char* StashItemCategoryName(StashItemCategory cat) {
    switch (cat) {
        case StashItemCategory::Tablets:          return "碑牌 (Tablets)";
        case StashItemCategory::Waystones:        return "地图钥匙 (Waystones)";
        case StashItemCategory::Jewels:           return "珠寶 (Jewels)";
        case StashItemCategory::Runes:            return "符文 (Runes)";
        case StashItemCategory::Essences:         return "精髓 (Essences)";
        case StashItemCategory::Liquids:          return "液態情感 (Liquids)";
        case StashItemCategory::Catalysts:        return "催化劑 (Catalysts)";
        case StashItemCategory::RefinedCatalysts: return "精製催化劑 (Refined Catalysts)";
        case StashItemCategory::Currency:         return "货币 (Currency)";
        case StashItemCategory::Fragments:        return "碎片 (Fragments)";
        case StashItemCategory::Maps:             return "地图 (Maps)";
        case StashItemCategory::Divination:       return "预言卡 (Divination)";
        case StashItemCategory::Jewellery:        return "珠寶飾品 (Jewellery)";
        case StashItemCategory::Gems:             return "技能宝石 (Gems)";
        case StashItemCategory::Flasks:           return "药剂 (Flasks)";
        case StashItemCategory::Socketable:       return "可镶嵌物品 (Socketable)";
        default:                                  return "未知 (Unknown)";
    }
}

// 物品分类选项列表（用于UI下拉选择）
inline const char* GetStashItemCategoryOptions() {
    return "未知 (Unknown)\0碑牌 (Tablets)\0地图钥匙 (Waystones)\0珠寶 (Jewels)\0符文 (Runes)\0精髓 (Essences)\0液態情感 (Liquids)\0催化劑 (Catalysts)\0精製催化劑 (Refined Catalysts)\0货币 (Currency)\0碎片 (Fragments)\0地图 (Maps)\0预言卡 (Divination)\0珠寶飾品 (Jewellery)\0技能宝石 (Gems)\0药剂 (Flasks)\0可镶嵌物品 (Socketable)\0";
}

// 获取物品分类选项数量
inline int GetStashItemCategoryOptionCount() {
    return 17;  // StashItemCategory 枚举的有效项（含Unknown，新增RefinedCatalysts）
}

// ============================================================
// 2. 仓库Tab类型与物品分类的映射配置（支持嵌套结构）
// ============================================================

// 单个仓库Tab的物品分类配置（支持多级嵌套：大仓 → 子仓 → 子子仓）
struct StashTabItemMapping {
    int inventoryId = 0;                          // 仓库页的 InventoryId
    int parentInventoryId = 0;                    // 父级仓库的 InventoryId（0=顶级）
    std::string stashTypeName;                     // 仓库类型名（如 "FragmentStash"）
    int stashTypeId = -1;                          // 仓库类型ID（0-24）
    std::string tabLabel;                          // Tab 标签文本
    std::vector<StashItemCategory> itemCategories;  // 存放的物品分类
    std::string screenshotPath;                    // 截图模板路径（用于图色识别）
    std::vector<StashTabItemMapping> subMappings;  // 子级映射（子仓列表）
    
    // 是否包含指定物品分类
    bool ContainsCategory(StashItemCategory cat) const {
        return std::find(itemCategories.begin(), itemCategories.end(), cat) != itemCategories.end();
    }
    
    // 是否为原料仓库
    bool IsMaterialStash() const {
        return !itemCategories.empty();
    }
    
    // 是否为子页（有父级）
    bool IsSubPage() const {
        return parentInventoryId > 0;
    }
    
    // 是否有子级
    bool HasSubPages() const {
        return !subMappings.empty();
    }
    
    // 是否有截图模板
    bool HasScreenshot() const {
        return !screenshotPath.empty();
    }
    
    // 查找子级映射
    StashTabItemMapping* FindSubMapping(int invId) {
        for (auto& sub : subMappings) {
            if (sub.inventoryId == invId) return &sub;
        }
        return nullptr;
    }
    
    // 查找或创建子级映射
    StashTabItemMapping& GetOrCreateSubMapping(int invId) {
        for (auto& sub : subMappings) {
            if (sub.inventoryId == invId) return sub;
        }
        subMappings.push_back(StashTabItemMapping{});
        subMappings.back().inventoryId = invId;
        subMappings.back().parentInventoryId = inventoryId;
        return subMappings.back();
    }
    
    // 获取所有物品分类（包含子级）
    std::vector<StashItemCategory> GetAllCategories() const {
        std::vector<StashItemCategory> all = itemCategories;
        for (const auto& sub : subMappings) {
            auto subCats = sub.GetAllCategories();
            all.insert(all.end(), subCats.begin(), subCats.end());
        }
        return all;
    }
};

// 仓库映射配置（包含所有仓库页的映射信息）
struct StashMappingConfig {
    std::vector<StashTabItemMapping> tabMappings;  // 所有仓库Tab的映射
    bool useAutoMapping = true;                     // 是否使用自动映射
    std::string defaultStashTypeName;               // 默认仓库类型（用于兜底）
    int defaultStashTypeId = -1;                    // 默认仓库类型ID
    
    // 根据 InventoryId 查找映射（递归搜索子级）
    const StashTabItemMapping* FindByInventoryId(int invId) const {
        for (const auto& m : tabMappings) {
            if (m.inventoryId == invId) return &m;
            // 递归查找子级
            for (const auto& sub : m.subMappings) {
                if (sub.inventoryId == invId) return &sub;
                for (const auto& subSub : sub.subMappings) {
                    if (subSub.inventoryId == invId) return &subSub;
                }
            }
        }
        return nullptr;
    }
    
    // 根据仓库类型查找映射
    const StashTabItemMapping* FindByStashType(const std::string& typeName) const {
        for (const auto& m : tabMappings) {
            if (m.stashTypeName == typeName) return &m;
        }
        return nullptr;
    }
    
    // 查找存放指定物品分类的仓库（递归搜索子级）
    const StashTabItemMapping* FindByCategory(StashItemCategory cat) const {
        for (const auto& m : tabMappings) {
            if (m.ContainsCategory(cat)) return &m;
            // 递归查找子级
            for (const auto& sub : m.subMappings) {
                if (sub.ContainsCategory(cat)) return &sub;
                for (const auto& subSub : sub.subMappings) {
                    if (subSub.ContainsCategory(cat)) return &subSub;
                }
            }
        }
        return nullptr;
    }
    
    // 查找存放多种物品分类的第一个仓库
    const StashTabItemMapping* FindByCategories(const std::vector<StashItemCategory>& cats) const {
        for (const auto& m : tabMappings) {
            for (auto cat : cats) {
                if (m.ContainsCategory(cat)) return &m;
            }
            // 递归查找子级
            for (const auto& sub : m.subMappings) {
                for (auto cat : cats) {
                    if (sub.ContainsCategory(cat)) return &sub;
                }
                for (const auto& subSub : sub.subMappings) {
                    for (auto cat : cats) {
                        if (subSub.ContainsCategory(cat)) return &subSub;
                    }
                }
            }
        }
        return nullptr;
    }
    
    // 序列化单个映射（含子级）
    static nlohmann::json SerializeMapping(const StashTabItemMapping& m) {
        nlohmann::json mj;
        mj["inventory_id"] = m.inventoryId;
        mj["parent_inventory_id"] = m.parentInventoryId;
        mj["stash_type_name"] = m.stashTypeName;
        mj["stash_type_id"] = m.stashTypeId;
        mj["tab_label"] = m.tabLabel;
        mj["screenshot_path"] = m.screenshotPath;
        
        nlohmann::json cats = nlohmann::json::array();
        for (auto cat : m.itemCategories) {
            cats.push_back(static_cast<int>(cat));
        }
        mj["item_categories"] = cats;
        
        // 递归序列化子级
        nlohmann::json subs = nlohmann::json::array();
        for (const auto& sub : m.subMappings) {
            subs.push_back(SerializeMapping(sub));
        }
        mj["sub_mappings"] = subs;
        
        return mj;
    }
    
    // 反序列化单个映射（含子级）
    static void DeserializeMapping(const nlohmann::json& j, StashTabItemMapping& m) {
        m.inventoryId = j.value("inventory_id", 0);
        m.parentInventoryId = j.value("parent_inventory_id", 0);
        m.stashTypeName = j.value("stash_type_name", std::string(""));
        m.stashTypeId = j.value("stash_type_id", -1);
        m.tabLabel = j.value("tab_label", std::string(""));
        m.screenshotPath = j.value("screenshot_path", std::string(""));
        
        if (j.contains("item_categories") && j["item_categories"].is_array()) {
            m.itemCategories.clear();
            for (const auto& c : j["item_categories"]) {
                m.itemCategories.push_back(static_cast<StashItemCategory>(c.get<int>()));
            }
        }
        
        if (j.contains("sub_mappings") && j["sub_mappings"].is_array()) {
            m.subMappings.clear();
            for (const auto& sj : j["sub_mappings"]) {
                StashTabItemMapping sub;
                DeserializeMapping(sj, sub);
                m.subMappings.push_back(std::move(sub));
            }
        }
    }
    
    // 序列化为JSON
    nlohmann::json ToJson() const {
        nlohmann::json j;
        j["use_auto_mapping"] = useAutoMapping;
        j["default_stash_type_name"] = defaultStashTypeName;
        j["default_stash_type_id"] = defaultStashTypeId;
        
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : tabMappings) {
            arr.push_back(SerializeMapping(m));
        }
        j["tab_mappings"] = arr;
        return j;
    }
    
    // 从JSON反序列化
    void FromJson(const nlohmann::json& j) {
        useAutoMapping = j.value("use_auto_mapping", true);
        defaultStashTypeName = j.value("default_stash_type_name", std::string(""));
        defaultStashTypeId = j.value("default_stash_type_id", -1);
        
        tabMappings.clear();
        if (j.contains("tab_mappings") && j["tab_mappings"].is_array()) {
            for (const auto& mj : j["tab_mappings"]) {
                StashTabItemMapping m;
                DeserializeMapping(mj, m);
                tabMappings.push_back(std::move(m));
            }
        }
    }
};

// ============================================================
// 3. 默认映射规则（游戏内建知识）
// ============================================================

// 根据仓库类型名返回默认存放的物品分类
inline std::vector<StashItemCategory> GetDefaultCategoriesForStashType(const std::string& stashTypeName) {
    // 常见仓库类型 → 物品分类映射
    static const std::unordered_map<std::string, std::vector<StashItemCategory>> defaults = {
        // 碎片仓库 - 存放碑牌、催化劑、精製催化劑、碎片等
        {"FragmentStash", {
            StashItemCategory::Tablets,
            StashItemCategory::Catalysts,
            StashItemCategory::RefinedCatalysts,
            StashItemCategory::Fragments,
            StashItemCategory::Runes,
        }},
        
        // 催化剂仓库 - 存放催化劑、精製催化劑
        {"CatalystStash", {
            StashItemCategory::Catalysts,
            StashItemCategory::RefinedCatalysts,
        }},
        
        // 地图仓库 - 存放地图、地图钥匙
        {"MapStash", {
            StashItemCategory::Maps,
            StashItemCategory::Waystones,
        }},
        
        // 货币仓库 - 存放各类货币、精髓、液態情感
        {"CurrencyStash", {
            StashItemCategory::Currency,
            StashItemCategory::Essences,
            StashItemCategory::Liquids,
        }},
        
        // 精髓仓库 - 存放精髓
        {"EssenceStash", {
            StashItemCategory::Essences,
        }},
        
        // 四方格仓库 - 存放珠宝饰品、预言卡等
        {"QuadStash", {
            StashItemCategory::Jewellery,
            StashItemCategory::Divination,
            StashItemCategory::Jewels,
        }},
        
        // 普通仓库 - 通用
        {"NormalStash", {
            StashItemCategory::Tablets,
            StashItemCategory::Jewels,
            StashItemCategory::Currency,
            StashItemCategory::Fragments,
        }},
        
        // 额外仓库类型
        {"UniqueStash",   {StashItemCategory::Jewellery, StashItemCategory::Jewels}},
        {"DivinationCardStash", {StashItemCategory::Divination}},
        {"FlaskStash",    {StashItemCategory::Flasks}},
        {"GemStash",      {StashItemCategory::Gems}},
        {"SocketableStash", {StashItemCategory::Socketable}},
        {"ExpeditionStash", {StashItemCategory::Tablets}},
        {"RitualStash",   {StashItemCategory::Tablets}},
        {"BreachStash",   {StashItemCategory::Tablets, StashItemCategory::Catalysts, StashItemCategory::RefinedCatalysts}},
        {"AbyssStash",    {StashItemCategory::Tablets}},
        {"DelveStash",    {StashItemCategory::Tablets}},
        {"BlightStash",   {StashItemCategory::Tablets}},
    };
    
    auto it = defaults.find(stashTypeName);
    if (it != defaults.end()) return it->second;
    
    // 未知类型默认通用
    return {
        StashItemCategory::Tablets,
        StashItemCategory::Currency,
        StashItemCategory::Fragments,
    };
}

// 根据 ReforgeItemType 返回可能的 StashItemCategory 列表
inline std::vector<StashItemCategory> GetCategoriesForReforgeType(ReforgeItemType type) {
    switch (type) {
        case ReforgeItemType::WaystonesOnly:
            return {StashItemCategory::Waystones, StashItemCategory::Maps};
            
        case ReforgeItemType::TabletsOnly:
        case ReforgeItemType::AllTablets:
            return {
                StashItemCategory::Tablets,
                StashItemCategory::Catalysts,
                StashItemCategory::Runes,
            };
            
        case ReforgeItemType::JewelsOnly:
            return {StashItemCategory::Jewels, StashItemCategory::Jewellery};
            
        case ReforgeItemType::RunesOnly:
            return {StashItemCategory::Runes, StashItemCategory::Tablets};
            
        case ReforgeItemType::EssencesOnly:
            return {StashItemCategory::Essences, StashItemCategory::Currency};
            
        case ReforgeItemType::LiquidsOnly:
            return {StashItemCategory::Liquids, StashItemCategory::Currency};
            
        case ReforgeItemType::CatalystsOnly:
            return {StashItemCategory::Catalysts, StashItemCategory::RefinedCatalysts, StashItemCategory::Fragments};
            
        case ReforgeItemType::CustomKeywords:
            return {
                StashItemCategory::Tablets,
                StashItemCategory::Currency,
                StashItemCategory::Fragments,
                StashItemCategory::Jewels,
                StashItemCategory::Catalysts,
            };
            
        default:
            return {StashItemCategory::Tablets};
    }
}

// ============================================================
// 4. 物品 Path → 物品分类识别
// ============================================================

// 根据物品路径判断物品分类
inline StashItemCategory IdentifyItemCategoryByPath(const std::string& path) {
    std::string lowerPath = path;
    for (char& c : lowerPath) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    
    // 碑牌类
    if (lowerPath.find("toweraugment") != std::string::npos ||
        lowerPath.find("precursor") != std::string::npos ||
        lowerPath.find("mastereddomain") != std::string::npos) {
        return StashItemCategory::Tablets;
    }
    
    // 地图钥匙
    if (lowerPath.find("mapkey") != std::string::npos ||
        lowerPath.find("waystone") != std::string::npos) {
        return StashItemCategory::Waystones;
    }
    
    // 珠宝
    if (lowerPath.find("jewel") != std::string::npos &&
        lowerPath.find("jewelcatalyst") == std::string::npos) {
        return StashItemCategory::Jewels;
    }
    
    // 符文
    if (lowerPath.find("soulcore") != std::string::npos ||
        lowerPath.find("runefire") != std::string::npos ||
        lowerPath.find("runecold") != std::string::npos) {
        return StashItemCategory::Runes;
    }
    
    // 精髓
    if (lowerPath.find("currencyessence") != std::string::npos) {
        return StashItemCategory::Essences;
    }
    
    // 液态情感
    if (lowerPath.find("distilledemotion") != std::string::npos) {
        return StashItemCategory::Liquids;
    }
    
    // 精製催化劑 (Jewel 用，影響珠寶)
    if (lowerPath.find("currencyjewelquality") != std::string::npos) {
        return StashItemCategory::RefinedCatalysts;
    }
    
    // 催化劑 (Jewellery 用，影響戒指/項鍊)
    if (lowerPath.find("currencyjewelleryquality") != std::string::npos) {
        return StashItemCategory::Catalysts;
    }
    
    // 通用催化劑（兜底匹配）
    if (lowerPath.find("catalyst") != std::string::npos) {
        return StashItemCategory::Catalysts;
    }
    
    // 通用货币
    if (lowerPath.find("currency") != std::string::npos) {
        return StashItemCategory::Currency;
    }
    
    // 碎片
    if (lowerPath.find("fragment") != std::string::npos ||
        lowerPath.find("scouring") != std::string::npos ||
        lowerPath.find("regret") != std::string::npos) {
        return StashItemCategory::Fragments;
    }
    
    return StashItemCategory::Unknown;
}

// ============================================================
// 5. 映射管理器（核心）
// ============================================================

class StashMappingManager {
public:
    // 获取单例
    static StashMappingManager& Instance() {
        static StashMappingManager instance;
        return instance;
    }
    
    // 初始化：从设置和仓库类型表构建映射
    void Initialize(
        const PluginSDK::Context* ctx,
        const TabletReforgeConfig::Settings& settings,
        const std::vector<PluginSDK::Inventory>& inventories,
        const std::filesystem::path& pluginDir)
    {
        config_.tabMappings.clear();
        config_.useAutoMapping = settings.autoDetectStashType;
        
        // 为每个 inventory 创建映射条目
        for (const auto& inv : inventories) {
            if (inv.Address == 0) continue;
            // ★ 修复：不再要求 inv.Grid.Valid！
            // Grid.Valid 只对当前屏幕上显示的那一页仓库为 true。
            // 如果只纳入 Grid.Valid 的Inventory，那么当前不可见的其他仓库Tab
            // （bug1.log中第一次扫描31页，第二次仅2页）会被完全排除在映射之外，
            // 导致状态机无法切换到其他页，表现为"仅两个标签"。
            // 代价：非当前页的Inventory没有屏幕坐标，但 StashItemMapper 只做类型映射，
            //       实际点击由 StashOps::ClickStashTabBySlotIndex/ClickStashTabV2 负责。
            // if (!inv.Grid.Valid) continue;  // 已移除：过度过滤
            if (inv.TotalBoxesX * inv.TotalBoxesY < 4) continue;  // 太小的忽略

            // 综合过滤：装备槽位 + 非仓库Tab的 Inventory_NNN（基于 ggpk 格子尺寸数据）
            // 这一步修复 bug1.log 中 StashMapping 阶段把 BodyArmour1/Weapon1 等装备槽位
            // 误纳入仓库映射的问题。
            const char* invNameC = ctx ? ctx->Inventory.GetName(inv.InventoryId) : nullptr;
            std::string invNameStr = invNameC ? invNameC : "";

            // ★ 补充：排除主背包 MainInventory
            // bug1.log 中 MainInventory1（id=1）也被纳入了仓库映射，
            // 但主背包不能作为"切换仓库页"的目标（点击它不会切换到仓库页），
            // 导致状态机切换到#1时失败（"切换仓库 #1 useClassified=1 失败"）。
            if (invNameStr.rfind("MainInventory", 0) == 0) {
                continue;
            }

            if (IsNonStashInventory(invNameStr, inv.TotalBoxesX, inv.TotalBoxesY, inv.InventoryId)) {
                continue;
            }

            StashTabItemMapping mapping;
            mapping.inventoryId = inv.InventoryId;

            // 复用前面已获取的 invNameStr（避免重定义）
            // 查找仓库类型
            const auto* stashType = FindStashTypeById(invNameStr);
            if (stashType) {
                mapping.stashTypeName = stashType->id;
                mapping.stashTypeId = stashType->stashId;
                
                // 使用默认物品分类映射
                mapping.itemCategories = GetDefaultCategoriesForStashType(stashType->id);
            } else {
                // 尝试用名字匹配
                mapping.stashTypeName = invNameStr;
                mapping.stashTypeId = -1;
                
                // 根据名字推断分类
                mapping.itemCategories = GetDefaultCategoriesForStashType(invNameStr);
            }
            
            config_.tabMappings.push_back(std::move(mapping));
        }
        
        OutputDebugStringA(("[StashMapping] 初始化完成: " + 
            std::to_string(config_.tabMappings.size()) + " 个仓库页映射\n").c_str());
    }
    
    // 根据 ReforgeItemType 查找第一个可用的原料仓库
    const StashTabItemMapping* FindMaterialStash(ReforgeItemType itemType) const {
        auto categories = GetCategoriesForReforgeType(itemType);
        return config_.FindByCategories(categories);
    }
    
    // 根据物品路径查找应该存放的仓库
    const StashTabItemMapping* FindStashForItem(const std::string& itemPath) const {
        auto cat = IdentifyItemCategoryByPath(itemPath);
        if (cat == StashItemCategory::Unknown) return nullptr;
        return config_.FindByCategory(cat);
    }
    
    // 根据 InventoryId 查找仓库映射
    const StashTabItemMapping* FindByInventoryId(int invId) const {
        return config_.FindByInventoryId(invId);
    }
    
    // 获取所有映射
    const StashMappingConfig& GetConfig() const { return config_; }

    // 可写访问配置（供UI在扫描后应用图标归类结果）
    StashMappingConfig& GetConfigMutable() { return config_; }
    
    // 日志输出当前映射状态
    void LogMappingStatus() const {
        OutputDebugStringA("[StashMapping] ===== 仓库映射状态 =====\n");
        
        for (const auto& m : config_.tabMappings) {
            char buf[256];
            std::string cats;
            for (auto cat : m.itemCategories) {
                if (!cats.empty()) cats += ", ";
                cats += StashItemCategoryName(cat);
            }
            
            sprintf_s(buf,
                "[StashMapping]   InventoryId=%d, Type=%s (id=%d), Categories=[%s]\n",
                m.inventoryId,
                m.stashTypeName.c_str(),
                m.stashTypeId,
                cats.c_str());
            OutputDebugStringA(buf);
        }
        
        OutputDebugStringA("[StashMapping] =======================\n");
    }
    
    // 更新指定仓库的物品分类映射
    void UpdateMapping(int inventoryId, const std::vector<StashItemCategory>& categories) {
        for (auto& m : config_.tabMappings) {
            if (m.inventoryId == inventoryId) {
                m.itemCategories = categories;
                OutputDebugStringA(("[StashMapping] 已更新仓库 " + 
                    std::to_string(inventoryId) + " 的物品分类映射\n").c_str());
                return;
            }
        }
    }
    
    // 设置仓库类型（用于视觉识别后的确认）
    void SetStashType(int inventoryId, const std::string& stashTypeName) {
        for (auto& m : config_.tabMappings) {
            if (m.inventoryId == inventoryId) {
                auto* type = FindStashTypeById(stashTypeName);
                if (type) {
                    m.stashTypeName = type->id;
                    m.stashTypeId = type->stashId;
                    m.itemCategories = GetDefaultCategoriesForStashType(type->id);
                    
                    char buf[256];
                    sprintf_s(buf,
                        "[StashMapping] 已设置仓库 %d 类型为 %s (id=%d)\n",
                        inventoryId, type->id, type->stashId);
                    OutputDebugStringA(buf);
                }
                return;
            }
        }
    }
    
    // 设置仓库类型（带stashTypeId参数）
    void SetStashType(int inventoryId, const std::string& stashTypeName, int stashTypeId) {
        for (auto& m : config_.tabMappings) {
            if (m.inventoryId == inventoryId) {
                m.stashTypeName = stashTypeName;
                m.stashTypeId = stashTypeId;
                
                // 如果是AutoDetect，不清空分类；否则更新默认分类
                if (stashTypeId >= 0) {
                    auto* type = FindStashTypeByStashId(stashTypeId);
                    if (type) {
                        m.stashTypeName = type->id;
                        m.itemCategories = GetDefaultCategoriesForStashType(type->id);
                    }
                }
                
                char buf[256];
                sprintf_s(buf,
                    "[StashMapping] 已设置仓库 %d 类型为 %s (id=%d)\n",
                    inventoryId, m.stashTypeName.c_str(), m.stashTypeId);
                OutputDebugStringA(buf);
                return;
            }
        }
    }
    
    // 添加或更新当前扫描到的仓库（只扫描当前打开的仓库）
    void AddOrUpdateCurrentStash(
        const PluginSDK::Context* ctx,
        const TabletReforgeConfig::Settings& settings,
        const PluginSDK::Inventory& currentInv,
        const std::filesystem::path& pluginDir)
    {
        const char* invNameC = ctx ? ctx->Inventory.GetName(currentInv.InventoryId) : nullptr;
        std::string invName = invNameC ? invNameC : "";
        
        // 检查是否已存在该inventoryId的映射
        for (auto& m : config_.tabMappings) {
            if (m.inventoryId == currentInv.InventoryId) {
                // 更新现有映射
                if (!invName.empty()) {
                    auto* type = FindStashTypeById(invName);
                    if (type) {
                        m.stashTypeName = type->id;
                        m.stashTypeId = type->stashId;
                    } else {
                        m.stashTypeName = invName;
                    }
                }
                
                char buf[256];
                sprintf_s(buf,
                    "[StashMapping] 更新仓库 %d (%s) 映射\n",
                    currentInv.InventoryId, invName.c_str());
                OutputDebugStringA(buf);
                return;
            }
        }
        
        // 添加新映射
        StashTabItemMapping mapping;
        mapping.inventoryId = currentInv.InventoryId;
        mapping.tabLabel = invName;
        
        if (!invName.empty()) {
            auto* type = FindStashTypeById(invName);
            if (type) {
                mapping.stashTypeName = type->id;
                mapping.stashTypeId = type->stashId;
                mapping.itemCategories = GetDefaultCategoriesForStashType(type->id);
            } else {
                mapping.stashTypeName = invName;
                mapping.stashTypeId = -1;
                mapping.itemCategories = GetDefaultCategoriesForStashType(invName);
            }
        }
        
        config_.tabMappings.push_back(std::move(mapping));
        
        char buf[256];
        sprintf_s(buf,
            "[StashMapping] 新增仓库 %d (%s) 映射\n",
            currentInv.InventoryId, invName.c_str());
        OutputDebugStringA(buf);
    }
    
    // 清空所有配置
    void ClearConfig() {
        config_.tabMappings.clear();
        config_.useAutoMapping = false;
        config_.defaultStashTypeName = "";
        config_.defaultStashTypeId = 0;
        OutputDebugStringA("[StashMapping] 已清空所有仓库映射配置\n");
    }
    
    // 保存映射配置到文件
    bool SaveToFile(const std::filesystem::path& pluginDir) const {
        try {
            std::filesystem::path configDir = pluginDir / "config";
            std::filesystem::create_directories(configDir);
            
            std::filesystem::path filePath = configDir / "stash_mapping.json";
            
            nlohmann::json j = config_.ToJson();
            
            std::ofstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                OutputDebugStringA("[StashMapping] 保存失败: 无法打开文件\n");
                return false;
            }
            
            file << j.dump(4);
            file.close();
            
            char log[256];
            sprintf_s(log, "[StashMapping] 映射配置已保存到: %s\n", filePath.string().c_str());
            OutputDebugStringA(log);
            return true;
        } catch (const std::exception& e) {
            char log[256];
            sprintf_s(log, "[StashMapping] 保存异常: %s\n", e.what());
            OutputDebugStringA(log);
            return false;
        }
    }
    
    // 从文件加载映射配置
    bool LoadFromFile(const std::filesystem::path& pluginDir) {
        try {
            std::filesystem::path filePath = pluginDir / "config" / "stash_mapping.json";
            
            if (!std::filesystem::exists(filePath)) {
                OutputDebugStringA("[StashMapping] 配置文件不存在，跳过加载\n");
                return false;
            }
            
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                OutputDebugStringA("[StashMapping] 加载失败: 无法打开文件\n");
                return false;
            }
            
            nlohmann::json j;
            file >> j;
            file.close();
            
            config_.FromJson(j);
            
            char log[256];
            sprintf_s(log, "[StashMapping] 已加载 %zu 个仓库映射\n", config_.tabMappings.size());
            OutputDebugStringA(log);
            return true;
        } catch (const std::exception& e) {
            char log[256];
            sprintf_s(log, "[StashMapping] 加载异常: %s\n", e.what());
            OutputDebugStringA(log);
            return false;
        }
    }

private:
    StashMappingConfig config_;
};

// ============================================================
// 6. 便捷函数：查找合成目标仓库
// ============================================================

// 根据当前设置查找应该扫描的仓库页列表
inline std::vector<int> GetTargetStashInventoryIds(
    const TabletReforgeConfig::Settings& settings,
    ReforgeItemType itemType)
{
    std::vector<int> result;
    auto& manager = StashMappingManager::Instance();
    const auto& config = manager.GetConfig();
    
    // 首先检查用户手动配置的仓库页
    if (!settings.selectedStashTabIds.empty()) {
        result.assign(settings.selectedStashTabIds.begin(), settings.selectedStashTabIds.end());
        return result;
    }
    
    // 如果启用了按角色配置
    if (!settings.numberedStashTabs.empty()) {
        for (const auto& tab : settings.numberedStashTabs) {
            if (tab.IsMaterial() && tab.inventoryId > 0) {
                result.push_back(tab.inventoryId);
            }
        }
        if (!result.empty()) return result;
    }
    
    // 否则使用自动映射
    auto* stash = manager.FindMaterialStash(itemType);
    if (stash) {
        result.push_back(stash->inventoryId);
    }
    
    return result;
}

// 获取指定仓库的物品分类信息
inline std::string GetStashCategoriesString(int inventoryId) {
    auto& manager = StashMappingManager::Instance();
    auto* mapping = manager.FindByInventoryId(inventoryId);
    if (!mapping) return "未知";
    
    std::string result;
    for (auto cat : mapping->itemCategories) {
        if (!result.empty()) result += ", ";
        result += StashItemCategoryName(cat);
    }
    return result;
}

// ============================================================
// 7. 视觉识别辅助：将视觉结果与仓库映射关联
// ============================================================

// 视觉识别仓库类型后的处理结果
struct VisionStashMappingResult {
    int inventoryId = 0;
    int stashTypeId = -1;
    std::string stashTypeName;
    double confidence = 0;
    std::vector<StashItemCategory> detectedCategories;
    bool mappingUpdated = false;
};

// 处理视觉识别结果：更新仓库映射并返回识别信息
inline VisionStashMappingResult ProcessVisionStashRecognition(
    int inventoryId,
    const std::string& detectedStashName,
    double confidence)
{
    VisionStashMappingResult result;
    result.inventoryId = inventoryId;
    result.confidence = confidence;
    
    // 查找仓库类型
    auto* stashType = FindStashTypeById(detectedStashName);
    if (stashType) {
        result.stashTypeId = stashType->stashId;
        result.stashTypeName = stashType->id;
        result.detectedCategories = GetDefaultCategoriesForStashType(stashType->id);
        
        // 更新映射
        auto& manager = StashMappingManager::Instance();
        manager.SetStashType(inventoryId, stashType->id);
        result.mappingUpdated = true;
    } else {
        result.stashTypeName = detectedStashName;
        result.detectedCategories = GetDefaultCategoriesForStashType(detectedStashName);
    }
    
    // 日志输出
    char buf[512];
    std::string cats;
    for (auto cat : result.detectedCategories) {
        if (!cats.empty()) cats += ", ";
        cats += StashItemCategoryName(cat);
    }
    
    sprintf_s(buf,
        "[StashMapping][Vision] 视觉识别结果: invId=%d, type=%s, conf=%.4f, categories=[%s], updated=%d\n",
        inventoryId,
        result.stashTypeName.c_str(),
        confidence,
        cats.c_str(),
        result.mappingUpdated ? 1 : 0);
    OutputDebugStringA(buf);
    
    return result;
}

} // namespace TabletReforgeGame
