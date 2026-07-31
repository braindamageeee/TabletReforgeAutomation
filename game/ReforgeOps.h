// ReforgeOps.h — 重铸台合成 UI 操作
//
// 提供合成流程所需的坐标查询：
//   - ResolveCombineButton：解析"合成"按钮的屏幕坐标（StringId 优先，坐标兜底）
//   - ResolveOutputSlot：解析"产物槽"的屏幕坐标
//
// v0.1（MVP）：只用 CalibData 里的兜底坐标。
// v0.2（标定向导）：优先用 UiService.FindPanelByStringId + ComputeScreenRect。
//
// 实际点击操作在状态机里用 Win32Input 完成，这里只负责"告诉状态机点哪里"。
//
// 安全：FindPanelByStringId 和 ComputeScreenRect 都是只读 UI 查询，零风险。
#pragma once

#include "../config/CalibData.h"
#include "PanelDetector.h"
#include "InventoryChecker.h"
#include "ReformatoryFinder.h"
#include "../sdk/PluginSDK.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <windows.h>

// ============================================================
// UTF-8 安全的文字比较工具
// ============================================================

// 检测字符串是否包含多字节 UTF-8 字符（byte >= 0x80）
inline bool HasMultibyteChars(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80) return true;
    }
    return false;
}

// UTF-8 安全的 tolower：只对 ASCII 字符（< 0x80）做 tolower，多字节字符保持不变
inline std::string Utf8SafeToLower(const std::string& s) {
    std::string result = s;
    for (auto& c : result) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x80) {
            c = static_cast<char>(std::tolower(uc));
        }
    }
    return result;
}

// UTF-8 安全的子串查找（跳过 tolower 对多字节字符的破坏）
inline bool ContainsKeyword(const std::string& text, const std::string& keyword) {
    if (keyword.empty()) return false;
    std::string lowerText = Utf8SafeToLower(text);
    std::string lowerKw = Utf8SafeToLower(keyword);
    return lowerText.find(lowerKw) != std::string::npos;
}

// 计算关键词在文本中的匹配分数（UTF-8 安全）
inline int CalcKeywordScore(const std::string& text, const std::vector<std::string>& keywords) {
    int score = 0;
    std::string lowerText = Utf8SafeToLower(text);
    for (const auto& kw : keywords) {
        std::string lowerKw = Utf8SafeToLower(kw);
        auto pos = lowerText.find(lowerKw);
        if (pos != std::string::npos) {
            if (lowerText == lowerKw) score += 1000;
            else if (pos == 0) score += 500;
            else score += 200;
            if (text.size() <= 10) score += 100;
            if (text.size() <= 5) score += 100;
            if (text.size() > 50) score -= 500;
            int spaces = 0;
            for (char c : text) if (c == ' ') spaces++;
            if (spaces > 3) score -= 100;
        }
    }
    return score;
}

// UTF-8 转 UTF-16（用于 OutputDebugStringW）
inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring wide(needed - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], needed);
    return wide;
}

// UTF-16 转 UTF-8（用于正确转换宽字符字符串）
inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string utf8(needed - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], needed, nullptr, nullptr);
    return utf8;
}

// 检测字符串是否是乱码（包含大量 '?' 或不可打印字符）
inline bool IsGarbledText(const std::string& s) {
    if (s.empty()) return true;
    int garbledCount = 0;
    int totalChars = static_cast<int>(s.size());
    for (int i = 0; i < totalChars; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '?' || (c < 32 && c != '\t' && c != '\n' && c != '\r')) {
            garbledCount++;
        }
    }
    // 超过 30% 的字符是乱码，认为是乱码
    return garbledCount > totalChars * 0.3;
}

// 正确读取 UI 节点 StringId（绕过窄字节转换的乱码问题）
// 优先使用宽字符读取，仅在宽字符也失败时回退到原有方法
inline std::string GetStringIdUtf8(const PluginSDK::Context* ctx, uintptr_t addr) {
    if (!ctx || addr == 0) return "";

    // 方法1: 尝试从 UiElement.StringIdAddress 直接读取宽字符
    auto elem = ctx->Ui.Read(addr);
    if (elem.Valid && elem.StringIdAddress != 0) {
        std::wstring wideStr = ctx->Memory.ReadWString(elem.StringIdAddress);
        if (!wideStr.empty()) {
            std::string utf8 = WideToUtf8(wideStr);
            if (!utf8.empty() && !IsGarbledText(utf8)) {
                return utf8;
            }
        }
    }

    // 方法2: 使用原有的 GetStringId（可能对 ASCII StringId 有效）
    std::string result = ctx->Ui.GetStringId(addr);
    if (!result.empty() && !IsGarbledText(result)) {
        return result;
    }

    // 方法3: 尝试 Sekhema 的 get_ui_string_id（如果可用）
    // 注意：这是一个备选方案，需要验证是否适用于所有 UI 元素
    return result;
}

// 正确读取 UI 节点文本（绕过窄字节转换的乱码问题）
inline std::string GetTextUtf8(const PluginSDK::Context* ctx, uintptr_t addr) {
    if (!ctx || addr == 0) return "";

    // 方法1: 使用原有的 GetText（可能对英文有效）
    std::string result = ctx->Ui.GetText(addr);
    if (!result.empty() && !IsGarbledText(result)) {
        return result;
    }

    // 如果原方法返回乱码，返回空字符串让调用者知道需要回退
    return result;
}

// 宽字符版 OutputDebugString（让中文正常显示在 DebugView 中）
// 同时用 A 和 W 版本输出，确保 DebugView 无论配置如何都能捕获
inline void DebugLogW(const std::string& utf8Msg) {
    std::wstring wide = Utf8ToWide(utf8Msg);
    OutputDebugStringW(wide.c_str());
    // 同时用 A 版本输出（虽然中文会乱码，但关键英文信息可见）
    OutputDebugStringA(utf8Msg.c_str());
}

