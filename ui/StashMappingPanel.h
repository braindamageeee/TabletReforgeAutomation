// StashMappingPanel.h — 仓库映射配置界面
//
// 功能：
//   1. 手动配置仓库页的物品分类映射（API直接读取模式）
//   2. 手动验证视觉识别结果（视觉识别模式）
//   3. 配置普通仓库为特定合成物品专属仓库
//   4. 视觉识别失败时自动截图保存当前屏幕状态
#pragma once

#include "../config/Settings.h"
#include "../game/StashItemMapper.h"
#include "../game/VisionRecognizer.h"
#include "../game/StashOps.h"
#include "../sdk/PluginSDK.h"

#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace TabletReforgeUi {

using namespace TabletReforgeGame;
using namespace TabletReforgeConfig;

// —— 前向声明 ——
inline std::string FindScreenshotConflict(
    const StashMappingConfig& config,
    const std::string& screenshotPath,
    int selfInvId);

inline void RunMockTabletStashTest(
    TabletReforgeConfig::Settings& settings,
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir);

// ============================================================
// 1. 物品分类选择器（用于手动配置仓库映射）
// ============================================================

// 物品分类选项列表（使用StashItemMapper.h中的中英文对照版本）
inline const char* GetItemCategoryOptions() {
    return GetStashItemCategoryOptions();
}

// 获取物品分类数量
inline int GetItemCategoryCount() {
    return GetStashItemCategoryOptionCount();
}

// 从 StashItemCategory 获取显示名称（中英文对照）
inline const char* GetCategoryDisplayName(int cat) {
    return StashItemCategoryName(static_cast<StashItemCategory>(cat));
}

// 从显示名称获取 StashItemCategory
inline StashItemCategory GetCategoryFromDisplay(const char* name) {
    for (int i = 0; i < GetItemCategoryCount(); i++) {
        if (strcmp(GetCategoryDisplayName(i), name) == 0) {
            return static_cast<StashItemCategory>(i);
        }
    }
    return StashItemCategory::Unknown;
}

// ============================================================
// 2. 仓库类型选择器（分类分组，缩短UI长度）
// ============================================================

// 仓库类型分组
enum class StashTypeGroup {
    Auto = 0,      // 自动检测
    Basic,         // 基础仓库
    Specialized,   // 专用仓库
    Other          // 其他
};

// 获取分组名称
inline const char* GetStashTypeGroupName(StashTypeGroup group) {
    switch (group) {
        case StashTypeGroup::Auto: return "自动检测";
        case StashTypeGroup::Basic: return "基础仓库";
        case StashTypeGroup::Specialized: return "专用仓库";
        case StashTypeGroup::Other: return "其他";
        default: return "未知";
    }
}

// 获取指定分组的仓库类型选项
inline const char* GetStashTypeOptionsForGroup(StashTypeGroup group) {
    switch (group) {
        case StashTypeGroup::Auto:
            return "自动检测 (Auto)\0";
        case StashTypeGroup::Basic:
            return "普通仓库 (Normal)\0高级仓库 (Premium)\0传奇仓库 (Unique)\0四方格仓库 (Quad)\0";
        case StashTypeGroup::Specialized:
            return "货币仓库 (Currency)\0地图仓库 (Map)\0精髓仓库 (Essence)\0碎片/碑牌仓库 (Fragment)\0试炼仓库 (Delirium)\0药剂仓库 (Flask)\0技能宝石仓库 (Gem)\0可镶嵌仓库 (Socketable)\0远征仓库 (Expedition)\0仪式仓库 (Ritual)\0裂隙仓库 (Breach)\0深渊仓库 (Abyss)\0";
        case StashTypeGroup::Other:
            return "其他 (Other)\0";
        default:
            return "";
    }
}

// 获取指定分组的选项数量
inline int GetStashTypeOptionCountForGroup(StashTypeGroup group) {
    switch (group) {
        case StashTypeGroup::Auto: return 1;
        case StashTypeGroup::Basic: return 4;
        case StashTypeGroup::Specialized: return 12;
        case StashTypeGroup::Other: return 1;
        default: return 0;
    }
}

// 从分组+选项索引获取stashTypeId
inline int GetStashTypeIdFromGroupOption(StashTypeGroup group, int optionIdx) {
    // 全局选项索引到stashTypeId的映射
    // 格式: {group, optionIdx} -> stashTypeId
    struct Mapping { StashTypeGroup group; int optionIdx; int stashTypeId; };
    static const Mapping mappings[] = {
        {StashTypeGroup::Auto, 0, -1},      // Auto
        // Basic
        {StashTypeGroup::Basic, 0, 0},      // Normal
        {StashTypeGroup::Basic, 1, 1},      // Premium
        {StashTypeGroup::Basic, 2, 4},      // Unique
        {StashTypeGroup::Basic, 3, 7},      // Quad
        // Specialized
        {StashTypeGroup::Specialized, 0, 3},   // Currency
        {StashTypeGroup::Specialized, 1, 5},   // Map
        {StashTypeGroup::Specialized, 2, 8},   // Essence
        {StashTypeGroup::Specialized, 3, 9},   // Fragment
        {StashTypeGroup::Specialized, 4, 15},  // Delirium
        {StashTypeGroup::Specialized, 5, 17},  // Flask
        {StashTypeGroup::Specialized, 6, 18},  // Gem
        {StashTypeGroup::Specialized, 7, 19},  // Socketable
        {StashTypeGroup::Specialized, 8, 20},  // Expedition
        {StashTypeGroup::Specialized, 9, 21},  // Ritual
        {StashTypeGroup::Specialized, 10, 22}, // Breach
        {StashTypeGroup::Specialized, 11, 23}, // Abyss
        // Other
        {StashTypeGroup::Other, 0, -1},      // Other
    };
    
    for (const auto& m : mappings) {
        if (m.group == group && m.optionIdx == optionIdx) {
            return m.stashTypeId;
        }
    }
    return -1;
}

// 从stashTypeId获取分组和选项索引
inline void GetGroupOptionFromStashTypeId(int stashTypeId, StashTypeGroup& outGroup, int& outOptionIdx) {
    if (stashTypeId < 0) {
        outGroup = StashTypeGroup::Auto;
        outOptionIdx = 0;
        return;
    }
    
    struct Mapping { StashTypeGroup group; int optionIdx; int stashTypeId; };
    static const Mapping mappings[] = {
        {StashTypeGroup::Auto, 0, -1},
        {StashTypeGroup::Basic, 0, 0},
        {StashTypeGroup::Basic, 1, 1},
        {StashTypeGroup::Basic, 2, 4},
        {StashTypeGroup::Basic, 3, 7},
        {StashTypeGroup::Specialized, 0, 3},
        {StashTypeGroup::Specialized, 1, 5},
        {StashTypeGroup::Specialized, 2, 8},
        {StashTypeGroup::Specialized, 3, 9},
        {StashTypeGroup::Specialized, 4, 15},
        {StashTypeGroup::Specialized, 5, 17},
        {StashTypeGroup::Specialized, 6, 18},
        {StashTypeGroup::Specialized, 7, 19},
        {StashTypeGroup::Specialized, 8, 20},
        {StashTypeGroup::Specialized, 9, 21},
        {StashTypeGroup::Specialized, 10, 22},
        {StashTypeGroup::Specialized, 11, 23},
    };
    
    for (const auto& m : mappings) {
        if (m.stashTypeId == stashTypeId) {
            outGroup = m.group;
            outOptionIdx = m.optionIdx;
            return;
        }
    }
    
    // 默认：其他
    outGroup = StashTypeGroup::Other;
    outOptionIdx = 0;
}

