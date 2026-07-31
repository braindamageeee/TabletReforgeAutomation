// PanelDetector.h — 背包/仓库面板检测 + 物品屏幕坐标解析
//
// 复用 tablet-helper 的 ResolveItemRect 混合策略：
//   1. 优先用 InventoryItem.ScreenX/Y（特殊仓库页有正确的 per-item 坐标）
//   2. 回退到 Grid.GridScreenX + slot * CellSize（普通网格页）
//
// 复用 QuickStash 的 FindMainInventory：按名称列表查找主背包。
//
// 仓库检测：Grid.Valid 且 TotalBoxes >= 40 的 inventory 视为仓库面板（pickmyloot Gates.h 逻辑）。
#pragma once

#include "../sdk/PluginSDK.h"

#include <cstring>
#include <optional>
#include <string>

namespace TabletReforgeGame {

// 屏幕矩形（像素坐标）
struct ScreenRect {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

// 矩形中心点
inline void RectCenter(const ScreenRect& r, float& cx, float& cy) {
    cx = r.x + r.w * 0.5f;
    cy = r.y + r.h * 0.5f;
}

// 网格是否在屏幕上（仓库页关闭时 Grid.Valid=false 或坐标在屏外）
inline bool GridOnScreen(const PluginSDK::Inventory& inv, float displayW, float displayH) {
    if (!inv.Grid.Valid || inv.Grid.CellSize <= 0.f) return false;
    if (displayW <= 0.f || displayH <= 0.f) return false;
    const float x = inv.Grid.GridScreenX;
    const float y = inv.Grid.GridScreenY;
    const float margin = 4.f;
    return x >= -margin && y >= -margin && x < displayW && y < displayH;
}

// 单个物品是否真的渲染在屏幕上（PoeFixer 只给可见页的物品赋 ScreenValid=true）
// 增强版：即使 ScreenValid 为 false，也检查 ScreenX/Y 是否在屏幕范围内
inline bool ItemOnScreen(const PluginSDK::InventoryItem& item, float displayW, float displayH) {
    if (item.ScreenW <= 0.f || item.ScreenH <= 0.f) return false;
    if (displayW <= 0.f || displayH <= 0.f) return false;
    const float cx = item.ScreenX + item.ScreenW * 0.5f;
    const float cy = item.ScreenY + item.ScreenH * 0.5f;
    if (cx < 0.f || cy < 0.f || cx >= displayW || cy >= displayH) return false;
    // 如果 ScreenValid 为 true，直接返回
    if (item.ScreenValid) return true;
    // ScreenValid 为 false 但坐标在屏幕内 → 仍视为有效（MainInventory 在某些状态下会这样）
    return true;
}

// 拒绝荒谬的网格布局（特殊页的逻辑网格可能 53x4 溢出屏幕）
// 注意：POE2 MainInventory 只有 5 行（12x5=60格），不能要求 >= 6 行
inline bool GridLayoutPlausible(const PluginSDK::Inventory& inv, float displayW) {
    if (inv.TotalBoxesY < 2) return false;
    if (inv.Grid.CellSize > 0.f
        && static_cast<float>(inv.TotalBoxesX) * inv.Grid.CellSize > displayW * 1.5f)
        return false;
    return true;
}

// 混合策略解析物品屏幕矩形：优先 per-item，回退 grid 数学
// 增强版：即使 GridLayoutPlausible 不通过，也尝试用 grid 数学计算（兜底）
inline std::optional<ScreenRect> ResolveItemRect(const PluginSDK::Inventory& inv,
                                                  const PluginSDK::InventoryItem& item,
                                                  float displayW, float displayH) {
    // 1) 优先用 per-item 屏幕坐标（最精确）
    if (ItemOnScreen(item, displayW, displayH)) {
        return ScreenRect{item.ScreenX, item.ScreenY, item.ScreenW, item.ScreenH};
    }

    // 2) Grid 有效 + 在屏幕上 + CellSize 有效 → grid 数学计算
    if (inv.Grid.Valid && GridOnScreen(inv, displayW, displayH)
        && inv.Grid.CellSize > 0.f) {
        const float cell = inv.Grid.CellSize;
        const float x = inv.Grid.GridScreenX + static_cast<float>(item.SlotX) * cell;
        const float y = inv.Grid.GridScreenY + static_cast<float>(item.SlotY) * cell;
        const float w = static_cast<float>(item.Width > 0 ? item.Width : 1) * cell;
        const float h = static_cast<float>(item.Height > 0 ? item.Height : 1) * cell;
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;
        if (cx >= 0.f && cy >= 0.f && cx < displayW && cy < displayH) {
            return ScreenRect{x, y, w, h};
        }
    }

    // 3) Grid 有效但 CellSize 异常 → 默认 40px 兜底
    if (inv.Grid.Valid && inv.Grid.GridScreenX >= 0.f && inv.Grid.GridScreenY >= 0.f) {
        const float defaultCell = 40.0f;
        const float cell = (inv.Grid.CellSize > 0.f) ? inv.Grid.CellSize : defaultCell;
        const float x = inv.Grid.GridScreenX + static_cast<float>(item.SlotX) * cell;
        const float y = inv.Grid.GridScreenY + static_cast<float>(item.SlotY) * cell;
        const float w = static_cast<float>(item.Width > 0 ? item.Width : 1) * cell;
        const float h = static_cast<float>(item.Height > 0 ? item.Height : 1) * cell;
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;
        const float margin = cell * 2.0f;
        if (cx >= -margin && cy >= -margin && cx < displayW + margin && cy < displayH + margin) {
            return ScreenRect{x, y, w, h};
        }
    }

    // 4) Grid.Valid=false 但有 GridScreenX/Y → 最宽松兜底（重铸台打开时 MainInventory Grid 失效的情况）
    if (!inv.Grid.Valid && inv.Grid.GridScreenX >= 0.f && inv.Grid.GridScreenY >= 0.f
        && item.SlotX >= 0 && item.SlotY >= 0) {
        const float defaultCell = 40.0f;
        const float cell = (inv.Grid.CellSize > 0.f) ? inv.Grid.CellSize : defaultCell;
        const float x = inv.Grid.GridScreenX + static_cast<float>(item.SlotX) * cell;
        const float y = inv.Grid.GridScreenY + static_cast<float>(item.SlotY) * cell;
        const float w = static_cast<float>(item.Width > 0 ? item.Width : 1) * cell;
        const float h = static_cast<float>(item.Height > 0 ? item.Height : 1) * cell;
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;
        const float margin = cell * 3.0f;
        if (cx >= -margin && cy >= -margin && cx < displayW + margin && cy < displayH + margin) {
            return ScreenRect{x, y, w, h};
        }
    }

    // 5) 完全兜底：物品有 ScreenX/Y（可能 ScreenValid=false 但屏幕内）
    //    前面的 ItemOnScreen 已检查 ScreenValid=true，这里放宽
    if (item.ScreenW > 0.f && item.ScreenH > 0.f
        && item.ScreenX + item.ScreenW > 0.f && item.ScreenY + item.ScreenH > 0.f
        && item.ScreenX < displayW && item.ScreenY < displayH) {
        return ScreenRect{item.ScreenX, item.ScreenY, item.ScreenW, item.ScreenH};
    }

    return std::nullopt;
}

// POE2 背包分成多个面板（装备区、通货区等），找最大的主装备面板
inline std::optional<PluginSDK::Inventory> FindMainInventory(
    const PluginSDK::Context* ctx) {
    if (!ctx) return std::nullopt;
    static const char* kNames[] = {
        "MainInventory1", "Main Inventory", "Backpack", "Player Inventory",
        "PlayerInventory", "MainBackpack", "Inventory"};
    for (const char* want : kNames) {
        for (const auto& inv : ctx->Inventory.GetAll()) {
            const char* name = ctx->Inventory.GetName(inv.InventoryId);
            if (name && std::strcmp(name, want) == 0 && inv.Grid.Valid)
                return inv;
        }
    }
    // 回退 1：找最大的 MainInventory* 前缀面板（POE2 可能有多个）
    std::optional<PluginSDK::Inventory> best;
    int bestSlots = 0;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (!name || !inv.Grid.Valid) continue;
        if (std::strncmp(name, "MainInventory", 12) != 0) continue;
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots > bestSlots) {
            bestSlots = slots;
            best = inv;
        }
    }
    if (best) return best;
    // 回退 2：找最大的中等面板（20~200 格）
    bestSlots = 0;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;
        if (slots < 20 || slots > 200) continue;
        if (slots > bestSlots) {
            bestSlots = slots;
            best = inv;
        }
    }
    return best;
}

