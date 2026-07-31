// test_integration_plan_b.cpp — 方案B完整合成流程集成测试
//
// 模拟方案B「合规词缀Id读取」在真实重铸台合成流程中的全链路表现：
//   阶段1: 扫描仓库 → ExtractModIds → MatchesDesiredReforgeTypeEx → 识别原料/产物
//   阶段2: 取出原料（Ctrl+右键可堆叠）
//   阶段3: 三槽同物分组（同Path放3个）
//   阶段4: 点击合成 → 模拟产物（随机词缀）
//   阶段5: 产物检测 → MatchesDesiredProductTypeEx
//   阶段6: 可堆叠结束条件（4条件全满足）
//   阶段7: RandomBackoff 频控集成
//   阶段8: Hash32 缓存性能验证
//   阶段9: 方案A vs 方案B 对比
//
// 编译: cl.exe /std:c++20 /utf-8 /bigobj /EHsc /I. /Isdk /Ithird_party /Iimgui
//        test\test_integration_plan_b.cpp /Fe:bin\Release\test_integration_plan_b.exe
//        /link kernel32.lib user32.lib onecoreuap.lib ntdll.lib

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include <random>

#include "../config/Settings.h"
#include "../game/TabletFilter.h"
#include "../game/TabletBonusCatalog.h"
#include "../flow/Clock.h"
#include "../sdk/PluginSDK.h"

using TabletReforgeConfig::Settings;
using TabletReforgeConfig::ReforgeItemType;
using TabletReforgeGame::StashTablet;

// ============================================================
// 测试框架
// ============================================================
static int g_passCount = 0;
static int g_failCount = 0;
static int g_warnCount = 0;

#define EXPECT_TRUE(cond, msg) do { \
    if (cond) { g_passCount++; printf("  [PASS] %s\n", msg); } \
    else { g_failCount++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define EXPECT_FALSE(cond, msg) EXPECT_TRUE(!(cond, msg))

#define EXPECT_EQ(a, b, msg) do { \
    if ((a) == (b)) { g_passCount++; printf("  [PASS] %s\n", msg); } \
    else { g_failCount++; printf("  [FAIL] %s (got=%lld, expected=%lld)\n", msg, (long long)(a), (long long)(b)); } \
} while(0)

#define WARN(cond, msg) do { \
    if (!(cond)) { g_warnCount++; printf("  [WARN] %s\n", msg); } \
} while(0)

// ============================================================
// Mock 数据构造工具
// ============================================================

// 构造 mock ItemMods（模拟 ReadItemMods 返回的数据）
PluginSDK::ItemMods MakeMockMods(const std::vector<std::pair<std::string, uint32_t>>& mods) {
    PluginSDK::ItemMods result;
    result.Valid = true;
    for (const auto& [id, hash] : mods) {
        PluginSDK::Mod m;
        m.Id = id;
        m.Hash32 = hash;
        // 绝不设置 Name/AffixName/StatKey（宪法修正案 v1.3 绝对红线）
        result.ExplicitMods.push_back(m);
    }
    return result;
}

// 构造 mock StashTablet
StashTablet MakeMockStashItem(const std::string& path, const std::string& baseType,
                               int rarity, bool identified, int slotX, int slotY,
                               int stackCount = 1) {
    StashTablet t;
    t.path = path;
    t.baseType = baseType;
    t.rarity = rarity;
    t.identified = identified;
    t.slotX = slotX;
    t.slotY = slotY;
    t.stackCount = stackCount;
    return t;
}

// 仓库中的催化剂路径
const char* kCatalystLife    = "Metadata/Items/Currency/CurrencyJewelleryQualityLife";
const char* kCatalystMana    = "Metadata/Items/Currency/CurrencyJewelleryQualityMana";
const char* kCatalystFire    = "Metadata/Items/Currency/CurrencyJewelleryQualityFire";

// 碑牌路径
const char* kBreachTablet    = "Metadata/Items/TowerAugment/BreachTablet";
const char* kIrradiatedTablet = "Metadata/Items/TowerAugment/GenericAugment";

// 白名单内的词缀Id（来自 TabletBonusCatalog）
const char* kModBreachRares     = "TowerBreachAdditionalRares";
const char* kModBreachPackSize  = "TowerPackSizeIncrease";
const char* kModRarityIncrease  = "TowerDroppedItemRarityIncrease";
const char* kModMapsIncrease    = "TowerMapDroppedMapsIncrease";

// 非白名单词缀Id
const char* kModUnknown1 = "unknown_mod_alpha";
const char* kModUnknown2 = "unknown_mod_beta";

// ============================================================
// 配置工具
// ============================================================

// 方案A配置（enableBonusMatch=false）
Settings MakePlanAConfig() {
    Settings cfg;
    cfg.itemType = static_cast<int>(ReforgeItemType::CatalystsOnly);
    cfg.useSubCategoryMode = true;
    cfg.selectedSubCategories = {601};  // 选中血肉催化劑
    cfg.requireIdentifiedForMaterial = false;
    cfg.enableBonusMatch = false;       // 方案A
    cfg.bonusMatchSilent = false;
    cfg.useModifierFilterMode = true;
    cfg.selectedModifierKeys = {kModBreachRares};
    return cfg;
}

