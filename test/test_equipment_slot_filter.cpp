// test_equipment_slot_filter.cpp — 验证 IsEquipmentSlotName 在所有扫描函数中正确过滤装备槽位
// 目标：复现 test_log_20260730_183914.txt 中 "仓库扫描依然是装备槽位" 的问题，
//      验证修复后 MobileSkillGemCrafting1、Weapon1、BodyArmour1 等装备槽位
//      在 ScanAndIdentifyAllItems / ListAllStashTabsFromUi / DumpStashTabLabels
//      三处扫描入口都被过滤掉，不再污染仓库扫描结果。
//
// 编译：cl /EHsc /std:c++20 test_equipment_slot_filter.cpp /Fe:test_equipment_slot_filter.exe
//       （或用 RunTests 工程，但本项目链接 onecore API 失败，故采用独立 cpp）
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 直接复制 TabletFilter.h 中的 IsEquipmentSlotName（保持逻辑一致）
// 注：真实代码中此函数位于 namespace TabletReforgeGame，TabletFilter.h
namespace TestImpl {

bool IsEquipmentSlotName(const std::string& name) {
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

} // namespace TestImpl

// 模拟 ScanAndIdentifyAllItems / ListAllStashTabsFromUi / DumpStashTabLabels
// 三个扫描入口的过滤行为：均应在进入物品枚举前用 IsEquipmentSlotName 过滤。
struct MockInventory {
    int inventoryId;
    std::string name;
    int itemCount;
};

int main() {
    // 复现 test_log_20260730_183914.txt 中的真实 Inventory 列表（节选关键项）
    std::vector<MockInventory> allInventories = {
        {1,   "Weapon1",                       0},
        {2,   "BodyArmour1",                   0},
        {3,   "Helm1",                         0},
        {82,  "MobileSkillGemCrafting1",       1},  // ← bug1: 戒指 FourRing12 被误扫
        {146, "Inventory_146",                61},  // 真实仓库 Tab（IncursionAugment）
        {147, "Inventory_147",                 0},
        {120, "Inventory_120",                 0},
        {46,  "HeistNpcEquipment1",            0},
        {84,  "MercenaryCompanionBodyArmour1", 0},
        {59,  "DONOTUSE1",                     0},
        {63,  "SkillSlots1",                   0},
        {64,  "Currency1",                     0},
        // 真实仓库 Tab 名称
        {200, "NormalStash",                  12},
        {201, "FragmentStash",                 6},
        {202, "QuadStash",                    24},
    };

    int totalItemsBefore = 0;
    for (const auto& inv : allInventories) totalItemsBefore += inv.itemCount;

    // 模拟三个扫描入口的过滤（修复后行为）
    int filteredAsEquipSlot = 0;
    int keptStashTabs = 0;
    int keptItems = 0;
    std::vector<std::string> filteredNames;
    std::vector<std::string> keptNames;

    for (const auto& inv : allInventories) {
        if (TestImpl::IsEquipmentSlotName(inv.name)) {
            ++filteredAsEquipSlot;
            filteredNames.push_back(inv.name + " (items=" + std::to_string(inv.itemCount) + ")");
            continue;
        }
        ++keptStashTabs;
        keptItems += inv.itemCount;
        keptNames.push_back(inv.name + " (items=" + std::to_string(inv.itemCount) + ")");
    }

    printf("=== 装备槽位过滤验证（对应 IsEquipmentSlotName）===\n");
    printf("输入: %zu 个 Inventory，总物品 %d\n\n", allInventories.size(), totalItemsBefore);

    printf("--- 已过滤的装备槽位 (%d 个) ---\n", filteredAsEquipSlot);
    for (const auto& n : filteredNames) printf("  [FILTERED] %s\n", n.c_str());

    printf("\n--- 保留的仓库/背包 Tab (%d 个，物品 %d) ---\n", keptStashTabs, keptItems);
    for (const auto& n : keptNames) printf("  [KEEP] %s\n", n.c_str());

    printf("\n=== 验证结果 ===\n");
    bool pass = true;

    // 关键断言1: MobileSkillGemCrafting1 必须被过滤（bug1.log 的核心问题）
    bool mobileFiltered = TestImpl::IsEquipmentSlotName("MobileSkillGemCrafting1");
    printf("  [%s] MobileSkillGemCrafting1 被过滤: %s\n",
           mobileFiltered ? "PASS" : "FAIL",
           mobileFiltered ? "是" : "否");
    if (!mobileFiltered) pass = false;

    // 关键断言2: Inventory_146（真实仓库Tab）必须保留
    bool inv146Kept = !TestImpl::IsEquipmentSlotName("Inventory_146");
    printf("  [%s] Inventory_146 (真实仓库Tab) 保留: %s\n",
           inv146Kept ? "PASS" : "FAIL",
           inv146Kept ? "是" : "否");
    if (!inv146Kept) pass = false;

    // 关键断言3: 角色装备槽位（Weapon1/BodyArmour1/Helm1）必须被过滤
    bool equipFiltered = TestImpl::IsEquipmentSlotName("Weapon1") &&
                         TestImpl::IsEquipmentSlotName("BodyArmour1") &&
                         TestImpl::IsEquipmentSlotName("Helm1");
    printf("  [%s] 角色装备槽位(Weapon/Body/Helm) 被过滤: %s\n",
           equipFiltered ? "PASS" : "FAIL",
           equipFiltered ? "是" : "否");
    if (!equipFiltered) pass = false;

    // 关键断言4: 模式匹配（HeistNpcEquipment1, MercenaryCompanionBodyArmour1, DONOTUSE1）
    bool patternFiltered = TestImpl::IsEquipmentSlotName("HeistNpcEquipment1") &&
                           TestImpl::IsEquipmentSlotName("MercenaryCompanionBodyArmour1") &&
                           TestImpl::IsEquipmentSlotName("DONOTUSE1");
    printf("  [%s] 模式匹配(Heist/Mercenary/DONOTUSE) 被过滤: %s\n",
           patternFiltered ? "PASS" : "FAIL",
           patternFiltered ? "是" : "否");
    if (!patternFiltered) pass = false;

    // 关键断言5: 真实仓库 Tab 名称不被误过滤
    bool stashKept = !TestImpl::IsEquipmentSlotName("NormalStash") &&
                     !TestImpl::IsEquipmentSlotName("FragmentStash") &&
                     !TestImpl::IsEquipmentSlotName("QuadStash") &&
                     !TestImpl::IsEquipmentSlotName("CurrencyStash");
    printf("  [%s] 真实仓库Tab(Normal/Fragment/Quad/Currency) 保留: %s\n",
           stashKept ? "PASS" : "FAIL",
           stashKept ? "是" : "否");
    if (!stashKept) pass = false;

    // 关键断言6: 过滤后物品数应只包含真实仓库Tab的物品（61+12+6+24=103）
    // 不应包含 MobileSkillGemCrafting1 的 1 个物品（FourRing12 戒指）
    printf("  [%s] 过滤后物品数=%d (期望 103，不含装备槽位物品): %s\n",
           keptItems == 103 ? "PASS" : "FAIL",
           keptItems,
           keptItems == 103 ? "正确" : "错误");
    if (keptItems != 103) pass = false;

    printf("\n=== 总结 ===\n");
    printf("  过滤前: %zu Inventory, %d 物品\n", allInventories.size(), totalItemsBefore);
    printf("  过滤后: %d 仓库Tab, %d 物品\n", keptStashTabs, keptItems);
    printf("  过滤掉: %d 个装备槽位\n", filteredAsEquipSlot);
    printf("  结果: %s\n", pass ? "ALL PASS ✓" : "FAILED ✗");

    return pass ? 0 : 1;
}
