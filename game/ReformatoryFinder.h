// ReformatoryFinder.h — 重铸台/仓库实体查找 + 屏幕坐标转换
//
// 通过 Entities.Enumerate 遍历场景实体，按 Path 匹配重铸台或仓库。
// 找到后用 Render.WorldToScreen 转换世界坐标到屏幕坐标，供鼠标点击。
//
// 实体的 Path 标定值存在 CalibData.benchEntityPath / stashEntityPath 里。
// 匹配策略：大小写不敏感的子串匹配（Path 可能含完整 Metadata 路径）。
//
// 安全：Enumerate 和 WorldToScreen 都是只读操作，零风险。
#pragma once

#include "../config/CalibData.h"
#include "../input/Win32Input.h"
#include "../sdk/PluginSDK.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

namespace TabletReforgeGame {

// 实体查找结果（重铸台/仓库通用）
struct BenchLocation {
    float screenX = 0.f;       // 屏幕坐标 X（像素）
    float screenY = 0.f;       // 屏幕坐标 Y（像素）
    float worldX = 0.f;        // 世界坐标 X（调试用）
    float worldY = 0.f;
    float worldZ = 0.f;
    uintptr_t entityAddress = 0;
    std::wstring matchedPath;  // 实际匹配到的 Path（调试/标定用）
    bool onScreen = false;     // WorldToScreen 是否成功且在屏内
};

// 宽字符串大小写不敏感子串匹配
inline bool ContainsCIW(std::wstring_view hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    // 把 needle 转小写
    std::wstring nl;
    nl.reserve(needle.size());
    for (wchar_t c : needle) nl.push_back(static_cast<wchar_t>(std::towlower(c)));
    for (size_t i = 0; i + nl.size() <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < nl.size(); ++j) {
            const wchar_t c = static_cast<wchar_t>(std::towlower(hay[i + j]));
            if (c != nl[j]) break;
        }
        if (j == nl.size()) return true;
    }
    return false;
}

// 窄字符串转宽字符串（用于把 CalibData 里的 std::string 转成 wstring 比较）
inline std::wstring StringToWString(const std::string& s) {
    if (s.empty()) return {};
    int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                           static_cast<int>(s.size()), wide.data(), needed);
    return wide;
}

// 屏幕坐标是否在屏幕范围内
inline bool IsOnScreen(float sx, float sy, const PluginSDK::Context* ctx) {
    if (!ctx) return false;
    auto ss = ctx->Game.GetScreenSize();
    if (ss.Width <= 0.f || ss.Height <= 0.f) return false;
    const float margin = 10.f;
    return sx >= -margin && sy >= -margin && sx < ss.Width + margin && sy < ss.Height + margin;
}

// 按实体 Path 子串查找，并转换世界坐标到屏幕坐标
// pathNeedle：要匹配的 Path 子串（窄字符串，内部转宽）
inline std::optional<BenchLocation> FindEntityByPath(const PluginSDK::Context* ctx,
                                                      const std::wstring& pathNeedle) {
    if (!ctx || pathNeedle.empty()) return std::nullopt;

    std::optional<BenchLocation> result;
    ctx->Entities.Enumerate([&](const PluginSDK::Entity& e) -> bool {
        if (!e.IsValid) return true; // 继续遍历
        if (e.Path.empty()) return true;

        // 按 Path 子串匹配
        if (!ContainsCIW(e.Path, pathNeedle)) return true;

        // 找到了，转换世界坐标到屏幕坐标
        BenchLocation loc;
        loc.worldX = e.WorldX;
        loc.worldY = e.WorldY;
        loc.worldZ = e.WorldZ;
        loc.entityAddress = e.Address;
        loc.matchedPath = e.Path;

        float sx = 0.f, sy = 0.f;
        if (ctx->Render.WorldToScreen(e.WorldX, e.WorldY, e.WorldZ, sx, sy)) {
            loc.screenX = sx;
            loc.screenY = sy;
            loc.onScreen = IsOnScreen(sx, sy, ctx);
        } else {
            loc.onScreen = false;
        }

        result = loc;
        return false; // 停止遍历
    });

    return result;
}

// 查找重铸台实体（便捷封装）
inline std::optional<BenchLocation> FindBench(const PluginSDK::Context* ctx,
                                               const TabletReforgeConfig::CalibData& calib) {
    if (calib.benchEntityPath.empty()) return std::nullopt;
    return FindEntityByPath(ctx, StringToWString(calib.benchEntityPath));
}