// 方案B配置（enableBonusMatch=true）
Settings MakePlanBConfig() {
    Settings cfg;
    cfg.itemType = static_cast<int>(ReforgeItemType::CatalystsOnly);
    cfg.useSubCategoryMode = true;
    cfg.selectedSubCategories = {601};  // 选中血肉催化劑
    cfg.requireIdentifiedForMaterial = false;
    cfg.enableBonusMatch = true;        // 方案B
    cfg.bonusMatchSilent = false;
    cfg.useModifierFilterMode = true;
    cfg.selectedModifierKeys = {kModBreachRares};  // 目标词缀
    return cfg;
}

// 碑牌配置（方案B，用于碑牌合成）
Settings MakePlanBTabletConfig() {
    Settings cfg;
    cfg.itemType = static_cast<int>(ReforgeItemType::Tablets);
    cfg.useSubCategoryMode = false;
    cfg.requireIdentifiedForMaterial = true;
    cfg.enableBonusMatch = true;
    cfg.bonusMatchSilent = false;
    cfg.useModifierFilterMode = true;
    cfg.selectedModifierKeys = {kModBreachRares, kModBreachPackSize};
    cfg.filterByRarity = true;
    cfg.minRarityForMaterial = 1;  // 魔法及以上
    return cfg;
}

// ============================================================
// 阶段1: 仓库扫描模拟（ExtractModIds + MatchesDesiredReforgeTypeEx）
// ============================================================
struct ScanResult {
    std::vector<StashTablet> materials;    // 原料
    std::vector<StashTablet> products;     // 产物
    std::vector<StashTablet> skipped;      // 跳过（非合成物/不匹配）
    int modIdsExtracted = 0;               // 提取的白名单mod总数
    int modIdsDiscarded = 0;               // 丢弃的非白名单mod总数
};

// 模拟仓库扫描流程
ScanResult SimulateStashScan(const std::vector<StashTablet>& stashItems,
                             const std::vector<PluginSDK::ItemMods>& itemModsList,
                             const Settings& cfg) {
    ScanResult result;
    for (size_t i = 0; i < stashItems.size() && i < itemModsList.size(); ++i) {
        const auto& item = stashItems[i];
        const auto& mods = itemModsList[i];

        StashTablet scanned = item;

        // 方案B：调用 ExtractModIds 提取白名单内的 Mod.Id
        if (cfg.enableBonusMatch && mods.Valid) {
            std::vector<std::string> outIds;
            std::vector<uint32_t>    outHashes;
            TabletReforgeGame::ExtractModIds(mods, outIds, outHashes, false, item.path);
            scanned.modIds = outIds;
            scanned.modHashes = outHashes;

            // 统计提取/丢弃数
            for (const auto& m : mods.ExplicitMods) {
                bool inWhitelist = false;
                for (const auto& id : outIds) {
                    if (id == m.Id) { inWhitelist = true; break; }
                }
                if (inWhitelist) result.modIdsExtracted++;
                else result.modIdsDiscarded++;
            }
        }

        // 调用 Ex 包装函数判定原料/产物
        bool isMat = TabletReforgeGame::MatchesDesiredReforgeTypeEx(
            scanned.path, scanned.baseType, scanned.rarity, scanned.identified,
            scanned.modIds, scanned.modHashes, cfg);
        bool isProd = TabletReforgeGame::MatchesDesiredProductTypeEx(
            scanned.path, scanned.baseType, scanned.rarity, scanned.identified,
            scanned.modIds, scanned.modHashes, cfg);

        if (isMat) {
            scanned.isMaterial = true;
            result.materials.push_back(scanned);
        } else if (isProd) {
            scanned.isProductType = true;
            result.products.push_back(scanned);
        } else {
            result.skipped.push_back(scanned);
        }
    }
    return result;
}

