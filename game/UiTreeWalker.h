// UiTreeWalker.h — UI 树遍历（标定向导用）
//
// 递归遍历游戏 UI 树，收集所有可见节点的 StringId/Text/屏幕坐标，
// 供标定向导列出候选按钮让用户选择。
//
// v0.1（MVP）：只放骨架，标定向导在 v0.2 实现。
// v0.2：实现 CollectVisible + FindByStringId。
//
// 安全：UiService 的所有方法都是只读查询，零风险。但 UI 树可能很深，
// 限制递归深度避免性能问题。
#pragma once

#include "../sdk/PluginSDK.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TabletReforgeGame {

// UI 节点信息（标定向导列表用）
struct UiNodeInfo {
    uintptr_t address = 0;
    std::string stringId;
    std::string text;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;  // 屏幕坐标
    bool visible = false;
    int depth = 0;
};

// 递归收集可见 UI 节点（标定向导用）
// maxDepth：最大递归深度（防止无限递归，默认 15）
// maxNodes：最大收集节点数（0=无上限，默认 0）
// minSize：最小尺寸过滤（像素，跳过装饰性微小节点，默认 8）
// requireIdOrText：是否要求节点有 StringId 或 Text（跳过无标识节点，默认 true）
// 只收集 visible=true 且 ComputeScreenRect 成功的节点
inline std::vector<UiNodeInfo> CollectVisible(const PluginSDK::Context* ctx,
                                               int maxDepth = 15,
                                               int maxNodes = 0,
                                               float minSize = 8.f,
                                               bool requireIdOrText = true) {
    std::vector<UiNodeInfo> out;
    if (!ctx) return out;

    uintptr_t root = ctx->Ui.GetGameUiRoot();
    if (!root) return out;

    // 用显式栈代替递归，避免栈溢出
    struct StackItem { uintptr_t addr; int depth; };
    std::vector<StackItem> stack;
    stack.reserve(256);
    stack.push_back({root, 0});

    // 预分配输出空间，避免反复 realloc
    out.reserve(maxNodes > 0 ? static_cast<size_t>((std::min)(maxNodes, 512)) : 512);

    // 地址有效性范围（用户态地址空间）
    auto isValidAddr = [](uintptr_t a) {
        return a >= 0x10000ull && a <= 0x00007FFFFFFFFFFFull;
    };

    while (!stack.empty()) {
        // maxNodes > 0 时限制节点数，0 表示无上限
        if (maxNodes > 0 && static_cast<int>(out.size()) >= maxNodes) break;

        StackItem cur = stack.back();
        stack.pop_back();
        if (cur.depth > maxDepth) continue;
        if (!isValidAddr(cur.addr)) continue;

        // 读节点信息
        auto elem = ctx->Ui.Read(cur.addr);
        if (!elem.Valid) continue;

        UiNodeInfo info;
        info.address = cur.addr;
        info.stringId = ctx->Ui.GetStringId(cur.addr);
        info.text = ctx->Ui.GetText(cur.addr);
        info.visible = ctx->Ui.IsVisible(cur.addr);
        info.depth = cur.depth;

        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if (ctx->Ui.ComputeScreenRect(cur.addr, x, y, w, h)) {
            info.x = x; info.y = y; info.w = w; info.h = h;
        }

        // 过滤条件：
        // 1. 必须可见
        // 2. 必须有有效屏幕坐标且尺寸 >= minSize（跳过 1x1 装饰节点）
        // 3. requireIdOrText=true 时必须有 StringId 或 Text（跳过无标识节点）
        if (info.visible && w >= minSize && h >= minSize) {
            if (!requireIdOrText || !info.stringId.empty() || !info.text.empty()) {
                out.push_back(std::move(info));
            }
        }

        // 把子节点压栈（逆序压，保证顺序遍历）
        auto children = ctx->Ui.GetChildren(cur.addr);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (isValidAddr(static_cast<uintptr_t>(*it))) {
                stack.push_back({static_cast<uintptr_t>(*it), cur.depth + 1});
            }
        }
    }

    return out;
}

