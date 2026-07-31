// TabletFilter.h — 三合一物品识别（基于 POE2 实际数据的精确匹配）
//
// POE2 可合成物品识别规则（来自 baseitemtypes.json）：
//
// 【地图钥匙 Waystone (引路石)】（Tier 1~16）
//   Path: Metadata/Items/Maps/MapKeyTier1..16
//   BaseType: "Waystone (Tier X)"
//   3 → 1 合成：3 个相同 Tier → 1 个更高 Tier
//
// 【先行者碑牌 Precursor Tablet / 輻照碑牌 / Irradiated Tablet】（8 种 Augment + 传奇 Mastered Domain）
//   Path: Metadata/Items/TowerAugment/BreachAugment     → "Breach Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/ExpeditionAugment → "Expedition Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/DeliriumAugment   → "Delirium Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/RitualAugment     → "Ritual Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/GenericAugment    → "Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/MapBossAugment    → "Overseer Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/AbyssAugment     → "Abyss Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/IncursionAugment → "Incursion Precursor Tablet"
//   Path: Metadata/Items/TowerAugment/MasteredDomain    → "Mastered Domain" (传奇，6 种地貌)
//   注意：父级路径 Metadata/Items/TowerAugments/TowerAugment（带 s）
//   3 → 1 合成：3 个相同类型 → 1 个更高阶版本
//
// 【珠宝 Jewel】（普通 + Time-Lost 变体）
//   Path: Metadata/Items/Jewels/JewelStr → "Ruby"
//   Path: Metadata/Items/Jewels/JewelDex → "Emerald"
//   Path: Metadata/Items/Jewels/JewelInt → "Sapphire"
//   Path: Metadata/Items/Jewels/JewelDiamond → "Diamond"
//   Path: Metadata/Items/Jewels/JewelTimeless → "Timeless Jewel"
//   Path: Metadata/Items/Jewels/JewelRadiusStr/Dex/Int/Diamond → "Time-Lost Xxx"
//   3 → 1 合成
//
// 【符文 Rune / SoulCore】
//   Path: Metadata/Items/SoulCores/RuneFire → "Desert Rune"
//   Path: Metadata/Items/SoulCores/RuneCold → "Glacial Rune"
//   Path: Metadata/Items/SoulCores/SoulCore* → 各类符文
//   3 → 1 合成
//
// 【精髓 Essence】
//   Path: Metadata/Items/Currency/CurrencyEssenceLife → "Essence of the Body"
//   3 → 1 合成
//
// 【情感蒸馏液 Distilled Emotion】
//   Path: Metadata/Items/Currency/DistilledEmotion1..10 → "Distilled Ire/Guilt/Greed..."
//   3 → 1 合成
//
// 【催化剂 Catalyst / 催化劑】（26 种）
//   Jewellery 用催化剂 (13 种): Path Metadata/Items/Currency/BreachCatalyst*
//   Jewel 用精製催化剂 (13 种): Path Metadata/Items/Currency/JewelCatalyst*
//   3 → 1 合成
//
// 安全：只用 Path + BaseTypeName + Rarity + IsIdentified，绝不调 ReadItemMods。
#pragma once

#include "../config/Settings.h"
#include "TabletBonusCatalog.h"
#include "StashTypeTable.h"  // FindStashTypeById / IsLikelyStashTabByGridSize（ggpk 解包数据）

#include <cctype>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace TabletReforgeGame {

using TabletReforgeConfig::ReforgeItemType;

inline constexpr int kRarityNormal  = 0;
inline constexpr int kRarityMagic   = 1;
inline constexpr int kRarityRare    = 2;
inline constexpr int kRarityUnique  = 3;

// —— 同步 TypeConfig.selectedBonusIds 到全局 selectedModifierKeys ——
// 解决 UI 勾选类型词缀后实际筛选未生效的脱节问题
inline void SyncBonusIdsToModifierKeys(TabletReforgeConfig::Settings& settings) {
    using namespace detail;

    // 先收集所有已有 bonus id → Bonus 的映射，加速查找
    std::unordered_map<std::string, Bonus> bonusById;

    auto indexCatalog = [&](const std::vector<Bonus>& list) {
        for (const auto& b : list) {
            bonusById.emplace(b.Id, b);
            bonusById.emplace(b.NormId, b);  // NormId 也能查
        }
    };
    // 索引通用词缀
    indexCatalog(CommonBonuses());
    // 索引所有石板类型
    const auto* s = settings.FindType("Breach");
    if (s) indexCatalog(GetBonusesForType("Breach"));
    indexCatalog(GetBonusesForType("Delirium"));
    indexCatalog(GetBonusesForType("Ritual"));
    indexCatalog(GetBonusesForType("Overseer"));
    indexCatalog(GetBonusesForType("Abyss"));
    indexCatalog(GetBonusesForType("Temple"));
    indexCatalog(GetBonusesForType("Irradiated"));
    indexCatalog(GetBonusesForType("Expedition"));

    // 从所有 TypeConfig.selectedBonusIds / requiredBonusIds 中收集
    std::vector<std::string> allBonusIds;
    for (const auto& tc : settings.typeConfigs) {
        for (const auto& id : tc.selectedBonusIds) allBonusIds.push_back(id);
        for (const auto& id : tc.requiredBonusIds) allBonusIds.push_back(id);
    }
    // ★ 修复：也收集全局 settings.selectedBonusIds（通用词缀通过此路径添加）
    for (const auto& id : settings.selectedBonusIds) {
        allBonusIds.push_back(id);
    }

    // 汇总到全局 selectedModifierKeys
    size_t before = settings.selectedModifierKeys.size();
    for (const auto& id : allBonusIds) {
        // 先插入本身（bonus id 原文）
        settings.selectedModifierKeys.insert(id);
        settings.selectedModifierKeys.insert(ToLowerCopy(id));

        // 查找 catalog 拿到 Label/NormId/NormIdStripped/Category
        auto it = bonusById.find(id);
        if (it == bonusById.end()) {
            // 用 NormalizeIdentifier 后的 id 再查一次
            std::string norm = NormalizeIdentifier(id);
            it = bonusById.find(norm);
        }
        if (it != bonusById.end()) {
            const Bonus& b = it->second;
            settings.selectedModifierKeys.insert(b.Id);
            settings.selectedModifierKeys.insert(b.Label);
            settings.selectedModifierKeys.insert(ToLowerCopy(b.Label));
            settings.selectedModifierKeys.insert(b.NormId);
            if (!b.NormIdStripped.empty() && b.NormIdStripped != b.NormId)
                settings.selectedModifierKeys.insert(b.NormIdStripped);
            if (!b.Category.empty()) {
                settings.selectedModifierKeys.insert(b.Category);
                settings.selectedModifierKeys.insert(ToLowerCopy(b.Category));
            }
            // ★ 关键修复：加入 POE2 游戏 mod id (snake_case) 关键词
            // 解决 Bonus.Id (CamelCase) 与游戏 ReadItemMods 返回的 mod id (snake_case)
            // 命名空间不一致导致子串匹配永远失败的问题
            auto poe2Keywords = GetPoe2ModIdKeywords(b.Id);
            for (const auto& kw : poe2Keywords) {
                settings.selectedModifierKeys.insert(kw);
                settings.selectedModifierKeys.insert(ToLowerCopy(kw));
            }
        }
    }
    size_t after = settings.selectedModifierKeys.size();

    // 强制 useModifierFilterMode=ON 如果有任何选中的词缀
    if (after > 0 && !settings.useModifierFilterMode) {
        settings.useModifierFilterMode = true;
    }
    if (after != before) {
        std::string logBuf = "[SyncBonusIds] sync done: typeConfigs bonus_ids="
            + std::to_string(allBonusIds.size())
            + ", modifierKeys " + std::to_string(before)
            + "->" + std::to_string(after)
            + ", useModifierFilterMode=" + std::to_string(settings.useModifierFilterMode ? 1 : 0)
            + "\n";
        OutputDebugStringA(logBuf.c_str());
    }
}

// —— 检测 BaseType 是否有效（不是乱码或无效字符）——
inline bool IsValidBaseType(std::string_view bt) {
    if (bt.empty()) return false;
    if (bt.size() < 3) return false;
    int validChars = 0;
    int questionMarks = 0;
    for (char c : bt) {
        if (c == '?') questionMarks++;
        if ((c >= 32 && c <= 126) || c == ' ' || c == '-') validChars++;
    }
    if (questionMarks > 0 && questionMarks >= static_cast<int>(bt.size()) / 3) return false;
    if (validChars < static_cast<int>(bt.size()) / 2) return false;
    return true;
}

// ============================================================
// 装备槽位过滤（参考 StashUtilityCore.cs + bug1.log 实际数据）
// Inventory API 返回的 inventoryId 包含角色装备槽位（Weapon1/2, Offhand1/2,
// Helm1, BodyArmour1 等），这些不是仓库Tab，必须过滤掉。
// 注：原定义在 StashOps.h，移至此处以便 InventoryChecker.h 等基础头文件复用。
// ============================================================
inline bool IsEquipmentSlotName(const std::string& name) {
    if (name.empty()) return false;

    // 角色装备槽位（精确匹配）
    static const char* kExactEquipSlots[] = {
        "Weapon1", "Weapon2", "Weapon3",
        "Offhand1", "Offhand2", "Offhand3",
        "Helm1", "BodyArmour1", "Gloves1", "Boots1", "Belt1",
        "Ring1", "Ring2", "Amulet1",
        "Flask1", "Flask2", "Flask3", "Flask4", "Flask5",
        "Cursor1", "PassiveJewels1", "AnimatedArmour1",
        "SkillSlots1", "Trinket1", "GuildTag1", "StashInventoryId",
        "TalismanTrade", "Leaguestone1", "Relics1",
        "DivinationCardTrade", "Darkshrine", "BestiaryCrafting",
        "IncursionSacrifice", "Unveiling1",
        "ItemSynthesisInput", "ItemSynthesisOutput",
        "BlightCraftingItem", "BlightCraftingInput",
        "AtlasUpgradesStorage", "AtlasUpgrades1",
        "ExpeditionMapMission", "ExpeditionDeal1",
        "HeistBlueprintMission", "HeistContractMission", "HeistStorage1",
        "RitualSavedRewards1",
        "DelveCraftingItem",
        "MobileHeldMapsInventory1", "MobileMapInventory1",
        "MemoryLineMaps", "RelicStorage1",
        "SanctumSpecialRelic1", "CurrentSanctumRun1",
        "ThreeToOneInput", "ThreeToOneOutput",
        "HarvestCraftingItem", "HellscapeModificationInventory1",
        "SentinelDroneInventory1", "SentinelStorage1",
        "LakeTabletInventory1",
        "UltimatumCraftingItem",
        "MobileSkillGemCrafting1",
        "Currency1", "MapCurrency1",
        "UNUSED1", "UNUSED2",
        "Map1",  // 地图槽位（bug1.log 中 invId=14 name='Map1' 被误识别为仓库Tab）
    };
    for (const char* slot : kExactEquipSlots) {
        if (name == slot) return true;
    }

    // 模式匹配（包含关键字）
    static const char* kPatternSlots[] = {
        "MasterCrafting",    // StrMasterCrafting, DexMasterCrafting 等
        "HeistNpcEquipment", // HeistNpcEquipment1-9
        "MercenaryCompanion",// MercenaryCompanionHelm1 等
        "DONOTUSE",          // DONOTUSE1-7
        "UNUSED",            // UNUSED1/2
    };
    for (const char* pat : kPatternSlots) {
        if (name.find(pat) != std::string::npos) return true;
    }

    return false;
}

// ============================================================
// 综合判断：一个 Inventory 是否"不是仓库Tab"（应被过滤掉）
// 结合名称匹配和 ggpk 格子尺寸数据，不依赖 "Inventory" 字符串过滤。
//
// 过滤策略（按优先级）：
//   1. 主背包（MainInventory*）→ 保留（返回 false，不是非仓库）
//   2. 已知仓库类型名（NormalStash/CurrencyStash 等）→ 保留
//   3. 装备槽位（Weapon1/BodyArmour1/MobileSkillGemCrafting1 等）→ 过滤
//   4. Inventory_NNN 动态命名：检查格子尺寸是否匹配 ggpk 仓库Tab规格
//      - 匹配 → 保留（是自定义命名的仓库Tab）
//      - 不匹配 → 过滤（是装备槽位或其他面板）
//   5. 其他未知名称：检查格子尺寸是否匹配 ggpk 仓库Tab规格
//      - 匹配 → 保留
//      - 不匹配 → 过滤
//
// 参数：
//   name      - Inventory.GetName() 返回的名称
//   width     - inv.TotalBoxesX
//   height    - inv.TotalBoxesY
// 返回：true = 应过滤（不是仓库Tab），false = 应保留（是仓库Tab或主背包）
// ============================================================
inline bool IsNonStashInventory(const std::string& name, int width, int height) {
    // 1. 主背包保留
    if (name.rfind("MainInventory", 0) == 0) return false;

    // 2. 已知仓库类型名保留（NormalStash, FragmentStash, CurrencyStash 等）
    if (FindStashTypeById(name) != nullptr) return false;

    // 3. 装备槽位过滤（名称精确匹配或模式匹配）
    if (IsEquipmentSlotName(name)) return true;

    // 3.5 ★ 修复：不再一刀切过滤 Inventory_- 开头的名称！
    // bug1.log 中 Inventory_-2147483648 的尺寸是 12x12=144 slots，完全匹配 NormalStash，
    // 这很可能是游戏内部给仓库Tab分配的特殊负数ID（SDK内部实现细节），不是装备槽位。
    // 如果一刀切过滤，会丢失大量真实仓库Tab。
    // 负数ID Inventory的判断改为依赖"先过名称装备槽位，再过格子尺寸"两步综合判断。
    // （名称含负数本身不构成过滤条件，因为装备槽位名称如 Weapon1/BodyArmour1 已经被 step 3 过滤）

    // 4 & 5. 基于格子尺寸判断（依赖 ggpk 解包的 stashtype 数据 + 启发式大尺寸匹配）
    // 仓库Tab有固定的格子规格（如 12x12, 53x4, 24x24 等），
    // 装备槽位和其他小面板的格子尺寸不会通过 IsLikelyStashTabByGridSize 判断。
    if (!IsLikelyStashTabByGridSize(width, height)) {
        return true;  // 格子尺寸不匹配任何仓库Tab → 过滤
    }

    return false;  // 格子尺寸匹配仓库Tab → 保留
}

// ============================================================
// 增强版：带 InventoryId 的综合过滤
// 对 Inventory_NNN 动态命名增加 InventoryId 辅助判断：
//   - InventoryId == 0 → 可能是无效 ID，但不强制过滤（依赖名称+格子尺寸）
//   - InventoryId < 0 → 不强制过滤！12x12=144 slots 的负数ID Inventory也可能是仓库Tab
//   - 其他规则同 IsNonStashInventory
//
// ★ 核心变化：判断依据从"ID符号"转移到"名称是否属于装备槽位 + 格子尺寸是否足够大"。
//   这样即使SDK返回奇怪的ID（负数/零），只要Inventory名称不是装备槽位且尺寸足够大，
//   就判定为真实仓库Tab，不丢失数据。
// ============================================================
inline bool IsNonStashInventory(const std::string& name, int width, int height, int inventoryId) {
    // 1. 主背包保留
    if (name.rfind("MainInventory", 0) == 0) return false;

    // 2. 已知仓库类型名保留
    if (FindStashTypeById(name) != nullptr) return false;

    // 3. 装备槽位过滤（名称精确匹配或模式匹配）
    if (IsEquipmentSlotName(name)) return true;

    // 4. ★ 修复：InventoryId 不再作为硬过滤条件！
    // bug1.log 中 invId=-2147483648 的尺寸为 12x12=144，完全匹配 NormalStash 规格，
    // 这说明负数ID也可能是真实仓库Tab（SDK内部hash溢出或特殊编码）。
    // 装备槽位已经通过 step 3 名称过滤了，不会走到这一步。
    // if (inventoryId < 0) return true;  // 已移除：过度过滤

    // 5. Inventory_ 开头但名称带负数的，也不强制过滤（原因同上）
    // if (name.rfind("Inventory_-", 0) == 0) return true;  // 已移除：过度过滤

    // 6 & 7. 基于格子尺寸判断（ggpk规格 + 启发式大尺寸匹配）
    if (!IsLikelyStashTabByGridSize(width, height)) {
        return true;  // 格子尺寸不匹配 → 过滤
    }

    return false;  // 格子尺寸匹配 → 保留
}