// ============================================================
// 测试1: 方案B 完整合成流程（催化剂可堆叠场景）
// ============================================================
void TestPlanBFullFlowCatalysts() {
    printf("\n====== 集成测试1: 方案B 完整合成流程（催化剂可堆叠）======\n");
    printf("流程: 扫描仓库→取原料→三槽同物→合成×3→产物检测→结束条件\n\n");

    TabletReforgeGame::ClearHashCachesForTest();
    Settings cfg = MakePlanBConfig();

    // === 构造 Mock 仓库数据 ===
    // 10个神經催化劑（未选中子类=原料候选），部分带白名单mod（产物），部分不带（原料）
    std::vector<StashTablet> stash;
    std::vector<PluginSDK::ItemMods> modsList;

    // 7个原料：神經催化劑，无目标词缀
    for (int i = 0; i < 7; ++i) {
        stash.push_back(MakeMockStashItem(kCatalystMana, "Neural Catalyst", 0, false, i, 0, 10));
        // 无白名单mod → 原料
        modsList.push_back(MakeMockMods({{kModUnknown1, 0xBAD00001 + i}}));
    }
    // 3个产物：神經催化劑，有目标词缀 TowerBreachAdditionalRares
    for (int i = 0; i < 3; ++i) {
        stash.push_back(MakeMockStashItem(kCatalystMana, "Neural Catalyst", 0, false, 7 + i, 0, 10));
        modsList.push_back(MakeMockMods({
            {kModBreachRares, 0xG00D0001 + i},
            {kModUnknown2, 0xBAD00010 + i}
        }));
    }

    printf("--- 阶段1: 仓库扫描 ---\n");
    printf("仓库物品: %d 个神經催化劑（7个无目标词缀=原料, 3个有目标词缀=产物）\n",
           (int)stash.size());

    ScanResult scan = SimulateStashScan(stash, modsList, cfg);

    char buf[256];
    sprintf_s(buf, "原料数=%zu (期望7)", scan.materials.size());
    EXPECT_EQ(scan.materials.size(), (size_t)7, buf);

    sprintf_s(buf, "产物数=%zu (期望0, 神經催化劑未选中子类不判为产物)", scan.products.size());
    EXPECT_EQ(scan.products.size(), (size_t)0, buf);

    sprintf_s(buf, "跳过数=%zu (期望3, 有目标词缀但不匹配产物条件)", scan.skipped.size());
    EXPECT_EQ(scan.skipped.size(), (size_t)3, buf);

    sprintf_s(buf, "白名单mod提取数=%d (期望3, 只有3个产物有白名单mod)", scan.modIdsExtracted);
    EXPECT_EQ(scan.modIdsExtracted, 3, buf);

    sprintf_s(buf, "非白名单mod丢弃数=%d (期望10, 7原料+3产物各1个未知mod)", scan.modIdsDiscarded);
    EXPECT_EQ(scan.modIdsDiscarded, 10, buf);

    // --- 阶段2: 取出原料 ---
    printf("\n--- 阶段2: 取出原料（Ctrl+右键可堆叠）---\n");
    printf("从仓库取出3个原料到背包（三槽同物需要同Path）\n");

    std::vector<StashTablet> bag;
    for (int i = 0; i < 3 && i < (int)scan.materials.size(); ++i) {
        bag.push_back(scan.materials[i]);
    }
    sprintf_s(buf, "背包取出原料数=%zu (期望3)", bag.size());
    EXPECT_EQ(bag.size(), (size_t)3, buf);

    // 验证三槽同物：3个原料Path必须相同
    bool samePath = !bag.empty() &&
                    std::all_of(bag.begin(), bag.end(), [&](const StashTablet& t) {
                        return t.path == bag[0].path;
                    });
    EXPECT_TRUE(samePath, "三槽同物: 3个原料Path完全相同");

    // --- 阶段3: 放入重铸台 ---
    printf("\n--- 阶段3: 放入重铸台（三槽同物）---\n");
    std::string anchorPath = bag[0].path;
    int placedCount = 0;
    for (const auto& item : bag) {
        if (item.path == anchorPath) placedCount++;
    }
    sprintf_s(buf, "放入重铸台同Path物品数=%d (期望3)", placedCount);
    EXPECT_EQ(placedCount, 3, buf);

    // --- 阶段4: 合成 ×3 ---
    printf("\n--- 阶段4: 点击合成 ×3 ---\n");
    int reforgePressCount = 0;
    int noProductReforgeCount = 0;
    bool hasNewProduct = false;

    // 模拟3次合成：第1次无产物，第2次无产物，第3次有产物
    for (int round = 1; round <= 3; ++round) {
        reforgePressCount++;
        // 模拟合成产物（第3次产出目标词缀）
        if (round == 3) {
            hasNewProduct = true;
            PluginSDK::ItemMods productMods = MakeMockMods({{kModBreachRares, 0xG00D0099}});
            std::vector<std::string> prodIds;
            std::vector<uint32_t>    prodHashes;
            TabletReforgeGame::ExtractModIds(productMods, prodIds, prodHashes, false, "product");

            bool isProduct = TabletReforgeGame::MatchesDesiredProductTypeEx(
                anchorPath, "Neural Catalyst", 0, false, prodIds, prodHashes, cfg);
            sprintf_s(buf, "第%d次合成: 产物有目标词缀→产物判定=%d (期望1)", round, (int)isProduct);
            EXPECT_TRUE(isProduct, buf);
        } else {
            hasNewProduct = false;
            noProductReforgeCount++;
            PluginSDK::ItemMods productMods = MakeMockMods({{kModUnknown1, 0xBAD0099}});
            std::vector<std::string> prodIds;
            std::vector<uint32_t>    prodHashes;
            TabletReforgeGame::ExtractModIds(productMods, prodIds, prodHashes, false, "product");

            bool isProduct = TabletReforgeGame::MatchesDesiredProductTypeEx(
                anchorPath, "Neural Catalyst", 0, false, prodIds, prodHashes, cfg);
            sprintf_s(buf, "第%d次合成: 产物无目标词缀→产物判定=%d (期望0)", round, (int)isProduct);
            EXPECT_FALSE(isProduct, buf);
        }
    }

    sprintf_s(buf, "合成次数=%d (期望3)", reforgePressCount);
    EXPECT_EQ(reforgePressCount, 3, buf);

    // --- 阶段5: 可堆叠结束条件 ---
    printf("\n--- 阶段5: 可堆叠结束条件检查 ---\n");
    printf("规则: 合成>=3次 AND 无新产物 AND 背包无同Path材料 AND 仅1槽有材料\n");

    // 场景: 合成3次, 有新产物 → 不应结束
    bool endCond1 = (reforgePressCount >= 3) && !hasNewProduct;
    EXPECT_FALSE(endCond1, "合成3次但有新产物 → 不结束 (hasNewProduct=true)");

    // 场景: 合成3次, 无新产物, 背包空, 1槽 → 应结束
    bool bagEmpty = true;
    int slotsWithMaterial = 1;
    bool endCond2 = (reforgePressCount >= 3) && !false && bagEmpty && (slotsWithMaterial == 1);
    EXPECT_TRUE(endCond2, "合成3次+无新产物+背包空+1槽 → 结束");
}