// UI 树诊断转储：用于 debug 日志（用 OutputDebugStringW 输出，中文正常显示）
inline void DumpUiTreeForDebug(const PluginSDK::Context* ctx,
                               uintptr_t addr,
                               const std::vector<std::string>& keywords,
                               int depth, int maxDepth,
                               std::string& outLog,
                               int& nodeCount) {
    if (depth > maxDepth || addr == 0) return;
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return;
    nodeCount++;

    // 使用 UTF-8 安全的读取方法
    std::string text = GetTextUtf8(ctx, addr);
    std::string stringId = GetStringIdUtf8(ctx, addr);
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;
    bool hasRect = ctx->Ui.ComputeScreenRect(addr, rx, ry, rw, rh);
    bool visible = ctx->Ui.IsVisible(addr);
    auto children = ctx->Ui.GetChildren(addr);

    bool keywordHit = false;
    if (!text.empty()) {
        for (const auto& kw : keywords) {
            if (ContainsKeyword(text, kw)) { keywordHit = true; break; }
        }
    }

    // 匹配 stringId 关键词（如果用户填的其实是 StringId）
    bool stringIdHit = false;
    if (!stringId.empty()) {
        for (const auto& kw : keywords) {
            if (!kw.empty() && ContainsKeyword(stringId, kw)) { stringIdHit = true; break; }
        }
    }

    // 输出所有节点（包括无文字节点），便于定位按钮结构
    char line[1024];
    sprintf_s(line, "  [%06d addr=%llX depth=%d vis=%d children=%zu] text='%s' stringId='%s' rect=(%.0f,%.0f,%.0f,%.0f)%s%s%s",
        nodeCount,
        (unsigned long long)addr, depth,
        visible ? 1 : 0,
        children.size(),
        text.c_str(),
        stringId.c_str(),
        rx, ry, rw, rh,
        keywordHit ? " *** TEXT HIT ***" : "",
        stringIdHit ? " *** STRINGID HIT ***" : "",
        (!text.empty() && HasMultibyteChars(text)) ? " [MB]" : "");
    outLog += line;
    outLog += "\n";

    for (auto child : children) {
        DumpUiTreeForDebug(ctx, child, keywords, depth + 1, maxDepth, outLog, nodeCount);
    }
}

// ============================================================
// UI 树文字搜索工具
// ============================================================

#include <optional>
#include <vector>