// 查找仓库实体（便捷封装）
// 如果 stashEntityPath 为空，返回 nullopt（状态机会等待用户手动开仓库）
inline std::optional<BenchLocation> FindStash(const PluginSDK::Context* ctx,
                                               const TabletReforgeConfig::CalibData& calib) {
    if (calib.stashEntityPath.empty()) return std::nullopt;
    return FindEntityByPath(ctx, StringToWString(calib.stashEntityPath));
}

// 点击仓库实体打开仓库面板（需要仓库实体在屏幕内）
inline bool OpenStashPanel(const PluginSDK::Context* ctx,
                            const TabletReforgeConfig::CalibData& calib) {
    auto loc = FindStash(ctx, calib);
    if (!loc) return false;
    if (!loc->onScreen) return false;
    // 移动到仓库位置，左键点击打开
    const int sx = static_cast<int>(loc->screenX + 0.5f);
    const int sy = static_cast<int>(loc->screenY + 0.5f);
    TabletReforgeInput::MoveCursorScreen(sx, sy);
    TabletReforgeInput::LeftClickScreen(sx, sy);
    return true;
}

// 实体类型显示名
inline const char* EntityTypeName(PluginSDK::EntityType t) {
    switch (t) {
        case PluginSDK::EntityType::Unidentified:      return "未识别";
        case PluginSDK::EntityType::Chest:              return "箱子";
        case PluginSDK::EntityType::NPC:                return "NPC";
        case PluginSDK::EntityType::Player:             return "玩家";
        case PluginSDK::EntityType::Shrine:             return "神龛";
        case PluginSDK::EntityType::Monster:            return "怪物";
        case PluginSDK::EntityType::DeliriumBomb:       return "谵妄炸弹";
        case PluginSDK::EntityType::DeliriumSpawner:    return "谵妄生成器";
        case PluginSDK::EntityType::OtherImportant:     return "重要物";
        case PluginSDK::EntityType::Item:               return "物品";
        case PluginSDK::EntityType::Renderable:         return "可渲染";
        case PluginSDK::EntityType::AreaTransition:     return "传送";
        case PluginSDK::EntityType::ExpeditionMarker:   return "远征标记";
        case PluginSDK::EntityType::ExpeditionRemnant:  return "远征遗物";
        default:                                        return "未知";
    }
}

// 列出附近实体（标定向导用，按距离排序，可指定类型过滤）
// filterType = Unidentified 表示不过滤，显示所有类型
struct NearbyEntity {
    std::wstring path;
    uintptr_t address = 0;
    float distance = 0.f;  // 到玩家的距离
    float worldX = 0.f, worldY = 0.f, worldZ = 0.f;
    PluginSDK::EntityType type = PluginSDK::EntityType::Unidentified;
};

inline std::vector<NearbyEntity> ListNearbyEntities(const PluginSDK::Context* ctx,
                                                      float maxRange = 80.f,
                                                      PluginSDK::EntityType filterType =
                                                          PluginSDK::EntityType::Unidentified) {
    std::vector<NearbyEntity> out;
    if (!ctx) return out;

    auto snap = ctx->Game.GetSnapshot();
    const float px = snap.Player.GridPositionX;
    const float py = snap.Player.GridPositionY;

    ctx->Entities.Enumerate([&](const PluginSDK::Entity& e) -> bool {
        if (!e.IsValid) return true;
        if (e.Path.empty()) return true;

        // 类型过滤（Unidentified = 不过滤）
        if (filterType != PluginSDK::EntityType::Unidentified
            && e.EntityType != filterType) {
            return true;
        }

        // 排除玩家自己
        if (e.EntityType == PluginSDK::EntityType::Player
            && e.EntitySubtype == PluginSDK::EntitySubtype::PlayerSelf) {
            return true;
        }

        const float dx = e.GridPositionX - px;
        const float dy = e.GridPositionY - py;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > maxRange) return true;

        NearbyEntity ne;
        ne.path = e.Path;
        ne.address = e.Address;
        ne.distance = dist;
        ne.worldX = e.WorldX;
        ne.worldY = e.WorldY;
        ne.worldZ = e.WorldZ;
        ne.type = e.EntityType;
        out.push_back(std::move(ne));
        return true;
    });

    // 按距离排序
    std::sort(out.begin(), out.end(),
              [](const NearbyEntity& a, const NearbyEntity& b) { return a.distance < b.distance; });
    return out;
}

// 兼容旧函数名（保留）
inline std::vector<NearbyEntity> ListNearbyInteractables(const PluginSDK::Context* ctx,
                                                          float maxRange = 80.f) {
    return ListNearbyEntities(ctx, maxRange, PluginSDK::EntityType::Unidentified);
}

} // namespace TabletReforgeGame