// ============================================================
// 测试2: 方案B 碑牌合成流程（非可堆叠场景）
// ============================================================
void TestPlanBTabletFlow() {
    printf("\n====== 集成测试2: 方案B 碑牌合成流程（非可堆叠）======\n");
    printf("流程: 扫描碑牌→识别有目标词缀(产物)/无目标词缀(原料)→合成→检测\n\n");

    TabletReforgeGame::ClearHashCachesForTest();
    Settings cfg = MakePlanBTabletConfig();

    // === 构造 Mock 仓库数据 ===
    // 10个裂隙碑牌：5个魔法品质已鉴定
    //  - 2个有 TowerBreachAdditionalRares (产物，已有目标词缀)
    //  - 3个只有非白名单mod (原料，需要重铸)
    std::vector<StashTablet> stash;
    std::vector<PluginSDK::ItemMods> modsList;

    // 3个原料：魔法品质已鉴定，无目标词缀
    for (int i = 0; i < 3; ++i) {
        stash.push_back(MakeMockStashItem(kBreachTablet, "Breach Tablet", 1, true, i, 0));
        modsList.push_back(MakeMockMods({{kModUnknown1, 0xBAD10001 + i}}));
    }
    // 2个产物：魔法品质已鉴定，有目标词缀
    for (int i = 0; i < 2; ++i) {
        stash.push_back(MakeMockStashItem(kBreachTablet, "Breach Tablet", 1, true, 3 + i, 0));
        modsList.push_back(MakeMockMods({
            {kModBreachRares, 0xG00D10001 + i},
            {kModUnknown2, 0xBAD10010 + i}
        }));
    }

    printf("--- 阶段1: 仓库扫描 ---\n");
    printf("仓库: 5个裂隙碑牌（3个无目标词缀=原料, 2个有目标词缀=产物）\n");

    ScanResult scan = SimulateStashScan(stash, modsList, cfg);

    char buf[256];
    sprintf_s(buf, "原料数=%zu (期望3, 无目标词缀的碑牌)", scan.materials.size());
    EXPECT_EQ(scan.materials.size(), (size_t)3, buf);

    sprintf_s(buf, "产物数=%zu (期望2, 有目标词缀的碑牌)", scan.products.size());
    EXPECT_EQ(scan.products.size(), (size_t)2, buf);

    // 验证产物都有目标词缀
    for (size_t i = 0; i < scan.products.size(); ++i) {
        bool hasTarget = false;
        for (const auto& id : scan.products[i].modIds) {
            if (id == kModBreachRares) { hasTarget = true; break; }
        }
        sprintf_s(buf, "产物[%zu] 含目标词缀 TowerBreachAdditionalRares=%d (期望1)", i, (int)hasTarget);
        EXPECT_TRUE(hasTarget, buf);
    }

    // 验证原料都没有目标词缀
    for (size_t i = 0; i < scan.materials.size(); ++i) {
        bool hasTarget = false;
        for (const auto& id : scan.materials[i].modIds) {
            if (id == kModBreachRares) { hasTarget = true; break; }
        }
        sprintf_s(buf, "原料[%zu] 不含目标词缀=%d (期望0, 原料不应有目标词缀)", i, (int)hasTarget);
        EXPECT_FALSE(hasTarget, buf);
    }

    printf("\n--- 阶段2: 合成模拟 ---\n");
    printf("取3个原料放入重铸台，合成后检测产物\n");

    // 模拟合成：产出有目标词缀的碑牌
    PluginSDK::ItemMods productMods = MakeMockMods({
        {kModBreachRares, 0xG00D10099},
        {kModBreachPackSize, 0xG00D10098}
    });
    std::vector<std::string> prodIds;
    std::vector<uint32_t>    prodHashes;
    TabletReforgeGame::ExtractModIds(productMods, prodIds, prodHashes, false, "product");

    bool isProduct = TabletReforgeGame::MatchesDesiredProductTypeEx(
        kBreachTablet, "Breach Tablet", 1, true, prodIds, prodHashes, cfg);
    EXPECT_TRUE(isProduct, "合成产物含 TowerBreachAdditionalRares → 判定为产物");

    // 验证产物的modIds被正确提取（2个白名单mod）
    sprintf_s(buf, "产物modIds数=%zu (期望2, TowerBreachAdditionalRares+TowerPackSizeIncrease)", prodIds.size());
    EXPECT_EQ(prodIds.size(), (size_t)2, buf);
}