// 按 StringId 精确查找 UI 节点
inline std::optional<UiNodeInfo> FindByStringId(const PluginSDK::Context* ctx,
                                                  const std::string& stringId) {
    if (!ctx || stringId.empty()) return std::nullopt;
    uintptr_t root = ctx->Ui.GetGameUiRoot();
    if (!root) return std::nullopt;

    // 优先用 UiService.FindPanelByStringId（宿主优化过）
    uintptr_t panel = ctx->Ui.FindPanelByStringId(root, stringId.c_str());
    if (panel) {
        UiNodeInfo info;
        info.address = panel;
        info.stringId = stringId;
        info.visible = ctx->Ui.IsVisible(panel);
        info.text = ctx->Ui.GetText(panel);
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if (ctx->Ui.ComputeScreenRect(panel, x, y, w, h)) {
            info.x = x; info.y = y; info.w = w; info.h = h;
        }
        return info;
    }
    return std::nullopt;
}

// ============================================================================
// 鉴定完成通知检测（用于 TickWaitForIdentification 状态）
// ============================================================================
// POE2 在 NPC 鉴定完成后，会在屏幕中央/上方显示白色系统通知，类似：
//   "已鉴定 N 个物品" (简中) / "鑑定 N 個物品" (繁中) / "Identified N items" (英文)
// 该通知是临时 UI 元素，会自动消失。
//
// 检测策略：
//   1. 遍历 UI 树，对每个可见节点检查 text 和 stringId
//   2. 关键词匹配（覆盖中英文 + 繁简体）
//   3. 中文客户端下 GetText 可能返回乱码（????），因此同时检查 StringId
//   4. 若节点文本疑似乱码（含大量 '?'），跳过该节点避免误判
//
// 性能：限制 maxNodes=500、maxDepth=12，避免扫描过深影响帧率
// 返回：匹配的节点文本（UTF-8），未找到返回空字符串
inline std::string FindIdentifyNotification(const PluginSDK::Context* ctx) {
    if (!ctx) return "";

    // 使用 CollectVisible 遍历 UI 树（关闭 requireIdOrText 过滤，避免漏掉纯文本通知节点）
    auto nodes = CollectVisible(ctx, 12, 500, 4.f, false);

    // 关键词列表（覆盖简中/繁中/英文）
    // 注意：中文关键词在窄字符 GetText 下可能变乱码，但英文关键词仍可匹配
    static const char* kKeywords[] = {
        // 英文（最可靠，窄字符下也能正确匹配）
        "Identified", "identified", "Items identified", "items identified",
        "Identify", "identify",
        // 简中（仅在 GetText 返回正确 UTF-8 时匹配）
        "已鉴定", "鉴定", "已鉴定 ",
        // 繁中
        "已鑑定", "鑑定", "已鑑定 ",
    };

    for (const auto& node : nodes) {
        // 跳过乱码节点（避免误匹配 '????' 中的 '?'）
        // 但仍检查 stringId（stringId 通常是英文，不会乱码）

        // 1. 检查 text 字段（英文客户端可靠，中文客户端可能乱码）
        if (!node.text.empty()) {
            bool textGarbled = false;
            int garbledCount = 0;
            int totalChars = static_cast<int>(node.text.size());
            for (int i = 0; i < totalChars; ++i) {
                unsigned char c = static_cast<unsigned char>(node.text[i]);
                if (c == '?' || (c < 32 && c != '\t' && c != '\n' && c != '\r')) {
                    garbledCount++;
                }
            }
            textGarbled = (garbledCount > totalChars * 0.3);

            if (!textGarbled) {
                for (const char* kw : kKeywords) {
                    if (std::strstr(node.text.c_str(), kw) != nullptr) {
                        return node.text;
                    }
                }
            }
        }

        // 2. 检查 stringId 字段（通常为英文，中文客户端下也可靠）
        if (!node.stringId.empty()) {
            for (const char* kw : kKeywords) {
                if (std::strstr(node.stringId.c_str(), kw) != nullptr) {
                    return node.stringId;
                }
            }
        }
    }
    return "";
}

} // namespace TabletReforgeGame