// —— 大小写不敏感的子串查找（窄字符串，用于 InventoryItem.Path）——
inline bool ContainsCI(std::string_view hay, std::string_view needleLower) {
    if (needleLower.empty()) return true;
    if (needleLower.size() > hay.size()) return false;
    for (size_t i = 0; i + needleLower.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < needleLower.size(); ++j) {
            const char c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            if (c != needleLower[j]) break;
        }
        if (j == needleLower.size()) return true;
    }
    return false;
}

// —— 大小写不敏感的子串查找（宽字符串，用于 Entity.Path）——
inline bool ContainsCIW(std::wstring_view hay, std::wstring_view needleLower) {
    if (needleLower.empty()) return true;
    if (needleLower.size() > hay.size()) return false;
    for (size_t i = 0; i + needleLower.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < needleLower.size(); ++j) {
            const wchar_t c = static_cast<wchar_t>(
                std::towlower(hay[i + j]));
            if (c != needleLower[j]) break;
        }
        if (j == needleLower.size()) return true;
    }
    return false;
}

// —— 把 "a,b,c" 形式的字符串按逗号切分成关键词列表（小写）——
inline std::vector<std::string> SplitKeywords(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : csv) {
        if (ch == ',') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ============================================================
// POE2 物品识别：基于 baseitemtypes.json 数据的精确匹配
// ============================================================

// POE2 所有可三合一物品的路径模式（用于精确匹配）
// 数据来源: F:\Trae\chuxue\Source code\poe2-data-main\data\baseitemtypes.json
struct Poe2ItemPattern {
    const char* pathKeyword;
    const char* baseTypeKeyword;
    ReforgeItemType category;
    const char* displayName;
    const char* chineseName;
    int subCategoryId;
};

// 子类 ID 定义
enum class TabletSubCategory {
    None = 0,
    // 先行者碑牌子类 (Precursor Tablet)
    IrradiatedTablet = 101,
    BreachTablet = 102,
    ExpeditionTablet = 103,
    DeliriumTablet = 104,
    AbyssTablet = 105,
    RitualTablet = 106,
    OverseerTablet = 107,
    TempleTablet = 108,
    // 珠宝子类
    RubyJewel = 201,
    EmeraldJewel = 202,
    SapphireJewel = 203,
    DiamondJewel = 204,
    TimelessJewel = 205,
    TimeLostJewel = 206,
    // 符文子类
    FireRune = 301,
    ColdRune = 302,
    LightningRune = 303,
    EnhanceRune = 304,
    LifeRune = 305,
    ManaRune = 306,
    // 精髓子类
    LifeEssence = 401,
    ManaEssence = 402,
    DefencesEssence = 403,
    PhysicalEssence = 404,
    FireEssence = 405,
    ColdEssence = 406,
    LightningEssence = 407,
    ChaosEssence = 408,
    AttackEssence = 409,
    CasterEssence = 410,
    SpeedEssence = 411,
    AttributeEssence = 412,
    // 液态情感子类 (Liquid Emotion)
    DistilledIre = 501,
    DistilledGuilt = 502,
    DistilledGreed = 503,
    DistilledParanoia = 504,
    DistilledEnvy = 505,
    DistilledDisgust = 506,
    DistilledDespair = 507,
    DistilledFear = 508,
    DistilledSuffering = 509,
    DistilledIsolation = 510,
    // 催化剂子类 (Catalyst - Jewellery 用, 影响戒指/项链)
    CatalystLife = 601,
    CatalystMana = 602,
    CatalystDefences = 603,
    CatalystPhysical = 604,
    CatalystFire = 605,
    CatalystCold = 606,
    CatalystLightning = 607,
    CatalystChaos = 608,
    CatalystAttack = 609,
    CatalystCaster = 610,
    CatalystSpeed = 611,
    CatalystAttribute = 612,
    CatalystNecrotic = 613,
    // 催化剂子类 (Refined Catalyst - Jewel 用, 影响珠宝)
    RefinedCatalystLife = 701,
    RefinedCatalystMana = 702,
    RefinedCatalystDefences = 703,
    RefinedCatalystPhysical = 704,
    RefinedCatalystFire = 705,
    RefinedCatalystCold = 706,
    RefinedCatalystLightning = 707,
    RefinedCatalystChaos = 708,
    RefinedCatalystAttack = 709,
    RefinedCatalystCaster = 710,
    RefinedCatalystSpeed = 711,
    RefinedCatalystAttribute = 712,
    RefinedCatalystNecrotic = 713,
};

// 子类信息结构
struct SubCategoryInfo {
    int id;
    ReforgeItemType parentCategory;
    const char* englishName;
    const char* chineseName;
};

inline const SubCategoryInfo* GetSubCategories(int& count) {
    static const SubCategoryInfo subs[] = {
        // 先行者碑牌子类 (Precursor Tablet)
        {static_cast<int>(TabletSubCategory::IrradiatedTablet), ReforgeItemType::TabletsOnly, "Irradiated Precursor Tablet (輻照碑牌)", "輻照碑牌"},
        {static_cast<int>(TabletSubCategory::BreachTablet), ReforgeItemType::TabletsOnly, "Breach Precursor Tablet (裂痕碑牌)", "裂痕碑牌"},
        {static_cast<int>(TabletSubCategory::ExpeditionTablet), ReforgeItemType::TabletsOnly, "Expedition Precursor Tablet (探險碑牌)", "探險碑牌"},
        {static_cast<int>(TabletSubCategory::DeliriumTablet), ReforgeItemType::TabletsOnly, "Delirium Precursor Tablet (譫妄碑牌)", "譫妄碑牌"},
        {static_cast<int>(TabletSubCategory::AbyssTablet), ReforgeItemType::TabletsOnly, "Abyss Precursor Tablet (深淵碑牌)", "深淵碑牌"},
        {static_cast<int>(TabletSubCategory::RitualTablet), ReforgeItemType::TabletsOnly, "Ritual Precursor Tablet (祭祀碑牌)", "祭祀碑牌"},
        {static_cast<int>(TabletSubCategory::OverseerTablet), ReforgeItemType::TabletsOnly, "Overseer Precursor Tablet (總督碑牌)", "總督碑牌"},
        {static_cast<int>(TabletSubCategory::TempleTablet), ReforgeItemType::TabletsOnly, "Temple Precursor Tablet (神廟碑牌)", "神廟碑牌"},
        // 珠宝子类（官方繁體中文：紅玉/翠綠碧雲/藍玉/鑽石/永恆珠寶/時迭珠寶）
        {static_cast<int>(TabletSubCategory::RubyJewel), ReforgeItemType::JewelsOnly, "Ruby Jewel (紅玉)", "紅玉"},
        {static_cast<int>(TabletSubCategory::EmeraldJewel), ReforgeItemType::JewelsOnly, "Emerald Jewel (翠綠碧雲)", "翠綠碧雲"},
        {static_cast<int>(TabletSubCategory::SapphireJewel), ReforgeItemType::JewelsOnly, "Sapphire Jewel (藍玉)", "藍玉"},
        {static_cast<int>(TabletSubCategory::DiamondJewel), ReforgeItemType::JewelsOnly, "Diamond Jewel (鑽石)", "鑽石"},
        {static_cast<int>(TabletSubCategory::TimelessJewel), ReforgeItemType::JewelsOnly, "Timeless Jewel (永恆珠寶)", "永恆珠寶"},
        {static_cast<int>(TabletSubCategory::TimeLostJewel), ReforgeItemType::JewelsOnly, "Time-Lost Jewel (時迭珠寶)", "時迭珠寶"},
        // 符文子类
        {static_cast<int>(TabletSubCategory::FireRune), ReforgeItemType::RunesOnly, "Fire Rune (沙漠符文)", "沙漠符文"},
        {static_cast<int>(TabletSubCategory::ColdRune), ReforgeItemType::RunesOnly, "Cold Rune (冰川符文)", "冰川符文"},
        {static_cast<int>(TabletSubCategory::LightningRune), ReforgeItemType::RunesOnly, "Lightning Rune (暴風符文)", "暴風符文"},
        {static_cast<int>(TabletSubCategory::EnhanceRune), ReforgeItemType::RunesOnly, "Enhance Rune (鍛鐵符文)", "鍛鐵符文"},
        {static_cast<int>(TabletSubCategory::LifeRune), ReforgeItemType::RunesOnly, "Body Rune (肉體符文)", "肉體符文"},
        {static_cast<int>(TabletSubCategory::ManaRune), ReforgeItemType::RunesOnly, "Mind Rune (心靈符文)", "心靈符文"},
        // 精髓子类
        {static_cast<int>(TabletSubCategory::LifeEssence), ReforgeItemType::EssencesOnly, "Life Essence (肉體精髓)", "肉體精髓"},
        {static_cast<int>(TabletSubCategory::ManaEssence), ReforgeItemType::EssencesOnly, "Mana Essence (心智精髓)", "心智精髓"},
        {static_cast<int>(TabletSubCategory::DefencesEssence), ReforgeItemType::EssencesOnly, "Defences Essence (強化精髓)", "強化精髓"},
        {static_cast<int>(TabletSubCategory::PhysicalEssence), ReforgeItemType::EssencesOnly, "Physical Essence (磨礪精髓)", "磨礪精髓"},
        {static_cast<int>(TabletSubCategory::FireEssence), ReforgeItemType::EssencesOnly, "Fire Essence (烈焰精髓)", "烈焰精髓"},
        {static_cast<int>(TabletSubCategory::ColdEssence), ReforgeItemType::EssencesOnly, "Cold Essence (寒冰精髓)", "寒冰精髓"},
        {static_cast<int>(TabletSubCategory::LightningEssence), ReforgeItemType::EssencesOnly, "Lightning Essence (電能精髓)", "電能精髓"},
        {static_cast<int>(TabletSubCategory::ChaosEssence), ReforgeItemType::EssencesOnly, "Chaos Essence (毀滅精髓)", "毀滅精髓"},
        {static_cast<int>(TabletSubCategory::AttackEssence), ReforgeItemType::EssencesOnly, "Attack Essence (戰鬥精髓)", "戰鬥精髓"},
        {static_cast<int>(TabletSubCategory::CasterEssence), ReforgeItemType::EssencesOnly, "Caster Essence (巫術精髓)", "巫術精髓"},
        {static_cast<int>(TabletSubCategory::SpeedEssence), ReforgeItemType::EssencesOnly, "Speed Essence (迅捷精髓)", "迅捷精髓"},
        {static_cast<int>(TabletSubCategory::AttributeEssence), ReforgeItemType::EssencesOnly, "Attribute Essence (無限精髓)", "無限精髓"},
        // 液态情感子类 (Liquid Emotions)
        {static_cast<int>(TabletSubCategory::DistilledIre), ReforgeItemType::LiquidsOnly, "Distilled Ire (液態憤怒)", "液態憤怒"},
        {static_cast<int>(TabletSubCategory::DistilledGuilt), ReforgeItemType::LiquidsOnly, "Distilled Guilt (液態罪孽)", "液態罪孽"},
        {static_cast<int>(TabletSubCategory::DistilledGreed), ReforgeItemType::LiquidsOnly, "Distilled Greed (液態貪婪)", "液態貪婪"},
        {static_cast<int>(TabletSubCategory::DistilledParanoia), ReforgeItemType::LiquidsOnly, "Distilled Paranoia (液態偏執)", "液態偏執"},
        {static_cast<int>(TabletSubCategory::DistilledEnvy), ReforgeItemType::LiquidsOnly, "Distilled Envy (液態忌妒)", "液態忌妒"},
        {static_cast<int>(TabletSubCategory::DistilledDisgust), ReforgeItemType::LiquidsOnly, "Distilled Disgust (液態厭惡)", "液態厭惡"},
        {static_cast<int>(TabletSubCategory::DistilledDespair), ReforgeItemType::LiquidsOnly, "Distilled Despair (液態絕望)", "液態絕望"},
        {static_cast<int>(TabletSubCategory::DistilledFear), ReforgeItemType::LiquidsOnly, "Distilled Fear (液態恐懼)", "液態恐懼"},
        {static_cast<int>(TabletSubCategory::DistilledSuffering), ReforgeItemType::LiquidsOnly, "Distilled Suffering (液態苦難)", "液態苦難"},
        {static_cast<int>(TabletSubCategory::DistilledIsolation), ReforgeItemType::LiquidsOnly, "Distilled Isolation (液態孤立)", "液態孤立"},
        // 催化剂子类 (Catalyst)
        {static_cast<int>(TabletSubCategory::CatalystLife), ReforgeItemType::CatalystsOnly, "Flesh Catalyst (血肉催化劑)", "血肉催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystMana), ReforgeItemType::CatalystsOnly, "Neural Catalyst (神經催化劑)", "神經催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystDefences), ReforgeItemType::CatalystsOnly, "Carapace Catalyst (甲殼催化劑)", "甲殼催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystPhysical), ReforgeItemType::CatalystsOnly, "Uul-Netol's Catalyst (烏爾尼多催化劑)", "烏爾尼多催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystFire), ReforgeItemType::CatalystsOnly, "Xoph's Catalyst (索伏催化劑)", "索伏催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystCold), ReforgeItemType::CatalystsOnly, "Tuls' Catalyst (托沃催化劑)", "托沃催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystLightning), ReforgeItemType::CatalystsOnly, "Esh's Catalyst (艾許催化劑)", "艾許催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystChaos), ReforgeItemType::CatalystsOnly, "Chayula's Catalyst (夏烏拉催化劑)", "夏烏拉催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystAttack), ReforgeItemType::CatalystsOnly, "Reaver Catalyst (掠奪催化劑)", "掠奪催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystCaster), ReforgeItemType::CatalystsOnly, "Sibilant Catalyst (嘶鳴催化劑)", "嘶鳴催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystSpeed), ReforgeItemType::CatalystsOnly, "Skittering Catalyst (飛掠催化劑)", "飛掠催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystAttribute), ReforgeItemType::CatalystsOnly, "Adaptive Catalyst (適性催化劑)", "適性催化劑"},
        {static_cast<int>(TabletSubCategory::CatalystNecrotic), ReforgeItemType::CatalystsOnly, "Necrotic Catalyst (死靈催化劑)", "死靈催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystLife), ReforgeItemType::CatalystsOnly, "Refined Flesh Catalyst (精製血肉催化劑)", "精製血肉催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystMana), ReforgeItemType::CatalystsOnly, "Refined Neural Catalyst (精製神經催化劑)", "精製神經催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystDefences), ReforgeItemType::CatalystsOnly, "Refined Carapace Catalyst (精製甲殼催化劑)", "精製甲殼催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystPhysical), ReforgeItemType::CatalystsOnly, "Refined Uul-Netol's Catalyst (精製烏爾尼多催化劑)", "精製烏爾尼多催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystFire), ReforgeItemType::CatalystsOnly, "Refined Xoph's Catalyst (精製索伏催化劑)", "精製索伏催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystCold), ReforgeItemType::CatalystsOnly, "Refined Tuls' Catalyst (精製托沃催化劑)", "精製托沃催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystLightning), ReforgeItemType::CatalystsOnly, "Refined Esh's Catalyst (精製艾許催化劑)", "精製艾許催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystChaos), ReforgeItemType::CatalystsOnly, "Refined Chayula's Catalyst (精製夏烏拉催化劑)", "精製夏烏拉催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystAttack), ReforgeItemType::CatalystsOnly, "Refined Reaver Catalyst (精製掠奪催化劑)", "精製掠奪催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystCaster), ReforgeItemType::CatalystsOnly, "Refined Sibilant Catalyst (精製嘶鳴催化劑)", "精製嘶鳴催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystSpeed), ReforgeItemType::CatalystsOnly, "Refined Skittering Catalyst (精製飛掠催化劑)", "精製飛掠催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystAttribute), ReforgeItemType::CatalystsOnly, "Refined Adaptive Catalyst (精製適性催化劑)", "精製適性催化劑"},
        {static_cast<int>(TabletSubCategory::RefinedCatalystNecrotic), ReforgeItemType::CatalystsOnly, "Refined Necrotic Catalyst (精製死靈催化劑)", "精製死靈催化劑"},
    };
    count = sizeof(subs) / sizeof(subs[0]);
    return subs;
}