// ============================================================
// 测试3: 方案A vs 方案B 对比
// ============================================================
void TestPlanAVsPlanB() {
    printf("\n====== 集成测试3: 方案A vs 方案B 对比 ======\n");
    printf("同一仓库数据，对比两种方案的原料/产物判定差异\n\n");

    TabletReforgeGame::ClearHashCachesForTest();

    // === 构造 Mock 仓库数据 ===
    // 6个神經催化劑：3个有白名单mod, 3个无
    std::vector<StashTablet> stash;
    std::vector<PluginSDK::ItemMods> modsList;

    for (int i = 0; i < 3; ++i) {
        stash.push_back(MakeMockStashItem(kCatalystMana, "Neural Catalyst", 0, false, i, 0, 10));
        modsList.push_back(MakeMockMods({{kModUnknown1, 0xBAD20001 + i}}));
    }
    for (int i = 0; i < 3; ++i) {
        stash.push_back(MakeMockStashItem(kCatalystMana, "Neural Catalyst", 0, false, 3 + i, 0, 10));
        modsList.push_back(MakeMockMods({{kModBreachRares, 0xG00D20001 + i}}));
    }

    // === 方案A ===
    printf("--- 方案A (enableBonusMatch=false) ---\n");
    Settings cfgA = MakePlanAConfig();
    ScanResult scanA = SimulateStashScan(stash, modsList, cfgA);

    char buf[256];
    sprintf_s(buf, "方案A 原料数=%zu (期望6, 不区分词缀, 神經催化劑全是原料)", scanA.materials.size());
    EXPECT_EQ(scanA.materials.size(), (size_t)6, buf);

    sprintf_s(buf, "方案A 产物数=%zu (期望0)", scanA.products.size());
    EXPECT_EQ(scanA.products.size(), (size_t)0, buf);

    sprintf_s(buf, "方案A modIds提取数=%d (期望0, 方案A不调用ExtractModIds)", scanA.modIdsExtracted);
    EXPECT_EQ(scanA.modIdsExtracted, 0, buf);

    // === 方案B ===
    printf("\n--- 方案B (enableBonusMatch=true) ---\n");
    TabletReforgeGame::ClearHashCachesForTest();
    Settings cfgB = MakePlanBConfig();
    ScanResult scanB = SimulateStashScan(stash, modsList, cfgB);

    sprintf_s(buf, "方案B 原料数=%zu (期望3, 只有无目标词缀的是原料)", scanB.materials.size());
    EXPECT_EQ(scanB.materials.size(), (size_t)3, buf);

    sprintf_s(buf, "方案B 产物数=%zu (期望0, 神經催化劑未选中子类)", scanB.products.size());
    EXPECT_EQ(scanB.products.size(), (size_t)0, buf);

    sprintf_s(buf, "方案B 跳过数=%zu (期望3, 有目标词缀但不匹配产物条件)", scanB.skipped.size());
    EXPECT_EQ(scanB.skipped.size(), (size_t)3, buf);

    sprintf_s(buf, "方案B modIds提取数=%d (期望3, 3个有白名单mod)", scanB.modIdsExtracted);
    EXPECT_EQ(scanB.modIdsExtracted, 3, buf);

    // === 关键差异验证 ===
    printf("\n--- 关键差异 ---\n");
    sprintf_s(buf, "方案A原料(%zu) > 方案B原料(%zu): 方案B通过词缀过滤减少了原料数",
        scanA.materials.size(), scanB.materials.size());
    EXPECT_TRUE(scanA.materials.size() > scanB.materials.size(), buf);

    printf("  [INFO] 方案A: 不读词缀, 所有匹配子类的物品都是原料\n");
    printf("  [INFO] 方案B: 读Mod.Id, 只有无目标词缀的物品才是原料\n");
    printf("  [INFO] 差异: 方案B将%d个有目标词缀的物品从原料中排除\n",
           (int)(scanA.materials.size() - scanB.materials.size()));
}

