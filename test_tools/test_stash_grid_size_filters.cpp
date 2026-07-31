// test_stash_grid_size_filters.cpp - 独立mock测试：验证仓库Tab格子尺寸过滤逻辑
// 不依赖PluginSDK或游戏进程，直接测试StashTypeTable.h和过滤函数
//
// Build (PowerShell/VS2022 x64 Native Tools prompt:
//   cl /EHsc /std:c++17 test_stash_grid_size_filters.cpp /I..\Plugins\TabletReforgeAutomation\game
//
#include "StashTypeTable.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// Mock版本的 IsEquipmentSlotName / IsNonStashInventory (不含SDK部分)
// ============================================================================
inline bool IsEquipmentSlotName(const std::string& name) {
    // 精确匹配的装备槽位名称（基于inventories.json + 历史过滤）
    static const char* kExact[] = {
        "MainInventory1","BodyArmour1","Weapon1","Offhand1","Helm1","Amulet1","Ring1",
        "Ring2","Gloves1","Boots1","Belt1","Flask1","Cursor1","Map1","Weapon2","Offhand2",
        "Weapon3","Offhand3","Trinket1","GuildTag1","StashInventoryId","SkillSlots1",
        "PassiveJewels1","AnimatedArmour1","Leaguestone1","Currency1","MapCurrency1",
        "MobileHeldMapsInventory1","Relics1","RelicStorage1","SanctumSpecialRelic1",
        "CurrentSanctumRun1","LakeTabletInventory1","MemoryLineMaps","SentinelDroneInventory1",
        "SentinelStorage1","AtlasUpgrades1","AtlasUpgradesStorage","DefaultAttackSkills1",
        "AscendancySkills1","Tower1","ExpandedInventory1","UltimatumKey1","UltimatumKeySacrifice1",
        "DONOTUSE1","DONOTUSE2","DONOTUSE3","DONOTUSE4","DONOTUSE5","DONOTUSE6","DONOTUSE7",
        "ThreeToOneOutput","ThreeToOneInput","UNUSED1","UNUSED2",
        // Heist系列
        "HeistBlueprintMission","HeistContractMission","HeistNpcEquipment1","HeistNpcEquipment2",
        "HeistNpcEquipment3","HeistNpcEquipment4","HeistNpcEquipment5","HeistNpcEquipment6",
        "HeistNpcEquipment7","HeistNpcEquipment8","HeistNpcEquipment9","HeistStorage1",
        // 佣兵系列
        "MercenaryCompanionBodyArmour1","MercenaryCompanionHelm1",
        "MercenaryCompanionGloves1","MercenaryCompanionBoots1",
        "MercenaryCompanionWeapon1","MercenaryCompanionOffhand1",
        // 各种Crafting系列
        "StrMasterCrafting","StrDexMasterCrafting","DexMasterCrafting","DexIntMasterCrafting",
        "IntMasterCrafting","StrIntMasterCrafting","PVPMasterCrafting",
        "DivinationCardTrade","Darkshrine","TalismanTrade","BestiaryCrafting",
        "IncursionSacrifice","BetrayalUnveiling","ItemSynthesisInput",
        "ItemSynthesisOutput","BlightCraftingInput","BlightCraftingItem",
        "HarvestCraftingItem","UltimatumCraftingItem","DelveCraftingItem",
        "MobileMapInventory1","HellscapeModificationInventory1","MobileSkillGemCrafting1",
        "RitualSavedRewards1","ExpeditionMapMission","ExpeditionDeal1",
        "ExpeditionDeal2","ExpeditionDeal3",
        nullptr
    };
    for (int i = 0; kExact[i]; ++i) if (name == kExact[i]) return true;
    // 前缀模式：HeistStorage N, AnimatedArmour N, 等N后缀
    if (name.size() > 5 && (
        (name.rfind("HeistNpcEquipment", 0) == 0
            && name.size() > 17)) return true;
    if (name.size() > 7 && name.rfind("Inventory_", 0) == 0) {
        // Inventory_NNN 类型：不在这里判断(交给后续尺寸判断；只有Weapon NNN等不装备槽位模式
        // 实际装备槽位不会走上面的精确匹配
    }
    return false;
}

// 简化版：只依赖尺寸判断（用于直接测试ggpk修正效果）
struct TestCase {
    const char* name; int w; int h; bool expect_stash; // true=是仓库Tab(保留), false=非仓库(过滤掉)
};

int main() {
    using namespace TabletReforgeGame;
    int pass = 0, fail = 0;

    printf("===== 测试1: StashTypeTable 精确匹配ggpk规格 (FindStashTypeByGridSize)\n");
    printf("使用stashtype.json最新值共18种，StashTypeEntry共25种\n\n");

    // ---- Case A. ggpk stashtype.json中的所有已知类型（应该都能被精确匹配
    TestCase cases_ggpk[] = {
        {"NormalStash",          12,  12, true},
        {"PremiumStash",         12,  12, true},
        {"TradeStash",           12,  12, true},
        {"CurrencyStash",        41,   4, true},   // v3修正: 旧值53x4
        {"UniqueStash",          94,   4, true},   // v3修正: 旧值146x4
        {"MapStash",             12,   6, true},   // v3修正: 旧值12x8
        {"QuadStash",            24,  24, true},
        {"EssenceStash",         30,   4, true},   // v3修正: 旧值88x4
        {"FragmentStash",       152,   3, true},   // v3修正: 旧值18x6
        {"PCBangPremiumStash",   12,  12, true},
        {"PCBangEssenceStash",   30,   4, true},   // v3修正
        {"DelveStash",          41,   4, true},
        {"BlightStash",         66,   4, true},
        {"DeliriumStash",       40,   1, true},
        {"FlaskStash",         250,   4, true},
        {"GemStash",           250,   2, true},
        // 扩展类型（不在18条中，但历史收录25条）
        {"SocketableStash",     214,   1, true},
        {"ExpeditionStash",      24,   4, true},
        {"RitualStash",          42,   1, true},
        {"BreachStash",          13,   5, true},
        {"AbyssStash",           12,   3, true},
        {"RelicStash",           12,  12, true},
        {nullptr, 0, 0, false}
    };
    for (int i = 0; cases_ggpk[i].name; ++i) {
        const TestCase& c = cases_ggpk[i];
        const StashTypeEntry* hit = FindStashTypeByGridSize(c.w, c.h);
        bool ok = (hit != nullptr) == c.expect_stash;
        printf("  [%s] %s (%dx%d) -> %s  匹配=%s\n",
            ok ? "PASS" : "FAIL",
            c.name, c.w, c.h,
            hit ? hit->id : "(null)",
            c.expect_stash ? "true" : "false");
        if (ok) ++pass; else ++fail;
    }

    printf("\n===== 测试2: 装备槽位尺寸（应该被精确匹配过滤掉，但IsLikelyStashTabByGridSize==false或IsNonStashInventory）\n");
    TestCase cases_eq[] = {
        // 装备槽位（来自inventories.json中的标准尺寸
        {"BodyArmour1",          3, 6, false},   // Unk005=3 Unk007=6
        {"Weapon1",                4, 0, false},  // 武器槽通常宽x高不会太大的典型
        {"Weapon1",                4, 1, false},
        {"Helm1",                  2, 8, false},
        {"Amulet1",                1, 15, false},
        {"Ring1",                  1, 12, false},
        {"Gloves1",                2, 9, false},
        {"Boots1",                 2, 10, false},
        {"Belt1",                  1, 14, false},
        {"Flask1",                 2, 24, false},
        {"Map1",                   2, 24, false},
        {"PassiveJewels1",         57, 1, false},   // 1行长条 (height=1 width>20过滤
        {"Leaguestone1",           1, 1, false},
        {"Trinket1",               1, 1, false},
        {"GuildTag1",              1, 1, false},
        {"SkillSlots1",             1, 1, false},
        {"MercenaryCompanionHelm1",    2, 2, false},
        {nullptr, 0, 0, false}
    };
    for (int i = 0; cases_eq[i].name; ++i) {
        TestCase& c = cases_eq[i];
        bool eq = IsEquipmentSlotName(c.name);
        // is_filtered = 通过 IsEquipmentSlotName 过滤 OR 通过尺寸过滤(尺寸不匹配仓库Tab)
        bool size_blocked = !IsLikelyStashTabByGridSize(c.w, c.h);
        bool is_filtered = eq || size_blocked;
        bool expected_filtered = !c.expect_stash;  // expect_stash=false → 应过滤
        bool pass2 = is_filtered == expected_filtered;
        const StashTypeEntry* hit = FindStashTypeByGridSize(c.w, c.h);
        printf("  [%s] %s (%dx%d) eq=%d sizeBlocked=%d ggpkMatch=%s  (filtered=%s expected=%s)\n",
            pass2 ? "PASS" : "FAIL",
            c.name, c.w, c.h, eq? 1 : 0, size_blocked ? 1 : 0,
            hit ? hit->id : "(null)",
            is_filtered ? "YES" : "NO",
            expected_filtered ? "YES" : "NO");
        if (pass2) ++pass; else { ++fail; }
    }

    printf("\n===== 测试3: 启发式大尺寸匹配 (IsHeuristicLargeStashTab / IsLikelyStashTabByGridSize)\n");
    TestCase cases_big[] = {
        // bug1.log中的真实仓库超大尺寸（应该通过启发式通过
        {"bug_107x12超大",  107, 12, true},
        {"bug_14x9",       14, 9, true},
        {"bug_37x10",        37, 10, true},
        // AbyssStash最低阈值 12x3=36
        {"Abyss尺寸",       12, 3, true},
        // 极小尺寸（应被启发式过滤，3x2=6<36, height<4 width<10 total<20
        {"特小_3x2",          3, 2, false},
        {"特小_4x1",          4, 1, false},
        {"PassiveJewels57x1长条", 57, 1, false},
        {nullptr, 0, 0, false}
    };
    for (int i = 0; cases_big[i].name; ++i) {
        TestCase& c = cases_big[i];
        bool hit_stash = IsLikelyStashTabByGridSize(c.w, c.h);
        bool ok = hit_stash == c.expect_stash;
        printf("  [%s] %s (%dx%d) IsLikely=%s\n",
            ok ? "PASS" : "FAIL",
            c.name, c.w, c.h,
            hit_stash ? "=true(=stash" : "=false(=non-stash");
        if (ok) ++pass; else ++fail;
    }

    printf("\n===== 总结: PASS=%d FAIL=%d 总用例数=%d\n", pass, fail, pass+fail);
    return fail == 0 ? 0 : 1;
}