inline const Poe2ItemPattern* GetPoe2Patterns(int& count) {
    static const Poe2ItemPattern patterns[] = {
        // === 地图钥匙 Waystone (引路石) ===
        {"mapkey", "", ReforgeItemType::WaystonesOnly, "Waystone (MapKey)", "地圖鑰匙", 0},
        {"Metadata/Items/Maps", "waystone", ReforgeItemType::WaystonesOnly, "Waystone (Maps+BT)", "地圖鑰匙（Maps+BT）", 0},
        {"", "waystone", ReforgeItemType::WaystonesOnly, "Waystone", "地圖鑰匙", 0},

        // === 先行者碑牌 Precursor Tablet ===
        // Path+BT 精确匹配
        {"Metadata/Items/TowerAugment/GenericAugment", "Irradiated Tablet", ReforgeItemType::TabletsOnly, "Irradiated Tablet", "輻照碑牌", 101},
        {"Metadata/Items/TowerAugment/BreachAugment", "Breach Tablet", ReforgeItemType::TabletsOnly, "Breach Tablet", "裂痕碑牌", 102},
        {"Metadata/Items/TowerAugment/ExpeditionAugment", "Expedition Tablet", ReforgeItemType::TabletsOnly, "Expedition Tablet", "探險碑牌", 103},
        {"Metadata/Items/TowerAugment/DeliriumAugment", "Delirium Tablet", ReforgeItemType::TabletsOnly, "Delirium Tablet", "譫妄碑牌", 104},
        {"Metadata/Items/TowerAugment/AbyssAugment", "Abyss Tablet", ReforgeItemType::TabletsOnly, "Abyss Tablet", "深淵碑牌", 105},
        {"Metadata/Items/TowerAugment/RitualAugment", "Ritual Tablet", ReforgeItemType::TabletsOnly, "Ritual Tablet", "祭祀碑牌", 106},
        {"Metadata/Items/TowerAugment/MapBossAugment", "Overseer Tablet", ReforgeItemType::TabletsOnly, "Overseer Tablet", "總督碑牌", 107},
        {"Metadata/Items/TowerAugment/IncursionAugment", "Temple Tablet", ReforgeItemType::TabletsOnly, "Temple Tablet", "神廟碑牌", 108},
        // Path-only 精确匹配（BT 不可用时使用）
        {"Metadata/Items/TowerAugment/GenericAugment", "", ReforgeItemType::TabletsOnly, "Irradiated (path)", "輻照碑牌（路徑）", 101},
        {"Metadata/Items/TowerAugment/BreachAugment", "", ReforgeItemType::TabletsOnly, "Breach (path)", "裂痕碑牌（路徑）", 102},
        {"Metadata/Items/TowerAugment/ExpeditionAugment", "", ReforgeItemType::TabletsOnly, "Expedition (path)", "探險碑牌（路徑）", 103},
        {"Metadata/Items/TowerAugment/DeliriumAugment", "", ReforgeItemType::TabletsOnly, "Delirium (path)", "譫妄碑牌（路徑）", 104},
        {"Metadata/Items/TowerAugment/AbyssAugment", "", ReforgeItemType::TabletsOnly, "Abyss (path)", "深淵碑牌（路徑）", 105},
        {"Metadata/Items/TowerAugment/RitualAugment", "", ReforgeItemType::TabletsOnly, "Ritual (path)", "祭祀碑牌（路徑）", 106},
        {"Metadata/Items/TowerAugment/MapBossAugment", "", ReforgeItemType::TabletsOnly, "Overseer (path)", "總督碑牌（路徑）", 107},
        {"Metadata/Items/TowerAugment/IncursionAugment", "", ReforgeItemType::TabletsOnly, "Temple (path)", "神廟碑牌（路徑）", 108},
        // 通用匹配 (宽匹配)
        {"toweraugment", "", ReforgeItemType::TabletsOnly, "Precursor Tablet (path)", "先行者碑牌（路徑匹配）", 0},
        {"toweraugments", "", ReforgeItemType::TabletsOnly, "Precursor Tablet (parent path)", "先行者碑牌（父路徑）", 0},
        {"", "precursor tablet", ReforgeItemType::TabletsOnly, "Precursor Tablet (BT)", "先行者碑牌（BT匹配）", 0},

        // === 珠宝 Jewel ===
        {"Metadata/Items/Jewels/JewelStr", "Ruby", ReforgeItemType::JewelsOnly, "Ruby Jewel", "紅玉", 201},
        {"Metadata/Items/Jewels/JewelDex", "Emerald", ReforgeItemType::JewelsOnly, "Emerald Jewel", "翠綠碧雲", 202},
        {"Metadata/Items/Jewels/JewelInt", "Sapphire", ReforgeItemType::JewelsOnly, "Sapphire Jewel", "藍玉", 203},
        {"Metadata/Items/Jewels/JewelDiamond", "Diamond", ReforgeItemType::JewelsOnly, "Diamond Jewel", "鑽石", 204},
        {"Metadata/Items/Jewels/JewelTimeless", "Timeless Jewel", ReforgeItemType::JewelsOnly, "Timeless Jewel", "永恆珠寶", 205},
        {"Metadata/Items/Jewels/JewelRadiusStr", "Time-Lost Ruby", ReforgeItemType::JewelsOnly, "Time-Lost Ruby", "時迭紅寶石", 206},
        {"Metadata/Items/Jewels/JewelRadiusDex", "Time-Lost Emerald", ReforgeItemType::JewelsOnly, "Time-Lost Emerald", "時迭綠寶石", 206},
        {"Metadata/Items/Jewels/JewelRadiusInt", "Time-Lost Sapphire", ReforgeItemType::JewelsOnly, "Time-Lost Sapphire", "時迭藍寶石", 206},
        {"Metadata/Items/Jewels/JewelRadiusDiamond", "Time-Lost Diamond", ReforgeItemType::JewelsOnly, "Time-Lost Diamond", "時迭鑽石", 206},
        {"Metadata/Items/Jewels/", "", ReforgeItemType::JewelsOnly, "Jewel (path)", "珠寶（路徑匹配）", 0},
        {"", "ruby", ReforgeItemType::JewelsOnly, "Ruby (BT)", "紅玉（BT）", 201},
        {"", "emerald", ReforgeItemType::JewelsOnly, "Emerald (BT)", "翠綠碧雲（BT）", 202},
        {"", "sapphire", ReforgeItemType::JewelsOnly, "Sapphire (BT)", "藍玉（BT）", 203},
        {"", "diamond", ReforgeItemType::JewelsOnly, "Diamond (BT)", "鑽石（BT）", 204},
        {"", "timeless jewel", ReforgeItemType::JewelsOnly, "Timeless Jewel (BT)", "永恆珠寶（BT）", 205},
        {"", "time-lost", ReforgeItemType::JewelsOnly, "Time-Lost Jewel (BT)", "時迭珠寶（BT）", 206},

        // === 符文 Rune ===
        {"Metadata/Items/SoulCores/RuneFire", "Desert Rune", ReforgeItemType::RunesOnly, "Desert Rune", "沙漠符文", 301},
        {"Metadata/Items/SoulCores/RuneCold", "Glacial Rune", ReforgeItemType::RunesOnly, "Glacial Rune", "冰川符文", 302},
        {"Metadata/Items/SoulCores/RuneLightning", "Storm Rune", ReforgeItemType::RunesOnly, "Storm Rune", "暴風符文", 303},
        {"Metadata/Items/SoulCores/RuneEnhance", "Iron Rune", ReforgeItemType::RunesOnly, "Iron Rune", "鍛鐵符文", 304},
        {"Metadata/Items/SoulCores/RuneLife", "Body Rune", ReforgeItemType::RunesOnly, "Body Rune", "肉體符文", 305},
        {"Metadata/Items/SoulCores/RuneMana", "Mind Rune", ReforgeItemType::RunesOnly, "Mind Rune", "心靈符文", 306},
        {"Metadata/Items/SoulCores/RuneLifeRecovery", "Rebirth Rune", ReforgeItemType::RunesOnly, "Rebirth Rune", "重生符文", 305},
        {"Metadata/Items/SoulCores/RuneManaRecovery", "Inspiration Rune", ReforgeItemType::RunesOnly, "Inspiration Rune", "啟發符文", 306},
        {"Metadata/Items/SoulCores/RuneStun", "Stone Rune", ReforgeItemType::RunesOnly, "Stone Rune", "岩石符文", 302},
        {"Metadata/Items/SoulCores/RuneAccuracy", "Vision Rune", ReforgeItemType::RunesOnly, "Vision Rune", "遠見符文", 301},
        {"Metadata/Items/SoulCores/SoulCoreChaos", "Chaos Rune", ReforgeItemType::RunesOnly, "Chaos Rune", "混沌符文", 303},
        {"Metadata/Items/SoulCores/SoulCoreBleed", "Blood Rune", ReforgeItemType::RunesOnly, "Blood Rune", "鮮血符文", 301},
        {"Metadata/Items/SoulCores/SoulCoreMaxLife", "Essence Rune", ReforgeItemType::RunesOnly, "Essence Rune", "精華符文", 305},
        {"Metadata/Items/SoulCores/SoulCoreMaxMana", "Soul Rune", ReforgeItemType::RunesOnly, "Soul Rune", "靈魂符文", 306},
        {"Metadata/Items/SoulCores/SoulCoreEnlighten", "Enlightenment Rune", ReforgeItemType::RunesOnly, "Enlightenment Rune", "啟蒙符文", 306},
        {"Metadata/Items/SoulCores/SoulCoreIgnite", "Ember Rune", ReforgeItemType::RunesOnly, "Ember Rune", "餘燼符文", 301},
        {"Metadata/Items/SoulCores/SoulCoreFreeze", "Frost Rune", ReforgeItemType::RunesOnly, "Frost Rune", "霜寒符文", 302},
        {"Metadata/Items/SoulCores/SoulCoreShock", "Shock Rune", ReforgeItemType::RunesOnly, "Shock Rune", "震擊符文", 303},
        {"Metadata/Items/SoulCores/SoulCoreLight", "Light Rune", ReforgeItemType::RunesOnly, "Light Rune", "光明符文", 306},
        {"Metadata/Items/SoulCores/SoulCoreElemental", "Elemental Rune", ReforgeItemType::RunesOnly, "Elemental Rune", "元素符文", 303},
        {"Metadata/Items/SoulCores/SoulCoreSpeed", "Swift Rune", ReforgeItemType::RunesOnly, "Swift Rune", "迅捷符文", 304},
        {"Metadata/Items/SoulCores/SoulCoreCrit", "Precision Rune", ReforgeItemType::RunesOnly, "Precision Rune", "精准符文", 304},
        {"Metadata/Items/SoulCores/SoulCoreStrength", "Strength Rune", ReforgeItemType::RunesOnly, "Strength Rune", "力量符文", 305},
        {"Metadata/Items/SoulCores/SoulCoreDexterity", "Agility Rune", ReforgeItemType::RunesOnly, "Agility Rune", "敏捷符文", 304},
        {"Metadata/Items/SoulCores/SoulCoreIntelligence", "Wit Rune", ReforgeItemType::RunesOnly, "Wit Rune", "智慧符文", 306},
        {"rune", "", ReforgeItemType::RunesOnly, "Rune (path)", "符文（路徑匹配）", 0},
        {"soulcores", "", ReforgeItemType::RunesOnly, "SoulCore (path)", "SoulCore（路徑匹配）", 0},
        {"", "rune", ReforgeItemType::RunesOnly, "Rune (BT)", "符文（BT匹配）", 0},

        // === 精髓 Essence ===
        {"Metadata/Items/Currency/CurrencyEssenceLife", "Essence of the Body", ReforgeItemType::EssencesOnly, "Essence of the Body", "肉體精髓", 401},
        {"Metadata/Items/Currency/CurrencyEssenceMana", "Essence of the Mind", ReforgeItemType::EssencesOnly, "Essence of the Mind", "心智精髓", 402},
        {"Metadata/Items/Currency/CurrencyEssenceDefences", "Essence of Enhancement", ReforgeItemType::EssencesOnly, "Essence of Enhancement", "強化精髓", 403},
        {"Metadata/Items/Currency/CurrencyEssencePhysical", "Essence of Torment", ReforgeItemType::EssencesOnly, "Essence of Torment", "磨礪精髓", 404},
        {"Metadata/Items/Currency/CurrencyEssenceFire", "Essence of Anger", ReforgeItemType::EssencesOnly, "Essence of Anger", "烈焰精髓", 405},
        {"Metadata/Items/Currency/CurrencyEssenceCold", "Essence of Doubt", ReforgeItemType::EssencesOnly, "Essence of Doubt", "寒冰精髓", 406},
        {"Metadata/Items/Currency/CurrencyEssenceLightning", "Essence of Fear", ReforgeItemType::EssencesOnly, "Essence of Fear", "電能精髓", 407},
        {"Metadata/Items/Currency/CurrencyEssenceChaos", "Essence of Deception", ReforgeItemType::EssencesOnly, "Essence of Deception", "毀滅精髓", 408},
        {"Metadata/Items/Currency/CurrencyEssenceAttack", "Essence of Hostility", ReforgeItemType::EssencesOnly, "Essence of Hostility", "戰鬥精髓", 409},
        {"Metadata/Items/Currency/CurrencyEssenceCaster", "Essence of Cognition", ReforgeItemType::EssencesOnly, "Essence of Cognition", "巫術精髓", 410},
        {"Metadata/Items/Currency/CurrencyEssenceSpeed", "Essence of Motion", ReforgeItemType::EssencesOnly, "Essence of Motion", "迅捷精髓", 411},
        {"Metadata/Items/Currency/CurrencyEssenceAttribute", "Essence of Fulfilment", ReforgeItemType::EssencesOnly, "Essence of Fulfilment", "無限精髓", 412},
        {"essence", "", ReforgeItemType::EssencesOnly, "Essence (path)", "精髓（路徑匹配）", 0},
        {"", "essence", ReforgeItemType::EssencesOnly, "Essence (BT)", "精髓（BT匹配）", 0},

        // === 液态情感 Liquid Emotion ===
        {"Metadata/Items/Currency/DistilledEmotion1", "Distilled Ire", ReforgeItemType::LiquidsOnly, "Distilled Ire", "液態憤怒", 501},
        {"Metadata/Items/Currency/DistilledEmotion2", "Distilled Guilt", ReforgeItemType::LiquidsOnly, "Distilled Guilt", "液態罪孽", 502},
        {"Metadata/Items/Currency/DistilledEmotion3", "Distilled Greed", ReforgeItemType::LiquidsOnly, "Distilled Greed", "液態貪婪", 503},
        {"Metadata/Items/Currency/DistilledEmotion4", "Distilled Paranoia", ReforgeItemType::LiquidsOnly, "Distilled Paranoia", "液態偏執", 504},
        {"Metadata/Items/Currency/DistilledEmotion5", "Distilled Envy", ReforgeItemType::LiquidsOnly, "Distilled Envy", "液態忌妒", 505},
        {"Metadata/Items/Currency/DistilledEmotion6", "Distilled Disgust", ReforgeItemType::LiquidsOnly, "Distilled Disgust", "液態厭惡", 506},
        {"Metadata/Items/Currency/DistilledEmotion7", "Distilled Despair", ReforgeItemType::LiquidsOnly, "Distilled Despair", "液態絕望", 507},
        {"Metadata/Items/Currency/DistilledEmotion8", "Distilled Fear", ReforgeItemType::LiquidsOnly, "Distilled Fear", "液態恐懼", 508},
        {"Metadata/Items/Currency/DistilledEmotion9", "Distilled Suffering", ReforgeItemType::LiquidsOnly, "Distilled Suffering", "液態苦難", 509},
        {"Metadata/Items/Currency/DistilledEmotion10", "Distilled Isolation", ReforgeItemType::LiquidsOnly, "Distilled Isolation", "液態孤立", 510},
        {"distilledemotion", "", ReforgeItemType::LiquidsOnly, "Distilled Emotion (path)", "液态情感（路徑匹配）", 0},
        {"", "distilled", ReforgeItemType::LiquidsOnly, "Distilled Emotion (BT)", "液态情感（BT匹配）", 0},

        // === 催化剂 Catalyst (催化劑) ===
        // Jewellery 用催化剂 (影响戒指/项链) - 13 种
        {"Metadata/Items/Currency/CurrencyJewelleryQualityLife", "Flesh Catalyst", ReforgeItemType::CatalystsOnly, "Flesh Catalyst", "血肉催化劑", 601},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityMana", "Neural Catalyst", ReforgeItemType::CatalystsOnly, "Neural Catalyst", "神經催化劑", 602},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityDefences", "Carapace Catalyst", ReforgeItemType::CatalystsOnly, "Carapace Catalyst", "甲殼催化劑", 603},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityPhysical", "Uul-Netol's Catalyst", ReforgeItemType::CatalystsOnly, "Uul-Netol's Catalyst", "烏爾尼多催化劑", 604},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityFire", "Xoph's Catalyst", ReforgeItemType::CatalystsOnly, "Xoph's Catalyst", "索伏催化劑", 605},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityCold", "Tuls' Catalyst", ReforgeItemType::CatalystsOnly, "Tuls' Catalyst", "托沃催化劑", 606},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityLightning", "Esh's Catalyst", ReforgeItemType::CatalystsOnly, "Esh's Catalyst", "艾許催化劑", 607},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityChaos", "Chayula's Catalyst", ReforgeItemType::CatalystsOnly, "Chayula's Catalyst", "夏烏拉催化劑", 608},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityAttack", "Reaver Catalyst", ReforgeItemType::CatalystsOnly, "Reaver Catalyst", "掠奪催化劑", 609},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityCaster", "Sibilant Catalyst", ReforgeItemType::CatalystsOnly, "Sibilant Catalyst", "嘶鳴催化劑", 610},
        {"Metadata/Items/Currency/CurrencyJewelleryQualitySpeed", "Skittering Catalyst", ReforgeItemType::CatalystsOnly, "Skittering Catalyst", "飛掠催化劑", 611},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityAttribute", "Adaptive Catalyst", ReforgeItemType::CatalystsOnly, "Adaptive Catalyst", "適性催化劑", 612},
        {"Metadata/Items/Currency/CurrencyJewelleryQualityNecrotic", "Necrotic Catalyst", ReforgeItemType::CatalystsOnly, "Necrotic Catalyst", "死靈催化劑", 613},
        // Jewel 用催化剂 (影响珠宝) - 13 种
        {"Metadata/Items/Currency/CurrencyJewelQualityLife", "Refined Flesh Catalyst", ReforgeItemType::CatalystsOnly, "Refined Flesh Catalyst", "精製血肉催化劑", 701},
        {"Metadata/Items/Currency/CurrencyJewelQualityMana", "Refined Neural Catalyst", ReforgeItemType::CatalystsOnly, "Refined Neural Catalyst", "精製神經催化劑", 702},
        {"Metadata/Items/Currency/CurrencyJewelQualityDefences", "Refined Carapace Catalyst", ReforgeItemType::CatalystsOnly, "Refined Carapace Catalyst", "精製甲殼催化劑", 703},
        {"Metadata/Items/Currency/CurrencyJewelQualityPhysical", "Refined Uul-Netol's Catalyst", ReforgeItemType::CatalystsOnly, "Refined Uul-Netol's Catalyst", "精製烏爾尼多催化劑", 704},
        {"Metadata/Items/Currency/CurrencyJewelQualityFire", "Refined Xoph's Catalyst", ReforgeItemType::CatalystsOnly, "Refined Xoph's Catalyst", "精製索伏催化劑", 705},
        {"Metadata/Items/Currency/CurrencyJewelQualityCold", "Refined Tuls' Catalyst", ReforgeItemType::CatalystsOnly, "Refined Tuls' Catalyst", "精製托沃催化劑", 706},
        {"Metadata/Items/Currency/CurrencyJewelQualityLightning", "Refined Esh's Catalyst", ReforgeItemType::CatalystsOnly, "Refined Esh's Catalyst", "精製艾許催化劑", 707},
        {"Metadata/Items/Currency/CurrencyJewelQualityChaos", "Refined Chayula's Catalyst", ReforgeItemType::CatalystsOnly, "Refined Chayula's Catalyst", "精製夏烏拉催化劑", 708},
        {"Metadata/Items/Currency/CurrencyJewelQualityAttack", "Refined Reaver Catalyst", ReforgeItemType::CatalystsOnly, "Refined Reaver Catalyst", "精製掠奪催化劑", 709},
        {"Metadata/Items/Currency/CurrencyJewelQualityCaster", "Refined Sibilant Catalyst", ReforgeItemType::CatalystsOnly, "Refined Sibilant Catalyst", "精製嘶鳴催化劑", 710},
        {"Metadata/Items/Currency/CurrencyJewelQualitySpeed", "Refined Skittering Catalyst", ReforgeItemType::CatalystsOnly, "Refined Skittering Catalyst", "精製飛掠催化劑", 711},
        {"Metadata/Items/Currency/CurrencyJewelQualityAttribute", "Refined Adaptive Catalyst", ReforgeItemType::CatalystsOnly, "Refined Adaptive Catalyst", "精製適性催化劑", 712},
        {"Metadata/Items/Currency/CurrencyJewelQualityNecrotic", "Refined Necrotic Catalyst", ReforgeItemType::CatalystsOnly, "Refined Necrotic Catalyst", "精製死靈催化劑", 713},
        // 催化剂通用匹配
        {"currencyjewelleryquality", "", ReforgeItemType::CatalystsOnly, "Catalyst (path)", "催化劑（路徑匹配）", 0},
        {"currencyjewelquality", "", ReforgeItemType::CatalystsOnly, "Refined Catalyst (path)", "精製催化劑（路徑匹配）", 0},
        {"", "catalyst", ReforgeItemType::CatalystsOnly, "Catalyst (BT)", "催化劑（BT匹配）", 0},
    };
    count = sizeof(patterns) / sizeof(patterns[0]);
    return patterns;
}