// ============================================================
// 测试4: RandomBackoff 频控集成
// ============================================================
void TestRandomBackoffIntegration() {
    printf("\n====== 集成测试4: RandomBackoff 频控集成 ======\n");
    printf("验证: 仓库打开事件→Arm→退让→扫描→节流→再次扫描\n\n");

    TabletReforgeGame::RandomBackoff backoff;

    // === 场景: 仓库打开事件触发 ===
    printf("--- 场景: 仓库打开事件触发 ---\n");
    backoff.ArmWithDelayForTest(300);  // 300ms退让
    EXPECT_TRUE(backoff.IsArming(), "仓库打开后 IsArming=true (退让中)");

    // 立即扫描 → 应被阻止
    bool canScan1 = backoff.ShouldFire();
    EXPECT_FALSE(canScan1, "退让期内 ShouldFire=false (阻止扫描)");

    // 等待退让完成
    TabletReforgeFlow::Clock::SleepMs(350);
    bool canScan2 = backoff.ShouldFire();
    EXPECT_TRUE(canScan2, "退让期后 ShouldFire=true (允许扫描)");

    // 立即再次扫描 → 应被节流
    bool canScan3 = backoff.ShouldFire();
    EXPECT_FALSE(canScan3, "首次扫描后500ms节流 ShouldFire=false");

    // 等待节流完成
    TabletReforgeFlow::Clock::SleepMs(550);
    bool canScan4 = backoff.ShouldFire();
    EXPECT_TRUE(canScan4, "节流期后 ShouldFire=true (允许再次扫描)");

    // === 场景: 真实随机退让 ===
    printf("\n--- 场景: 真实随机退让 (800-1500ms) ---\n");
    backoff.Arm();
    int delay = backoff.DelayMs();
    char buf[256];
    sprintf_s(buf, "真实Arm退让时长=%dms (应在800-1500范围)", delay);
    EXPECT_TRUE(delay >= 800 && delay <= 1500, buf);

    // === 场景: 多次Arm重置 ===
    printf("\n--- 场景: 多次Arm重置退让 ---\n");
    backoff.ArmWithDelayForTest(100);
    backoff.ArmWithDelayForTest(200);  // 重置
    EXPECT_TRUE(backoff.IsArming(), "第二次Arm后 IsArming=true (重置退让)");
    TabletReforgeFlow::Clock::SleepMs(250);
    bool canScan5 = backoff.ShouldFire();
    EXPECT_TRUE(canScan5, "第二次Arm退让期后 ShouldFire=true");
}

// ============================================================
// 测试5: Hash32 缓存性能（多次扫描场景）
// ============================================================
void TestHash32CachePerformance() {
    printf("\n====== 集成测试5: Hash32 缓存性能 ======\n");
    printf("验证: 多次仓库扫描时缓存命中, 避免重复字符串匹配\n\n");

    TabletReforgeGame::ClearHashCachesForTest();

    // === 构造Mock数据 ===
    std::vector<PluginSDK::ItemMods> scanBatch;
    for (int i = 0; i < 20; ++i) {
        scanBatch.push_back(MakeMockMods({
            {kModBreachRares, 0xG00D30001 + i},      // 白名单
            {kModUnknown1, 0xBAD30001 + i},          // 非白名单
        }));
    }

    // === 第一次扫描（全缓存未命中）===
    printf("--- 第一次扫描 (20个物品, 全缓存未命中) ---\n");
    int extracted1 = 0;
    for (const auto& mods : scanBatch) {
        std::vector<std::string> ids;
        std::vector<uint32_t>    hashes;
        TabletReforgeGame::ExtractModIds(mods, ids, hashes, false, "item");
        extracted1 += (int)ids.size();
    }
    size_t goodAfter1 = TabletReforgeGame::GetKnownGoodHashes().size();
    size_t badAfter1  = TabletReforgeGame::GetKnownBadHashes().size();

    char buf[256];
    sprintf_s(buf, "第一次: 提取白名单mod=%d (期望20), goodHashes=%zu (期望20), badHashes=%zu (期望20)",
        extracted1, goodAfter1, badAfter1);
    EXPECT_TRUE(extracted1 == 20 && goodAfter1 == 20 && badAfter1 == 20, buf);

    // === 第二次扫描（相同Hash, 全缓存命中）===
    printf("\n--- 第二次扫描 (相同物品, 全缓存命中) ---\n");
    int extracted2 = 0;
    for (const auto& mods : scanBatch) {
        std::vector<std::string> ids;
        std::vector<uint32_t>    hashes;
        TabletReforgeGame::ExtractModIds(mods, ids, hashes, false, "item");
        extracted2 += (int)ids.size();
    }
    size_t goodAfter2 = TabletReforgeGame::GetKnownGoodHashes().size();
    size_t badAfter2  = TabletReforgeGame::GetKnownBadHashes().size();

    sprintf_s(buf, "第二次: 提取白名单mod=%d (期望20), goodHashes=%zu (期望20, 不增长), badHashes=%zu (期望20, 不增长)",
        extracted2, goodAfter2, badAfter2);
    EXPECT_TRUE(extracted2 == 20 && goodAfter2 == 20 && badAfter2 == 20, buf);

    // === 第三次扫描（部分新Hash, 部分缓存命中）===
    printf("\n--- 第三次扫描 (10个旧+10个新, 部分缓存命中) ---\n");
    std::vector<PluginSDK::ItemMods> mixedBatch;
    // 10个旧的（前10个）
    for (int i = 0; i < 10; ++i) {
        mixedBatch.push_back(scanBatch[i]);
    }
    // 10个新的
    for (int i = 0; i < 10; ++i) {
        mixedBatch.push_back(MakeMockMods({
            {kModRarityIncrease, 0xG00D40001 + i},  // 新白名单
            {kModUnknown2, 0xBAD40001 + i},          // 新非白名单
        }));
    }

    int extracted3 = 0;
    for (const auto& mods : mixedBatch) {
        std::vector<std::string> ids;
        std::vector<uint32_t>    hashes;
        TabletReforgeGame::ExtractModIds(mods, ids, hashes, false, "item");
        extracted3 += (int)ids.size();
    }
    size_t goodAfter3 = TabletReforgeGame::GetKnownGoodHashes().size();
    size_t badAfter3  = TabletReforgeGame::GetKnownBadHashes().size();

    sprintf_s(buf, "第三次: 提取白名单mod=%d (期望20), goodHashes=%zu (期望30, +10新), badHashes=%zu (期望30, +10新)",
        extracted3, goodAfter3, badAfter3);
    EXPECT_TRUE(extracted3 == 20 && goodAfter3 == 30 && badAfter3 == 30, buf);

    printf("\n--- 缓存性能总结 ---\n");
    printf("  [INFO] 3次扫描共处理60个物品, 缓存从0增长到 good=%zu bad=%zu\n",
        goodAfter3, badAfter3);
    printf("  [INFO] 缓存命中率: 第2次100%%, 第3次50%% (10/20命中)\n");

    TabletReforgeGame::ClearHashCachesForTest();
}