namespace TabletReforgeGame {

// 按钮屏幕坐标（中心点，像素）
struct ButtonPos {
    int x = -1;
    int y = -1;
    bool valid = false;
};

// ============================================================
// UI 树文字搜索工具
// ============================================================

// 带评分的搜索结果
struct SearchResult {
    uintptr_t addr = 0;
    int score = 0;           // 匹配分数（越高越好）
    std::string text;        // 节点文字
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// 用文字内容在 UI 树中搜索节点（递归，深度限制）
// 修复：UTF-8 安全比较，避免 tolower 破坏中文字符
inline SearchResult FindBestUiNodeByText(const PluginSDK::Context* ctx,
                                          uintptr_t addr,
                                          const std::vector<std::string>& keywords,
                                          int depth, int maxDepth,
                                          bool requireButtonLike = false) {
    SearchResult best;
    if (depth > maxDepth || addr == 0) return best;
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return best;

    // 使用 GetTextUtf8 替代原有的 GetText，支持中文客户端
    std::string text = GetTextUtf8(ctx, addr);
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;
    bool hasRect = false;

    if (!text.empty()) {
        hasRect = ctx->Ui.ComputeScreenRect(addr, rx, ry, rw, rh);
        bool validRect = hasRect && rw > 3.f && rh > 3.f;

        if (validRect || !requireButtonLike) {
            int score = CalcKeywordScore(text, keywords);

            // 额外的按钮特征评分（更严格的验证，与StringId搜索一致）
            if (score > 0 && validRect) {
                float area = rw * rh;

                // 按钮通常是中等大小
                if (area >= 500 && area <= 20000) score += 100;
                else if (area >= 100 && area <= 50000) score += 50;
                else score -= 50;

                // 按钮宽度
                if (rw >= 40 && rw <= 200) score += 50;
                else if (rw > 20 && rw <= 400) score += 20;
                else score -= 30;

                // 按钮高度
                if (rh >= 15 && rh <= 50) score += 50;
                else if (rh >= 10 && rh <= 100) score += 20;
                else score -= 30;

                // 检查子节点数（按钮通常是叶子或少子节点）
                auto children = ctx->Ui.GetChildren(addr);
                int childCount = (int)children.size();
                if (childCount == 0) score += 30;
                else if (childCount <= 3) score += 10;
                else score -= 20;

                // 坐标必须在合理范围内
                if (rx < -100.f || ry < -100.f || rx > 10000.f || ry > 10000.f) {
                    score -= 200;
                }
            }

            if (score > 0 && validRect) {
                if (score > best.score) {
                    best.addr = addr;
                    best.score = score;
                    best.text = text;
                    best.x = rx; best.y = ry; best.w = rw; best.h = rh;
                }
            }
        }
    }

    auto children = ctx->Ui.GetChildren(addr);
    for (auto child : children) {
        auto childResult = FindBestUiNodeByText(ctx, child, keywords, depth + 1, maxDepth, requireButtonLike);
        if (childResult.score > best.score) {
            best = childResult;
        }
    }
    return best;
}

// 搜索包含匹配文字的 UI 节点（返回第一个有有效矩形的祖先节点）
// 即使文字在子节点上，也返回父节点的矩形（按钮容器通常有矩形，文字标签在子节点上）
// 修复：UTF-8 安全比较，避免 tolower 破坏中文字符
inline SearchResult FindBestUiNodeContainingText(const PluginSDK::Context* ctx,
                                                  uintptr_t addr,
                                                  const std::vector<std::string>& keywords,
                                                  int depth, int maxDepth) {
    SearchResult best;
    if (depth > maxDepth || addr == 0) return best;
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return best;

    std::string text = ctx->Ui.GetText(addr);
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;
    bool hasRect = ctx->Ui.ComputeScreenRect(addr, rx, ry, rw, rh);
    bool validRect = hasRect && rw > 3.f && rh > 3.f;

    // 计算节点的"按钮特征分数"
    auto CalcButtonBonus = [&](float w, float h, float x, float y) -> int {
        int bonus = 0;
        float area = w * h;

        // 面积评分
        if (area >= 500 && area <= 20000) bonus += 100;
        else if (area >= 100 && area <= 50000) bonus += 50;
        else bonus -= 50;

        // 宽度评分
        if (w >= 40 && w <= 200) bonus += 50;
        else if (w > 20 && w <= 400) bonus += 20;
        else bonus -= 30;

        // 高度评分
        if (h >= 15 && h <= 50) bonus += 50;
        else if (h >= 10 && h <= 100) bonus += 20;
        else bonus -= 30;

        // 坐标必须在合理范围内
        if (x < -100.f || y < -100.f || x > 10000.f || y > 10000.f) {
            bonus -= 200;
        }

        return bonus;
    };

    int selfScore = 0;
    if (!text.empty()) {
        selfScore = CalcKeywordScore(text, keywords);
        if (selfScore > 0 && validRect) {
            int buttonBonus = CalcButtonBonus(rw, rh, rx, ry);
            selfScore += buttonBonus;  // 添加按钮特征分数
            if (selfScore > best.score) {
                best.addr = addr;
                best.score = selfScore;
                best.text = text;
                best.x = rx; best.y = ry; best.w = rw; best.h = rh;
            }
        }
    }

    auto children = ctx->Ui.GetChildren(addr);
    int childMatchScore = 0;
    SearchResult childMatch;
    for (auto child : children) {
        auto childResult = FindBestUiNodeContainingText(ctx, child, keywords, depth + 1, maxDepth);
        if (childResult.score > childMatchScore) {
            childMatchScore = childResult.score;
            childMatch = childResult;
        }
    }

    // 只有当父容器的矩形看起来像按钮时，才选择父容器
    if (childMatchScore > 0 && validRect) {
        int buttonBonus = CalcButtonBonus(rw, rh, rx, ry);
        // 容器奖励：只有当容器也像按钮时才加高分
        int containerScore = childMatchScore + 200 + buttonBonus;  // 降低容器奖励，增加按钮验证
        if (containerScore > best.score) {
            best.addr = addr;
            best.score = containerScore;
            best.text = text + " > " + childMatch.text;
            best.x = rx; best.y = ry; best.w = rw; best.h = rh;
        }
    } else if (childMatchScore > best.score) {
        best = childMatch;
    }

    return best;
}

// 简化版搜索接口（返回地址，向后兼容）
inline uintptr_t FindUiNodeByText(const PluginSDK::Context* ctx,
                                   uintptr_t addr,
                                   const std::vector<std::string>& keywords,
                                   int depth, int maxDepth) {
    auto result = FindBestUiNodeByText(ctx, addr, keywords, depth, maxDepth, true);
    return result.addr;
}

// ============================================================
// StringId 搜索（Fixer SDK 推荐方式，比 GetText 更可靠）
// ============================================================

// 用 StringId 在 UI 树中搜索节点（递归，深度限制）
// Fixer SDK 的 StringId 是引擎内部标识，不受客户端语言影响
inline SearchResult FindBestUiNodeByStringId(const PluginSDK::Context* ctx,
                                               uintptr_t addr,
                                               const std::vector<std::string>& keywords,
                                               int depth, int maxDepth) {
    SearchResult best;
    if (depth > maxDepth || addr == 0) return best;
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return best;

    // 使用 GetStringIdUtf8 替代原有的 GetStringId，支持中文客户端
    std::string stringId = GetStringIdUtf8(ctx, addr);
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;
    bool hasRect = ctx->Ui.ComputeScreenRect(addr, rx, ry, rw, rh);
    bool validRect = hasRect && rw > 3.f && rh > 3.f;

    // 严格验证：StringId 必须非空且节点必须有有效矩形
    if (!stringId.empty() && validRect) {
        int score = CalcKeywordScore(stringId, keywords);

        // 额外的按钮特征评分（更严格的验证）
        if (score > 0) {
            float area = rw * rh;

            // 按钮通常是中等大小（太小可能是文字标签，太大可能是面板）
            // 鉴定按钮通常在 60x25 到 150x35 左右
            if (area >= 500 && area <= 20000) score += 100;  // 合理面积加分
            else if (area >= 100 && area <= 50000) score += 50;  // 较大范围加分
            else score -= 50;  // 不合理面积减分

            // 按钮宽度通常在 40-200 之间
            if (rw >= 40 && rw <= 200) score += 50;
            else if (rw > 20 && rw <= 400) score += 20;
            else score -= 30;

            // 按钮高度通常在 15-50 之间
            if (rh >= 15 && rh <= 50) score += 50;
            else if (rh >= 10 && rh <= 100) score += 20;
            else score -= 30;

            // 检查是否是叶子节点（按钮通常没有子节点或只有很少的装饰性子节点）
            auto children = ctx->Ui.GetChildren(addr);
            int childCount = (int)children.size();
            if (childCount == 0) score += 30;  // 叶子节点加分
            else if (childCount <= 3) score += 10;  // 少子节点加分
            else score -= 20;  // 多子节点可能是容器而非按钮

            // 坐标必须在合理范围内（屏幕坐标）
            if (rx < -100.f || ry < -100.f || rx > 10000.f || ry > 10000.f) {
                score -= 200;  // 坐标异常，大幅减分
            }
        }

        if (score > 0) {
            if (score > best.score) {
                best.addr = addr;
                best.score = score;
                best.text = stringId;
                best.x = rx; best.y = ry; best.w = rw; best.h = rh;
            }
        }
    }

    auto children = ctx->Ui.GetChildren(addr);
    for (auto child : children) {
        auto childResult = FindBestUiNodeByStringId(ctx, child, keywords, depth + 1, maxDepth);
        if (childResult.score > best.score) {
            best = childResult;
        }
    }
    return best;
}

// 收集所有匹配 StringId 的节点（用于调试，输出所有候选）
inline void CollectAllStringIdMatches(const PluginSDK::Context* ctx,
                                        uintptr_t addr,
                                        const std::vector<std::string>& keywords,
                                        int depth, int maxDepth,
                                        std::vector<SearchResult>& results,
                                        int& nodeCount) {
    if (depth > maxDepth || addr == 0) return;
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return;

    nodeCount++;
    std::string stringId = ctx->Ui.GetStringId(addr);
    float rx = 0.f, ry = 0.f, rw = 0.f, rh = 0.f;
    bool hasRect = ctx->Ui.ComputeScreenRect(addr, rx, ry, rw, rh);
    bool validRect = hasRect && rw > 3.f && rh > 3.f;

    if (!stringId.empty() && validRect) {
        int score = CalcKeywordScore(stringId, keywords);
        if (score > 0) {
            SearchResult r;
            r.addr = addr;
            r.score = score;
            r.text = stringId;
            r.x = rx; r.y = ry; r.w = rw; r.h = rh;
            
            // 获取父节点链信息（最多3级）
            auto elem = ctx->Ui.Read(addr);
            char logBuf[1024];
            sprintf_s(logBuf, "[鉴定按钮候选] addr=%llX score=%d text='%s' rect=(%.0f,%.0f,%.0f,%.0f)",
                (unsigned long long)addr, score, stringId.c_str(), rx, ry, rw, rh);
            
            if (elem.Valid && elem.ParentAddress) {
                // 父节点
                auto pElem = ctx->Ui.Read(elem.ParentAddress);
                if (pElem.Valid) {
                    float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;
                    if (ctx->Ui.ComputeScreenRect(elem.ParentAddress, px, py, pw, ph)) {
                        char parentBuf[256];
                        sprintf_s(parentBuf, " parent=[%llX] (%.0f,%.0f,%.0f,%.0f) children=%d",
                            (unsigned long long)elem.ParentAddress, px, py, pw, ph, pElem.ChildCount);
                        strcat_s(logBuf, parentBuf);
                        
                        // 祖父节点
                        if (pElem.ParentAddress) {
                            auto gElem = ctx->Ui.Read(pElem.ParentAddress);
                            if (gElem.Valid) {
                                float gx = 0.f, gy = 0.f, gw = 0.f, gh = 0.f;
                                if (ctx->Ui.ComputeScreenRect(pElem.ParentAddress, gx, gy, gw, gh)) {
                                    char grandBuf[256];
                                    sprintf_s(grandBuf, " grandparent=[%llX] (%.0f,%.0f,%.0f,%.0f) children=%d",
                                        (unsigned long long)pElem.ParentAddress, gx, gy, gw, gh, gElem.ChildCount);
                                    strcat_s(logBuf, grandBuf);
                                }
                            }
                        }
                    }
                }
            }
            
            OutputDebugStringA(logBuf);
            results.push_back(r);
        }
    }

    auto children = ctx->Ui.GetChildren(addr);
    for (auto child : children) {
        CollectAllStringIdMatches(ctx, child, keywords, depth + 1, maxDepth, results, nodeCount);
    }
}

// ============================================================
// 重铸台 UI 操作
// ============================================================

// 解析合成按钮坐标
// 策略（优先级从高到低）：
//   1. useManualCoords=true → 直接用坐标
//   2. 有 combineButtonStringId → 用 FindPanelByStringId
//   3. 文字匹配：搜索 "合成"/"Combine" 等关键词
//   4. 回退到坐标
inline ButtonPos ResolveCombineButton(const PluginSDK::Context* ctx,
                                       const TabletReforgeConfig::CalibData& calib) {
    ButtonPos pos;

    // 模式 1：强制用坐标
    if (calib.useManualCoords) {
        if (calib.combineButtonX >= 0 && calib.combineButtonY >= 0) {
            pos.x = calib.combineButtonX;
            pos.y = calib.combineButtonY;
            pos.valid = true;
        }
        return pos;
    }

    uintptr_t root = ctx ? ctx->Ui.GetGameUiRoot() : 0;
    if (!root) return pos;

    // 模式 2：用 StringId 找 UI 面板（但必须验证节点像按钮，避免错误 StringId 匹配到标签页）
    if (!calib.combineButtonStringId.empty()) {
        uintptr_t panel = ctx->Ui.FindPanelByStringId(root, calib.combineButtonStringId.c_str());
        if (panel) {
            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            if (ctx->Ui.ComputeScreenRect(panel, x, y, w, h) && w > 0.f && h > 0.f) {
                // StringId 节点验证：必须像按钮（不是太长的标题）
                std::string text = ctx->Ui.GetText(panel);
                float area = w * h;
                bool looksLikeButton = (text.size() <= 15) && (area >= 100.f && area <= 20000.f) && (h >= 15.f && h <= 60.f);
                if (looksLikeButton || calib.useManualCoords == false /* 强制用 StringId 时跳过验证？ */ ) {
                    // 仍需要检查：如果 StringId 节点不是按钮（例如 Reforging Benches 是标题），退回模式 3
                    if (text.size() > 20 || text.find(" ") != std::string::npos && text.size() > 10) {
                        // 检测到这可能是标签页标题，不使用此坐标，继续往下模式 3 搜索
                    } else {
                        pos.x = static_cast<int>(x + w * 0.5f);
                        pos.y = static_cast<int>(y + h * 0.5f);
                        pos.valid = true;
                        return pos;
                    }
                }
            }
        }
    }

    // 模式 3：用文字匹配搜索合成/重铸按钮（带评分，优先精确匹配）
    if (ctx) {
        // 只保留精确关键词，移除宽泛词（避免匹配标签页标题）
        std::vector<std::string> combineKeywords = {
            "reforge",              // 精确匹配（最可靠：按钮就是 REFORGE）
            "重铸",                  // 中文按钮
            "combine", "合成"        // 其他可能的按钮文字
        };
        auto best = FindBestUiNodeByText(ctx, root, combineKeywords, 0, 20, true);
        if (best.addr && best.w > 0.f && best.h > 0.f) {
            pos.x = static_cast<int>(best.x + best.w * 0.5f);
            pos.y = static_cast<int>(best.y + best.h * 0.5f);
            pos.valid = true;
            return pos;
        }
    }

    // 模式 4：回退到坐标
    if (calib.combineButtonX >= 0 && calib.combineButtonY >= 0) {
        pos.x = calib.combineButtonX;
        pos.y = calib.combineButtonY;
        pos.valid = true;
    }
    return pos;
}

// 解析产物槽坐标
// 策略（优先级从高到低）：
//   1. useManualCoords=true → 直接用坐标
//   2. 有 outputSlotStringId → 用 FindPanelByStringId
//   3. 文字匹配：搜索 "产物" / "Output" 等关键词
//   4. 回退到坐标
inline ButtonPos ResolveOutputSlot(const PluginSDK::Context* ctx,
                                    const TabletReforgeConfig::CalibData& calib) {
    ButtonPos pos;

    // 模式 1：强制用坐标
    if (calib.useManualCoords) {
        if (calib.outputSlotX >= 0 && calib.outputSlotY >= 0) {
            pos.x = calib.outputSlotX;
            pos.y = calib.outputSlotY;
            pos.valid = true;
        }
        return pos;
    }

    uintptr_t root = ctx ? ctx->Ui.GetGameUiRoot() : 0;
    if (!root) return pos;

    // 模式 2：用 StringId 找 UI 面板
    if (!calib.outputSlotStringId.empty()) {
        uintptr_t panel = ctx->Ui.FindPanelByStringId(root, calib.outputSlotStringId.c_str());
        if (panel) {
            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            if (ctx->Ui.ComputeScreenRect(panel, x, y, w, h) && w > 0.f && h > 0.f) {
                pos.x = static_cast<int>(x + w * 0.5f);
                pos.y = static_cast<int>(y + h * 0.5f);
                pos.valid = true;
                return pos;
            }
        }
    }

    // 模式 3：用文字匹配搜索产物槽（带评分）
    if (ctx) {
        std::vector<std::string> outputKeywords = {
            "output", "产物", "result", "slot"
        };
        auto best = FindBestUiNodeByText(ctx, root, outputKeywords, 0, 20, true);
        if (best.addr && best.w > 0.f && best.h > 0.f) {
            pos.x = static_cast<int>(best.x + best.w * 0.5f);
            pos.y = static_cast<int>(best.y + best.h * 0.5f);
            pos.valid = true;
            return pos;
        }
    }

    // 模式 4：产物槽无文字 → 基于 REFORGE 按钮位置相对推断（产品槽在按钮正上方约 420px）
    // 重铸台面板布局：顶部是标题 → 中间是3原料槽 → 最底部是 REFORGE 按钮
    // 产物槽（输出槽）通常在原料槽正上方，比按钮高约 380-450px
    if (ctx) {
        auto btn = ResolveCombineButton(ctx, calib);
        if (btn.valid) {
            // 计算产物槽中心位置（按钮 X 对齐，Y 向上偏移 415px，经验值）
            // F7 标定产物槽=(596, 485)，按钮 F7=(596, 733)，差值= -248？
            // 但日志里 reforge 按钮是(573,900) → 实际标定产物槽=(596,485)，差值≈-415
            pos.x = btn.x;
            pos.y = btn.y - 415;
            pos.valid = true;
            return pos;
        }
    }

    // 模式 5：回退到坐标
    if (calib.outputSlotX >= 0 && calib.outputSlotY >= 0) {
        pos.x = calib.outputSlotX;
        pos.y = calib.outputSlotY;
        pos.valid = true;
    }
    return pos;
}

// 重铸台合成面板是否打开（启发式 + 多信号综合）
// 综合判断（任一满足即认为打开）：
//   1. 有独立的合成 Inventory（4~24 格的小面板，非装备/非腰带）
//   2. 主背包打开 且 合成按钮可解析（坐标/StringId 有效）
//   3. 仓库面板已关闭（取反：之前是开仓库，现在关了说明游戏切换了面板）
inline bool IsBenchPanelLikelyOpen(const PluginSDK::Context* ctx,
                                    const TabletReforgeConfig::CalibData& calib) {
    if (!ctx) return false;

    // 信号 1：独立合成面板 Inventory 存在（最可靠）
    if (IsBenchInventoryOpen(ctx)) return true;

    // 信号 2：主背包打开 且 合成按钮坐标有效
    bool invOpen = IsInventoryOpen(ctx);
    if (invOpen) {
        auto btn = ResolveCombineButton(ctx, calib);
        if (btn.valid) return true;
    }

    // 信号 3：有 benchPanelStringId 且 StringId 检测通过（在 IsBenchPanelOpen 里做）
    return false;
}

// 精确检测重铸台面板是否打开（通过 StringId 查找 UI 面板）
// 需要在 CalibData 中配置 benchPanelStringId；未配置时退化为启发式。
inline bool IsBenchPanelOpen(const PluginSDK::Context* ctx,
                              const TabletReforgeConfig::CalibData& calib) {
    if (!ctx) return false;

    // 优先：StringId 精确检测（如果配置了）
    if (!calib.benchPanelStringId.empty()) {
        uintptr_t root = ctx->Ui.GetGameUiRoot();
        if (root) {
            uintptr_t panel = ctx->Ui.FindPanelByStringId(root, calib.benchPanelStringId.c_str());
            if (panel) {
                bool vis = ctx->Ui.IsVisible(panel);
                float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
                bool hasRect = ctx->Ui.ComputeScreenRect(panel, x, y, w, h) && w > 0.f && h > 0.f;
                if (vis && hasRect) return true;
            }
        }
    }

    // StringId 未配置 / 没找到 → 用启发式 + 合成面板 Inventory 检测兜底
    return IsBenchPanelLikelyOpen(ctx, calib);
}

// 检测"重铸台是否真正打开"——给状态机用的综合函数
// 同时检查：
//   - 仓库面板已关闭（说明面板切换确实发生了）
//   - 上述任一检测方法返回 true
//   - 合成按钮坐标可解析（至少兜底坐标要有）
inline bool IsBenchPanelTrulyOpen(const PluginSDK::Context* ctx,
                                   const TabletReforgeConfig::CalibData& calib) {
    if (!ctx) return false;

    bool stashStillOpen = IsStashOpen(ctx);
    bool benchDetected  = IsBenchPanelOpen(ctx, calib) || IsBenchContextOpen(ctx);
    auto btn            = ResolveCombineButton(ctx, calib);

    // 最佳：仓库关了 + 检测到合成上下文 + 按钮有效
    if (!stashStillOpen && benchDetected && btn.valid) return true;

    // 次佳：仓库关了 + 检测到合成上下文（按钮可能还没加载出来，但面板已开）
    if (!stashStillOpen && benchDetected) return true;

    // 兜底：检测到合成上下文（用户可能是手动开的重铸台，仓库没开）
    if (benchDetected && btn.valid) return true;

    return false;
}

// ============================================================
// NPC 鉴定辅助（多利亚尼 Doryani）
// ============================================================

// 查找 NPC 实体（按 CalibData.npcEntityPath 匹配）
inline std::optional<BenchLocation> FindNPC(const PluginSDK::Context* ctx,
                                             const TabletReforgeConfig::CalibData& calib) {
    if (calib.npcEntityPath.empty()) return std::nullopt;
    return FindEntityByPath(ctx, StringToWString(calib.npcEntityPath));
}

// 检测 NPC 对话面板是否打开
// 策略：
//   1. 有 npcDialogStringId → 用 StringId 匹配
//   2. 回退：用文字 "多利亚尼"/"Doryani" 或对话面板特征关键词搜索
inline bool IsNpcDialogOpen(const PluginSDK::Context* ctx,
                             const TabletReforgeConfig::CalibData& calib) {
    if (!ctx) return false;

    uintptr_t root = ctx->Ui.GetGameUiRoot();
    if (!root) return false;

    // 模式 1：用 StringId
    if (!calib.npcDialogStringId.empty()) {
        uintptr_t panel = ctx->Ui.FindPanelByStringId(root, calib.npcDialogStringId.c_str());
        if (panel) {
            bool vis = ctx->Ui.IsVisible(panel);
            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            bool hasRect = ctx->Ui.ComputeScreenRect(panel, x, y, w, h) && w > 0.f && h > 0.f;
            if (vis && hasRect) {
                static int lastLogId = 0;
                if (++lastLogId % 30 == 0) {
                    char buf[256];
                    sprintf_s(buf, "[NPC对话] 通过StringId检测到面板 vis=%d rect=(%.0f,%.0f,%.0f,%.0f)",
                        vis ? 1 : 0, x, y, w, h);
                    OutputDebugStringA(buf);
                }
                return true;
            }
        }
    }

    // 模式 2：用文字匹配搜索对话面板
    // NPC 对话面板通常包含 NPC 名字作为标题
    std::vector<std::string> dialogKeywords = {
        "doryani", "多利亚尼", "多里亞尼",
        "dialog", "对话", "對話", "npc"
    };
    uintptr_t panel = FindUiNodeByText(ctx, root, dialogKeywords, 0, 12);
    if (panel) {
        bool vis = ctx->Ui.IsVisible(panel);
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        bool hasRect = ctx->Ui.ComputeScreenRect(panel, x, y, w, h) && w > 0.f && h > 0.f;
        if (vis && hasRect) {
            static int lastLogId2 = 0;
            if (++lastLogId2 % 30 == 0) {
                char buf[256];
                sprintf_s(buf, "[NPC对话] 通过文字匹配检测到面板 addr=%llX vis=%d rect=(%.0f,%.0f,%.0f,%.0f)",
                    (unsigned long long)panel, vis ? 1 : 0, x, y, w, h);
                OutputDebugStringA(buf);
            }
            return true;
        }
    }

    return false;
}

// 统计 UI 树节点总数（用于诊断）
inline int CountUiNodes(const PluginSDK::Context* ctx, uintptr_t addr, int depth, int maxDepth) {
    if (depth > maxDepth || addr == 0) return 0;
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return 0;
    int count = 1;
    auto children = ctx->Ui.GetChildren(addr);
    for (auto child : children) {
        count += CountUiNodes(ctx, child, depth + 1, maxDepth);
    }
    return count;
}

// 解析"鉴定/Identify"按钮坐标（强制使用实时扫描，不再使用固定坐标）
// 策略：
//   1. 有 identifyButtonStringId → 用 FindPanelByStringId 动态查找
//   2. 文字匹配（UTF-8 安全）：搜索 "鑑定" / "Identify" 等关键词
//   3. 失败时转储 UI 树到 debug 日志，便于排查
// 检查矩形坐标是否在有效屏幕范围内（考虑多显示器）
inline bool IsRectValidOnScreen(float x, float y, float w, float h) {
    if (w <= 0.f || h <= 0.f) return false;
    // 获取虚拟桌面范围
    const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsW <= 0 || vsH <= 0) return false;
    