// 安全转义辅助函数：将用户数据转义为安全字符串（防止格式化漏洞）
inline std::string EscapeFormatString(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() + 16);
    for (char c : input) {
        if (c == '%') {
            escaped += "%%";  // 转义格式化字符
        } else if (c < 32 || c > 126) {
            escaped += '.';   // 替换不可打印字符
        } else {
            escaped += c;
        }
    }
    return escaped;
}

// 前向声明：IsCraftableItem（定义在文件后面）
inline bool IsCraftableItem(const std::string& path, const std::string& baseType);

// 前向声明：白色品质非碑牌非珠宝判定（定义在文件后面）
inline bool IsNormalRarityCraftable(int rarity, const std::string& path, const std::string& baseType);

// 前向声明：子类未选中判定（定义在文件后面）
struct TabletReforgeConfigSettings;
inline bool IsItemUnwantedBySubCategory(const std::string& path, const std::string& baseType,
                                        const TabletReforgeConfig::Settings& cfg);
inline bool IsItemWantedBySubCategory(const std::string& path, const std::string& baseType,
                                      const TabletReforgeConfig::Settings& cfg);

// 详细调试版本的匹配函数（输出每一步匹配逻辑）
// 新版本：基于 IsCraftableItem 判定（不使用大类/子类过滤），直接匹配物品是否可合成
inline bool MatchesPoe2DataPatternsDebug(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg,
    std::string& debugLog)
{
    try {
        const bool btValid = IsValidBaseType(baseType);

        int count = 0;
        const auto* patterns = GetPoe2Patterns(count);

        // 安全转义用户数据，防止格式化字符串漏洞
        std::string safePath = EscapeFormatString(path);
        std::string safeBt = EscapeFormatString(baseType);

        char buf[1024];
        ::sprintf_s(buf, "\n[匹配调试] path='%s', bt='%s', bt有效=%d, rarity=%d, identified=%d",
            safePath.c_str(), safeBt.c_str(), btValid ? 1 : 0, rarity, identified);
        debugLog += buf;

        if (!btValid) {
            debugLog += "\n  [注意] BaseType无效，将跳过需要BT匹配的模式，仅使用Path匹配";
        }

        if (path.empty() && baseType.empty()) {
            debugLog += "\n  → 跳过：Path和BaseType均为空";
            return false;
        }

        // Step 1: 检查是否是可合成物品（不再使用大类/子类过滤）
        bool isCraftable = IsCraftableItem(path, baseType);
        ::sprintf_s(buf, "\n  IsCraftableItem=%d (检查路径/类型是否属于可合成物品)", isCraftable ? 1 : 0);
        debugLog += buf;

        if (!isCraftable) {
            debugLog += "\n  → 不是可合成物品，跳过";
            return false;
        }

        // 白色品质非碑牌非珠宝物品：不需要鉴定，未选中子类 = 原料
        if (IsNormalRarityCraftable(rarity, path, baseType)) {
            if (cfg.selectedSubCategories.empty()) {
                debugLog += "\n  → 白色品质非碑牌珠宝: 无选中子类 → 无原料";
                return false;
            }
            bool unwanted = IsItemUnwantedBySubCategory(path, baseType, cfg);
            ::sprintf_s(buf, "\n  → 白色品质非碑牌珠宝: %s → %s",
                unwanted ? "未选中(原料)" : "已选中(产物)",
                unwanted ? "是原料" : "不是原料");
            debugLog += buf;
            return unwanted;
        }

        // Step 2: 检查已鉴定要求
        if (cfg.requireIdentifiedForMaterial && !identified) {
            debugLog += "\n  → 未鉴定，不是原料（需要先NPC鉴定）";
            return false;
        }

        // Step 3: 检查稀有度要求
        if (cfg.filterByRarity && rarity < cfg.minRarityForMaterial) {
            debugLog += "\n  -> rarity filter=ON rarity=" + std::to_string(rarity)
                + " < min=" + std::to_string(cfg.minRarityForMaterial)
                + ", skip";
            return false;
        } else if (!cfg.filterByRarity) {
            debugLog += "\n  -> rarity filter=OFF skip (rarity=" + std::to_string(rarity) + ")";
        }

        // Step 4: 词缀筛选（如果启用）
        if (cfg.useModifierFilterMode && !cfg.selectedModifierKeys.empty()) {
            // 此版本没有词缀信息，无法判断是否有匹配词缀
            // 为了安全起见，当词缀筛选启用但没有词缀信息时，默认通过（物品会在后续被验证）
            debugLog += "\n  → 词缀筛选已启用但无词缀信息，默认通过（物品将在拾取后被验证）";
        }

        debugLog += "\n  → 通过所有检查，判定为原料候选";
        return true;
    } catch (const std::exception& e) {
        char errLog[512];
        ::sprintf_s(errLog, "\n[匹配异常] %s", e.what());
        debugLog += errLog;
        return false;
    } catch (...) {
        debugLog += "\n[匹配异常] 未知异常";
        return false;
    }
}

// 使用 POE2 数据表进行精确匹配（仅用于调试/报告，不用于核心过滤）
// 核心过滤使用 IsCraftableItem + 词缀匹配
inline bool MatchesPoe2DataPatterns(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    // 直接使用 IsCraftableItem 判定（不再使用大类/子类过滤）
    if (!IsCraftableItem(path, baseType)) return false;
    if (cfg.requireIdentifiedForMaterial && !identified) return false;
    if (cfg.filterByRarity && rarity < cfg.minRarityForMaterial) return false;
    return true;
}

// 生成 POE2 数据匹配报告
inline std::string Poe2MatchReport(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    std::string result;
    char buf[512];

    ::sprintf_s(buf, "\n  POE2 Match Report (基于 IsCraftableItem 判定):");
    result += buf;

    if (path.empty() && baseType.empty()) {
        result += "\n  [Path和BaseType均为空] 游戏未提供物品路径/类型数据";
        return result;
    }

    bool isCraftable = IsCraftableItem(path, baseType);
    ::sprintf_s(buf, "\n  IsCraftableItem=%d, rarity=%d, identified=%d",
        isCraftable ? 1 : 0, rarity, identified);
    result += buf;

    if (!isCraftable) {
        result += "\n  → 不是可合成物品";
    } else {
        result += "\n  → 是可合成物品";
        
        if (cfg.requireIdentifiedForMaterial && !identified) {
            result += "\n  → 未鉴定（需要先NPC鉴定才能成为原料）";
        }
        if (rarity < cfg.minRarityForMaterial) {
            result += "\n  -> rarity insufficient (" + std::to_string(rarity)
                + " < " + std::to_string(cfg.minRarityForMaterial) + ")";
        }
    }

    return result;
}

// ============================================================
// POE2 物品识别：关键词匹配层
// ============================================================

// ✅ 地图钥匙 Waystone (引路石)
// POE2 Waystone 数据：Path Metadata/Items/Maps/MapKeyTier1..16
// BaseType: "Waystone (Tier X)"
inline bool IsWaystone(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "mapkey"))     return true;
    if (ContainsCI(path, "waystone"))   return true;
    if (ContainsCI(path, "map_key"))   return true;
    if (ContainsCI(baseType, "waystone")) return true;
    if (ContainsCI(baseType, "map key")) return true;
    return false;
}

// ✅ 先行者碑牌 Precursor Tablet (輻照碑牌 / Irradiated Tablet)
// POE2 TowerAugment 数据：Metadata/Items/TowerAugment/*
// 父级路径: Metadata/Items/TowerAugments/TowerAugment（带 s）
inline bool IsPrecursorTablet(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "toweraugment")) return true;
    if (ContainsCI(path, "toweraugments")) return true;
    if (ContainsCI(path, "precursor")) return true;
    if (ContainsCI(baseType, "precursor tablet")) return true;
    if (ContainsCI(baseType, "precursor")) return true;
    return false;
}

// ✅ 先行者碑牌（兼容旧名 + 实际 POE2 数据）
inline bool IsTempleTablet(const std::string& path, const std::string& baseType) {
    if (IsPrecursorTablet(path, baseType)) return true;
    if (ContainsCI(path, "incursion")) return true;
    if (ContainsCI(path, "temple"))    return true;
    if (ContainsCI(path, "vaal"))      return true;
    if (ContainsCI(path, "sentinel"))  return true;
    if (ContainsCI(baseType, "incursion")) return true;
    if (ContainsCI(baseType, "temple"))    return true;
    if (ContainsCI(baseType, "vaal"))      return true;
    if (ContainsCI(baseType, "sentinel"))  return true;
    return false;
}

// ✅ 所有碑牌
inline bool IsAnyTabletLike(const std::string& path, const std::string& baseType) {
    if (IsWaystone(path, baseType))     return true;
    if (IsPrecursorTablet(path, baseType)) return true;
    if (ContainsCI(baseType, "tablet")) return true;
    if (ContainsCI(baseType, "precursor")) return true;
    if (ContainsCI(path, "tablet")) return true;
    if (ContainsCI(path, "toweraugment")) return true;
    if (ContainsCI(path, "toweraugments")) return true;
    return false;
}

// ✅ 珠宝 Jewel（包含 Time-Lost 变体）
// POE2 Jewel 数据：Path Metadata/Items/Jewels/Jewel*
// 普通珠宝 BaseType: "Ruby", "Emerald", "Sapphire", "Diamond", "Timeless Jewel"
// Time-Lost 变体: "Time-Lost Ruby", 等
inline bool IsJewel(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "currencyjewelquality")) return false;
    if (ContainsCI(path, "currencyjewelleryquality")) return false;
    if (ContainsCI(baseType, "catalyst")) return false;
    if (ContainsCI(path, "jewels")) return true;
    if (ContainsCI(path, "jewel"))  return true;
    if (ContainsCI(baseType, "jewel")) return true;
    if (ContainsCI(baseType, "ruby")) return true;
    if (ContainsCI(baseType, "emerald")) return true;
    if (ContainsCI(baseType, "sapphire")) return true;
    if (ContainsCI(baseType, "diamond")) return true;
    if (ContainsCI(baseType, "timeless jewel")) return true;
    if (ContainsCI(baseType, "time-lost")) return true;
    return false;
}

// ✅ 符文 Rune / SoulCore
// POE2 数据: Path Metadata/Items/SoulCores/Rune* 或 SoulCore*
inline bool IsRune(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "rune")) return true;
    if (ContainsCI(path, "soulcores")) return true;
    if (ContainsCI(baseType, "rune")) return true;
    if (ContainsCI(baseType, "soul core")) return true;
    return false;
}

// ✅ 精髓 Essence
// POE2 数据: Path Metadata/Items/Currency/CurrencyEssence*
inline bool IsEssence(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "essence")) return true;
    if (ContainsCI(path, "currencyessence")) return true;
    if (ContainsCI(baseType, "essence")) return true;
    return false;
}

// ✅ 情感蒸馏液 Distilled Emotion
// POE2 数据: Path Metadata/Items/Currency/DistilledEmotion1..10
inline bool IsLiquidEmotion(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "distilledemotion")) return true;
    if (ContainsCI(baseType, "distilled")) return true;
    return false;
}

// ✅ 催化剂 Catalyst (催化劑)
// POE2 数据: Path Metadata/Items/Currency/CurrencyJewelleryQuality* (Jewellery用) 或 CurrencyJewelQuality* (Jewel用)
inline bool IsCatalyst(const std::string& path, const std::string& baseType) {
    if (ContainsCI(path, "currencyjewelleryquality")) return true;
    if (ContainsCI(path, "currencyjewelquality")) return true;
    if (ContainsCI(baseType, "catalyst")) return true;
    return false;
}

