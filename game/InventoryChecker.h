// InventoryChecker.h — 背包状态检查（物品计数/背包满/快照比对）
//
// 提供背包内合成物品的查询能力：
//   - CountMaterialTablets：匹配当前合成类型的原料物品数
//   - CountAllTabletsInBag：背包内所有碑牌类物品数
//   - NextMaterialTablet：下一个要取的原料的屏幕坐标
//   - IsBagFull：背包是否满（考虑预留格数）
//   - SnapshotBag：快照当前背包物品地址集合
//
// 关键改进（2026-07-28）：不再只扫描 MainInventory* 前缀面板，
// 而是扫描所有可见 Inventory 面板，排除：
//   - 大仓库面板（>= 40 格）
//   - 纯装备面板（Helmet/Body/Gloves 等）
//   - 重铸台合成面板（4-24 格的小面板）
// 这样当重铸台打开时，背包物品仍然能被正确扫描到。
//
// 安全：只用 InventoryItem 的零风险字段，绝不调 ReadItemMods。
#pragma once

#include "PanelDetector.h"

#include <string>
#include <fstream>
#include <mutex>
#include "TabletFilter.h"
#include "../sdk/PluginSDK.h"

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace TabletReforgeGame {

// 调试日志文件路径
inline std::string GetDebugLogPath() {
    return "D:\\PoeFixer\\Plugins\\TabletReforgeAutomation\\logs\\debug_matching.log";
}

// 文件日志辅助函数：同时输出到 OutputDebugString 和文件
inline void DebugLog(const std::string& msg) {
    // 输出到调试器
    OutputDebugStringA(msg.c_str());
    
    // 追加写入日志文件
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    
    std::ofstream file(GetDebugLogPath(), std::ios::app);
    if (file.is_open()) {
        file << msg;
        file.flush();
    }
}

// 背包内的一个可点击物品（屏幕坐标 + 地址，用于快照比对）
struct BagItem {
    uintptr_t address = 0;
    ScreenRect rect;
    bool identified = false;
    int rarity = 0;
    int  slotX = 0;       // 格子 X 坐标（排序用）
    int  slotY = 0;       // 格子 Y 坐标
    std::string path;
    std::string baseType;
    std::string inventoryName;
    // 【方案 B v1.3】合规词缀 Id（替代旧的 modNames/modAffixes/modStatKeys）
    // 仅在 enableBonusMatch=true 时通过 ExtractModIds 填充
    // 只含白名单内的 Mod.Id（snake_case），绝不含 Mod.Name/AffixName/StatKey
    std::vector<std::string> modIds;     // Mod.Id（已白名单过滤）
    std::vector<uint32_t>    modHashes;  // Mod.Hash32（与 modIds 一一对应）
};

// 【方案 B v1.3】从 ItemMods 提取白名单内的 Mod.Id + Mod.Hash32
// 安全保证（宪法修正案 v1.3 绝对红线）：
//   - 只读 m.Id 和 m.Hash32，绝不读 m.Name/AffixName/StatKey
//   - 未知 Id 立即丢弃，不入 bi.modIds
// 仅当 settings.enableBonusMatch=true 时调用（由调用方判断）
inline void ExtractModIds(const PluginSDK::ItemMods& mods, BagItem& bi,
                          bool silentLog = false) {
    ExtractModIds(mods, bi.modIds, bi.modHashes, silentLog, bi.path);
}

// 判断一个 inventory 面板是否是"背包类"面板（应被扫描）
// 规则：
//   1. MainInventory* 前缀 / Backpack / Player Inventory 等 → 始终视为背包
//      POE2 主背包在某些状态下（如重铸台打开时）Grid.Valid 可能为 false，
//      但物品数据仍然存在，必须扫描以识别物品类型
//   2. 非背包名且 >= 40 格 → 视为仓库，排除
//   3. <= 2 格 → 太小，排除
//   4. 名字含纯装备部位关键词（Helmet/Body/Weapon 等）→ 排除
//   5. 4-24 格的小面板：若名字无装备关键词且有物品 → 可能是背包子面板
inline bool IsBagLikeInventory(const PluginSDK::Inventory& inv, const char* name) {
    const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
    if (slots <= 0) return false;

    // === 1. 背包面板识别：MainInventory* / Backpack / Player Inventory 等 ===
    // 关键：即使 Grid.Valid=false，物品数据仍可用于识别
    if (name && name[0]) {
        // MainInventory* 前缀（POE2 主背包子面板）
        if (std::strncmp(name, "MainInventory", 12) == 0) {
            return true;
        }
        // 常见背包名称（不含 Stash，因为 Stash 是仓库）
        static const char* kBagNames[] = {
            "Backpack", "Player Inventory", "PlayerInventory",
            "MainBackpack",
        };
        for (const char* bn : kBagNames) {
            if (std::strstr(name, bn) != nullptr) {
                return true;
            }
        }
    }

    // === 2. 非背包面板且 Grid 无效 → 跳过 ===
    if (!inv.Grid.Valid) return false;

    // === 3. >= 40 格 → 仓库，排除 ===
    if (slots >= 40) return false;

    // === 4. <= 2 格 → HUD 小面板，排除 ===
    if (slots <= 2) return false;

    // === 5. 名字含纯装备部位关键词 → 排除 ===
    if (name && name[0]) {
        static const char* kEquipOnlyKeywords[] = {
            "Helmet", "Body", "Gloves", "Boots", "Weapon", "Shield",
            "Amulet", "Ring", "Quiver", "Trinket", "Charm",
            "Jewellery", "Equipment", "Equip",
        };
        for (const char* kw : kEquipOnlyKeywords) {
            if (std::strstr(name, kw) != nullptr) return false;
        }
    }

    // === 6. 名字含 Belt/Skill/Gem/Flask → HUD 小面板，排除 ===
    if (name && name[0]) {
        static const char* kHudKeywords[] = {
            "Belt", "Skill", "Gem", "Flask",
        };
        for (const char* kw : kHudKeywords) {
            if (std::strstr(name, kw) != nullptr) return false;
        }
    }

    // === 7. 4-24 格的小面板：如果有物品且不是装备/HUD → 可能是背包子面板 ===
    if (slots >= 4 && slots <= 24) {
        if (inv.Items.empty()) {
            return false;
        }
        return true;
    }

    // 其他情况 → 保留
    return true;
}

// 判断某 inventory 是否为重铸台/合成面板（需从背包计数中排除）
// 重铸台打开时，其输入槽面板(4-24格 on-screen)会包含已放入的原料，
// 若不排除这些面板，CountMaterialTablets 会重复计数导致背包数量永不减少
inline bool IsBenchInputPanel(const PluginSDK::Inventory& inv, const char* name,
                               float displayW, float displayH) {
    const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
    if (slots < 4 || slots > 24) return false;
    if (!inv.Grid.Valid) return false;
    if (!GridOnScreen(inv, displayW, displayH)) return false;

    // MainInventory* 和常见背包名面板已经由 IsBagLikeInventory 单独处理，这里排除它们
    if (name && name[0]) {
        if (std::strncmp(name, "MainInventory", 12) == 0) return false;
        static const char* kBagNames[] = {
            "Backpack", "Player Inventory", "PlayerInventory", "MainBackpack",
        };
        for (const char* bn : kBagNames) {
            if (std::strstr(name, bn) != nullptr) return false;
        }
        static const char* kEquipHudKws[] = {
            "Helmet", "Body", "Gloves", "Boots", "Weapon", "Shield",
            "Amulet", "Ring", "Quiver", "Trinket", "Charm",
            "Jewellery", "Equipment", "Equip", "Belt", "Skill", "Gem", "Flask",
        };
        for (const char* kw : kEquipHudKws) {
            if (std::strstr(name, kw) != nullptr) return false;
        }
    }
    // 4-24格 on-screen 且非背包/装备/HUD → 很可能是重铸台输入面板
    return true;
}

// 收集所有"背包类"面板内的物品（扫描所有可见面板，排除仓库、装备和重铸台面板）
// 关键改进：即使物品没有解析到屏幕坐标，也记录下来（用于诊断）
// 修复：排除重铸台输入面板(4-24格 on-screen)，避免合成后物品计数不减少的BUG
inline std::vector<BagItem> CollectBagItems(const PluginSDK::Context* ctx) {
    std::vector<BagItem> out;
    if (!ctx) return out;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    // 预先识别重铸台输入面板的 inventoryId 集合
    std::unordered_set<int> benchPanelIds;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (IsBenchInputPanel(inv, name, displayW, displayH)) {
            benchPanelIds.insert(inv.InventoryId);
        }
    }

    for (const auto& inv : ctx->Inventory.GetAll()) {
        // 跳过重铸台输入面板（避免重复计数）
        if (benchPanelIds.count(inv.InventoryId)) continue;

        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        
        if (!IsBagLikeInventory(inv, name)) continue;

        std::string nameStr = name ? name : "(unknown)";

        for (const auto& item : inv.Items) {
            auto rect = ResolveItemRect(inv, item, displayW, displayH);
            
            // 即使没有坐标，也记录物品（用于诊断和计数）
            // 但只有有坐标的物品才能被点击操作
            BagItem bi;
            bi.address = item.Address;
            bi.rect = rect.value_or(ScreenRect{});  // 空坐标或有效坐标
            bi.identified = item.IsIdentified;
            bi.rarity = item.Rarity;
            bi.slotX = item.SlotX;
            bi.slotY = item.SlotY;
            bi.path = item.Path;
            bi.baseType = item.BaseTypeName;
            bi.inventoryName = nameStr;
            out.push_back(std::move(bi));
        }
    }

    // === 排序：从左至右，从上至下（相邻点击优先）===
    std::sort(out.begin(), out.end(), [](const BagItem& a, const BagItem& b) {
        if (a.inventoryName != b.inventoryName) return a.inventoryName < b.inventoryName;
        if (a.slotY != b.slotY) return a.slotY < b.slotY;
        return a.slotX < b.slotX;
    });

    return out;
}

