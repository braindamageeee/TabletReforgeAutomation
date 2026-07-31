#pragma once
// =============================================================================
// StashTypeTable.h
// Hardcoded PoE 2 StashType table (extracted from Content.ggpk).
//
// Source: data/balance/stashtype.datc64 (25 rows), parsed via ggpk-explorer
// toolchain (Oodle-decompressed bundle -> .dat64 binary -> JSON).
//
// Schema (verified by hex dump, 48 bytes/row):
//   +0  Id          string   "NormalStash"...
//   +8  StashId     enumrow  row index 0..24
//   +12 Id2         string   usually == Id
//   +20 StorageSlots i32
//   +24 _           i32
//   +28 _           i32
//   +32 IconPC      string   Art/2DArt/UIImages/InGame/MTX/<X>TabIcon
//   +40 IconConsole string   Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/...
//
// IconPC paths map to bundle files at:
//   art/textures/interface/2d/2dart/uiimages/ingame/mtx/<x>tabicon.dds
// (28x28 BC7 DDS, already extracted to resources/stash_icons/<id>_<name>.dds)
// =============================================================================

#include <string>
#include <string_view>
#include <array>
#include <optional>

namespace TabletReforgeGame {

struct StashTypeEntry {
    int         stashId;          // 0..24
    const char* id;               // "NormalStash", "CurrencyStash", ...
    const char* id2;              // usually same as id
    int         storageSlots;     // 宽（X方向格子数，对应 ggpk StorageSlots 字段）
    int         gridHeight;       // 高（Y方向格子数，对应 ggpk _ 字段）
    const char* iconPc;           // Art/2DArt/UIImages/InGame/MTX/...TabIcon
    const char* iconConsole;      // Art/.../ConsoleNew/Stash/Icons/Console... (may be empty)
    const char* ddsFileName;      // local resource file name (in resources/stash_icons/)
    const char* chineseName;     // 中文显示名
};

// 25 entries, ordered by StashId
// gridHeight 来源：stashtype.json Unk004 字段（2026最新版解包，poe2-data-main）
//   注意：Unk003=列数(X)，Unk004=行数(Y)，与StashTypeEntry.storageSlots/gridHeight对应
//   2026-06修正：Currency(53→41), Unique(146→94), Map(12x8→12x6), Essence(88→30),
//               Fragment(18x6→152x3), PCBangEssence(88→30) 匹配 poe2-data-main/stashtype.json
inline constexpr std::array<StashTypeEntry, 25> kStashTypeTable = {{
    { 0,  "NormalStash",          "NormalStash",          12,  12,  "Art/2DArt/UIImages/InGame/MTX/BlankTabIcon",                "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleRegularTabIcon",       "00_NormalStash.dds",   "普通仓库" },
    { 1,  "PremiumStash",         "PremiumStash",         12,  12,  "Art/2DArt/UIImages/InGame/MTX/PremiumTabIcon",              "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsolePremiumTabIcon",       "01_PremiumStash.dds",  "高级仓库" },
    { 2,  "TradeStash",           "TradeStash",           12,  12,  "Art/2DArt/UIImages/InGame/MTX/BlankTabIcon",                "",                                                                              "02_TradeStash.dds",     "交易仓库" },
    { 3,  "CurrencyStash",        "CurrencyStash",        41,  4,   "Art/2DArt/UIImages/InGame/MTX/CurrencyTabIcon",            "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleCurrencyTabIcon",     "03_CurrencyStash.dds", "货币仓库" },
    { 4,  "UniqueStash",          "UniqueStash",          94,  4,   "Art/2DArt/UIImages/InGame/MTX/UniqueTabIcon",              "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleUniqueTabIcon",       "04_UniqueStash.dds",   "传奇仓库" },
    { 5,  "MapStash",             "MapStash",             12,  6,   "Art/2DArt/UIImages/InGame/MTX/MapTabIcon",                 "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleMapTabIcon",          "05_MapStash.dds",      "地图仓库" },
    { 6,  "DivinationCardStash",  "DivinationCardStash",  0,   1,   "Art/2DArt/UIImages/InGame/MTX/DivinationTabIcon",          "",                                                                              "06_DivinationCardStash.dds", "预言卡仓库" },
    { 7,  "QuadStash",            "QuadStash",            24,  24,  "Art/2DArt/UIImages/InGame/MTX/QuadTabIcon",                "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleQuadTabIcon",         "07_QuadStash.dds",     "四方格仓库" },
    { 8,  "EssenceStash",         "EssenceStash",         30,  4,   "Art/2DArt/UIImages/InGame/MTX/EssenceTabIcon",             "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleEssenceTabIcon",      "08_EssenceStash.dds",  "精髓仓库" },
    { 9,  "FragmentStash",        "FragmentStash",        152, 3,   "Art/2DArt/UIImages/InGame/MTX/FragmentTabIcon",            "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleFragmentTabIcon",     "09_FragmentStash.dds", "碎片/碑牌仓库" },
    { 10, "PCBangPremiumStash",   "PCBangPremiumStash",   12,  12,  "Art/2DArt/UIImages/InGame/MTX/PremiumTabIcon",             "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsolePremiumTabIcon",      "10_PCBangPremiumStash.dds", "PC高级仓库" },
    { 11, "PCBangEssenceStash",   "PCBangEssenceStash",   30,  4,   "Art/2DArt/UIImages/InGame/MTX/EssenceTabIcon",             "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleEssenceTabIcon",      "11_PCBangEssenceStash.dds", "PC精髓仓库" },
    { 12, "DelveStash",           "DelveStash",           41,  4,   "Art/2DArt/UIImages/InGame/MTX/DelveTabIcon",               "",                                                                              "12_DelveStash.dds",    "深渊仓库" },
    { 13, "BlightStash",          "BlightStash",          66,  4,   "Art/2DArt/UIImages/InGame/MTX/BlightTabIcon",              "",                                                                              "13_BlightStash.dds",   "疫情仓库" },
    { 14, "MetamorphStash",       "MetamorphStash",       62,  5,   "Art/2DArt/UIImages/InGame/MTX/MetamorphTabIcon",           "",                                                                              "14_UltimatumStash.dds", "终极仓库" },
    { 15, "DeliriumStash",        "DeliriumStash",        40,  1,   "Art/2DArt/UIImages/InGame/MTX/DeliriumTabIcon",            "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleDeliriumTabIcon",     "15_DeliriumStash.dds", "试炼仓库" },
    { 16, "Folder",               "Folder",               0,   0,   "Art/2DArt/UIImages/InGame/MTX/StashFolderTabIcon",         "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleStashFolderTabIcon",  "16_Folder.dds",        "文件夹" },
    { 17, "FlaskStash",           "FlaskStash",           250, 4,   "Art/2DArt/UIImages/InGame/MTX/FlaskStash",                 "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleFlaskStash",          "17_FlaskStash.dds",    "药剂仓库" },
    { 18, "GemStash",             "GemStash",             250, 2,   "Art/2DArt/UIImages/InGame/MTX/GemStash",                   "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleGemStash",            "18_GemStash.dds",      "技能宝石仓库" },
    { 19, "SocketableStash",      "SocketableStash",      214, 1,   "Art/2DArt/UIImages/InGame/MTX/SocketableTabIcon",          "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleSocketableTabIcon",   "19_SocketableStash.dds", "可镶嵌仓库" },
    { 20, "ExpeditionStash",      "ExpeditionStash",      24,  4,   "Art/2DArt/UIImages/InGame/MTX/ExpeditionTabIcon",          "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleExpeditionTabIcon",   "20_ExpeditionStash.dds", "远征仓库" },
    { 21, "RitualStash",          "RitualStash",          42,  1,   "Art/2DArt/UIImages/InGame/MTX/RitualTabIcon",              "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleRitualTabIcon",       "21_RitualStash.dds",   "仪式仓库" },
    { 22, "BreachStash",          "BreachStash",          13,  5,   "Art/2DArt/UIImages/InGame/MTX/BreachTabIcon",              "",                                                                              "22_BreachStash.dds",   "裂隙仓库" },
    { 23, "AbyssStash",           "AbyssStash",           12,  3,   "Art/2DArt/UIImages/InGame/MTX/AbyssTabIcon",               "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleAbyssTabIcon",        "23_AbyssStash.dds",    "深渊仓库" },
    { 24, "RelicStash",           "RelicStash",           12,  12,  "Art/2DArt/UIImages/InGame/MTX/RelicStash/RelicTabIcon",    "Art/2DArt/UIImages/InGame/ConsoleNew/Stash/Icons/ConsoleRelicTabIcon",        "24_RelicStash.dds",    "遗物仓库" },
}};

// ---- Lookup helpers --------------------------------------------------------

inline const StashTypeEntry* FindStashTypeById(std::string_view id) {
    for (const auto& e : kStashTypeTable) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

inline const StashTypeEntry* FindStashTypeByStashId(int stashId) {
    for (const auto& e : kStashTypeTable) {
        if (e.stashId == stashId) return &e;
    }
    return nullptr;
}

// Map a stash tab name (as reported by SDK Inventory API, e.g. "CurrencyStash")
// to its hardcoded StashTypeEntry. Returns nullptr if unknown.
inline const StashTypeEntry* ResolveStashType(std::string_view tabName) {
    return FindStashTypeById(tabName);
}

// Total number of distinct stash types (including Folder / PCBang variants).
inline constexpr size_t StashTypeCount() { return kStashTypeTable.size(); }

// ============================================================
// 基于格子数的仓库Tab识别（不依赖名称，仅依赖 ggpk 解包的尺寸数据）
// 用途：Inventory_NNN 这种动态命名的物品栏无法通过名称匹配识别，
//       但仓库Tab有固定的格子规格（StorageSlots × gridHeight），
//       可通过比对 TotalBoxesX × TotalBoxesY 精确识别。
// 数据来源：data_balance_stashtype.datc64.json
// ============================================================

// 检查给定的宽×高是否匹配任何已知仓库Tab规格
// 返回匹配的 StashTypeEntry（ nullptr 表示不匹配，很可能是装备槽位或其他面板）
inline const StashTypeEntry* FindStashTypeByGridSize(int width, int height) {
    // 过滤无效尺寸
    if (width <= 0 || height <= 0) return nullptr;
    for (const auto& e : kStashTypeTable) {
        // 跳过 Folder / DivinationCardStash / UltimatumStash（格子数为0的特殊类型）
        if (e.storageSlots <= 0 || e.gridHeight <= 0) continue;
        if (e.storageSlots == width && e.gridHeight == height) {
            return &e;
        }
    }
    return nullptr;
}

// ============================================================
// 启发式判断：大尺寸Inventory即使不匹配ggpk精确规格，也大概率是仓库Tab
// 用于过滤 Inventory_NNN 动态命名时，POE2新增类型或自定义类型未收录到 kStashTypeTable 的情况。
//
// bug1.log 中被误过滤的真实仓库Tab案例：
//   Inventory_131: 107x12=1284 slots (超大尺寸，明显不是装备槽位)
//   Inventory_136: 14x9=126 slots
//   Inventory_137: 37x10=370 slots
//
// 装备槽位尺寸上限参考（bug1.log中最大的装备槽位）：
//   HeistNpcEquipment5: 64x5=320（但名称匹配 HeistNpcEquipment 模式已被 IsEquipmentSlotName 过滤）
//   AnimatedArmour1: 10x4=40
//   PassiveJewels1: 57x1=57（1行长条，height=1特征明显）
//   HeistNpcEquipment1: 100x4=400（但名称被过滤）
// ============================================================
inline bool IsHeuristicLargeStashTab(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    const int total = width * height;

    // 先排除明确的非仓库特征（长条单行/单列等）
    // PassiveJewels1 57x1, MemoryLineMaps 8x1 这类单行长条都不是仓库Tab
    if (height == 1 && width > 20) return false;  // 超宽单行：PassiveJewels/RitualSavedRewards等
    if (width == 1 && height > 20) return false;  // 超高单列

    // ★ 核心启发式：大尺寸Inventory几乎都是仓库Tab
    // 任何通过 IsEquipmentSlotName 名称过滤后仍残留、且尺寸较大的，都是仓库Tab
    // 阈值选择：
    //   - AbyssStash: 12x3=36 → 最低标准
    //   - 装备槽位中接近的：ThreeToOneInput 4x4=16，Currency1 2x3=6 → 远小于36
    //   - 小Fragment子页：3x2=6, 4x1=4 → 小于36（这些后续会被识别为子Tab）
    if (total >= 36) return true;

    // 稍低阈值：高度>=4 或 宽度>=10 的Inventory，基本不可能是装备槽位
    //   MapStash: 12x8=96 匹配此规则
    //   EssenceStash: 88x4=352 匹配此规则
    if (height >= 4 || width >= 10) {
        if (total >= 20) return true;  // 适度降低总格子要求
    }

    return false;
}

// 判断一个 Inventory 是否"看起来像"仓库Tab（基于格子尺寸）
// 用于过滤 Inventory_NNN 这种动态命名的物品栏：
//   - 如果名称匹配已知仓库类型 → 直接返回 true（名称优先）
//   - 如果名称是 Inventory_NNN，但格子尺寸匹配已知仓库Tab → 返回 true
//   - 启发式匹配：大尺寸Inventory即使不匹配规格，也返回true（POE2新增类型）
//   - 否则 → 返回 false（很可能是装备槽位或其他小面板）
// 注意：装备槽位（Weapon1/BodyArmour1 等）应先用 IsEquipmentSlotName 过滤，
//       本函数专注于"不依赖名称"的格子尺寸识别。
inline bool IsLikelyStashTabByGridSize(int width, int height) {
    // 先尝试精确匹配ggpk规格
    if (FindStashTypeByGridSize(width, height) != nullptr) return true;
    // 再尝试启发式大尺寸匹配
    return IsHeuristicLargeStashTab(width, height);
}

} // namespace TabletReforgeGame