// 背包是否打开（任一 MainInventory* 面板有物品即为打开）
// 关键：不要求 Grid.Valid=true，因为重铸台打开时 MainInventory Grid 可能失效
// 但物品数据仍然存在，背包实际是打开的
inline bool IsInventoryOpen(const PluginSDK::Context* ctx) {
    if (!ctx) return false;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (!name) continue;
        bool isBag = false;
        if (std::strncmp(name, "MainInventory", 12) == 0) isBag = true;
        else if (std::strstr(name, "Backpack") != nullptr) isBag = true;
        else if (std::strstr(name, "Player Inventory") != nullptr) isBag = true;
        else if (std::strstr(name, "PlayerInventory") != nullptr) isBag = true;
        if (isBag && inv.Items.size() > 0) return true;
    }
    return false;
}

// 是否有任何物品面板打开（背包/仓库/商人）——pickmyloot Gates.h 逻辑
// >=40 格的 inventory 视为面板（背包 12x5=60，仓库更大；HUD 腰带 5x2=10 被跳过）
inline bool AnyItemPanelOpen(const PluginSDK::Context* ctx) {
    if (!ctx) return false;
    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        if (inv.TotalBoxesX * inv.TotalBoxesY < 40) continue;
        return true;
    }
    return false;
}

// 仓库是否打开（有 >=40 格的 inventory 且不是主背包）
inline bool IsStashOpen(const PluginSDK::Context* ctx) {
    if (!ctx) return false;
    auto mainInv = FindMainInventory(ctx);
    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        if (inv.TotalBoxesX * inv.TotalBoxesY < 40) continue;
        // 如果找到了主背包，排除它
        if (mainInv && inv.InventoryId == mainInv->InventoryId) continue;
        // 如果没找到主背包，排除所有 MainInventory* 前缀的面板
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (!mainInv && name && std::strncmp(name, "MainInventory", 12) == 0) continue;
        return true;
    }
    return false;
}