// 调试函数：输出背包物品的详细匹配信息
inline void DebugLogBagItems(const PluginSDK::Context* ctx,
                              const TabletReforgeConfig::Settings& settings) {
    if (!ctx) return;
    
    auto all = CollectBagItems(ctx);
    
    // 记录settings配置
    {
        char buf[512];
        ::sprintf_s(buf, "[InventoryDebug] Settings: itemType=%d, subCategoryId=%d, requireIdentified=%d, useSubCategoryMode=%d, selectedSubCategories.size=%zu\n",
            settings.itemType, settings.subCategoryId, 
            settings.requireIdentified ? 1 : 0,
            settings.useSubCategoryMode ? 1 : 0,
            settings.selectedSubCategories.size());
        DebugLog(buf);
    }
    
    // 记录每个物品的详细信息
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& bi = all[i];
        auto match = AnalyzeMatchDetail(bi.path, bi.baseType, bi.rarity, bi.identified, settings);
        
        // 安全格式化Path和BT
        std::string safePath = bi.path;
        std::string safeBt = bi.baseType;
        for (auto& c : safePath) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        for (auto& c : safeBt) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
        if (safePath.size() > 80) safePath = safePath.substr(0, 80) + "...";
        if (safeBt.size() > 80) safeBt = safeBt.substr(0, 80) + "...";
        
        char buf[2048];
        ::sprintf_s(buf, "[InventoryDebug] #%zu: Path='%s' BT='%s' rarity=%d ident=%d rect=(%.0f,%.0f,%.0f,%.0f) matchAsMaterial=%d matchAsProduct=%d typeKey='%s' subCatId=%d\n",
            i, safePath.c_str(), safeBt.c_str(), bi.rarity, bi.identified ? 1 : 0,
            bi.rect.x, bi.rect.y, bi.rect.w, bi.rect.h,
            match.matchesAsMaterial ? 1 : 0, match.matchesAsProduct ? 1 : 0,
            match.typeKey.c_str(), match.subCategoryId);
        DebugLog(buf);
        
        // 记录匹配原因
        ::sprintf_s(buf, "[InventoryDebug]   匹配细节: %s\n", match.debugDetail.c_str());
        DebugLog(buf);
    }
    
    DebugLog("[InventoryDebug] 物品调试日志结束\n");
}