// ✅ 传奇 Mastered Domain (主宰領地)
// POE2: 六种类型 Water/Mountain/Grass/Desert/Lava/Thicket
// 通过 Path 含 MasteredDomain 且 Rarity=3 (unique) 识别
inline bool IsMasteredDomain(const std::string& path, const std::string& icon, int rarity) {
    if (ContainsCI(path, "mastereddomain")) return true;
    if (ContainsCI(icon, "precursortabletmastereddomain")) return true;
    if (rarity == kRarityUnique && !path.empty()) return true;
    return false;
}

// ✅ 自定义关键词
inline bool MatchesCustomKeywords(const std::string& path, const std::string& baseType,
                                   const std::string& customCsv) {
    if (customCsv.empty()) return false;
    auto kws = SplitKeywords(customCsv);
    for (const auto& kw : kws) {
        if (!kw.empty() && (ContainsCI(path, kw) || ContainsCI(baseType, kw)))
            return true;
    }
    return false;
}

// ============================================================
// 物品分类器核心逻辑
// ============================================================

inline bool IsLikelyCraftableMaterial(int rarity) {
    if (rarity == kRarityNormal) return false;
    return true;
}

inline std::string DescribeRarity(int rarity) {
    switch (rarity) {
        case 0: return "未设置/未知";
        case 1: return "普通/白色 (Normal)";
        case 2: return "魔法/蓝色 (Magic)";
        case 3: return "稀有/黄色 (Rare)";
        case 4: return "传奇/橙色 (Unique)";
        default: return "未知稀有度(" + std::to_string(rarity) + ")";
    }
}

inline void LogFilteredItem(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::string& reason,
    std::vector<std::string>* outLog)
{
    if (!outLog) return;
    std::string shortPath = path;
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) shortPath = path.substr(slash + 1);
    std::string bt = baseType.empty() ? "(empty)" : baseType;
    char buf[512];
    ::sprintf_s(buf,
        "[过滤] Path='%s' BT='%s' Rarity=%d(%s) Id=%d 原因: %s",
        shortPath.c_str(), bt.c_str(), rarity, DescribeRarity(rarity).c_str(),
        identified ? 1 : 0, reason.c_str());
    outLog->push_back(buf);
}

// ============================================================
// 新版：基于子类别的筛选（去掉大类模式）
// ============================================================

enum class TabletKind {
    None,
    Waystone,
    PrecursorTablet,
    Jewel,
    Rune,
    Essence,
    Liquid,
    Catalyst,
};

struct SubCategoryMatch {
    TabletKind kind = TabletKind::None;
    int subCategoryId = 0;
    std::string typeKey;
};

inline SubCategoryMatch ClassifyItemSubCategory(
    const std::string& path,
    const std::string& baseType)
{
    SubCategoryMatch m;
    
    if (IsWaystone(path, baseType)) {
        m.kind = TabletKind::Waystone;
        m.subCategoryId = 0;
        m.typeKey = "Waystone";
        return m;
    }
    
    // 碑牌识别：扩展匹配所有碑牌类型
    if (IsPrecursorTablet(path, baseType) || IsAnyTabletLike(path, baseType) ||
        ContainsCI(baseType, "tablet")) {
        m.kind = TabletKind::PrecursorTablet;
        m.subCategoryId = detail::GetSubCategoryIdForType(
            detail::ClassifyTabletType(path, baseType));
        m.typeKey = detail::ClassifyTabletType(path, baseType);
        if (m.subCategoryId == 0) {
            // Fallback: use path/bt matching when ClassifyTabletType fails
            if (ContainsCI(path, "generic") || ContainsCI(baseType, "precursor") || ContainsCI(baseType, "irradiated")) m.subCategoryId = 101;
            else if (ContainsCI(path, "breach") || ContainsCI(baseType, "breach")) m.subCategoryId = 102;
            else if (ContainsCI(path, "expedition") || ContainsCI(baseType, "expedition")) m.subCategoryId = 103;
            else if (ContainsCI(path, "delirium") || ContainsCI(baseType, "delirium")) m.subCategoryId = 104;
            else if (ContainsCI(path, "abyss") || ContainsCI(baseType, "abyss")) m.subCategoryId = 105;
            else if (ContainsCI(path, "ritual") || ContainsCI(baseType, "ritual")) m.subCategoryId = 106;
            else if (ContainsCI(path, "mapboss") || ContainsCI(baseType, "overseer")) m.subCategoryId = 107;
            else if (ContainsCI(path, "incursion") || ContainsCI(baseType, "temple")) m.subCategoryId = 108;
            else m.subCategoryId = 101; // default to Irradiated
        }
        return m;
    }
    
    if (IsJewel(path, baseType)) {
        m.kind = TabletKind::Jewel;
        if (ContainsCI(path, "jewelstr") || ContainsCI(baseType, "ruby")) m.subCategoryId = 201;
        else if (ContainsCI(path, "jeweldex") || ContainsCI(baseType, "emerald")) m.subCategoryId = 202;
        else if (ContainsCI(path, "jewelint") || ContainsCI(baseType, "sapphire")) m.subCategoryId = 203;
        else if (ContainsCI(path, "jeweldiamond") || ContainsCI(baseType, "diamond")) m.subCategoryId = 204;
        else if (ContainsCI(path, "jeweltimeless") || ContainsCI(baseType, "timeless")) m.subCategoryId = 205;
        else if (ContainsCI(path, "jewelryadius")) m.subCategoryId = 206;
        else m.subCategoryId = 201;
        return m;
    }
    
    if (IsRune(path, baseType)) {
        m.kind = TabletKind::Rune;
        if (ContainsCI(path, "runefire") || ContainsCI(baseType, "desert")) m.subCategoryId = 301;
        else if (ContainsCI(path, "runecold") || ContainsCI(baseType, "glacial")) m.subCategoryId = 302;
        else if (ContainsCI(path, "runelightning") || ContainsCI(baseType, "storm")) m.subCategoryId = 303;
        else if (ContainsCI(path, "runeenhance") || ContainsCI(baseType, "iron")) m.subCategoryId = 304;
        else if (ContainsCI(path, "runelife") || ContainsCI(baseType, "body")) m.subCategoryId = 305;
        else if (ContainsCI(path, "runemana") || ContainsCI(baseType, "mind")) m.subCategoryId = 306;
        else m.subCategoryId = 301;
        return m;
    }
    
    if (IsEssence(path, baseType)) {
        m.kind = TabletKind::Essence;
        if (ContainsCI(path, "essencelife") || ContainsCI(baseType, "body")) m.subCategoryId = 401;
        else if (ContainsCI(path, "essencemana") || ContainsCI(baseType, "mind")) m.subCategoryId = 402;
        else if (ContainsCI(path, "essencedefences") || ContainsCI(baseType, "enhancement")) m.subCategoryId = 403;
        else if (ContainsCI(path, "essencephysical") || ContainsCI(baseType, "torment")) m.subCategoryId = 404;
        else if (ContainsCI(path, "essencefire") || ContainsCI(baseType, "anger")) m.subCategoryId = 405;
        else if (ContainsCI(path, "essencecold") || ContainsCI(baseType, "doubt")) m.subCategoryId = 406;
        else if (ContainsCI(path, "essencelightning") || ContainsCI(baseType, "fear")) m.subCategoryId = 407;
        else if (ContainsCI(path, "essencechaos") || ContainsCI(baseType, "deception")) m.subCategoryId = 408;
        else if (ContainsCI(path, "essenceattack") || ContainsCI(baseType, "hostility")) m.subCategoryId = 409;
        else if (ContainsCI(path, "essencecaster") || ContainsCI(baseType, "cognition")) m.subCategoryId = 410;
        else if (ContainsCI(path, "essencespeed") || ContainsCI(baseType, "motion")) m.subCategoryId = 411;
        else if (ContainsCI(path, "essenceattribute") || ContainsCI(baseType, "fulfilment")) m.subCategoryId = 412;
        else m.subCategoryId = 401;
        return m;
    }
    
    if (IsLiquidEmotion(path, baseType)) {
        m.kind = TabletKind::Liquid;
        if (ContainsCI(path, "emotion1") || ContainsCI(baseType, "ire")) m.subCategoryId = 501;
        else if (ContainsCI(path, "emotion2") || ContainsCI(baseType, "guilt")) m.subCategoryId = 502;
        else if (ContainsCI(path, "emotion3") || ContainsCI(baseType, "greed")) m.subCategoryId = 503;
        else if (ContainsCI(path, "emotion4") || ContainsCI(baseType, "paranoia")) m.subCategoryId = 504;
        else if (ContainsCI(path, "emotion5") || ContainsCI(baseType, "envy")) m.subCategoryId = 505;
        else if (ContainsCI(path, "emotion6") || ContainsCI(baseType, "disgust")) m.subCategoryId = 506;
        else if (ContainsCI(path, "emotion7") || ContainsCI(baseType, "despair")) m.subCategoryId = 507;
        else if (ContainsCI(path, "emotion8") || ContainsCI(baseType, "fear")) m.subCategoryId = 508;
        else if (ContainsCI(path, "emotion9") || ContainsCI(baseType, "suffering")) m.subCategoryId = 509;
        else if (ContainsCI(path, "emotion10") || ContainsCI(baseType, "isolation")) m.subCategoryId = 510;
        else m.subCategoryId = 501;
        return m;
    }
    
    if (IsCatalyst(path, baseType)) {
        m.kind = TabletKind::Catalyst;
        const bool isRefined = ContainsCI(path, "currencyjewelquality") && !ContainsCI(path, "currencyjewelleryquality");
        if (isRefined) {
            // 精製催化劑 (Refined Catalyst - Jewel 用, 影響珠寶)
            if (ContainsCI(path, "qualitylife")) m.subCategoryId = 701;
            else if (ContainsCI(path, "qualitymana")) m.subCategoryId = 702;
            else if (ContainsCI(path, "qualitydefences")) m.subCategoryId = 703;
            else if (ContainsCI(path, "qualityphysical")) m.subCategoryId = 704;
            else if (ContainsCI(path, "qualityfire")) m.subCategoryId = 705;
            else if (ContainsCI(path, "qualitycold")) m.subCategoryId = 706;
            else if (ContainsCI(path, "qualitylightning")) m.subCategoryId = 707;
            else if (ContainsCI(path, "qualitychaos")) m.subCategoryId = 708;
            else if (ContainsCI(path, "qualityattack")) m.subCategoryId = 709;
            else if (ContainsCI(path, "qualitycaster")) m.subCategoryId = 710;
            else if (ContainsCI(path, "qualityspeed")) m.subCategoryId = 711;
            else if (ContainsCI(path, "qualityattribute")) m.subCategoryId = 712;
            else if (ContainsCI(path, "qualitynecrotic")) m.subCategoryId = 713;
            else m.subCategoryId = 701;
        } else {
            // 普通催化劑 (Catalyst - Jewellery 用, 影響戒指/項鍊)
            if (ContainsCI(path, "qualitylife")) m.subCategoryId = 601;
            else if (ContainsCI(path, "qualitymana")) m.subCategoryId = 602;
            else if (ContainsCI(path, "qualitydefences")) m.subCategoryId = 603;
            else if (ContainsCI(path, "qualityphysical")) m.subCategoryId = 604;
            else if (ContainsCI(path, "qualityfire")) m.subCategoryId = 605;
            else if (ContainsCI(path, "qualitycold")) m.subCategoryId = 606;
            else if (ContainsCI(path, "qualitylightning")) m.subCategoryId = 607;
            else if (ContainsCI(path, "qualitychaos")) m.subCategoryId = 608;
            else if (ContainsCI(path, "qualityattack")) m.subCategoryId = 609;
            else if (ContainsCI(path, "qualitycaster")) m.subCategoryId = 610;
            else if (ContainsCI(path, "qualityspeed")) m.subCategoryId = 611;
            else if (ContainsCI(path, "qualityattribute")) m.subCategoryId = 612;
            else if (ContainsCI(path, "qualitynecrotic")) m.subCategoryId = 613;
            else m.subCategoryId = 601;
        }
        return m;
    }
    
    return m;
}

inline bool IsSubCategoryInSelection(int subCategoryId,
                                      const TabletReforgeConfig::Settings& cfg) {
    if (cfg.selectedSubCategories.empty()) return true;
    return cfg.selectedSubCategories.count(subCategoryId) > 0;
}

inline bool IsItemWantedBySubCategory(
    const std::string& path,
    const std::string& baseType,
    const TabletReforgeConfig::Settings& cfg)
{
    auto match = ClassifyItemSubCategory(path, baseType);
    if (match.kind == TabletKind::None) return false;
    if (cfg.selectedSubCategories.empty()) return true;
    return cfg.selectedSubCategories.count(match.subCategoryId) > 0;
}

inline bool IsItemUnwantedBySubCategory(
    const std::string& path,
    const std::string& baseType,
    const TabletReforgeConfig::Settings& cfg)
{
    auto match = ClassifyItemSubCategory(path, baseType);
    if (match.kind == TabletKind::None) return false;
    if (cfg.selectedSubCategories.empty()) return false;
    return cfg.selectedSubCategories.count(match.subCategoryId) == 0;
}

inline bool IsItemSynthesizeable(
    const std::string& path,
    const std::string& baseType)
{
    auto match = ClassifyItemSubCategory(path, baseType);
    return match.kind != TabletKind::None;
}

inline int GetEffectiveItemType(const TabletReforgeConfig::Settings& cfg) {
    if (cfg.useSubCategoryMode && !cfg.selectedSubCategories.empty()) {
        return static_cast<int>(ReforgeItemType::TabletsOnly);
    }
    return cfg.itemType;
}

// ============================================================
// 核心识别逻辑：基于词缀（Modifier）匹配
// ============================================================
//
// 【原料判定规则】
//   1. 物品必须是可合成类型（碑牌/珠宝/符文等）
//   2. 如果开启词缀筛选模式：
//      - 原料 = 物品拥有的所有词缀中，没有任何一个在用户勾选列表里
//      - 即：凡是拥有筛选配置中任意一个词缀的物品，都不是原料（是产物）
//   3. 如果要求已鉴定：未鉴定物品 → 不是原料（需要先NPC鉴定）
//   4. 如果有稀有度要求：低于最低稀有度 → 不是原料
//
// 【产物判定规则】
//   1. 物品必须是可合成类型
//   2. 产物 = 物品拥有的词缀中，至少有一个在用户勾选列表里
//

