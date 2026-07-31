// test_non_stash_filter.cpp — 验证 IsNonStashInventory 综合过滤函数（独立版）
// 目标：复现 bug1.log 中 "Inventory开头除了Inventory_146 为仓库，其他大多都是装备槽位" 的问题，
//      验证基于 ggpk 解包数据的格子尺寸过滤 + InventoryId 校验能正确区分：
//        - 真仓库Tab（Inventory_143，匹配 ggpk 规格）
//        - 装备槽位（Inventory_122~130，14 slots 不匹配 ggpk）
//        - 负数 ID 特殊槽位（Inventory_-2147483648，即使 12×12 也过滤）
//        - 已知仓库类型名（NormalStash, FragmentStash 等）
//        - 角色装备槽位（Weapon1, BodyArmour1 等）
//
// 编译：cl /EHsc /std:c++20 test_non_stash_filter.cpp /Fe:test_non_stash_filter.exe
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <array>
#include <vector>

// ============================================================
// 从 StashTypeTable.h 复制的 ggpk 解包数据（25 种仓库Tab类型）
// 来源：data_balance_stashtype.datc64.json
// ============================================================
namespace TestImpl {

struct StashTypeEntry {
    int         stashId;
    const char* id;
    int         storageSlots;  // 宽（X方向格子数）
    int         gridHeight;    // 高（Y方向格子数）
};

inline constexpr std::array<StashTypeEntry, 25> kStashTypeTable = {{
    { 0,  "NormalStash",          12,  12 },
    { 1,  "PremiumStash",         12,  12 },
    { 2,  "TradeStash",           12,  12 },
    { 3,  "CurrencyStash",        53,  4  },
    { 4,  "UniqueStash",          146, 4  },
    { 5,  "MapStash",             12,  8  },
    { 6,  "DivinationCardStash",  0,   1  },
    { 7,  "QuadStash",            24,  24 },
    { 8,  "EssenceStash",         88,  4  },
    { 9,  "FragmentStash",        18,  6  },
    { 10, "PCBangPremiumStash",   12,  12 },
    { 11, "PCBangEssenceStash",   88,  4  },
    { 12, "DelveStash",           41,  4  },
    { 13, "BlightStash",          66,  4  },
    { 14, "UltimatumStash",       0,   1  },
    { 15, "DeliriumStash",        60,  1  },
    { 16, "Folder",               0,   0  },
    { 17, "FlaskStash",           250, 4  },
    { 18, "GemStash",             250, 2  },
    { 19, "SocketableStash",      214, 1  },
    { 20, "ExpeditionStash",      24,  4  },
    { 21, "RitualStash",          42,  1  },
    { 22, "BreachStash",          13,  5  },
    { 23, "AbyssStash",           12,  3  },
    { 24, "RelicStash",           12,  12 },
}};

inline const StashTypeEntry* FindStashTypeById(std::string_view id) {
    for (const auto& e : kStashTypeTable) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

inline const StashTypeEntry* FindStashTypeByGridSize(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    for (const auto& e : kStashTypeTable) {
        if (e.storageSlots <= 0 || e.gridHeight <= 0) continue;
        if (e.storageSlots == width && e.gridHeight == height) {
            return &e;
        }
    }
    return nullptr;
}

inline bool IsLikelyStashTabByGridSize(int width, int height) {
    return FindStashTypeByGridSize(width, height) != nullptr;
}

// 从 TabletFilter.h 复制的装备槽位名称匹配
inline bool IsEquipmentSlotName(const std::string& name) {
    if (name.empty()) return false;

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
        "Map1",  // 地图槽位
    };
    for (const char* slot : kExactEquipSlots) {
        if (name == slot) return true;
    }

    static const char* kPatternSlots[] = {
        "MasterCrafting",
        "HeistNpcEquipment",
        "MercenaryCompanion",
        "DONOTUSE",
        "UNUSED",
    };
    for (const char* pat : kPatternSlots) {
        if (name.find(pat) != std::string::npos) return true;
    }

    return false;
}

// ============================================================
// 综合过滤函数（与项目代码同步）
// ============================================================
inline bool IsNonStashInventory(const std::string& name, int width, int height, int inventoryId) {
    // 1. 主背包保留
    if (name.rfind("MainInventory", 0) == 0) return false;

    // 2. 已知仓库类型名保留
    if (FindStashTypeById(name) != nullptr) return false;

    // 3. 装备槽位过滤
    if (IsEquipmentSlotName(name)) return true;

    // 4. InventoryId 校验：负数 ID 一定是游戏内部特殊槽位
    if (inventoryId < 0) return true;

    // 5. 名称中带负数 ID 的 Inventory（兜底）
    if (name.rfind("Inventory_-", 0) == 0) return true;

    // 6 & 7. 基于格子尺寸判断（依赖 ggpk 解包的 stashtype 数据）
    if (!IsLikelyStashTabByGridSize(width, height)) {
        return true;  // 格子尺寸不匹配任何仓库Tab → 过滤
    }

    return false;  // 格子尺寸匹配仓库Tab → 保留
}

} // namespace TestImpl