// 统计「匹配当前选择的合成类型」的原料物品数量（基于 Settings）
// 注意：始终通过 ReadItemMods 获取准确的 rarity/identified，而非库存数据
inline int CountMaterialTablets(const PluginSDK::Context* ctx,
                                const TabletReforgeConfig::Settings& settings) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredReforgeTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            ++count;
    }
    return count;
}

// 统计背包里所有碑牌类物品数量（不限鉴定状态，用于诊断）
inline int CountAllTabletsInBag(const PluginSDK::Context* ctx) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        if (IsAnyTabletLike(bi.path, bi.baseType))
            ++count;
    }
    return count;
}

// 统计背包里所有物品数量
inline int CountAllItemsInBag(const PluginSDK::Context* ctx) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        count++;
    }
    return count;
}

// 统计背包里有有效坐标的物品数量（可点击）
inline int CountClickableItemsInBag(const PluginSDK::Context* ctx) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        if (bi.rect.w > 0.f && bi.rect.h > 0.f) count++;
    }
    return count;
}

// 诊断：返回背包物品的统计信息
struct BagItemStats {
    int totalItems = 0;
    int itemsWithCoords = 0;      // 有有效屏幕坐标（可点击）
    int itemsWithoutCoords = 0;   // 无有效坐标（诊断用）
    int itemsWithPath = 0;        // 有 Path 字段
    int itemsWithBT = 0;          // 有 BaseType 字段
    int itemsMatchingType = 0;    // 匹配当前合成类型
    int bagLikePanels = 0;        // 背包类面板数量
    int totalPanels = 0;          // 总面板数量
};

inline BagItemStats GetBagItemStats(const PluginSDK::Context* ctx,
                                     const TabletReforgeConfig::Settings& settings) {
    BagItemStats stats;
    if (!ctx) return stats;

    auto all = CollectBagItems(ctx);
    stats.totalItems = static_cast<int>(all.size());

    for (const auto& bi : all) {
        if (bi.rect.w > 0.f && bi.rect.h > 0.f) stats.itemsWithCoords++;
        else stats.itemsWithoutCoords++;
        if (!bi.path.empty()) stats.itemsWithPath++;
        if (!bi.baseType.empty()) stats.itemsWithBT++;
        
        // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredReforgeTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            stats.itemsMatchingType++;
    }

    // 统计面板数量
    for (const auto& inv : ctx->Inventory.GetAll()) {
        stats.totalPanels++;
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (IsBagLikeInventory(inv, name)) stats.bagLikePanels++;
    }

    return stats;
}

// 合并扫描结果：一次扫描返回所有背包统计数据（性能优化）
struct CombinedBagScan {
    int materialCount = 0;       // 匹配当前合成类型的原料数
    int allTabletsCount = 0;     // 所有碑牌类物品数
    int totalItems = 0;          // 总物品数
    int clickableItems = 0;      // 可点击物品数（有坐标）
    int emptySlots1x1 = 0;       // 1x1 空槽数
    int itemsWithPath = 0;       // 有 Path 字段的物品数
    int itemsWithBT = 0;         // 有 BaseType 字段的物品数
    int bagLikePanels = 0;       // 背包类面板数
    int totalPanels = 0;         // 总面板数
    std::vector<BagItem> items;  // 扫描到的所有物品（供后续操作用）
};