// 辅助函数：检查物品的词缀是否与用户勾选的词缀有交集
inline bool HasMatchingModifier(const std::vector<std::string>& itemMods,
                                const std::set<std::string>& selectedKeys) {
    if (selectedKeys.empty()) return false;
    
    for (const auto& mod : itemMods) {
        // 检查物品词缀是否包含任何一个用户勾选的关键词
        for (const auto& key : selectedKeys) {
            if (!key.empty() && !mod.empty()) {
                // 不区分大小写的子串匹配
                if (ContainsCI(mod, key) || ContainsCI(key, mod)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// 调试版本：返回详细匹配信息
struct ModMatchDetail {
    bool matched = false;
    std::string matchedMod;      // 匹配到的物品词缀
    std::string matchedKey;      // 匹配到的用户关键词
};

inline ModMatchDetail HasMatchingModifierDebug(
    const std::vector<std::string>& itemMods,
    const std::set<std::string>& selectedKeys)
{
    ModMatchDetail detail;
    if (selectedKeys.empty()) return detail;
    
    for (const auto& mod : itemMods) {
        for (const auto& key : selectedKeys) {
            if (!key.empty() && !mod.empty()) {
                if (ContainsCI(mod, key) || ContainsCI(key, mod)) {
                    detail.matched = true;
                    detail.matchedMod = mod;
                    detail.matchedKey = key;
                    return detail;
                }
            }
        }
    }
    return detail;
}

// 检查物品是否是可合成类型（碑牌/珠宝/符文/精髓/催化/情绪/地图）
inline bool IsCraftableItem(const std::string& path, const std::string& baseType) {
    if (IsAnyTabletLike(path, baseType)) return true;
    if (IsJewel(path, baseType)) return true;
    if (IsRune(path, baseType)) return true;
    if (IsEssence(path, baseType)) return true;
    if (IsLiquidEmotion(path, baseType)) return true;
    if (IsCatalyst(path, baseType)) return true;
    if (IsWaystone(path, baseType)) return true;
    return false;
}

// ============================================================
// 白色品质（普通品质）非碑牌非珠宝物品判定
// 这类物品（催化剂/精髓/液态情感/符文/路引石等）不需要鉴定
// 合成逻辑：选中子类 = 产物（保留），未选中子类 = 原料（合成用）
// 碑牌和珠宝需要鉴定（有词缀），不适用此逻辑
// ============================================================
inline bool IsNormalRarityCraftable(int rarity, const std::string& path, const std::string& baseType) {
    if (rarity != 0) return false;  // 只关心白色品质（普通品质 Rarity=0）
    if (IsAnyTabletLike(path, baseType)) return false;  // 排除碑牌（碑牌需要鉴定）
    if (IsJewel(path, baseType)) return false;  // 排除珠宝（珠宝需要鉴定）
    return IsCraftableItem(path, baseType);  // 必须是可合成物品
}

// ============================================================
// 可堆叠可合成物品判定
// IsNormalRarityCraftable 的子集：白色品质非碑牌非珠宝的可合成物品
// 这些物品（催化剂/精髓/液态情感/符文/路引石）在仓库中10个/组叠加
// 重铸台合成时每次从3个槽各消耗1个，可多次合成
// ============================================================
inline bool IsStackableCraftable(int rarity, const std::string& path, const std::string& baseType) {
    return IsNormalRarityCraftable(rarity, path, baseType);
}

// === 原料判定（基础版：无词缀信息时用此版本）===
inline bool MatchesDesiredReforgeType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    // Step 1: 检查是否是可合成物品
    if (!IsCraftableItem(path, baseType)) return false;

    // 白色品质非碑牌非珠宝物品（催化剂/精髓/液态情感/符文等）：
    // 不需要鉴定，未选中子类 = 原料（用来合成选中子类的产物）
    if (IsNormalRarityCraftable(rarity, path, baseType)) {
        if (cfg.selectedSubCategories.empty()) return false;  // 无选中 → 无原料
        return IsItemUnwantedBySubCategory(path, baseType, cfg);
    }

    // Step 2: 子类检查（必须属于选中的子类）
    if (!IsItemWantedBySubCategory(path, baseType, cfg)) {
        return false;
    }

    // Step 3: 检查已鉴定要求
    if (cfg.requireIdentifiedForMaterial && !identified) {
        return false;
    }

    // Step 4: 检查稀有度要求（仅当 filterByRarity=ON 时）
    if (cfg.filterByRarity && rarity < cfg.minRarityForMaterial) {
        return false;
    }

    // Step 5: 如果没有启用词缀筛选，所有通过上面检查的都是原料
    if (!cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        return true;
    }

    // Step 6: 词缀筛选模式（但此版本无词缀信息，默认通过）
    return true;
}

// ============================================================
// 【方案 B v1.3】前置声明：7 参数版（带 modIds/modHashes）和 5 参数版
// 定义在下方，Ex 包装函数需先声明才能调用
// ============================================================
inline bool MatchesDesiredReforgeType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg);

inline bool MatchesDesiredProductType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg);

inline bool MatchesDesiredProductType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg);

// ============================================================
// 【方案 B v1.3】统一包装函数：根据开关自动选择 4 参数或 8 参数版
// 所有调用点改用 Ex 版，消除散落的分支判断
// ============================================================
inline bool MatchesDesiredReforgeTypeEx(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg)
{
    // 熔断开关关闭 / 静默测试 / 未启用词缀筛选 / 无选中关键词 → 走 4 参数版（方案 A）
    if (!cfg.enableBonusMatch || cfg.bonusMatchSilent ||
        !cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        return MatchesDesiredReforgeType(path, baseType, rarity, identified, cfg);
    }
    // 方案 B：走 8 参数版（含 modIds 匹配）
    return MatchesDesiredReforgeType(path, baseType, rarity, identified,
                                     modIds, modHashes, cfg);
}

// 4 参数重载（用于无 modIds 信息的调用点）
inline bool MatchesDesiredReforgeTypeEx(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    return MatchesDesiredReforgeType(path, baseType, rarity, identified, cfg);
}

inline bool MatchesDesiredProductTypeEx(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg)
{
    if (!cfg.enableBonusMatch || cfg.bonusMatchSilent ||
        !cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        return MatchesDesiredProductType(path, baseType, rarity, identified, cfg);
    }
    return MatchesDesiredProductType(path, baseType, rarity, identified,
                                     modIds, modHashes, cfg);
}

// 4 参数重载
inline bool MatchesDesiredProductTypeEx(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    return MatchesDesiredProductType(path, baseType, rarity, identified, cfg);
}

// === 原料判定（词缀版：带词缀信息）===
// 【方案 B v1.3】参数从 (modNames, modAffixes, modStatKeys) 改为 (modIds, modHashes)
// modIds 已在 ExtractModIds 阶段经白名单过滤，只含合规的 Mod.Id（snake_case）
inline bool MatchesDesiredReforgeType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg)
{
    (void)modHashes;  // 当前不参与决策，保留用于未来扩展和调试日志
    // Step 1: 检查是否是可合成物品
    if (!IsCraftableItem(path, baseType)) return false;

    // 白色品质非碑牌非珠宝物品：不需要鉴定，未选中子类 = 原料
    if (IsNormalRarityCraftable(rarity, path, baseType)) {
        if (cfg.selectedSubCategories.empty()) return false;
        return IsItemUnwantedBySubCategory(path, baseType, cfg);
    }

    // Step 2: 子类检查（必须属于选中的子类）
    if (!IsItemWantedBySubCategory(path, baseType, cfg)) {
        return false;
    }

    // Step 3: 检查已鉴定要求
    if (cfg.requireIdentifiedForMaterial && !identified) {
        return false;
    }

    // Step 4: 检查稀有度要求
    if (cfg.filterByRarity && rarity < cfg.minRarityForMaterial) {
        return false;
    }

    // Step 5: 如果没有启用词缀筛选，所有通过上面检查的都是原料
    if (!cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        return true;
    }

    // Step 6: 词缀筛选模式（合规版：用 modIds 替代 modNames+modAffixes+modStatKeys）
    // modIds 已是白名单过滤后的 Mod.Id 列表（snake_case，与 selectedModifierKeys 同命名空间）
    // 原料 = 物品词缀中**没有任何一个**在用户勾选列表里
    bool hasMatchingMod = HasMatchingModifier(modIds, cfg.selectedModifierKeys);

    return !hasMatchingMod;  // 原料 = 没有匹配的词缀
}

// === 产物判定（基础版：无词缀信息时用此版本）===
// 产物 = 未鉴定的可合成物品（需要先NPC鉴定）或 有选中词缀的可合成物品
// 必须属于选中的子类
inline bool MatchesDesiredProductType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    if (!IsCraftableItem(path, baseType)) return false;

    // 白色品质非碑牌非珠宝物品：选中子类 = 产物（保留）
    if (IsNormalRarityCraftable(rarity, path, baseType)) {
        return IsItemWantedBySubCategory(path, baseType, cfg);
    }

    // 子类检查
    if (!IsItemWantedBySubCategory(path, baseType, cfg)) {
        return false;
    }

    // 未鉴定的可合成物品 → 始终是产物（需要先NPC鉴定）
    if (!identified) {
        return true;
    }

    // 已鉴定的物品：
    // 如果没有启用词缀筛选，所有已鉴定可合成物品都是产物候选
    if (!cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        return true;
    }

    // 有词缀筛选时，需要词缀信息才能判定
    return false;
}

// === 产物判定（词缀版：带词缀信息）===
// 产物 = 未鉴定（需要先NPC鉴定）或 至少有一个词缀在用户勾选列表里
// 必须属于选中的子类
// 【方案 B v1.3】参数从 (modNames, modAffixes, modStatKeys) 改为 (modIds, modHashes)
inline bool MatchesDesiredProductType(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg)
{
    (void)modHashes;  // 当前不参与决策，保留用于未来扩展和调试日志
    if (!IsCraftableItem(path, baseType)) return false;

    // 白色品质非碑牌非珠宝物品：选中子类 = 产物（保留）
    if (IsNormalRarityCraftable(rarity, path, baseType)) {
        return IsItemWantedBySubCategory(path, baseType, cfg);
    }

    // 子类检查
    if (!IsItemWantedBySubCategory(path, baseType, cfg)) {
        return false;
    }

    // 未鉴定的可合成物品 → 始终是产物（需要先NPC鉴定）
    if (!identified) {
        return true;
    }

    if (!cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        return true;
    }

    // 产物 = 至少有一个词缀在用户勾选列表里（合规版：用 modIds）
    return HasMatchingModifier(modIds, cfg.selectedModifierKeys);
}

// === 原料判定（词缀版 + 调试日志）===
inline bool MatchesDesiredReforgeTypeDebug(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg,
    std::string& debugLog)
{
    (void)modHashes;
    char buf[2048];
    std::string safePath = path;
    std::string safeBt = baseType;
    for (auto& c : safePath) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
    for (auto& c : safeBt) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
    if (safePath.size() > 100) safePath = safePath.substr(0, 100) + "...";
    if (safeBt.size() > 100) safeBt = safeBt.substr(0, 100) + "...";

    ::sprintf_s(buf, "\n[判定原料] Path='%s' BT='%s' rarity=%d ident=%d useModFilter=%d selectedKeys=%zu",
        safePath.c_str(), safeBt.c_str(), rarity, identified ? 1 : 0,
        cfg.useModifierFilterMode ? 1 : 0, cfg.selectedModifierKeys.size());
    debugLog += buf;

    // 列出所有选中的关键词
    if (!cfg.selectedModifierKeys.empty()) {
        debugLog += "\n  选中关键词: [";
        bool first = true;
        for (const auto& k : cfg.selectedModifierKeys) {
            if (!first) debugLog += ", ";
            first = false;
            debugLog += "'" + k + "'";
        }
        debugLog += "]";
    }

    // Step 1: 检查是否是可合成物品
    bool isCraftable = IsCraftableItem(path, baseType);
    ::sprintf_s(buf, "\n  Step1-IsCraftableItem: %d", isCraftable ? 1 : 0);
    debugLog += buf;
    if (!isCraftable) {
        debugLog += " → 不是可合成物品，跳过";
        return false;
    }

    // 白色品质非碑牌非珠宝物品：不需要鉴定，未选中子类 = 原料
    bool isNormalCraftable = IsNormalRarityCraftable(rarity, path, baseType);
    if (isNormalCraftable) {
        debugLog += "\n  [白色品质非碑牌珠宝] 不需要鉴定，未选中子类=原料";
        if (cfg.selectedSubCategories.empty()) {
            debugLog += "\n  → 无选中子类，无原料";
            return false;
        }
        bool unwanted = IsItemUnwantedBySubCategory(path, baseType, cfg);
        ::sprintf_s(buf, "\n  → 未选中子类检查: %s → %s",
            unwanted ? "未选中(原料)" : "已选中(产物)", unwanted ? "是原料" : "不是原料");
        debugLog += buf;
        return unwanted;
    }

    // Step 2: 子类检查
    auto subMatch = ClassifyItemSubCategory(path, baseType);
    bool subOk = cfg.selectedSubCategories.empty() ||
                 cfg.selectedSubCategories.count(subMatch.subCategoryId) > 0;
    ::sprintf_s(buf, "\n  Step2-子类检查: kind=%d subCatId=%d selectedCount=%zu → %s",
        (int)subMatch.kind, subMatch.subCategoryId, cfg.selectedSubCategories.size(),
        subOk ? "通过" : "不匹配");
    debugLog += buf;
    if (!subOk) {
        debugLog += " → 子类不匹配，不是原料";
        return false;
    }

    // Step 3: 检查已鉴定要求
    if (cfg.requireIdentifiedForMaterial && !identified) {
        ::sprintf_s(buf, "\n  Step3-鉴定检查: requireIdentified=%d ident=%d → 未鉴定，不是原料",
            cfg.requireIdentifiedForMaterial ? 1 : 0, identified ? 1 : 0);
        debugLog += buf;
        return false;
    }

    // Step 4: 检查稀有度要求
    if (cfg.filterByRarity && rarity < cfg.minRarityForMaterial) {
        ::sprintf_s(buf, "\n  Step4-稀有度检查: filterByRarity=ON rarity=%d < %d → 稀有度不足，不是原料",
            rarity, cfg.minRarityForMaterial);
        debugLog += buf;
        return false;
    } else if (!cfg.filterByRarity) {
        ::sprintf_s(buf, "\n  Step4-稀有度检查: filterByRarity=OFF 跳过稀有度检查 (rarity=%d)", rarity);
        debugLog += buf;
    } else {
        ::sprintf_s(buf, "\n  Step4-稀有度检查: filterByRarity=ON rarity=%d ≥ %d → 通过",
            rarity, cfg.minRarityForMaterial);
        debugLog += buf;
    }

    // Step 5: 词缀筛选
    if (!cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        debugLog += "\n  Step5-词缀筛选: 未启用或无选中词缀 → 通过所有检查 = 原料";
        return true;
    }

    // 【方案 B v1.3】modIds 已是白名单过滤后的 Mod.Id 列表
    ::sprintf_s(buf, "\n  Step5-物品词缀Id总数: %zu (已白名单过滤)", modIds.size());
    debugLog += buf;

    for (size_t i = 0; i < modIds.size() && i < 20; ++i) {
        std::string safeMod = modIds[i];
        for (auto& c : safeMod) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        if (safeMod.size() > 80) safeMod = safeMod.substr(0, 80) + "...";
        uint32_t h = (i < modHashes.size()) ? modHashes[i] : 0;
        ::sprintf_s(buf, "\n    Mod[%zu]: Id='%s' Hash=0x%08X", i, safeMod.c_str(), h);
        debugLog += buf;
    }
    if (modIds.size() > 20) {
        ::sprintf_s(buf, "\n    ... 还有 %zu 个词缀未显示", modIds.size() - 20);
        debugLog += buf;
    }

    // 词缀匹配检查：原料 = 没有任何匹配
    ModMatchDetail modDetail = HasMatchingModifierDebug(modIds, cfg.selectedModifierKeys);
    if (modDetail.matched) {
        std::string safeMatchedMod = modDetail.matchedMod;
        std::string safeMatchedKey = modDetail.matchedKey;
        for (auto& c : safeMatchedMod) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        for (auto& c : safeMatchedKey) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        ::sprintf_s(buf, "\n  Step5-词缀匹配: 物品Id'%s' 包含 选中关键词'%s' → 有匹配 → 不是原料(是产物)",
            safeMatchedMod.c_str(), safeMatchedKey.c_str());
        debugLog += buf;
        return false;
    } else {
        debugLog += "\n  Step5-词缀匹配: 无任何词缀匹配 → 通过所有检查 = 原料";
        return true;
    }
}

// === 产物判定（词缀版 + 调试日志）===
// 【方案 B v1.3】参数从 (modNames, modAffixes, modStatKeys) 改为 (modIds, modHashes)
inline bool MatchesDesiredProductTypeDebug(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const std::vector<std::string>& modIds,
    const std::vector<uint32_t>&    modHashes,
    const TabletReforgeConfig::Settings& cfg,
    std::string& debugLog)
{
    (void)modHashes;
    char buf[2048];
    std::string safePath = path;
    std::string safeBt = baseType;
    for (auto& c : safePath) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
    for (auto& c : safeBt) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
    if (safePath.size() > 100) safePath = safePath.substr(0, 100) + "...";
    if (safeBt.size() > 100) safeBt = safeBt.substr(0, 100) + "...";

    ::sprintf_s(buf, "\n[判定产物] Path='%s' BT='%s' rarity=%d ident=%d useModFilter=%d selectedKeys=%zu",
        safePath.c_str(), safeBt.c_str(), rarity, identified ? 1 : 0,
        cfg.useModifierFilterMode ? 1 : 0, cfg.selectedModifierKeys.size());
    debugLog += buf;

    // Step 1: 检查是否是可合成物品
    bool isCraftable = IsCraftableItem(path, baseType);
    ::sprintf_s(buf, "\n  Step1-IsCraftableItem: %d", isCraftable ? 1 : 0);
    debugLog += buf;
    if (!isCraftable) {
        debugLog += " → 不是可合成物品，跳过";
        return false;
    }

    // 白色品质非碑牌非珠宝物品：选中子类 = 产物（保留）
    bool isNormalCraftable = IsNormalRarityCraftable(rarity, path, baseType);
    if (isNormalCraftable) {
        bool wanted = IsItemWantedBySubCategory(path, baseType, cfg);
        ::sprintf_s(buf, "\n  [白色品质非碑牌珠宝] 选中子类=产物 → %s",
            wanted ? "是产物(已选中)" : "不是产物(未选中→原料)");
        debugLog += buf;
        return wanted;
    }

    // Step 2: 未鉴定 → 始终是产物
    if (!identified) {
        debugLog += "\n  Step2-未鉴定: 未鉴定的可合成物品 = 产物（需要先NPC鉴定）";
        return true;
    }

    // Step 3: 没有词缀筛选 → 所有已鉴定可合成物品都是产物
    if (!cfg.useModifierFilterMode || cfg.selectedModifierKeys.empty()) {
        debugLog += "\n  Step3-词缀筛选: 未启用或无选中 → 已鉴定可合成物品 = 产物";
        return true;
    }

    // 【方案 B v1.3】modIds 已是白名单过滤后的 Mod.Id 列表
    ::sprintf_s(buf, "\n  Step4-物品词缀Id总数: %zu (已白名单过滤)", modIds.size());
    debugLog += buf;
    for (size_t i = 0; i < modIds.size() && i < 20; ++i) {
        std::string safeMod = modIds[i];
        for (auto& c : safeMod) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        if (safeMod.size() > 80) safeMod = safeMod.substr(0, 80) + "...";
        uint32_t h = (i < modHashes.size()) ? modHashes[i] : 0;
        ::sprintf_s(buf, "\n    Mod[%zu]: Id='%s' Hash=0x%08X", i, safeMod.c_str(), h);
        debugLog += buf;
    }

    // 词缀匹配检查
    ModMatchDetail modDetail = HasMatchingModifierDebug(modIds, cfg.selectedModifierKeys);
    if (modDetail.matched) {
        std::string safeMatchedMod = modDetail.matchedMod;
        std::string safeMatchedKey = modDetail.matchedKey;
        for (auto& c : safeMatchedMod) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        for (auto& c : safeMatchedKey) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        ::sprintf_s(buf, "\n  Step4-词缀匹配: 物品Id'%s' 包含 选中关键词'%s' → 有匹配 = 产物",
            safeMatchedMod.c_str(), safeMatchedKey.c_str());
        debugLog += buf;
        return true;
    } else {
        debugLog += "\n  Step4-词缀匹配: 无任何词缀匹配 → 不是产物";
        return false;
    }
}

