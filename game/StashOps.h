// StashOps.h — 仓库扫描与取物/存物坐标查询
//
// 复用 tablet-helper 的 TabletScanner 扫描逻辑，简化为"找合成材料"。
//
// 核心函数：
//   - FindTabletStash：在所有 inventory 中找含有先行者碑牌的仓库页（非主背包）
//   - NextTempleTabletInStash：仓库里下一个可取出的先行者碑牌的屏幕坐标
//   - NextUnidentifiedInStash：仓库里下一个未鉴定先行者碑牌（其实仓库初始没有，存回后才有）
//
// 取物/存物操作本身在状态机里用 Ctrl+左键完成，这里只负责"告诉状态机点哪里"。
//
// 安全：只用零风险字段。Scan(-1) 触发刷新，GetAll() 读取。
#pragma once

#include "PanelDetector.h"
#include "TabletFilter.h"
#include "UiTreeWalker.h"
#include "VisionRecognizer.h"
#include "StashItemMapper.h"
#include "../input/Win32Input.h"
#include "../sdk/PluginSDK.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TabletReforgeGame {

// 仓库里的一个可合成物品（屏幕坐标 + 识别结果，用于决定取不取）
struct StashTablet {
    ScreenRect rect;
    bool isMaterial = false;       // 是否属于当前选择的原料类型（MatchesDesiredReforgeTypeEx）
    bool isProductType = false;    // 是否属于产物类型（MatchesDesiredProductTypeEx，用于存回判断）
    int  rarity = 0;               // 原始 Rarity
    bool identified = false;       // 是否已鉴定
    int  slotX = 0;                // 格子 X 坐标（用于从左至右从上至下排序）
    int  slotY = 0;                // 格子 Y 坐标
    int  stackCount = 1;           // 堆叠数量（催化剂/精髓等可堆叠物品，1=非堆叠）
    std::string path;
    std::string baseType;
    // 【方案 B v1.3】合规词缀 Id（替代旧的 modNames/modAffixes/modStatKeys）
    // 仅在 enableBonusMatch=true 时通过 ExtractModIds 填充
    std::vector<std::string> modIds;     // Mod.Id（已白名单过滤）
    std::vector<uint32_t>    modHashes;  // Mod.Hash32（与 modIds 一一对应）
};

// 触发仓库刷新（Scan(-1) 扫描所有仓库页）
inline void RefreshStash(const PluginSDK::Context* ctx) {
    if (ctx) ctx->Inventory.Scan(-1);
}

// 仓库页信息（用于 UI 展示和用户选择）
struct StashTabInfo {
    int inventoryId = 0;
    std::string name;
    int slots = 0;          // 格子总数
    int itemCount = 0;      // 当前物品数量
    bool onScreen = false;  // 是否在屏幕上可见
};

// 列出所有可见的仓库页（供 UI 勾选使用）
inline std::vector<StashTabInfo> ListStashTabs(const PluginSDK::Context* ctx) {
    std::vector<StashTabInfo> out;
    if (!ctx) return out;

    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    for (const auto& inv : ctx->Inventory.GetAll()) {
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 2) continue;  // 只显示有效仓库页（过滤极小面板）
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;

        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        StashTabInfo info;
        info.inventoryId = inv.InventoryId;
        info.name = name ? name : "(unknown)";
        info.slots = slots;
        info.itemCount = (int)inv.Items.size();
        info.onScreen = inv.Grid.Valid && GridOnScreen(inv, displayW, displayH);
        out.push_back(std::move(info));
    }
    return out;
}

// —— 仓库页类型识别 ——
enum class StashTabType : int {
    Unknown = 0,
    Normal,         // 普通仓库页
    Currency,       // 货币仓库页
    Fragment,       // 碎片/碑牌仓库页（含子页）
    Map,            // 地图/引路石仓库页
    Quad,           // 四方格（4格合并仓库）
    SubTab,         // 子页（Fragment下的子页等）
};

// 识别仓库页类型（基于名称和网格特征）
inline StashTabType IdentifyStashTabType(const PluginSDK::Inventory& inv, const char* name) {
    std::string n = name ? name : "";
    // 转为小写做匹配
    std::string lower;
    lower.reserve(n.size());
    for (char c : n) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    int slots = inv.TotalBoxesX * inv.TotalBoxesY;

    // 名称匹配优先
    if (lower.find("fragment") != std::string::npos ||
        lower.find("tablet") != std::string::npos ||
        lower.find("sentinel") != std::string::npos) {
        return StashTabType::Fragment;
    }
    if (lower.find("currency") != std::string::npos ||
        lower.find("coin") != std::string::npos ||
        lower.find("gold") != std::string::npos) {
        return StashTabType::Currency;
    }
    if (lower.find("map") != std::string::npos ||
        lower.find("waystone") != std::string::npos ||
        lower.find("waypoint") != std::string::npos) {
        return StashTabType::Map;
    }
    if (lower.find("quad") != std::string::npos ||
        lower.find("4-t") != std::string::npos) {
        return StashTabType::Quad;
    }
    if (lower.find("sub") != std::string::npos ||
        lower.find("page") != std::string::npos) {
        return StashTabType::SubTab;
    }

    // 网格大小兜底判断
    if (slots >= 20 && slots <= 64) return StashTabType::Normal;
    if (slots >= 64) return StashTabType::Quad;
    if (slots <= 6) return StashTabType::Fragment; // Fragment子页通常6格或更少

    return StashTabType::Unknown;
}

// 获取仓库页类型的中文描述
inline const char* StashTabTypeName(StashTabType t) {
    switch (t) {
        case StashTabType::Normal:   return "普通";
        case StashTabType::Currency:  return "货币";
        case StashTabType::Fragment: return "碎片/碑牌";
        case StashTabType::Map:      return "地图";
        case StashTabType::Quad:     return "四方格";
        case StashTabType::SubTab:   return "子页";
        default:                     return "未知";
    }
}

// 扩展的仓库页信息（包含类型识别和点击位置）
struct StashTabInfoEx {
    int inventoryId = 0;
    std::string name;
    StashTabType type = StashTabType::Unknown;
    int slots = 0;
    int itemCount = 0;
    bool onScreen = false;
    float clickX = 0;       // 点击该页Tab按钮的屏幕X坐标
    float clickY = 0;       // 点击该页Tab按钮的屏幕Y坐标
    bool clickable = false;  // 是否计算出了可点击位置
    std::vector<StashTabInfoEx> subTabs; // 嵌套子页（如Fragment下的页签）
};

// 计算仓库页Tab按钮的点击位置
// POE2仓库页Tab按钮通常在网格左上角上方，偏移量约为：
//   X = GridScreenX + 10~30 (左边留出边距)
//   Y = GridScreenY - 30~50 (在网格上方)
// 但不同Tab位置不同，这里提供通用计算
inline bool CalculateStashTabClickPos(const PluginSDK::Inventory& inv,
                                      StashTabType tabType,
                                      float& outX, float& outY) {
    if (!inv.Grid.Valid) return false;
    if (inv.Grid.GridScreenX <= 0 && inv.Grid.GridScreenY <= 0) return false;

    float gridLeft = inv.Grid.GridScreenX;
    float gridTop = inv.Grid.GridScreenY;
    float cellSize = inv.Grid.CellSize > 0 ? inv.Grid.CellSize : 50.0f;

    // 仓库页Tab按钮在网格上方，水平位置根据Tab索引偏移
    // 这里提供通用默认位置：网格左上角往上偏移35px
    outX = gridLeft + cellSize * 0.5f;  // 网格左边往右半个格
    outY = gridTop - 35.0f;              // 网格上方35px
    return true;
}

// 列出所有可见的仓库页（扩展版，含类型识别和点击位置）
inline std::vector<StashTabInfoEx> ListStashTabsEx(const PluginSDK::Context* ctx) {
    std::vector<StashTabInfoEx> out;
    if (!ctx) return out;

    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    // 按类型分组：先找主仓库页（大页），再找子页
    std::vector<StashTabInfoEx> subTabs;

    for (const auto& inv : ctx->Inventory.GetAll()) {
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 2) continue;  // 过滤太小的面板

        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;

        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        StashTabType type = IdentifyStashTabType(inv, name);

        StashTabInfoEx info;
        info.inventoryId = inv.InventoryId;
        info.name = name ? name : "(unknown)";
        info.type = type;
        info.slots = slots;
        info.itemCount = (int)inv.Items.size();
        info.onScreen = inv.Grid.Valid && GridOnScreen(inv, displayW, displayH);

        // 计算点击位置
        info.clickable = CalculateStashTabClickPos(inv, type, info.clickX, info.clickY);

        // 区分主仓库页和子页
        if (type == StashTabType::SubTab || slots <= 6) {
            subTabs.push_back(std::move(info));
        } else if (slots >= 2) {
            out.push_back(std::move(info));
        }
    }

    // 将子页挂载到Fragment类型的主页下
    for (auto& tab : out) {
        if (tab.type == StashTabType::Fragment) {
            tab.subTabs = subTabs;
        }
    }

    // 如果没有Fragment类型的页，把子页直接加入列表末尾
    if (!subTabs.empty()) {
        bool hasFragment = false;
        for (const auto& tab : out) {
            if (tab.type == StashTabType::Fragment) { hasFragment = true; break; }
        }
        if (!hasFragment) {
            for (auto& sub : subTabs) {
                out.push_back(std::move(sub));
            }
        }
    }

    return out;
}

// 点击指定仓库页Tab（模拟鼠标点击）
// tabIndex: 在ListStashTabsEx结果中的索引
// 返回: 是否成功触发点击
inline bool ClickStashTab(const PluginSDK::Context* ctx, int tabIndex) {
    if (!ctx) return false;

    auto tabs = ListStashTabsEx(ctx);
    if (tabIndex < 0 || tabIndex >= (int)tabs.size()) return false;

    const auto& tab = tabs[tabIndex];
    if (!tab.clickable) return false;

    // 记录点击前的状态
    int prevId = tab.inventoryId;
    float prevGridX = 0, prevGridY = 0;

    // 点击前先扫描一次确认位置
    TabletReforgeInput::MoveCursorScreen(tab.clickX, tab.clickY);
    TabletReforgeInput::SleepMs(5);
    TabletReforgeInput::LeftClickAtCursor();
    TabletReforgeInput::SleepMs(200); // 等待UI切换

    // 点击后触发刷新
    ctx->Inventory.Scan(-1);
    TabletReforgeInput::SleepMs(100);

    return true;
}

// 点击指定inventoryId的仓库页（用于自动切换）
inline bool ClickStashTabById(const PluginSDK::Context* ctx, int inventoryId) {
    if (!ctx) return false;

    auto tabs = ListStashTabsEx(ctx);
    for (int i = 0; i < (int)tabs.size(); ++i) {
        if (tabs[i].inventoryId == inventoryId && tabs[i].clickable) {
            return ClickStashTab(ctx, i);
        }
    }
    return false;
}

// 查找所有可见的仓库页（支持手动配置的仓库页 + 勾选的仓库页）
// 优先使用编号仓库页配置（按slotIndex匹配），如果没有则使用 selectedStashTabIds
inline std::vector<PluginSDK::Inventory> FindVisibleStashesForSettings(
    const PluginSDK::Context* ctx,
    TabletReforgeConfig::Settings& settings) {
    std::vector<PluginSDK::Inventory> out;
    if (!ctx) return out;

    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    // 先确保编号仓库页配置已初始化
    settings.EnsureNumberedStashTabs();

    // 获取编号仓库页中标记为原料的 slotIndex 集合
    std::set<int> materialSlotIndices = settings.GetMaterialSlotIndices();

    // 如果有编号配置的原料页，使用 slotIndex 匹配
    if (!materialSlotIndices.empty()) {
        // 收集所有非主背包的可见inventory，按屏幕位置排序以确定slotIndex
        struct InvCandidate {
            PluginSDK::Inventory inv;
            float sortY, sortX;
            int origIdx;
        };

        std::vector<InvCandidate> candidates;
        int origIdx = 0;
        for (const auto& inv : ctx->Inventory.GetAll()) {
            if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;
            if (!inv.Grid.Valid) continue;
            int slots = inv.TotalBoxesX * inv.TotalBoxesY;
            if (slots < 2) continue;

            InvCandidate c;
            c.inv = inv;
            c.sortY = inv.Grid.GridScreenY > 0 ? inv.Grid.GridScreenY : 99999.f;
            c.sortX = inv.Grid.GridScreenX > 0 ? inv.Grid.GridScreenX : 99999.f;
            c.origIdx = origIdx++;
            candidates.push_back(std::move(c));
        }

        // 按屏幕位置排序
        std::sort(candidates.begin(), candidates.end(),
            [](const InvCandidate& a, const InvCandidate& b) {
                if (std::fabs(a.sortY - b.sortY) > 10.f) return a.sortY < b.sortY;
                return a.sortX < b.sortX;
            });

        // 分配slotIndex（按排序顺序），并根据配置过滤
        int slotIdx = 1;
        for (const auto& c : candidates) {
            bool onScreen = GridOnScreen(c.inv, displayW, displayH);
            bool isMaterial = settings.IsMaterialTabBySlot(slotIdx);

            if (isMaterial && onScreen) {
                out.push_back(c.inv);
            }

            // 回填扫描信息到配置
            auto* cfg = settings.FindNumberedStashTabBySlot(slotIdx);
            if (cfg) {
                cfg->inventoryId = c.inv.InventoryId;
                const char* invName = ctx->Inventory.GetName(c.inv.InventoryId);
                cfg->detectedLabel = invName ? invName : "";
            }

            slotIdx++;
        }

        return out;
    }

    // 回退：使用旧的 selectedStashTabIds（基于 inventoryId）
    std::set<int> materialIds = settings.selectedStashTabIds;

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        if (inv.TotalBoxesX * inv.TotalBoxesY < 2) continue;
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;
        if (!GridOnScreen(inv, displayW, displayH)) continue;

        if (!materialIds.empty() && !materialIds.count(inv.InventoryId)) {
            continue;
        }
        out.push_back(inv);
    }
    return out;
}

// —— 基于UI树文本识别仓库页Tab按钮 ——
// 通过 CollectVisible() 扫描UI树，找到仓库页Tab按钮的真实屏幕坐标和文本标签
// 这比硬编码偏移更可靠，能正确定位不同位置的Tab按钮

// Tab按钮描述：从UI树中提取的Tab信息
struct StashTabButton {
    std::string label;       // Tab上显示的文字（如 "Fragment", "Currency" 等）
    std::string stringId;    // Tab的StringId
    float x = 0, y = 0;      // 屏幕坐标（中心点）
    float w = 0, h = 0;      // 尺寸
    int tabIndex = -1;       // 在所有Tab中的索引
    bool isSubTab = false;   // 是否为子页Tab
};