inline CombinedBagScan ScanBagCombined(const PluginSDK::Context* ctx,
                                        TabletReforgeConfig::Settings& settings) {
    CombinedBagScan result;
    if (!ctx) return result;

    // === 关键: 同步类型级别的词缀勾选到全局词缀过滤器 ===
    SyncBonusIdsToModifierKeys(settings);

    result.items = CollectBagItems(ctx);
    result.totalItems = static_cast<int>(result.items.size());

    // 如果开启词缀筛选模式，需要读取每个物品的词缀
    const bool needMods = settings.useModifierFilterMode && !settings.selectedModifierKeys.empty();

    // 记录配置信息
    if (needMods || settings.verboseLogging) {
        char cfgBuf[2048];
        ::sprintf_s(cfgBuf, "\n[ScanBag] === 背包扫描开始 === 总物品=%d 词缀筛选=%d 选中词缀数=%zu minRarity=%d requireIdentified=%d",
            result.totalItems, needMods ? 1 : 0, settings.selectedModifierKeys.size(),
            settings.minRarityForMaterial, settings.requireIdentifiedForMaterial ? 1 : 0);
        DebugLog(cfgBuf);
        
        if (!settings.selectedModifierKeys.empty()) {
            DebugLog("[ScanBag] 选中词缀关键词: [");
            bool first = true;
            for (const auto& k : settings.selectedModifierKeys) {
                if (!first) DebugLog(", ");
                first = false;
                DebugLog("'" + k + "'");
            }
            DebugLog("]\n");
        }
    }

    int itemIndex = 0;
    for (auto& bi : result.items) {
        if (bi.rect.w > 0.f && bi.rect.h > 0.f) result.clickableItems++;
        if (!bi.path.empty()) result.itemsWithPath++;
        if (!bi.baseType.empty()) result.itemsWithBT++;
        
        // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
        // 库存数据中的 Rarity/IsIdentified 可能不准确，Mods 数据更可靠
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                // 用 Mods 数据更新（更准确）
                if (itemMods.Rarity > 0) {
                    bi.rarity = itemMods.Rarity;
                }
                bi.identified = itemMods.IsIdentified;

                // 【方案 B v1.3】合规词缀 Id 读取（仅 enableBonusMatch=true 时）
                // 只读 Mod.Id + Mod.Hash32，绝不读 Mod.Name/AffixName/StatKey
                if (settings.enableBonusMatch) {
                    ExtractModIds(itemMods, bi, settings.bonusMatchSilent);
                }
            } else if (settings.verboseLogging) {
                char buf[512];
                ::sprintf_s(buf, "[ScanBag]   Item[%d] ReadItemMods 失败 (address=%llu)\n",
                    itemIndex, (unsigned long long)bi.address);
                DebugLog(buf);
            }
        }

        if (IsAnyTabletLike(bi.path, bi.baseType)) result.allTabletsCount++;

        // 【方案 B v1.3】统一走 Ex 包装函数（自动根据开关选择 4/8 参数版）
        bool isMaterial = false;
        std::string matchDebugLog;

        if (settings.verboseLogging) {
            isMaterial = MatchesPoe2DataPatternsDebug(
                bi.path, bi.baseType, bi.rarity, bi.identified, settings, matchDebugLog);
        } else {
            isMaterial = MatchesDesiredReforgeTypeEx(
                bi.path, bi.baseType, bi.rarity, bi.identified,
                bi.modIds, bi.modHashes, settings);
        }

        if (isMaterial) result.materialCount++;

        // 输出详细匹配日志
        if (settings.enableBonusMatch || settings.verboseLogging) {
            char headerBuf[256];
            ::sprintf_s(headerBuf, "[ScanBag] Item[%d] material=%d allTablet=%d rarity=%d ident=%d addr=%llu",
                itemIndex, isMaterial ? 1 : 0,
                IsAnyTabletLike(bi.path, bi.baseType) ? 1 : 0,
                bi.rarity, bi.identified ? 1 : 0,
                (unsigned long long)bi.address);
            DebugLog(headerBuf);
            if (!matchDebugLog.empty()) DebugLog(matchDebugLog);

            bool isProduct = MatchesDesiredProductTypeEx(
                bi.path, bi.baseType, bi.rarity, bi.identified,
                bi.modIds, bi.modHashes, settings);
            char productBuf[128];
            ::sprintf_s(productBuf, "\n[ScanBag] Item[%d] product=%d", itemIndex, isProduct ? 1 : 0);
            DebugLog(productBuf);
        }

        itemIndex++;
    }

    // 输出扫描摘要
    if (needMods || settings.verboseLogging) {
        char summaryBuf[512];
        ::sprintf_s(summaryBuf, "\n[ScanBag] === 扫描摘要 === 总物品=%d 原料=%d 碑牌=%d 可点击=%d 有Path=%d 有BT=%d\n",
            result.totalItems, result.materialCount, result.allTabletsCount,
            result.clickableItems, result.itemsWithPath, result.itemsWithBT);
        DebugLog(summaryBuf);
    }

    // 统计面板数量和空槽
    for (const auto& inv : ctx->Inventory.GetAll()) {
        result.totalPanels++;
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (IsBagLikeInventory(inv, name)) {
            result.bagLikePanels++;

            // 计算空槽
            const int X = inv.TotalBoxesX;
            const int Y = inv.TotalBoxesY;
            if (X > 0 && Y > 0) {
                std::vector<bool> occupied(static_cast<size_t>(X) * Y, false);
                for (const auto& item : inv.Items) {
                    const int w = item.Width > 0 ? item.Width : 1;
                    const int h = item.Height > 0 ? item.Height : 1;
                    const int ix = item.SlotX;
                    const int iy = item.SlotY;
                    for (int dy = 0; dy < h; ++dy) {
                        for (int dx = 0; dx < w; ++dx) {
                            const int x = ix + dx;
                            const int y = iy + dy;
                            if (x >= 0 && x < X && y >= 0 && y < Y)
                                occupied[static_cast<size_t>(y) * X + x] = true;
                        }
                    }
                }
                for (bool b : occupied) if (!b) result.emptySlots1x1++;
            }
        }
    }

    return result;
}

// 统计背包里匹配产品类别的物品数（产物判定，不卡鉴定状态）
inline int CountProductItemsInBag(const PluginSDK::Context* ctx,
                                  const TabletReforgeConfig::Settings& settings) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredProductTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            ++count;
    }
    return count;
}

// 兼容旧接口（不接收 settings 的情况）
inline int CountMaterialTablets(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings defaultCfg;
    defaultCfg.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    defaultCfg.requireIdentified = false;
    defaultCfg.withdrawRequireIdentified = false;
    return CountMaterialTablets(ctx, defaultCfg);
}
inline int CountAnyTempleTablets(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings defaultCfg;
    defaultCfg.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    return CountProductItemsInBag(ctx, defaultCfg);
}