// === 向后兼容版本：3参数版本（默认已鉴定、稀有度0）===
// 警告：此版本假设物品已鉴定，不能正确处理未鉴定物品
// 请尽量使用5参数版本 MatchesDesiredProductType(path, baseType, rarity, identified, cfg)
inline bool MatchesDesiredProductType(
    const std::string& path,
    const std::string& baseType,
    const TabletReforgeConfig::Settings& cfg)
{
    return MatchesDesiredProductType(path, baseType, 0, true, cfg);
}

// ============================================================
// 调试用函数
// ============================================================

inline std::string DebugItemTypeTag(const std::string& path, const std::string& baseType) {
    std::vector<std::string> tags;
    if (IsWaystone(path, baseType))             tags.push_back("Waystone");
    if (IsPrecursorTablet(path, baseType))      tags.push_back("Precursor");
    if (IsTempleTablet(path, baseType))         tags.push_back("Temple");
    if (IsJewel(path, baseType))                tags.push_back("Jewel");
    if (IsRune(path, baseType))                 tags.push_back("Rune");
    if (IsEssence(path, baseType))              tags.push_back("Essence");
    if (IsLiquidEmotion(path, baseType))        tags.push_back("Liquid");
    if (IsCatalyst(path, baseType))             tags.push_back("Catalyst");
    if (IsAnyTabletLike(path, baseType))        tags.push_back("Tablet");
    if (tags.empty()) return std::string();
    std::string out = "<";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += ",";
        out += tags[i];
    }
    out += ">";
    return out;
}

// ============================================================
// ItemAnalysis 诊断结构
// ============================================================

struct ItemAnalysis {
    bool isWaystone = false;
    bool isPrecursorTablet = false;
    bool isTempleTablet = false;
    bool isJewel = false;
    bool isRune = false;
    bool isEssence = false;
    bool isLiquid = false;
    bool isCatalyst = false;
    bool matchesCurrentType = false;
    bool matchesAsMaterial = false;
    bool matchesAsProduct = false;
    bool hasPath = false;
    bool hasBaseType = false;
    bool identified = false;
    int rarity = 0;
    std::string shortPath;
    std::string baseType;
};

inline ItemAnalysis AnalyzeItem(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    ItemAnalysis a;
    a.isWaystone = IsWaystone(path, baseType);
    a.isPrecursorTablet = IsPrecursorTablet(path, baseType);
    a.isTempleTablet = IsTempleTablet(path, baseType);
    a.isJewel = IsJewel(path, baseType);
    a.isRune = IsRune(path, baseType);
    a.isEssence = IsEssence(path, baseType);
    a.isLiquid = IsLiquidEmotion(path, baseType);
    a.isCatalyst = IsCatalyst(path, baseType);
    a.matchesAsMaterial = MatchesDesiredReforgeType(path, baseType, rarity, identified, cfg);
    a.matchesAsProduct = MatchesDesiredProductType(path, baseType, rarity, identified, cfg);
    a.matchesCurrentType = a.matchesAsMaterial || a.matchesAsProduct;
    a.hasPath = !path.empty();
    a.hasBaseType = !baseType.empty();
    a.baseType = baseType;
    a.rarity = rarity;
    a.identified = identified;

    if (!path.empty()) {
        size_t slash = path.rfind('/');
        a.shortPath = (slash == std::string::npos) ? path : path.substr(slash + 1);
    }
    return a;
}

// 用于调试的匹配详情结构体
struct MatchDetail {
    bool matchesAsMaterial = false;
    bool matchesAsProduct = false;
    std::string typeKey;       // 类型标识符 (如 "Irradiated", "Breach" 等)
    int subCategoryId = 0;     // 子类别ID
    std::string debugDetail;   // 调试信息
};

// 生成详细的匹配分析文本
inline std::string DebugMatchDetail(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    std::string result;
    char buf[1024];

    ::sprintf_s(buf,
        "Path='%s'  BaseType='%s'  Rarity=%d  Identified=%d  [有Path=%d 有BT=%d]",
        path.empty() ? "(empty)" : path.c_str(),
        baseType.empty() ? "(empty)" : baseType.c_str(),
        rarity, identified ? 1 : 0,
        path.empty() ? 0 : 1, baseType.empty() ? 0 : 1);
    result += buf;

    result += "\n  路径匹配检查:";

    auto check = [&](const char* label, bool pass) {
        ::sprintf_s(buf, " [%c] %s", pass ? 'V' : 'X', label);
        result += buf;
    };

    check("path含'mapkey'", ContainsCI(path, "mapkey"));
    check("path含'toweraugment'", ContainsCI(path, "toweraugment"));
    check("path含'toweraugments'", ContainsCI(path, "toweraugments"));
    check("path含'precursor'", ContainsCI(path, "precursor"));
    check("path含'waystone'", ContainsCI(path, "waystone"));
    check("path含'maps'", ContainsCI(path, "maps"));
    check("path含'jewels'", ContainsCI(path, "jewels"));
    check("path含'rune'", ContainsCI(path, "rune"));
    check("path含'soulcores'", ContainsCI(path, "soulcores"));
    check("path含'essence'", ContainsCI(path, "essence"));
    check("path含'currencyessence'", ContainsCI(path, "currencyessence"));
    check("path含'distilledemotion'", ContainsCI(path, "distilledemotion"));
    check("path含'currencyjewelleryquality'", ContainsCI(path, "currencyjewelleryquality"));
    check("path含'currencyjewelquality'", ContainsCI(path, "currencyjewelquality"));
    check("path含'tablet'", ContainsCI(path, "tablet"));

    result += "\n  BaseType 检查:";
    check("bt含'precursor tablet'", ContainsCI(baseType, "precursor tablet"));
    check("bt含'waystone'", ContainsCI(baseType, "waystone"));
    check("bt含'map key'", ContainsCI(baseType, "map key"));
    check("bt含'jewel'", ContainsCI(baseType, "jewel"));
    check("bt含'ruby'", ContainsCI(baseType, "ruby"));
    check("bt含'emerald'", ContainsCI(baseType, "emerald"));
    check("bt含'sapphire'", ContainsCI(baseType, "sapphire"));
    check("bt含'diamond'", ContainsCI(baseType, "diamond"));
    check("bt含'timeless'", ContainsCI(baseType, "timeless"));
    check("bt含'time-lost'", ContainsCI(baseType, "time-lost"));
    check("bt含'rune'", ContainsCI(baseType, "rune"));
    check("bt含'essence'", ContainsCI(baseType, "essence"));
    check("bt含'distilled'", ContainsCI(baseType, "distilled"));
    check("bt含'catalyst'", ContainsCI(baseType, "catalyst"));
    check("bt含'tablet'", ContainsCI(baseType, "tablet"));

    result += "\n  分类结果:";
    auto a = AnalyzeItem(path, baseType, rarity, identified, cfg);

    ::sprintf_s(buf, " Waystone=%d Precursor=%d Temple=%d Jewel=%d Rune=%d Essence=%d Liquid=%d Catalyst=%d",
        a.isWaystone ? 1 : 0, a.isPrecursorTablet ? 1 : 0,
        a.isTempleTablet ? 1 : 0, a.isJewel ? 1 : 0,
        a.isRune ? 1 : 0, a.isEssence ? 1 : 0, a.isLiquid ? 1 : 0,
        a.isCatalyst ? 1 : 0);
    result += buf;

    const char* typeName = "Unknown";
    switch (static_cast<ReforgeItemType>(cfg.itemType)) {
        case ReforgeItemType::WaystonesOnly: typeName = "Waystone"; break;
        case ReforgeItemType::TabletsOnly: typeName = "PrecursorTablet"; break;
        case ReforgeItemType::AllTablets: typeName = "AllTablet"; break;
        case ReforgeItemType::JewelsOnly: typeName = "Jewel"; break;
        case ReforgeItemType::RunesOnly: typeName = "Rune"; break;
        case ReforgeItemType::EssencesOnly: typeName = "Essence"; break;
        case ReforgeItemType::LiquidsOnly: typeName = "Liquid"; break;
        case ReforgeItemType::CatalystsOnly: typeName = "Catalyst"; break;
        case ReforgeItemType::CustomKeywords: typeName = "Custom"; break;
    }

    ::sprintf_s(buf, "\n  当前设置: itemType=%s(%d) requireIdentified=%d",
        typeName, cfg.itemType, cfg.requireIdentified ? 1 : 0);
    result += buf;

    auto poe2Result = Poe2MatchReport(path, baseType, rarity, identified, cfg);
    result += poe2Result;

    if (path.empty() && baseType.empty()) {
        result += "\n  [宽松回退] Path和BT均为空，尝试基于Rarity判断:";
        ::sprintf_s(buf, " Rarity=%d → %s",
            rarity,
            IsLikelyCraftableMaterial(rarity) ? "可能是合成材料" : "不太可能是合成材料");
        result += buf;
    }

    ::sprintf_s(buf, " → 原料匹配=%s 产物匹配=%s",
        a.matchesAsMaterial ? "YES" : "NO", a.matchesAsProduct ? "YES" : "NO");
    result += buf;

    if (!a.matchesAsMaterial && !a.matchesAsProduct) {
        result += "  [不匹配原因]: ";
        if (cfg.requireIdentified && !identified) {
            result += "requireIdentified=ON但物品未鉴定; ";
        }
        if (path.empty() && baseType.empty()) {
            result += "Path和BaseType均为空，无法做精确匹配; ";
        }
        result += "路径和BaseType均不含目标类别关键词";
    }

    return result;
}

// 分析物品匹配详情（用于调试）
inline MatchDetail AnalyzeMatchDetail(
    const std::string& path,
    const std::string& baseType,
    int rarity,
    bool identified,
    const TabletReforgeConfig::Settings& cfg)
{
    MatchDetail md;
    md.matchesAsMaterial = MatchesDesiredReforgeType(path, baseType, rarity, identified, cfg);
    md.matchesAsProduct = MatchesDesiredProductType(path, baseType, rarity, identified, cfg);
    
    // 获取类型信息
    auto info = ClassifyItemSubCategory(path, baseType);
    md.typeKey = info.typeKey;
    md.subCategoryId = info.subCategoryId;
    
    // 生成调试详情
    md.debugDetail = DebugMatchDetail(path, baseType, rarity, identified, cfg);
    
    return md;
}

// ============================================================
// 兼容旧名
// ============================================================

inline bool IsRare(int rarity) { return rarity == kRarityRare; }

inline bool IsTempleTabletW(const std::wstring& path) {
    return ContainsCIW(path, L"toweraugment")
        || ContainsCIW(path, L"toweraugments")
        || ContainsCIW(path, L"precursor")
        || ContainsCIW(path, L"waystone")
        || ContainsCIW(path, L"mapkey")
        || ContainsCIW(path, L"incursion")
        || ContainsCIW(path, L"temple")
        || ContainsCIW(path, L"vaal");
}

// ============================================================
// UI 显示用：可合成物品列表
// ============================================================

struct CraftableItemInfo {
    int itemType;
    const char* displayName;
    const char* description;
    const char* pathPattern;
};

inline const CraftableItemInfo* GetCraftableItemList(int& count) {
    static const CraftableItemInfo list[] = {
        {static_cast<int>(ReforgeItemType::WaystonesOnly),
         "Waystone (地图钥匙)",
         "MapKeyTier 1~16, 3 same Tier → higher Tier",
         "Path含mapkey / BaseType含Waystone"},
        {static_cast<int>(ReforgeItemType::TabletsOnly),
         "Precursor Tablet (先行者碑牌)",
         "Breach/Expedition/Delirium/Ritual/Generic/Overseer/Abyss/Incursion + Mastered Domain",
         "Path含toweraugment / BaseType含Precursor Tablet"},
        {static_cast<int>(ReforgeItemType::AllTablets),
         "All Tablets (所有碑牌)",
         "Waystone + Precursor Tablet, unified",
         "Path/BaseType含mapkey/tablet/precursor"},
        {static_cast<int>(ReforgeItemType::JewelsOnly),
         "Jewel (珠宝)",
         "Ruby/Emerald/Sapphire/Diamond/Timeless/Time-Lost",
         "Path含jewels / BaseType含Ruby等"},
        {static_cast<int>(ReforgeItemType::RunesOnly),
         "Rune/SoulCore (符文)",
         "Desert/Glacial/Storm/Iron/Body/Mind等",
         "Path含rune/soulcores / BaseType含Rune"},
        {static_cast<int>(ReforgeItemType::EssencesOnly),
         "Essence (精髓)",
         "Life/Mana/Defence/Attack/Speed等",
         "Path含essence/currencyessence / BT含Essence"},
        {static_cast<int>(ReforgeItemType::LiquidsOnly),
         "Distilled Emotion (液态情感)",
         "Ire/Guilt/Greed/Paranoia/Envy/Disgust/Despair/Fear/Suffering/Isolation 共10类",
         "Path含distilledemotion / BaseType含Distilled"},
        {static_cast<int>(ReforgeItemType::CatalystsOnly),
         "Catalyst + Refined Catalyst (催化劑+精製催化劑)",
         "Jewellery催化劑×13 + Jewel精製催化劑×13 = 26种",
         "Path含currencyjewelleryquality/currencyjewelquality / BT含Catalyst"},
        {static_cast<int>(ReforgeItemType::CustomKeywords),
         "Custom Keywords (自定义关键词)",
         "Fill custom keywords in settings",
         "自定义 Path/BaseType 关键词匹配"},
    };
    count = sizeof(list) / sizeof(list[0]);
    return list;
}

// ============================================================
// 鉴定辅助函数
// ============================================================

// 判断物品是否需要鉴定（合成后出现未鉴定状态的规则）
// 规则：魔法/稀有品质的碑牌类物品需要鉴定，白色物品（催化剂/符文等）不需要
inline bool NeedsIdentification(int rarity, const std::string& path) {
    // 白色（普通）品质物品不需要鉴定
    if (rarity == kRarityNormal) return false;

    // 魔法或稀有品质的碑牌类物品需要鉴定
    // 原因：3合1合成后，魔法/稀有品质的碑牌会以未鉴定状态出现
    if (rarity == kRarityMagic || rarity == kRarityRare) {
        // 只有 TowerAugment 路径的碑牌才需要鉴定
        // 白色的催化剂、符文等合成后不会变成未鉴定
        if (ContainsCI(path, "toweraugment")) return true;
    }

    return false;
}

