// Gates.h — 安全门控
//
// 复用 pickmyloot Gates.h 的逻辑，每个门控返回 true 表示"通过"。
// 状态机每帧 Tick 开头检查，命中即 Abort。
//
// 门控项：
//   - InGame：游戏已加载且在前台
//   - InTownOrHideout：在城镇或藏身处（不在野外乱跑）
//   - NoEnemyNear：附近没有敌人
//   - AreaUnchanged：区域没换（AreaChangeCounter 没变）
//   - NoUserCancel：用户没按右键/Esc 取消
//
// 安全：所有检查都是只读查询，零风险。
#pragma once

#include "../config/Settings.h"
#include "../input/Win32Input.h"
#include "../sdk/PluginSDK.h"

#include <cmath>

namespace TabletReforgeFlow {

// 游戏已加载且在前台
inline bool IsInGameAndForeground(const PluginSDK::Context* ctx,
                                   const TabletReforgeConfig::Settings& s) {
    if (!ctx) return false;
    if (!ctx->Game.IsInGame()) return false;
    if (s.gateNotForeground && !ctx->Game.IsForeground()) return false;
    return true;
}

// 在城镇或藏身处
inline bool InTownOrHideout(const PluginSDK::Snapshot& snap,
                             const TabletReforgeConfig::Settings& s) {
    if (!s.gateTownHideout) return true; // 关闭门控则放行
    return snap.IsTown || snap.IsHideout;
}

// 附近没有敌人（复用 pickmyloot EnemyNear 逻辑）
inline bool NoEnemyNear(const PluginSDK::Snapshot& snap,
                         const TabletReforgeConfig::Settings& s) {
    if (!s.gateEnemyNear) return true; // 关闭门控则放行
    const float px = snap.Player.GridPositionX;
    const float py = snap.Player.GridPositionY;
    const float range = static_cast<float>(s.enemyRange);
    for (const auto& e : snap.Entities) {
        if (!e.IsValid) continue;
        if (e.EntityType != PluginSDK::EntityType::Monster) continue;
        if (e.CurrentHP <= 0) continue;
        if (e.EntityState == PluginSDK::EntityState::MonsterFriendly) continue;
        const float dx = e.GridPositionX - px;
        const float dy = e.GridPositionY - py;
        if (std::sqrt(dx * dx + dy * dy) <= range) return false; // 有敌人
    }
    return true;
}

// 插件UI可见性全局标志（由主插件在DrawSettings/DrawUI中设置）
inline bool& PluginUIVisibleRef() { static bool v = false; return v; }
inline void SetPluginUIVisible(bool visible) { PluginUIVisibleRef() = visible; }
inline bool IsPluginUIVisible() { return PluginUIVisibleRef(); }

inline bool& StateMachinePanelOpenRef() { static bool v = false; return v; }
inline void SetStateMachinePanelOpen(bool open) { StateMachinePanelOpenRef() = open; }
inline bool StateMachineHasPanelOpen() { return StateMachinePanelOpenRef(); }

// 没有菜单遮挡（忽略插件自身UI和状态机主动打开的面板）
inline bool NoMenuVisible(const PluginSDK::Context* ctx,
                           const TabletReforgeConfig::Settings& s) {
    if (!s.gateMenu) return true;
    if (!ctx) return false;
    // 插件自身的ImGui覆盖层不视为遮挡
    if (IsPluginUIVisible()) return true;
    // 状态机主动打开的仓库/重铸台面板不视为遮挡
    if (StateMachineHasPanelOpen()) return true;
    return !ctx->Game.IsMenuVisible();
}

// 区域没换（用传入的 baseline 比较）
inline bool AreaUnchanged(uint64_t baselineCounter, uint64_t currentCounter) {
    return baselineCounter == currentCounter;
}

// 用户没按右键取消
inline bool NoRightClickCancel(const TabletReforgeConfig::Settings& s) {
    if (!s.cancelOnRightClick) return true;
    return !TabletReforgeInput::IsRightMouseDown();
}

// 用户没按 Esc 取消
inline bool NoEscCancel(const TabletReforgeConfig::Settings& s) {
    if (!s.cancelOnEsc) return true;
    return !TabletReforgeInput::IsKeyDown(0x1B); // VK_ESCAPE
}

// 综合门控检查：返回 true 表示全部通过，false 表示应 Abort
// areaBaseline：启动时记录的 AreaChangeCounter
inline bool AllGatesPass(const PluginSDK::Context* ctx,
                          const TabletReforgeConfig::Settings& s,
                          uint64_t areaBaseline) {
    if (!IsInGameAndForeground(ctx, s)) return false;

    // GetSnapshot 较重，但门控需要它。状态机里已经取过快照就传进来，避免重复取。
    // 这里作为独立检查时用。
    auto snap = ctx->Game.GetSnapshot();
    if (!InTownOrHideout(snap, s)) return false;
    if (!NoEnemyNear(snap, s)) return false;
    if (!NoMenuVisible(ctx, s)) return false;
    if (!AreaUnchanged(areaBaseline, snap.AreaChangeCounter)) return false;
    if (!NoRightClickCancel(s)) return false;
    if (!NoEscCancel(s)) return false;
    return true;
}

} // namespace TabletReforgeFlow