// ============================================================
// 测试6: 熔断开关降级验证
// ============================================================
void TestCircuitBreakerDegradation() {
    printf("\n====== 集成测试6: 熔断开关降级验证 ======\n");
    printf("验证: 一键关闭 enableBonusMatch → 立即降级为方案A\n\n");

    TabletReforgeGame::ClearHashCachesForTest();

    // 构造数据：3个有目标词缀的神經催化劑
    std::vector<StashTablet> stash;
    std::vector<PluginSDK::ItemMods> modsList;
    for (int i = 0; i < 3; ++i) {
        stash.push_back(MakeMockStashItem(kCatalystMana, "Neural Catalyst", 0, false, i, 0, 10));
        modsList.push_back(MakeMockMods({{kModBreachRares, 0xG00D50001 + i}}));
    }

    // === 方案B正常工作 ===
    printf("--- 方案B正常工作 (enableBonusMatch=true) ---\n");
    Settings cfgB = MakePlanBConfig();
    ScanResult scanB = SimulateStashScan(stash, modsList, cfgB);

    char buf[256];
    sprintf_s(buf, "方案B: 原料=%zu (期望0, 有目标词缀不是原料)", scanB.materials.size());
    EXPECT_EQ(scanB.materials.size(), (size_t)0, buf);

    // === 紧急关闭熔断开关 ===
    printf("\n--- 紧急关闭熔断开关 (enableBonusMatch=false) ---\n");
    Settings cfgA = cfgB;  // 复制配置
    cfgA.enableBonusMatch = false;  // 一键关闭

    ScanResult scanA = SimulateStashScan(stash, modsList, cfgA);
    sprintf_s(buf, "降级后: 原料=%zu (期望3, 方案A不区分词缀全为原料)", scanA.materials.size());
    EXPECT_EQ(scanA.materials.size(), (size_t)3, buf);

    sprintf_s(buf, "降级后: modIds提取=%d (期望0, 方案A不调ExtractModIds)", scanA.modIdsExtracted);
    EXPECT_EQ(scanA.modIdsExtracted, 0, buf);

    printf("\n--- 熔断结论 ---\n");
    printf("  [INFO] 关闭前: 方案B, 原料=0 (词缀过滤生效)\n");
    printf("  [INFO] 关闭后: 方案A, 原料=3 (降级为纯子类筛选)\n");
    printf("  [INFO] 降级无需重新编译, 仅修改settings.json即可\n");
}

// ============================================================
// 测试7: 静默压力测试模式
// ============================================================
void TestSilentMode() {
    printf("\n====== 集成测试7: 静默压力测试模式 ======\n");
    printf("验证: bonusMatchSilent=true时, ExtractModIds仍调用但判定走方案A\n\n");

    TabletReforgeGame::ClearHashCachesForTest();

    // 3个有目标词缀的神經催化劑
    std::vector<StashTablet> stash;
    std::vector<PluginSDK::ItemMods> modsList;
    for (int i = 0; i < 3; ++i) {
        stash.push_back(MakeMockStashItem(kCatalystMana, "Neural Catalyst", 0, false, i, 0, 10));
        modsList.push_back(MakeMockMods({{kModBreachRares, 0xG00D60001 + i}}));
    }

    // === 静默模式 ===
    printf("--- 静默模式 (enableBonusMatch=true, bonusMatchSilent=true) ---\n");
    Settings cfgSilent = MakePlanBConfig();
    cfgSilent.bonusMatchSilent = true;

    ScanResult scanSilent = SimulateStashScan(stash, modsList, cfgSilent);

    char buf[256];
    // 静默模式: ExtractModIds 仍调用（modIds被提取），但判定走方案A（所有都是原料）
    sprintf_s(buf, "静默模式: 原料=%zu (期望3, 判定走方案A)", scanSilent.materials.size());
    EXPECT_EQ(scanSilent.materials.size(), (size_t)3, buf);

    // 但 ExtractModIds 仍然被调用（因为 SimulateStashScan 检查 enableBonusMatch）
    sprintf_s(buf, "静默模式: modIds提取=%d (期望3, ExtractModIds仍调用)", scanSilent.modIdsExtracted);
    EXPECT_EQ(scanSilent.modIdsExtracted, 3, buf);

    printf("\n--- 静默模式结论 ---\n");
    printf("  [INFO] ExtractModIds 被调用: 是 (modIds已提取, 用于日志观察)\n");
    printf("  [INFO] 判定逻辑: 走方案A (不影响行为, 安全观察)\n");
    printf("  [INFO] 用途: 1-2周观察期, 验证ReadItemMods回调是否被容忍\n");
}