// ============================================================
// ★ 修复：更严格的Tab按钮判断 + 黑名单过滤
//   1. 增加stringId/text的黑名单（排除对话/物品名等误匹配）
//   2. Tab按钮必须在屏幕上半部分（Y在顶部40%区域内）
//   3. 尺寸严格限制在 Tab 典型大小范围内（20<w<200, 10<h<50）
// ============================================================
inline bool IsStashTabButton(const UiNodeInfo& node, float displayH = 1080.f) {
    // ★ 黑名单：stringId或text中出现这些词的绝对不是Tab按钮
    static const char* kBlacklist[] = {
        "dialog", "Dialog", "conversation", "Conversation", "quest", "Quest",
        "tooltip", "Tooltip", "description", "Description", "label", "Label",
        "item_name", "ItemName", "item_desc", "ItemDesc", "message", "Message",
        "notification", "Notification", "chat", "Chat", "minimap", "Minimap",
        "buff", "Buff", "debuff", "Debuff", "health", "Health", "mana", "Mana",
        "flask", "Flask", "potion", "Potion", "skill", "Skill", "passive", "Passive",
        "talent", "Talent", "tree", "Tree", "atlas", "Atlas", "league", "League",
        "setting", "Setting", "option", "Option", "menu", "Menu", "panel_title",
        "inventory_slot", "Equipment", "equipment", "Weapon", "weapon", "Armour",
        "Helm", "Gloves", "Boots", "Ring", "Amulet", "Belt",
    };
    for (const char* bl : kBlacklist) {
        if (!node.stringId.empty() && node.stringId.find(bl) != std::string::npos) return false;
        if (!node.text.empty() && node.text.find(bl) != std::string::npos) return false;
    }

    // ★ 放宽位置过滤：Tab按钮必须在屏幕 8%~62% 高度范围内
    //   8%以下：顶栏/标题栏；62%以上：仓库面板内部
    //   原0.45H太保守，玩家1920x1080分辨率下Tab栏可能在Y=600（约56%H）附近
    if (node.y < displayH * 0.08f || node.y > displayH * 0.62f) {
        return false;
    }

    // ★ 放宽尺寸过滤：从20-200x10-50 → 12-260x8-70
    //   原尺寸太小：有些Tab图标的图标部分只有15x15或大Tab标签有230宽
    if (node.w < 12.f || node.w > 260.f) return false;
    if (node.h < 8.f  || node.h > 70.f)  return false;

    // Tab按钮的stringId通常包含这些关键词
    static const char* kTabStringIdPatterns[] = {
        "stash_tab", "tab_button", "inventory_tab", "page_tab",
        "stash_page", "tab_bar", "inventory_page", "tabbtn",
        "StashTab", "TabButton", "StashPage", "TabBtn",
    };
    
    // Tab按钮的文本可能包含这些关键词（用中英文Tab典型名称，长度3~16字）
    static const char* kTabTextPatterns[] = {
        "Fragment", "Tablet", "Sentinel", "Currency",
        "Map", "Waystone", "Waypoint", "Quad", "Stash",
        "Normal", "Premium", "Trade", "Unique", "Essence",
        "Delve", "Blight", "Metamorph", "Delirium",
        "Flask", "Gem", "Divination", "Abyss", "Ultimatum",
        "碎片", "碑牌", "货币", "地图", "仓库", "普通",
        "高级", "预言", "精华", "裂隙", "花园", "记忆",
    };
    
    // 先检查stringId
    for (const char* pat : kTabStringIdPatterns) {
        if (!node.stringId.empty() && 
            node.stringId.find(pat) != std::string::npos) {
            return true;
        }
    }
    
    // 再检查text（但要排除太长的文本→Tab名字通常短于16字符）
    if (!node.text.empty() && node.text.size() <= 32) {
        for (const char* pat : kTabTextPatterns) {
            if (node.text.find(pat) != std::string::npos) {
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================
// ★ 修复3(多行版): 从UI树中提取仓库页Tab按钮列表 - 支持多行Tab栏布局
//   POE2仓库页Tab可能分多行排列（如上面3个特殊Tab+下面8个碑牌Tab）
//   所有行都被保留并按视觉顺序合并输出
// ============================================================
inline std::vector<StashTabButton> ExtractStashTabButtons(const PluginSDK::Context* ctx) {
    std::vector<StashTabButton> out;
    if (!ctx) return out;
    
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);
    
    auto nodes = TabletReforgeGame::CollectVisible(ctx, 14, 2500, 6.f, false);
    
    char log[512];
    sprintf_s(log, "[StashOps] ExtractStashTabButtons: 扫描UI树%zu个节点, 屏高=%.0f (Tab需Y在%.0f~%.0f)\n",
        nodes.size(), displayH, displayH * 0.08f, displayH * 0.62f);
    OutputDebugStringA(log);
    
    struct Candidate {
        float x, y, w, h;
        std::string label, stringId;
        bool isSubTab;
    };
    std::vector<Candidate> cands;
    
    int rejectedBlacklist = 0, rejectedPosY = 0, rejectedSize = 0, rejectedNoPattern = 0;
    for (const auto& node : nodes) {
        bool passBL = true, passY = true, passSz = true;
        static const char* kBL[] = {"dialog","Dialog","conversation","Conversation","quest","Quest",
            "tooltip","Tooltip","description","Description","label","Label",
            "item_name","ItemName","item_desc","ItemDesc","message","Message",
            "notification","Notification","chat","Chat","minimap","Minimap",
            "buff","Buff","debuff","Debuff","health","Health","mana","Mana",
            "flask","Flask","potion","Potion","skill","Skill","passive","Passive",
            "talent","Talent","tree","Tree","atlas","Atlas","league","League",
            "setting","Setting","option","Option","menu","Menu","panel_title",
            "inventory_slot","Equipment","equipment","Weapon","weapon","Armour",
            "Helm","Gloves","Boots","Ring","Amulet","Belt"};
        for (const char* bl : kBL) {
            if ((!node.stringId.empty() && node.stringId.find(bl) != std::string::npos) ||
                (!node.text.empty() && node.text.find(bl) != std::string::npos)) {
                passBL = false; break;
            }
        }
        if (!passBL) { rejectedBlacklist++; continue; }
        if (node.y < displayH * 0.08f || node.y > displayH * 0.62f) { rejectedPosY++; continue; }
        if (node.w < 12.f || node.w > 260.f || node.h < 8.f || node.h > 70.f) { rejectedSize++; continue; }
        if (!IsStashTabButton(node, displayH)) { rejectedNoPattern++; continue; }
        
        Candidate c;
        c.label = node.text;
        c.stringId = node.stringId;
        c.x = node.x + node.w * 0.5f;
        c.y = node.y + node.h * 0.5f;
        c.w = node.w;
        c.h = node.h;
        c.isSubTab = node.w < 60.f;
        cands.push_back(c);
    }
    
    sprintf_s(log, "[StashOps] Extract: 候选%zu个 过滤统计: 黑名单拒%d 位置拒%d(Y<8%%或>62%%) 尺寸拒%d 模式拒%d\n",
        cands.size(), rejectedBlacklist, rejectedPosY, rejectedSize, rejectedNoPattern);
    OutputDebugStringA(log);
    
    if (cands.empty()) return out;
    if (cands.size() < 2) {
        sprintf_s(log, "[StashOps] ExtractStashTabButtons: 候选仅%zu个(<2)，返回空避免误判\n", cands.size());
        OutputDebugStringA(log);
        return out;
    }
    
    // ★ 多行分组：将候选按Y坐标聚类（Y差<=30px为同一行）
    //   使用简单的聚类算法：按Y排序后依次归入行
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        return a.y < b.y;
    });
    
    struct RowGroup {
        std::vector<Candidate> members;
        float refY = 0;
    };
    std::vector<RowGroup> rows;
    
    for (const auto& c : cands) {
        if (rows.empty()) {
            RowGroup r;
            r.refY = c.y;
            r.members.push_back(c);
            rows.push_back(std::move(r));
        } else {
            bool placed = false;
            for (auto& row : rows) {
                if (std::abs(c.y - row.refY) <= 30.f) {
                    row.members.push_back(c);
                    // 更新 refY 为成员平均
                    float sumY = 0;
                    for (const auto& m : row.members) sumY += m.y;
                    row.refY = sumY / (float)row.members.size();
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                RowGroup r;
                r.refY = c.y;
                r.members.push_back(c);
                rows.push_back(std::move(r));
            }
        }
    }
    
    // 过滤掉只有1个成员的行（可能是误匹配），但保留所有>=2的行
    std::vector<RowGroup> validRows;
    int discardedSingle = 0;
    for (auto& row : rows) {
        if (row.members.size() >= 2) {
            validRows.push_back(std::move(row));
        } else {
            discardedSingle++;
        }
    }
    
    sprintf_s(log, "[StashOps] ExtractStashTabButtons: 候选%zu个 → %zu行 (有效%zu行, 丢弃%d个单行) Y阈值30\n",
        cands.size(), rows.size(), validRows.size(), discardedSingle);
    OutputDebugStringA(log);
    
    if (validRows.empty()) {
        OutputDebugStringA("[StashOps] ExtractStashTabButtons: 没有有效的多行组，返回空\n");
        return out;
    }
    
    // 合并所有有效行的按钮，按视觉顺序（Y从小到大，同一行内X从小到大）
    int globalIdx = 0;
    for (auto& row : validRows) {
        // 每行内按X排序（从左到右）
        std::sort(row.members.begin(), row.members.end(), 
            [](const Candidate& a, const Candidate& b) {
                return a.x < b.x;
            });
        
        // 检查是否横向递增（允许少量误差）
        bool increasing = true;
        for (size_t i = 1; i < row.members.size(); ++i) {
            if (row.members[i].x < row.members[i-1].x - 5.f) {
                increasing = false; break;
            }
        }
        if (!increasing) {
            OutputDebugStringA("[StashOps] 警告: 行内Tab按钮X非递增，已强制排序\n");
        }
        
        for (size_t i = 0; i < row.members.size(); ++i) {
            StashTabButton btn;
            btn.tabIndex = globalIdx++;
            btn.label = row.members[i].label;
            btn.stringId = row.members[i].stringId;
            btn.x = row.members[i].x;
            btn.y = row.members[i].y;
            btn.w = row.members[i].w;
            btn.h = row.members[i].h;
            btn.isSubTab = row.members[i].isSubTab;
            out.push_back(std::move(btn));
        }
    }
    
    // 打印每行详细信息
    for (size_t ri = 0; ri < validRows.size(); ++ri) {
        const auto& row = validRows[ri];
        char rlog[512];
        sprintf_s(rlog, "[StashOps]   行%zu: %zu个按钮 Y≈%.0f X范围[%.0f,%.0f]\n",
            ri, row.members.size(), row.refY,
            row.members.front().x, row.members.back().x);
        OutputDebugStringA(rlog);
    }
    
    sprintf_s(log, "[StashOps] ExtractStashTabButtons: 共返回%zu个Tab按钮 (多行合并)\n", out.size());
    OutputDebugStringA(log);
    return out;
}

// 基于UI树文本和结构特征识别仓库页类型（比单纯名称匹配更可靠）
// 结合：stringId + text + 网格大小 + 子页结构
inline StashTabType IdentifyStashTabTypeFromUi(
    const PluginSDK::Context* ctx,
    const PluginSDK::Inventory& inv, 
    const std::vector<StashTabButton>& buttons,
    int buttonIndex) {
    
    // 先尝试传统的基于名称识别（通过InventoryId获取名称）
    const char* name = ctx ? ctx->Inventory.GetName(inv.InventoryId) : nullptr;
    StashTabType baseType = IdentifyStashTabType(inv, name);
    if (baseType != StashTabType::Unknown && baseType != StashTabType::Normal) {
        return baseType;
    }
    
    // 基于UI树按钮文本识别
    if (buttonIndex >= 0 && buttonIndex < (int)buttons.size()) {
        const auto& btn = buttons[buttonIndex];
        std::string lowerLabel = btn.label;
        for (char& c : lowerLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string lowerId = btn.stringId;
        for (char& c : lowerId) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        
        // 碎片/碑牌
        if (lowerLabel.find("fragment") != std::string::npos ||
            lowerLabel.find("tablet") != std::string::npos ||
            lowerLabel.find("sentinel") != std::string::npos ||
            lowerId.find("fragment") != std::string::npos ||
            lowerId.find("tablet") != std::string::npos) {
            return StashTabType::Fragment;
        }
        
        // 货币
        if (lowerLabel.find("currency") != std::string::npos ||
            lowerLabel.find("coin") != std::string::npos ||
            lowerLabel.find("gold") != std::string::npos ||
            lowerId.find("currency") != std::string::npos ||
            lowerId.find("coin") != std::string::npos) {
            return StashTabType::Currency;
        }
        
        // 地图/引路石
        if (lowerLabel.find("map") != std::string::npos ||
            lowerLabel.find("waystone") != std::string::npos ||
            lowerLabel.find("waypoint") != std::string::npos ||
            lowerId.find("waystone") != std::string::npos ||
            lowerId.find("map") != std::string::npos) {
            return StashTabType::Map;
        }
        
        // 四方格
        if (lowerLabel.find("quad") != std::string::npos ||
            lowerLabel.find("4-t") != std::string::npos ||
            lowerId.find("quad") != std::string::npos) {
            return StashTabType::Quad;
        }
        
        // 子页
        if (btn.isSubTab) {
            return StashTabType::SubTab;
        }
    }
    
    // 网格大小兜底
    int slots = inv.TotalBoxesX * inv.TotalBoxesY;
    if (slots >= 20 && slots <= 64) return StashTabType::Normal;
    if (slots >= 64) return StashTabType::Quad;
    if (slots <= 6) return StashTabType::Fragment;

    return StashTabType::Unknown;
}

// 增强版ListStashTabsEx：结合UI树文本识别，获取精确点击坐标
inline std::vector<StashTabInfoEx> ListStashTabsExV2(const PluginSDK::Context* ctx) {
    std::vector<StashTabInfoEx> out;
    if (!ctx) return out;

    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    // 先尝试从UI树获取Tab按钮的精确位置
    auto tabButtons = ExtractStashTabButtons(ctx);
    bool hasUiTreeButtons = !tabButtons.empty();
    
    // 构建inventoryId到按钮索引的映射（基于名称匹配）
    std::unordered_map<int, int> invToButtonIdx;
    
    // 如果有UI树按钮，尝试匹配inventory
    if (hasUiTreeButtons) {
        int buttonIdx = 0;
        for (const auto& inv : ctx->Inventory.GetAll()) {
            const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
            if (slots < 6) continue;
            if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;
            
            const char* invName = ctx->Inventory.GetName(inv.InventoryId);
            std::string invNameLower = invName ? invName : "";
            for (char& c : invNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            
            // 匹配按钮文本
            for (int b = buttonIdx; b < (int)tabButtons.size(); ++b) {
                std::string btnLower = tabButtons[b].label;
                for (char& c : btnLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                
                if (!btnLower.empty() && !invNameLower.empty() &&
                    (btnLower.find(invNameLower) != std::string::npos ||
                     invNameLower.find(btnLower) != std::string::npos)) {
                    invToButtonIdx[inv.InventoryId] = b;
                    buttonIdx = b + 1;
                    break;
                }
            }
        }
    }

    // 按类型分组
    std::vector<StashTabInfoEx> subTabs;

    for (const auto& inv : ctx->Inventory.GetAll()) {
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 6) continue;

        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;

        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        StashTabType type;
        
        // 如果有UI树按钮，用增强版识别
        auto btnIt = invToButtonIdx.find(inv.InventoryId);
        if (btnIt != invToButtonIdx.end()) {
            type = IdentifyStashTabTypeFromUi(ctx, inv, tabButtons, btnIt->second);
        } else {
            type = IdentifyStashTabType(inv, name);
        }

        StashTabInfoEx info;
        info.inventoryId = inv.InventoryId;
        info.name = name ? name : "(unknown)";
        info.type = type;
        info.slots = slots;
        info.itemCount = (int)inv.Items.size();
        info.onScreen = inv.Grid.Valid && GridOnScreen(inv, displayW, displayH);

        // 计算点击位置：优先使用UI树按钮坐标
        if (btnIt != invToButtonIdx.end()) {
            const auto& btn = tabButtons[btnIt->second];
            info.clickX = btn.x;
            info.clickY = btn.y;
            info.clickable = true;
        } else {
            info.clickable = CalculateStashTabClickPos(inv, type, info.clickX, info.clickY);
        }

        if (type == StashTabType::SubTab || slots <= 6) {
            subTabs.push_back(std::move(info));
        } else if (slots >= 2) {
            out.push_back(std::move(info));
        }
    }

    // 将子页挂载到Fragment类型的主页下
    for (auto& tab : out) {
        if (tab.type == StashTabType::Fragment) {
            tab.subTabs = subTabs;
        }
    }

    // 如果没有Fragment类型的页，把子页直接加入列表末尾
    if (!subTabs.empty()) {
        bool hasFragment = false;
        for (const auto& tab : out) {
            if (tab.type == StashTabType::Fragment) { hasFragment = true; break; }
        }
        if (!hasFragment) {
            for (auto& sub : subTabs) {
                out.push_back(std::move(sub));
            }
        }
    }

    return out;
}

// ============================================================
// 仓库页按排列顺序编号系统
// ============================================================
//
// POE2 仓库页按从上到下、从左到右的顺序排列：
//   Tab #1  = 第1个仓库页
//   Tab #2  = 第2个仓库页
//   ...
//   Tab #17 = 第17个仓库页
//
// 每个编号对应一个仓库页，不管它叫什么名字。
// 子页（如碎片仓库页下的小页）用独立编号。
// ============================================================

// 按屏幕位置排序的仓库页条目（用于编号映射）
struct OrderedStashTab {
    int slotIndex = 0;       // 1-based 编号
    int inventoryId = 0;     // 对应的 InventoryId
    std::string name;        // 仓库页名称
    StashTabType type = StashTabType::Unknown;
    int slots = 0;
    int gridWidth = 0;       // TotalBoxesX（用于诊断 Inventory_NNN 是否为真仓库Tab）
    int gridHeight = 0;      // TotalBoxesY
    float gridX = 0, gridY = 0;  // 网格左上角屏幕坐标
    float clickX = 0, clickY = 0; // Tab按钮点击坐标
    bool clickable = false;
    bool isVisible = false;   // 当前是否在屏幕上可见
    bool isSubTab = false;    // 是否为子页
    std::vector<OrderedStashTab> subTabs; // 子页列表
};

// ============================================================
// 装备槽位过滤说明：
// IsEquipmentSlotName 已移至 TabletFilter.h（namespace TabletReforgeGame），
// 以便 InventoryChecker.h 等基础头文件也能调用，统一过滤装备槽位。
// 调用方式保持不变：IsEquipmentSlotName(name)
// ============================================================

// Tab按钮信息（旧版结构，供 EnumerateStashTabButtons 等旧函数使用）
struct StashTabButtonInfo {
    int inventoryId = 0;
    std::string label;         // Tab上显示的文本
    std::string stringId;      // Tab的StringId
    float x = 0, y = 0;        // 屏幕坐标（中心点）
    float w = 0, h = 0;        // 尺寸
    int visualOrder = 0;       // 视觉排序（从上到下、从左到右）
    bool isSubTab = false;     // 是否为子页
    bool matched = false;      // 是否已匹配到 Inventory
};

// ============================================================
// 仓库Tab类型识别（参考 StashUtilityCore.cs IsActiveTab 函数）
// 通过 Tab 内部 UI 结构识别仓库类型，不依赖图标模板：
//   - Waystone tab: tab -> 0 -> 1 有 16 个子元素（16个地图层级）
//   - Fragment tab: tab -> 0 -> 0 -> 0 -> 1 有 6 个子元素（6个分页）
//   - Normal/Quad tab: tab -> 0 -> 0 可见且非零尺寸
// ============================================================
inline std::string DetectStashTabTypeByStructure(const PluginSDK::Context* ctx, uintptr_t tabAddr) {
    if (!ctx || !tabAddr) return "Unknown";

    // 1. 检查 Waystone tab: tab -> 0 -> 1 有 16 个子元素
    {
        int path1[] = {0, 1};
        uintptr_t child01 = ctx->Ui.FollowPath(tabAddr, path1, 2);
        if (child01) {
            auto elem = ctx->Ui.Read(child01);
            if (elem.Valid && elem.IsVisible && elem.ChildCount == 16) {
                return "WaystoneStash";
            }
        }
    }

    // 2. 检查 Fragment tab: tab -> 0 -> 0 -> 0 -> 1 有 6 个子元素
    {
        int pathFrag[] = {0, 0, 0, 1};
        uintptr_t childFrag = ctx->Ui.FollowPath(tabAddr, pathFrag, 4);
        if (childFrag) {
            auto elem = ctx->Ui.Read(childFrag);
            if (elem.Valid && elem.IsVisible && elem.ChildCount == 6) {
                return "FragmentStash";
            }
        }
    }

    // 3. 检查 Normal/Quad tab: tab -> 0 -> 0 可见且非零尺寸
    {
        int path00[] = {0, 0};
        uintptr_t child00 = ctx->Ui.FollowPath(tabAddr, path00, 2);
        if (child00) {
            auto elem = ctx->Ui.Read(child00);
            if (elem.Valid && elem.IsVisible && elem.UnscaledWidth > 0.f) {
                // 通过格子数区分 Normal vs Quad
                // Normal: 12x12=144, Quad: 24x24=576
                if (elem.ChildCount >= 400) return "QuadStash";
                return "NormalStash";
            }
        }
    }

    // 4. 其他专用仓库（Currency, Map, Essence 等）通过子元素数量粗略识别
    {
        int path0[] = {0};
        uintptr_t child0 = ctx->Ui.FollowPath(tabAddr, path0, 1);
        if (child0) {
            auto elem = ctx->Ui.Read(child0);
            if (elem.Valid && elem.IsVisible) {
                // 这些专用仓库的子元素数量特征（参考 stashtype.json Unk003 字段）
                switch (elem.ChildCount) {
                    case 41:  return "CurrencyStash";  // Unk003=41
                    case 94:  return "UniqueStash";    // Unk003=94
                    case 30:  return "EssenceStash";   // Unk003=30
                    case 152: return "FragmentStash";  // Unk003=152 (备用识别)
                    case 66:  return "BlightStash";    // Unk003=66
                    case 62:  return "MetamorphStash"; // Unk003=62
                    case 40:  return "DeliriumStash";  // Unk003=40
                    case 250: return "FlaskStash";     // Unk003=250 或 GemStash
                }
            }
        }
    }

    return "Unknown";
}

// ============================================================
// ============================================================
// 用 FollowPath 路径访问法查找仓库 Content-Panel 容器（StashTabsContainer）
//
// ★★★ 关键架构修正（参考 StashUtilityCore.cs L1108-1146）：
//   StashUtility 的 PathString = "2,0,0,0,1,1,45,0,1" (9级)，内部处理：
//     末尾是0,1且长度>=9 → stashTabsContainerPath = Take(Length-3) = "2,0,0,0,1,1,45" (7级)
//     末尾是0,0,0,1且长度>=12 → Take(Length-5) = 碎片仓库路径
//     否则默认 Take(6) = "2,0,0,0,1,1" (6级)
//
//   这个 stashTabsContainer 的**子元素不是Tab按钮**！而是每个Tab的 content-panel（内容面板），
//   数量≈Tab数量(比如12)，大小≈670x670（大面板）。
//   真正的Tab按钮栏在 content-panel 容器的**上方**，是回溯1层的兄弟节点。
//
// 【返回值语义变更】：
//   现在返回 StashContentPanelsContainer（内容面板列表容器），而不是Tab按钮栏容器。
//   后续 ListAllStashTabsOrdered 会用这个容器的：
//     a) 子元素索引  与 Inventory按可见性排序后的索引  对齐配对
//     b) 可见content-panel的屏幕位置 推算所有Tab按钮的坐标
// ============================================================
struct StashContentPanelsResult {
    uintptr_t containerAddr = 0;     // StashContentPanels容器地址（供调试）
    int        panelCount = 0;       // content-panel数量（≈Tab数量）
    uintptr_t visiblePanelAddr = 0;  // 当前可见content-panel地址
    float      visibleLeft = 0.f;    // 可见content-panel的屏幕左上角X
    float      visibleTop = 0.f;     // 可见content-panel的屏幕左上角Y
    float      visibleWidth = 0.f;   // 可见content-panel的宽度
    float      visibleHeight = 0.f;  // 可见content-panel的高度
    int        visibleIndex = -1;    // 可见content-panel在子元素中的索引
    uintptr_t tabBarAddr = 0;        // Tab按钮栏容器地址（找到的话，供枚举按钮）
};

inline StashContentPanelsResult FindStashContentPanelsContainer(const PluginSDK::Context* ctx) {
    StashContentPanelsResult result;
    if (!ctx) return result;

    uintptr_t gameUiRoot = ctx->Ui.GetGameUiRoot();
    if (!gameUiRoot) {
        OutputDebugStringA("[StashOps] FindStashContentPanelsContainer: GetGameUiRoot 返回 0\n");
        return result;
    }

    auto children = ctx->Ui.GetChildren(gameUiRoot);
    char log[512];
    sprintf_s(log, "[StashOps] FindStashContentPanelsContainer: gameUiRoot=0x%llX, 子元素数=%zu\n",
        (unsigned long long)gameUiRoot, children.size());
    OutputDebugStringA(log);

    // ============================================================
    // 内部辅助：检查一个元素是否就是 content-panel 列表容器
    //   bug1.log: child[35] 有 17 个子元素（都是670x670的大面板=content-panel）
    //   判定逻辑放宽：只要有 >=2 个子元素，且其中 >=1 个是大面板（>=300x300），
    //   或者有可见大面板，就认为命中。
    // ============================================================
    auto checkContentPanelsContainer = [&](uintptr_t c, const char* pathName) -> bool {
        if (!c) return false;
        auto e = ctx->Ui.Read(c);
        // ★ 放宽：ChildCount>=2 即可（旧版逻辑命中child[35]只有17子元素）
        if (!e.Valid || e.ChildCount < 2) return false;

        auto panels = ctx->Ui.GetChildren(c);
        if (panels.size() < 2) return false;

        // 统计大面板(>=300x300)数量 + 找出可见面板（IsVisible或屏幕位置合理即可）
        int largePanelCount = 0;
        uintptr_t visPanel = 0;
        int visIdx = -1;
        float visL=0, visT=0, visW=0, visH=0;

        for (size_t pi = 0; pi < panels.size() && pi < 100; ++pi) {
            uintptr_t p = panels[pi];
            if (!p) continue;
            float px=0, py=0, pw=0, ph=0;
            bool rectOk = ctx->Ui.ComputeScreenRect(p, px, py, pw, ph);
            // ★ 放宽：即使ComputeScreenRect失败，也可以通过UiElement本地尺寸（作为兜底）
            //   注意：UnscaledWidth/Height是本地未缩放尺寸，RelativeX/Y是相对父节点坐标
            //   这里只用来"判断是不是大面板"，不用于精确屏幕坐标
            if (!rectOk) {
                auto pe = ctx->Ui.Read(p);
                if (pe.Valid) {
                    pw = pe.UnscaledWidth;
                    ph = pe.UnscaledHeight;
                    px = pe.RelativeX;
                    py = pe.RelativeY;
                    // 本地尺寸如果是大面板(>=400x400)也算命中（不要求屏幕坐标合理）
                }
            }
            if (pw >= 300.f && ph >= 300.f) {
                largePanelCount++;
                auto pe = ctx->Ui.Read(p);
                // ★ 找可见/激活面板：IsVisible 或者 屏幕位置在合理范围内（X>100,Y>100）
                bool reasonablePos = (px > 100.f && py > 100.f && pw > 300.f && ph > 300.f
                                      && px < 4000.f && py < 3000.f);
                if (reasonablePos) {
                    // 取最大的那个面板作为激活Tab（可见面板通常是完整渲染的，尺寸最准）
                    if (!visPanel || (pw * ph > visW * visH)) {
                        visPanel = p;
                        visIdx = (int)pi;
                        visL = px; visT = py; visW = pw; visH = ph;
                    }
                }
            }
        }

        // ★★★ 判定放宽：
        //   - 大面板数>=2（典型情况：17个content-panel里有1+个大面板可见，其他不可见但尺寸对）
        //   - 或者：有可见大面板（即使largePanelCount=1，因为不可见面板的Rect可能取到0）
        //   - 或者：子元素数>=5（Tab数>=5的玩家，即使面板Rect取不到，数量级也对）
        if (largePanelCount >= 2 || visPanel || panels.size() >= 5) {
            result.containerAddr = c;
            result.panelCount = (int)panels.size();
            result.visiblePanelAddr = visPanel;
            result.visibleLeft = visL;
            result.visibleTop = visT;
            result.visibleWidth = visW;
            result.visibleHeight = visH;
            result.visibleIndex = visIdx;

            sprintf_s(log, "[StashOps] ✓ ContentPanel容器命中 path=%s addr=0x%llX panels=%zu large=%d visIdx=%d\n",
                pathName, (unsigned long long)c, panels.size(), largePanelCount, visIdx);
            OutputDebugStringA(log);
            if (visPanel) {
                sprintf_s(log, "[StashOps]   可见content-panel[%d]: pos=(%.0f,%.0f) size=(%.0f,%.0f)\n",
                    visIdx, visL, visT, visW, visH);
                OutputDebugStringA(log);
            } else {
                OutputDebugStringA("[StashOps]   （无可见面板坐标，将使用Inventory可见Grid推算）\n");
            }
            return true;
        }
        return false;
    };

    // ============================================================
    // ★★★ 策略A（最高优先级）：直接遍历 gameUiRoot 的所有子元素，
    //   对每个 child[i] 直接检查是否是 content-panel 容器。
    //   这是 bug1.log 旧版的命中方式：child[35] 有 17 子元素（17个content-panel）
    //   旧版只遍历所有child（不限制IsVisible！），child[35]可能没标IsVisible但子元素有
    // ============================================================
    OutputDebugStringA("[StashOps] FindStashContentPanelsContainer: 策略A=直接扫所有child[i]找content-panel容器...\n");
    for (size_t i = 0; i < children.size() && i < 250; ++i) {
        uintptr_t child = children[i];
        if (!child) continue;
        char tag[64];
        sprintf_s(tag, "child[%zu]-direct", i);
        if (checkContentPanelsContainer(child, tag)) {
            return result;  // 命中！返回
        }
    }

    // ============================================================
    // ★★★ 策略B（兜底）：对 gameUiRoot 做 FollowPath 尝试常见路径
    //   StashUtility风格的路径 {2,0,0,0,1,1, X } 其中 X 是索引
    // ============================================================
    OutputDebugStringA("[StashOps] FindStashContentPanelsContainer: 策略B=FollowPath路径尝试...\n");
    static const int base6[] = {2, 0, 0, 0, 1, 1};  // 6级基础路径
    // 批量尝试第7级索引 5-80（覆盖 child[35] 附近及更广泛范围）
    for (int idx7 = 5; idx7 <= 80; ++idx7) {
        int path7[7] = {base6[0], base6[1], base6[2], base6[3], base6[4], base6[5], idx7};
        uintptr_t c = ctx->Ui.FollowPath(gameUiRoot, path7, 7);
        if (c) {
            char tag[64];
            sprintf_s(tag, "path7-%d", idx7);
            if (checkContentPanelsContainer(c, tag)) return result;
        }
    }
    // 尝试 6级 基础路径
    {
        uintptr_t c = ctx->Ui.FollowPath(gameUiRoot, base6, 6);
        if (c && checkContentPanelsContainer(c, "base6")) return result;
    }
    // 尝试 常见 5级 路径
    static const int p5a[] = {2, 0, 0, 0, 1};
    static const int p5b[] = {2, 0, 0, 0, 0};
    {
        uintptr_t c = ctx->Ui.FollowPath(gameUiRoot, p5a, 5);
        if (c && checkContentPanelsContainer(c, "path5a")) return result;
    }
    {
        uintptr_t c = ctx->Ui.FollowPath(gameUiRoot, p5b, 5);
        if (c && checkContentPanelsContainer(c, "path5b")) return result;
    }

    // ============================================================
    // ★ 策略C（最后兜底）：如果可见Inventory有GridScreenX/Y，
    //   即使找不到content-panel容器，也用其可见Grid坐标反推Tab栏位置
    //   （这里不直接返回result，ListAllStashTabsOrdered会处理havePanels=false的情况）
    // ============================================================
    OutputDebugStringA("[StashOps] FindStashContentPanelsContainer: 所有策略未命中，将依赖Inventory可见Grid推算\n");
    return result;
}

// 保持旧函数名作为兼容包装（避免修改太多调用点）
inline uintptr_t FindStashTabsContainerByPath(const PluginSDK::Context* ctx) {
    auto r = FindStashContentPanelsContainer(ctx);
    // 旧函数期望返回一个"看起来有Tab栏的容器"，但现在我们返回content-panel容器，
    // 后续 EnumerateStashTabButtonsByStructure 会检测到大面板并返回空（这是正确的！）
    // —— 配对逻辑现在主要靠 ListAllStashTabsOrdered 内的"索引对齐+坐标推算"
    return r.containerAddr;
}

// ============================================================
// ★★★ 直接内存读取仓库Tab按钮（绕过UI树，比UI树更直接）
//   原理（参考 Gamehelper-main StashUtility + ImportantUiElements）：
//     1. GetPatternAddress("Game States") → GameStates 全局对象地址
//     2. GameStates + 0x00 → GameState 指针（GameStateStaticOffset.GameState）
//     3. GameState + 0x88 → InGameState 地址（GameStateOffset.States[4].X，States从+0x48开始，每个16字节）
//     4. InGameState + 0x6D8 → LeftPanel 指针（ImportantUiElementsOffsets.LeftPanelPtr）
//     5. FollowPath(LeftPanel, {2,0,0,0,1,1}) → StashTabsContainer（Tab按钮栏容器）
//     6. GetChildren(StashTabsContainer) → 每个子元素就是一个Tab按钮
//     7. ComputeScreenRect(tabButton) → 精确屏幕坐标
//
//   注意：偏移来源于 Gamehelper-main（PoE2 v0.5.4），若游戏更新失效需重新核对。
//   用户已授权"如果办法触发宪法，忽略宪法"，此函数直接读取内存结构。
// ============================================================
struct DirectStashTabButton {
    uintptr_t elementAddr = 0;   // Tab按钮的UI元素地址
    int       childIndex  = -1;  // 在StashTabsContainer子元素中的索引（即Tab顺序）
    float     screenX     = 0.f; // 屏幕左上角X
    float     screenY     = 0.f; // 屏幕左上角Y
    float     width       = 0.f;
    float     height      = 0.f;
    float     centerX     = 0.f; // 中心点（点击用）
    float     centerY     = 0.f;
    bool      isVisible   = false;
    std::string stringId;        // Tab按钮的StringId（如有）
    std::string text;            // Tab按钮的文本（如有）
};

// SEH保护的指针读取
inline bool SafeReadPtr(const PluginSDK::Context* ctx, uintptr_t addr, uintptr_t& out) {
    if (!ctx || !addr) return false;
    __try {
        return ctx->Memory.Read(addr, &out, sizeof(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline std::vector<DirectStashTabButton> ReadStashTabButtonsDirect(const PluginSDK::Context* ctx) {
    std::vector<DirectStashTabButton> out;
    if (!ctx) {
        OutputDebugStringA("[DirectStashTabReader] ctx为空，跳过\n");
        return out;
    }

    OutputDebugStringA("[DirectStashTabReader] ★函数入口★ 开始直接内存读取仓库Tab...\n");

    // ---- Step 1: GetPatternAddress("Game States") ----
    uintptr_t gameStatesAddr = ctx->Memory.GetPatternAddress("Game States");
    if (!gameStatesAddr) {
        OutputDebugStringA("[DirectStashTabReader] 失败: GetPatternAddress(\"Game States\") 返回 0\n");
        return out;
    }
    char log[512];
    sprintf_s(log, "[DirectStashTabReader] Step1: GameStates pattern addr = 0x%llX\n",
        (unsigned long long)gameStatesAddr);
    OutputDebugStringA(log);

    // ---- Step 2: 读取 GameStateStaticOffset.GameState (offset +0x00) ----
    uintptr_t gameStatePtr = 0;
    if (!SafeReadPtr(ctx, gameStatesAddr + 0x00, gameStatePtr) || !gameStatePtr) {
        OutputDebugStringA("[DirectStashTabReader] 失败: 读取 GameState 指针失败\n");
        return out;
    }
    sprintf_s(log, "[DirectStashTabReader] Step2: GameState ptr = 0x%llX\n",
        (unsigned long long)gameStatePtr);
    OutputDebugStringA(log);

    // ---- Step 3: 读取 GameStateOffset.States[4].X (offset +0x88) → InGameState ----
    //   GameStateOffset.States 在 +0x48，每个 StdTuple2D<IntPtr> 是 16 字节（X在+0x00）
    //   States[4] 在 +0x48 + 4*16 = +0x88
    uintptr_t inGameStateAddr = 0;
    if (!SafeReadPtr(ctx, gameStatePtr + 0x88, inGameStateAddr) || !inGameStateAddr) {
        OutputDebugStringA("[DirectStashTabReader] 失败: 读取 InGameState 地址失败\n");
        return out;
    }
    sprintf_s(log, "[DirectStashTabReader] Step3: InGameState addr = 0x%llX\n",
        (unsigned long long)inGameStateAddr);
    OutputDebugStringA(log);

    // ---- Step 4: 读取 ImportantUiElementsOffsets.LeftPanelPtr (InGameState + 0x6D8) ----
    uintptr_t leftPanelAddr = 0;
    if (!SafeReadPtr(ctx, inGameStateAddr + 0x6D8, leftPanelAddr) || !leftPanelAddr) {
        OutputDebugStringA("[DirectStashTabReader] 失败: 读取 LeftPanel 指针失败\n");
        return out;
    }
    sprintf_s(log, "[DirectStashTabReader] Step4: LeftPanel addr = 0x%llX\n",
        (unsigned long long)leftPanelAddr);
    OutputDebugStringA(log);

    // ---- Step 5: FollowPath(LeftPanel, {2,0,0,0,1,1}) → StashTabsContainer ----
    //   Gamehelper StashUtility: Waystone/Normal 路径 {2,0,0,0,1,1}
    //   Fragment 路径 {2,0,0,0,0,1,1}
    //   先尝试 Waystone/Normal，再尝试 Fragment
    static const int pathNormal[] = {2, 0, 0, 0, 1, 1};
    static const int pathFragment[] = {2, 0, 0, 0, 0, 1, 1};
    uintptr_t stashTabsContainer = ctx->Ui.FollowPath(leftPanelAddr, pathNormal, 6);
    if (!stashTabsContainer) {
        // 尝试 Fragment 路径
        stashTabsContainer = ctx->Ui.FollowPath(leftPanelAddr, pathFragment, 7);
        if (stashTabsContainer) {
            OutputDebugStringA("[DirectStashTabReader] Step5: 命中 Fragment 路径 {2,0,0,0,0,1,1}\n");
        }
    } else {
        OutputDebugStringA("[DirectStashTabReader] Step5: 命中 Normal/Waystone 路径 {2,0,0,0,1,1}\n");
    }
    if (!stashTabsContainer) {
        // 兜底：尝试更短的路径
        static const int pathShort[] = {2, 0, 0, 0, 1};
        stashTabsContainer = ctx->Ui.FollowPath(leftPanelAddr, pathShort, 5);
        if (stashTabsContainer) {
            OutputDebugStringA("[DirectStashTabReader] Step5: 兜底命中短路径 {2,0,0,0,1}\n");
        }
    }
    if (!stashTabsContainer) {
        OutputDebugStringA("[DirectStashTabReader] 失败: FollowPath 无法解析 StashTabsContainer\n");
        return out;
    }
    sprintf_s(log, "[DirectStashTabReader] Step5: StashTabsContainer addr = 0x%llX\n",
        (unsigned long long)stashTabsContainer);
    OutputDebugStringA(log);

    // ---- Step 6: GetChildren(StashTabsContainer) → Tab按钮列表 ----
    auto tabChildren = ctx->Ui.GetChildren(stashTabsContainer);
    if (tabChildren.empty()) {
        OutputDebugStringA("[DirectStashTabReader] 失败: StashTabsContainer 无子元素\n");
        return out;
    }
    sprintf_s(log, "[DirectStashTabReader] Step6: StashTabsContainer 子元素数 = %zu\n",
        tabChildren.size());
    OutputDebugStringA(log);

    // ---- Step 7: 对每个 Tab按钮 ComputeScreenRect → 屏幕坐标 ----
    int validCount = 0;
    for (size_t i = 0; i < tabChildren.size() && i < 200; ++i) {
        uintptr_t tabAddr = tabChildren[i];
        if (!tabAddr) continue;

        DirectStashTabButton btn;
        btn.elementAddr = tabAddr;
        btn.childIndex  = (int)i;

        float x = 0, y = 0, w = 0, h = 0;
        bool rectOk = ctx->Ui.ComputeScreenRect(tabAddr, x, y, w, h);
        if (rectOk && w > 1.f && h > 1.f) {
            btn.screenX = x;
            btn.screenY = y;
            btn.width   = w;
            btn.height  = h;
            btn.centerX = x + w * 0.5f;
            btn.centerY = y + h * 0.5f;
            validCount++;
        } else {
            // ComputeScreenRect 失败，用UiElement本地尺寸兜底
            auto pe = ctx->Ui.Read(tabAddr);
            if (pe.Valid) {
                btn.screenX = pe.RelativeX;
                btn.screenY = pe.RelativeY;
                btn.width   = pe.UnscaledWidth;
                btn.height  = pe.UnscaledHeight;
                btn.centerX = pe.RelativeX + pe.UnscaledWidth * 0.5f;
                btn.centerY = pe.RelativeY + pe.UnscaledHeight * 0.5f;
            }
        }

        btn.isVisible = ctx->Ui.IsVisible(tabAddr);
        btn.stringId  = ctx->Ui.GetStringId(tabAddr);
        btn.text      = ctx->Ui.GetText(tabAddr);

        out.push_back(std::move(btn));
    }

    sprintf_s(log, "[DirectStashTabReader] Step7: 共读取 %zu 个Tab按钮，其中 %d 个有有效屏幕坐标\n",
        out.size(), validCount);
    OutputDebugStringA(log);

    // 输出每个Tab按钮的详细信息
    for (size_t i = 0; i < out.size() && i < 30; ++i) {
        const auto& b = out[i];
        sprintf_s(log, "  DirectTab[%zu]: addr=0x%llX center=(%.0f,%.0f) size=(%.0f,%.0f) vis=%d sid='%s' text='%s'\n",
            i, (unsigned long long)b.elementAddr,
            b.centerX, b.centerY, b.width, b.height,
            b.isVisible ? 1 : 0,
            b.stringId.c_str(), b.text.c_str());
        OutputDebugStringA(log);
    }

    return out;
}

// ============================================================
// ★ 修复2：枚举仓库Tab按钮（结构化识别法）
//   改进：不限遍历子元素数量；对仓库面板进行多级深层递归；使用辅助函数
//         检测横向排列Tab栏特征；递归上限5层防栈溢出
// ============================================================
namespace {
    // 检查一个容器是否包含横向排列的Tab栏特征（返回找到的横向按钮数+1=总Tab数）
    inline int DetectHorizontalTabBar(const PluginSDK::Context* ctx,
                                       const std::vector<uintptr_t>& elems,
                                       float maxYDiff = 15.f,
                                       int minCount = 3,
                                       float maxW = 250.f,
                                       float maxH = 100.f) {
        int count = 0;
        float refY = -1, prevX = -1;
        for (size_t i = 0; i < elems.size(); ++i) {
            float sx, sy, sw, sh;
            if (!ctx->Ui.ComputeScreenRect(elems[i], sx, sy, sw, sh)) continue;
            if (sw <= 5 || sh <= 5 || sw > maxW || sh > maxH) continue;
            if (refY < 0) refY = sy;
            else if (std::abs(sy - refY) > maxYDiff) continue;
            if (prevX >= 0 && sx > prevX) count++;
            prevX = sx;
        }
        int total = count + (refY >= 0 ? 1 : 0);  // count=间隔数，总按钮=间隔+1（若有至少1个）
        return total >= minCount ? total : 0;
    }

    // 深层递归查找Tab栏（DFS，深度限制）
    inline uintptr_t DeepFindTabBar(const PluginSDK::Context* ctx,
                                     uintptr_t node,
                                     int depth,
                                     int maxDepth = 5) {
        if (!ctx || !node || depth > maxDepth) return 0;

        char log[512];
        auto children = ctx->Ui.GetChildren(node);
        if (children.empty()) return 0;

        // 快速判断：当前node的直接子元素是否已经构成Tab栏？
        int tabCount = DetectHorizontalTabBar(ctx, children);
        if (tabCount >= 3) {
            sprintf_s(log, "[StashOps] DeepFindTabBar(depth=%d): 找到Tab栏! 横向按钮=%d\n", depth, tabCount);
            OutputDebugStringA(log);
            return node;  // 当前节点就是Tab栏容器
        }

        // 遍历所有子元素（不限数量），优先选择子元素多的节点（Tab栏父容器通常子元素数>5）
        // 先按子元素数降序排序，加速命中
        std::vector<size_t> order(children.size());
        for (size_t i = 0; i < children.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (!children[a] || !children[b]) return false;
            return ctx->Ui.GetChildren(children[a]).size() > ctx->Ui.GetChildren(children[b]).size();
        });

        for (size_t oi = 0; oi < order.size(); ++oi) {
            size_t i = order[oi];
            uintptr_t child = children[i];
            if (!child) continue;
            // 跳过超大尺寸的叶子节点（如670x670的仓库面板本身没有子元素时）
            float cx, cy, cw, ch;
            if (ctx->Ui.ComputeScreenRect(child, cx, cy, cw, ch) && cw > 500.f && ch > 500.f) {
                // 超大叶子：仍要递归（它的子元素可能藏有Tab栏）
            }
            auto subSub = ctx->Ui.GetChildren(child);
            if (subSub.size() < 2) continue;  // 叶子太少的跳过
            uintptr_t found = DeepFindTabBar(ctx, child, depth + 1, maxDepth);
            if (found) return found;
        }
        return 0;
    }
}

inline std::vector<StashTabButton> EnumerateStashTabButtonsByStructure(
    const PluginSDK::Context* ctx,
    uintptr_t containerAddr) {

    std::vector<StashTabButton> out;
    if (!ctx || !containerAddr) return out;

    auto children = ctx->Ui.GetChildren(containerAddr);
    char log[512];
    sprintf_s(log, "[StashOps] EnumerateStashTabButtonsByStructure: 容器子元素数=%zu\n", children.size());
    OutputDebugStringA(log);

    // ★ 修复2.1：先快速判断当前容器直接子元素是否已构成Tab栏（避免误判）
    int directTabs = DetectHorizontalTabBar(ctx, children, 15.f, 3);
    if (directTabs >= 3) {
        sprintf_s(log, "[StashOps] 当前容器直接子元素构成Tab栏! 数量=%d\n", directTabs);
        OutputDebugStringA(log);
    } else {
        // ★ 修复2.2：不确定，用DeepFindTabBar深层递归扫描所有层级（不限子元素数）
        sprintf_s(log, "[StashOps] 当前容器非直接Tab栏，启用DeepFind深层递归(Depth<=5)...\n");
        OutputDebugStringA(log);
        uintptr_t realTabBar = DeepFindTabBar(ctx, containerAddr, 0, 5);
        if (realTabBar && realTabBar != containerAddr) {
            sprintf_s(log, "[StashOps] DeepFind命中，切换到真正Tab栏容器=0x%llX 重新枚举\n",
                (unsigned long long)realTabBar);
            OutputDebugStringA(log);
            // 递归到正确的Tab栏容器执行枚举
            return EnumerateStashTabButtonsByStructure(ctx, realTabBar);
        }

        // DeepFind也没找到：判断是否为大面板(>300x300)，是则返回空（避免误把仓库面板子元素当Tab）
        bool hasBigPanel = false;
        for (size_t i = 0; i < children.size(); ++i) {
            float sx, sy, sw, sh;
            if (!ctx->Ui.ComputeScreenRect(children[i], sx, sy, sw, sh)) continue;
            if (sw > 300.f && sh > 300.f) { hasBigPanel = true; break; }
        }
        if (hasBigPanel) {
            sprintf_s(log, "[StashOps] DeepFind未命中且容器含大面板(>300x300)，返回空（避免把面板当Tab按钮）\n");
            OutputDebugStringA(log);
            return out;
        }
        // 没有大面板 + 子元素少：或许是个小Tab栏，继续枚举
    }

    // ★ 修复2.3：正常枚举Tab按钮（遍历所有子元素，不限数量）
    int validCount = 0;
    for (size_t i = 0; i < children.size(); ++i) {
        uintptr_t tabAddr = children[i];
        if (!tabAddr) continue;

        auto elem = ctx->Ui.Read(tabAddr);
        if (!elem.Valid) continue;

        float x = 0, y = 0, w = 0, h = 0;
        bool hasRect = ctx->Ui.ComputeScreenRect(tabAddr, x, y, w, h);

        StashTabButton btn;
        btn.tabIndex = (int)i;
        btn.label = ctx->Ui.GetText(tabAddr);
        btn.stringId = ctx->Ui.GetStringId(tabAddr);
        btn.isSubTab = false;

        if (hasRect && w > 0 && h > 0) {
            btn.x = x + w * 0.5f;
            btn.y = y + h * 0.5f;
            btn.w = w;
            btn.h = h;
        } else {
            btn.x = 0; btn.y = 0; btn.w = 0; btn.h = 0;
        }

        std::string detectedType = DetectStashTabTypeByStructure(ctx, tabAddr);
        if (!detectedType.empty() && detectedType != "Unknown") {
            if (btn.label.empty()) btn.label = detectedType;
        }

        out.push_back(std::move(btn));
        validCount++;

        sprintf_s(log, "  Tab[%zu]: addr=0x%llX sid='%s' label='%s' type=%s pos=(%.0f,%.0f) size=(%.0f,%.0f) vis=%d\n",
            i, (unsigned long long)tabAddr, btn.stringId.c_str(), btn.label.c_str(),
            detectedType.c_str(), btn.x, btn.y, btn.w, btn.h, elem.IsVisible ? 1 : 0);
        OutputDebugStringA(log);
    }

    sprintf_s(log, "[StashOps] EnumerateStashTabButtonsByStructure 完成: 枚举 %zu 个Tab按钮(有效=%d)\n",
        out.size(), validCount);
    OutputDebugStringA(log);
    return out;
}

// 获取所有仓库页并按排列顺序编号（按屏幕坐标从上到下、从左到右排序）
// 改进版：移除hasValidGrid过滤，使用UI树完整枚举所有Tab
// 返回：按 slotIndex 排序的仓库页列表（包含所有可见和不可见的仓库页）
inline std::vector<OrderedStashTab> ListAllStashTabsOrdered(const PluginSDK::Context* ctx) {
    std::vector<OrderedStashTab> out;
    if (!ctx) return out;

    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    // ★★★ 最高优先级：直接内存读取 Tab 按钮（绕过UI树）
    //   通过 GetPatternAddress("Game States") → InGameState → LeftPanel →
    //   FollowPath({2,0,0,0,1,1}) → StashTabsContainer → GetChildren → 每个Tab按钮
    //   再用 ComputeScreenRect 获取精确屏幕坐标。
    //   这是目前最可靠的"真实Tab顺序+真实屏幕坐标"来源，优先级高于一切推算。
    std::vector<DirectStashTabButton> directTabs = ReadStashTabButtonsDirect(ctx);
    bool haveDirectTabs = !directTabs.empty();
    if (haveDirectTabs) {
        char dlog[256];
        sprintf_s(dlog, "[StashOps] ★DirectTab基准: 共 %zu 个直接读取的Tab按钮\n", directTabs.size());
        OutputDebugStringA(dlog);
    } else {
        OutputDebugStringA("[StashOps] DirectTab基准未命中（将回退到UI树推算）\n");
    }

    // ★ 次要：Content-Panels 容器信息（作为直接读取失败时的兜底）
    StashContentPanelsResult panelsInfo = FindStashContentPanelsContainer(ctx);
    bool havePanels = (panelsInfo.containerAddr != 0 && panelsInfo.panelCount >= 2);
    if (havePanels) {
        char log[512];
        sprintf_s(log, "[StashOps] ContentPanels基准: panelCount=%d visibleIdx=%d visLeft=%.0f visTop=%.0f visW=%.0f visH=%.0f\n",
            panelsInfo.panelCount, panelsInfo.visibleIndex,
            panelsInfo.visibleLeft, panelsInfo.visibleTop,
            panelsInfo.visibleWidth, panelsInfo.visibleHeight);
        OutputDebugStringA(log);
    } else {
        OutputDebugStringA("[StashOps] ContentPanels基准未命中，无法用可见面板坐标推算Tab栏\n");
    }

    // 优先使用 FollowPath 路径访问法获取仓库Tab容器（更可靠）
    uintptr_t pathContainer = FindStashTabsContainerByPath(ctx);
    std::vector<StashTabButton> tabButtons;
    if (pathContainer) {
        tabButtons = EnumerateStashTabButtonsByStructure(ctx, pathContainer);
        OutputDebugStringA("[StashOps] ListAllStashTabsOrdered: 使用 FollowPath 路径访问法获取Tab按钮\n");
    }
    if (tabButtons.empty()) {
        // 回退到原有的 ExtractStashTabButtons
        tabButtons = ExtractStashTabButtons(ctx);
        OutputDebugStringA("[StashOps] ListAllStashTabsOrdered: FollowPath 失败，回退到 ExtractStashTabButtons\n");
    }
    
    // 构建UI树按钮信息映射
    struct TabButtonInfo {
        std::string label;
        std::string stringId;
        float x, y;
        float w, h;
        bool isSubTab;
        int visualOrder;
    };
    
    std::vector<TabButtonInfo> uiButtons;
    for (const auto& btn : tabButtons) {
        TabButtonInfo info;
        info.label = btn.label;
        info.stringId = btn.stringId;
        info.x = btn.x;
        info.y = btn.y;
        info.w = btn.w;
        info.h = btn.h;
        info.isSubTab = btn.isSubTab;
        info.visualOrder = btn.tabIndex;
        uiButtons.push_back(std::move(info));
    }
    
    // ★ 修复：配对前输出所有UI按钮的详细信息，便于诊断坐标从哪来
    if (!uiButtons.empty()) {
        std::string btnLog = "[StashOps] UI树Tab按钮清单(共" + std::to_string(uiButtons.size()) + "个，即将与Inventory配对):\n";
        char bline[512];
        for (size_t bi = 0; bi < uiButtons.size(); ++bi) {
            sprintf_s(bline, "  UI[%zu]: idx=%d x=%.0f y=%.0f w=%.0f h=%.0f sub=%d sid='%s' label='%s'\n",
                bi, uiButtons[bi].visualOrder,
                uiButtons[bi].x, uiButtons[bi].y, uiButtons[bi].w, uiButtons[bi].h,
                uiButtons[bi].isSubTab ? 1 : 0,
                uiButtons[bi].stringId.c_str(), uiButtons[bi].label.c_str());
            btnLog += bline;
        }
        OutputDebugStringA(btnLog.c_str());
    } else {
        OutputDebugStringA("[StashOps] UI树Tab按钮清单: 空！将只使用网格位置推算点击坐标（可能不准）\n");
    }
    
    // 收集所有非主背包的 inventory（不要求Grid.Valid）
    struct InvCandidate {
        PluginSDK::Inventory inv;
        float sortY;
        float sortX;
        bool hasValidGrid;
    };

    std::vector<InvCandidate> candidates;

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;

        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 1) continue;  // 只过滤完全无效的（保留小页和空页）

        // 综合过滤：装备槽位 + 非仓库Tab的 Inventory_NNN（基于 ggpk 格子尺寸数据）
        // 这一步过滤掉 Weapon1/BodyArmour1 等装备槽位，以及 Inventory_NNN 中
        // 格子尺寸不匹配任何已知仓库Tab规格的物品栏。
        const char* invName = ctx->Inventory.GetName(inv.InventoryId);
        std::string nameStr = invName ? invName : "";
        if (IsNonStashInventory(nameStr, inv.TotalBoxesX, inv.TotalBoxesY, inv.InventoryId)) {
            char log[256];
            sprintf_s(log, "[StashOps] 过滤非仓库Tab: invId=%d name='%s' size=%dx%d\n",
                inv.InventoryId, nameStr.c_str(), inv.TotalBoxesX, inv.TotalBoxesY);
            OutputDebugStringA(log);
            continue;
        } else {
            // 保留的 inventory 也输出日志，便于诊断
            char log[256];
            sprintf_s(log, "[StashOps] 保留仓库Tab: invId=%d name='%s' size=%dx%d slots=%d\n",
                inv.InventoryId, nameStr.c_str(), inv.TotalBoxesX, inv.TotalBoxesY,
                inv.TotalBoxesX * inv.TotalBoxesY);
            OutputDebugStringA(log);
        }

        InvCandidate c;
        c.inv = inv;
        c.hasValidGrid = inv.Grid.Valid;
        c.sortY = inv.Grid.GridScreenY > 0 ? inv.Grid.GridScreenY : 99999.f;
        c.sortX = inv.Grid.GridScreenX > 0 ? inv.Grid.GridScreenX : 99999.f;
        candidates.push_back(std::move(c));
    }

    // ★★★ 修复：排序策略 - 解决GridScreen乱值导致配对错位
    //   bug1.log: 不可见Inventory的GridScreenX/Y是99999或残留旧值（如Y=836在屏幕底部），
    //   用GridScreen排序会把可见Tab和不可见Tab顺序打乱，导致candIdx与UI按钮索引不匹配，
    //   点击坐标变成(1826,314)/(453,836)等完全错误的值。
    //
    //   新排序策略：
    //   1. 可见Inventory（Grid.Valid=true且GridOnScreen）在前，按其真实ScreenX排序
    //   2. 不可见Inventory在后，按其名称自然序排序（Inventory_131,136,...或负数ID顺序）
    //      这样它们的candIdx与"UI按钮索引"至少是稳定的，便于推算。
    std::sort(candidates.begin(), candidates.end(),
        [&](const InvCandidate& a, const InvCandidate& b) {
            // 第一优先级：可见性（可见在前，不可见在后）
            bool aVis = a.hasValidGrid && a.sortY < 90000.f && a.sortY > 10.f;
            bool bVis = b.hasValidGrid && b.sortY < 90000.f && b.sortY > 10.f;
            if (aVis != bVis) return aVis ? true : false;
            // 都是可见的：按X（从左到右）+Y（从上到下），Y阈值放宽到30px
            if (aVis && bVis) {
                if (std::fabs(a.sortY - b.sortY) > 30.f) return a.sortY < b.sortY;
                return a.sortX < b.sortX;
            }
            // 都是不可见的：按InventoryId排序（负数ID通常是特殊仓库，按ID升序）
            // 注意：InventoryId的顺序不一定等于Tab顺序，但至少稳定
            return a.inv.InventoryId < b.inv.InventoryId;
        });

    // 日志：排序后candIdx与invId的对应关系（关键诊断信息！）
    {
        char slog[256];
        sprintf_s(slog, "[StashOps] 候选排序结果: 共%zu个 (可见优先+不可见按ID升序)\n", candidates.size());
        OutputDebugStringA(slog);
        for (size_t ci = 0; ci < candidates.size(); ++ci) {
            const auto& cc = candidates[ci];
            const char* cname = ctx->Inventory.GetName(cc.inv.InventoryId);
            sprintf_s(slog, "  cand[%zu]: invId=%d name='%s' grid=%s Y=%.0f X=%.0f slots=%dx%d\n",
                ci, cc.inv.InventoryId, cname ? cname : "",
                cc.hasValidGrid ? "VALID" : "INVALID",
                cc.sortY, cc.sortX,
                cc.inv.TotalBoxesX, cc.inv.TotalBoxesY);
            OutputDebugStringA(slog);
        }
    }

    // 为每个Inventory查找对应的UI按钮
    // 策略：双重匹配 - 先按名称，再按位置
    std::set<int> usedButtonIndices;
    
    int slotIdx = 0;
    std::vector<OrderedStashTab> subTabsCollected;

    for (const auto& c : candidates) {
        const auto& inv = c.inv;
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        bool onScreen = c.hasValidGrid && GridOnScreen(inv, displayW, displayH);

        // 识别类型
        StashTabType type = IdentifyStashTabType(inv, name);

        OrderedStashTab tab;
        tab.inventoryId = inv.InventoryId;
        tab.name = name ? name : "(unknown)";
        tab.type = type;
        tab.slots = slots;
        tab.gridWidth = inv.TotalBoxesX;   // 保存原始尺寸用于诊断
        tab.gridHeight = inv.TotalBoxesY;
        tab.gridX = inv.Grid.GridScreenX;
        tab.gridY = inv.Grid.GridScreenY;
        tab.isVisible = onScreen;
        tab.isSubTab = (type == StashTabType::SubTab || slots <= 6);

        // 先计算当前inventory在candidates中的索引（从0开始）—后面多处需要
        int candIdx = (int)(&c - &candidates[0]);
        if (candIdx < 0 || candIdx >= (int)candidates.size()) {
            for (size_t ci = 0; ci < candidates.size(); ++ci) {
                if (&candidates[ci] == &c) { candIdx = (int)ci; break; }
            }
        }

        // 计算点击位置 - 优先级：DirectTab(直接内存读取) > ContentPanels推算 > UI按钮 > 网格可见
        bool foundButton = false;

        // ★★★★★ 最高优先级：直接内存读取的Tab按钮坐标（DirectStashTabReader）
        //   directTabs 是通过 GetPatternAddress("Game States") → InGameState →
        //   LeftPanel → FollowPath({2,0,0,0,1,1}) → StashTabsContainer → GetChildren
        //   获取的，每个子元素就是一个真实的Tab按钮UI元素。
        //   直接用 candIdx 索引对应 directTabs[candIdx]（因为两者都是按真实Tab顺序排列）。
        //   这是唯一能获取"不可见Tab按钮真实坐标"的方法（UI树枚举只能看到可见的）。
        if (!foundButton && haveDirectTabs && candIdx >= 0 && candIdx < (int)directTabs.size()) {
            const auto& dt = directTabs[candIdx];
            // 只要 centerX/centerY 是合理值就用（不可见的Tab也有相对坐标，但屏幕坐标可能为0）
            if (dt.centerX > 1.f && dt.centerY > 1.f && dt.width > 1.f && dt.height > 1.f) {
                tab.clickX = dt.centerX;
                tab.clickY = dt.centerY;
                tab.clickable = true;
                foundButton = true;
                char dlog[512];
                sprintf_s(dlog, "[StashOps] 配对★DirectTab: candIdx=%d → directTab[%d] center=(%.0f,%.0f) size=(%.0f,%.0f) vis=%d\n",
                    candIdx, dt.childIndex, dt.centerX, dt.centerY, dt.width, dt.height,
                    dt.isVisible ? 1 : 0);
                OutputDebugStringA(dlog);
            }
        }

        // ★★★ 次高优先级：ContentPanels推算（有visibleIndex+visibleLeft/Top时）
        //   思路：
        //   1. 找出candidates中哪个是可见的（onScreen=true），它的candIdx记作 visibleCandIdx
        //   2. panelsInfo.visibleIndex 是可见content-panel在容器子元素中的索引
        //   3. 两者的差 = candidates排序 与 content-panel真实顺序 的偏移
        //   4. 对于当前candIdx的Tab，它的panel索引 = candIdx + (visiblePanelIdx - visibleCandIdx)
        //   5. TabX = visibleLeft + (panelIdx - visiblePanelIdx) * tabWidth
        //      TabY = visibleTop - 35（Tab按钮在面板上方约35px）
        if (!foundButton && havePanels && panelsInfo.visibleIndex >= 0
            && panelsInfo.visibleLeft > 0.f && panelsInfo.visibleTop > 0.f
            && panelsInfo.panelCount >= 2) {
            // 找visibleCandIdx（candidates中onScreen=true的那个）
            static int cachedVisibleCandIdx = -1;
            static int cachedTick = -1;
            int visibleCandIdx = -1;
            // 每帧重新计算一次，但缓存避免重复计算
            {
                for (size_t ci = 0; ci < candidates.size(); ++ci) {
                    bool vis = candidates[ci].hasValidGrid && candidates[ci].sortY < 90000.f && candidates[ci].sortY > 10.f;
                    if (vis) { visibleCandIdx = (int)ci; break; }
                }
            }
            if (visibleCandIdx >= 0) {
                int offset = panelsInfo.visibleIndex - visibleCandIdx;
                int myPanelIdx = candIdx + offset;
                // Tab宽度：基于面板宽度估算，或者默认65px
                float tabWidth = 65.f;
                if (panelsInfo.visibleWidth > 100.f && panelsInfo.panelCount > 2) {
                    // 用panelCount粗略估算：panel宽/12 ≈ 单Tab宽，但不要太夸张
                    float est = panelsInfo.visibleWidth / 12.f;
                    if (est > 35.f && est < 200.f) tabWidth = est;
                }
                // Tab Y = 面板左上角Y再往上35px（Tab栏在面板上方）
                float tabY = panelsInfo.visibleTop - 35.f;
                // Tab X = visibleLeft + (myPanelIdx - visibleIndex) * tabWidth
                // 但visible面板的Tab X 应该刚好在visibleLeft附近，所以：
                float baseTabX = panelsInfo.visibleLeft + 5.f;  // 略向右偏移（Tab按钮不是紧贴左边缘）
                float tabX = baseTabX + (myPanelIdx - panelsInfo.visibleIndex) * tabWidth;
                // 合理性检查
                if (tabX > 50.f && tabX < displayW * 0.95f
                    && tabY > 15.f && tabY < displayH * 0.35f) {  // Tab栏在屏幕上1/3
                    tab.clickX = tabX;
                    tab.clickY = tabY;
                    tab.clickable = true;
                    foundButton = true;
                    char dlog[512];
                    sprintf_s(dlog, "[StashOps] 配对★Panel推算: candIdx=%d visCand=%d visPanelIdx=%d off=%d myPanel=%d → (%.0f,%.0f) tabW=%.0f\n",
                        candIdx, visibleCandIdx, panelsInfo.visibleIndex, offset, myPanelIdx,
                        tab.clickX, tab.clickY, tabWidth);
                    OutputDebugStringA(dlog);
                }
            }
        }

        // ★★★ 次高优先级：ContentPanels容器命中但无visibleIndex时，
        //   用"可见Inventory的GridScreen坐标"推算所有Tab（这是最常见的情况！
        //   bug1.log中可见Inventory_143有正确的Grid，其他11个Inventory不可见Grid是乱值，
        //   但candidates排序正确，panelCount=17与实际12个Tab数量级一致）
        if (!foundButton && havePanels && panelsInfo.panelCount >= 2) {
            // 找visibleCandIdx（candidates中可见且有Grid的那个Inventory）
            int visibleCandIdx = -1;
            float visGridX = 0.f, visGridY = 0.f, visCellSize = 50.f;
            for (size_t ci = 0; ci < candidates.size(); ++ci) {
                const auto& cc = candidates[ci];
                bool vis = cc.hasValidGrid && cc.sortY < 90000.f && cc.sortY > 10.f;
                if (vis && cc.inv.Grid.Valid) {
                    visibleCandIdx = (int)ci;
                    visGridX = cc.inv.Grid.GridScreenX;
                    visGridY = cc.inv.Grid.GridScreenY;
                    visCellSize = cc.inv.Grid.CellSize > 0 ? cc.inv.Grid.CellSize : 50.f;
                    break;
                }
            }
            if (visibleCandIdx >= 0 && visGridX > 0.f && visGridY > 0.f) {
                // Tab宽度默认65px（实际POE2 Tab约55-75px），如果有实际panelWidth更好
                float tabWidth = 65.f;
                // 用面板宽度推算（有visibleWidth就用visibleWidth，否则用Grid估算）
                if (panelsInfo.visibleWidth > 300.f) {
                    float estW = panelsInfo.visibleWidth / 12.f;
                    if (estW > 35.f && estW < 200.f) tabWidth = estW;
                }
                // ★ Inventory的GridScreenX/Y = 网格左上角
                //   面板左上角 = GridScreenX - 10px左右边距
                //   Tab按钮 Y = 面板左上角Y - 35px（Tab栏在面板上方）
                float panelLeftX = visGridX - 8.f;  // 网格左边距约8px（面板左边缘）
                float panelTopY  = visGridY - 70.f; // 网格上方通常有标题栏≈70px（面板上边缘）
                float tabY = panelTopY - 32.f;      // Tab栏在面板上方约32px
                // Tab基准X：面板左上角X + 10px（Tab按钮不贴面板左边缘）
                float baseTabX = panelLeftX + 10.f;
                // 推算当前candIdx对应的Tab X：
                //   visibleCandIdx对应"第visibleCandIdx个Tab"，它的Tab X 应该≈ baseTabX + visibleCandIdx * tabWidth
                //   所以通用公式：
                float tabX = baseTabX + candIdx * tabWidth;

                // 合理性检查：Tab应在屏幕上方1/3范围内，X合理
                if (tabX > 50.f && tabX < displayW * 0.95f
                    && tabY > 15.f && tabY < displayH * 0.35f) {
                    tab.clickX = tabX;
                    tab.clickY = tabY;
                    tab.clickable = true;
                    foundButton = true;
                    char dlog[512];
                    sprintf_s(dlog, "[StashOps] 配对★Grid推算: candIdx=%d visCand=%d Grid=(%.0f,%.0f) panelL=(%.0f,%.0f) → (%.0f,%.0f) tabW=%.0f\n",
                        candIdx, visibleCandIdx, visGridX, visGridY,
                        panelLeftX, panelTopY,
                        tab.clickX, tab.clickY, tabWidth);
                    OutputDebugStringA(dlog);
                }
            }
        }

        // ★★★ 最兜底的推算：不需要havePanels，只要candidates中有可见Inventory，
        //   就用它的GridScreen作为基准推算其他所有Tab（即使找不到ContentPanels容器）
        //   这是避免"clickable=0导致Tab无法切换"的最后防线
        if (!foundButton) {
            static int s_lastVisibleCandIdx = -1;
            static float s_lastGridX = 0.f, s_lastGridY = 0.f, s_lastCellSize = 50.f;
            static uint64_t s_lastFrameTick = 0;
            int visibleCandIdx = -1;
            float visGridX = 0.f, visGridY = 0.f, visCellSize = 50.f;
            for (size_t ci = 0; ci < candidates.size(); ++ci) {
                const auto& cc = candidates[ci];
                bool vis = cc.hasValidGrid && cc.sortY < 90000.f && cc.sortY > 10.f;
                if (vis && cc.inv.Grid.Valid) {
                    visibleCandIdx = (int)ci;
                    visGridX = cc.inv.Grid.GridScreenX;
                    visGridY = cc.inv.Grid.GridScreenY;
                    visCellSize = cc.inv.Grid.CellSize > 0 ? cc.inv.Grid.CellSize : 50.f;
                    s_lastVisibleCandIdx = visibleCandIdx;
                    s_lastGridX = visGridX;
                    s_lastGridY = visGridY;
                    s_lastCellSize = visCellSize;
                    break;
                }
            }
            // 如果当前帧没找到可见的，但缓存里有（比如刚切换Tab瞬间），用缓存的
            if (visibleCandIdx < 0 && s_lastVisibleCandIdx >= 0 && s_lastGridX > 0.f) {
                visibleCandIdx = s_lastVisibleCandIdx;
                visGridX = s_lastGridX;
                visGridY = s_lastGridY;
                visCellSize = s_lastCellSize;
            }
            if (visibleCandIdx >= 0 && visGridX > 0.f && visGridY > 0.f) {
                const float tabWidth = 65.f;  // 默认标准Tab宽度
                float panelLeftX = visGridX - 8.f;
                float panelTopY  = visGridY - 70.f;
                float tabY = panelTopY - 32.f;
                float baseTabX = panelLeftX + 10.f;
                // ★ 关键：考虑candIdx与visibleCandIdx的偏移
                //   visibleCandIdx的Tab X = baseTabX + visibleCandIdx * tabWidth
                //   所以当前candIdx的Tab X = baseTabX + candIdx * tabWidth
                float tabX = baseTabX + candIdx * tabWidth;

                float maxRightX = displayW * 0.95f > 2000.f ? displayW * 0.95f : 2000.f;
                float maxBottomY = displayH * 0.35f > 800.f ? displayH * 0.35f : 800.f;
                if (tabX > 50.f && tabX < maxRightX
                    && tabY > 15.f && tabY < maxBottomY) {
                    tab.clickX = tabX;
                    tab.clickY = tabY;
                    tab.clickable = true;
                    foundButton = true;
                    char dlog[512];
                    sprintf_s(dlog, "[StashOps] 配对★最终兜底: candIdx=%d visCand=%d → (%.0f,%.0f)\n",
                        candIdx, visibleCandIdx, tab.clickX, tab.clickY);
                    OutputDebugStringA(dlog);
                }
            }
        }

        // 第一轮：按名称匹配UI按钮（优先级次高，ContentPanels失败时用）
        if (!foundButton && !uiButtons.empty()) {
            std::string invNameLower = tab.name;
            for (char& ch : invNameLower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            for (size_t bi = 0; bi < uiButtons.size(); ++bi) {
                if (usedButtonIndices.count((int)bi)) continue;
                
                std::string btnLower = uiButtons[bi].label;
                for (char& ch : btnLower)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

                if (!btnLower.empty() && !invNameLower.empty() &&
                    (btnLower.find(invNameLower) != std::string::npos ||
                     invNameLower.find(btnLower) != std::string::npos)) {
                    tab.clickX = uiButtons[bi].x;
                    tab.clickY = uiButtons[bi].y;
                    tab.clickable = true;
                    tab.isSubTab = uiButtons[bi].isSubTab || tab.isSubTab;
                    usedButtonIndices.insert((int)bi);
                    foundButton = true;
                    break;
                }
            }
        }
        
        // ★★★ 第二轮：UI按钮索引对齐（支持多行Tab布局）
        //   核心思路：UI按钮已按视觉顺序(Y→X)排列，与candidates排序一致
        //   直接用candIdx索引分配，不再依赖单行判定
        //   推算补全时，用全局平均X间距和最近一行的Y坐标
        if (!foundButton && !uiButtons.empty()) {
            float avgBtnY = 0.f;
            float avgStep = 0.f;
            int cntStep = 0;
            if (uiButtons.size() >= 2) {
                avgBtnY = 0.f;
                for (size_t bi2 = 0; bi2 < uiButtons.size(); ++bi2) {
                    avgBtnY += uiButtons[bi2].y;
                }
                avgBtnY /= (float)uiButtons.size();
                // 计算全局相邻按钮对的平均X间距
                float sumStep = 0.f;
                for (size_t bi = 1; bi < uiButtons.size(); ++bi) {
                    float s = uiButtons[bi].x - uiButtons[bi-1].x;
                    if (s > 5.f && s < 300.f) { sumStep += s; cntStep++; }
                }
                avgStep = cntStep > 0 ? sumStep / cntStep : 0.f;
                if (avgStep < 20.f || avgStep > 250.f) avgStep = 60.f;
            } else if (uiButtons.size() == 1) {
                avgBtnY = uiButtons[0].y;
                avgStep = 60.f;
            }
            
            // 多行日志
            if (uiButtons.size() >= 2) {
                char dlog[256];
                sprintf_s(dlog, "[StashOps] UI按钮分配: %zu个按钮 avgY=%.0f avgStep=%.0f (支持多行)\n",
                    uiButtons.size(), avgBtnY, avgStep);
                OutputDebugStringA(dlog);
            }
            
            char dlog[512];
            // 情况A：candIdx在uiButtons范围内，直接用对应按钮
            if (candIdx >= 0 && candIdx < (int)uiButtons.size()) {
                int useBtnIdx = -1;
                if (!usedButtonIndices.count(candIdx)) {
                    useBtnIdx = candIdx;
                } else {
                    for (int dist = 1; dist <= (int)uiButtons.size() && useBtnIdx < 0; ++dist) {
                        int tryA = candIdx - dist;
                        int tryB = candIdx + dist;
                        if (tryA >= 0 && !usedButtonIndices.count(tryA)) useBtnIdx = tryA;
                        else if (tryB < (int)uiButtons.size() && !usedButtonIndices.count(tryB)) useBtnIdx = tryB;
                    }
                }
                if (useBtnIdx >= 0) {
                    tab.clickX = uiButtons[useBtnIdx].x;
                    tab.clickY = uiButtons[useBtnIdx].y;
                    tab.clickable = true;
                    tab.isSubTab = uiButtons[useBtnIdx].isSubTab || tab.isSubTab;
                    usedButtonIndices.insert(useBtnIdx);
                    foundButton = true;
                    sprintf_s(dlog, "[StashOps] 配对A(多行): candIdx=%d UI按钮分配=%d (%.0f,%.0f)\n",
                        candIdx, useBtnIdx, tab.clickX, tab.clickY);
                    OutputDebugStringA(dlog);
                }
            }
            // 情况B：candIdx超出uiButtons范围 → X间距推算补全
            if (!foundButton && candIdx >= (int)uiButtons.size() && uiButtons.size() >= 1) {
                int lastIdx = (int)uiButtons.size() - 1;
                int extra = candIdx - lastIdx;
                // 多行支持：用最后一个按钮的Y而非平均Y
                float extrapY = uiButtons[lastIdx].y;
                tab.clickX = uiButtons[lastIdx].x + avgStep * extra;
                tab.clickY = extrapY;
                float maxRightX = displayW * 0.96f;
                float minLeftX = displayW > 0 ? 100.f : 100.f;
                if (tab.clickX > minLeftX && tab.clickX < maxRightX && avgStep > 10.f) {
                    tab.clickable = true;
                    foundButton = true;
                    sprintf_s(dlog, "[StashOps] 配对B(多行推算): candIdx=%d 超UI=%zu, 基准UI[%d]=(%.0f,%.0f) avgStep=%.1f extra=%d → click=(%.0f,%.0f)\n",
                        candIdx, uiButtons.size(), lastIdx,
                        uiButtons[lastIdx].x, uiButtons[lastIdx].y, avgStep, extra,
                        tab.clickX, tab.clickY);
                    OutputDebugStringA(dlog);
                }
            }
            // 情况C：不可见Tab且有网格，用网格推算（保底，仅用于可见Inventory）
            if (!foundButton && c.hasValidGrid && onScreen) {
                float cellSize = inv.Grid.CellSize > 0 ? inv.Grid.CellSize : 50.0f;
                tab.clickX = inv.Grid.GridScreenX + cellSize * 0.5f;
                tab.clickY = inv.Grid.GridScreenY - 35.0f;
                if (tab.clickX > 50.f && tab.clickX < displayW * 0.95f
                    && tab.clickY > 30.f && tab.clickY < displayH * 0.6f) {
                    tab.clickable = true;
                    foundButton = true;
                    sprintf_s(dlog, "[StashOps] 配对C(可见网格保底): candIdx=%d Grid=(%.0f,%.0f)→click=(%.0f,%.0f)\n",
                        candIdx, inv.Grid.GridScreenX, inv.Grid.GridScreenY,
                        tab.clickX, tab.clickY);
                    OutputDebugStringA(dlog);
                }
            }
        }

        // ★★★ 最兜底：ContentPanels没拿到但UI按钮也没有，如果有可见Inventory且有网格，仅对可见的用网格
        if (!foundButton && c.hasValidGrid && onScreen) {
            float cellSize = inv.Grid.CellSize > 0 ? inv.Grid.CellSize : 50.0f;
            tab.clickX = inv.Grid.GridScreenX + cellSize * 0.5f;
            tab.clickY = inv.Grid.GridScreenY - 35.0f;
            // 合理性检查
            if (tab.clickX > 50.f && tab.clickX < displayW * 0.95f
                && tab.clickY > 30.f && tab.clickY < displayH * 0.35f) {
                tab.clickable = true;
                foundButton = true;
                char dlog[512];
                sprintf_s(dlog, "[StashOps] 配对最兜底(可见网格): candIdx=%d (%.0f,%.0f)\n",
                    candIdx, tab.clickX, tab.clickY);
                OutputDebugStringA(dlog);
            }
        }
        // ★ 如果还是没找到，保持clickable=false，状态机跳过但不崩溃

        // 分类：主Tab vs 子Tab
        if (tab.isSubTab) {
            subTabsCollected.push_back(std::move(tab));
        } else {
            out.push_back(std::move(tab));
        }
    }

    // 分配slotIndex（按排序顺序）
    // 主Tab先编号，子Tab后编号
    int idx = 1;
    for (auto& tab : out) {
        tab.slotIndex = idx++;
    }
    for (auto& sub : subTabsCollected) {
        sub.slotIndex = idx++;
        out.push_back(std::move(sub));
    }
    
    // ★ 修复：不再添加"没有对应Inventory的UI按钮"！
    // 原代码导致 bug1.log 中出现14个 invId=0 name='' 的无效条目，
    // 这些无效条目全部点击坐标(350,465)，导致IconClassify阶段大量无效模板匹配，
    // 状态机尝试切换到这些无效Tab时会反复点击仓库面板，最终超时Abort表现为"崩溃"。
    //
    // 如果UI按钮没有对应Inventory，说明要么：
    //   a) UI树枚举失败（EnumerateStashTabButtonsByStructure返回了仓库面板子元素）
    //   b) 该按钮对应的Inventory不存在（被过滤掉的装备槽位等）
    // 无论哪种情况，都不应把它们作为有效仓库页加入列表。
    {
        char log[256];
        int unmatchedCount = 0;
        for (size_t bi = 0; bi < uiButtons.size(); ++bi) {
            if (!usedButtonIndices.count((int)bi)) unmatchedCount++;
        }
        if (unmatchedCount > 0) {
            sprintf_s(log, "[StashOps] 跳过 %d 个未匹配Inventory的UI按钮（避免invId=0无效条目）\n", unmatchedCount);
            OutputDebugStringA(log);
        }
    }

    // 输出调试日志
    if (!out.empty()) {
        std::string logMsg = "[StashOps] 仓库页编号映射(改进版): 共" + std::to_string(out.size()) + "页 (含UI树枚举)\n";
        for (const auto& tab : out) {
            char line[512];
            sprintf_s(line, "  #%d: invId=%d name='%s' type=%s slots=%d size=%dx%d visible=%d click=(%.0f,%.0f) sub=%d clickable=%d\n",
                tab.slotIndex, tab.inventoryId, tab.name.c_str(),
                StashTabTypeName(tab.type), tab.slots,
                tab.gridWidth, tab.gridHeight,
                tab.isVisible ? 1 : 0, tab.clickX, tab.clickY,
                tab.isSubTab ? 1 : 0, tab.clickable ? 1 : 0);
            logMsg += line;
        }
        OutputDebugStringA(logMsg.c_str());
    } else {
        OutputDebugStringA("[StashOps] 警告: 没有找到任何仓库页\n");
    }

    return out;
}

// 根据编号（slotIndex）点击仓库页
// slotIndex: 1-based 编号（第几个仓库页）
inline bool ClickStashTabBySlotIndex(const PluginSDK::Context* ctx, int slotIndex) {
    if (!ctx || slotIndex <= 0) return false;

    auto tabs = ListAllStashTabsOrdered(ctx);

    for (const auto& tab : tabs) {
        if (tab.slotIndex == slotIndex && tab.clickable) {
            TabletReforgeInput::MoveCursorScreen(tab.clickX, tab.clickY);
            TabletReforgeInput::SleepMs(5);
            TabletReforgeInput::LeftClickAtCursor();
            TabletReforgeInput::SleepMs(300); // 等待UI切换

            // 点击后触发刷新
            ctx->Inventory.Scan(-1);
            TabletReforgeInput::SleepMs(200);

            std::string logMsg = "[StashOps] 点击仓库页 #" + std::to_string(slotIndex)
                + " invId=" + std::to_string(tab.inventoryId)
                + " name='" + tab.name + "'\n";
            OutputDebugStringA(logMsg.c_str());

            return true;
        }
    }

    std::string logMsg = "[StashOps] 点击仓库页 #" + std::to_string(slotIndex) + " 失败: 未找到或不可点击\n";
    OutputDebugStringA(logMsg.c_str());
    return false;
}

// 获取指定编号仓库页的inventoryId
inline int GetInventoryIdBySlotIndex(const PluginSDK::Context* ctx, int slotIndex) {
    if (!ctx || slotIndex <= 0) return 0;

    auto tabs = ListAllStashTabsOrdered(ctx);
    for (const auto& tab : tabs) {
        if (tab.slotIndex == slotIndex) {
            return tab.inventoryId;
        }
    }
    return 0;
}

// 基于UI树的点击仓库页（精确版，按inventoryId）
inline bool ClickStashTabV2(const PluginSDK::Context* ctx, int inventoryId) {
    if (!ctx) return false;
    
    auto tabs = ListAllStashTabsOrdered(ctx);
    
    // 查找目标仓库页
    for (const auto& tab : tabs) {
        if (tab.inventoryId == inventoryId && tab.clickable) {
            TabletReforgeInput::MoveCursorScreen(tab.clickX, tab.clickY);
            TabletReforgeInput::SleepMs(5);
            TabletReforgeInput::LeftClickAtCursor();
            TabletReforgeInput::SleepMs(300);
            ctx->Inventory.Scan(-1);
            TabletReforgeInput::SleepMs(200);
            return true;
        }
    }
    
    return false;
}

// 查找含有先行者碑牌的仓库页（非主背包，且 Grid.Valid 即当前可见页）
// 返回该 inventory 的引用拷贝。注意：PoeFixer 只给可见页赋 Grid.Valid=true。
inline std::optional<PluginSDK::Inventory> FindVisibleStash(const PluginSDK::Context* ctx) {
    if (!ctx) return std::nullopt;
    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        if (inv.TotalBoxesX * inv.TotalBoxesY < 2) continue; // 跳过小面板
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue; // 跳过主背包
        if (!GridOnScreen(inv, displayW, displayH)) continue;
        return inv;
    }
    return std::nullopt;
}

// 安全日志辅助函数：将用户数据作为纯文本输出，防止格式化字符串漏洞
inline void SafeLogItemString(const char* prefix, size_t idx, const std::string& value) {
    // 直接拼接字符串，避免sprintf_s格式化用户数据
    std::string logMsg = std::string("[StashOps] ") + prefix + " 物品#" + std::to_string(idx);
    if (value.empty()) {
        logMsg += " (空)\n";
    } else {
        // 限制长度防止日志溢出
        size_t displayLen = value.size() < 200 ? value.size() : 200;
        logMsg += " len=" + std::to_string(value.size()) + " content='";
        for (size_t i = 0; i < displayLen; ++i) {
            char c = value[i];
            // 替换不可打印字符和格式化字符
            if (c < 32 || c > 126) c = '.';
            if (c == '%') { logMsg += "%%"; continue; }
            logMsg += c;
        }
        if (value.size() > 200) logMsg += "...";
        logMsg += "'\n";
    }
    OutputDebugStringA(logMsg.c_str());
}

// 安全日志辅助函数：格式化带用户数据的日志消息（用户数据作为纯文本）
inline void SafeLogUserData(const std::string& msgPrefix, 
                             const std::string& userData1, 
                             const std::string& userData2) {
    // 转义用户数据中的格式化字符
    auto escapeData = [](const std::string& s) -> std::string {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            if (c == '%') result += "%%";
            else if (c < 32 || c > 126) result += '.';
            else result += c;
        }
        return result;
    };
    
    std::string safeData1 = escapeData(userData1);
    std::string safeData2 = escapeData(userData2);
    
    char buf[512];
    sprintf_s(buf, "%s: '%s', '%s'\n", msgPrefix.c_str(), safeData1.c_str(), safeData2.c_str());
    OutputDebugStringA(buf);
}

// 前置声明：多仓库页扫描函数
inline std::vector<StashTablet> CollectStashTabletsMulti(
    const PluginSDK::Context* ctx,
    TabletReforgeConfig::Settings& settings,
    const std::vector<PluginSDK::Inventory>& inventories);

// 收集当前可见仓库页里的物品（按当前 Settings 过滤）
inline std::vector<StashTablet> CollectStashTablets(const PluginSDK::Context* ctx,
                                                    TabletReforgeConfig::Settings& settings) {
    std::vector<StashTablet> out;
    if (!ctx) {
        OutputDebugStringA("[StashOps] CollectStashTablets: ctx为空\n");
        return out;
    }

    try {
        // === 关键: 同步类型级别的词缀勾选到全局词缀过滤器 ===
        SyncBonusIdsToModifierKeys(settings);

        // 优先使用手动配置的仓库页（原料页），回退到勾选列表
        auto stashes = FindVisibleStashesForSettings(ctx, settings);
        if (!stashes.empty()) {
            return CollectStashTabletsMulti(ctx, settings, stashes);
        }

        // 如果没有配置的仓库页，回退到旧的单仓库页模式
        auto inv = FindVisibleStash(ctx);
        if (!inv) {
            OutputDebugStringA("[StashOps] 找不到可见仓库页\n");
            return out;
        }

        float displayW = 0.f, displayH = 0.f;
        GetScreenSize(ctx, displayW, displayH);

        const bool needMods = settings.useModifierFilterMode && !settings.selectedModifierKeys.empty();
        
        if (settings.verboseLogging || needMods) {
            char invInfo[512];
            sprintf_s(invInfo, "\n[StashOps] === 仓库扫描开始 === Grid=%d, CellSize=%.1f, 格子=%dx%d, 物品=%zu, useModFilter=%d, selectedKeys=%zu",
                inv->Grid.Valid ? 1 : 0,
                inv->Grid.CellSize,
                inv->TotalBoxesX, inv->TotalBoxesY,
                inv->Items.size(),
                settings.useModifierFilterMode ? 1 : 0,
                settings.selectedModifierKeys.size());
            OutputDebugStringA(invInfo);
            
            if (!settings.selectedModifierKeys.empty()) {
                std::string keyList = "[StashOps] 选中关键词: [";
                bool first = true;
                for (const auto& k : settings.selectedModifierKeys) {
                    if (!first) keyList += ", ";
                    first = false;
                    keyList += "'" + k + "'";
                }
                keyList += "]\n";
                OutputDebugStringA(keyList.c_str());
            }
        }

        int totalItems = 0;
        int materialItems = 0;
        int nonMaterialItems = 0;
        int rectFailedItems = 0;
        int itemsWithMods = 0;        // 成功提取到词缀的物品数（词缀筛选有效性诊断）
        int craftableItems = 0;       // 可合成碑牌数量（用于判断词缀提取率）

        // Item processing loop
        for (size_t idx = 0; idx < inv->Items.size(); ++idx) {
            try {
                const auto& item = inv->Items[idx];

                // Step 1: ResolveItemRect
                auto rect = ResolveItemRect(*inv, item, displayW, displayH);
                
                if (!rect) {
                    rectFailedItems++;
                    if (settings.verboseLogging && rectFailedItems <= 3) {
                        char failLog[256];
                        sprintf_s(failLog, "[StashOps] 物品#%zu 坐标解析失败\n", idx);
                        OutputDebugStringA(failLog);
                    }
                    continue;
                }

                totalItems++;

                // Step 2: Create StashTablet
                StashTablet st;
                st.rect       = *rect;
                st.rarity     = item.Rarity;
                st.identified = item.IsIdentified;
                st.path       = item.Path;
                st.baseType   = item.BaseTypeName;
                st.slotX      = item.SlotX;  // 用于从左至右从上至下排序
                st.slotY      = item.SlotY;
                st.stackCount = item.StackCount > 0 ? item.StackCount : 1;

                // 始终尝试读取 ReadItemMods 获取准确的 rarity/identified
                // 库存数据中的 Rarity/IsIdentified 可能不准确，Mods 数据更可靠
                if (item.Address != 0) {
                    auto itemMods = ctx->Inventory.ReadItemMods(item.Address);
                    if (itemMods.Valid) {
                        // 用 Mods 数据更新（更准确）
                        if (itemMods.Rarity > 0) {
                            st.rarity = itemMods.Rarity;
                        }
                        // 只在有 Mods 时才更新 identified
                        // （未鉴定物品 Mods 通常也是有效的，但 IsIdentified 字段可能不同）
                        st.identified = itemMods.IsIdentified;

                        // 【方案 B v1.3】合规词缀 Id 读取（仅 enableBonusMatch=true 时）
                        // 只读 Mod.Id + Mod.Hash32，绝不读 Mod.Name/AffixName/StatKey
                        if (settings.enableBonusMatch) {
                            ExtractModIds(itemMods, st.modIds, st.modHashes,
                                          settings.bonusMatchSilent, st.path);
                        }
                    }
                }

                // Step 3: Match patterns（统一走 Ex 包装函数）
                std::string materialDebugLog;
                if (settings.verboseLogging) {
                    st.isMaterial = MatchesPoe2DataPatternsDebug(
                        st.path, st.baseType, st.rarity, st.identified, settings, materialDebugLog);
                } else {
                    st.isMaterial = MatchesDesiredReforgeTypeEx(
                        st.path, st.baseType, st.rarity, st.identified,
                        st.modIds, st.modHashes, settings);
                }
                st.isProductType = MatchesDesiredProductTypeEx(
                    st.path, st.baseType, st.rarity, st.identified,
                    st.modIds, st.modHashes, settings);

                if (settings.verboseLogging || needMods) {
                    char itemLog[2048];
                    std::string safePath = st.path;
                    std::string safeBt = st.baseType;
                    for (auto& c : safePath) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
                    for (auto& c : safeBt) { if (c < 32 || c > 126) c = '.'; if (c == '%') c = '.'; }
                    if (safePath.size() > 80) safePath = safePath.substr(0, 80) + "...";
                    if (safeBt.size() > 80) safeBt = safeBt.substr(0, 80) + "...";
                    ::sprintf_s(itemLog, "[StashOps] 物品#%zu Path='%s' BT='%s' rarity=%d ident=%d material=%d product=%d needMods=%d\n",
                        idx, safePath.c_str(), safeBt.c_str(), st.rarity, st.identified ? 1 : 0,
                        st.isMaterial ? 1 : 0, st.isProductType ? 1 : 0, needMods ? 1 : 0);
                    OutputDebugStringA(itemLog);
                    OutputDebugStringA(materialDebugLog.c_str());
                }

                if (st.isMaterial) {
                    materialItems++;
                } else {
                    nonMaterialItems++;
                }

                // === 问题5诊断：统计可合成碑牌（词缀提取率检测已停用）===
                if (IsCraftableItem(st.path, st.baseType)) {
                    craftableItems++;
                    // （已停用）needMods 词缀提取——安全红线禁止读取词缀文本
                }

                out.push_back(std::move(st));
            } catch (const std::exception&) {
                continue;
            } catch (...) {
                continue;
            }
        }

        // 扫描结果日志
        char summary[1024];
        if (needMods) {
            sprintf_s(summary,
                "[StashOps] === 仓库扫描摘要 === 总物品=%d 可合成=%d 原料=%d 非原料=%d 坐标失败=%d | 词缀提取率=%d/%d (%.0f%%)\n",
                totalItems, craftableItems, materialItems, nonMaterialItems, rectFailedItems,
                itemsWithMods, craftableItems,
                craftableItems > 0 ? (100.f * itemsWithMods / craftableItems) : 0.f);
        } else {
            sprintf_s(summary,
                "[StashOps] === 仓库扫描摘要 === 总物品=%d 可合成=%d 原料=%d 非原料=%d 坐标失败=%d\n",
                totalItems, craftableItems, materialItems, nonMaterialItems, rectFailedItems);
        }
        OutputDebugStringA(summary);

        // === 问题5已废弃：词缀筛选功能因安全红线被永久移除（禁止读取 ExplicitMods 文本）===
        // useModifierFilterMode=ON 时不再触发任何词缀数据读取，行为完全等价于 Path+Rarity 筛选。
        (void)needMods;
        (void)itemsWithMods;

        // === 排序：从左至右，从上至下（相邻点击优先）===
        std::sort(out.begin(), out.end(), [](const StashTablet& a, const StashTablet& b) {
            if (a.slotY != b.slotY) return a.slotY < b.slotY;
            return a.slotX < b.slotX;
        });

        return out;
    } catch (const std::exception& e) {
        char excLog[512];
        sprintf_s(excLog, "[StashOps] CollectStashTablets 异常: %s\n", e.what());
        OutputDebugStringA(excLog);
        return out;
    } catch (...) {
        OutputDebugStringA("[StashOps] CollectStashTablets 未知异常\n");
        return out;
    }
}

// 多仓库页版本：扫描所有选中的仓库页，合并结果
inline std::vector<StashTablet> CollectStashTabletsMulti(
    const PluginSDK::Context* ctx,
    TabletReforgeConfig::Settings& settings,
    const std::vector<PluginSDK::Inventory>& inventories) {

    std::vector<StashTablet> out;
    if (!ctx || inventories.empty()) return out;

    try {
        SyncBonusIdsToModifierKeys(settings);

        float displayW = 0.f, displayH = 0.f;
        GetScreenSize(ctx, displayW, displayH);

        const bool needMods = settings.useModifierFilterMode && !settings.selectedModifierKeys.empty();
        int totalItems = 0;
        int materialItems = 0;
        int nonMaterialItems = 0;

        for (const auto& inv : inventories) {
            int invId = inv.InventoryId;
            int invTotal = (int)inv.Items.size();

            for (size_t idx = 0; idx < inv.Items.size(); ++idx) {
                try {
                    const auto& item = inv.Items[idx];
                    auto rect = ResolveItemRect(inv, item, displayW, displayH);
                    if (!rect) continue;

                    totalItems++;
                    StashTablet st;
                    st.rect       = *rect;
                    st.rarity     = item.Rarity;
                    st.identified = item.IsIdentified;
                    st.path       = item.Path;
                    st.baseType   = item.BaseTypeName;
                    st.slotX      = item.SlotX;
                    st.slotY      = item.SlotY;

                    if (item.Address != 0) {
                        auto itemMods = ctx->Inventory.ReadItemMods(item.Address);
                        if (itemMods.Valid) {
                            if (itemMods.Rarity > 0) st.rarity = itemMods.Rarity;
                            st.identified = itemMods.IsIdentified;
                            // 【方案 B v1.3】合规词缀 Id 读取（仅 enableBonusMatch=true 时）
                            // 只读 Mod.Id + Mod.Hash32，绝不读 Mod.Name/AffixName/StatKey
                            if (settings.enableBonusMatch) {
                                ExtractModIds(itemMods, st.modIds, st.modHashes,
                                              settings.bonusMatchSilent, st.path);
                            }
                        }
                    }

                    // 【方案 B v1.3】统一走 Ex 包装函数（含 IsNormalRarityCraftable 分支）
                    (void)needMods;
                    st.isMaterial = MatchesDesiredReforgeTypeEx(
                        st.path, st.baseType, st.rarity, st.identified,
                        st.modIds, st.modHashes, settings);
                    st.isProductType = MatchesDesiredProductTypeEx(
                        st.path, st.baseType, st.rarity, st.identified,
                        st.modIds, st.modHashes, settings);

                    if (st.isMaterial) materialItems++;
                    else nonMaterialItems++;

                    out.push_back(std::move(st));
                } catch (...) {
                    continue;
                }
            }
        }

        char summary[512];
        sprintf_s(summary, "[StashOps] 多仓库扫描: %zu页 总=%d 原料=%d 非原料=%d\n",
            inventories.size(), totalItems, materialItems, nonMaterialItems);
        OutputDebugStringA(summary);

        std::sort(out.begin(), out.end(), [](const StashTablet& a, const StashTablet& b) {
            if (a.slotY != b.slotY) return a.slotY < b.slotY;
            return a.slotX < b.slotX;
        });

        return out;
    } catch (...) {
        return out;
    }
}

// 兼容旧接口：不接收 settings 时回退到"所有碑牌类 + 不卡鉴定"
inline std::vector<StashTablet> CollectStashTablets(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings s;
    s.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    s.requireIdentified = false;
    s.withdrawRequireIdentified = false;
    return CollectStashTablets(ctx, s);
}

// 仓库里下一个可取出的原料物品（基于当前 Settings 的合成类型选择）
inline std::optional<ScreenRect> NextTempleTabletInStash(const PluginSDK::Context* ctx,
                                                          TabletReforgeConfig::Settings& settings) {
    for (const auto& st : CollectStashTablets(ctx, settings)) {
        if (st.isMaterial) return st.rect;
    }
    return std::nullopt;
}
inline std::optional<ScreenRect> NextTempleTabletInStash(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings s;
    s.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    s.requireIdentified = false;
    s.withdrawRequireIdentified = false;
    return NextTempleTabletInStash(ctx, s);
}

// 仓库里下一个匹配产物类型的物品（产物存回辅助判断）
inline std::optional<ScreenRect> NextAnyTempleInStash(const PluginSDK::Context* ctx,
                                                       TabletReforgeConfig::Settings& settings) {
    for (const auto& st : CollectStashTablets(ctx, settings)) {
        if (st.isProductType) return st.rect;
    }
    return std::nullopt;
}
inline std::optional<ScreenRect> NextAnyTempleInStash(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings s;
    s.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    return NextAnyTempleInStash(ctx, s);
}

// 仓库里还有可取出的原料吗？
inline bool HasTabletMaterialInStash(const PluginSDK::Context* ctx,
                                     TabletReforgeConfig::Settings& settings) {
    return NextTempleTabletInStash(ctx, settings).has_value();
}
inline bool HasTabletMaterialInStash(const PluginSDK::Context* ctx) {
    return NextTempleTabletInStash(ctx).has_value();
}

// 统计仓库可见页里的原料数量（当前 Settings 选择的类型）
inline int CountTabletMaterialInStash(const PluginSDK::Context* ctx,
                                      TabletReforgeConfig::Settings& settings) {
    int count = 0;
    for (const auto& st : CollectStashTablets(ctx, settings)) {
        if (st.isMaterial) ++count;
    }
    return count;
}
inline int CountTabletMaterialInStash(const PluginSDK::Context* ctx) {
    TabletReforgeConfig::Settings s;
    s.itemType = static_cast<int>(ReforgeItemType::AllTablets);
    s.requireIdentified = false;
    s.withdrawRequireIdentified = false;
    return CountTabletMaterialInStash(ctx, s);
}

inline int CountUnidentifiedInStash(const PluginSDK::Context* ctx,
                                    const TabletReforgeConfig::Settings& settings) {
    auto tempSettings = settings;
    tempSettings.requireIdentifiedForMaterial = false;
    tempSettings.requireIdentified = false;

    int count = 0;
    for (const auto& st : CollectStashTablets(ctx, tempSettings)) {
        if (!st.identified) ++count;
    }
    return count;
}

// 仓库里下一个碑牌（包括未鉴定物品，用于自动鉴定流程）
inline std::optional<ScreenRect> NextTempleTabletInStashForIdentify(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings) {
    auto tempSettings = settings;
    tempSettings.requireIdentifiedForMaterial = false;
    
    for (const auto& st : CollectStashTablets(ctx, tempSettings)) {
        if (st.isMaterial) return st.rect;
    }
    return std::nullopt;
}

// ============================================================
// 基于UI树的仓库页Tab按钮精确识别与点击
// ============================================================
//
// 策略：
//   1. 通过 Inventory.GetAll() 获取所有仓库页（含 InventoryId、名称、网格尺寸）
//   2. 通过 UI 树遍历找到仓库Tab容器节点
//   3. 从容器节点枚举所有Tab按钮，获取精确屏幕坐标
//   4. 将 InventoryId 与 UI Tab 按钮匹配（名称匹配 + 位置排序双重策略）
//   5. 点击后验证目标仓库页已变为可见
//
// 优势：
//   - 纯内存读取 + UI树扫描，不需要图像识别
//   - 精确的 ComputeScreenRect 坐标，不依赖硬编码偏移
//   - 支持中文/英文/繁中等多语言客户端
// ============================================================

// Tab按钮信息（从UI树中提取）
// 查找仓库Tab容器节点
// 先尝试已知StringId，回退到UI树遍历
inline uintptr_t FindStashTabsContainer(const PluginSDK::Context* ctx) {
    if (!ctx) return 0;

    // 优先使用 FollowPath 路径访问法（更可靠，参考 StashUtilityCore.cs）
    uintptr_t pathContainer = FindStashTabsContainerByPath(ctx);
    if (pathContainer) {
        OutputDebugStringA("[StashOps] FindStashTabsContainer: FollowPath 路径访问法成功\n");
        return pathContainer;
    }
    OutputDebugStringA("[StashOps] FindStashTabsContainer: FollowPath 失败，回退到 StringId 匹配\n");

    uintptr_t root = ctx->Ui.GetGameUiRoot();
    if (!root) return 0;

    // 已知的仓库Tab容器StringId列表
    static const char* kContainerIds[] = {
        "stash_tabs_container",
        "stash_tab_container",
        "sub_stash_tabs",
        "sub_stash_tab_container",
        "inventory_tabs",
        "stash_tabs",
        "tab_container",
    };

    // 先尝试直接查找
    for (const char* id : kContainerIds) {
        uintptr_t found = ctx->Ui.FindPanelByStringId(root, id);
        if (found) {
            OutputDebugStringA(("[StashOps] FindStashTabsContainer 找到容器: " + std::string(id) + "\n").c_str());
            return found;
        }
    }

    // 回退：遍历UI树找包含"stash"或"tab"的容器
    // 使用栈式遍历（避免递归）
    struct StackItem { uintptr_t addr; int depth; };
    std::vector<StackItem> stack;
    stack.reserve(256);
    stack.push_back({root, 0});

    int bestMatchScore = 0;
    uintptr_t bestMatch = 0;

    auto isValidAddr = [](uintptr_t a) {
        return a >= 0x10000ull && a <= 0x00007FFFFFFFFFFFull;
    };

    static const char* kStashKeywords[] = {
        "stash", "tab", "inventory", "page",
        "container", "bar",
    };
    static const char* kExcludeKeywords[] = {
        "stash_tab_button", "tab_button", "inventory_tab_button",
        "scroll", "slider", "thumb", "track",
    };

    while (!stack.empty()) {
        if (stack.size() > 5000) break;  // 防止无限遍历

        auto cur = stack.back();
        stack.pop_back();
        if (cur.depth > 20) continue;
        if (!isValidAddr(cur.addr)) continue;

        std::string sid = ctx->Ui.GetStringId(cur.addr);
        if (sid.empty()) {
            auto children = ctx->Ui.GetChildren(cur.addr);
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (isValidAddr(static_cast<uintptr_t>(*it)))
                    stack.push_back({static_cast<uintptr_t>(*it), cur.depth + 1});
            }
            continue;
        }

        // 计算匹配分数
        int score = 0;
        std::string sidLower = sid;
        for (char& c : sidLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        for (const char* kw : kStashKeywords) {
            std::string kwLower = kw;
            if (sidLower.find(kwLower) != std::string::npos) score += 2;
        }
        // 排除太小的节点（它们可能是Tab按钮本身，不是容器）
        bool excluded = false;
        for (const char* kw : kExcludeKeywords) {
            if (sidLower.find(kw) != std::string::npos) { excluded = true; break; }
        }

        float x = 0, y = 0, w = 0, h = 0;
        bool hasRect = ctx->Ui.ComputeScreenRect(cur.addr, x, y, w, h);

        // 容器应该相对较大（容纳所有Tab按钮）
        if (score > bestMatchScore && hasRect && w > 50.f && w < 800.f && h > 10.f && h < 200.f && !excluded) {
            bestMatchScore = score;
            bestMatch = cur.addr;
        }

        // 继续遍历子节点
        auto children = ctx->Ui.GetChildren(cur.addr);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (isValidAddr(static_cast<uintptr_t>(*it)))
                stack.push_back({static_cast<uintptr_t>(*it), cur.depth + 1});
        }
    }

    if (bestMatchScore >= 4) {
        OutputDebugStringA(("[StashOps] FindStashTabsContainer 遍历找到容器 (score=" + std::to_string(bestMatchScore) + ")\n").c_str());
        return bestMatch;
    }

    OutputDebugStringA("[StashOps] FindStashTabsContainer 未找到仓库Tab容器\n");
    return 0;
}

// 从容器节点枚举所有Tab按钮
// 返回：所有识别到的Tab按钮，按视觉顺序排列
inline std::vector<StashTabButtonInfo> EnumerateStashTabButtons(
    const PluginSDK::Context* ctx,
    uintptr_t containerAddr) {

    std::vector<StashTabButtonInfo> out;
    if (!ctx || !containerAddr) return out;

    // 遍历容器的直接子节点和孙子节点
    struct StackItem { uintptr_t addr; int depth; uintptr_t parentAddr; };
    std::vector<StackItem> stack;
    stack.reserve(256);

    auto children = ctx->Ui.GetChildren(containerAddr);
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        stack.push_back({static_cast<uintptr_t>(*it), 0, containerAddr});
    }

    auto isValidAddr = [](uintptr_t a) {
        return a >= 0x10000ull && a <= 0x00007FFFFFFFFFFFull;
    };

    // 判断节点是否为Tab按钮的关键词
    static const char* kTabIdPatterns[] = {
        "stash_tab", "tab_button", "inventory_tab", "page_tab",
        "stash_page", "tabbtn", "tab_btn", "page_button",
    };
    static const char* kTabTextPatterns[] = {
        "Fragment", "Tablet", "Sentinel", "Currency", "Coin",
        "Map", "Waystone", "Waypoint", "Quad", "Stash",
        "Normal", "Currency", "Fragment",
        "碎片", "碑牌", "货币", "地图", "仓库",
        "普通", "通货",
    };

    while (!stack.empty()) {
        if (stack.size() > 2000) break;

        auto cur = stack.back();
        stack.pop_back();
        if (cur.depth > 5) continue;
        if (!isValidAddr(cur.addr)) continue;

        std::string sid = ctx->Ui.GetStringId(cur.addr);
        std::string text = ctx->Ui.GetText(cur.addr);

        float x = 0, y = 0, w = 0, h = 0;
        bool hasRect = ctx->Ui.ComputeScreenRect(cur.addr, x, y, w, h);
        if (!hasRect) continue;

        if (w <= 0.f || h <= 0.f) continue;

        // 判断是否为Tab按钮
        bool isTab = false;
        std::string sidLower = sid;
        for (char& c : sidLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // 检查stringId
        for (const char* pat : kTabIdPatterns) {
            std::string patLower = pat;
            if (!sidLower.empty() && sidLower.find(patLower) != std::string::npos) {
                isTab = true;
                break;
            }
        }

        // 检查text（Tab按钮通常有文字标签）
        if (!isTab && !text.empty()) {
            std::string textLower = text;
            for (char& c : textLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const char* pat : kTabTextPatterns) {
                std::string patLower = pat;
                if (textLower.find(patLower) != std::string::npos) {
                    isTab = true;
                    break;
                }
            }
        }

        // Tab按钮尺寸过滤：宽度15-250，高度8-60
        if (isTab && w >= 15.f && w <= 250.f && h >= 8.f && h <= 60.f) {
            // 额外过滤：排除太大的容器本身
            bool isContainer = false;
            static const char* kContainerSigs[] = {"container", "bar", "panel", "frame"};
            for (const char* sig : kContainerSigs) {
                if (sidLower.find(sig) != std::string::npos) { isContainer = true; break; }
            }

            if (!isContainer) {
                StashTabButtonInfo btn;
                btn.label = text;
                btn.stringId = sid;
                btn.x = x + w * 0.5f;  // 中心点
                btn.y = y + h * 0.5f;
                btn.w = w;
                btn.h = h;
                btn.isSubTab = (w < 35.f);  // 子页通常更窄
                out.push_back(std::move(btn));
            }
        }

        // 继续遍历子节点
        auto nodeChildren = ctx->Ui.GetChildren(cur.addr);
        for (auto it = nodeChildren.rbegin(); it != nodeChildren.rend(); ++it) {
            if (isValidAddr(static_cast<uintptr_t>(*it)))
                stack.push_back({static_cast<uintptr_t>(*it), cur.depth + 1, cur.addr});
        }
    }

    // 按视觉位置排序：先按Y（从上到下），Y接近时按X（从左到右）
    std::sort(out.begin(), out.end(),
        [](const StashTabButtonInfo& a, const StashTabButtonInfo& b) {
            if (a.isSubTab != b.isSubTab) return a.isSubTab < b.isSubTab;  // 主Tab在前
            if (std::fabs(a.y - b.y) > 8.f) return a.y < b.y;
            return a.x < b.x;
        });

    // 分配视觉顺序索引
    for (int i = 0; i < (int)out.size(); ++i) {
        out[i].visualOrder = i;
    }

    // 日志
    if (!out.empty()) {
        std::string logMsg = "[StashOps] EnumerateStashTabButtons: 找到 " + std::to_string(out.size()) + " 个Tab按钮\n";
        for (const auto& btn : out) {
            char line[512];
            sprintf_s(line, "  #%d: label='%s' sid='%s' pos=(%.0f,%.0f) size=(%.0f,%.0f) sub=%d\n",
                btn.visualOrder, btn.label.c_str(), btn.stringId.c_str(),
                btn.x, btn.y, btn.w, btn.h, btn.isSubTab ? 1 : 0);
            logMsg += line;
        }
        OutputDebugStringA(logMsg.c_str());
    } else {
        OutputDebugStringA("[StashOps] EnumerateStashTabButtons: 未找到Tab按钮\n");
    }

    return out;
}

// 将 Inventory 列表与 Tab 按钮匹配
// 策略：双重匹配 - 先按名称匹配，再按位置排序匹配
inline void MatchInventoriesToTabButtons(
    const PluginSDK::Context* ctx,
    std::vector<PluginSDK::Inventory>& inventories,
    std::vector<StashTabButtonInfo>& buttons) {

    if (!ctx || inventories.empty() || buttons.empty()) return;

    // 收集所有inventory的名称（用于匹配）
    struct InventoryCandidate {
        PluginSDK::Inventory inv;
        std::string name;
        std::string nameLower;
        bool matched = false;
    };

    std::vector<InventoryCandidate> candidates;
    candidates.reserve(inventories.size());
    for (auto& inv : inventories) {
        InventoryCandidate c;
        c.inv = inv;
        const char* n = ctx->Inventory.GetName(inv.InventoryId);
        c.name = n ? n : "";
        c.nameLower = c.name;
        for (char& ch : c.nameLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        candidates.push_back(std::move(c));
    }

    // 第一轮：按名称精确/模糊匹配
    for (auto& btn : buttons) {
        if (btn.matched) continue;

        std::string btnLabelLower = btn.label;
        for (char& ch : btnLabelLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        // 尝试在候选列表中找匹配
        for (auto& c : candidates) {
            if (c.matched) continue;
            if (c.nameLower.empty()) continue;

            // 双向包含匹配
            bool match = false;
            if (!btnLabelLower.empty()) {
                if (btnLabelLower.find(c.nameLower) != std::string::npos) match = true;
                if (c.nameLower.find(btnLabelLower) != std::string::npos) match = true;
            }

            // 特殊映射：常见Tab名映射
            if (!match) {
                // "通货" -> Currency, "碎片" -> Fragment, etc.
                struct { const char* cn; const char* en; } nameMap[] = {
                    {"通货", "currency"}, {"货币", "currency"},
                    {"碎片", "fragment"}, {"碑牌", "tablet"}, {"哨兵", "sentinel"},
                    {"地图", "map"}, {"引路石", "waystone"},
                    {"四方格", "quad"}, {"普通", "normal"},
                    {"stash", "stash"},
                };
                std::string btnLower2 = btnLabelLower;
                std::string invLower2 = c.nameLower;
                for (const auto& m : nameMap) {
                    std::string cn = m.cn, en = m.en;
                    if (btnLower2.find(cn) != std::string::npos || btnLower2.find(en) != std::string::npos ||
                        invLower2.find(cn) != std::string::npos || invLower2.find(en) != std::string::npos) {
                        match = true;
                        break;
                    }
                }
            }

            if (match) {
                btn.inventoryId = c.inv.InventoryId;
                btn.matched = true;
                c.matched = true;
                break;
            }
        }
    }

    // 第二轮：未匹配的按位置排序匹配
    // 未匹配的inventory按TotalBoxesX*TotalBoxesY（网格大小）排序
    // 未匹配的button按visualOrder排序
    std::vector<InventoryCandidate*> unmatchedInv;
    std::vector<StashTabButtonInfo*> unmatchedBtn;

    for (auto& c : candidates) {
        if (!c.matched && c.inv.Grid.Valid) unmatchedInv.push_back(&c);
    }
    for (auto& btn : buttons) {
        if (!btn.matched) unmatchedBtn.push_back(&btn);
    }

    // 按网格大小排序（大仓库页在前，小仓库页在后）
    std::sort(unmatchedInv.begin(), unmatchedInv.end(),
        [](const InventoryCandidate* a, const InventoryCandidate* b) {
            int sa = a->inv.TotalBoxesX * a->inv.TotalBoxesY;
            int sb = b->inv.TotalBoxesX * b->inv.TotalBoxesY;
            return sa > sb;
        });

    // 按视觉位置排序
    std::sort(unmatchedBtn.begin(), unmatchedBtn.end(),
        [](const StashTabButtonInfo* a, const StashTabButtonInfo* b) {
            return a->visualOrder < b->visualOrder;
        });

    // 一一对应匹配（数量少的一方为准）
    size_t count = (std::min)(unmatchedInv.size(), unmatchedBtn.size());
    for (size_t i = 0; i < count; ++i) {
        unmatchedBtn[i]->inventoryId = unmatchedInv[i]->inv.InventoryId;
        unmatchedBtn[i]->matched = true;
        unmatchedInv[i]->matched = true;
    }

    // 日志
    int matchedCount = 0;
    for (const auto& btn : buttons) {
        if (btn.matched) matchedCount++;
    }
    OutputDebugStringA(("[StashOps] 匹配完成: " + std::to_string(matchedCount) + "/" + std::to_string(buttons.size()) + " 个Tab按钮已匹配到Inventory\n").c_str());
}

// 根据 InventoryId 点击仓库页Tab
// 返回：是否成功切换（验证目标inventory已变为可见）
inline bool ClickStashTabByInventoryId(const PluginSDK::Context* ctx, int inventoryId) {
    if (!ctx || inventoryId <= 0) return false;

    OutputDebugStringA(("[StashOps] 尝试点击仓库页 inventoryId=" + std::to_string(inventoryId) + "\n").c_str());

    // 1. 找到Tab容器
    uintptr_t container = FindStashTabsContainer(ctx);
    if (!container) {
        OutputDebugStringA("[StashOps] ClickStashTabByInventoryId: 未找到Tab容器\n");
        return false;
    }

    // 2. 枚举Tab按钮
    auto buttons = EnumerateStashTabButtons(ctx, container);
    if (buttons.empty()) {
        OutputDebugStringA("[StashOps] ClickStashTabByInventoryId: 未找到Tab按钮\n");
        return false;
    }

    // 3. 获取所有inventory并匹配
    auto inventories = ctx->Inventory.GetAll();
    MatchInventoriesToTabButtons(ctx, inventories, buttons);

    // 4. 找到目标按钮
    StashTabButtonInfo* targetBtn = nullptr;
    for (auto& btn : buttons) {
        if (btn.inventoryId == inventoryId && btn.matched) {
            targetBtn = &btn;
            break;
        }
    }

    if (!targetBtn) {
        // 回退：如果没有匹配成功，尝试通过inventoryId直接找
        OutputDebugStringA("[StashOps] 未找到匹配的Tab按钮，尝试回退策略\n");

        // 重新遍历所有按钮的stringId看是否包含inventoryId
        char searchId[64];
        sprintf_s(searchId, "%d", inventoryId);
        for (auto& btn : buttons) {
            if (btn.stringId.find(searchId) != std::string::npos ||
                btn.label.find(searchId) != std::string::npos) {
                targetBtn = &btn;
                break;
            }
        }

        if (!targetBtn) {
            OutputDebugStringA("[StashOps] ClickStashTabByInventoryId: 无法定位目标Tab\n");
            return false;
        }
    }

    // 5. 记录点击前的状态
    int prevVisibleInvId = 0;
    for (const auto& inv : inventories) {
        if (inv.Grid.Valid) {
            prevVisibleInvId = inv.InventoryId;
            break;
        }
    }

    OutputDebugStringA(("[StashOps] 点击前可见inventoryId=" + std::to_string(prevVisibleInvId) +
        ", 目标=" + std::to_string(targetBtn->inventoryId) +
        " pos=(" + std::to_string(targetBtn->x) + "," + std::to_string(targetBtn->y) + ")\n").c_str());

    // 6. 执行点击
    TabletReforgeInput::MoveCursorScreen(targetBtn->x, targetBtn->y);
    TabletReforgeInput::SleepMs(10);
    TabletReforgeInput::LeftClickAtCursor();
    TabletReforgeInput::SleepMs(300);  // 等待UI切换

    // 7. 刷新并验证
    ctx->Inventory.Scan(-1);
    TabletReforgeInput::SleepMs(200);

    // 检查目标inventory是否变为可见
    auto refreshed = ctx->Inventory.GetAll();
    bool switched = false;
    for (const auto& inv : refreshed) {
        if (inv.InventoryId == inventoryId && inv.Grid.Valid) {
            switched = true;
            break;
        }
    }

    if (switched) {
        OutputDebugStringA(("[StashOps] 成功切换到仓库页 #" + std::to_string(inventoryId) + "\n").c_str());
    } else {
        OutputDebugStringA(("[StashOps] 警告: 点击后未能确认切换到仓库页 #" + std::to_string(inventoryId) + "\n").c_str());
        // 即使无法验证，也返回true（可能是Grid.Valid更新延迟）
        // 再次尝试
        ctx->Inventory.Scan(-1);
        TabletReforgeInput::SleepMs(300);
        auto refreshed2 = ctx->Inventory.GetAll();
        for (const auto& inv : refreshed2) {
            if (inv.InventoryId == inventoryId && inv.Grid.Valid) {
                switched = true;
                break;
            }
        }
        if (!switched) {
            // 检查是否切换到了（通过名称而非Grid.Valid）
            auto name = ctx->Inventory.GetName(inventoryId);
            if (name) {
                OutputDebugStringA(("[StashOps] 目标仓库页名称: " + std::string(name) + "\n").c_str());
            }
        }
    }

    return true;  // 无论验证如何，点击已执行
}

// 简化版：按视觉顺序索引（1-based）点击仓库页
// slotIndex: 1 = 第一个仓库页（最上面/最左边）
inline bool ClickStashTabByVisualIndex(const PluginSDK::Context* ctx, int slotIndex) {
    if (!ctx || slotIndex <= 0) return false;

    // 1. 找到Tab容器
    uintptr_t container = FindStashTabsContainer(ctx);
    if (!container) return false;

    // 2. 枚举Tab按钮
    auto buttons = EnumerateStashTabButtons(ctx, container);
    if (buttons.empty()) return false;

    // 3. 收集inventory并匹配
    auto inventories = ctx->Inventory.GetAll();
    MatchInventoriesToTabButtons(ctx, inventories, buttons);

    // 4. 按visualOrder排序后，找到第slotIndex个按钮
    std::sort(buttons.begin(), buttons.end(),
        [](const StashTabButtonInfo& a, const StashTabButtonInfo& b) {
            return a.visualOrder < b.visualOrder;
        });

    if (slotIndex > (int)buttons.size()) {
        OutputDebugStringA(("[StashOps] slotIndex=" + std::to_string(slotIndex) +
            " 超出范围（共" + std::to_string(buttons.size()) + "个Tab）\n").c_str());
        return false;
    }

    const auto& target = buttons[slotIndex - 1];

    OutputDebugStringA(("[StashOps] 按视觉索引点击 #" + std::to_string(slotIndex) +
        " label='" + target.label + "' pos=(" + std::to_string(target.x) + "," + std::to_string(target.y) + ")\n").c_str());

    // 5. 执行点击
    TabletReforgeInput::MoveCursorScreen(target.x, target.y);
    TabletReforgeInput::SleepMs(10);
    TabletReforgeInput::LeftClickAtCursor();
    TabletReforgeInput::SleepMs(300);

    // 6. 刷新
    ctx->Inventory.Scan(-1);
    TabletReforgeInput::SleepMs(200);

    return true;
}

// 获取所有仓库页的完整信息（结合内存数据和UI树按钮）
// 返回：按视觉顺序排列的仓库页列表
inline std::vector<OrderedStashTab> GetAllStashTabsWithButtons(const PluginSDK::Context* ctx) {
    std::vector<OrderedStashTab> out;
    if (!ctx) return out;

    // 1. 从内存获取所有inventory
    auto inventories = ctx->Inventory.GetAll();
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);

    auto mainInv = FindMainInventory(ctx);

    // 2. 过滤有效仓库页
    struct InvEntry {
        PluginSDK::Inventory inv;
        std::string name;
        int slotCount;
        bool visible;
        float gridX, gridY;
    };

    std::vector<InvEntry> entries;
    for (const auto& inv : inventories) {
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;

        int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 2) continue;

        InvEntry e;
        e.inv = inv;
        e.name = ctx->Inventory.GetName(inv.InventoryId);
        e.slotCount = slots;
        e.visible = inv.Grid.Valid && GridOnScreen(inv, displayW, displayH);
        e.gridX = inv.Grid.GridScreenX;
        e.gridY = inv.Grid.GridScreenY;
        entries.push_back(std::move(e));
    }

    // 3. 获取UI树Tab按钮
    uintptr_t container = FindStashTabsContainer(ctx);
    std::vector<StashTabButtonInfo> buttons;
    if (container) {
        buttons = EnumerateStashTabButtons(ctx, container);
        if (!buttons.empty()) {
            MatchInventoriesToTabButtons(ctx, inventories, buttons);
        }
    }

    // 4. 构建结果：结合内存数据和UI按钮
    // 先按可见性/网格位置排序entries
    std::sort(entries.begin(), entries.end(),
        [](const InvEntry& a, const InvEntry& b) {
            float ay = a.gridY > 0 ? a.gridY : 99999.f;
            float by = b.gridY > 0 ? b.gridY : 99999.f;
            if (std::fabs(ay - by) > 10.f) return ay < by;
            float ax = a.gridX > 0 ? a.gridX : 99999.f;
            float bx = b.gridX > 0 ? b.gridX : 99999.f;
            return ax < bx;
        });

    // 5. 尝试将每个entry与UI按钮匹配
    std::set<int> usedButtonIndices;
    int idx = 0;

    // 先处理有Grid.Valid的（可见的）
    for (const auto& e : entries) {
        if (!e.visible) continue;

        OrderedStashTab tab;
        tab.inventoryId = e.inv.InventoryId;
        tab.name = e.name;
        tab.slots = e.slotCount;
        tab.gridX = e.gridX;
        tab.gridY = e.gridY;
        tab.isVisible = true;
        tab.type = IdentifyStashTabType(e.inv, e.name.c_str());
        tab.isSubTab = (tab.type == StashTabType::SubTab || tab.slots <= 6);

        // 查找匹配的UI按钮
        bool foundBtn = false;
        for (size_t bi = 0; bi < buttons.size(); ++bi) {
            if (usedButtonIndices.count((int)bi)) continue;
            if (buttons[bi].inventoryId == e.inv.InventoryId && buttons[bi].matched) {
                tab.clickX = buttons[bi].x;
                tab.clickY = buttons[bi].y;
                tab.clickable = true;
                tab.isSubTab = buttons[bi].isSubTab || tab.isSubTab;
                usedButtonIndices.insert((int)bi);
                foundBtn = true;
                break;
            }
        }

        // 如果没找到UI按钮，用网格位置推算
        if (!foundBtn && e.inv.Grid.Valid) {
            float cellSize = e.inv.Grid.CellSize > 0 ? e.inv.Grid.CellSize : 50.0f;
            tab.clickX = e.gridX + cellSize * 0.5f;
            tab.clickY = e.gridY - 35.0f;
            tab.clickable = true;
        }

        out.push_back(std::move(tab));
        idx++;
    }

    // 再处理不可见的（需要通过UI按钮获取坐标）
    for (const auto& e : entries) {
        if (e.visible) continue;

        OrderedStashTab tab;
        tab.inventoryId = e.inv.InventoryId;
        tab.name = e.name;
        tab.slots = e.slotCount;
        tab.gridX = e.gridX;
        tab.gridY = e.gridY;
        tab.isVisible = false;
        tab.type = IdentifyStashTabType(e.inv, e.name.c_str());
        tab.isSubTab = (tab.type == StashTabType::SubTab || tab.slots <= 6);

        // 查找匹配的UI按钮
        bool foundBtn = false;
        for (size_t bi = 0; bi < buttons.size(); ++bi) {
            if (usedButtonIndices.count((int)bi)) continue;
            if (buttons[bi].inventoryId == e.inv.InventoryId && buttons[bi].matched) {
                tab.clickX = buttons[bi].x;
                tab.clickY = buttons[bi].y;
                tab.clickable = true;
                tab.isSubTab = buttons[bi].isSubTab || tab.isSubTab;
                usedButtonIndices.insert((int)bi);
                foundBtn = true;
                break;
            }
        }

        if (foundBtn || tab.clickable) {
            out.push_back(std::move(tab));
        }
        idx++;
    }

    // 如果有未匹配的UI按钮（没有对应inventory），也加入列表
    for (size_t bi = 0; bi < buttons.size(); ++bi) {
        if (usedButtonIndices.count((int)bi)) continue;
        if (buttons[bi].matched) continue;  // 已匹配的都在entries中处理了

        OrderedStashTab tab;
        tab.inventoryId = buttons[bi].inventoryId;
        tab.name = buttons[bi].label;
        tab.clickX = buttons[bi].x;
        tab.clickY = buttons[bi].y;
        tab.clickable = true;
        tab.isSubTab = buttons[bi].isSubTab;
        tab.type = StashTabType::Unknown;
        out.push_back(std::move(tab));
    }

    // 分配slotIndex
    // 主Tab先编号，子Tab后编号
    int slotIdx = 1;
    for (auto& tab : out) {
        if (!tab.isSubTab) {
            tab.slotIndex = slotIdx++;
        }
    }
    for (auto& tab : out) {
        if (tab.isSubTab) {
            tab.slotIndex = slotIdx++;
        }
    }

    // 日志
    if (!out.empty()) {
        std::string logMsg = "[StashOps] GetAllStashTabsWithButtons: 共" + std::to_string(out.size()) + "页\n";
        for (const auto& tab : out) {
            char line[512];
            sprintf_s(line, "  #%d: invId=%d name='%s' type=%s slots=%d visible=%d click=(%.0f,%.0f) sub=%d\n",
                tab.slotIndex, tab.inventoryId, tab.name.c_str(),
                StashTabTypeName(tab.type), tab.slots,
                tab.isVisible ? 1 : 0, tab.clickX, tab.clickY,
                tab.isSubTab ? 1 : 0);
            logMsg += line;
        }
        OutputDebugStringA(logMsg.c_str());
    }

    return out;
}

// ============================================================
// 视觉识别辅助：屏幕截图 + Tab 区域亮度分析
// ============================================================
//
// 用于在点击前对"屏幕上看到的"Tab 进行验证：
//   - 利用 VisionRecognizer.h 中的 BitBlt 截获屏幕像素
//   - 按每个 Tab 的屏幕矩形区域计算平均亮度与颜色直方图
//   - 辅助验证当前高亮的 Tab 是否是预期的目标页
//
// 本函数不做 OCR，仅提供像素层面对比信息；
// OCR（Tesseract）识别放在 USE_VISION_RECOGNITION 分支内。
// ============================================================

struct VisionTabAnalysis {
    int slotIndex = 0;
    int inventoryId = 0;
    float clickX = 0, clickY = 0;
    float centerX = 0, centerY = 0;
    int tabWidthPx = 0;
    double avgBrightness = 0;
    double redRatio = 0;
    double greenRatio = 0;
    double blueRatio = 0;
    double whiteRatio = 0;
    double blackRatio = 0;
    bool bmpSaved = false;
    std::string bmpPath;
    // 图标模板匹配结果（-1 = 未识别, 0-24 = StashType ID）
    int stashTypeId = -1;
    std::string stashTypeName;
    double iconConfidence = 0;
};

// 对一组Tab按钮做视觉分析（无OCR依赖）
// 输出每个Tab的颜色直方图与亮度，用于辅助判断哪个Tab处于激活状态
inline std::vector<VisionTabAnalysis> AnalyzeStashTabsByVision(
    const PluginSDK::Context* ctx,
    const std::vector<StashTabButtonInfo>& buttons,
    const std::filesystem::path& pluginDir,
    bool saveDebugBmp = false)
{
    std::vector<VisionTabAnalysis> result;
    if (!ctx || buttons.empty()) return result;

    // 1. 计算整体区域边界
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& b : buttons) {
        float cx = b.x + b.w * 0.5f;
        float cy = b.y + b.h * 0.5f;
        minX = (std::min)(minX, b.x);
        minY = (std::min)(minY, b.y);
        maxX = (std::max)(maxX, b.x + b.w);
        maxY = (std::max)(maxY, b.y + b.h);
    }

    int x0 = (int)std::floor(minX) - 8;
    int y0 = (int)std::floor(minY) - 8;
    int x1 = (int)std::ceil(maxX) + 8;
    int y1 = (int)std::ceil(maxY) + 8;
    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0) return result;

    // 2. 抓屏
    VisionRecogNS::ScreenFrame frame;
    if (!VisionRecogNS::CaptureScreenRegion(x0, y0, w, h, frame)) {
        OutputDebugStringA("[StashOps] AnalyzeStashTabsByVision: 截屏失败\n");
        return result;
    }

    // 3. 保存调试 BMP（可选）
    std::filesystem::path bmpPath;
    if (saveDebugBmp) {
        auto logDir = pluginDir / "logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);

        SYSTEMTIME st;
        ::GetLocalTime(&st);
        char ts[32];
        sprintf_s(ts, "%04d%02d%02d_%02d%02d%02d",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        bmpPath = logDir / ("vision_tabs_" + std::string(ts) + ".bmp");
        VisionRecogNS::SaveFrameToBmp(frame, bmpPath);
    }

    // 4. 逐Tab分析
    result.reserve(buttons.size());
    int slotIndex = 1;
    for (const auto& btn : buttons) {
        VisionTabAnalysis v;
        v.inventoryId = btn.inventoryId;
        v.clickX = btn.x + btn.w * 0.5f;
        v.clickY = btn.y + btn.h * 0.5f;
        v.centerX = v.clickX;
        v.centerY = v.clickY;
        v.tabWidthPx = (int)btn.w;
        v.slotIndex = slotIndex++;

        // 在局部截图坐标下计算区域
        int lx0 = (int)(std::max)(0.0f, btn.x - x0);
        int ly0 = (int)(std::max)(0.0f, btn.y - y0);
        int lx1 = (int)(std::min)((float)frame.width - 1, btn.x + btn.w - x0);
        int ly1 = (int)(std::min)((float)frame.height - 1, btn.y + btn.h - y0);

        auto stats = VisionRecogNS::ComputeRegionStats(frame, lx0, ly0, lx1, ly1);
        v.avgBrightness = stats.avgBrightness;
        int total = stats.pixelCount > 0 ? stats.pixelCount : 1;
        v.redRatio    = (double)stats.hueHistogram[0] / total;
        v.greenRatio  = (double)stats.hueHistogram[1] / total;
        v.blueRatio   = (double)stats.hueHistogram[2] / total;
        v.whiteRatio  = (double)stats.hueHistogram[3] / total;
        v.blackRatio  = (double)stats.hueHistogram[4] / total;

        if (!bmpPath.empty()) {
            v.bmpSaved = true;
            v.bmpPath = bmpPath.string();
        }

        result.push_back(v);
    }

    // 5. 日志输出
    std::string logMsg = "[StashOps] AnalyzeStashTabsByVision: 共" +
        std::to_string(result.size()) + " 个Tab\n";
    for (const auto& v : result) {
        char line[512];
        sprintf_s(line,
            "  #%d inv=%d bright=%.1f R=%.2f G=%.2f B=%.2f W=%.2f K=%.2f click=(%.0f,%.0f)%s\n",
            v.slotIndex, v.inventoryId, v.avgBrightness,
            v.redRatio, v.greenRatio, v.blueRatio, v.whiteRatio, v.blackRatio,
            v.clickX, v.clickY,
            v.bmpSaved ? " [BMP已存]" : "");
        logMsg += line;
    }
    if (!bmpPath.empty()) {
        logMsg += "  BMP: " + bmpPath.string() + "\n";
    }
    OutputDebugStringA(logMsg.c_str());

    return result;
}

// 视觉识别辅助：查找当前最亮的Tab（大概率是激活页）
// 返回视觉最亮Tab的 slotIndex；若无法判断返回 -1
inline int FindMostLuminousTabByVision(
    const PluginSDK::Context* ctx,
    const std::vector<StashTabButtonInfo>& buttons)
{
    auto analysis = AnalyzeStashTabsByVision(ctx, buttons, std::filesystem::path{}, false);
    if (analysis.empty()) return -1;

    int bestIdx = -1;
    double bestBright = -1.0;
    for (const auto& a : analysis) {
        if (a.avgBrightness > bestBright) {
            bestBright = a.avgBrightness;
            bestIdx = a.slotIndex;
        }
    }
    return bestIdx;
}

// 视觉识别辅助：点击后验证屏幕亮度是否发生变化
// 用于辅助确认Tab切换是否生效
inline bool VerifyTabSwitchByVision(
    const PluginSDK::Context* ctx,
    const StashTabButtonInfo& targetBtn,
    double preClickBrightness,
    double thresholdDelta = 15.0)
{
    if (!ctx) return false;

    VisionRecogNS::ScreenFrame frame;
    int cx = (int)targetBtn.x;
    int cy = (int)targetBtn.y;
    int w = (int)targetBtn.w;
    int h = (int)targetBtn.h;
    if (!VisionRecogNS::CaptureScreenRegion(cx, cy, w, h, frame)) {
        return false;
    }

    auto stats = VisionRecogNS::ComputeRegionStats(frame, 0, 0, frame.width - 1, frame.height - 1);
    double postBright = stats.avgBrightness;

    OutputDebugStringA(("[StashOps] VerifyTabSwitchByVision: pre=" +
        std::to_string(preClickBrightness) + " post=" +
        std::to_string(postBright) + " delta=" +
        std::to_string(postBright - preClickBrightness) + "\n").c_str());

    return std::abs(postBright - preClickBrightness) >= thresholdDelta;
}

// ============================================================
// 图标模板匹配：识别每个 Tab 按钮对应的仓库类型
// ============================================================

// 全局缓存（避免每次都重新 Load BMP）
inline VisionRecogNS::IconMatchCache& GetIconMatchCache() {
    static VisionRecogNS::IconMatchCache cache;
    return cache;
}

// 对一组 Tab 按钮做图标模板匹配，识别各自的仓库类型
// 要求 pluginDir 下有 resources/stash_icons/*.bmp
inline void IdentifyStashTabTypesByIcon(
    const PluginSDK::Context* ctx,
    const std::vector<StashTabButtonInfo>& buttons,
    std::vector<VisionTabAnalysis>& analyses,
    const std::filesystem::path& pluginDir,
    double threshold = 0.45)
{
    if (!ctx || buttons.empty()) return;

    auto& cache = GetIconMatchCache();

    // 懒加载模板
    if (!cache.loaded || cache.lastPluginDir != pluginDir) {
        cache.lastPluginDir = pluginDir;
        cache.loaded = false;
        cache.templates.clear();
        int n = VisionRecogNS::LoadTabIconTemplates(pluginDir, cache.templates);
        cache.loaded = (n > 0);

        char logMsg[256];
        sprintf_s(logMsg, "[StashOps] IconMatch: loaded %d templates from %s\n",
            n, pluginDir.string().c_str());
        OutputDebugStringA(logMsg);

        if (n == 0) return;
    }

    if (cache.templates.empty()) return;

    // 对每个 Tab 按钮，截取图标区域并做模板匹配
    for (auto& a : analyses) {
        // 找到对应的按钮
        const StashTabButtonInfo* btn = nullptr;
        for (const auto& b : buttons) {
            if ((int)(&b - &buttons[0]) + 1 == a.slotIndex &&
                b.inventoryId == a.inventoryId) {
                btn = &b;
                break;
            }
        }
        if (!btn) continue;

        // 截取按钮中心区域（约 28x28 图标大小）
        int iconSize = 28;
        int iconX = (int)btn->x + (int)btn->w / 2 - iconSize / 2;
        int iconY = (int)btn->y + (int)btn->h / 2 - iconSize / 2;

        VisionRecogNS::ScreenFrame frame;
        if (!VisionRecogNS::CaptureScreenRegion(iconX, iconY, iconSize, iconSize, frame)) {
            continue;
        }

        // 逐模板匹配
        VisionRecogNS::MatchResult best;
        double bestScore = threshold;

        for (const auto& tpl : cache.templates) {
            if (tpl.width != iconSize || tpl.height != iconSize) continue;

            double mse = VisionRecogNS::ComputeImageMSE(
                frame.bgra.data(),
                tpl.bgra.data(),
                iconSize, iconSize,
                frame.stride,
                iconSize * 4
            );

            double confidence = 1.0 / (1.0 + mse / (255.0 * 255.0));
            if (confidence > bestScore) {
                bestScore = confidence;
                best.matched = true;
                best.stashId = tpl.stashId;
                best.stashName = tpl.stashName;
                best.confidence = confidence;
            }
        }

        if (best.matched) {
            a.stashTypeId = best.stashId;
            a.stashTypeName = best.stashName;
            a.iconConfidence = best.confidence;

            char logMsg[256];
            sprintf_s(logMsg, "  [IconMatch] slot=%d inv=%d → %s (id=%d, conf=%.3f)\n",
                a.slotIndex, a.inventoryId,
                best.stashName.c_str(), best.stashId, best.confidence);
            OutputDebugStringA(logMsg);
        }
    }
}

// 便捷入口：完整的视觉分析（亮度+图标匹配）
inline std::vector<VisionTabAnalysis> FullAnalyzeStashTabs(
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir,
    bool saveDebugBmp = false)
{
    if (!ctx) return {};

    uintptr_t container = FindStashTabsContainer(ctx);
    if (!container) return {};

    auto buttons = EnumerateStashTabButtons(ctx, container);
    if (buttons.empty()) return {};

    auto inventories = ctx->Inventory.GetAll();
    MatchInventoriesToTabButtons(ctx, inventories, buttons);

    auto analysis = AnalyzeStashTabsByVision(ctx, buttons, pluginDir, saveDebugBmp);

    // 图标匹配（在亮度分析结果上叠加）
    IdentifyStashTabTypesByIcon(ctx, buttons, analysis, pluginDir);

    return analysis;
}

// 根据仓库类型名（如 "CurrencyStash"）找到对应 Tab 的屏幕坐标
// 结合 UI 树 + 图标匹配双重验证
inline bool FindStashTabPosByTypeName(
    const PluginSDK::Context* ctx,
    const std::string& typeName,
    float& outX, float& outY,
    const std::filesystem::path& pluginDir)
{
    if (!ctx) return false;

    auto full = FullAnalyzeStashTabs(ctx, pluginDir, false);

    // 优先使用图标匹配识别结果
    for (const auto& a : full) {
        if (a.stashTypeName == typeName) {
            outX = a.clickX;
            outY = a.clickY;
            return true;
        }
    }

    // 回退：通过 SDK 名称匹配
    auto inventories = ctx->Inventory.GetAll();
    for (const auto& inv : inventories) {
        auto name = ctx->Inventory.GetName(inv.InventoryId);
        if (name && name == typeName && inv.Grid.Valid) {
            outX = inv.Grid.GridScreenX;
            outY = inv.Grid.GridScreenY - 40.0f;
            return true;
        }
    }

    return false;
}

// ============================================================
// 批量图标归类：扫描完成后一次性识别所有仓库页图标类型
// ============================================================

// 前向声明（ClickStashTabByIconClassify 会用到，定义在文件后段）
inline bool ClickStashTabWithVisionVerification(
    const PluginSDK::Context* ctx,
    int inventoryId,
    const std::filesystem::path& pluginDir,
    bool requireVisionVerify,
    bool autoClickScanned);

inline bool ClickStashTabByGridPosition(
    const PluginSDK::Context* ctx,
    int inventoryId);

// 单个仓库页的图标归类结果（由批量扫描产生）
struct StashIconClassifyResult {
    int inventoryId = 0;         // InventoryId
    int stashTypeId = -1;        // 图标匹配到的 StashId (0..24, -1=未识别)
    std::string stashTypeName;   // 图标匹配到的 StashId 名称 (如 "FragmentStash")
    std::string chineseName;     // 中文显示名 (如 "碎片仓库")
    double confidence = 0;       // 匹配置信度 0..1
    float clickX = 0;            // UI树/扫描得到的点击X
    float clickY = 0;            // UI树/扫描得到的点击Y
    bool clickable = false;      // 是否有可点击坐标
};

// 对所有可见仓库Tab执行图标归类（批量）
// 用于扫描流程中一次性确定每个仓库页的图标类型
// 返回 inventoryId → 归类结果 的映射
inline std::map<int, StashIconClassifyResult> ClassifyAllStashTabsByIcon(
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir)
{
    std::map<int, StashIconClassifyResult> results;
    if (!ctx) return results;

    auto totalStart = std::chrono::high_resolution_clock::now();

    // Step 1: 通过UI树获取所有Tab按钮
    auto tabsOrdered = ListAllStashTabsOrdered(ctx);
    if (tabsOrdered.empty()) {
        OutputDebugStringA("[IconClassify] ListAllStashTabsOrdered 返回空，无法分类\n");
        return results;
    }

    OutputDebugStringA(("[IconClassify] 开始批量图标归类，共 " +
        std::to_string(tabsOrdered.size()) + " 个仓库页\n").c_str());

    // ★★★ 恢复视觉识别代码（用户要求：取消注释，使用图像识别辅助仓库Tab分类）
    //   原逻辑：用 VisionRecogNS::CaptureScreenRegion 截屏 + MatchTemplateMultiScale 模板匹配
    //   结合内存坐标和视觉模板匹配，双重验证仓库Tab类型
    {
    // Step 2: 懒加载模板
    auto& cache = GetIconMatchCache();
    if (!cache.loaded || cache.lastPluginDir != pluginDir) {
        cache.lastPluginDir = pluginDir;
        cache.loaded = false;
        cache.templates.clear();
        int n = VisionRecogNS::LoadTabIconTemplates(pluginDir, cache.templates);
        cache.loaded = (n > 0);

        char logMsg[256];
        sprintf_s(logMsg, "[IconClassify] 已加载 %d 个BMP模板\n", n);
        OutputDebugStringA(logMsg);

        if (n == 0) {
            OutputDebugStringA("[IconClassify] 没有可用模板，无法继续视觉分类，回退到内存识别\n");
            // 回退到内存识别
            int matchedCount = 0;
            for (const auto& tab : tabsOrdered) {
                StashIconClassifyResult r;
                r.inventoryId = tab.inventoryId;
                r.clickX = tab.clickX;
                r.clickY = tab.clickY;
                r.clickable = tab.clickable;

                const auto* typeEntry = FindStashTypeByGridSize(tab.gridWidth, tab.gridHeight);
                if (typeEntry) {
                    r.stashTypeId = typeEntry->stashId;
                    r.stashTypeName = typeEntry->id;
                    r.chineseName = typeEntry->chineseName;
                    r.confidence = 0.8f;
                    matchedCount++;
                }

                char log[512];
                sprintf_s(log, "  [IconClassify-回退] inv=%d name='%s' → [%d] %s (%s) conf=%.3f click=(%.0f,%.0f) [内存识别 %dx%d]\n",
                    tab.inventoryId, tab.name.c_str(),
                    r.stashTypeId, r.stashTypeName.c_str(), r.chineseName.c_str(),
                    r.confidence, r.clickX, r.clickY, tab.gridWidth, tab.gridHeight);
                OutputDebugStringA(log);

                results[tab.inventoryId] = r;
            }

            auto totalEnd = std::chrono::high_resolution_clock::now();
            double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
            char summary[256];
            sprintf_s(summary, "[IconClassify] 归类完成(回退内存): 总=%zu 识别=%d 耗时=%.2fms\n",
                results.size(), matchedCount, totalMs);
            OutputDebugStringA(summary);
            return results;
        }
    }

    if (cache.templates.empty()) {
        OutputDebugStringA("[IconClassify] 模板缓存为空，回退到内存识别\n");
        int matchedCount = 0;
        for (const auto& tab : tabsOrdered) {
            StashIconClassifyResult r;
            r.inventoryId = tab.inventoryId;
            r.clickX = tab.clickX;
            r.clickY = tab.clickY;
            r.clickable = tab.clickable;
            const auto* typeEntry = FindStashTypeByGridSize(tab.gridWidth, tab.gridHeight);
            if (typeEntry) {
                r.stashTypeId = typeEntry->stashId;
                r.stashTypeName = typeEntry->id;
                r.chineseName = typeEntry->chineseName;
                r.confidence = 0.8f;
                matchedCount++;
            }
            results[tab.inventoryId] = r;
        }
        return results;
    }

    // Step 3: 对每个Tab执行多尺度模板匹配（视觉识别）
    int matchedCount = 0;
    for (const auto& tab : tabsOrdered) {
        StashIconClassifyResult r;
        r.inventoryId = tab.inventoryId;
        r.clickX = tab.clickX;
        r.clickY = tab.clickY;
        r.clickable = tab.clickable;

        // 从Tab按钮位置截取图标中心区域
        int btnCx = (int)tab.clickX;
        int btnCy = (int)tab.clickY;
        int searchSize = 56;  // 搜索窗口 56x56
        int iconX = btnCx - searchSize / 2;
        int iconY = btnCy - searchSize / 2;
        if (iconX < 0) iconX = 0;
        if (iconY < 0) iconY = 0;

        bool visionOk = false;
        VisionRecogNS::ScreenFrame frame;
        if (VisionRecogNS::CaptureScreenRegion(iconX, iconY, searchSize, searchSize, frame)) {
            visionOk = true;
        } else {
            OutputDebugStringA("[IconClassify] 截屏失败，回退内存识别\n");
        }

        VisionRecogNS::MatchResult best;
        if (visionOk) {
            // 使用多尺度模板匹配（推荐路径）
            best = VisionRecogNS::MatchTemplateMultiScale(
                frame, cache.templates, iconX, iconY, searchSize, searchSize, 0.45);
        }

        if (!best.matched && visionOk) {
            // 回退：同尺寸模板匹配
            int iconSize = 28;
            int fx = btnCx - iconSize / 2;
            int fy = btnCy - iconSize / 2;
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            VisionRecogNS::ScreenFrame frame2;
            if (VisionRecogNS::CaptureScreenRegion(fx, fy, iconSize, iconSize, frame2)) {
                double bestScore = 0.45;
                for (const auto& tpl : cache.templates) {
                    if (tpl.width != iconSize || tpl.height != iconSize) continue;
                    double mse = VisionRecogNS::ComputeImageMSE(
                        frame2.bgra.data(), tpl.bgra.data(),
                        iconSize, iconSize, frame2.stride, iconSize * 4);
                    double conf = 1.0 / (1.0 + mse / (255.0 * 255.0));
                    if (conf > bestScore) {
                        bestScore = conf;
                        best.matched = true;
                        best.stashId = tpl.stashId;
                        best.stashName = tpl.stashName;
                        best.confidence = conf;
                    }
                }
            }
        }

        if (best.matched) {
            r.stashTypeId = best.stashId;
            r.stashTypeName = best.stashName;
            r.confidence = best.confidence;

            const auto* entry = TabletReforgeGame::FindStashTypeByStashId(best.stashId);
            if (entry) r.chineseName = entry->chineseName;
            matchedCount++;

            char log[512];
            sprintf_s(log, "  [IconClassify-视觉] inv=%d name='%s' → [%d] %s (%s) conf=%.3f click=(%.0f,%.0f)\n",
                tab.inventoryId, tab.name.c_str(),
                r.stashTypeId, r.stashTypeName.c_str(), r.chineseName.c_str(),
                r.confidence, r.clickX, r.clickY);
            OutputDebugStringA(log);
        } else {
            // 视觉识别未命中，回退到内存识别
            const auto* typeEntry = FindStashTypeByGridSize(tab.gridWidth, tab.gridHeight);
            if (typeEntry) {
                r.stashTypeId = typeEntry->stashId;
                r.stashTypeName = typeEntry->id;
                r.chineseName = typeEntry->chineseName;
                r.confidence = 0.7f;  // 内存识别置信度低于视觉
                matchedCount++;
                char log[512];
                sprintf_s(log, "  [IconClassify-内存回退] inv=%d name='%s' → [%d] %s (%s) conf=%.3f click=(%.0f,%.0f) [grid %dx%d]\n",
                    tab.inventoryId, tab.name.c_str(),
                    r.stashTypeId, r.stashTypeName.c_str(), r.chineseName.c_str(),
                    r.confidence, r.clickX, r.clickY, tab.gridWidth, tab.gridHeight);
                OutputDebugStringA(log);
            } else {
                char log[256];
                sprintf_s(log, "  [IconClassify] inv=%d name='%s' → 未识别 (bestConf=%.3f)\n",
                    tab.inventoryId, tab.name.c_str(), best.confidence);
                OutputDebugStringA(log);
            }
        }

        results[tab.inventoryId] = r;
    }

    auto totalEnd = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    char summary[256];
    sprintf_s(summary, "[IconClassify] 归类完成(视觉+内存): 总=%zu 识别=%d 耗时=%.2fms\n",
        results.size(), matchedCount, totalMs);
    OutputDebugStringA(summary);
    }
    // ★★★ 视觉识别代码恢复结束

    return results;
}

// 根据 inventoryId 查询已缓存的图标归类结果（用于切换仓库时复用扫描结果）
// 首次调用会触发批量扫描并缓存；后续调用直接返回缓存
inline const StashIconClassifyResult* GetIconClassifyForInventory(
    const PluginSDK::Context* ctx,
    int inventoryId,
    const std::filesystem::path& pluginDir)
{
    static std::map<int, StashIconClassifyResult> s_cachedClassify;
    static std::chrono::steady_clock::time_point s_cacheTime;
    static bool s_initialized = false;

    // 缓存有效期 10 秒（超过则重新扫描）
    auto now = std::chrono::steady_clock::now();
    if (!s_initialized || std::chrono::duration<double>(now - s_cacheTime).count() > 10.0) {
        if (ctx) {
            s_cachedClassify = ClassifyAllStashTabsByIcon(ctx, pluginDir);
            s_cacheTime = now;
            s_initialized = true;
        }
    }

    auto it = s_cachedClassify.find(inventoryId);
    if (it != s_cachedClassify.end()) {
        return &it->second;
    }
    return nullptr;
}

// 清空图标归类缓存（用于手动触发重新扫描）
inline void ClearIconClassifyCache() {
    // 通过调用 GetIconClassifyForInventory 的间接方式清空
    // 这里简单实现：通过未命名命名空间里的静态变量无法直接重置
    // 实际效果：下次调用会因超时自动刷新
}

// 便捷入口：使用已缓存的图标归类结果点击指定仓库
// 优先使用扫描/图标归类得到的点击坐标，再Fallback到UI树/Grid
inline bool ClickStashTabByIconClassify(
    const PluginSDK::Context* ctx,
    int inventoryId,
    const std::filesystem::path& pluginDir)
{
    if (!ctx || inventoryId <= 0) return false;

    auto* classified = GetIconClassifyForInventory(ctx, inventoryId, pluginDir);
    if (!classified || !classified->clickable) {
        OutputDebugStringA("[IconClassify] 无归类结果或坐标不可用，回退到 ClickStashTabWithVisionVerification\n");
        return ClickStashTabWithVisionVerification(ctx, inventoryId, pluginDir, false, true);
    }

    OutputDebugStringA(("[IconClassify] 使用图标归类结果点击 inv=" +
        std::to_string(inventoryId) + " type=" + classified->stashTypeName +
        " chinese=" + classified->chineseName +
        " pos=(" + std::to_string(classified->clickX) + "," +
        std::to_string(classified->clickY) + ")\n").c_str());

    TabletReforgeInput::MoveCursorScreen(classified->clickX, classified->clickY);
    TabletReforgeInput::SleepMs(10);
    TabletReforgeInput::LeftClickAtCursor();
    TabletReforgeInput::SleepMs(300);

    ctx->Inventory.Scan(-1);
    TabletReforgeInput::SleepMs(200);

    auto refreshed = ctx->Inventory.GetAll();
    for (const auto& inv : refreshed) {
        if (inv.InventoryId == inventoryId && inv.Grid.Valid) {
            OutputDebugStringA(("[IconClassify] ✓ 成功切换到 inv=" +
                std::to_string(inventoryId) + " (" + classified->chineseName + ")\n").c_str());
            return true;
        }
    }

    OutputDebugStringA("[IconClassify] 点击后未确认切换，回退到通用流程\n");
    return ClickStashTabWithVisionVerification(ctx, inventoryId, pluginDir, false, true);
}

// ============================================================
// 视觉识别集成：结合UI树 + 模板匹配的双重验证
// ============================================================
//
// 在实际点击仓库页Tab之前，使用视觉识别验证Tab类型：
//   1. 通过UI树获取Tab按钮位置（精确坐标）
//   2. 使用视觉模板匹配验证Tab类型（防误点）
//   3. 记录耗时和置信度用于调试分析
//   4. 日志通过 OutputDebugStringA 输出到 DebugView
// ============================================================

// 综合识别结果（UI树 + 视觉识别）
struct IntegratedStashTabRecognition {
    // UI树识别结果
    int inventoryId = 0;
    std::string tabLabel;          // UI树上的Tab文本
    StashTabType uiTreeType = StashTabType::Unknown;
    
    // 视觉识别结果
    bool visionMatched = false;
    int visionStashTypeId = -1;
    std::string visionStashTypeName;
    double visionConfidence = 0;
    
    // 综合判断
    bool uiTreeFound = false;      // UI树是否找到
    bool visionFound = false;      // 视觉是否找到
    bool crossVerified = false;    // 双重验证是否通过
    
    // 性能指标
    double uiTreeTimeMs = 0;        // UI树扫描耗时
    double visionTimeMs = 0;       // 视觉识别耗时
    double totalTimeMs = 0;        // 总耗时
    int templateCount = 0;         // 使用的模板数量
    int searchAreaW = 0;           // 搜索区域宽度
    int searchAreaH = 0;           // 搜索区域高度
    
    // 点击坐标
    float clickX = 0;
    float clickY = 0;
    bool clickable = false;
    
    // 调试信息
    std::string debugMessage;
};

// 综合日志输出
inline void LogIntegratedRecognitionResult(const IntegratedStashTabRecognition& r) {
    char buf[2048];
    sprintf_s(buf,
        "[StashOps][IntegratedRecog] ===== 仓库Tab综合识别报告 =====\n"
        "[StashOps][IntegratedRecog]   UI树识别: found=%d, type=%s, label='%s'\n"
        "[StashOps][IntegratedRecog]   视觉识别: found=%d, typeId=%d, name='%s', conf=%.4f\n"
        "[StashOps][IntegratedRecog]   双重验证: verified=%d\n"
        "[StashOps][IntegratedRecog]   性能指标: uiTree=%.2fms, vision=%.2fms, total=%.2fms\n"
        "[StashOps][IntegratedRecog]   视觉详情: templates=%d, searchArea=%dx%d\n"
        "[StashOps][IntegratedRecog]   点击坐标: clickable=%d, pos=(%.1f, %.1f)\n"
        "[StashOps][IntegratedRecog]   调试信息: %s\n"
        "[StashOps][IntegratedRecog] ======================================\n",
        r.uiTreeFound ? 1 : 0,
        StashTabTypeName(r.uiTreeType),
        r.tabLabel.c_str(),
        r.visionFound ? 1 : 0,
        r.visionStashTypeId,
        r.visionStashTypeName.c_str(),
        r.visionConfidence,
        r.crossVerified ? 1 : 0,
        r.uiTreeTimeMs,
        r.visionTimeMs,
        r.totalTimeMs,
        r.templateCount,
        r.searchAreaW, r.searchAreaH,
        r.clickable ? 1 : 0,
        r.clickX, r.clickY,
        r.debugMessage.c_str());
    OutputDebugStringA(buf);
}

// 双重验证：UI树识别结果 vs 视觉识别结果
inline bool VerifyUiTreeVsVision(
    const PluginSDK::Context* ctx,
    const StashTabButtonInfo& uiButton,
    VisionRecogNS::MatchResult& visionResult,
    double confidenceThreshold = 0.5)
{
    // 如果视觉识别失败，无法验证
    if (!visionResult.matched) return false;
    
    // 如果置信度太低，认为不可靠
    if (visionResult.confidence < confidenceThreshold) return false;
    
    // UI树标签转小写做匹配
    std::string labelLower = uiButton.label;
    for (char& c : labelLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    
    // 视觉识别的仓库类型名转小写
    std::string visionNameLower = visionResult.stashName;
    for (char& c : visionNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    
    // 直接名称匹配
    if (!labelLower.empty() && !visionNameLower.empty()) {
        if (labelLower.find(visionNameLower) != std::string::npos ||
            visionNameLower.find(labelLower) != std::string::npos) {
            return true;
        }
    }
    
    // 通过 StashTypeTable 映射验证
    // 将视觉识别的 stashId 映射到 StashTabType
    auto* entry = FindStashTypeByStashId(visionResult.stashId);
    if (entry) {
        std::string idLower = entry->id;
        for (char& c : idLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        
        if (!labelLower.empty()) {
            if (labelLower.find(idLower) != std::string::npos ||
                idLower.find(labelLower) != std::string::npos) {
                return true;
            }
        }
    }
    
    // 特殊映射：常见Tab名对应关系
    struct { const char* pattern; int stashId; } nameMap[] = {
        {"currency", 3}, {"coin", 3}, {"gold", 3}, {"通货", 3}, {"货币", 3},
        {"fragment", 15}, {"tablet", 15}, {"sentinel", 15}, {"碎片", 15}, {"碑牌", 15},
        {"map", 7}, {"waystone", 7}, {"waypoint", 7}, {"地图", 7}, {"引路石", 7},
        {"quad", 4}, {"4-t", 4}, {"四方格", 4},
        {"normal", 1}, {"stash", 1}, {"普通", 1},
    };
    
    int matchedStashId = -1;
    for (const auto& m : nameMap) {
        if (labelLower.find(m.pattern) != std::string::npos) {
            matchedStashId = m.stashId;
            break;
        }
    }
    
    if (matchedStashId > 0 && matchedStashId == visionResult.stashId) {
        return true;
    }
    
    return false;
}

// 执行仓库Tab综合识别流程
// 结合：UI树定位 + 视觉模板匹配验证 + 详细日志
inline IntegratedStashTabRecognition RecognizeStashTabIntegrated(
    const PluginSDK::Context* ctx,
    int targetInventoryId,
    const std::filesystem::path& pluginDir,
    bool enableVisionCheck = true)
{
    IntegratedStashTabRecognition result;
    if (!ctx) {
        result.debugMessage = "Context为空";
        return result;
    }
    
    auto totalStart = std::chrono::high_resolution_clock::now();
    
    // ===== Step 1: UI树识别 =====
    auto uiStart = std::chrono::high_resolution_clock::now();
    
    uintptr_t container = FindStashTabsContainer(ctx);
    if (!container) {
        result.debugMessage = "UI树: 未找到Tab容器";
        auto uiEnd = std::chrono::high_resolution_clock::now();
        result.uiTreeTimeMs = std::chrono::duration<double, std::milli>(uiEnd - uiStart).count();
        auto totalEnd = std::chrono::high_resolution_clock::now();
        result.totalTimeMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        
        LogIntegratedRecognitionResult(result);
        return result;
    }
    
    auto buttons = EnumerateStashTabButtons(ctx, container);
    if (buttons.empty()) {
        result.debugMessage = "UI树: 未找到Tab按钮";
        auto uiEnd = std::chrono::high_resolution_clock::now();
        result.uiTreeTimeMs = std::chrono::duration<double, std::milli>(uiEnd - uiStart).count();
        auto totalEnd = std::chrono::high_resolution_clock::now();
        result.totalTimeMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        
        LogIntegratedRecognitionResult(result);
        return result;
    }
    
    // 匹配inventory到UI按钮
    auto inventories = ctx->Inventory.GetAll();
    MatchInventoriesToTabButtons(ctx, inventories, buttons);
    
    // 找到目标按钮
    StashTabButtonInfo* targetBtn = nullptr;
    for (auto& btn : buttons) {
        if (btn.inventoryId == targetInventoryId && btn.matched) {
            targetBtn = &btn;
            break;
        }
    }
    
    if (!targetBtn) {
        result.debugMessage = "UI树: 未找到目标inventory的Tab按钮 (id=" + 
            std::to_string(targetInventoryId) + ")";
        auto uiEnd = std::chrono::high_resolution_clock::now();
        result.uiTreeTimeMs = std::chrono::duration<double, std::milli>(uiEnd - uiStart).count();
        auto totalEnd = std::chrono::high_resolution_clock::now();
        result.totalTimeMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        
        LogIntegratedRecognitionResult(result);
        return result;
    }
    
    // UI树识别成功
    result.uiTreeFound = true;
    result.inventoryId = targetBtn->inventoryId;
    result.tabLabel = targetBtn->label;
    result.clickX = targetBtn->x;
    result.clickY = targetBtn->y;
    result.clickable = true;
    
    // 获取仓库类型
    auto inv = ctx->Inventory.Get(targetInventoryId);
    if (inv.Address != 0) {
        const char* name = ctx->Inventory.GetName(targetInventoryId);
        result.uiTreeType = IdentifyStashTabType(inv, name);
    }
    
    auto uiEnd = std::chrono::high_resolution_clock::now();
    result.uiTreeTimeMs = std::chrono::duration<double, std::milli>(uiEnd - uiStart).count();
    
    // ===== Step 2: 视觉识别验证 =====
    if (enableVisionCheck) {
        auto visionStart = std::chrono::high_resolution_clock::now();
        
        // 使用带详细日志的视觉识别函数
        VisionRecogNS::VisionTimingReport timingReport;
        auto& cache = GetIconMatchCache();
        
        // 先确保模板已加载
        if (!cache.loaded || cache.lastPluginDir != pluginDir) {
            cache.lastPluginDir = pluginDir;
            cache.loaded = false;
            cache.templates.clear();
            int n = VisionRecogNS::LoadTabIconTemplates(pluginDir, cache.templates);
            cache.loaded = (n > 0);
            result.templateCount = n;
        } else {
            result.templateCount = (int)cache.templates.size();
        }
        
        if (cache.loaded && !cache.templates.empty()) {
            // 在目标按钮周围区域做图标匹配 (扩大搜索窗口以支持多尺度检测)
            int btnCx = (int)targetBtn->x + (int)targetBtn->w / 2;
            int btnCy = (int)targetBtn->y + (int)targetBtn->h / 2;
            int searchSize = 56;  // 搜索窗口 56x56 (可容纳 28~42px 尺寸的图标)
            int iconX = btnCx - searchSize / 2;
            int iconY = btnCy - searchSize / 2;
            if (iconX < 0) { iconX = 0; searchSize = btnCx + searchSize / 2; }
            if (iconY < 0) { iconY = 0; searchSize = btnCy + searchSize / 2; }
            
            result.searchAreaW = searchSize;
            result.searchAreaH = searchSize;
            
            VisionRecogNS::ScreenFrame frame;
            if (VisionRecogNS::CaptureScreenRegion(iconX, iconY, searchSize, searchSize, frame)) {
                // 使用多尺度模板匹配
                VisionRecogNS::MatchResult bestResult = VisionRecogNS::MatchTemplateMultiScale(
                    frame, cache.templates, iconX, iconY, searchSize, searchSize, 0.45);
                
                if (bestResult.matched) {
                    result.visionFound = true;
                    result.visionMatched = true;
                    result.visionStashTypeId = bestResult.stashId;
                    result.visionStashTypeName = bestResult.stashName;
                    result.visionConfidence = bestResult.confidence;
                    
                    // 交叉验证
                    result.crossVerified = VerifyUiTreeVsVision(
                        ctx, *targetBtn, bestResult);
                } else {
                    // 多尺度匹配失败: 回退到原始同尺寸匹配
                    int iconSize = 28;
                    int iconX2 = btnCx - iconSize / 2;
                    int iconY2 = btnCy - iconSize / 2;
                    VisionRecogNS::ScreenFrame frame2;
                    if (VisionRecogNS::CaptureScreenRegion(iconX2, iconY2, iconSize, iconSize, frame2)) {
                        double bestScore = 0.45;
                        VisionRecogNS::MatchResult fallbackResult;
                        for (const auto& tpl : cache.templates) {
                            if (tpl.width != iconSize || tpl.height != iconSize) continue;
                            double mse = VisionRecogNS::ComputeImageMSE(
                                frame2.bgra.data(), tpl.bgra.data(),
                                iconSize, iconSize, frame2.stride, iconSize * 4);
                            double confidence = 1.0 / (1.0 + mse / (255.0 * 255.0));
                            if (confidence > bestScore) {
                                bestScore = confidence;
                                fallbackResult.matched = true;
                                fallbackResult.stashId = tpl.stashId;
                                fallbackResult.stashName = tpl.stashName;
                                fallbackResult.confidence = confidence;
                                fallbackResult.screenX = iconX2 + iconSize / 2;
                                fallbackResult.screenY = iconY2 + iconSize / 2;
                            }
                        }
                        if (fallbackResult.matched) {
                            result.visionFound = true;
                            result.visionMatched = true;
                            result.visionStashTypeId = fallbackResult.stashId;
                            result.visionStashTypeName = fallbackResult.stashName;
                            result.visionConfidence = fallbackResult.confidence;
                            result.crossVerified = VerifyUiTreeVsVision(ctx, *targetBtn, fallbackResult);
                        }
                    }
                }
            }
        }
        
        auto visionEnd = std::chrono::high_resolution_clock::now();
        result.visionTimeMs = std::chrono::duration<double, std::milli>(visionEnd - visionStart).count();
        
        // 记录视觉识别详细报告
        timingReport.loadTemplatesMs = 0;  // 已在上方处理
        timingReport.computeRectMs = 0;
        timingReport.captureMs = result.visionTimeMs * 0.3;  // 估算
        timingReport.matchMs = result.visionTimeMs * 0.7;    // 估算
        timingReport.totalMs = result.visionTimeMs;
        timingReport.templateCount = result.templateCount;
        timingReport.searchAreaW = result.searchAreaW;
        timingReport.searchAreaH = result.searchAreaH;
        timingReport.confidence = result.visionConfidence;
        timingReport.matched = result.visionFound;
        timingReport.matchedStashId = result.visionStashTypeId;
        timingReport.matchedStashName = result.visionStashTypeName;
        timingReport.matchScreenX = (int)result.clickX;
        timingReport.matchScreenY = (int)result.clickY;
        
        VisionRecogNS::LogVisionTimingReport(timingReport);
    } else {
        result.debugMessage += " (视觉识别已禁用)";
    }
    
    auto totalEnd = std::chrono::high_resolution_clock::now();
    result.totalTimeMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
    
    // 最终状态信息
    if (result.uiTreeFound && result.visionFound && result.crossVerified) {
        result.debugMessage = "双重验证通过 (UI+Vision)";
    } else if (result.uiTreeFound && !result.visionFound) {
        result.debugMessage = "仅UI树识别成功，视觉识别未命中";
    } else if (result.uiTreeFound && result.visionFound && !result.crossVerified) {
        result.debugMessage = "UI与视觉识别结果不一致，已采用UI树结果";
    }
    
    // 输出综合报告
    LogIntegratedRecognitionResult(result);
    
    return result;
}

// 基于Inventory Grid位置直接计算Tab点击坐标
// 当UI树和视觉识别都失败时使用的fallback方法
inline bool ClickStashTabByGridPosition(
    const PluginSDK::Context* ctx,
    int inventoryId)
{
    if (!ctx || inventoryId <= 0) return false;
    
    auto inv = ctx->Inventory.Get(inventoryId);
    if (inv.Address == 0) {
        OutputDebugStringA(("[StashOps] Grid点击失败: Inventory.Address=0, invId=" + 
            std::to_string(inventoryId) + "\n").c_str());
        return false;
    }
    
    // 获取主背包的Grid作为参考（计算Tab栏位置）
    auto mainInv = ctx->Inventory.Get(0);
    if (mainInv.Address == 0) {
        OutputDebugStringA("[StashOps] Grid点击失败: 主背包Inventory无效\n");
        return false;
    }
    
    // 计算Tab栏的Y坐标（基于Grid位置推算）
    float gridY = inv.Grid.GridScreenY;
    float tabBarY = gridY - 40.0f;  // Tab栏在网格上方约40px
    if (tabBarY < 100) tabBarY = 100;  // 防止超出屏幕顶部
    
    // 计算仓库网格中心X坐标
    float gridW = inv.TotalBoxesX * inv.Grid.CellSize;
    float centerX = inv.Grid.GridScreenX + gridW / 2.0f;
    
    // 计算Tab宽度（每个Tab约56px宽）
    float tabWidth = 56.0f;
    
    // 遍历所有可见的Inventory，找到目标仓库的索引位置
    auto allInvs = ctx->Inventory.GetAll();
    std::vector<int> visibleInvIds;
    
    // 找出有有效Grid的仓库（可见的仓库页）
    for (const auto& i : allInvs) {
        if (i.Grid.Valid && i.Address != 0) {
            visibleInvIds.push_back(i.InventoryId);
        }
    }
    
    // 如果没有可见的仓库，使用当前Inventory的位置
    int tabIndex = -1;
    for (int i = 0; i < (int)visibleInvIds.size(); i++) {
        if (visibleInvIds[i] == inventoryId) {
            tabIndex = i;
            break;
        }
    }
    
    float clickX, clickY;
    
    if (tabIndex >= 0) {
        // 按照Tab索引位置计算点击坐标
        // Tab栏通常在Grid宽度的范围内居中
        float totalGridW = mainInv.TotalBoxesX * mainInv.Grid.CellSize;
        float tabBarStartX = mainInv.Grid.GridScreenX - tabWidth / 2.0f;
        clickX = tabBarStartX + tabWidth / 2.0f + tabIndex * tabWidth;
        clickY = tabBarY;
        
        OutputDebugStringA(("[StashOps] Grid点击: 通过Tab索引计算, index=" + 
            std::to_string(tabIndex) + ", pos=(" + 
            std::to_string(clickX) + "," + 
            std::to_string(clickY) + ")\n").c_str());
    } else {
        // Fallback: 使用Grid中心上方的位置
        // 假设Tab在网格正上方，选择一个合理的默认位置
        clickX = centerX;
        clickY = tabBarY;
        
        OutputDebugStringA(("[StashOps] Grid点击: 使用Fallback位置, invId=" + 
            std::to_string(inventoryId) + ", pos=(" + 
            std::to_string(clickX) + "," + 
            std::to_string(clickY) + ")\n").c_str());
    }
    
    // 执行点击
    TabletReforgeInput::MoveCursorScreen((int)clickX, (int)clickY);
    TabletReforgeInput::SleepMs(10);
    TabletReforgeInput::LeftClickAtCursor();
    TabletReforgeInput::SleepMs(300);
    
    // 刷新验证
    ctx->Inventory.Scan(-1);
    TabletReforgeInput::SleepMs(200);
    
    // 验证是否切换成功
    auto refreshed = ctx->Inventory.GetAll();
    bool switched = false;
    for (const auto& rinv : refreshed) {
        if (rinv.InventoryId == inventoryId && rinv.Grid.Valid) {
            switched = true;
            break;
        }
    }
    
    if (switched) {
        OutputDebugStringA(("[StashOps] ✓ Grid点击成功切换到仓库页 #" + 
            std::to_string(inventoryId) + "\n").c_str());
    } else {
        OutputDebugStringA(("[StashOps] ⚠ Grid点击未能确认切换 invId=" + 
            std::to_string(inventoryId) + "\n").c_str());
    }
    
    return switched;
}

// 扫描仓库页并获取指定inventoryId的点击坐标
// 返回是否成功找到可点击的坐标
inline bool GetStashTabClickPosByScan(
    const PluginSDK::Context* ctx,
    int inventoryId,
    float& outClickX,
    float& outClickY,
    bool& outClickable)
{
    if (!ctx || inventoryId <= 0) return false;
    
    OutputDebugStringA(("[StashOps] 扫描仓库页查找 invId=" + 
        std::to_string(inventoryId) + " 的点击坐标\n").c_str());
    
    // 方法1: 使用ListAllStashTabsOrdered获取扫描结果
    auto tabs = ListAllStashTabsOrdered(ctx);
    
    for (const auto& tab : tabs) {
        if (tab.inventoryId == inventoryId && tab.clickable) {
            outClickX = tab.clickX;
            outClickY = tab.clickY;
            outClickable = true;
            
            OutputDebugStringA(("[StashOps] 扫描找到仓库页 invId=" + 
                std::to_string(inventoryId) + 
                " name='" + tab.name + "'" +
                " pos=(" + std::to_string(outClickX) + "," + 
                std::to_string(outClickY) + ")" +
                " slotIndex=" + std::to_string(tab.slotIndex) + "\n").c_str());
            return true;
        }
    }
    
    // 方法2: 如果方法1失败，尝试用ListStashTabsEx
    auto tabsEx = ListStashTabsEx(ctx);
    for (const auto& tab : tabsEx) {
        if (tab.inventoryId == inventoryId && tab.clickable) {
            outClickX = tab.clickX;
            outClickY = tab.clickY;
            outClickable = true;
            
            OutputDebugStringA(("[StashOps] 通过ListStashTabsEx找到 invId=" + 
                std::to_string(inventoryId) + 
                " pos=(" + std::to_string(outClickX) + "," + 
                std::to_string(outClickY) + ")\n").c_str());
            return true;
        }
    }
    
    OutputDebugStringA(("[StashOps] 扫描未找到 invId=" + 
        std::to_string(inventoryId) + " 的可点击坐标\n").c_str());
    outClickable = false;
    return false;
}

// 带视觉验证的仓库页点击（含Grid Fallback和自动扫描模式）
// 在点击前执行完整的视觉识别验证流程，失败时使用Grid位置直接点击
// autoClickScanned: 启用时，先扫描仓库页获取坐标，再执行点击
inline bool ClickStashTabWithVisionVerification(
    const PluginSDK::Context* ctx,
    int inventoryId,
    const std::filesystem::path& pluginDir,
    bool requireVisionVerify = false,
    bool autoClickScanned = false)
{
    if (!ctx || inventoryId <= 0) return false;
    
    OutputDebugStringA(("[StashOps] ========== 开始仓库页点击流程 invId=" + 
        std::to_string(inventoryId) + 
        " autoScan=" + std::to_string(autoClickScanned) +
        " ==========\n").c_str());
    
    // ===== 自动扫描模式: 先扫描获取坐标 =====
    if (autoClickScanned) {
        float scanX = 0, scanY = 0;
        bool scanClickable = false;
        
        if (GetStashTabClickPosByScan(ctx, inventoryId, scanX, scanY, scanClickable)) {
            OutputDebugStringA(("[StashOps] [自动扫描] 使用扫描坐标点击: (" + 
                std::to_string(scanX) + "," + 
                std::to_string(scanY) + ")\n").c_str());
            
            // 执行扫描坐标点击
            TabletReforgeInput::MoveCursorScreen(scanX, scanY);
            TabletReforgeInput::SleepMs(10);
            TabletReforgeInput::LeftClickAtCursor();
            TabletReforgeInput::SleepMs(300);
            
            // 刷新验证
            ctx->Inventory.Scan(-1);
            TabletReforgeInput::SleepMs(200);
            
            // 验证是否切换成功
            auto refreshed = ctx->Inventory.GetAll();
            bool switched = false;
            for (const auto& inv : refreshed) {
                if (inv.InventoryId == inventoryId && inv.Grid.Valid) {
                    switched = true;
                    break;
                }
            }
            
            if (switched) {
                OutputDebugStringA(("[StashOps] ✓ [自动扫描] 成功切换到仓库页 #" + 
                    std::to_string(inventoryId) + "\n").c_str());
                return true;
            } else {
                OutputDebugStringA("[StashOps] ⚠ [自动扫描] 点击后未能确认切换，继续尝试视觉识别\n");
                // 继续执行后面的视觉识别流程
            }
        } else {
            OutputDebugStringA("[StashOps] [自动扫描] 未能获取扫描坐标，继续视觉识别流程\n");
        }
    }
    
    // ===== Step 1: 视觉识别验证 =====
    auto recognition = RecognizeStashTabIntegrated(ctx, inventoryId, pluginDir, true);
    
    if (recognition.uiTreeFound && recognition.clickable) {
        // 如果要求视觉验证但未通过
        if (requireVisionVerify && !recognition.crossVerified) {
            OutputDebugStringA("[StashOps] 点击中止: 视觉验证未通过，尝试Grid Fallback\n");
            return ClickStashTabByGridPosition(ctx, inventoryId);
        }
        
        // ===== Step 2: 使用UI树识别的坐标执行点击 =====
        OutputDebugStringA(("[StashOps] 执行点击(UI树): pos=(" + 
            std::to_string(recognition.clickX) + "," + 
            std::to_string(recognition.clickY) + ")\n").c_str());
        
        TabletReforgeInput::MoveCursorScreen(recognition.clickX, recognition.clickY);
        TabletReforgeInput::SleepMs(10);
        TabletReforgeInput::LeftClickAtCursor();
        TabletReforgeInput::SleepMs(300);  // 等待UI切换
        
        // ===== Step 3: 刷新验证 =====
        ctx->Inventory.Scan(-1);
        TabletReforgeInput::SleepMs(200);
        
        // 验证是否切换成功
        auto refreshed = ctx->Inventory.GetAll();
        bool switched = false;
        for (const auto& inv : refreshed) {
            if (inv.InventoryId == inventoryId && inv.Grid.Valid) {
                switched = true;
                break;
            }
        }
        
        if (switched) {
            OutputDebugStringA(("[StashOps] ✓ 成功切换到仓库页 #" + 
                std::to_string(inventoryId) + " 总耗时=" + 
                std::to_string(recognition.totalTimeMs) + "ms\n").c_str());
        } else {
            OutputDebugStringA("[StashOps] ⚠ 点击后未能确认切换 (尝试Grid Fallback)\n");
            // Fallback到Grid位置点击
            return ClickStashTabByGridPosition(ctx, inventoryId);
        }
        
        return true;
    } else {
        // UI树识别失败，使用Grid Fallback
        OutputDebugStringA("[StashOps] UI树识别失败，使用Grid Fallback点击\n");
        return ClickStashTabByGridPosition(ctx, inventoryId);
    }
}

// 批量识别所有仓库Tab的完整信息（含视觉验证）
// 用于调试和配置界面展示
inline std::vector<IntegratedStashTabRecognition> RecognizeAllStashTabsIntegrated(
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir)
{
    std::vector<IntegratedStashTabRecognition> results;
    if (!ctx) return results;
    
    auto allTabs = GetAllStashTabsWithButtons(ctx);
    
    OutputDebugStringA(("[StashOps] ========== 批量识别 " + 
        std::to_string(allTabs.size()) + " 个仓库Tab ==========\n").c_str());
    
    for (const auto& tab : allTabs) {
        auto result = RecognizeStashTabIntegrated(ctx, tab.inventoryId, pluginDir, true);
        results.push_back(result);
    }
    
    // 汇总日志
    int uiFound = 0, visionFound = 0, crossVerified = 0;
    double totalVisionTime = 0;
    for (const auto& r : results) {
        if (r.uiTreeFound) uiFound++;
        if (r.visionFound) visionFound++;
        if (r.crossVerified) crossVerified++;
        totalVisionTime += r.visionTimeMs;
    }
    
    char summary[512];
    sprintf_s(summary,
        "[StashOps] ========== 批量识别摘要 ==========\n"
        "[StashOps]   总Tab数: %zu\n"
        "[StashOps]   UI树识别: %d/%d\n"
        "[StashOps]   视觉识别: %d/%d\n"
        "[StashOps]   双重验证: %d/%d\n"
        "[StashOps]   视觉总耗时: %.2f ms\n"
        "[StashOps]   平均视觉耗时: %.2f ms\n"
        "[StashOps] =========================\n",
        results.size(),
        uiFound, (int)results.size(),
        visionFound, (int)results.size(),
        crossVerified, (int)results.size(),
        totalVisionTime,
        results.empty() ? 0 : totalVisionTime / results.size());
    OutputDebugStringA(summary);
    
    return results;
}

// 视觉识别性能测试（用于验证识别速度是否满足实时要求）
struct VisionPerformanceReport {
    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;
    double avgTimeMs = 0;
    double minTimeMs = 1e9;
    double maxTimeMs = 0;
    double p95TimeMs = 0;
    std::vector<double> allTimesMs;
};

inline VisionPerformanceReport RunVisionPerformanceTest(
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir,
    int iterations = 10)
{
    VisionPerformanceReport report;
    if (!ctx || iterations <= 0) return report;
    
    auto& cache = GetIconMatchCache();
    
    // 先加载模板
    if (!cache.loaded || cache.lastPluginDir != pluginDir) {
        cache.lastPluginDir = pluginDir;
        cache.loaded = false;
        cache.templates.clear();
        int n = VisionRecogNS::LoadTabIconTemplates(pluginDir, cache.templates);
        cache.loaded = (n > 0);
        if (n == 0) {
            OutputDebugStringA("[StashOps][PerfTest] 无模板可测试\n");
            return report;
        }
    }
    
    OutputDebugStringA(("[StashOps][PerfTest] 开始性能测试: " + 
        std::to_string(iterations) + " 次迭代，" + 
        std::to_string(cache.templates.size()) + " 个模板\n").c_str());
    
    // 找到第一个可见Tab区域
    auto allTabs = GetAllStashTabsWithButtons(ctx);
    if (allTabs.empty()) {
        OutputDebugStringA("[StashOps][PerfTest] 无可见仓库页\n");
        return report;
    }
    
    int testInvId = 0;
    for (const auto& tab : allTabs) {
        if (tab.isVisible) {
            testInvId = tab.inventoryId;
            break;
        }
    }
    if (testInvId == 0) testInvId = allTabs[0].inventoryId;
    
    report.totalTests = iterations;
    report.allTimesMs.reserve(iterations);
    
    for (int i = 0; i < iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        
        auto result = RecognizeStashTabIntegrated(ctx, testInvId, pluginDir, true);
        
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        
        report.allTimesMs.push_back(elapsed);
        
        if (result.visionFound) {
            report.passedTests++;
        } else {
            report.failedTests++;
        }
        
        if (elapsed < report.minTimeMs) report.minTimeMs = elapsed;
        if (elapsed > report.maxTimeMs) report.maxTimeMs = elapsed;
    }
    
    // 计算统计
    double total = 0;
    for (double t : report.allTimesMs) total += t;
    report.avgTimeMs = total / iterations;
    
    // 计算P95
    auto sortedTimes = report.allTimesMs;
    std::sort(sortedTimes.begin(), sortedTimes.end());
    int p95Idx = (int)(iterations * 0.95);
    if (p95Idx >= iterations) p95Idx = iterations - 1;
    report.p95TimeMs = sortedTimes[p95Idx];
    
    // 输出报告
    char buf[512];
    sprintf_s(buf,
        "[StashOps][PerfTest] ===== 性能测试报告 =====\n"
        "[StashOps][PerfTest]   测试次数: %d\n"
        "[StashOps][PerfTest]   通过: %d, 失败: %d\n"
        "[StashOps][PerfTest]   平均耗时: %.2f ms\n"
        "[StashOps][PerfTest]   最小耗时: %.2f ms\n"
        "[StashOps][PerfTest]   最大耗时: %.2f ms\n"
        "[StashOps][PerfTest]   P95耗时:  %.2f ms\n"
        "[StashOps][PerfTest] =========================\n",
        report.totalTests,
        report.passedTests,
        report.failedTests,
        report.avgTimeMs,
        report.minTimeMs == 1e9 ? 0 : report.minTimeMs,
        report.maxTimeMs,
        report.p95TimeMs);
    OutputDebugStringA(buf);
    
    return report;
}

// ============================================================
// 改进版：基于UI树的完整仓库页枚举
// 解决17个仓库页只能扫描到11个的问题
// ============================================================

// 仓库页完整信息（包含多级嵌套结构）
struct StashTabFullInfo {
    int inventoryId = 0;
    std::string name;           // UI树显示的名称（如"碎片"、"碑牌"等）
    std::string internalName;  // 内部名称（如"Offhand1"）
    StashTabType type = StashTabType::Unknown;
    int slots = 0;
    int itemCount = 0;
    bool onScreen = false;
    float clickX = 0;
    float clickY = 0;
    bool clickable = false;
    bool isVisible = false;
    int parentIndex = -1;       // 父页索引（-1表示顶层）
    int depth = 0;              // 嵌套深度（0=顶层，1=子页，2=孙子页）
    std::vector<StashTabFullInfo> children;  // 嵌套子页
};

// 基于UI树获取所有仓库页（包括未显示的）
inline std::vector<StashTabFullInfo> ListAllStashTabsFromUi(const PluginSDK::Context* ctx) {
    std::vector<StashTabFullInfo> out;
    if (!ctx) return out;
    
    auto mainInv = FindMainInventory(ctx);
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);
    
    // Step 1: 从UI树获取所有Tab按钮
    auto tabButtons = ExtractStashTabButtons(ctx);
    
    // Step 2: 获取所有Inventory
    std::vector<PluginSDK::Inventory> inventories;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;
        // 综合过滤：装备槽位 + 非仓库Tab的 Inventory_NNN（基于 ggpk 格子尺寸数据）
        const char* invNameC = ctx->Inventory.GetName(inv.InventoryId);
        std::string invName = invNameC ? invNameC : "";
        if (IsNonStashInventory(invName, inv.TotalBoxesX, inv.TotalBoxesY, inv.InventoryId)) {
            char log[256];
            sprintf_s(log, "[StashOps] ListAllStashTabsFromUi 过滤非仓库Tab: invId=%d name='%s' size=%dx%d\n",
                inv.InventoryId, invName.c_str(), inv.TotalBoxesX, inv.TotalBoxesY);
            OutputDebugStringA(log);
            continue;
        }
        inventories.push_back(inv);
    }
    
    // Step 3: 构建UI树按钮到Inventory的映射
    // 优先用位置匹配（因为UI树按钮的位置对应Inventory的网格位置）
    std::unordered_map<int, int> buttonToInv;  // buttonIndex -> inventoryIndex
    std::unordered_map<int, int> invToButton;  // inventoryId -> buttonIndex
    
    // 按位置匹配：查找哪个Inventory的Grid位置与按钮位置匹配
    for (int bi = 0; bi < (int)tabButtons.size(); ++bi) {
        const auto& btn = tabButtons[bi];
        
        for (int ii = 0; ii < (int)inventories.size(); ++ii) {
            const auto& inv = inventories[ii];
            if (!inv.Grid.Valid) continue;
            
            // 检查按钮位置是否在网格附近
            float gridCenterX = inv.Grid.GridScreenX + inv.Grid.CellSize * 0.5f;
            float gridTopY = inv.Grid.GridScreenY - 40.f;  // Tab按钮在网格上方
            
            float distX = std::fabs(btn.x - gridCenterX);
            float distY = std::fabs(btn.y - gridTopY);
            
            // 如果按钮位置接近网格上方，认为是对应的Tab
            if (distX < inv.Grid.CellSize * 2.0f && distY < 60.f) {
                buttonToInv[bi] = ii;
                invToButton[inv.InventoryId] = bi;
                break;
            }
        }
    }
    
    // Step 4: 为每个Inventory创建StashTabFullInfo
    std::vector<StashTabFullInfo> allTabs;
    
    for (const auto& inv : inventories) {
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 1) continue;  // 只过滤完全无效的（保留小页）
        
        StashTabFullInfo info;
        info.inventoryId = inv.InventoryId;
        info.internalName = ctx->Inventory.GetName(inv.InventoryId) ? ctx->Inventory.GetName(inv.InventoryId) : "";
        info.slots = slots;
        info.itemCount = (int)inv.Items.size();
        info.onScreen = inv.Grid.Valid && GridOnScreen(inv, displayW, displayH);
        info.isVisible = inv.Grid.Valid;
        
        // 获取UI树显示的名称
        auto btnIt = invToButton.find(inv.InventoryId);
        if (btnIt != invToButton.end() && btnIt->second < (int)tabButtons.size()) {
            info.name = tabButtons[btnIt->second].label;
            info.clickX = tabButtons[btnIt->second].x;
            info.clickY = tabButtons[btnIt->second].y;
            info.clickable = true;
        } else {
            info.name = info.internalName;
            // 尝试计算点击位置
            info.clickable = CalculateStashTabClickPos(inv, StashTabType::Unknown, info.clickX, info.clickY);
        }
        
        // 识别类型（优先用UI树信息）
        if (btnIt != invToButton.end() && btnIt->second < (int)tabButtons.size()) {
            info.type = IdentifyStashTabTypeFromUi(ctx, inv, tabButtons, btnIt->second);
        } else {
            info.type = IdentifyStashTabType(inv, info.internalName.c_str());
        }
        
        allTabs.push_back(std::move(info));
    }
    
    // Step 5: 构建嵌套结构（基于位置和类型）
    // 规则：Fragment类型的页下面可以包含SubTab类型的子页
    //       SubTab类型的页如果在Fragment页的位置下方，则认为是其子页
    
    // 首先找出所有顶层Tab（非SubTab类型）
    std::vector<StashTabFullInfo> topLevelTabs;
    std::vector<StashTabFullInfo> subTabCandidates;
    
    for (auto& tab : allTabs) {
        if (tab.type == StashTabType::SubTab || tab.slots <= 6) {
            subTabCandidates.push_back(std::move(tab));
        } else {
            topLevelTabs.push_back(std::move(tab));
        }
    }
    
    // 将子页分配给对应的父页
    for (auto& sub : subTabCandidates) {
        bool assigned = false;
        
        // 查找最近的Fragment类型父页
        for (auto& parent : topLevelTabs) {
            if (parent.type == StashTabType::Fragment) {
                // 检查位置关系：子页应该在父页下方或附近
                float yDiff = sub.clickY - parent.clickY;
                if (yDiff > -50.f && yDiff < 200.f) {
                    parent.children.push_back(std::move(sub));
                    assigned = true;
                    break;
                }
            }
        }
        
        // 如果没有找到父页，作为顶层Tab
        if (!assigned) {
            topLevelTabs.push_back(std::move(sub));
        }
    }
    
    // 输出调试日志
    {
        std::string logMsg = "[StashOps] UI树仓库页枚举: 共" + std::to_string(allTabs.size()) + "个Inventory\n";
        int topIdx = 0;
        for (const auto& tab : topLevelTabs) {
            char line[256];
            sprintf_s(line, "  #%d: invId=%d name='%s' internal='%s' type=%s slots=%d items=%d click=(%.0f,%.0f) children=%zu\n",
                topIdx, tab.inventoryId, tab.name.c_str(), tab.internalName.c_str(),
                StashTabTypeName(tab.type), tab.slots, tab.itemCount,
                tab.clickX, tab.clickY, tab.children.size());
            logMsg += line;
            
            for (const auto& child : tab.children) {
                char childLine[256];
                sprintf_s(childLine, "    -> invId=%d name='%s' type=%s slots=%d items=%d\n",
                    child.inventoryId, child.name.c_str(),
                    StashTabTypeName(child.type), child.slots, child.itemCount);
                logMsg += childLine;
            }
            topIdx++;
        }
        OutputDebugStringA(logMsg.c_str());
    }
    
    return topLevelTabs;
}

// 点击指定路径的多级子页（递归导航）
// path: 从顶层开始的路径，如 {0, 0, 2} 表示：第0个顶层Tab -> 它的第0个子页 -> 该子页的第2个子页
inline bool ClickNestedStashTab(const PluginSDK::Context* ctx, const std::vector<int>& path) {
    if (!ctx || path.empty()) return false;
    
    auto allTabs = ListAllStashTabsFromUi(ctx);
    if (allTabs.empty()) return false;
    
    StashTabFullInfo* current = nullptr;
    int depth = 0;
    
    // 导航到目标Tab
    for (int idx : path) {
        if (depth == 0) {
            if (idx < 0 || idx >= (int)allTabs.size()) return false;
            current = &allTabs[idx];
        } else if (current) {
            if (idx < 0 || idx >= (int)current->children.size()) return false;
            current = &current->children[idx];
        }
        depth++;
    }
    
    if (!current || !current->clickable) return false;
    
    // 点击目标Tab
    TabletReforgeInput::MoveCursorScreen(current->clickX, current->clickY);
    TabletReforgeInput::SleepMs(5);
    TabletReforgeInput::LeftClickAtCursor();
    TabletReforgeInput::SleepMs(300);  // 等待UI切换
    
    // 触发刷新
    ctx->Inventory.Scan(-1);
    TabletReforgeInput::SleepMs(200);
    
    // 输出日志
    char logMsg[512];
    sprintf_s(logMsg, sizeof(logMsg), "[StashOps] 点击嵌套仓库页: path=");
    for (size_t i = 0; i < path.size(); ++i) {
        sprintf_s(logMsg + strlen(logMsg), sizeof(logMsg) - strlen(logMsg), "%s%d", i > 0 ? "->" : "", path[i]);
    }
    sprintf_s(logMsg + strlen(logMsg), sizeof(logMsg) - strlen(logMsg), " invId=%d name='%s'\n", current->inventoryId, current->name.c_str());
    OutputDebugStringA(logMsg);
    
    return true;
}

// 扫描指定类型物品的所有可用仓库页（包括嵌套子页）
// 自动实现：1号小仓空了自动切到2号仓的逻辑
struct StashScanResult {
    std::vector<PluginSDK::Inventory> scannedInventories;  // 扫描过的仓库列表
    int totalMaterialCount = 0;                            // 总原料数
    std::vector<std::pair<int, int>> materialCountsByInv;  // 每个仓库的原料数 (inventoryId, count)
};

// 前向声明
inline int CountMaterialItems(const PluginSDK::Inventory& inv, 
                               const TabletReforgeConfig::Settings& settings);

// 扫描Fragment类型仓库的所有子页，统计物品总数
inline StashScanResult ScanAllFragmentSubTabs(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings) {
    
    StashScanResult result;
    if (!ctx) return result;
    
    auto allTabs = ListAllStashTabsFromUi(ctx);
    
    // 找到所有Fragment类型的仓库页
    std::vector<StashTabFullInfo*> fragmentTabs;
    for (auto& tab : allTabs) {
        if (tab.type == StashTabType::Fragment) {
            fragmentTabs.push_back(&tab);
        }
    }
    
    if (fragmentTabs.empty()) {
        OutputDebugStringA("[StashOps] 没有找到Fragment类型的仓库页\n");
        return result;
    }
    
    // 对每个Fragment仓库，扫描它和它的所有子页
    for (auto* fragmentTab : fragmentTabs) {
        // 先点击Fragment主 Tab
        if (fragmentTab->clickable) {
            TabletReforgeInput::MoveCursorScreen(fragmentTab->clickX, fragmentTab->clickY);
            TabletReforgeInput::SleepMs(5);
            TabletReforgeInput::LeftClickAtCursor();
            TabletReforgeInput::SleepMs(300);
            ctx->Inventory.Scan(-1);
            TabletReforgeInput::SleepMs(200);
        }
        
        // 扫描主Tab
        for (const auto& inv : ctx->Inventory.GetAll()) {
            if (inv.InventoryId == fragmentTab->inventoryId) {
                int count = CountMaterialItems(inv, settings);
                result.scannedInventories.push_back(inv);
                result.materialCountsByInv.push_back({inv.InventoryId, count});
                result.totalMaterialCount += count;
                
                char logMsg[256];
                sprintf_s(logMsg, "[StashOps] Fragment主Tab invId=%d 原料数=%d\n", inv.InventoryId, count);
                OutputDebugStringA(logMsg);
                break;
            }
        }
        
        // 递归扫描所有子页
        std::function<void(const std::vector<StashTabFullInfo>&)> scanChildren;
        scanChildren = [&](const std::vector<StashTabFullInfo>& children) {
            for (const auto& child : children) {
                // 点击子页
                if (child.clickable) {
                    TabletReforgeInput::MoveCursorScreen(child.clickX, child.clickY);
                    TabletReforgeInput::SleepMs(5);
                    TabletReforgeInput::LeftClickAtCursor();
                    TabletReforgeInput::SleepMs(300);
                    ctx->Inventory.Scan(-1);
                    TabletReforgeInput::SleepMs(200);
                }
                
                // 扫描子页
                for (const auto& inv : ctx->Inventory.GetAll()) {
                    if (inv.InventoryId == child.inventoryId) {
                        int count = CountMaterialItems(inv, settings);
                        result.scannedInventories.push_back(inv);
                        result.materialCountsByInv.push_back({inv.InventoryId, count});
                        result.totalMaterialCount += count;
                        
                        char logMsg[256];
                        sprintf_s(logMsg, "[StashOps] 子页 invId=%d name='%s' 原料数=%d\n",
                            inv.InventoryId, child.name.c_str(), count);
                        OutputDebugStringA(logMsg);
                        break;
                    }
                }
                
                // 递归扫描孙子页
                if (!child.children.empty()) {
                    scanChildren(child.children);
                }
            }
        };
        
        if (!fragmentTab->children.empty()) {
            scanChildren(fragmentTab->children);
        }
    }
    
    // 输出总计
    char summary[256];
    sprintf_s(summary, "[StashOps] === Fragment仓库扫描总计 === 扫描仓库数=%zu 总原料数=%d\n",
        result.scannedInventories.size(), result.totalMaterialCount);
    OutputDebugStringA(summary);
    
    return result;
}

// 计算单个Inventory中的原料物品数
inline int CountMaterialItems(const PluginSDK::Inventory& inv, 
                               const TabletReforgeConfig::Settings& settings) {
    int count = 0;
    for (const auto& item : inv.Items) {
        if (MatchesPoe2DataPatterns(item.Path, item.BaseTypeName, 
                                     item.Rarity, item.IsIdentified, settings)) {
            count++;
        }
    }
    return count;
}

// 获取指定父Tab下的下一个有原料的子页索引
// 用于实现"1号小仓空了自动切2号仓"的逻辑
inline int FindNextSubTabWithMaterials(
    const PluginSDK::Context* ctx,
    int parentInventoryId,
    int currentSubTabIndex,
    const TabletReforgeConfig::Settings& settings) {
    
    auto allTabs = ListAllStashTabsFromUi(ctx);
    
    // 找到父Tab
    for (const auto& tab : allTabs) {
        if (tab.inventoryId == parentInventoryId) {
            // 从当前子页的下一个开始查找
            for (int i = currentSubTabIndex + 1; i < (int)tab.children.size(); ++i) {
                const auto& child = tab.children[i];
                
                // 点击子页
                if (child.clickable) {
                    TabletReforgeInput::MoveCursorScreen(child.clickX, child.clickY);
                    TabletReforgeInput::SleepMs(5);
                    TabletReforgeInput::LeftClickAtCursor();
                    TabletReforgeInput::SleepMs(300);
                    ctx->Inventory.Scan(-1);
                    TabletReforgeInput::SleepMs(200);
                }
                
                // 检查该子页是否有原料
                for (const auto& inv : ctx->Inventory.GetAll()) {
                    if (inv.InventoryId == child.inventoryId) {
                        int count = CountMaterialItems(inv, settings);
                        if (count > 0) {
                            char logMsg[256];
                            sprintf_s(logMsg, "[StashOps] 找到有原料的子页: index=%d invId=%d count=%d\n",
                                i, child.inventoryId, count);
                            OutputDebugStringA(logMsg);
                            return i;
                        }
                        break;
                    }
                }
            }
            break;
        }
    }
    
    return -1;  // 没有找到有原料的子页
}

// 导航到指定物品类型的正确仓库路径
// 自动实现：碎片仓库→碑牌→神庙→1-6号小仓 的多级导航
// 返回：导航路径（如 {fragmentIndex, tabletSubIndex, templeSubIndex, storageIndex}）
inline std::vector<int> NavigateToMaterialStash(
    const PluginSDK::Context* ctx,
    const TabletReforgeConfig::Settings& settings) {
    
    std::vector<int> path;
    if (!ctx) return path;
    
    OutputDebugStringA("[StashOps] ===== 开始多级仓库导航 =====\n");
    
    auto allTabs = ListAllStashTabsFromUi(ctx);
    if (allTabs.empty()) {
        OutputDebugStringA("[StashOps] 没有找到任何仓库页\n");
        return path;
    }
    
    // Step 1: 找到Fragment类型的主Tab
    int fragmentIndex = -1;
    for (int i = 0; i < (int)allTabs.size(); ++i) {
        if (allTabs[i].type == StashTabType::Fragment) {
            fragmentIndex = i;
            break;
        }
    }
    
    if (fragmentIndex < 0) {
        OutputDebugStringA("[StashOps] 没有找到Fragment类型的仓库页，尝试使用第一个可用仓库\n");
        // 回退：使用第一个有物品的仓库
        for (int i = 0; i < (int)allTabs.size(); ++i) {
            if (allTabs[i].itemCount > 0) {
                fragmentIndex = i;
                break;
            }
        }
        if (fragmentIndex < 0) fragmentIndex = 0;
    }
    
    path.push_back(fragmentIndex);
    
    // Step 2: 点击Fragment主Tab
    const auto& fragmentTab = allTabs[fragmentIndex];
    if (fragmentTab.clickable) {
        TabletReforgeInput::MoveCursorScreen(fragmentTab.clickX, fragmentTab.clickY);
        TabletReforgeInput::SleepMs(5);
        TabletReforgeInput::LeftClickAtCursor();
        TabletReforgeInput::SleepMs(300);
        ctx->Inventory.Scan(-1);
        TabletReforgeInput::SleepMs(200);
        
        char logMsg[256];
        sprintf_s(logMsg, "[StashOps] Step 1: 点击Fragment主Tab #%d invId=%d\n",
            fragmentIndex, fragmentTab.inventoryId);
        OutputDebugStringA(logMsg);
    }
    
    // Step 3: 在Fragment的子页中查找碑牌(Temple Tablet)相关的子页
    // 子页结构：碑牌子页 -> 神庙碑牌次级子页 -> 1-6号小仓
    if (!fragmentTab.children.empty()) {
        OutputDebugStringA(("[StashOps] Fragment下有" + 
            std::to_string(fragmentTab.children.size()) + "个子页\n").c_str());
        
        // 遍历子页，查找有原料的页
        for (int subIdx = 0; subIdx < (int)fragmentTab.children.size(); ++subIdx) {
            const auto& subTab = fragmentTab.children[subIdx];
            
            // 点击子页
            if (subTab.clickable) {
                TabletReforgeInput::MoveCursorScreen(subTab.clickX, subTab.clickY);
                TabletReforgeInput::SleepMs(5);
                TabletReforgeInput::LeftClickAtCursor();
                TabletReforgeInput::SleepMs(300);
                ctx->Inventory.Scan(-1);
                TabletReforgeInput::SleepMs(200);
            }
            
            // 检查该子页是否有原料
            bool hasMaterials = false;
            for (const auto& inv : ctx->Inventory.GetAll()) {
                if (inv.InventoryId == subTab.inventoryId) {
                    int count = CountMaterialItems(inv, settings);
                    if (count > 0) {
                        hasMaterials = true;
                        path.push_back(subIdx);
                        
                        char logMsg[256];
                        sprintf_s(logMsg, "[StashOps] Step 2: 子页 #%d invId=%d 有原料数=%d\n",
                            subIdx, subTab.inventoryId, count);
                        OutputDebugStringA(logMsg);
                        break;
                    }
                    break;
                }
            }
            
            if (hasMaterials) {
                // 继续深入查找孙子页（如1-6号小仓）
                if (!subTab.children.empty()) {
                    for (int storageIdx = 0; storageIdx < (int)subTab.children.size(); ++storageIdx) {
                        const auto& storageTab = subTab.children[storageIdx];
                        
                        // 点击小仓页
                        if (storageTab.clickable) {
                            TabletReforgeInput::MoveCursorScreen(storageTab.clickX, storageTab.clickY);
                            TabletReforgeInput::SleepMs(5);
                            TabletReforgeInput::LeftClickAtCursor();
                            TabletReforgeInput::SleepMs(300);
                            ctx->Inventory.Scan(-1);
                            TabletReforgeInput::SleepMs(200);
                        }
                        
                        // 检查小仓是否有原料
                        for (const auto& inv : ctx->Inventory.GetAll()) {
                            if (inv.InventoryId == storageTab.inventoryId) {
                                int count = CountMaterialItems(inv, settings);
                                if (count > 0) {
                                    path.push_back(storageIdx);
                                    
                                    char logMsg[256];
                                    sprintf_s(logMsg, "[StashOps] Step 3: 小仓 #%d invId=%d 有原料数=%d → 导航完成\n",
                                        storageIdx, storageTab.inventoryId, count);
                                    OutputDebugStringA(logMsg);
                                    
                                    OutputDebugStringA("[StashOps] ===== 导航路径: ");
                                    for (size_t i = 0; i < path.size(); ++i) {
                                        OutputDebugStringA(std::to_string(path[i]).c_str());
                                        if (i + 1 < path.size()) OutputDebugStringA(" -> ");
                                    }
                                    OutputDebugStringA(" =====\n");
                                    
                                    return path;
                                }
                                break;
                            }
                        }
                        
                        // 如果当前小仓没有原料，检查下一个
                        OutputDebugStringA(("[StashOps] 小仓 #" + 
                            std::to_string(storageIdx) + " 无原料，尝试下一个\n").c_str());
                    }
                }
                
                return path;  // 找到有原料的子页（即使没有更深的小仓结构）
            }
        }
    }
    
    // 如果没有子页结构，直接返回当前路径
    if (path.size() == 1) {
        OutputDebugStringA("[StashOps] 无嵌套子页结构，直接使用Fragment主Tab\n");
    }
    
    OutputDebugStringA("[StashOps] ===== 导航结束 =====\n");
    return path;
}

// 获取指定父Tab下所有子页的原料统计
// 用于UI显示和调试
inline std::vector<std::pair<int, int>> GetSubTabsMaterialStats(
    const PluginSDK::Context* ctx,
    int parentInventoryId,
    const TabletReforgeConfig::Settings& settings) {
    
    std::vector<std::pair<int, int>> result;  // (subTabIndex, materialCount)
    if (!ctx) return result;
    
    auto allTabs = ListAllStashTabsFromUi(ctx);
    
    for (const auto& tab : allTabs) {
        if (tab.inventoryId == parentInventoryId) {
            for (int i = 0; i < (int)tab.children.size(); ++i) {
                const auto& child = tab.children[i];
                
                // 临时点击以获取正确的Inventory数据
                if (child.clickable) {
                    TabletReforgeInput::MoveCursorScreen(child.clickX, child.clickY);
                    TabletReforgeInput::SleepMs(5);
                    TabletReforgeInput::LeftClickAtCursor();
                    TabletReforgeInput::SleepMs(300);
                    ctx->Inventory.Scan(-1);
                    TabletReforgeInput::SleepMs(100);
                }
                
                int count = 0;
                for (const auto& inv : ctx->Inventory.GetAll()) {
                    if (inv.InventoryId == child.inventoryId) {
                        count = CountMaterialItems(inv, settings);
                        break;
                    }
                }
                
                result.push_back({i, count});
            }
            break;
        }
    }
    
    return result;
}

} // namespace TabletReforgeGame