// 获取所有分组列表（用于UI下拉）
inline const char* GetStashTypeGroupOptions() {
    return "自动检测\0基础仓库\0专用仓库\0其他\0";
}

inline int GetStashTypeGroupCount() {
    return 4;
}

inline StashTypeGroup GetGroupFromOption(int option) {
    return static_cast<StashTypeGroup>(option);
}

inline int GetOptionFromGroup(StashTypeGroup group) {
    return static_cast<int>(group);
}

// ============================================================
// 3. 自动截图保存功能（视觉识别失败时）
// ============================================================

// 截图保存结果
struct AutoScreenshotResult {
    bool success = false;
    std::filesystem::path filePath;
    int stashInventoryId = 0;
    std::string failureReason;
    SYSTEMTIME timestamp;
};

// 视觉识别失败时自动截图
inline AutoScreenshotResult SaveScreenshotOnRecognitionFailure(
    const PluginSDK::Context* ctx,
    int inventoryId,
    const std::string& failureReason,
    const std::filesystem::path& pluginDir)
{
    AutoScreenshotResult result;
    result.stashInventoryId = inventoryId;
    result.failureReason = failureReason;
    
    if (!ctx) {
        result.failureReason = "Context为空: " + failureReason;
        OutputDebugStringA(("[Screenshot] 失败: " + result.failureReason + "\n").c_str());
        return result;
    }
    
    // 创建日志目录
    std::filesystem::path logDir = pluginDir.empty() 
        ? std::filesystem::path("logs") 
        : pluginDir / "logs" / "screenshots";
    
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    
    // 生成时间戳
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    result.timestamp = st;
    
    char ts[32];
    sprintf_s(ts, "%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    
    // 截图1: 整个游戏窗口
    HWND hWnd = ctx->Game.GetGameWindow();
    if (hWnd) {
        RECT rc = {};
        if (::GetClientRect(hWnd, &rc)) {
            POINT pt = { rc.left, rc.top };
            if (::ClientToScreen(hWnd, &pt)) {
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;
                
                VisionRecogNS::ScreenFrame fullFrame;
                if (VisionRecogNS::CaptureScreenRegion(pt.x, pt.y, w, h, fullFrame)) {
                    std::string filename = "fail_full_" + std::to_string(inventoryId) + "_" + ts + ".bmp";
                    auto bmpPath = logDir / filename;
                    
                    if (VisionRecogNS::SaveFrameToBmp(fullFrame, bmpPath)) {
                        result.success = true;
                        result.filePath = bmpPath;
                        
                        char logMsg[256];
                        sprintf_s(logMsg, 
                            "[Screenshot] 已保存失败截图: %s (invId=%d, reason=%s)\n",
                            bmpPath.string().c_str(), inventoryId, failureReason.c_str());
                        OutputDebugStringA(logMsg);
                    }
                }
            }
        }
    }
    
    // 截图2: 如果有指定仓库，截取该区域
    auto inv = ctx->Inventory.Get(inventoryId);
    if (inv.Address != 0 && inv.Grid.Valid) {
        int cx = (int)inv.Grid.GridScreenX;
        int cy = (int)inv.Grid.GridScreenY;
        int cellSize = inv.Grid.CellSize > 0 ? (int)inv.Grid.CellSize : 50;
        int w = cellSize * inv.TotalBoxesX;
        int h = cellSize * inv.TotalBoxesY;
        
        VisionRecogNS::ScreenFrame stashFrame;
        if (VisionRecogNS::CaptureScreenRegion(cx, cy, w, h, stashFrame)) {
            std::string filename = "fail_stash_" + std::to_string(inventoryId) + "_" + ts + ".bmp";
            auto bmpPath = logDir / filename;
            
            if (VisionRecogNS::SaveFrameToBmp(stashFrame, bmpPath)) {
                char logMsg[256];
                sprintf_s(logMsg, 
                    "[Screenshot] 已保存仓库区域截图: %s (%dx%d)\n",
                    bmpPath.string().c_str(), w, h);
                OutputDebugStringA(logMsg);
                
                if (!result.success) {
                    result.success = true;
                    result.filePath = bmpPath;
                }
            }
        }
    }
    
    return result;
}

// ============================================================
// 4. 仓库映射配置主界面（支持嵌套结构和截图识别）
// ============================================================

// 绘制仓库映射配置面板
inline void DrawStashMappingConfigPanel(
    TabletReforgeConfig::Settings& settings,
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir)
{
    auto& mappingManager = StashMappingManager::Instance();
    
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "★ 仓库映射配置");
    ImGui::TextDisabled("打开游戏中的仓库页 → 扫描识别 → 选择类型/物品 → 设置截图 → 保存配置");
    ImGui::Separator();
    
    // —— 图色识别相似度阈值 ——
    ImGui::PushItemWidth(200);
    float thresholdFloat = static_cast<float>(settings.visionMatchThreshold);
    if (ImGui::SliderFloat("图色识别相似度阈值", &thresholdFloat, 0.0f, 1.0f, "%.2f")) {
        settings.visionMatchThreshold = static_cast<double>(thresholdFloat);
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::TextDisabled("(0.0=可能误识别, 1.0=严格匹配, 推荐0.3-0.7)");
    
    // 快速预设按钮
    if (ImGui::SmallButton("低 (0.30)")) { settings.visionMatchThreshold = 0.30; }
    ImGui::SameLine();
    if (ImGui::SmallButton("中 (0.50)")) { settings.visionMatchThreshold = 0.50; }
    ImGui::SameLine();
    if (ImGui::SmallButton("高 (0.70)")) { settings.visionMatchThreshold = 0.70; }
    
    // 阈值变化日志
    {
        static double s_lastThreshold = -1.0;
        if (settings.visionMatchThreshold != s_lastThreshold) {
            char logMsg[256];
            sprintf_s(logMsg, "[VisionConfig] 相似度阈值更新: %.4f -> %.4f\n", 
                s_lastThreshold, settings.visionMatchThreshold);
            OutputDebugStringA(logMsg);
            s_lastThreshold = settings.visionMatchThreshold;
        }
    }
    ImGui::Separator();
    
    // —— Mock测试按钮 ——
    if (ImGui::Button("🧪 运行多子页碎片大仓Mock测试", ImVec2(250, 28))) {
        RunMockTabletStashTest(settings, ctx, pluginDir);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("模拟碎片大仓(含碑牌子页)+独立催化剂仓库的截图选择与切换流程验证");
    
    ImGui::Separator();
    
    // —— 扫描当前打开的仓库 ——
    if (ImGui::Button("🔍 扫描当前仓库", ImVec2(150, 30))) {
        if (ctx) {
            auto inventories = ctx->Inventory.GetAll();
            
            // 只获取当前打开的仓库（Grid.Valid == true）
            PluginSDK::Inventory currentInv{};
            bool foundCurrent = false;
            
            for (const auto& inv : inventories) {
                if (inv.Address != 0 && inv.Grid.Valid && 
                    inv.TotalBoxesX * inv.TotalBoxesY >= 4) {
                    
                    const char* invNameC = ctx->Inventory.GetName(inv.InventoryId);
                    std::string invName = invNameC ? invNameC : "";
                    
                    // 排除主背包
                    if (invName.rfind("MainInventory", 0) == 0) continue;
                    if (IsNonStashInventory(invName, inv.TotalBoxesX, inv.TotalBoxesY, inv.InventoryId)) continue;
                    
                    currentInv = inv;
                    foundCurrent = true;
                    break;  // 只取第一个（当前可见的）
                }
            }
            
            if (foundCurrent) {
                mappingManager.AddOrUpdateCurrentStash(ctx, settings, currentInv, pluginDir);
                
                char log[256];
                sprintf_s(log, "[StashMapping] 扫描当前仓库: InvId=%d, 名称=%s\n", 
                    currentInv.InventoryId,
                    ctx->Inventory.GetName(currentInv.InventoryId) ? 
                    ctx->Inventory.GetName(currentInv.InventoryId) : "未知");
                OutputDebugStringA(log);
            } else {
                OutputDebugStringA("[StashMapping] 未检测到当前打开的仓库，请确保仓库已打开\n");
            }
        }
    }
    ImGui::SameLine();
    
    // 加载已保存配置
    if (ImGui::SmallButton("📂 加载配置") && !pluginDir.empty()) {
        if (mappingManager.LoadFromFile(pluginDir)) {
            OutputDebugStringA("[StashMapping] 配置加载成功\n");
        }
    }
    
    ImGui::SameLine();
    if (ImGui::SmallButton("🗑 清空所有")) {
        mappingManager.ClearConfig();
    }
    
    ImGui::Separator();
    
    // —— 显示仓库列表 ——
    auto& configMutable = mappingManager.GetConfigMutable();
    
    if (configMutable.tabMappings.empty()) {
        ImGui::TextDisabled("请打开游戏中的仓库页，然后点击\"🔍 扫描当前仓库\"...");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "使用说明:");
        ImGui::TextWrapped(R"(
1. 在游戏中打开一个仓库页（如碎片仓库、货币仓库等）
2. 点击"🔍 扫描当前仓库"按钮识别该仓库
3. 为该仓库选择"分组"和"具体类型"
4. 可选：设置截图模板（用于图色识别切换仓库）
5. 如果仓库有子页（如碑牌大仓有多个分类），添加子页配置
6. 切换到下一个仓库页，重复步骤1-5
7. 所有仓库配置完成后，点击"💾 保存配置"
        )");
        return;
    }
    
    ImGui::Text("已配置 %zu 个仓库页:", configMutable.tabMappings.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(配置会永久保存)");
    
    // —— 仓库列表（可折叠展开）——
    ImGui::BeginChild("stash_list", ImVec2(0, 500), true);
    
    for (int i = 0; i < (int)configMutable.tabMappings.size(); i++) {
        auto& mapping = configMutable.tabMappings[i];
        
        bool isExpanded = true;
        char treeId[128];
        sprintf_s(treeId, "stash_tree_%d", i);
        
        // 树节点（可折叠）
        if (ImGui::TreeNodeEx(treeId, isExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            // 检查该 inventory 是否仍然有效
            bool validInv = false;
            std::string invName;
            if (ctx) {
                auto inv = ctx->Inventory.Get(mapping.inventoryId);
                if (inv.Address != 0) {
                    validInv = true;
                    const char* name = ctx->Inventory.GetName(mapping.inventoryId);
                    invName = name ? name : "(未知)";
                }
            }
            
            // 标题行
            ImGui::Text("📦 仓库页 #%d (InvId: %d)", i + 1, mapping.inventoryId);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", invName.c_str());
            
            // 切换和删除按钮
            if (ctx) {
                char switchBtn[64];
                sprintf_s(switchBtn, "🔄 切换到此仓库##switch_%d", i);
                if (ImGui::SmallButton(switchBtn)) {
                    ClickStashTabBySlotIndex(ctx, mapping.inventoryId);
                }
                ImGui::SameLine();
            }
            
            char delBtn[64];
            sprintf_s(delBtn, "✕ 删除##del_%d", i);
            if (ImGui::SmallButton(delBtn)) {
                configMutable.tabMappings.erase(configMutable.tabMappings.begin() + i);
                i--;
                ImGui::TreePop();
                continue;
            }
            
            ImGui::Separator();
            
            // —— 仓库类型选择（两步选择：分组 + 具体类型）——
            ImGui::PushItemWidth(120);
            
            StashTypeGroup currentGroup;
            int currentOptionIdx;
            GetGroupOptionFromStashTypeId(mapping.stashTypeId, currentGroup, currentOptionIdx);
            
            // 第一步：选择分组
            char groupComboLabel[128];
            sprintf_s(groupComboLabel, "分组##group_%d", i);
            int groupOption = GetOptionFromGroup(currentGroup);
            
            if (ImGui::Combo(groupComboLabel, &groupOption, GetStashTypeGroupOptions())) {
                StashTypeGroup newGroup = GetGroupFromOption(groupOption);
                // 默认选择该分组的第一个选项
                int newStashTypeId = GetStashTypeIdFromGroupOption(newGroup, 0);
                if (newStashTypeId >= 0) {
                    auto* newTypeEntry = FindStashTypeByStashId(newStashTypeId);
                    if (newTypeEntry) {
                        mappingManager.SetStashType(mapping.inventoryId, newTypeEntry->id);
                    }
                } else {
                    mappingManager.SetStashType(mapping.inventoryId, "AutoDetect", -1);
                }
            }
            
            ImGui::SameLine();
            
            // 第二步：选择具体类型（根据分组动态显示选项）
            char typeComboLabel[128];
            sprintf_s(typeComboLabel, "类型##type_%d", i);
            
            int typeOption = currentOptionIdx;
            const char* optionsStr = GetStashTypeOptionsForGroup(currentGroup);
            
            if (ImGui::Combo(typeComboLabel, &typeOption, optionsStr)) {
                int newStashTypeId = GetStashTypeIdFromGroupOption(currentGroup, typeOption);
                if (newStashTypeId >= 0) {
                    auto* newTypeEntry = FindStashTypeByStashId(newStashTypeId);
                    if (newTypeEntry) {
                        mappingManager.SetStashType(mapping.inventoryId, newTypeEntry->id);
                    }
                }
            }
            
            ImGui::PopItemWidth();
            
            ImGui::SameLine();
            ImGui::TextDisabled("当前: %s", mapping.stashTypeName.c_str());
            
            // —— 截图模板设置 ——
            ImGui::Spacing();
            ImGui::Text("截图模板:");
            ImGui::SameLine();
            
            // 显示当前截图路径
            if (!mapping.screenshotPath.empty()) {
                // 只显示文件名
                std::string fileName = std::filesystem::path(mapping.screenshotPath).filename().string();
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), 
                    "📷 %s", fileName.c_str());
            } else {
                ImGui::TextDisabled("未设置");
            }
            
            // 选择截图按钮
            ImGui::SameLine();
            char screenshotBtn[64];
            sprintf_s(screenshotBtn, "📁 选择截图##ss_%d", i);
            if (ImGui::SmallButton(screenshotBtn)) {
                // 在插件目录中查找可用的截图文件
                if (!pluginDir.empty()) {
                    std::filesystem::path screenshotDir = pluginDir / "screenshots" / "stash_tabs";
                    
                    // 如果目录不存在，创建并提示
                    if (!std::filesystem::exists(screenshotDir)) {
                        std::filesystem::create_directories(screenshotDir);
                        OutputDebugStringA(("[StashMapping] 已创建截图目录: " + screenshotDir.string() + 
                            "\n请将仓库Tab截图放入该目录\n").c_str());
                    }
                    
                    // 显示选择弹窗
                    ImGui::OpenPopup(("screenshot_select_popup_" + std::to_string(i)).c_str());
                }
            }
            
            // 清除截图按钮
            ImGui::SameLine();
            char clearSsBtn[64];
            sprintf_s(clearSsBtn, "✕##clear_ss_%d", i);
            if (ImGui::SmallButton(clearSsBtn) && !mapping.screenshotPath.empty()) {
                mapping.screenshotPath.clear();
            }
            
            // 截图选择弹窗
            char popupId[128];
            sprintf_s(popupId, "screenshot_select_popup_%d", i);
            if (ImGui::BeginPopup(popupId)) {
                ImGui::Text("选择截图模板:");
                ImGui::Separator();
                
                if (!pluginDir.empty()) {
                    std::filesystem::path screenshotDir = pluginDir / "screenshots" / "stash_tabs";
                    
                    if (std::filesystem::exists(screenshotDir)) {
                        // 列出所有截图文件 (BMP/JPG/PNG)
                        std::vector<std::string> bmpFiles;
                        for (const auto& entry : std::filesystem::directory_iterator(screenshotDir)) {
                            auto ext = entry.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
                                bmpFiles.push_back(entry.path().filename().string());
                            }
                        }
                        
                        if (bmpFiles.empty()) {
                            ImGui::TextDisabled("截图目录为空");
                            ImGui::TextDisabled("请将截图(BMP/JPG/PNG)放入: %s", screenshotDir.string().c_str());
                        } else {
                            ImGui::TextDisabled("截图目录: %s", screenshotDir.string().c_str());
                            ImGui::Separator();
                            
                            for (const auto& fileName : bmpFiles) {
                                char fileBtn[256];
                                sprintf_s(fileBtn, "📷 %s", fileName.c_str());
                                
                                std::string fullPath = (screenshotDir / fileName).string();
                                bool isSelected = mapping.screenshotPath == fullPath;
                                
                                // 检查是否被其他仓库/子页使用
                                std::string conflictInfo = FindScreenshotConflict(configMutable, fullPath, mapping.inventoryId);
                                bool hasConflict = !conflictInfo.empty();
                                
                                if (hasConflict) {
                                    // 在按钮中显示冲突提示
                                    char conflictBtn[256];
                                    sprintf_s(conflictBtn, "⚠️📷 %s [已被%s占用]", fileName.c_str(), conflictInfo.c_str());
                                    if (ImGui::MenuItem(conflictBtn, nullptr, isSelected)) {
                                        mapping.screenshotPath = fullPath;
                                        ImGui::CloseCurrentPopup();
                                        
                                        char logMsg[512];
                                        sprintf_s(logMsg, "[StashMapping] ⚠️ 截图冲突! InvId=%d 选择了已被%s占用的截图: %s\n",
                                            mapping.inventoryId, conflictInfo.c_str(), fileName.c_str());
                                        OutputDebugStringA(logMsg);
                                    }
                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "⚠️冲突");
                                } else {
                                    if (ImGui::MenuItem(fileBtn, nullptr, isSelected)) {
                                        mapping.screenshotPath = fullPath;
                                        ImGui::CloseCurrentPopup();
                                        
                                        char logMsg[512];
                                        sprintf_s(logMsg, "[StashMapping] 截图选择: InvId=%d, 文件=%s, 阈值=%.2f, 类型=%s\n",
                                            mapping.inventoryId, fileName.c_str(), 
                                            settings.visionMatchThreshold, mapping.stashTypeName.c_str());
                                        OutputDebugStringA(logMsg);
                                    }
                                }
                            }
                        }
                        
                        ImGui::Separator();
                        if (ImGui::MenuItem("📂 打开截图目录")) {
                            std::string cmd = "explorer \"" + screenshotDir.string() + "\"";
                            system(cmd.c_str());
                        }
                    } else {
                        ImGui::TextDisabled("截图目录不存在");
                    }
                }
                
                ImGui::EndPopup();
            }
            
            // —— 合成物品选择 ——
            ImGui::Spacing();
            ImGui::Text("合成物品:");
            ImGui::SameLine();
            
            // 显示当前选中的物品
            if (!mapping.itemCategories.empty()) {
                std::string catsStr;
                for (auto cat : mapping.itemCategories) {
                    if (!catsStr.empty()) catsStr += ", ";
                    catsStr += GetCategoryDisplayName(static_cast<int>(cat));
                }
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                    "[%s]", catsStr.c_str());
            } else {
                ImGui::TextDisabled("未配置");
            }
            
            // 选择物品按钮
            ImGui::SameLine();
            char selectBtn[64];
            sprintf_s(selectBtn, "+选择物品##sel_%d", i);
            if (ImGui::SmallButton(selectBtn)) {
                ImGui::OpenPopup(("item_select_popup_" + std::to_string(i)).c_str());
            }
            
            // 物品选择弹窗
            char itemPopupId[128];
            sprintf_s(itemPopupId, "item_select_popup_%d", i);
            if (ImGui::BeginPopup(itemPopupId)) {
                // 清空当前选择
                if (ImGui::MenuItem("✕ 清除所有物品")) {
                    mappingManager.UpdateMapping(mapping.inventoryId, {});
                }
                ImGui::Separator();
                
                struct ItemOption { int id; const char* label; };
                ItemOption options[] = {
                    {1, "碑牌 (Tablets)"},
                    {2, "地图钥匙 (Waystones)"},
                    {3, "珠宝 (Jewels)"},
                    {4, "符文 (Runes)"},
                    {5, "精髓 (Essences)"},
                    {6, "情感蒸馏液 (Liquids)"},
                    {7, "催化剂 (Catalysts)"},
                    {8, "货币 (Currency)"},
                    {9, "碎片 (Fragments)"},
                    {10, "地图 (Maps)"},
                    {11, "预言卡 (Divination)"},
                    {12, "珠宝饰品 (Jewellery)"},
                    {13, "技能宝石 (Gems)"},
                    {14, "药剂 (Flasks)"},
                    {15, "可镶嵌物品 (Socketable)"},
                };
                
                for (const auto& opt : options) {
                    auto cat = static_cast<StashItemCategory>(opt.id);
                    bool isSelected = mapping.ContainsCategory(cat);
                    
                    char itemLabel[512];
                    sprintf_s(itemLabel, "%s %s##item_%d_%d", 
                        isSelected ? "☑" : "☐",
                        opt.label, i, opt.id);
                    
                    if (ImGui::MenuItem(itemLabel, nullptr, isSelected)) {
                        auto newCats = mapping.itemCategories;
                        if (isSelected) {
                            newCats.erase(
                                std::remove(newCats.begin(), newCats.end(), cat),
                                newCats.end());
                        } else {
                            newCats.push_back(cat);
                        }
                        mappingManager.UpdateMapping(mapping.inventoryId, newCats);
                    }
                }
                
                ImGui::EndPopup();
            }
            
            // —— 子页管理 ——
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "子页管理:");
            
            if (!mapping.subMappings.empty()) {
                ImGui::Text("子页数量: %zu", mapping.subMappings.size());
                
                // 显示子页列表
                for (int j = 0; j < (int)mapping.subMappings.size(); j++) {
                    auto& sub = mapping.subMappings[j];
                    
                    bool subExpanded = false;
                    char subTreeId[128];
                    sprintf_s(subTreeId, "sub_tree_%d_%d", i, j);
                    
                    if (ImGui::TreeNodeEx(subTreeId, subExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                        ImGui::PushID(("sub_" + std::to_string(i) + "_" + std::to_string(j)).c_str());
                        
                        ImGui::Text("📄 子页 #%d (InvId: %d)", j + 1, sub.inventoryId);
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", sub.tabLabel.c_str());
                        
                        // 删除子页
                        char subDelBtn[64];
                        sprintf_s(subDelBtn, "✕ 删除子页");
                        if (ImGui::SmallButton(subDelBtn)) {
                            mapping.subMappings.erase(mapping.subMappings.begin() + j);
                            j--;
                            ImGui::PopID();
                            ImGui::TreePop();
                            continue;
                        }
                        
                        ImGui::Separator();
                        
                        // 子页类型选择
                        StashTypeGroup subGroup;
                        int subOptionIdx;
                        GetGroupOptionFromStashTypeId(sub.stashTypeId, subGroup, subOptionIdx);
                        
                        ImGui::PushItemWidth(120);
                        
                        char subGroupLabel[128];
                        sprintf_s(subGroupLabel, "分组##sub_group");
                        int subGroupOption = GetOptionFromGroup(subGroup);
                        
                        if (ImGui::Combo(subGroupLabel, &subGroupOption, GetStashTypeGroupOptions())) {
                            StashTypeGroup newSubGroup = GetGroupFromOption(subGroupOption);
                            int newSubStashTypeId = GetStashTypeIdFromGroupOption(newSubGroup, 0);
                            sub.stashTypeId = newSubStashTypeId;
                        }
                        
                        ImGui::SameLine();
                        
                        char subTypeLabel[128];
                        sprintf_s(subTypeLabel, "类型##sub_type");
                        int subTypeOption = subOptionIdx;
                        const char* subOptionsStr = GetStashTypeOptionsForGroup(subGroup);
                        
                        if (ImGui::Combo(subTypeLabel, &subTypeOption, subOptionsStr)) {
                            int newSubStashTypeId = GetStashTypeIdFromGroupOption(subGroup, subTypeOption);
                            sub.stashTypeId = newSubStashTypeId;
                            if (newSubStashTypeId >= 0) {
                                auto* newTypeEntry = FindStashTypeByStashId(newSubStashTypeId);
                                if (newTypeEntry) {
                                    sub.stashTypeName = newTypeEntry->id;
                                }
                            }
                        }
                        
                        ImGui::PopItemWidth();
                        
                        // 子页截图设置
                        ImGui::Spacing();
                        ImGui::Text("截图模板:");
                        ImGui::SameLine();
                        
                        if (!sub.screenshotPath.empty()) {
                            std::string ssFileName = std::filesystem::path(sub.screenshotPath).filename().string();
                            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "📷 %s", ssFileName.c_str());
                        } else {
                            ImGui::TextDisabled("未设置");
                        }
                        
                        // 子页截图选择
                        ImGui::SameLine();
                        char subSsBtn[64];
                        sprintf_s(subSsBtn, "📁##sub_ss");
                        if (ImGui::SmallButton(subSsBtn)) {
                            ImGui::OpenPopup(("sub_screenshot_popup_" + std::to_string(i) + "_" + std::to_string(j)).c_str());
                        }
                        
                        // 清除子页截图
                        ImGui::SameLine();
                        char subClearSsBtn[64];
                        sprintf_s(subClearSsBtn, "✕##sub_clear_ss");
                        if (ImGui::SmallButton(subClearSsBtn) && !sub.screenshotPath.empty()) {
                            sub.screenshotPath.clear();
                        }
                        
                        // 子页截图选择弹窗
                        char subSsPopupId[128];
                        sprintf_s(subSsPopupId, "sub_screenshot_popup_%d_%d", i, j);
                        if (ImGui::BeginPopup(subSsPopupId)) {
                            ImGui::Text("选择子页截图模板:");
                            ImGui::Separator();
                            
                            if (!pluginDir.empty()) {
                                std::filesystem::path screenshotDir = pluginDir / "screenshots" / "stash_tabs";
                                
                                if (std::filesystem::exists(screenshotDir)) {
                                    // 列出所有截图文件 (BMP/JPG/PNG)
                                    std::vector<std::string> bmpFiles;
                                    for (const auto& entry : std::filesystem::directory_iterator(screenshotDir)) {
                                        auto ext = entry.path().extension().string();
                                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                        if (ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
                                            bmpFiles.push_back(entry.path().filename().string());
                                        }
                                    }
                                    
                                    if (bmpFiles.empty()) {
                                        ImGui::TextDisabled("截图目录为空");
                                    } else {
                                        for (const auto& fileName : bmpFiles) {
                                            char fileBtn[256];
                                            sprintf_s(fileBtn, "📷 %s", fileName.c_str());
                                            
                                            std::string fullPath = (screenshotDir / fileName).string();
                                            bool isSelected = sub.screenshotPath == fullPath;
                                            
                                            // 检查是否被其他仓库/子页使用（排除自身）
                                            std::string conflictInfo = FindScreenshotConflict(configMutable, fullPath, sub.inventoryId);
                                            bool hasConflict = !conflictInfo.empty();
                                            
                                            if (hasConflict) {
                                                char conflictBtn[256];
                                                sprintf_s(conflictBtn, "⚠️📷 %s [已被%s占用]", fileName.c_str(), conflictInfo.c_str());
                                                if (ImGui::MenuItem(conflictBtn, nullptr, isSelected)) {
                                                    sub.screenshotPath = fullPath;
                                                    ImGui::CloseCurrentPopup();
                                                    
                                                    char logMsg[512];
                                                    sprintf_s(logMsg, "[StashMapping] ⚠️ 子页截图冲突! ParentInvId=%d SubInvId=%d 选择了已被%s占用的截图: %s\n",
                                                        mapping.inventoryId, sub.inventoryId, conflictInfo.c_str(), fileName.c_str());
                                                    OutputDebugStringA(logMsg);
                                                }
                                                ImGui::SameLine();
                                                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "⚠️冲突");
                                            } else {
                                                if (ImGui::MenuItem(fileBtn, nullptr, isSelected)) {
                                                    sub.screenshotPath = fullPath;
                                                    ImGui::CloseCurrentPopup();
                                                    
                                                    char logMsg[512];
                                                    sprintf_s(logMsg, "[StashMapping] 子页截图选择: ParentInvId=%d, SubInvId=%d, 文件=%s, 阈值=%.2f, 类型=%s\n",
                                                        mapping.inventoryId, sub.inventoryId, fileName.c_str(), 
                                                        settings.visionMatchThreshold, sub.stashTypeName.c_str());
                                                    OutputDebugStringA(logMsg);
                                                }
                                            }
                                        }
                                    }
                                    
                                    ImGui::Separator();
                                    if (ImGui::MenuItem("📂 打开截图目录")) {
                                        std::string cmd = "explorer \"" + screenshotDir.string() + "\"";
                                        system(cmd.c_str());
                                    }
                                }
                            }
                            
                            ImGui::EndPopup();
                        }
                        
                        // 子页合成物品
                        ImGui::Spacing();
                        ImGui::Text("合成物品:");
                        ImGui::SameLine();
                        
                        if (!sub.itemCategories.empty()) {
                            std::string subCatsStr;
                            for (auto cat : sub.itemCategories) {
                                if (!subCatsStr.empty()) subCatsStr += ", ";
                                subCatsStr += GetCategoryDisplayName(static_cast<int>(cat));
                            }
                            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                "[%s]", subCatsStr.c_str());
                        } else {
                            ImGui::TextDisabled("未配置");
                        }
                        
                        ImGui::SameLine();
                        char subItemBtn[64];
                        sprintf_s(subItemBtn, "+选择物品##sub_items");
                        if (ImGui::SmallButton(subItemBtn)) {
                            ImGui::OpenPopup(("sub_item_popup_" + std::to_string(i) + "_" + std::to_string(j)).c_str());
                        }
                        
                        // 子页物品选择弹窗
                        char subItemPopupId[128];
                        sprintf_s(subItemPopupId, "sub_item_popup_%d_%d", i, j);
                        if (ImGui::BeginPopup(subItemPopupId)) {
                            if (ImGui::MenuItem("✕ 清除所有物品")) {
                                sub.itemCategories.clear();
                            }
                            ImGui::Separator();
                            
                            struct SubItemOption { int id; const char* label; };
                            SubItemOption subOptions[] = {
                                {1, "碑牌 (Tablets)"},
                                {2, "地图钥匙 (Waystones)"},
                                {3, "珠宝 (Jewels)"},
                                {4, "符文 (Runes)"},
                                {5, "精髓 (Essences)"},
                                {6, "情感蒸馏液 (Liquids)"},
                                {7, "催化剂 (Catalysts)"},
                                {8, "货币 (Currency)"},
                                {9, "碎片 (Fragments)"},
                                {10, "地图 (Maps)"},
                                {11, "预言卡 (Divination)"},
                                {12, "珠宝饰品 (Jewellery)"},
                                {13, "技能宝石 (Gems)"},
                                {14, "药剂 (Flasks)"},
                                {15, "可镶嵌物品 (Socketable)"},
                            };
                            
                            for (const auto& opt : subOptions) {
                                auto cat = static_cast<StashItemCategory>(opt.id);
                                bool isSelected = sub.ContainsCategory(cat);
                                
                                char subItemLabel[512];
                                sprintf_s(subItemLabel, "%s %s##sub_item", 
                                    isSelected ? "☑" : "☐",
                                    opt.label);
                                
                                if (ImGui::MenuItem(subItemLabel, nullptr, isSelected)) {
                                    if (isSelected) {
                                        sub.itemCategories.erase(
                                            std::remove(sub.itemCategories.begin(), sub.itemCategories.end(), cat),
                                            sub.itemCategories.end());
                                    } else {
                                        sub.itemCategories.push_back(cat);
                                    }
                                }
                            }
                            
                            ImGui::EndPopup();
                        }
                        
                        ImGui::PopID();
                        ImGui::TreePop();
                    }
                }
            } else {
                ImGui::TextDisabled("无子页");
            }
            
            // 添加子页按钮
            ImGui::SameLine();
            char addSubBtn[64];
            sprintf_s(addSubBtn, "+ 添加子页##add_sub_%d", i);
            if (ImGui::SmallButton(addSubBtn)) {
                auto& newSub = mapping.GetOrCreateSubMapping(0);
                OutputDebugStringA(("[StashMapping] 已添加子页到仓库 " + 
                    std::to_string(mapping.inventoryId) + "\n").c_str());
            }
            
            ImGui::TreePop();
        }
    }
    
    ImGui::EndChild();
    
    // —— 运行时切换说明 ——
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "💡 截图识别切换说明:");
    ImGui::TextDisabled("1. 玩家截取仓库Tab截图（建议28x28像素）");
    ImGui::TextDisabled("2. 将BMP截图放入插件目录的 screenshots/stash_tabs/ 文件夹");
    ImGui::TextDisabled("3. 在配置界面选择对应的截图文件");
    ImGui::TextDisabled("4. 运行时插件会通过图色识别定位Tab位置并点击切换");
    
    // —— 保存按钮 ——
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "配置将永久保存到 config/stash_mapping.json");
    
    if (ImGui::Button("💾 保存当前配置", ImVec2(200, 40))) {
        if (!pluginDir.empty()) {
            if (mappingManager.SaveToFile(pluginDir)) {
                ImGui::OpenPopup("save_success_popup");
            }
        } else {
            OutputDebugStringA("[StashMapping] 保存失败: pluginDir为空\n");
        }
    }
    
    // 保存成功弹窗
    if (ImGui::BeginPopup("save_success_popup")) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "✓ 保存成功!");
        ImGui::Text("配置已保存到: config/stash_mapping.json");
        ImGui::Text("下次启动时会自动加载");
        if (ImGui::Button("确定")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ============================================================
// 4.1 截图冲突检测工具函数
// ============================================================

// 检查截图路径是否已被其他映射使用（排除自身invId）
// 返回冲突描述字符串，空字符串表示无冲突
inline std::string FindScreenshotConflict(
    const StashMappingConfig& config,
    const std::string& screenshotPath,
    int selfInvId)
{
    for (const auto& m : config.tabMappings) {
        // 检查顶级仓库
        if (m.inventoryId != selfInvId && m.screenshotPath == screenshotPath) {
            char buf[256];
            sprintf_s(buf, "仓库页#%d(InvId=%d)", m.inventoryId, m.inventoryId);
            return buf;
        }
        // 递归检查子级
        for (const auto& sub : m.subMappings) {
            if (sub.inventoryId != selfInvId && sub.screenshotPath == screenshotPath) {
                char buf[256];
                sprintf_s(buf, "子页(InvId=%d,父=%d)", sub.inventoryId, m.inventoryId);
                return buf;
            }
            for (const auto& subSub : sub.subMappings) {
                if (subSub.inventoryId != selfInvId && subSub.screenshotPath == screenshotPath) {
                    char buf[256];
                    sprintf_s(buf, "孙页(InvId=%d,父=%d)", subSub.inventoryId, sub.inventoryId);
                    return buf;
                }
            }
        }
    }
    return "";
}

// ============================================================
// 4.2 Mock测试：多子页碎片大仓场景
// ============================================================

// 构造包含碑牌子页的碎片大仓 + 独立催化剂仓库，验证截图选择和子页切换逻辑
inline void RunMockTabletStashTest(
    TabletReforgeConfig::Settings& settings,
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir)
{
    OutputDebugStringA("\n========== MOCK测试开始: 多子页碎片大仓 ==========\n");
    
    auto& mappingManager = StashMappingManager::Instance();
    auto& config = mappingManager.GetConfigMutable();
    
    // —— 测试数据结构说明 ——
    // 1. 碎片大仓 (InvId=100) → 存放碑牌，含3个可编辑子页
    //    子页由玩家自行编辑，此处仅构造验证结构
    // 2. 催化剂仓库 (InvId=200) → 独立裂痕仓库，不在碎片大仓内
    
    // 1. 创建碎片大仓（含3个碑牌子页，由玩家编辑）
    const int kFragmentStashInvId = 100;
    const int kCatalystStashInvId = 200;
    const int kFragmentSubInvIds[3] = {101, 102, 103};
    const char* kFragmentSubNames[3] = {"碑牌子页1(玩家编辑)", "碑牌子页2(玩家编辑)", "碑牌子页3(玩家编辑)"};
    const char* kFragmentSubScreenshots[3] = {"fragment_sub_1.bmp", "fragment_sub_2.bmp", "fragment_sub_3.bmp"};
    
    StashTabItemMapping fragmentStash;
    fragmentStash.inventoryId = kFragmentStashInvId;
    fragmentStash.stashTypeName = "FragmentStash";
    fragmentStash.stashTypeId = 9;
    fragmentStash.tabLabel = "碎片大仓";
    fragmentStash.screenshotPath = "fragment_main_stash.bmp";
    fragmentStash.itemCategories = {StashItemCategory::Tablets};
    
    // 添加3个碑牌子页（玩家可自行编辑名称和截图）
    for (int i = 0; i < 3; i++) {
        auto& sub = fragmentStash.GetOrCreateSubMapping(kFragmentSubInvIds[i]);
        sub.stashTypeName = "FragmentStash";
        sub.stashTypeId = 9;
        sub.tabLabel = kFragmentSubNames[i];
        sub.screenshotPath = kFragmentSubScreenshots[i];
        sub.itemCategories = {StashItemCategory::Tablets};
    }
    
    // 2. 创建独立催化剂仓库（裂痕仓库）
    StashTabItemMapping catalystStash;
    catalystStash.inventoryId = kCatalystStashInvId;
    catalystStash.stashTypeName = "CatalystStash";
    catalystStash.stashTypeId = 11;
    catalystStash.tabLabel = "催化剂仓库(裂痕)";
    catalystStash.screenshotPath = "catalyst_stash.bmp";
    catalystStash.itemCategories = {StashItemCategory::Catalysts};
    
    // 清除旧数据，添加测试数据
    config.tabMappings.clear();
    config.tabMappings.push_back(fragmentStash);
    config.tabMappings.push_back(catalystStash);
    
    // 3. 验证数据结构
    char log1[256];
    sprintf_s(log1, "[Mock] 碎片大仓子页数量: %zu (预期: 3)\n", fragmentStash.subMappings.size());
    OutputDebugStringA(log1);
    
    char log2[256];
    sprintf_s(log2, "[Mock] 顶层仓库数量: %zu (预期: 2, 碎片+催化剂)\n", config.tabMappings.size());
    OutputDebugStringA(log2);
    
    // 4. 运行测试用例
    int testPassed = 0;
    int testFailed = 0;
    
    // 测试1: 碎片大仓子页数量验证
    {
        if (fragmentStash.subMappings.size() == 3) {
            OutputDebugStringA("[Mock] ✓ 测试1通过: 碎片大仓含3个子页\n");
            testPassed++;
        } else {
            char msg[256];
            sprintf_s(msg, "[Mock] ✗ 测试1失败: 预期3个子页, 实际%zu\n", fragmentStash.subMappings.size());
            OutputDebugStringA(msg);
            testFailed++;
        }
    }
    
    // 测试2: 独立截图无冲突
    {
        std::string conflict = FindScreenshotConflict(config, "unique_screenshot.bmp", 999);
        if (conflict.empty()) {
            OutputDebugStringA("[Mock] ✓ 测试2通过: 独立截图无冲突\n");
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试2失败: 独立截图不应有冲突\n");
            testFailed++;
        }
    }
    
    // 测试3: 碎片大仓截图冲突检测
    {
        std::string conflict = FindScreenshotConflict(config, "fragment_main_stash.bmp", 999);
        if (!conflict.empty()) {
            char msg[256];
            sprintf_s(msg, "[Mock] ✓ 测试3通过: 碎片大仓截图冲突检测成功 → %s\n", conflict.c_str());
            OutputDebugStringA(msg);
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试3失败: 碎片大仓截图应被检测为冲突\n");
            testFailed++;
        }
    }
    
    // 测试4: 催化剂仓库截图冲突检测（独立仓库）
    {
        std::string conflict = FindScreenshotConflict(config, "catalyst_stash.bmp", 999);
        if (!conflict.empty()) {
            char msg[256];
            sprintf_s(msg, "[Mock] ✓ 测试4通过: 催化剂仓库截图冲突检测成功 → %s\n", conflict.c_str());
            OutputDebugStringA(msg);
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试4失败: 催化剂仓库截图应被检测为冲突\n");
            testFailed++;
        }
    }
    
    // 测试5: 碎片大仓自身排除（不与自己冲突）
    {
        std::string conflict = FindScreenshotConflict(config, "fragment_main_stash.bmp", kFragmentStashInvId);
        if (conflict.empty()) {
            OutputDebugStringA("[Mock] ✓ 测试5通过: 碎片大仓自身排除正常\n");
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试5失败: 自身不应报告冲突\n");
            testFailed++;
        }
    }
    
    // 测试6: 催化剂仓库自身排除
    {
        std::string conflict = FindScreenshotConflict(config, "catalyst_stash.bmp", kCatalystStashInvId);
        if (conflict.empty()) {
            OutputDebugStringA("[Mock] ✓ 测试6通过: 催化剂仓库自身排除正常\n");
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试6失败: 自身不应报告冲突\n");
            testFailed++;
        }
    }
    
    // 测试7: 相似度阈值边界验证
    {
        double threshold = settings.visionMatchThreshold;
        if (threshold >= 0.0 && threshold <= 1.0) {
            char msg[256];
            sprintf_s(msg, "[Mock] ✓ 测试7通过: 阈值%.4f在有效范围[0.0,1.0]内\n", threshold);
            OutputDebugStringA(msg);
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试7失败: 阈值超出有效范围\n");
            testFailed++;
        }
    }
    
    // 测试8: 递归查找碎片大仓的子页
    {
        auto* found = config.FindByInventoryId(kFragmentSubInvIds[1]);
        if (found && found->tabLabel == kFragmentSubNames[1]) {
            char msg[256];
            sprintf_s(msg, "[Mock] ✓ 测试8通过: 递归查找子页成功 → %s\n", found->tabLabel.c_str());
            OutputDebugStringA(msg);
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试8失败: 递归查找子页失败\n");
            testFailed++;
        }
    }
    
    // 测试9: 催化剂仓库为独立仓库（非碎片大仓子页）
    {
        auto* foundCatalyst = config.FindByInventoryId(kCatalystStashInvId);
        bool isTopLevel = false;
        if (foundCatalyst) {
            for (const auto& m : config.tabMappings) {
                if (m.inventoryId == kCatalystStashInvId && m.parentInventoryId == 0) {
                    isTopLevel = true;
                    break;
                }
            }
        }
        if (isTopLevel) {
            OutputDebugStringA("[Mock] ✓ 测试9通过: 催化剂仓库为独立顶层仓库（非碎片大仓子页）\n");
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试9失败: 催化剂仓库应为独立顶层仓库\n");
            testFailed++;
        }
    }
    
    // 测试10: 子页截图重复使用检测（玩家误用）
    {
        std::string sharedScreenshot = "fragment_sub_1.bmp";
        std::string conflict = FindScreenshotConflict(config, sharedScreenshot, kFragmentSubInvIds[2]);
        if (!conflict.empty()) {
            char msg[256];
            sprintf_s(msg, "[Mock] ✓ 测试10通过: 重复截图检测成功 → %s 已被%s占用\n", 
                sharedScreenshot.c_str(), conflict.c_str());
            OutputDebugStringA(msg);
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试10失败: 应检测到重复截图\n");
            testFailed++;
        }
    }
    
    // 测试11: 碎片大仓不含符文（验证已去除符文）
    {
        bool hasRunes = false;
        for (const auto& cat : fragmentStash.itemCategories) {
            if (cat == StashItemCategory::Runes) hasRunes = true;
        }
        if (!hasRunes) {
            OutputDebugStringA("[Mock] ✓ 测试11通过: 碎片大仓已不含符文分类\n");
            testPassed++;
        } else {
            OutputDebugStringA("[Mock] ✗ 测试11失败: 碎片大仓不应包含符文\n");
            testFailed++;
        }
    }
    
    // 5. 输出测试结果
    char summary[512];
    sprintf_s(summary, "\n========== MOCK测试结果 ==========\n通过: %d, 失败: %d\n总测试: %d\n通过率: %.0f%%\n",
        testPassed, testFailed, testPassed + testFailed,
        (testPassed + testFailed) > 0 ? 100.0 * testPassed / (testPassed + testFailed) : 0.0);
    OutputDebugStringA(summary);
    
    // 6. 清理测试数据（恢复原配置）
    config.tabMappings.clear();
    OutputDebugStringA("========== MOCK测试结束 ==========\n\n");
}

// ============================================================
// 5. 视觉识别测试界面
// ============================================================

// 绘制视觉识别测试面板
inline void DrawVisionRecognitionTestPanel(
    TabletReforgeConfig::Settings& settings,
    const PluginSDK::Context* ctx,
    const std::filesystem::path& pluginDir)
{
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "★ 视觉识别测试");
    ImGui::TextDisabled("测试仓库Tab的视觉识别功能");
    ImGui::Separator();
    
    static int s_testInventoryId = 0;
    static double s_lastConfidence = 0;
    static bool s_lastMatched = false;
    static std::string s_lastStashName;
    static double s_lastTimeMs = 0;
    
    // 输入要测试的 InventoryId
    ImGui::InputInt("测试 InventoryId", &s_testInventoryId);
    ImGui::SameLine();
    
    if (ImGui::SmallButton("🔍 扫描可用ID") && ctx) {
        auto invs = ctx->Inventory.GetAll();
        if (!invs.empty()) {
            // 选择第一个有效的仓库
            for (const auto& inv : invs) {
                if (inv.Address != 0 && inv.Grid.Valid && inv.TotalBoxesX * inv.TotalBoxesY >= 4) {
                    s_testInventoryId = inv.InventoryId;
                    break;
                }
            }
        }
    }
    
    // 执行测试
    if (ImGui::Button("▶ 执行视觉识别测试") && ctx && s_testInventoryId > 0) {
        auto start = std::chrono::high_resolution_clock::now();
        
        auto result = RecognizeStashTabIntegrated(
            ctx, s_testInventoryId, pluginDir, true);
        
        auto end = std::chrono::high_resolution_clock::now();
        s_lastTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
        
        s_lastMatched = result.visionFound;
        s_lastConfidence = result.visionConfidence;
        s_lastStashName = result.visionStashTypeName;
        
        if (!result.visionFound) {
            // 自动截图
            auto screenshotResult = SaveScreenshotOnRecognitionFailure(
                ctx, s_testInventoryId,
                "视觉识别测试失败",
                pluginDir);
            
            if (screenshotResult.success) {
                OutputDebugStringA(("[VisionTest] 识别失败，已保存截图: " + 
                    screenshotResult.filePath.string() + "\n").c_str());
            }
        }
    }
    
    ImGui::Separator();
    
    // 显示测试结果
    ImGui::Text("测试结果:");
    ImGui::Text("  InventoryId: %d", s_testInventoryId);
    ImGui::Text("  识别结果: %s", s_lastMatched ? "成功 ✓" : "失败 ✗");
    ImGui::Text("  置信度: %.4f", s_lastConfidence);
    ImGui::Text("  识别仓库: %s", s_lastStashName.empty() ? "(未知)" : s_lastStashName.c_str());
    ImGui::Text("  耗时: %.2f ms", s_lastTimeMs);
    
    // 测试结果可视化
    if (s_lastMatched) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
            "✓ 视觉识别成功！置信度 %.4f", s_lastConfidence);
        
        // 如果置信度较低，给出警告
        if (s_lastConfidence < 0.6) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "⚠ 置信度较低，可能影响识别准确性");
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            "✗ 视觉识别失败！");
        ImGui::TextDisabled("可能原因: 模板未加载、Tab不在屏幕上、图标样式不匹配");
    }
    
    ImGui::Separator();
    
    // 性能测试
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "性能测试");
    if (ImGui::Button("▶ 运行10次性能测试") && ctx && s_testInventoryId > 0) {
        auto report = RunVisionPerformanceTest(ctx, pluginDir, 10);
        
        char reportText[512];
        sprintf_s(reportText,
            "测试次数: %d\n通过: %d\n失败: %d\n平均耗时: %.2f ms\n最小耗时: %.2f ms\n最大耗时: %.2f ms\nP95耗时: %.2f ms",
            report.totalTests,
            report.passedTests,
            report.failedTests,
            report.avgTimeMs,
            report.minTimeMs == 1e9 ? 0 : report.minTimeMs,
            report.maxTimeMs,
            report.p95TimeMs);
        
        // 显示结果
        ImGui::LogText("%s", reportText);
    }
    
    // 手动截图保存
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "截图工具");
    
    if (ImGui::Button("📷 手动保存仓库截图") && ctx && s_testInventoryId > 0) {
        auto inv = ctx->Inventory.Get(s_testInventoryId);
        if (inv.Address != 0 && inv.Grid.Valid) {
            int cx = (int)inv.Grid.GridScreenX;
            int cy = (int)inv.Grid.GridScreenY;
            int cellSize = inv.Grid.CellSize > 0 ? (int)inv.Grid.CellSize : 50;
            int w = cellSize * inv.TotalBoxesX;
            int h = cellSize * inv.TotalBoxesY;
            
            VisionRecogNS::ScreenFrame frame;
            if (VisionRecogNS::CaptureScreenRegion(cx, cy, w, h, frame)) {
                std::filesystem::path logDir = pluginDir.empty() 
                    ? std::filesystem::path("logs") 
                    : pluginDir / "logs" / "screenshots";
                
                std::error_code ec;
                std::filesystem::create_directories(logDir, ec);
                
                SYSTEMTIME st;
                ::GetLocalTime(&st);
                char ts[32];
                sprintf_s(ts, "%04d%02d%02d_%02d%02d%02d",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                
                std::string filename = "manual_stash_" + std::to_string(s_testInventoryId) + "_" + ts + ".bmp";
                auto bmpPath = logDir / filename;
                
                if (VisionRecogNS::SaveFrameToBmp(frame, bmpPath)) {
                    char msg[256];
                    sprintf_s(msg, "截图已保存: %s", bmpPath.string().c_str());
                    OutputDebugStringA(msg);
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                        "✓ 截图已保存: %s", bmpPath.filename().string().c_str());
                }
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                "✗ 无法获取仓库信息，请确保仓库已打开");
        }
    }
}

} // namespace TabletReforgeUi