// 获取屏幕尺寸（从 GameService）
inline void GetScreenSize(const PluginSDK::Context* ctx, float& w, float& h) {
    w = 0.f;
    h = 0.f;
    if (!ctx) return;
    auto ss = ctx->Game.GetScreenSize();
    w = ss.Width;
    h = ss.Height;
}

// 重铸台/合成面板是否打开（POE2 专属检测）
// 策略：找一个 Grid.Valid=true、且格子数较少（4~20 格）的 inventory——
//       这通常是合成面板（3个输入槽+1个输出槽，或更多变体）。
//       同时排除 HUD 腰带（5x2=10 但名字含 Belt）和其他已知小面板。
inline bool IsBenchInventoryOpen(const PluginSDK::Context* ctx) {
    if (!ctx) return false;
    static const char* kSkipNames[] = {
        "Belt", "Flask", "Amulet", "Ring", "Helmet", "Body", "Gloves",
        "Boots", "Weapon", "Shield", "Quiver", "Trinket", "Charm",
        "Jewellery", "Equipment", "Equip", "Skill", "Gem",
    };

    auto mainInv = FindMainInventory(ctx);
    const int mainInvId = mainInv ? mainInv->InventoryId : -1;

    // 一次性获取屏幕尺寸，避免循环内重复查询
    float displayW = 0.f, displayH = 0.f;
    GetScreenSize(ctx, displayW, displayH);
    if (displayW <= 0.f || displayH <= 0.f) return false;

    for (const auto& inv : ctx->Inventory.GetAll()) {
        if (!inv.Grid.Valid) continue;
        const int slots = inv.TotalBoxesX * inv.TotalBoxesY;

        // 合成面板：格子数在 4~24 之间（3入+1出=4，有些可能更大）
        if (slots < 4 || slots > 24) continue;

        // 跳过主背包（虽然主背包 >24 格已经被上面排除了）
        if (mainInvId != -1 && inv.InventoryId == mainInvId) continue;

        // 跳过名字里包含装备部位关键词的面板
        const char* name = ctx->Inventory.GetName(inv.InventoryId);
        if (name) {
            bool skip = false;
            for (const char* kw : kSkipNames) {
                if (std::strstr(name, kw) != nullptr) { skip = true; break; }
            }
            if (skip) continue;
        }

        // 还需要 Grid 在屏幕上（不是关闭状态的空 inventory）
        if (GridOnScreen(inv, displayW, displayH)) return true;
    }
    return false;
}

// 重铸台相关的"任意物品面板"是否打开
// 打开重铸台时，可能主背包也开着（显示玩家物品），也可能只有合成面板。
// 所以这个检测是宽松的：要么主背包开着（有合成按钮坐标），要么有合成面板。
inline bool IsBenchContextOpen(const PluginSDK::Context* ctx) {
    if (!ctx) return false;
    if (IsInventoryOpen(ctx)) return true;
    if (IsBenchInventoryOpen(ctx)) return true;
    return false;
}

} // namespace TabletReforgeGame