// ============================================================
// 测试用例数据（来自 bug1.log 行 156-186 的真实 Inventory 列表）
// 日志只显示 slots 总数，这里基于 slots 推断可能的 width × height
// ============================================================
struct MockInv {
    int inventoryId;
    std::string name;
    int width;    // TotalBoxesX
    int height;   // TotalBoxesY
    int slots;    // width * height
    bool expectFiltered;  // true = 应被过滤，false = 应保留
    std::string reason;   // 期望原因
};

int main() {
    printf("=== IsNonStashInventory 综合过滤验证 ===\n");
    printf("（基于 ggpk 解包的 stashtype 数据 + InventoryId 校验）\n\n");

    // ---- 测试1: ggpk 仓库Tab规格表完整性 ----
    printf("--- 测试1: ggpk 仓库Tab规格表（25 种类型）---\n");
    {
        struct GridSpec { int w, h; const char* id; };
        GridSpec specs[] = {
            {12, 12, "NormalStash/PremiumStash/RelicStash"},
            {53, 4,  "CurrencyStash"},
            {146, 4, "UniqueStash"},
            {12, 8,  "MapStash"},
            {24, 24, "QuadStash"},
            {88, 4,  "EssenceStash"},
            {18, 6,  "FragmentStash"},
            {41, 4,  "DelveStash"},
            {66, 4,  "BlightStash"},
            {60, 1,  "DeliriumStash"},
            {250, 4, "FlaskStash"},
            {250, 2, "GemStash"},
            {214, 1, "SocketableStash"},
            {24, 4,  "ExpeditionStash"},
            {42, 1,  "RitualStash"},
            {13, 5,  "BreachStash"},
            {12, 3,  "AbyssStash"},
        };
        bool pass = true;
        for (const auto& s : specs) {
            auto* entry = TestImpl::FindStashTypeByGridSize(s.w, s.h);
            bool ok = (entry != nullptr);
            printf("  [%s] %dx%d (%s): %s\n",
                   ok ? "PASS" : "FAIL",
                   s.w, s.h, s.id,
                   ok ? entry->id : "未匹配");
            if (!ok) pass = false;
        }
        printf("  结果: %s\n\n", pass ? "ALL PASS" : "FAILED");
    }

    // ---- 测试2: 日志中真实 Inventory 列表（bug1.log 行 156-186）----
    printf("--- 测试2: bug1.log 真实 Inventory 列表过滤验证 ---\n");
    std::vector<MockInv> realInvs = {
        // 真仓库Tab（用户说 Inventory_146 是仓库，日志中是 Inventory_143）
        // slots=212 → 推断 4×53=212（匹配 CurrencyStash 53×4，注意 width/height 顺序）
        // 注意：ggpk 中 CurrencyStash 是 StorageSlots=53, gridHeight=4
        //       但 TotalBoxesX × TotalBoxesY 的顺序需要实测确认
        //       这里假设 TotalBoxesX=53, TotalBoxesY=4（与 ggpk 一致）
        {143,    "Inventory_143",              53, 4,  212, false, "匹配 CurrencyStash 规格"},

        // 装备槽位/非仓库面板（14 slots，不匹配任何 ggpk 规格）
        {122,    "Inventory_122",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {123,    "Inventory_123",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {124,    "Inventory_124",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {125,    "Inventory_125",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {126,    "Inventory_126",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {127,    "Inventory_127",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {128,    "Inventory_128",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {129,    "Inventory_129",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},
        {130,    "Inventory_130",               7, 2,   14, true,  "14 slots 不匹配 ggpk"},

        // 负数 ID 的特殊槽位（即使 12×12 匹配 NormalStash，也要过滤）
        {-2147483648, "Inventory_-2147483648", 12, 12, 144, true,  "负数 InventoryId"},
        {-2147483647, "Inventory_-2147483647", 12, 12, 144, true,  "负数 InventoryId"},
        {-2147483646, "Inventory_-2147483646", 12, 12, 144, true,  "负数 InventoryId"},
        {-2147483645, "Inventory_-2147483645", 12, 12, 144, true,  "负数 InventoryId"},
        {-2147483644, "Inventory_-2147483644", 12, 12, 144, true,  "负数 InventoryId"},
        {-2147483643, "Inventory_-2147483643", 12, 12, 144, true,  "负数 InventoryId"},

        // 奇怪尺寸（不匹配 ggpk）
        {131,    "Inventory_131",              36, 36, 1296, true, "1296 slots 不匹配 ggpk"},
        {136,    "Inventory_136",              14, 9,  126, true,  "126 slots 不匹配 ggpk"},
        {137,    "Inventory_137",              37, 10, 370, true,  "370 slots 不匹配 ggpk"},

        // 可能匹配 ggpk 的（需要根据真实尺寸判断）
        {139,    "Inventory_139",              12, 12, 144, false, "12x12 匹配 NormalStash"},
        {140,    "Inventory_140",              60, 1,  60, false,  "60x1 匹配 DeliriumStash"},

        // 小尺寸（不匹配 ggpk）
        {141,    "Inventory_141",              12, 1,  12, true,  "12x1 不匹配 ggpk"},
        // Map1 已加入 IsEquipmentSlotName，会被过滤
        {14,     "Map1",                        2, 2,   4, true,  "Map1 是装备槽位"},
        {120,    "Inventory_120",               2, 2,   4, true,  "4 slots 不匹配 ggpk"},
        {121,    "Inventory_121",               2, 2,   4, true,  "4 slots 不匹配 ggpk"},
        {132,    "Inventory_132",               2, 3,   6, true,  "6 slots 不匹配 ggpk"},
        {133,    "Inventory_133",               2, 3,   6, true,  "6 slots 不匹配 ggpk"},
        {134,    "Inventory_134",               2, 3,   6, true,  "6 slots 不匹配 ggpk"},
        {135,    "Inventory_135",               2, 3,   6, true,  "6 slots 不匹配 ggpk"},
        {138,    "Inventory_138",               5, 1,   5, true,  "5 slots 不匹配 ggpk"},
        {142,    "Inventory_142",               1, 1,   1, true,  "1 slot 不匹配 ggpk"},
    };

    int passCount = 0;
    int failCount = 0;
    int filteredCount = 0;
    int keptCount = 0;

    printf("  %-12s %-28s %-10s %-10s %-8s %-8s %s\n",
           "invId", "name", "size", "slots", "期望", "实际", "结果");
    printf("  %-12s %-28s %-10s %-10s %-8s %-8s %s\n",
           "----", "----", "----", "----", "----", "----", "----");

    for (const auto& inv : realInvs) {
        bool actual = TestImpl::IsNonStashInventory(inv.name, inv.width, inv.height, inv.inventoryId);
        bool ok = (actual == inv.expectFiltered);
        const char* expectStr = inv.expectFiltered ? "过滤" : "保留";
        const char* actualStr = actual ? "过滤" : "保留";

        printf("  %-12d %-28s %dx%-7d %-10d %-8s %-8s %s\n",
               inv.inventoryId, inv.name.c_str(),
               inv.width, inv.height, inv.slots,
               expectStr, actualStr,
               ok ? "PASS" : "FAIL");

        if (ok) {
            ++passCount;
            if (actual) ++filteredCount; else ++keptCount;
        } else {
            ++failCount;
            printf("    -> 期望: %s（%s）\n",
                   inv.expectFiltered ? "过滤" : "保留",
                   inv.reason.c_str());
        }
    }

    printf("\n  结果: %d PASS, %d FAIL\n", passCount, failCount);
    printf("  保留: %d 个, 过滤: %d 个\n\n", keptCount, filteredCount);

    // ---- 测试3: 角色装备槽位（名称匹配）----
    printf("--- 测试3: 角色装备槽位名称过滤 ---\n");
    {
        const char* equipSlots[] = {
            "Weapon1", "Weapon2", "BodyArmour1", "Helm1", "Gloves1", "Boots1",
            "Offhand1", "Offhand2", "Ring1", "Ring2", "Amulet1", "Belt1",
            "Flask1", "Cursor1", "MobileSkillGemCrafting1",
            "HeistNpcEquipment1", "MercenaryCompanionBodyArmour1",
            "DONOTUSE1", "StrMasterCrafting", "Map1",
        };
        bool pass = true;
        for (const char* name : equipSlots) {
            bool filtered = TestImpl::IsNonStashInventory(name, 1, 1, 999);
            printf("  [%s] %-35s -> %s\n",
                   filtered ? "PASS" : "FAIL",
                   name,
                   filtered ? "过滤" : "保留");
            if (!filtered) pass = false;
        }
        printf("  结果: %s\n\n", pass ? "ALL PASS" : "FAILED");
    }

    // ---- 测试4: 已知仓库类型名保留 ----
    printf("--- 测试4: 已知仓库类型名保留 ---\n");
    {
        const char* stashNames[] = {
            "NormalStash", "PremiumStash", "CurrencyStash", "UniqueStash",
            "MapStash", "QuadStash", "EssenceStash", "FragmentStash",
            "DelveStash", "BlightStash", "FlaskStash", "GemStash",
            "SocketableStash", "ExpeditionStash", "RitualStash",
            "BreachStash", "AbyssStash", "RelicStash",
        };
        bool pass = true;
        for (const char* name : stashNames) {
            bool filtered = TestImpl::IsNonStashInventory(name, 12, 12, 200);
            printf("  [%s] %-35s -> %s\n",
                   !filtered ? "PASS" : "FAIL",
                   name,
                   filtered ? "过滤" : "保留");
            if (filtered) pass = false;
        }
        printf("  结果: %s\n\n", pass ? "ALL PASS" : "FAILED");
    }

    // ---- 测试5: 主背包保留 ----
    printf("--- 测试5: 主背包（MainInventory*）保留 ---\n");
    {
        const char* mainInvNames[] = {
            "MainInventory1", "MainInventory", "MainInventory2",
        };
        bool pass = true;
        for (const char* name : mainInvNames) {
            bool filtered = TestImpl::IsNonStashInventory(name, 12, 12, 1);
            printf("  [%s] %-35s -> %s\n",
                   !filtered ? "PASS" : "FAIL",
                   name,
                   filtered ? "过滤" : "保留");
            if (filtered) pass = false;
        }
        printf("  结果: %s\n\n", pass ? "ALL PASS" : "FAILED");
    }

    // ---- 总结 ----
    printf("=== 总结 ===\n");
    printf("  测试2（真实数据）: %d PASS, %d FAIL\n", passCount, failCount);
    printf("  过滤后保留: %d 个仓库Tab\n", keptCount);
    printf("  过滤掉: %d 个非仓库Tab\n", filteredCount);
    printf("\n");

    // 关键断言：过滤后应该只保留少量真正的仓库Tab
    // bug1.log 中 31 页应被过滤到 <= 5 页
    bool finalPass = (failCount == 0 && keptCount <= 5);
    printf("  [%s] 过滤后保留 %d 页（期望 <= 5），失败 %d 项\n",
           finalPass ? "PASS" : "FAIL",
           keptCount, failCount);

    return finalPass ? 0 : 1;
}