// 查找下一个「匹配当前合成类型」原料物品的屏幕坐标
// 只返回有有效坐标的物品（rect.w > 0 && rect.h > 0）
// 注意：始终通过 ReadItemMods 获取准确的 rarity/identified
inline std::optional<ScreenRect> NextMaterialTablet(const PluginSDK::Context* ctx,
                                                    const TabletReforgeConfig::Settings& settings) {
    for (const auto& bi : CollectBagItems(ctx)) {
        if (bi.rect.w <= 0.f || bi.rect.h <= 0.f) continue;
        
        // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredReforgeTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            return bi.rect;
    }
    return std::nullopt;
}
inline std::optional<ScreenRect> NextMaterialTablet(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings defaultCfg;
    defaultCfg.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    defaultCfg.requireIdentified = false;
    defaultCfg.withdrawRequireIdentified = false;
    return NextMaterialTablet(ctx, defaultCfg);
}

// ============================================================
// 按Path过滤找下一个原料（三槽同物规则）
// requiredPath 为空时等同于 NextMaterialTablet
// ============================================================
inline std::optional<ScreenRect> NextMaterialTabletByPath(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings,
    const std::string& requiredPath)
{
    for (const auto& bi : CollectBagItems(ctx)) {
        if (bi.rect.w <= 0.f || bi.rect.h <= 0.f) continue;

        int rarity = bi.rarity;
        bool identified = bi.identified;
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }

        if (!MatchesDesiredReforgeTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            continue;
        if (!requiredPath.empty() && bi.path != requiredPath)
            continue;
        return bi.rect;
    }
    return std::nullopt;
}

// ============================================================
// 读取重铸台3个输入槽的物品（可堆叠结束条件判定）
// ============================================================
struct BenchSlotItem {
    std::string path;
    std::string baseType;
    int  stackCount = 0;
    int  rarity = 0;
    ScreenRect rect;
};

inline std::vector<BenchSlotItem> CollectBenchInputSlotItems(const PluginSDK::Context* ctx) {
    std::vector<BenchSlotItem> out;
    if (!ctx) return out;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (!IsBenchInputPanel(inv, name, displayW, displayH)) continue;

        for (const auto& item : inv.Items) {
            BenchSlotItem b;
            b.path = item.Path;
            b.baseType = item.BaseTypeName;
            b.stackCount = item.StackCount > 0 ? item.StackCount : 1;
            b.rarity = item.Rarity;
            auto rect = ResolveItemRect(inv, item, displayW, displayH);
            if (rect) b.rect = *rect;
            out.push_back(std::move(b));
        }
    }
    return out;
}

// 查找下一个「匹配当前合成类型」产物的屏幕坐标
// 注意：始终通过 ReadItemMods 获取准确的 rarity/identified
inline std::optional<ScreenRect> NextProductTablet(const PluginSDK::Context* ctx,
                                                   const TabletReforgeConfig::Settings& settings) {
    for (const auto& bi : CollectBagItems(ctx)) {
        if (bi.rect.w <= 0.f || bi.rect.h <= 0.f) continue;
        
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredProductTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            return bi.rect;
    }
    return std::nullopt;
}
inline std::optional<ScreenRect> NextProductTablet(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings defaultCfg;
    defaultCfg.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    return NextProductTablet(ctx, defaultCfg);
}

// 查找下一个"选择的（wanted）"物品的屏幕坐标
// 用于子类模式中将选择的物品存入仓库
// 注意：始终通过 ReadItemMods 获取准确的 rarity/identified
inline std::optional<ScreenRect> NextWantedTabletInBag(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings) {
    for (const auto& bi : CollectBagItems(ctx)) {
        if (bi.rect.w <= 0.f || bi.rect.h <= 0.f) continue;
        
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredProductTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            return bi.rect;
    }
    return std::nullopt;
}

// 统计背包中"选择的（wanted）"物品数量
inline int CountWantedItemsInBag(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredProductTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            ++count;
    }
    return count;
}

// 统计背包中"非选择的（unwanted）"物品数量
inline int CountUnwantedItemsInBag(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings) {
    int count = 0;
    for (const auto& bi : CollectBagItems(ctx)) {
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (MatchesDesiredReforgeTypeEx(bi.path, bi.baseType, rarity, identified, settings))
            ++count;
    }
    return count;
}

// 通过地址查找物品的屏幕坐标（扫描所有可见面板）
inline std::optional<ScreenRect> FindBagItemByAddress(const PluginSDK::Context* ctx, uintptr_t address) {
    if (!ctx || address == 0) return std::nullopt;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        for (const auto& item : inv.Items) {
            if (item.Address == address) {
                auto rect = ResolveItemRect(inv, item, displayW, displayH);
                if (rect) return rect;
                // 坐标解析失败时，用兜底方式计算
                if (inv.Grid.GridScreenX >= 0.f && inv.Grid.GridScreenY >= 0.f) {
                    const float cell = (inv.Grid.CellSize > 0.f) ? inv.Grid.CellSize : 40.0f;
                    const float x = inv.Grid.GridScreenX + static_cast<float>(item.SlotX) * cell;
                    const float y = inv.Grid.GridScreenY + static_cast<float>(item.SlotY) * cell;
                    const float w = static_cast<float>(item.Width > 0 ? item.Width : 1) * cell;
                    const float h = static_cast<float>(item.Height > 0 ? item.Height : 1) * cell;
                    return ScreenRect{x, y, w, h};
                }
            }
        }
    }
    return std::nullopt;
}

// —— 兼容旧名 ——
inline int CountIdentifiedTablets(const PluginSDK::Context* ctx) { return CountMaterialTablets(ctx); }
inline int CountUnidentifiedTablets(const PluginSDK::Context* ctx) { return CountAnyTempleTablets(ctx); }
inline std::optional<ScreenRect> NextIdentifiedTablet(const PluginSDK::Context* ctx) { return NextMaterialTablet(ctx); }
inline std::optional<ScreenRect> NextUnidentifiedTablet(const PluginSDK::Context* ctx) { return NextProductTablet(ctx); }