    // 检查矩形是否与屏幕有交集（允许部分超出）
    float right = x + w;
    float bottom = y + h;
    // 至少要有一部分在屏幕内（宽松检查）
    bool xOverlap = (right >= vsX && x < vsX + vsW);
    bool yOverlap = (bottom >= vsY && y < vsY + vsH);
    return xOverlap && yOverlap;
}

// 从有效矩形获取中心点坐标
inline ButtonPos MakeButtonPosFromRect(float x, float y, float w, float h) {
    ButtonPos pos;
    pos.x = static_cast<int>(x + w * 0.5f);
    pos.y = static_cast<int>(y + h * 0.5f);
    pos.valid = IsRectValidOnScreen(x, y, w, h);
    return pos;
}

inline ButtonPos ResolveIdentifyButton(const PluginSDK::Context* ctx,
                                       const TabletReforgeConfig::CalibData& calib) {
    ButtonPos pos;

    uintptr_t root = ctx ? ctx->Ui.GetGameUiRoot() : 0;
    if (!root || !ctx) {
        OutputDebugStringA("[鉴定按钮] GetGameUiRoot 返回空指针\n");
        return pos;
    }

    // 准备关键词列表：硬编码关键词 + 用户填入的 StringId（作为文字兜底）
    // StringId 通常是英文标识符，所以优先使用英文关键词
    std::vector<std::string> identifyKeywords = {
        // 中文关键词（用于 GetText 匹配）
        "鑑定", "鉴定",
        "鑑定物品", "鉴定物品",
        "鑑定全部", "鉴定全部",
        // 英文关键词（用于 StringId 和 GetText 匹配）
        "identify", "identif",
        "Identify Items", "Identify",
        "identify item", "scroll of wisdom",
        "Identify All", "identify all",
        // 可能的 StringId 模式（引擎内部标识）
        "IdentifyButton", "IdentifyItem", "IdentifyItems",
        "IdentifyAll", "IdentifyPanel",
        "Doryani", "doryani",
        "ScrollOfWisdom", "scroll_of_wisdom"
    };
    if (!calib.identifyButtonStringId.empty()) {
        identifyKeywords.push_back(calib.identifyButtonStringId);
    }

    // 调试日志：每次搜索都输出开始信息
    {
        char logBuf[256];
        sprintf_s(logBuf, "[鉴定按钮] 开始搜索: 关键词数=%zu, StringId='%s'",
            identifyKeywords.size(), calib.identifyButtonStringId.c_str());
        OutputDebugStringA(logBuf);
    }

    // 辅助函数：输出节点详细信息
    // 注意：float 在变参函数中会提升为 double，必须用 %f 格式，不能用 %d
    auto LogNodeDetail = [&](const char* mode, const SearchResult& result) {
        char logBuf[1024];
        int childrenCount = 0;
        // 安全获取子节点数（避免无效地址导致崩溃）
        if (result.addr >= 0x10000ull && result.addr <= 0x00007FFFFFFFFFFFull) {
            childrenCount = (int)ctx->Ui.GetChildren(result.addr).size();
        }
        sprintf_s(logBuf, "[鉴定按钮] %s 找到节点: addr=%llX pos=(%.0f,%.0f) size=(%.0f,%.0f) score=%d children=%d text='%s'",
            mode,
            (unsigned long long)result.addr,
            result.x, result.y, result.w, result.h,
            result.score, childrenCount,
            result.text.c_str());
        OutputDebugStringA(logBuf);
    };

    // === 策略 B（优先）：全局搜索 ===
    // 先尝试全局搜索，因为 NPC 面板的子节点可能不可达
    
    // 辅助函数：当找到的节点是文字标签时，计算正确的点击坐标
    // 策略：
    // 1. 如果节点本身就是按钮（尺寸够大），直接使用节点中心
    // 2. 如果是文字标签，估算实际按钮尺寸并点击中心
    auto ComputeClickPoint = [&](const SearchResult& result, float& outX, float& outY, float& outW, float& outH) -> bool {
        if (!result.addr) return false;
        
        // 读取节点信息
        auto elem = ctx->Ui.Read(result.addr);
        if (!elem.Valid || !elem.IsVisible) {
            char logBuf[256];
            sprintf_s(logBuf, "[鉴定按钮] 节点不可见或无效: valid=%d visible=%d",
                elem.Valid ? 1 : 0, elem.IsVisible ? 1 : 0);
            OutputDebugStringA(logBuf);
            return false;
        }
        
        // 如果节点本身就够大（像按钮），直接使用节点中心
        if (result.h >= 30.f && result.w >= 60.f) {
            outX = result.x; outY = result.y; outW = result.w; outH = result.h;
            char logBuf[256];
            sprintf_s(logBuf, "[鉴定按钮] 节点本身像按钮: (%.0f,%.0f,%.0f,%.0f)", result.x, result.y, result.w, result.h);
            OutputDebugStringA(logBuf);
            return true;
        }
        
        // 文字标签：查找按钮容器，并估算实际按钮尺寸
        uintptr_t currentAddr = elem.ParentAddress;
        float labelCenterX = result.x + result.w * 0.5f;
        float labelCenterY = result.y + result.h * 0.5f;
        
        // 先沿父链查找菜单容器（用于估算按钮尺寸）
        uintptr_t menuContainerAddr = 0;
        float menuX = 0.f, menuY = 0.f, menuW = 0.f, menuH = 0.f;
        int menuChildren = 0;
        
        {
            uintptr_t searchAddr = elem.ParentAddress;
            for (int i = 1; i <= 4 && searchAddr; ++i) {
                auto e = ctx->Ui.Read(searchAddr);
                if (!e.Valid) break;
                float mx = 0.f, my = 0.f, mw = 0.f, mh = 0.f;
                if (ctx->Ui.ComputeScreenRect(searchAddr, mx, my, mw, mh) && mw > 100.f && mh > 50.f) {
                    if (e.ChildCount >= 2 && e.ChildCount <= 8) {
                        // 找到可能的菜单容器
                        menuContainerAddr = searchAddr;
                        menuX = mx; menuY = my; menuW = mw; menuH = mh;
                        menuChildren = e.ChildCount;
                        char logBuf[256];
                        sprintf_s(logBuf, "[鉴定按钮] 找到菜单容器: level=%d rect=(%.0f,%.0f,%.0f,%.0f) children=%d",
                            i, mx, my, mw, mh, e.ChildCount);
                        OutputDebugStringA(logBuf);
                        break;
                    }
                }
                searchAddr = e.ParentAddress;
            }
        }
        
        // 计算估算的按钮尺寸
        float estimatedBtnW = result.w;
        float estimatedBtnH = result.h;
        float clickCenterX = labelCenterX;
        float clickCenterY = labelCenterY;
        
        if (menuContainerAddr && menuChildren > 0) {
            // 基于菜单容器估算每个按钮的尺寸
            float itemHeight = menuH / menuChildren;
            float itemWidth = menuW;  // 每个菜单项通常占满菜单宽度
            
            // 按钮通常比文字标签大，添加一些内边距
            estimatedBtnW = result.w + 30.f;  // 文字宽度 + 30px 内边距
            estimatedBtnH = itemHeight;  // 使用菜单的每项高度
            
            // 确保尺寸合理
            if (estimatedBtnH < result.h * 1.3f) estimatedBtnH = result.h * 1.8f;
            if (estimatedBtnW < result.w * 1.1f) estimatedBtnW = result.w * 1.2f;
            
            // 计算点击位置：按钮中心
            // 按钮中心 Y = 菜单顶部 + 菜单项高度的一半 + 在第几项的偏移
            // 由于文字标签的 Y 位置已知，可以估算按钮的中心 Y
            float relativeY = result.y - menuY;  // 文字相对于菜单顶部的偏移
            float itemIndex = relativeY / itemHeight;  // 文字在第几项
            clickCenterY = menuY + (itemIndex + 0.5f) * itemHeight;
            
            // 按钮中心 X：使用菜单宽度中心或文字标签中心
            clickCenterX = result.x + estimatedBtnW * 0.5f;
            
            char logBuf[512];
            sprintf_s(logBuf, "[鉴定按钮] 估算按钮: label=(%.0f,%.0f,%.0f,%.0f) menu=(%.0f,%.0f,%.0f,%.0f) items=%d estimatedBtn=(%.0f,%.0f,%.0f,%.0f) click=(%.0f,%.0f)",
                result.x, result.y, result.w, result.h,
                menuX, menuY, menuW, menuH, menuChildren,
                result.x, result.y, estimatedBtnW, estimatedBtnH,
                clickCenterX, clickCenterY);
            OutputDebugStringA(logBuf);
        } else {
            // 没找到菜单容器，使用文字标签并添加估算的内边距
            estimatedBtnW = result.w + 20.f;
            estimatedBtnH = result.h * 1.8f;
            clickCenterX = labelCenterX;
            clickCenterY = result.y + estimatedBtnH * 0.5f;  // 按钮中心略低于文字中心
            
            char logBuf[256];
            sprintf_s(logBuf, "[鉴定按钮] 估算按钮(无菜单): estimatedBtn=(%.0f,%.0f,%.0f,%.0f) click=(%.0f,%.0f)",
                result.x, result.y, estimatedBtnW, estimatedBtnH,
                clickCenterX, clickCenterY);
            OutputDebugStringA(logBuf);
        }
        
        // 使用估算的按钮中心作为点击坐标
        // 注意：PoE的菜单按钮点击区域通常在文字下方，添加向下偏移
        const float Y_OFFSET = 15.0f;  // 向下偏移15像素，使点击位置更准确
        outX = clickCenterX - estimatedBtnW * 0.5f;
        outY = clickCenterY - estimatedBtnH * 0.5f + Y_OFFSET;  // 添加向下偏移
        outW = estimatedBtnW;
        outH = estimatedBtnH;
        
        char logBuf[256];
        sprintf_s(logBuf, "[鉴定按钮] 最终点击位置: center=(%.0f,%.0f) rect=(%.0f,%.0f,%.0f,%.0f) yOffset=%.0f",
            clickCenterX, clickCenterY + Y_OFFSET, outX, outY, outW, outH, Y_OFFSET);
        OutputDebugStringA(logBuf);
        return true;
    };

    // === 优化：优先使用精确查找，减少全局遍历 ===
    // 如果用户配置了 StringId，先尝试精确查找（最快）
    if (!calib.identifyButtonStringId.empty()) {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        uintptr_t panel = ctx->Ui.FindPanelByStringId(root, calib.identifyButtonStringId.c_str());
        if (panel && ctx->Ui.ComputeScreenRect(panel, x, y, w, h) && w > 0.f && h > 0.f) {
            pos = MakeButtonPosFromRect(x, y, w, h);
            if (pos.valid) {
                char logBuf[256];
                sprintf_s(logBuf, "[鉴定按钮] 模式B1(StringId精确) 成功: (%d,%d) rect=(%.0f,%.0f,%.0f,%.0f)",
                    pos.x, pos.y, x, y, w, h);
                OutputDebugStringA(logBuf);
                return pos;
            }
        }
    }

    // 模式 B0：StringId 模糊搜索（仅在精确查找失败时执行）
    // StringId 是引擎内部标识，不受客户端语言影响
    {
        auto bestStringId = FindBestUiNodeByStringId(ctx, root, identifyKeywords, 0, 15);
        // 严格验证：addr 非空、text 非空（StringId 必须有值）、矩形有效
        if (bestStringId.addr && !bestStringId.text.empty() &&
            bestStringId.w > 0.f && bestStringId.h > 0.f &&
            bestStringId.score > 0) {
            
            LogNodeDetail("模式B0(StringId模糊)", bestStringId);
            
            // 计算正确的点击坐标
            float btnX = 0.f, btnY = 0.f, btnW = 0.f, btnH = 0.f;
            bool hasClickPoint = ComputeClickPoint(bestStringId, btnX, btnY, btnW, btnH);
            
            if (hasClickPoint) {
                pos = MakeButtonPosFromRect(btnX, btnY, btnW, btnH);
                if (pos.valid) {
                    char logBuf[256];
                    sprintf_s(logBuf, "[鉴定按钮] 模式B0 有效坐标: (%d,%d) rect=(%.0f,%.0f,%.0f,%.0f)",
                        pos.x, pos.y, btnX, btnY, btnW, btnH);
                    OutputDebugStringA(logBuf);
                    return pos;
                } else {
                    OutputDebugStringA("[鉴定按钮] 模式B0 坐标无效，继续搜索...\n");
                }
            } else {
                // 回退到原始节点坐标
                pos = MakeButtonPosFromRect(bestStringId.x, bestStringId.y, bestStringId.w, bestStringId.h);
                if (pos.valid) {
                    char logBuf[256];
                    sprintf_s(logBuf, "[鉴定按钮] 模式B0 有效坐标(回退): (%d,%d)", pos.x, pos.y);
                    OutputDebugStringA(logBuf);
                    return pos;
                }
            }
        }
    }

    // 模式 B2：包含式文字匹配（UTF-8 安全）
    auto best = FindBestUiNodeContainingText(ctx, root, identifyKeywords, 0, 20);
    if (best.addr && best.w > 0.f && best.h > 0.f) {
        pos = MakeButtonPosFromRect(best.x, best.y, best.w, best.h);
        LogNodeDetail("模式B2(包含式匹配)", best);
        if (pos.valid) {
            return pos;
        }
    }

    // 模式 B3：严格文字匹配
    auto bestStrict = FindBestUiNodeByText(ctx, root, identifyKeywords, 0, 20, true);
    if (bestStrict.addr && bestStrict.w > 0.f && bestStrict.h > 0.f) {
        pos = MakeButtonPosFromRect(bestStrict.x, bestStrict.y, bestStrict.w, bestStrict.h);
        LogNodeDetail("模式B3(严格匹配)", bestStrict);
        if (pos.valid) {
            return pos;
        }
    }

    // 模式 B4：宽松文字匹配（接受无矩形的文字节点，再尝试获取矩形）
    auto bestLoose = FindBestUiNodeByText(ctx, root, identifyKeywords, 0, 20, false);
    if (bestLoose.addr) {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if (ctx->Ui.ComputeScreenRect(bestLoose.addr, x, y, w, h) && w > 0.f && h > 0.f) {
            pos = MakeButtonPosFromRect(x, y, w, h);
            LogNodeDetail("模式B4(宽松匹配)", bestLoose);
            if (pos.valid) {
                return pos;
            }
        }
    }

    // === 策略 A（回退）：在 NPC 对话面板子树内搜索 ===
    // 先找 NPC 对话面板（与 IsNpcDialogOpen 相同逻辑），然后在其子树内搜索鉴定按钮
    std::vector<std::string> npcKeywords = {
        "doryani", "多利亚尼", "多里亞尼",
        "dialog", "对话", "對話", "npc"
    };
    uintptr_t npcPanel = FindUiNodeByText(ctx, root, npcKeywords, 0, 12);
    
    if (npcPanel) {
        float npcX = 0.f, npcY = 0.f, npcW = 0.f, npcH = 0.f;
        bool npcHasRect = ctx->Ui.ComputeScreenRect(npcPanel, npcX, npcY, npcW, npcH);
        int npcChildren = (int)ctx->Ui.GetChildren(npcPanel).size();
        
        char logBuf[256];
        sprintf_s(logBuf, "[鉴定按钮] NPC面板: addr=%llX rect=(%.0f,%.0f,%.0f,%.0f) children=%d",
            (unsigned long long)npcPanel, npcX, npcY, npcW, npcH, npcChildren);
        OutputDebugStringA(logBuf);

        // 只有当 NPC 面板有有效矩形时才在其子树内搜索
        if (npcHasRect && npcW > 0.f && npcH > 0.f) {
            // 模式 A1：在 NPC 面板子树内用 StringId 查找
            if (!calib.identifyButtonStringId.empty()) {
                uintptr_t panel = ctx->Ui.FindPanelByStringId(npcPanel, calib.identifyButtonStringId.c_str());
                if (panel && ctx->Ui.ComputeScreenRect(panel, npcX, npcY, npcW, npcH) && npcW > 0.f && npcH > 0.f) {
                    pos = MakeButtonPosFromRect(npcX, npcY, npcW, npcH);
                    if (pos.valid) return pos;
                }
            }

            // 模式 A2：在 NPC 面板子树内做包含式文字匹配
            auto bestInNpc = FindBestUiNodeContainingText(ctx, npcPanel, identifyKeywords, 0, 15);
            if (bestInNpc.addr && bestInNpc.w > 0.f && bestInNpc.h > 0.f) {
                pos = MakeButtonPosFromRect(bestInNpc.x, bestInNpc.y, bestInNpc.w, bestInNpc.h);
                if (pos.valid) return pos;
            }

            // 模式 A3：在 NPC 面板子树内做严格文字匹配
            auto bestStrictInNpc = FindBestUiNodeByText(ctx, npcPanel, identifyKeywords, 0, 15, true);
            if (bestStrictInNpc.addr && bestStrictInNpc.w > 0.f && bestStrictInNpc.h > 0.f) {
                pos = MakeButtonPosFromRect(bestStrictInNpc.x, bestStrictInNpc.y, bestStrictInNpc.w, bestStrictInNpc.h);
                if (pos.valid) return pos;
            }

            // 模式 A4：在 NPC 面板子树内做宽松文字匹配
            auto bestLooseInNpc = FindBestUiNodeByText(ctx, npcPanel, identifyKeywords, 0, 15, false);
            if (bestLooseInNpc.addr) {
                float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
                if (ctx->Ui.ComputeScreenRect(bestLooseInNpc.addr, x, y, w, h) && w > 0.f && h > 0.f) {
                    pos = MakeButtonPosFromRect(x, y, w, h);
                    if (pos.valid) return pos;
                }
            }
        }
    }

    // === 全部动态搜索失败：转储诊断信息 ===
    {
        int totalNodes = CountUiNodes(ctx, root, 0, 20);
        std::string dumpLog;
        dumpLog += "=== UI 树转储（鉴定按钮搜索失败，节点总数=" + std::to_string(totalNodes) + "） ===\n";
        dumpLog += "搜索关键词: ";
        for (const auto& kw : identifyKeywords) {
            dumpLog += "[" + kw + "] ";
        }
        dumpLog += "\n";

        // 如果找到了 NPC 面板，转储 NPC 子树
        if (npcPanel) {
            dumpLog += "\n--- NPC 对话面板子树 ---\n";
            int npcCount = 0;
            DumpUiTreeForDebug(ctx, npcPanel, identifyKeywords, 0, 15, dumpLog, npcCount);
            dumpLog += "--- NPC 子树结束（" + std::to_string(npcCount) + " 个节点）---\n\n";
        } else {
            dumpLog += "\n[未找到 NPC 对话面板，转储全树]\n\n";
        }

        // 全树转储（限制节点数避免日志过大）
        int dumpCount = 0;
        DumpUiTreeForDebug(ctx, root, identifyKeywords, 0, 20, dumpLog, dumpCount);
        dumpLog += "=== UI 树转储结束（共 " + std::to_string(dumpCount) + " 个节点） ===\n";
        DebugLogW(dumpLog);
    }

    return pos;
}

// 检测背包是否还存在需要鉴定的未鉴定物品（用于等待鉴定完成）
// 如果返回 false 表示"需要鉴定的物品"都已鉴定完成
inline bool HasUnidentifiedNeedingIdentify(const PluginSDK::Context* ctx,
                                             const TabletReforgeConfig::Settings& settings) {
    auto items = FindUnidentifiedItemsInBag(ctx, settings);
    for (const auto& item : items) {
        if (item.needsIdentify) return true;
    }
    return false;
}

} // namespace TabletReforgeGame