// ============================================================
// Mock 筛选/排序测试（本地验证用）
// 使用方法：在游戏内按 F8 热键触发，然后在 DebugView /
// F:\Trae\chuxue\debug\bug1.log 查看 [MOCK TEST] 开头的输出
// ============================================================
inline std::string RunMockFilterTests() {
    using namespace TabletReforgeConfig;
    std::string out;

    auto Print = [&](const std::string& s) {
        out += s + "\n";
        OutputDebugStringA(("[MOCK TEST] " + s + "\n").c_str());
    };

    Print("============= MOCK FILTER & SORT TESTS START =============");

    // —— Step 1: 构造 Settings 基准配置 ——
    Settings cfg;
    cfg.filterByRarity = true;
    cfg.minRarityForMaterial = 1;     // 魔法+
    cfg.useModifierFilterMode = true;
    cfg.requireIdentifiedForMaterial = true;
    cfg.itemType = 9;                  // AllTablets
    cfg.selectedModifierKeys = {
        "map_monster_potency",
        "towerincursiontokenchance",
        "TowerMapAdditionalModifier"
    };
    SyncBonusIdsToModifierKeys(cfg);   // 确保同步（这里其实不用 TypeConfig，但展示调用链）

    Print("Settings: filterByRarity=ON minRarity=1 useModifierFilterMode=ON reqIdentified=ON");
    Print("  selectedModifierKeys: map_monster_potency / towerincursiontokenchance / TowerMapAdditionalModifier");
    Print("");

    // —— Step 2: 构造 16 个测试物品 ——
    struct Case {
        const char* name;
        const char* path;
        const char* baseType;
        bool identified;
        int rarity;
        std::vector<std::string> mods;  // modNames
        bool expectedMaterial;          // 预期: 是否原料
        const char* reason;
        Settings* customCfg;            // 可选: 自定义配置（否则用 cfg）
    };

    // 子场景 9/13/14 需要自定义 cfg
    Settings cfg9 = cfg;  cfg9.filterByRarity = false;
    Settings cfg13 = cfg; cfg13.filterByRarity = false; cfg13.minRarityForMaterial = 0;
    Settings cfg14 = cfg; cfg14.requireIdentifiedForMaterial = false;

    Case cases[] = {
        // 1: 未鉴定 → 未鉴定要求=ON 直接 FAIL
        {"#01 未鉴定稀有碑牌", "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
            false, 2, {}, false, "未鉴定(requireIdentified=ON)", nullptr},
        // 2: 白色品质<最低要求
        {"#02 白色催化剂(普通品质)", "Metadata/Items/Currency/CurrencyItemReroll", "Orb of Chance",
            true, 0, {}, false, "rarity=0 < minRarity=1 (品质筛选=ON)", nullptr},
        // 3: 魔法品质+无选中词缀 → 是原料
        {"#03 魔法碑牌+无选中词缀", "Metadata/Items/TowerAugment/DeliriumAugment", "Delirium Tablet",
            true, 1, {"map_shrine_chance", "map_monster_pack_density"}, true,
            "rarity=1=minRarity + 无选中词缀 → 原料", nullptr},
        // 4: 稀有+选中词缀A → 产物
        {"#04 稀有碑牌+含 怪物效能(选中)", "Metadata/Items/TowerAugment/ExpeditionAugment", "Expedition Tablet",
            true, 2, {"map_monster_potency", "map_Item_quantity"}, false,
            "含选中词缀 map_monster_potency → 产物", nullptr},
        // 5: 稀有+选中词缀B → 产物
        {"#05 稀有碑牌+含 多利亚尼机会(选中)", "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
            true, 2, {"map_extra_gold", "towerincursiontokenchance"}, false,
            "含选中词缀 towerincursiontokenchance → 产物", nullptr},
        // 6: 稀有+无选中词缀 → 原料
        {"#06 稀有碑牌+无选中词缀", "Metadata/Items/TowerAugment/RitualAugment", "Ritual Tablet",
            true, 2, {"map_item_drop_rarity", "map_bonus_quantity"}, true,
            "rarity=2≥minRarity + 无选中词缀 → 原料", nullptr},
        // 7: 传奇品质+无选中词缀 → 原料（通常传奇不重铸，但筛选逻辑按配置）
        {"#07 传奇碑牌+无选中词缀", "Metadata/Items/TowerAugment/MasteredDomain", "Mastered Domain",
            true, 3, {"map_player_damage"}, true,
            "rarity=3≥minRarity + 无选中词缀 → 原料", nullptr},
        // 8: 魔法未鉴定 → 要求=ON, FAIL
        {"#08 魔法碑牌+未鉴定", "Metadata/Items/TowerAugment/BreachAugment", "Breach Tablet",
            false, 1, {}, false, "未鉴定(requireIdentified=ON)", nullptr},
        // 9: 白色但 filterByRarity=OFF → 是原料
        {"#09 白色催化剂 品质筛选=OFF", "Metadata/Items/Currency/CurrencyItemReroll", "Orb of Chance",
            true, 0, {}, true, "filterByRarity=OFF 跳过稀有度检查(白→原料)", &cfg9},
        // 10: 稀有+无关词缀 → 原料
        {"#10 稀有碑牌+无关词缀", "Metadata/Items/TowerAugment/OverseerAugment", "Overseer Tablet",
            true, 2, {"map_boss_fire_resist", "map_vaal_sidearea_chance"}, true,
            "无选中词缀 → 原料", nullptr},
        // 11: 魔法+选中C词缀 → 产物
        {"#11 魔法碑牌+额外随机词缀(选中)", "Metadata/Items/TowerAugment/AbyssAugment", "Abyss Tablet",
            true, 1, {"TowerMapAdditionalModifier"}, false,
            "含选中词缀 TowerMapAdditionalModifier → 产物", nullptr},
        // 12: 白色+选中词缀 → 品质筛选=ON 直接拒（不走到词缀判断）
        {"#12 白色物品+选中词缀 品质筛选=ON", "Metadata/Items/Currency/CurrencyItemReroll", "Orb of Chance",
            true, 0, {"map_monster_potency"}, false,
            "rarity=0<minRarity=1 (品质筛选=ON 先于词缀判断)", nullptr},
        // 13: 稀有+无选中词缀 品质筛选=OFF + minRarity=0 → 原料
        {"#13 稀有碑牌 品质筛选=OFF+minRarity=0", "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
            true, 2, {}, true, "filterByRarity=OFF minRarity=0 (宽松配置→原料)", &cfg13},
        // 14: 魔法未鉴定 但 requireIdentifiedForMaterial=OFF → 原料
        {"#14 魔法未鉴定 但无需已鉴定=原料", "Metadata/Items/TowerAugment/RitualAugment", "Ritual Tablet",
            false, 1, {"map_shrine_chance"}, true,
            "requireIdentified=OFF 且 rarity≥minRarity → 原料", &cfg14},
        // 15: 稀有+多词缀(含1个选中) → 产物
        {"#15 稀有碑牌 混合词缀(含1选中)", "Metadata/Items/TowerAugment/ExpeditionAugment", "Expedition Tablet",
            true, 2, {"aaa","map_monster_potency","bbb"}, false,
            "混合词缀中含 1 个选中 → 产物", nullptr},
        // 16: 大小写不敏感匹配
        {"#16 稀有碑牌 词缀大写(大小写不敏感)", "Metadata/Items/TowerAugment/DeliriumAugment", "Delirium Tablet",
            true, 2, {"TOWERMONSTERPOTENCY"}, false,
            "大小写不敏感匹配: TOWERMONSTERPOTENCY==map_monster_potency → 产物", nullptr},
    };

    int pass = 0, fail = 0;
    Print("");
    Print("--- PART 1: 筛选逻辑 (16 test cases) ---");
    for (const auto& c : cases) {
        Settings& s = c.customCfg ? *c.customCfg : cfg;
        std::vector<uint32_t> emptyHashes;
        bool actual = MatchesDesiredReforgeType(c.path, c.baseType, c.identified, c.rarity,
            c.mods, emptyHashes, s);
        const char* status = (actual == c.expectedMaterial) ? "PASS" : "FAIL";
        if (actual == c.expectedMaterial) ++pass; else ++fail;

        char line[1024];
        std::snprintf(line, sizeof(line), "[%s] %s → actual=%d (expected=%d) 原因: %s\n  -> ident=%d rarity=%d mods=%zu cfg.filterByRarity=%d cfg.reqIdent=%d",
            status, c.name, (int)actual, (int)c.expectedMaterial, c.reason,
            (int)c.identified, c.rarity, c.mods.size(),
            s.filterByRarity ? 1 : 0, s.requireIdentifiedForMaterial ? 1 : 0);
        Print(line);
    }

    char summary[256];
    std::snprintf(summary, sizeof(summary),
        "筛选逻辑总分: PASS=%d  FAIL=%d  总=%d", pass, fail, pass + fail);
    Print("");
    Print(summary);

    // —— Step 3: 验证 slot 排序（从左至右, 从上至下）——
    Print("");
    Print("--- PART 2: 物品槽位排序 (从左→右 / 上→下) ---");
    // StashTablet 定义自 StashOps.h，我们只验证通用的 slotX/slotY 排序逻辑
    struct SlotItem {
        int slotX;
        int slotY;
        const char* name;
    };
    std::vector<SlotItem> items = {
        {3, 2, "C2"}, {1, 1, "A1"}, {5, 0, "E0"}, {2, 1, "B1"},
        {0, 2, "D2_0"}, {1, 2, "D2_1"}, {0, 0, "A0"}, {4, 1, "C1"}
    };
    std::sort(items.begin(), items.end(), [](const SlotItem& a, const SlotItem& b) {
        if (a.slotY != b.slotY) return a.slotY < b.slotY;
        return a.slotX < b.slotX;
    });
    std::string orderStr;
    for (auto& i : items) orderStr += std::string(i.name) + " ";
    Print("排序后顺序: " + orderStr);
    bool sortOk = (items[0].slotY==0 && items[0].slotX==0 &&  // A0
                   items[1].slotY==0 && items[1].slotX==5 &&  // E0
                   items[2].slotY==1 && items[2].slotX==1 &&  // A1
                   items[3].slotY==1 && items[3].slotX==2 &&  // B1
                   items[4].slotY==1 && items[4].slotX==4);   // C1
    Print(std::string("排序正确性: ") + (sortOk ? "PASS" : "FAIL")
        + " (期望: 先按Y升序，同一Y按X升序 = A0 E0 A1 B1 C1 C2 D2_0 D2_1 ...)");

    // —— Step 4: 调试信息：MatchesDesiredReforgeTypeDebug 输出（演示用，取 case4）——
    Print("");
    Print("--- PART 3: MatchesDesiredReforgeTypeDebug 详细报告 (取 #04 稀有+选中词缀) ---");
    {
        auto& c = cases[3];  // #04
        Settings& s = cfg;
        std::string dbg;
        std::vector<uint32_t> emptyHashes;
        MatchesDesiredReforgeTypeDebug(c.path, c.baseType, c.identified, c.rarity,
            c.mods, emptyHashes, s, dbg);
        Print(dbg);
    }

    // —— Step 5: PART 4 - 验证 POE2 mod id 映射（完整链路）——
    // 模拟用户在 UI 中勾选 Bonus.Id，通过 SyncBonusIdsToModifierKeys 自动加入 POE2 mod id
    Print("");
    Print("--- PART 4: POE2 mod id 映射验证 (TypeConfig.selectedBonusIds → SyncBonusIds → 匹配) ---");
    {
        Settings cfgMap;
        cfgMap.filterByRarity = true;
        cfgMap.minRarityForMaterial = 1;
        cfgMap.useModifierFilterMode = true;
        cfgMap.requireIdentifiedForMaterial = true;

        // 模拟用户在 UI 中勾选了以下 Bonus.Id（来自 TabletBonusCatalog）
        // 在 Temple 类型下勾选 TowerIncursionTokenChance（從瓦爾烽塔獲得額外水晶的幾率提高）
        // 在通用词缀下勾选 TowerMonsterEffectiveness（怪物效能提高）
        // 在通用词缀下勾选 TowerMapAdditionalModifier（地图有额外随机词缀）
        TypeConfig* templeCfg = cfgMap.FindType("Temple");
        if (templeCfg) {
            templeCfg->selectedBonusIds.push_back("TowerIncursionTokenChance");
        }
        // 通用词缀通过全局 selectedBonusIds 添加
        cfgMap.selectedBonusIds.insert("TowerMonsterEffectiveness");
        cfgMap.selectedBonusIds.insert("TowerMapAdditionalModifier");

        // 执行同步：这应该自动把 POE2 mod id 关键词加入 selectedModifierKeys
        SyncBonusIdsToModifierKeys(cfgMap);

        Print("用户勾选的 Bonus.Id: TowerIncursionTokenChance, TowerMonsterEffectiveness, TowerMapAdditionalModifier");
        char buf[512];
        std::snprintf(buf, sizeof(buf), "SyncBonusIdsToModifierKeys 后 selectedModifierKeys 数量: %zu", cfgMap.selectedModifierKeys.size());
        Print(buf);

        // 验证关键 POE2 mod id 关键词是否被加入
        auto checkKey = [&](const std::string& key) -> bool {
            return cfgMap.selectedModifierKeys.find(key) != cfgMap.selectedModifierKeys.end()
                || cfgMap.selectedModifierKeys.find(TabletReforgeGame::ToLowerCopy(key)) != cfgMap.selectedModifierKeys.end();
        };
        bool hasPotency = checkKey("map_monster_potency");
        bool hasCrystal = checkKey("map_incursion_vaal_beacon_crystal");
        bool hasAddMod = checkKey("map_additional_modifier");
        Print(std::string("  map_monster_potency 已加入: ") + (hasPotency ? "YES ✓" : "NO ✗"));
        Print(std::string("  map_incursion_vaal_beacon_crystal 已加入: ") + (hasCrystal ? "YES ✓" : "NO ✗"));
        Print(std::string("  map_additional_modifier 已加入: ") + (hasAddMod ? "YES ✓" : "NO ✗"));

        // 测试：用日志中实际出现的游戏 mod id 匹配
        struct MapCase {
            const char* name;
            const char* path;
            const char* baseType;
            bool identified;
            int rarity;
            std::vector<std::string> mods;  // 模拟 ReadItemMods 返回的实际 mod id
            bool expectedMaterial;          // 预期: 是否原料
            const char* reason;
        };
        MapCase mapCases[] = {
            // 日志中实际出现的 Temple 碑牌 mod id
            {"M01 神庙碑牌 含怪物效能(map_monster_potency_+.) → 产物",
                "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
                true, 2, {"tower_add_incursion_to_X_maps", "map_monster_potency_+.", "map_item_drop_rarity_+."},
                false, "map_monster_potency_+. 含 map_monster_potency → 产物"},
            {"M02 神庙碑牌 含瓦尔烽塔水晶(map_incursion_vaal_beacon_crystal) → 产物",
                "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
                true, 2, {"tower_add_incursion_to_X_maps", "map_incursion_vaal_beacon_crystal_chance_+.", "map_shrine_chance_+."},
                false, "map_incursion_vaal_beacon_crystal_chance_+. 含 map_incursion_vaal_beacon_crystal → 产物"},
            {"M03 神庙碑牌 无选中词缀(只有map_strongbox_chance) → 原料",
                "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
                true, 2, {"tower_add_incursion_to_X_maps", "map_strongbox_chance_+.", "map_extra_gold_piles_chance_."},
                true, "无选中词缀 → 原料"},
            {"M04 神庙碑牌 含额外随机词缀(map_random_modifier) → 产物",
                "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
                true, 2, {"tower_add_incursion_to_X_maps", "map_random_modifier_count_+", "map_experience_gain_+."},
                false, "map_random_modifier_count_+ 含 map_random_modifier → 产物"},
            {"M05 神庙碑牌 含多个mod但无选中 → 原料",
                "Metadata/Items/TowerAugment/IncursionAugment", "Incursion Tablet",
                true, 2, {"tower_add_incursion_to_X_maps", "map_incursion_vaal_beacon_pack_size_+.", "map_stone_circle_chance_+."},
                true, "pack_size/stone_circle 不在选中词缀中 → 原料"},
        };

        int mapPass = 0, mapFail = 0;
        for (const auto& mc : mapCases) {
            std::vector<uint32_t> emptyHashes;
            bool actual = MatchesDesiredReforgeType(mc.path, mc.baseType, mc.identified, mc.rarity,
                mc.mods, emptyHashes, cfgMap);
            const char* status = (actual == mc.expectedMaterial) ? "PASS" : "FAIL";
            if (actual == mc.expectedMaterial) ++mapPass; else ++mapFail;

            char line[1024];
            std::snprintf(line, sizeof(line), "[%s] %s → actual=%d (expected=%d) 原因: %s",
                status, mc.name, (int)actual, (int)mc.expectedMaterial, mc.reason);
            Print(line);
        }

        char mapSummary[256];
        std::snprintf(mapSummary, sizeof(mapSummary),
            "POE2 mod id 映射验证: PASS=%d  FAIL=%d  总=%d", mapPass, mapFail, mapPass + mapFail);
        Print("");
        Print(mapSummary);
    }

    Print("");
    Print("============= MOCK FILTER & SORT TESTS END =============");
    return out;
}

// ============================================================
} // namespace TabletReforgeGame