// 统计所有背包类面板里"单个 1x1 空槽位"的精确数量
inline int Count1x1EmptySlots(const PluginSDK::Context* ctx) {
    if (!ctx) return 0;
    int totalEmpty = 0;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        
        if (!IsBagLikeInventory(inv, name)) continue;

        const int X = inv.TotalBoxesX;
        const int Y = inv.TotalBoxesY;
        if (X <= 0 || Y <= 0) continue;
        std::vector<bool> occupied(static_cast<size_t>(X) * Y, false);
        for (const auto& item : inv.Items) {
            const int w = item.Width > 0 ? item.Width : 1;
            const int h = item.Height > 0 ? item.Height : 1;
            const int ix = item.SlotX;
            const int iy = item.SlotY;
            for (int dy = 0; dy < h; ++dy) {
                for (int dx = 0; dx < w; ++dx) {
                    const int x = ix + dx;
                    const int y = iy + dy;
                    if (x < 0 || x >= X || y < 0 || y >= Y) continue;
                    occupied[y * X + x] = true;
                }
            }
        }
        for (bool b : occupied) if (!b) ++totalEmpty;
    }
    return totalEmpty;
}

// 背包是否"放不下 3 个 1x1 碑牌"
inline bool IsBagFull(const PluginSDK::Context* ctx, int reservedSlots) {
    if (!ctx) return true;
    const int empty = Count1x1EmptySlots(ctx);
    const int need = reservedSlots + 3;
    if (empty < need) {
        return empty <= reservedSlots;
    }
    return false;
}

// 取碑牌阶段专用：当前背包是否还能再容纳至少 1 个 1x1 碑牌
inline bool BagCanFitOneMoreTablet(const PluginSDK::Context* ctx, int reservedSlots) {
    if (!ctx) return false;
    const int empty = Count1x1EmptySlots(ctx);
    return empty > reservedSlots;
}

// 背包物品地址快照（扫描所有背包类面板）
inline std::unordered_set<uintptr_t> SnapshotBag(const PluginSDK::Context* ctx) {
    std::unordered_set<uintptr_t> snap;
    if (!ctx) return snap;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        
        if (!IsBagLikeInventory(inv, name)) continue;

        for (const auto& item : inv.Items) {
            if (item.Address != 0) snap.insert(item.Address);
        }
    }
    return snap;
}

// 快照差异：返回 before 有但 after 没有的地址
inline std::vector<uintptr_t> DiffRemoved(const std::unordered_set<uintptr_t>& before,
                                           const std::unordered_set<uintptr_t>& after) {
    std::vector<uintptr_t> removed;
    for (auto addr : before) {
        if (after.find(addr) == after.end())
            removed.push_back(addr);
    }
    return removed;
}

// 快照差异：返回 after 有但 before 没有的地址
inline std::vector<uintptr_t> DiffAdded(const std::unordered_set<uintptr_t>& before,
                                         const std::unordered_set<uintptr_t>& after) {
    std::vector<uintptr_t> added;
    for (auto addr : after) {
        if (before.find(addr) == before.end())
            added.push_back(addr);
    }
    return added;
}

// ============================================================
// 全面扫描：遍历所有可见 Inventory（用于诊断和调试）
// ============================================================

// 扫描所有可见的 Inventory 面板，返回所有物品
inline std::vector<BagItem> ScanAllVisibleInventories(
    const PluginSDK::Context* ctx,
    bool excludeStashBig = false,
    bool excludeEquipPanels = false)
{
    std::vector<BagItem> out;
    if (!ctx) return out;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    static const char* kEquipKeywords[] = {
        "Helmet", "Body", "Gloves", "Boots", "Weapon", "Shield",
        "Amulet", "Ring", "Quiver", "Trinket", "Charm",
        "Jewellery", "Equipment", "Equip",
    };

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        std::string nameStr = name ? name : "";
        const bool isMainInv = (nameStr.rfind("MainInventory", 0) == 0);

        if (excludeStashBig && slots >= 40 && !isMainInv) continue;
        
        if (excludeEquipPanels && name && name[0]) {
            bool skip = false;
            for (const char* kw : kEquipKeywords) {
                if (std::strstr(name, kw) != nullptr) { skip = true; break; }
            }
            if (skip) continue;
        }

        for (const auto& item : inv.Items) {
            auto rect = ResolveItemRect(inv, item, displayW, displayH);
            if (!rect) continue;
            BagItem bi;
            bi.address = item.Address;
            bi.rect = *rect;
            bi.identified = item.IsIdentified;
            bi.rarity = item.Rarity;
            bi.path = item.Path;
            bi.baseType = item.BaseTypeName;
            bi.inventoryName = nameStr;
            out.push_back(std::move(bi));
        }
    }
    return out;
}

// 增强版 BagItem，包含面板名称用于诊断
struct FullBagItem {
    uintptr_t address = 0;
    ScreenRect rect;
    bool identified = false;
    int rarity = 0;
    std::string path;
    std::string baseType;
    std::string inventoryName;
    bool matchesCurrentFilter = false;
    std::string matchDetail;
};