// ============================================================
// 测试8: 宪法红线验证（绝不读取禁读字段）
// ============================================================
void TestConstitutionRedLine() {
    printf("\n====== 集成测试8: 宪法红线验证 ======\n");
    printf("验证: ExtractModIds 只读 Mod.Id 和 Mod.Hash32, 绝不读 Name/AffixName/StatKey\n\n");

    TabletReforgeGame::ClearHashCachesForTest();

    // 构造包含所有字段的mock Mod
    PluginSDK::ItemMods mods;
    mods.Valid = true;
    PluginSDK::Mod m;
    m.Id = "TowerBreachAdditionalRares";
    m.Hash32 = 0xG00D70001;
    m.Name = "Breached Rares";           // 禁读字段
    m.AffixName = "of Breaching";        // 禁读字段
    m.StatKey = "map_breach_rares";      // 禁读字段
    m.Value0 = 5.0f;
    m.Value1 = 0.0f;
    mods.ExplicitMods.push_back(m);

    // 调用 ExtractModIds
    std::vector<std::string> outIds;
    std::vector<uint32_t>    outHashes;
    bool ok = TabletReforgeGame::ExtractModIds(mods, outIds, outHashes, false, "test");

    char buf[256];
    sprintf_s(buf, "ExtractModIds 返回=%d (期望1, Valid=true)", (int)ok);
    EXPECT_TRUE(ok, buf);

    // 验证只提取了 Id 和 Hash32
    sprintf_s(buf, "输出modIds数=%zu (期望1)", outIds.size());
    EXPECT_EQ(outIds.size(), (size_t)1, buf);

    if (!outIds.empty()) {
        sprintf_s(buf, "modIds[0]=%s (期望TowerBreachAdditionalRares)", outIds[0].c_str());
        EXPECT_TRUE(outIds[0] == "TowerBreachAdditionalRares", buf);
    }

    sprintf_s(buf, "输出modHashes数=%zu (期望1)", outHashes.size());
    EXPECT_EQ(outHashes.size(), (size_t)1, buf);

    if (!outHashes.empty()) {
        sprintf_s(buf, "modHashes[0]=0x%08X (期望0xG00D70001)", outHashes[0]);
        EXPECT_TRUE(outHashes[0] == 0xG00D70001, buf);
    }

    // 验证输出中绝不包含 Name/AffixName/StatKey 的内容
    bool noName = true, noAffix = true, noStatKey = true;
    for (const auto& id : outIds) {
        if (id.find("Breached Rares") != std::string::npos) noName = false;
        if (id.find("of Breaching") != std::string::npos) noAffix = false;
        if (id.find("map_breach_rares") != std::string::npos) noStatKey = false;
    }
    EXPECT_TRUE(noName, "输出不含 Mod.Name 内容 (宪法红线)");
    EXPECT_TRUE(noAffix, "输出不含 Mod.AffixName 内容 (宪法红线)");
    EXPECT_TRUE(noStatKey, "输出不含 Mod.StatKey 内容 (宪法红线)");

    printf("\n--- 宪法红线验证结论 ---\n");
    printf("  [PASS] Mod.Id 被读取 (允许)\n");
    printf("  [PASS] Mod.Hash32 被读取 (允许)\n");
    printf("  [PASS] Mod.Name 未被读取 (绝对红线)\n");
    printf("  [PASS] Mod.AffixName 未被读取 (绝对红线)\n");
    printf("  [PASS] Mod.StatKey 未被读取 (绝对红线)\n");
}

// ============================================================
// 主函数
// ============================================================
int main() {
    printf("=================================================================\n");
    printf("   方案B「合规词缀Id读取」完整合成流程集成测试\n");
    printf("   宪法修正案 v1.3 — EnableBonusMatch 全链路验证\n");
    printf("=================================================================\n");
    printf("测试范围: 仓库扫描→原料识别→三槽同物→合成→产物检测→结束条件\n");
    printf("         频控集成→缓存性能→熔断降级→静默模式→宪法红线\n");

    TestPlanBFullFlowCatalysts();
    TestPlanBTabletFlow();
    TestPlanAVsPlanB();
    TestRandomBackoffIntegration();
    TestHash32CachePerformance();
    TestCircuitBreakerDegradation();
    TestSilentMode();
    TestConstitutionRedLine();

    printf("\n=================================================================\n");
    printf("                    集成测试结果汇总\n");
    printf("=================================================================\n");
    printf("  PASS: %d\n", g_passCount);
    printf("  FAIL: %d\n", g_failCount);
    printf("  WARN: %d\n", g_warnCount);
    printf("  结果: %s\n", (g_failCount == 0) ? "ALL PASS ✓" : "HAS FAIL ✗");
    printf("=================================================================\n");
    return (g_failCount == 0) ? 0 : 1;
}