// 完整扫描：返回所有可见面板的物品 + 匹配详情（用于调试窗口）
inline std::vector<FullBagItem> FullScanAllInventories(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings)
{
    std::vector<FullBagItem> out;
    if (!ctx) return out;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        std::string nameStr = name ? name : "(null)";
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;

        for (const auto& item : inv.Items) {
            auto rect = ResolveItemRect(inv, item, displayW, displayH);
            
            FullBagItem bi;
            bi.address = item.Address;
            bi.identified = item.IsIdentified;
            bi.rarity = item.Rarity;
            bi.path = item.Path;
            bi.baseType = item.BaseTypeName;
            bi.inventoryName = nameStr;
            
            if (rect) {
                bi.rect = *rect;
            }
            
            bi.matchesCurrentFilter = MatchesDesiredReforgeTypeEx(
                item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, settings);
            
            bi.matchDetail = TabletReforgeGame::DebugMatchDetail(
                item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, settings);
            
            out.push_back(std::move(bi));
        }
    }
    return out;
}

// 仅扫描背包类面板（排除仓库和装备面板）的完整诊断
inline std::vector<FullBagItem> FullScanBagLikeInventories(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings)
{
    std::vector<FullBagItem> out;
    if (!ctx) return out;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        
        // 主背包始终扫描
        if (!(name && std::strncmp(name, "MainInventory", 12) == 0)) {
            if (!IsBagLikeInventory(inv, name)) continue;
        }

        std::string nameStr = name ? name : "(unknown)";

        for (const auto& item : inv.Items) {
            auto rect = ResolveItemRect(inv, item, displayW, displayH);
            
            FullBagItem bi;
            bi.address = item.Address;
            bi.identified = item.IsIdentified;
            bi.rarity = item.Rarity;
            bi.path = item.Path;
            bi.baseType = item.BaseTypeName;
            bi.inventoryName = nameStr;
            
            if (rect) {
                bi.rect = *rect;
            }
            
            bi.matchesCurrentFilter = MatchesDesiredReforgeTypeEx(
                item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, settings);
            
            bi.matchDetail = TabletReforgeGame::DebugMatchDetail(
                item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, settings);
            
            out.push_back(std::move(bi));
        }
    }
    return out;
}

// 获取当前所有可见面板的摘要信息（用于调试窗口显示面板列表）
struct InventoryPanelInfo {
    int inventoryId = 0;
    std::string name;
    int totalBoxesX = 0;
    int totalBoxesY = 0;
    int itemCount = 0;
    bool isBagLike = false;
    bool isStashLike = false;
    bool isBenchLike = false;
    bool gridValid = false;
};

inline std::vector<InventoryPanelInfo> GetAllInventoryPanels(const PluginSDK::Context* ctx) {
    std::vector<InventoryPanelInfo> out;
    if (!ctx) return out;

    for (const auto& inv : ctx->Inventory.GetAll()) {
        InventoryPanelInfo info;
        info.inventoryId = inv.InventoryId;
        info.totalBoxesX = inv.TotalBoxesX;
        info.totalBoxesY = inv.TotalBoxesY;
        info.itemCount = static_cast<int>(inv.Items.size());
        info.gridValid = inv.Grid.Valid;
        info.name = ctx->Inventory.GetName(inv.InventoryId) ? ctx->Inventory.GetName(inv.InventoryId) : "(null)";

        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        const bool isMainInv = (info.name.rfind("MainInventory", 0) == 0);

        if (isMainInv) {
            info.isBagLike = true;
        } else if (slots >= 40) {
            info.isStashLike = true;
        } else if (slots >= 4 && slots <= 24 && inv.Items.empty()) {
            info.isBenchLike = true;
        } else {
            info.isBagLike = IsBagLikeInventory(inv, info.name.c_str());
        }

        out.push_back(std::move(info));
    }
    return out;
}

// 完整物品识别诊断：扫描所有面板（包括仓库和背包），返回每个物品的识别详情
// 用于调试窗口的"全物品识别诊断"功能
struct IdentifiedItemInfo {
    uintptr_t address = 0;
    std::string inventoryName;
    int rarity = 0;
    bool identified = false;
    bool hasPath = false;
    bool hasBT = false;
    std::string path;
    std::string baseType;
    std::string shortName;
    bool isWaystone = false;
    bool isPrecursorTablet = false;
    bool isTempleTablet = false;
    bool isJewel = false;
    bool isRune = false;
    bool isEssence = false;
    bool isLiquid = false;
    bool isCatalyst = false;
    bool matchesCurrentType = false;
    std::string matchReason;
};

inline std::vector<IdentifiedItemInfo> ScanAndIdentifyAllItems(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings,
    bool scanAllPanels = true)
{
    std::vector<IdentifiedItemInfo> out;
    if (!ctx) return out;

    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    int filteredEquipSlots = 0;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* nameC = ctx->Inventory.GetName(inv.InventoryId);
        std::string invName = nameC ? nameC : "(null)";

        // 综合过滤：装备槽位 + 非仓库Tab的 Inventory_NNN（基于 ggpk 格子尺寸数据）
        // 这一步确保只有真正的仓库Tab和主背包的物品被识别为原料，
        // 不会把角色身上的戒指、武器等装备误识别为仓库原料。
        if (IsNonStashInventory(invName, inv.TotalBoxesX, inv.TotalBoxesY, inv.InventoryId)) {
            ++filteredEquipSlots;
            continue;
        }

        if (!scanAllPanels) {
            if (!IsBagLikeInventory(inv, nameC)) continue;
        }

        for (const auto& item : inv.Items) {
            IdentifiedItemInfo info;
            info.address = item.Address;
            info.inventoryName = invName;
            info.rarity = item.Rarity;
            info.identified = item.IsIdentified;
            info.hasPath = !item.Path.empty();
            info.hasBT = !item.BaseTypeName.empty();
            info.path = item.Path;
            info.baseType = item.BaseTypeName;

            if (!item.Path.empty()) {
                size_t slash = item.Path.rfind('/');
                info.shortName = (slash == std::string::npos) ? item.Path : item.Path.substr(slash + 1);
            } else if (!item.BaseTypeName.empty()) {
                info.shortName = item.BaseTypeName;
            }

            info.isWaystone = IsWaystone(item.Path, item.BaseTypeName);
            info.isPrecursorTablet = IsPrecursorTablet(item.Path, item.BaseTypeName);
            info.isTempleTablet = IsTempleTablet(item.Path, item.BaseTypeName);
            info.isJewel = IsJewel(item.Path, item.BaseTypeName);
            info.isRune = IsRune(item.Path, item.BaseTypeName);
            info.isEssence = IsEssence(item.Path, item.BaseTypeName);
            info.isLiquid = IsLiquidEmotion(item.Path, item.BaseTypeName);
            info.isCatalyst = IsCatalyst(item.Path, item.BaseTypeName);

            info.matchesCurrentType = MatchesDesiredReforgeTypeEx(
                item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, settings);

            if (!info.matchesCurrentType) {
                if (info.hasPath || info.hasBT) {
                    info.matchReason = "有数据但未匹配任何已知模式";
                } else {
                    info.matchReason = "无Path/BaseType数据，尝试宽松回退";
                    bool wouldMatch = false;
                    if (!info.identified && info.rarity == 0) {
                        auto type = static_cast<ReforgeItemType>(settings.itemType);
                        if (type == ReforgeItemType::TabletsOnly || type == ReforgeItemType::AllTablets)
                            wouldMatch = true;
                        else if (type == ReforgeItemType::WaystonesOnly)
                            wouldMatch = true;
                    }
                    info.matchReason += wouldMatch ? "→ 宽松回退可能匹配" : "→ 宽松回退也不匹配";
                }
            } else {
                info.matchReason = "✓ 匹配成功";
            }

            out.push_back(std::move(info));
        }
    }
    return out;
}

// 生成物品识别摘要文本
inline std::string GenerateIdentificationSummary(
    const std::vector<IdentifiedItemInfo>& items,
    const TabletReforgeConfig::Settings& settings)
{
    std::string result;
    char buf[256];

    int total = static_cast<int>(items.size());
    int matched = 0;
    int withPath = 0;
    int withBT = 0;
    int noData = 0;
    int waystoneCount = 0;
    int precursorCount = 0;
    int jewelCount = 0;
    int runeCount = 0;
    int essenceCount = 0;
    int liquidCount = 0;
    int catalystCount = 0;

    for (const auto& it : items) {
        if (it.matchesCurrentType) matched++;
        if (it.hasPath) withPath++;
        if (it.hasBT) withBT++;
        if (!it.hasPath && !it.hasBT) noData++;
        if (it.isWaystone) waystoneCount++;
        if (it.isPrecursorTablet) precursorCount++;
        if (it.isJewel) jewelCount++;
        if (it.isRune) runeCount++;
        if (it.isEssence) essenceCount++;
        if (it.isLiquid) liquidCount++;
        if (it.isCatalyst) catalystCount++;
    }

    ::sprintf_s(buf, "总物品=%d  匹配=%d  有Path=%d  有BT=%d  无数据=%d",
        total, matched, withPath, withBT, noData);
    result += buf;

    ::sprintf_s(buf, "\n分类: Waystone=%d Precursor=%d Jewel=%d Rune=%d Essence=%d Liquid=%d Catalyst=%d",
        waystoneCount, precursorCount, jewelCount, runeCount, essenceCount, liquidCount, catalystCount);
    result += buf;

    ::sprintf_s(buf, "\n设置: itemType=%d requireIdentified=%d",
        settings.itemType, settings.requireIdentified ? 1 : 0);
    result += buf;

    return result;
}

// 查找背包中未鉴定且需要鉴定的物品
// 用于触发NPC鉴定流程：当合成产物未鉴定且符合鉴定条件时
struct UnidentifiedBagItem {
    uintptr_t address = 0;
    ScreenRect rect;
    int rarity = 0;
    std::string path;
    std::string baseType;
    bool needsIdentify = false;  // 是否需要NPC鉴定（魔法/稀有碑牌）
};

inline std::vector<UnidentifiedBagItem> FindUnidentifiedItemsInBag(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings)
{
    std::vector<UnidentifiedBagItem> out;
    if (!ctx) return out;

    auto all = CollectBagItems(ctx);
    for (const auto& bi : all) {
        // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
        int rarity = bi.rarity;
        bool identified = bi.identified;
        
        if (bi.address != 0) {
            auto itemMods = ctx->Inventory.ReadItemMods(bi.address);
            if (itemMods.Valid) {
                if (itemMods.Rarity > 0) rarity = itemMods.Rarity;
                identified = itemMods.IsIdentified;
            }
        }
        
        if (identified) continue;  // 已鉴定的跳过

        UnidentifiedBagItem ui;
        ui.address = bi.address;
        ui.rect = bi.rect;
        ui.rarity = rarity;
        ui.path = bi.path;
        ui.baseType = bi.baseType;
        ui.needsIdentify = NeedsIdentification(rarity, bi.path);

        out.push_back(std::move(ui));
    }
    return out;
}

// 统计背包中需要鉴定的未鉴定物品数量
inline int CountUnidentifiedNeedingIdentify(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings)
{
    int count = 0;
    auto items = FindUnidentifiedItemsInBag(ctx, settings);
    for (const auto& item : items) {
        if (item.needsIdentify) count++;
    }
    return count;
}

// 统计背包中所有未鉴定物品数量
inline int CountUnidentifiedInBag(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings)
{
    auto items = FindUnidentifiedItemsInBag(ctx, settings);
    return static_cast<int>(items.size());
}

} // namespace TabletReforgeGame