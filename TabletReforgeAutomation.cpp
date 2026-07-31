// TabletReforgeAutomation.cpp — 先行者碑牌重铸自动化插件主入口
//
// PoeFixer 插件（SDK v6），实现"取碑牌→重铸台三合成→存回仓库"的全自动循环。
// 支持：地图钥匙(Waystone)、先行者碑牌(Precursor Tablet/輻照碑牌)、
//       珠宝、符文、精髓、情感蒸馏液、催化剂(Catalyst) 等多种三合一合成。
//
// 生命周期：
//   OnEnable  → 加载配置，订阅 OnFrame 事件
//   OnFrame   → 检测热键，驱动状态机 Tick
//   OnDisable → 停止状态机，取消订阅，保存配置
//   DrawSettings → ImGui 设置面板
//   DrawUI    → 状态 overlay
//
// 编译：MSBuild /p:Configuration=Release /p:Platform=x64
// 输出：bin\Release\TabletReforgeAutomation.dll
#include "sdk/PluginSDK.h"

#include "config/CalibData.h"
#include "config/Settings.h"
#include "flow/StateMachine.h"
#include "game/ReforgeOps.h"
#include "game/StashItemMapper.h"
#include "input/Win32Input.h"
#include "ui/CalibOverlay.h"
#include "ui/SettingsPanel.h"
#include "game/TabletFilter.h"
#include "ui/StatusOverlay.h"
#include "test/MockGameState.h"

#include <imgui.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <functional>

class TabletReforgePlugin : public PluginSDK::Plugin {
public:
    const char* GetName() const override { return "Tablet Reforge Automation"; }

    bool WantsOverlay() const override { return m_settings.enabled || m_debugWindowOpen; }

    void OnEnable(bool /*isGameAttached*/) override {
        // 设置 ImGui 上下文
        if (ctx()->ImGuiContext) {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

            // 加载中文字体（用于显示 POE2 的中文 StringId 和 UI 文本）
            LoadChineseFont();
        }

        // 加载配置
        m_settings.Load(DirectoryPath());
        m_calib.Load(DirectoryPath());
        SyncConfigToStateMachine();
        
        // 自动加载仓库映射配置
        auto& mappingManager = TabletReforgeGame::StashMappingManager::Instance();
        mappingManager.LoadFromFile(DirectoryPath());

        // 订阅 OnFrame 事件
        auto& events = const_cast<PluginSDK::EventsService&>(ctx()->Events);
        m_frameToken = events.OnFrame([this] { OnFrameTick(); });

        ctx()->Log.Info("Tablet Reforge Automation plugin enabled");
    }

    // —— 加载中文字体 ——
    // POE2 的 UI 节点 StringId 和文本包含中文，ImGui 默认字体只有 ASCII。
    // 此函数加载 Windows 系统中文字体并合并到 ImGui 字体中。
    void LoadChineseFont() {
        if (m_chineseFontLoaded) return;
        if (!ImGui::GetIO().Fonts) return;

        // 手动构建中文常用字形范围（CJK 统一汉字 + 标点符号）
        // 因为 Fixer SDK 的 ImGui 可能禁用了 GetGlyphRangesChinese 系列函数
        static const ImWchar chineseGlyphRanges[] = {
            // 常用中文标点
            0x0020, 0x007E,   // Basic ASCII
            0x00A0, 0x00FF,   // Latin-1 Supplement（含部分标点）
            0x2000, 0x206F,   // General Punctuation
            0x3000, 0x303F,   // CJK Symbols and Punctuation
            0x3040, 0x309F,   // Hiragana（日文假名）
            0x30A0, 0x30FF,   // Katakana（日文片假名）
            0x4E00, 0x9FFF,   // CJK Unified Ideographs（中文核心区）
            0x3400, 0x4DBF,   // CJK Unified Ideographs Extension A
            0x20000, 0x2A6DF, // CJK Unified Ideographs Extension B
            0, 0              // 终止符
        };

        // Windows 系统中文字体候选（按优先级排序）
        const char* fontCandidates[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",        // Microsoft YaHei 微软雅黑
            "C:\\Windows\\Fonts\\msyh.ttf",        // Microsoft YaHei
            "C:\\Windows\\Fonts\\msyhui.ttf",      // Microsoft YaHei UI
            "C:\\Windows\\Fonts\\simhei.ttf",      // SimHei 黑体
            "C:\\Windows\\Fonts\\simsun.ttc",      // SimSun 宋体
            "C:\\Windows\\Fonts\\simkai.ttf",      // KaiTi 楷体
        };

        for (const char* fontPath : fontCandidates) {
            std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;

            std::streamsize size = file.tellg();
            if (size <= 0 || size > 16 * 1024 * 1024) continue;

            file.seekg(0, std::ios::beg);
            std::vector<char> fontData(static_cast<size_t>(size));
            if (!file.read(fontData.data(), size)) continue;

            // 合并到现有字体（MergeMode=true，保留原有的 ASCII 字体）
            ImFontConfig config;
            config.MergeMode = true;
            config.PixelSnapH = true;
            config.FontDataOwnedByAtlas = false;

            // 使用手动构建的中文常用字形范围
            const ImWchar* glyphRanges = chineseGlyphRanges;

            ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                fontData.data(), size, 16.0f, &config, glyphRanges);

            if (font) {
                m_chineseFont = font;
                m_chineseFontLoaded = true;
                m_chineseFontData = std::move(fontData); // 保持字体数据存活

                // 强制重建字体 atlas（如果 Fixer 宿主已经构建过 atlas，需要重新构建才能包含中文字体）
                ImGui::GetIO().Fonts->Build();

                std::string msg = std::string("中文字体已加载: ") + fontPath;
                ctx()->Log.Info(msg.c_str());
                return;
            }
        }

        ctx()->Log.Warn("无法加载中文字体，UI 中的中文可能显示为乱码");
    }

    void OnDisable() override {
        // 停止状态机（确保 Ctrl 释放）
        m_stateMachine.Stop();

        // 取消订阅
        if (m_frameToken.Valid()) {
            auto& events = const_cast<PluginSDK::EventsService&>(ctx()->Events);
            events.Unsubscribe(m_frameToken);
            m_frameToken = {};
        }

        // 保存配置
        SaveSettings();

        ctx()->Log.Info("Tablet Reforge Automation plugin disabled");
    }

    void DrawSettings() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        TabletReforgeUi::DrawSettingsPanel(m_settings, m_calib, m_stateMachine, ctx(), DirectoryPath());

        // 标定向导按钮
        ImGui::Separator();
        if (ImGui::Button("打开标定向导...")) {
            m_calibWizard.active = true;
        }
    }

    void DrawUI() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        // 标定向导窗口
        const bool wasActive = m_calibWizard.active;
        TabletReforgeUi::DrawCalibWizard(m_calibWizard, m_calib, ctx());

        // 向导点了"保存"或刚关闭 → 写入 calib.json
        if (m_calibWizard.calibSaved || (wasActive && !m_calibWizard.active)) {
            m_calib.Save(DirectoryPath());
            m_calibWizard.calibSaved = false;
        }

        // 向导点了"测试标定" → 同步配置 + 启动测试模式（跑 1 轮）
        if (m_calibWizard.testRequested) {
            m_calibWizard.testRequested = false;
            m_calib.Save(DirectoryPath());         // 先保存标定
            m_settings.enabled = true;             // 确保 overlay 显示
            SyncConfigToStateMachine();            // 同步最新配置
            m_stateMachine.StartTest(ctx());        // 启动测试
        }

        // 调试窗口（始终可用，独立于主设置）
        if (m_debugWindowOpen) {
            DrawDebugWindow();
        }

        // 状态 overlay（仅在启用时显示）
        if (!m_settings.enabled) return;
        if (!ctx()->Game.IsInGame()) return;

        TabletReforgeUi::DrawStatusOverlay(m_stateMachine);
    }

    void DrawDebugWindow() {
        if (!m_debugWindowOpen) return;

        ImGui::SetNextWindowSize(ImVec2(700, 700), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("状态机调试工具", &m_debugWindowOpen)) {
            // === 第一行：测试和清空 ===
            if (ImGui::Button("运行所有测试")) {
                m_debugLog.clear();
                TabletReforgeTest::RunAllTests([this](const std::string& msg) {
                    m_debugLog.push_back(msg);
                });
            }
            ImGui::SameLine();
            if (ImGui::Button("清空日志")) {
                m_debugLog.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button("导出日志")) {
                ExportDebugLog();
            }

            ImGui::Separator();

            // === 状态机日志 ===
            ImGui::Text("=== 状态机运行日志 ===");
            if (ImGui::Button("刷新状态机日志")) {
                auto logs = m_stateMachine.GetRecentLogs(100);
                m_debugLog.push_back("");
                m_debugLog.push_back("=== 状态机运行日志 ===");
                for (const auto& entry : logs) {
                    std::string line = "[" + std::to_string(entry.timestampMs) + "ms] ";
                    if (entry.severity == 2) {
                        line += "[ERROR] ";
                    } else if (entry.severity == 1) {
                        line += "[WARN] ";
                    }
                    line += entry.message;
                    m_debugLog.push_back(line);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("导出状态机日志")) {
                ExportStateMachineLog();
            }

            ImGui::Separator();

            // === 面板诊断（关键功能：显示所有面板类型）===
            ImGui::Text("=== 面板诊断（确认背包/仓库/合成面板能被识别）===");
            if (ImGui::Button("🔍 诊断所有面板类型")) {
                if (!ctx()) {
                    m_debugLog.push_back("[ERROR] ctx() 为空，不在游戏中");
                } else {
                    m_debugLog.push_back("");
                    m_debugLog.push_back("=== 面板类型诊断 ===");
                    auto panels = TabletReforgeGame::GetAllInventoryPanels(ctx());
                    int bagLikeCount = 0, stashLikeCount = 0, benchLikeCount = 0;
                    for (const auto& p : panels) {
                        char line[512];
                        const char* type = "Unknown";
                        if (p.isBagLike) { type = "BAG_LIKE"; bagLikeCount++; }
                        else if (p.isStashLike) { type = "STASH_LIKE"; stashLikeCount++; }
                        else if (p.isBenchLike) { type = "BENCH_LIKE"; benchLikeCount++; }
                        ::sprintf_s(line,
                            "[%s] ID=%04d Name='%s' %dx%d=%3d格  Items=%-3d  Grid.Valid=%d",
                            type, p.inventoryId,
                            p.name.c_str(),
                            p.totalBoxesX, p.totalBoxesY,
                            p.totalBoxesX * p.totalBoxesY,
                            p.itemCount, p.gridValid ? 1 : 0);
                        m_debugLog.push_back(std::string(line));
                    }
                    char summary[256];
                    ::sprintf_s(summary,
                        "[汇总] 总面板=%d  背包类=%d  仓库类=%d  合成类=%d",
                        (int)panels.size(), bagLikeCount, stashLikeCount, benchLikeCount);
                    m_debugLog.push_back(std::string(summary));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("📋 列出所有 Inventory")) {
                DumpAllInventories(/*withItems*/ false);
            }
            ImGui::SameLine();
            if (ImGui::Button("📋 列出 Inventory+物品")) {
                DumpAllInventories(/*withItems*/ true);
            }

            ImGui::Separator();

            // === 背包扫描验证（使用新的 CollectBagItems）===
            ImGui::Text("=== 背包扫描验证（使用新的全面板扫描）===");
            if (ImGui::Button("🧪 扫描背包物品（新算法）")) {
                if (!ctx()) {
                    m_debugLog.push_back("[ERROR] ctx() 为空");
                } else {
                    m_debugLog.push_back("");
                    m_debugLog.push_back("=== 背包扫描结果（使用 IsBagLikeInventory 过滤）===");
                    auto items = TabletReforgeGame::CollectBagItems(ctx());
                    char line[256];
                    ::sprintf_s(line, "背包类面板物品总数: %d", (int)items.size());
                    m_debugLog.push_back(std::string(line));
                    
                    int matchCount = 0;
                    for (const auto& bi : items) {
                        auto analysis = TabletReforgeGame::AnalyzeItem(
                            bi.path, bi.baseType, bi.rarity, bi.identified, m_settings);
                        if (analysis.matchesCurrentType) matchCount++;
                        
                        char itemline[512];
                        ::sprintf_s(itemline,
                            "  [%s] R=%d Id=%d Path='%s' BT='%s' Matches=%d",
                            bi.inventoryName.c_str(),
                            bi.rarity, bi.identified ? 1 : 0,
                            bi.path.empty() ? "(empty)" : bi.path.c_str(),
                            bi.baseType.empty() ? "(empty)" : bi.baseType.c_str(),
                            analysis.matchesCurrentType ? 1 : 0);
                        m_debugLog.push_back(std::string(itemline));
                    }
                    char summary[256];
                    ::sprintf_s(summary,
                        "匹配当前设置 itemType=%d 的物品数: %d / %d",
                        m_settings.itemType, matchCount, (int)items.size());
                    m_debugLog.push_back(std::string(summary));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("📊 统计背包碑牌数")) {
                if (!ctx()) {
                    m_debugLog.push_back("[ERROR] ctx() 为空");
                } else {
                    auto stats = TabletReforgeGame::GetBagItemStats(ctx(), m_settings);
                    int matCount = TabletReforgeGame::CountMaterialTablets(ctx(), m_settings);
                    int emptySlots = TabletReforgeGame::Count1x1EmptySlots(ctx());
                    char line[1024];
                    ::sprintf_s(line,
                        "背包统计: 匹配原料=%d  总碑牌=%d  总物品=%d  空1x1格=%d\n"
                        "  [详细] 总物品=%d  有坐标=%d  无坐标=%d  有Path=%d  有BT=%d  匹配类型=%d\n"
                        "  [面板] 背包类=%d  总可见=%d",
                        matCount, stats.itemsMatchingType, stats.totalItems, emptySlots,
                        stats.totalItems, stats.itemsWithCoords,
                        stats.totalItems - stats.itemsWithCoords,
                        stats.itemsWithPath, stats.itemsWithBT,
                        stats.itemsMatchingType,
                        stats.bagLikePanels, stats.totalPanels);
                    m_debugLog.push_back(std::string(line));
                }
            }

            // === MainInventory 专项诊断 ===
            ImGui::Separator();
            ImGui::Text("=== MainInventory 专项诊断（验证背包扫描）===");
            if (ImGui::Button("🔍 诊断 MainInventory 面板")) {
                if (!ctx()) {
                    m_debugLog.push_back("[ERROR] ctx() 为空");
                } else {
                    m_debugLog.push_back("");
                    m_debugLog.push_back("=== MainInventory 面板诊断 ===");
                    
                    float dispW = 0.f, dispH = 0.f;
                    TabletReforgeGame::GetScreenSize(ctx(), dispW, dispH);
                    
                    int mainInvCount = 0;
                    int mainInvItems = 0;
                    int mainInvWithCoords = 0;
                    
                    for (const auto& inv : ctx()->Inventory.GetAll()) {
                        const char* nameC = ctx()->Inventory.GetName(inv.InventoryId);
                        std::string name = nameC ? nameC : "(null)";
                        bool isMainInv = (name.rfind("MainInventory", 0) == 0);
                        
                        if (isMainInv) {
                            mainInvCount++;
                            int slots = inv.TotalBoxesX * inv.TotalBoxesY;
                            int itemCount = static_cast<int>(inv.Items.size());
                            
                            char line[512];
                            ::sprintf_s(line,
                                "\n[MainInventory] ID=%04d Name='%s' %dx%d=%d格  Items=%d  Grid.Valid=%d"
                                "  GridScreen=(%.0f,%.0f)  CellSize=%.1f",
                                inv.InventoryId, name.c_str(),
                                inv.TotalBoxesX, inv.TotalBoxesY, slots, itemCount,
                                inv.Grid.Valid ? 1 : 0,
                                inv.Grid.GridScreenX, inv.Grid.GridScreenY,
                                inv.Grid.CellSize);
                            m_debugLog.push_back(std::string(line));
                            
                            for (const auto& item : inv.Items) {
                                mainInvItems++;
                                auto rect = TabletReforgeGame::ResolveItemRect(inv, item, dispW, dispH);
                                
                                bool hasPath = !item.Path.empty();
                                bool hasBT = !item.BaseTypeName.empty();
                                bool sv = item.ScreenValid;
                                
                                char itemline[512];
                                ::sprintf_s(itemline,
                                    "  Item[%d,%d] R%d Id=%d SV=%d Path='%s' BT='%s' ScreenXY=(%.0f,%.0f) Size=(%.0f,%.0f)  GridCalc=%s",
                                    item.SlotX, item.SlotY,
                                    item.Rarity, item.IsIdentified ? 1 : 0,
                                    sv ? 1 : 0,
                                    hasPath ? item.Path.c_str() : "(empty)",
                                    hasBT ? item.BaseTypeName.c_str() : "(empty)",
                                    item.ScreenX, item.ScreenY,
                                    item.ScreenW, item.ScreenH,
                                    rect ? "OK" : "FAIL");
                                m_debugLog.push_back(std::string(itemline));
                                
                                if (rect) mainInvWithCoords++;
                            }
                        }
                    }
                    
                    char summary[512];
                    ::sprintf_s(summary,
                        "\n[MainInventory 汇总] 面板数=%d  总物品=%d  有坐标=%d  无坐标=%d",
                        mainInvCount, mainInvItems, mainInvWithCoords,
                        mainInvItems - mainInvWithCoords);
                    m_debugLog.push_back(std::string(summary));
                }
            }

            ImGui::Separator();

            // === 关键诊断：扫描所有可见物品（不过滤）===
            ImGui::Text("=== 关键诊断：扫描所有可见物品（忽略面板过滤）===");
            if (ImGui::Button("🔬 扫描所有物品（原始数据）")) {
                if (!ctx()) {
                    m_debugLog.push_back("[ERROR] ctx() 为空");
                } else {
                    m_debugLog.push_back("");
                    m_debugLog.push_back("=== 所有可见 Inventory 物品（原始 Dump，不做任何过滤）===");
                    m_debugLog.push_back("提示：此功能显示游戏 SDK 返回的所有原始数据，用于对比 POE2 数据文件");
                    
                    const auto& all = ctx()->Inventory.GetAll();
                    char hdr[512];
                    ::sprintf_s(hdr, "可见 Inventory 面板总数: %d", (int)all.size());
                    m_debugLog.push_back(std::string(hdr));
                    
                    int totalItems = 0;
                    int itemsWithPath = 0;
                    int itemsWithBT = 0;
                    int itemsWithBoth = 0;
                    
                    for (const auto& inv : all) {
                        const char* invNameC = ctx()->Inventory.GetName(inv.InventoryId);
                        std::string invName = invNameC ? invNameC : "(null)";
                        int slots = inv.TotalBoxesX * inv.TotalBoxesY;
                        
                        char invline[256];
                        ::sprintf_s(invline,
                            "\n[面板] ID=%04d Name='%s' %dx%d=%d格  Items=%d  GridValid=%d  GridOnScreen=%s",
                            inv.InventoryId, invName.c_str(),
                            inv.TotalBoxesX, inv.TotalBoxesY, slots,
                            (int)inv.Items.size(),
                            inv.Grid.Valid ? 1 : 0,
                            (inv.Grid.GridScreenX >= 0 && inv.Grid.GridScreenY >= 0) ? "YES" : "no");
                        m_debugLog.push_back(std::string(invline));
                        
                        for (const auto& item : inv.Items) {
                            totalItems++;
                            bool hasPath = !item.Path.empty();
                            bool hasBT = !item.BaseTypeName.empty();
                            if (hasPath) itemsWithPath++;
                            if (hasBT) itemsWithBT++;
                            if (hasPath && hasBT) itemsWithBoth++;
                            
                            auto analysis = TabletReforgeGame::AnalyzeItem(
                                item.Path, item.BaseTypeName, item.Rarity, 
                                item.IsIdentified, m_settings);
                            
                            auto poe2Report = TabletReforgeGame::Poe2MatchReport(
                                item.Path, item.BaseTypeName, item.Rarity,
                                item.IsIdentified, m_settings);
                            
                            char itemline[1024];
                            ::sprintf_s(itemline,
                                "  -> [%d,%d] R%d Id=%d Path='%s' BT='%s' Match=%d  Tag=%s",
                                item.SlotX, item.SlotY,
                                item.Rarity, item.IsIdentified ? 1 : 0,
                                hasPath ? item.Path.c_str() : "(empty)",
                                hasBT ? item.BaseTypeName.c_str() : "(empty)",
                                analysis.matchesCurrentType ? 1 : 0,
                                TabletReforgeGame::DebugItemTypeTag(item.Path, item.BaseTypeName).c_str());
                            m_debugLog.push_back(std::string(itemline));
                            
                            if (!poe2Report.empty()) {
                                m_debugLog.push_back(poe2Report);
                            }
                        }
                    }
                    
                    char summary[512];
                    ::sprintf_s(summary,
                        "\n[汇总] 总物品=%d  有Path=%d  有BT=%d  两者都有=%d",
                        totalItems, itemsWithPath, itemsWithBT, itemsWithBoth);
                    m_debugLog.push_back(std::string(summary));
                    m_debugLog.push_back("[提示] 若物品 Path 和 BaseType 都为空，说明 SDK 未提供这些信息，无法识别");
                    m_debugLog.push_back("[提示] 若 Path 有值但不匹配任何已知模式，请到 POE2 wiki 查询该物品类型");
                }
            }

            ImGui::Separator();

            // === 全物品识别诊断（使用新的 ScanAndIdentifyAllItems）===
            ImGui::Text("=== 全物品识别诊断（验证物品能否被正确识别）===");
            if (ImGui::Button("🆕 全物品识别诊断（含背包+仓库）")) {
                if (!ctx()) {
                    m_debugLog.push_back("[ERROR] ctx() 为空");
                } else {
                    m_debugLog.push_back("");
                    m_debugLog.push_back("=== 全物品识别诊断 ===");
                    m_debugLog.push_back("使用 ScanAndIdentifyAllItems 扫描所有可见面板");
                    m_debugLog.push_back("对比 POE2 baseitemtypes.json 数据验证识别准确性");
                    
                    auto items = TabletReforgeGame::ScanAndIdentifyAllItems(ctx(), m_settings, /*scanAllPanels*/ true);
                    
                    auto summary = TabletReforgeGame::GenerateIdentificationSummary(items, m_settings);
                    m_debugLog.push_back(summary);
                    
                    m_debugLog.push_back("\n--- 逐项识别结果 ---");
                    for (const auto& it : items) {
                        char line[2048];
                        ::sprintf_s(line,
                            "[%s] R=%d Id=%d Path='%s' BT='%s' 分类: W=%d P=%d T=%d J=%d R=%d E=%d L=%d 匹配=%d 原因=%s",
                            it.inventoryName.c_str(),
                            it.rarity, it.identified ? 1 : 0,
                            it.hasPath ? it.path.c_str() : "(空)",
                            it.hasBT ? it.baseType.c_str() : "(空)",
                            it.isWaystone ? 1 : 0,
                            it.isPrecursorTablet ? 1 : 0,
                            it.isTempleTablet ? 1 : 0,
                            it.isJewel ? 1 : 0,
                            it.isRune ? 1 : 0,
                            it.isEssence ? 1 : 0,
                            it.isLiquid ? 1 : 0,
                            it.matchesCurrentType ? 1 : 0,
                            it.matchReason.c_str());
                        m_debugLog.push_back(std::string(line));
                    }
                    
                    m_debugLog.push_back("\n--- POE2 数据匹配详情 ---");
                    m_debugLog.push_back("对照 baseitemtypes.json 验证每个物品的 Path/BaseType");
                    for (const auto& it : items) {
                        if (it.hasPath || it.hasBT) {
                            auto report = TabletReforgeGame::Poe2MatchReport(
                                it.path, it.baseType, it.rarity, it.identified, m_settings);
                            if (!report.empty()) {
                                char hdr[256];
                                ::sprintf_s(hdr, "\n  物品: %s", it.shortName.c_str());
                                m_debugLog.push_back(std::string(hdr));
                                m_debugLog.push_back(report);
                            }
                        }
                    }
                    
                    m_debugLog.push_back("\n--- 诊断建议 ---");
                    m_debugLog.push_back("1. 如果物品 Path 和 BaseType 都为空：SDK 未提供数据，无法精确识别");
                    m_debugLog.push_back("2. 如果有 Path 但未匹配：物品可能不在已知 POE2 三合一列表中");
                    m_debugLog.push_back("3. 如果匹配为 0 且原因显示'无数据'：尝试在游戏中 Ctrl+Click 物品查看详情");
                    m_debugLog.push_back("4. 查看 POE2 wiki 确认物品是否可三合一合成");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("📖 查看可合成物品列表")) {
                int count = 0;
                auto list = TabletReforgeGame::GetCraftableItemList(count);
                m_debugLog.push_back("");
                m_debugLog.push_back("=== POE2 可合成物品完整列表 ===");
                m_debugLog.push_back("数据来源: baseitemtypes.json");
                for (int i = 0; i < count; ++i) {
                    char line[512];
                    ::sprintf_s(line,
                        "  [%d] %s\n      描述: %s\n      识别: %s",
                        list[i].itemType,
                        list[i].displayName,
                        list[i].description,
                        list[i].pathPattern);
                    m_debugLog.push_back(std::string(line));
                }
            }

            ImGui::Separator();

            // === UI 元素搜索 ===
            ImGui::Text("=== UI 元素搜索（找重铸台面板）===");
            ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##UiSearch", "输入关键词搜索...", m_uiSearchBuffer, sizeof(m_uiSearchBuffer));
            ImGui::SameLine();
            if (ImGui::Button("搜索UI元素")) {
                SearchUiElements();
            }
            ImGui::SameLine();
            if (ImGui::Button("列出所有面板")) {
                ListAllPanels();
            }

            // === 中文面板扫描（宽字符支持）===
            ImGui::Separator();
            ImGui::Text("=== 中文面板扫描（识别中文UI标签）===");
            if (ImGui::Button("🔤 扫描所有UI文本（含中文）")) {
                ScanAllUiText();
            }
            ImGui::SameLine();
            if (ImGui::Button("📑 扫描仓库页Tab（中文标签）")) {
                ScanStashTabsWithChinese();
            }
            ImGui::SameLine();
            if (ImGui::Button("🏷️ 列出当前仓库页标签")) {
                DumpStashTabLabels();
            }

            ImGui::Separator();

            // === NPC 对话流程测试 ===
            ImGui::Text("=== NPC 对话流程测试（单独测试多利亚尼鉴定）===");
            if (ImGui::Button("🧙 测试 NPC 对话流程（步骤1:找NPC）")) {
                TestNpcDialogFlow(1);
            }
            ImGui::SameLine();
            if (ImGui::Button("📝 测试对话检测（步骤2:检测对话）")) {
                TestNpcDialogFlow(2);
            }
            ImGui::SameLine();
            if (ImGui::Button("🔍 测试鉴定按钮搜索（步骤3:找按钮）")) {
                TestNpcDialogFlow(3);
            }
            if (ImGui::Button("▶️ 一键完整测试 NPC 流程（1+2+3）")) {
                TestNpcDialogFlow(0);
            }

            ImGui::Separator();

            // === 重铸台按钮搜索测试 ===
            ImGui::Text("=== 重铸台 REFORGE 按钮测试 ===");
            if (ImGui::Button("🔍 测试 REFORGE 按钮搜索（无节点上限）")) {
                TestReforgeButtonSearch();
            }

            // === 强制刷新仓库 ===
            if (ImGui::Button("🔁 强制 Scan(-1) 刷新所有仓库")) {
                if (!ctx()) m_debugLog.push_back("[ERROR] ctx() 为空，不在游戏中");
                else {
                    m_debugLog.push_back("");
                    m_debugLog.push_back("=== 触发 Inventory.Scan(-1) 全仓库刷新 ===");
                    ctx()->Inventory.Scan(-1);
                    m_debugLog.push_back("[OK] Scan(-1) 已触发");
                }
            }

            ImGui::Separator();

            ImGui::BeginChild("LogScroll", ImVec2(0, 0), true);
            for (const auto& line : m_debugLog) {
                if (line.find("[PASS]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[FAIL]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[ERROR]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[WARN]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[UI]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[BAG_LIKE]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[STASH_LIKE]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "%s", line.c_str());
                } else if (line.find("[BENCH_LIKE]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.8f, 0.0f, 1.0f, 1.0f), "%s", line.c_str());
                } else {
                    ImGui::Text("%s", line.c_str());
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    void TestNpcDialogFlow(int step) {
        if (!ctx()) {
            m_debugLog.push_back("[ERROR] ctx() 为空，不在游戏中");
            return;
        }

        m_debugLog.push_back("");
        m_debugLog.push_back("=== NPC 对话流程测试 ===");

        // 步骤 1：查找 NPC 实体
        if (step == 0 || step == 1) {
            m_debugLog.push_back("[步骤1] 查找 NPC 实体...");
            auto npc = TabletReforgeGame::FindNPC(ctx(), m_calib);
            if (npc) {
                char line[512];
                ::sprintf_s(line,
                    "[OK] 找到 NPC！世界坐标=(%.1f,%.1f,%.1f)  屏幕=(%.0f,%.0f)  在屏=%d",
                    npc->worldX, npc->worldY, npc->worldZ,
                    npc->screenX, npc->screenY, npc->onScreen ? 1 : 0);
                m_debugLog.push_back(std::string(line));

                // 输出 NPC Path 验证
                char pathInfo[256];
                ::sprintf_s(pathInfo,
                    "[NPC] Config Path='%s'  EntityAt=0x%llX  OnScreen=%d",
                    m_calib.npcEntityPath.empty() ? "(empty)" : m_calib.npcEntityPath.c_str(),
                    (unsigned long long)npc->entityAddress,
                    npc->onScreen ? 1 : 0);
                m_debugLog.push_back(std::string(pathInfo));
            } else {
                m_debugLog.push_back("[FAIL] 未找到 NPC！请检查标定中的 npcEntityPath");
                m_debugLog.push_back("       提示：默认路径为 Metadata/NPC/Hideout/Doryani");
            }
        }

        // 步骤 2：检测对话面板
        if (step == 0 || step == 2) {
            m_debugLog.push_back("[步骤2] 检测 NPC 对话面板...");
            bool dialogOpen = TabletReforgeGame::IsNpcDialogOpen(ctx(), m_calib);
            if (dialogOpen) {
                m_debugLog.push_back("[OK] NPC 对话面板已打开！");

                // 输出配置的 StringId 用于调试
                char line[256];
                ::sprintf_s(line,
                    "[Dialog] StringId='%s'",
                    m_calib.npcDialogStringId.empty() ? "(empty)" : m_calib.npcDialogStringId.c_str());
                m_debugLog.push_back(std::string(line));
            } else {
                m_debugLog.push_back("[FAIL] 对话面板未打开！");
                m_debugLog.push_back("       请与 NPC 互动打开对话后再测试");

                // 调试：显示当前所有可见面板
                m_debugLog.push_back("[调试] 搜索 UI 根节点下的面板...");
                uintptr_t root = ctx()->Ui.GetGameUiRoot();
                if (root) {
                    // 搜索包含 npc/dialog/doryani 等关键词的节点
                    std::vector<std::string> dialogKw = {
                        "doryani", "多利亚尼", "dialog", "对话", "npc"
                    };
                    uintptr_t found = TabletReforgeGame::FindUiNodeByText(ctx(), root, dialogKw, 0, 12);
                    if (found) {
                        m_debugLog.push_back("[调试] 找到可能的对话相关节点 (通过文字匹配)");
                        std::string text = ctx()->Ui.GetText(found);
                        if (!text.empty()) {
                            m_debugLog.push_back("       节点文字: " + text);
                        }
                        float x, y, w, h;
                        if (ctx()->Ui.ComputeScreenRect(found, x, y, w, h)) {
                            char rect[256];
                            ::sprintf_s(rect, "       位置: (%.0f,%.0f) 大小: %.0fx%.0f", x, y, w, h);
                            m_debugLog.push_back(std::string(rect));
                        }
                    } else {
                        m_debugLog.push_back("[调试] 未找到对话相关节点");
                    }
                }
            }
        }

        // 步骤 3：搜索鉴定按钮
        if (step == 0 || step == 3) {
            m_debugLog.push_back("[步骤3] 搜索鉴定/Identify 按钮...");
            auto btn = TabletReforgeGame::ResolveIdentifyButton(ctx(), m_calib);
            if (btn.valid) {
                char line[512];
                ::sprintf_s(line,
                    "[OK] 找到鉴定按钮！坐标=(%d, %d)",
                    btn.x, btn.y);
                m_debugLog.push_back(std::string(line));

                // 显示配置详情
                char configInfo[512];
                ::sprintf_s(configInfo,
                    "[Button] 实时扫描模式: StringId='%s'",
                    m_calib.identifyButtonStringId.empty() ? "(empty, 使用文字搜索)" : m_calib.identifyButtonStringId.c_str());
                m_debugLog.push_back(std::string(configInfo));
            } else {
                m_debugLog.push_back("[FAIL] 实时扫描未找到鉴定按钮！");
                m_debugLog.push_back("       请打开 NPC 对话后再测试");
                m_debugLog.push_back("       或在标定向导中配置 identifyButtonStringId");

                // 调试：搜索包含鉴定关键词的 UI 节点
                m_debugLog.push_back("[调试] 搜索鉴定关键词节点...");
                uintptr_t root = ctx()->Ui.GetGameUiRoot();
                if (root) {
                    std::vector<std::string> identifyKw = {
                        "鑑定", "鉴定", "identify", "鑑定物品", "鉴定物品"
                    };
                    for (const auto& kw : identifyKw) {
                        std::vector<std::string> singleKw = {kw};
                        uintptr_t found = TabletReforgeGame::FindUiNodeByText(ctx(), root, singleKw, 0, 15);
                        if (found) {
                            std::string text = ctx()->Ui.GetText(found);
                            char line[512];
                            ::sprintf_s(line,
                                "[调试] 找到含 '%s' 的节点！文字='%s'",
                                kw.c_str(), text.empty() ? "(空)" : text.c_str());
                            m_debugLog.push_back(std::string(line));

                            float x, y, w, h;
                            if (ctx()->Ui.ComputeScreenRect(found, x, y, w, h)) {
                                char rect[256];
                                ::sprintf_s(rect, "       位置: (%.0f,%.0f) 大小: %.0fx%.0f", x, y, w, h);
                                m_debugLog.push_back(std::string(rect));
                            }
                        } else {
                            m_debugLog.push_back("[调试] 未找到含 '" + kw + "' 的节点");
                        }
                    }
                }
            }
        }

        m_debugLog.push_back("=== 测试完成 ===");
    }

    void TestReforgeButtonSearch() {
        if (!ctx()) {
            m_debugLog.push_back("[ERROR] ctx() 为空，不在游戏中");
            return;
        }

        m_debugLog.push_back("");
        m_debugLog.push_back("=== 重铸台 REFORGE 按钮搜索测试 ===");
        m_debugLog.push_back("模式：无节点上限全量扫描，深度20");

        uintptr_t root = ctx()->Ui.GetGameUiRoot();
        if (!root) {
            m_debugLog.push_back("[ERROR] 找不到 GameUI 根节点");
            return;
        }

        // 逐个关键词搜索，不使用 CollectVisible 限制
        std::vector<std::string> reforgeKeywords = {
            "reforge", "重铸", "reforging bench", "重铸台",
            "reforging", "合成", "combine", "bench"
        };

        for (const auto& kw : reforgeKeywords) {
            std::vector<std::string> singleKw = {kw};
            m_debugLog.push_back("");
            m_debugLog.push_back("[搜索] 关键词: '" + kw + "'");

            // 用 FindUiNodeByText 搜索（深度20，无节点上限）
            uintptr_t found = TabletReforgeGame::FindUiNodeByText(ctx(), root, singleKw, 0, 20);
            if (found) {
                std::string text = ctx()->Ui.GetText(found);
                float x, y, w, h;
                bool hasRect = ctx()->Ui.ComputeScreenRect(found, x, y, w, h);

                char line[512];
                ::sprintf_s(line,
                    "  [OK] 找到匹配节点！地址=0x%llX",
                    (unsigned long long)found);
                m_debugLog.push_back(std::string(line));

                if (!text.empty()) {
                    m_debugLog.push_back("  文字内容: '" + text + "'");
                } else {
                    m_debugLog.push_back("  文字内容: (空)");
                }

                if (hasRect) {
                    char rect[256];
                    ::sprintf_s(rect, "  屏幕位置: (%.0f,%.0f)  尺寸: %.0f x %.0f", x, y, w, h);
                    m_debugLog.push_back(std::string(rect));

                    // 计算中心点坐标（这就是插件要点击的位置）
                    int cx = static_cast<int>(x + w * 0.5f);
                    int cy = static_cast<int>(y + h * 0.5f);
                    char center[256];
                    ::sprintf_s(center, "  中心坐标: (%d, %d)  ← 这就是要点击的位置", cx, cy);
                    m_debugLog.push_back(std::string(center));
                } else {
                    m_debugLog.push_back("  [WARN] 无有效屏幕坐标（可能是隐藏节点）");
                }

                // 检测父节点信息
                std::string parentText = "(无子节点信息)";
                auto children = ctx()->Ui.GetChildren(found);
                char childInfo[256];
                ::sprintf_s(childInfo, "  子节点数: %d", (int)children.size());
                m_debugLog.push_back(std::string(childInfo));

                // 只取第一个匹配（避免重复），继续搜索其他关键词
            } else {
                m_debugLog.push_back("  [未找到] 无匹配节点");
            }
        }

        // 用 ResolveCombineButton 做最终验证
        m_debugLog.push_back("");
        m_debugLog.push_back("=== ResolveCombineButton 最终解析结果 ===");
        auto btn = TabletReforgeGame::ResolveCombineButton(ctx(), m_calib);
        if (btn.valid) {
            char line[256];
            ::sprintf_s(line,
                "[OK] ResolveCombineButton 成功！坐标=(%d, %d)",
                btn.x, btn.y);
            m_debugLog.push_back(std::string(line));
        } else {
            m_debugLog.push_back("[FAIL] ResolveCombineButton 未能解析出有效坐标");
            m_debugLog.push_back("       可能原因：重铸台面板未打开，或按钮文字不在搜索范围内");
        }

        // 输出当前配置
        m_debugLog.push_back("");
        m_debugLog.push_back("=== 当前标定配置 ===");
        char configInfo[1024];
        ::sprintf_s(configInfo,
            "  useManualCoords=%d\n"
            "  combineButtonStringId='%s'\n"
            "  combineButtonX=%d  combineButtonY=%d\n"
            "  outputSlotStringId='%s'\n"
            "  outputSlotX=%d  outputSlotY=%d",
            m_calib.useManualCoords ? 1 : 0,
            m_calib.combineButtonStringId.empty() ? "(empty)" : m_calib.combineButtonStringId.c_str(),
            m_calib.combineButtonX, m_calib.combineButtonY,
            m_calib.outputSlotStringId.empty() ? "(empty)" : m_calib.outputSlotStringId.c_str(),
            m_calib.outputSlotX, m_calib.outputSlotY);
        m_debugLog.push_back(std::string(configInfo));

        m_debugLog.push_back("=== REFORGE 按钮测试完成 ===");
    }

    void SearchUiElements() {
        if (!ctx() || std::strlen(m_uiSearchBuffer) == 0) return;
        
        m_debugLog.push_back("");
        m_debugLog.push_back("=== UI 元素搜索结果 ===");
        
        std::string query(m_uiSearchBuffer);
        std::transform(query.begin(), query.end(), query.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        uintptr_t root = ctx()->Ui.GetGameUiRoot();
        if (root == 0) {
            m_debugLog.push_back("[ERROR] 找不到 GameUI 根节点");
            return;
        }

        std::vector<std::pair<uintptr_t, std::string>> results;
        SearchUiTree(root, query, 0, 15, results);

        if (results.empty()) {
            m_debugLog.push_back("[UI] 未找到匹配的元素");
        } else {
            m_debugLog.push_back("[UI] 找到 " + std::to_string(results.size()) + " 个匹配元素:");
            for (auto& [addr, name] : results) {
                m_debugLog.push_back("[UI] 0x" + std::to_string(addr) + " - " + name);
            }
        }
    }

    void ListAllPanels() {
        if (!ctx()) return;
        
        m_debugLog.push_back("");
        m_debugLog.push_back("=== 所有可见面板 ===");
        
        uintptr_t root = ctx()->Ui.GetGameUiRoot();
        if (root == 0) {
            m_debugLog.push_back("[ERROR] 找不到 GameUI 根节点");
            return;
        }

        std::vector<std::pair<uintptr_t, std::string>> panels;
        FindPanels(root, 0, 15, panels);

        if (panels.empty()) {
            m_debugLog.push_back("[UI] 未找到任何面板");
        } else {
            m_debugLog.push_back("[UI] 找到 " + std::to_string(panels.size()) + " 个面板:");
            for (auto& [addr, name] : panels) {
                m_debugLog.push_back("[UI] " + name);
            }
        }
    }

    // === 中文文本扫描：收集UI树中所有可见节点的文本 ===
    // 使用 GetText 获取节点显示文字，包括中文等多字节UTF-8字符
    void ScanAllUiText() {
        if (!ctx()) {
            m_debugLog.push_back("[ERROR] ctx() 为空，不在游戏中");
            return;
        }

        m_debugLog.push_back("");
        m_debugLog.push_back("=== UI树全文本扫描（含中文）===");
        m_debugLog.push_back("扫描所有可见UI节点的文本内容...");

        uintptr_t root = ctx()->Ui.GetGameUiRoot();
        if (root == 0) {
            m_debugLog.push_back("[ERROR] 找不到 GameUI 根节点");
            return;
        }

        // 递归扫描所有节点
        struct ScanResult {
            uintptr_t addr;
            std::string stringId;
            std::string text;
            float x, y, w, h;
            bool hasChinese;
        };

        std::vector<ScanResult> results;
        int depthLimit = 12;

        std::function<void(uintptr_t, int)> scanNode = [&](uintptr_t addr, int depth) {
            if (depth > depthLimit || addr == 0) return;
            if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return;

            // 读取节点信息
            std::string stringId = ctx()->Ui.GetStringId(addr);
            std::string text = ctx()->Ui.GetText(addr);
            bool visible = ctx()->Ui.IsVisible(addr);

            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            bool hasRect = ctx()->Ui.ComputeScreenRect(addr, x, y, w, h);

            // 检测是否包含中文字符（UTF-8: 0xE0-0xEF开头的3字节序列）
            bool hasChinese = false;
            for (size_t i = 0; i < text.size(); ++i) {
                unsigned char c = static_cast<unsigned char>(text[i]);
                if (c >= 0xE0 && c <= 0xEF) {
                    if (i + 2 < text.size()) {
                        unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
                        unsigned char c3 = static_cast<unsigned char>(text[i + 2]);
                        if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
                            hasChinese = true;
                            break;
                        }
                    }
                }
            }

            // 记录有意义的节点（有文本或stringId且可见）
            if (visible && hasRect && w > 3.f && h > 3.f) {
                if (!text.empty() || !stringId.empty()) {
                    ScanResult r;
                    r.addr = addr;
                    r.stringId = stringId;
                    r.text = text;
                    r.x = x; r.y = y; r.w = w; r.h = h;
                    r.hasChinese = hasChinese;
                    results.push_back(r);
                }
            }

            // 递归子节点
            auto kids = ctx()->Ui.GetChildren(addr);
            for (uintptr_t child : kids) {
                scanNode(child, depth + 1);
            }
        };

        scanNode(root, 0);

        // 输出结果
        int chineseCount = 0;
        int totalWithText = 0;
        for (const auto& r : results) {
            if (r.hasChinese) chineseCount++;
            if (!r.text.empty()) totalWithText++;
        }

        char summary[256];
        sprintf_s(summary,
            "[汇总] 总节点=%zu  有文本=%d  含中文=%d",
            results.size(), totalWithText, chineseCount);
        m_debugLog.push_back(std::string(summary));

        m_debugLog.push_back("\n--- 含中文的节点 ---");
        for (const auto& r : results) {
            if (r.hasChinese) {
                char line[1024];
                sprintf_s(line,
                    "  [addr=0x%llX] text='%s' stringId='%s' pos=(%.0f,%.0f) size=(%.0f,%.0f)",
                    (unsigned long long)r.addr,
                    r.text.c_str(),
                    r.stringId.empty() ? "(none)" : r.stringId.c_str(),
                    r.x, r.y, r.w, r.h);
                m_debugLog.push_back(std::string(line));
            }
        }

        m_debugLog.push_back("\n--- 所有有文本的节点（前100个）---");
        int outputCount = 0;
        for (const auto& r : results) {
            if (!r.text.empty() && outputCount < 100) {
                char line[512];
                sprintf_s(line,
                    "  [%s%s] '%s' pos=(%.0f,%.0f)",
                    r.hasChinese ? "[CN] " : "     ",
                    r.stringId.empty() ? "(no-id)" : r.stringId.c_str(),
                    r.text.c_str(),
                    r.x, r.y);
                m_debugLog.push_back(std::string(line));
                outputCount++;
            }
        }
    }

    // === 扫描仓库页Tab按钮（含中文标签）===
    void ScanStashTabsWithChinese() {
        if (!ctx()) {
            m_debugLog.push_back("[ERROR] ctx() 为空");
            return;
        }

        m_debugLog.push_back("");
        m_debugLog.push_back("=== 仓库页Tab按钮扫描（含中文标签）===");

        // 使用StashOps的ExtractStashTabButtons
        auto buttons = TabletReforgeGame::ExtractStashTabButtons(ctx());

        if (buttons.empty()) {
            m_debugLog.push_back("[WARN] 未识别到任何Tab按钮，尝试全树扫描...");
        } else {
            char summary[256];
            sprintf_s(summary, "[OK] 识别到 %zu 个Tab按钮", buttons.size());
            m_debugLog.push_back(std::string(summary));

            int idx = 0;
            for (const auto& btn : buttons) {
                char line[512];
                sprintf_s(line,
                    "  #%d: label='%s' stringId='%s' pos=(%.0f,%.0f) size=(%.0f,%.0f) subTab=%d",
                    idx,
                    btn.label.c_str(),
                    btn.stringId.empty() ? "(none)" : btn.stringId.c_str(),
                    btn.x, btn.y, btn.w, btn.h,
                    btn.isSubTab ? 1 : 0);
                m_debugLog.push_back(std::string(line));
                idx++;
            }
        }

        // 同时输出按编号排列的仓库页映射
        m_debugLog.push_back("\n--- 编号仓库页映射（ListAllStashTabsOrdered）---");
        auto orderedTabs = TabletReforgeGame::ListAllStashTabsOrdered(ctx());
        if (!orderedTabs.empty()) {
            for (const auto& tab : orderedTabs) {
                char line[512];
                sprintf_s(line,
                    "  #%d: invId=%d name='%s' type=%s slots=%d visible=%d click=(%.0f,%.0f) sub=%d",
                    tab.slotIndex, tab.inventoryId, tab.name.c_str(),
                    TabletReforgeGame::StashTabTypeName(tab.type), tab.slots,
                    tab.isVisible ? 1 : 0, tab.clickX, tab.clickY,
                    tab.isSubTab ? 1 : 0);
                m_debugLog.push_back(std::string(line));
            }
        } else {
            m_debugLog.push_back("[WARN] 未获取到有序仓库页列表");
        }
    }

    // === 列出当前所有仓库页标签 ===
    void DumpStashTabLabels() {
        if (!ctx()) {
            m_debugLog.push_back("[ERROR] ctx() 为空");
            return;
        }

        m_debugLog.push_back("");
        m_debugLog.push_back("=== 当前仓库页标签（Inventory.GetName）===");

        auto mainInv = TabletReforgeGame::FindMainInventory(ctx());
        float displayW = 0.f, displayH = 0.f;
        TabletReforgeGame::GetScreenSize(ctx(), displayW, displayH);

        int count = 0;
        int filteredCount = 0;
        for (const auto& inv : ctx()->Inventory.GetAll()) {
            const char* nameC = ctx()->Inventory.GetName(inv.InventoryId);
            std::string name = nameC ? nameC : "(null)";
            int slots = inv.TotalBoxesX * inv.TotalBoxesY;

            bool isMainInv = mainInv && inv.InventoryId == mainInv->InventoryId;

            // 综合过滤：装备槽位 + 非仓库Tab的 Inventory_NNN（基于 ggpk 格子尺寸数据）
            // 不依赖名称中的 "Inventory" 字符串，而是通过 ggpk 解包的仓库Tab格子规格识别。
            if (!isMainInv && TabletReforgeGame::IsNonStashInventory(name, inv.TotalBoxesX, inv.TotalBoxesY, inv.InventoryId)) {
                ++filteredCount;
                continue;
            }

            bool onScreen = inv.Grid.Valid && TabletReforgeGame::GridOnScreen(inv, displayW, displayH);

            char line[512];
            sprintf_s(line,
                "  [%c] ID=%04d name='%s' %dx%d=%d格  Items=%zu  GridValid=%d  OnScreen=%d%s",
                isMainInv ? 'M' : ' ',
                inv.InventoryId, name.c_str(),
                inv.TotalBoxesX, inv.TotalBoxesY, slots,
                inv.Items.size(),
                inv.Grid.Valid ? 1 : 0,
                onScreen ? 1 : 0,
                isMainInv ? " (MAIN)" : "");
            m_debugLog.push_back(std::string(line));
            count++;
        }

        char summary[256];
        sprintf_s(summary, "[汇总] 仓库页数=%d  已过滤非仓库Tab=%d", count, filteredCount);
        m_debugLog.push_back(std::string(summary));
    }

    // 诊断核心：列出所有 Inventory（背包、仓库、合成面板等）+ 物品明细
    // 用于验证：SDK 到底能不能扫描到背包和仓库中的每一个物品？
    void DumpAllInventories(bool withItems) {
        if (!ctx()) {
            m_debugLog.push_back("[ERROR] ctx() 为空，不在游戏中");
            return;
        }

        m_debugLog.push_back("");
        m_debugLog.push_back(withItems
            ? "=== 所有 Inventory + 物品明细（完整 Dump）==="
            : "=== 所有 Inventory（不带物品明细）===");

        const auto& all = ctx()->Inventory.GetAll();
        m_debugLog.push_back("[INFO] Inventory.GetAll() 返回总数: " + std::to_string(all.size()));

        int totalItems = 0;
        int mainBagItems = 0;
        int stashItems = 0;
        int smallInvItems = 0;

        for (const auto& inv : all) {
            const char* nameC = ctx()->Inventory.GetName(inv.InventoryId);
            std::string name = nameC ? nameC : "(null)";

            int slots = inv.TotalBoxesX * inv.TotalBoxesY;
            int itemCount = static_cast<int>(inv.Items.size());
            totalItems += itemCount;

            // 分类
            const bool isMainBag   = (name.rfind("MainInventory", 0) == 0);
            const bool isBigStash  = (slots >= 40 && !isMainBag);
            const bool isSmallInv  = (slots >= 4 && slots <= 24 && !isMainBag);
            if (isMainBag)  mainBagItems += itemCount;
            if (isBigStash) stashItems   += itemCount;
            if (isSmallInv) smallInvItems += itemCount;

            char line[512];
            ::sprintf_s(line,
                "[ID=%04d] %-34s Grid.Valid=%d %3dx%-3d=%3d格  Items=%-3d  GridOnScreen=%s",
                inv.InventoryId,
                name.c_str(),
                inv.Grid.Valid ? 1 : 0,
                inv.TotalBoxesX, inv.TotalBoxesY, slots,
                itemCount,
                (inv.Grid.Valid && inv.Grid.GridScreenX > 0 && inv.Grid.GridScreenY > 0) ? "YES" : "no");
            m_debugLog.push_back(std::string(line));

            // 物品明细（按需开启）
            if (withItems) {
                for (const auto& item : inv.Items) {
                    const int w = item.Width > 0 ? item.Width : 1;
                    const int h = item.Height > 0 ? item.Height : 1;
                    const char* baseType = item.BaseTypeName.empty() ? "(no basetype)" : item.BaseTypeName.c_str();
                    std::string rarityTag;
                    switch (item.Rarity) {
                        case 0: rarityTag = "Normal";   break;
                        case 1: rarityTag = "Magic";    break;
                        case 2: rarityTag = "Rare";     break;
                        case 3: rarityTag = "Unique";   break;
                        default: rarityTag = "R" + std::to_string(item.Rarity); break;
                    }
                    // 识别标签：命中哪个三合一类别
                    const std::string tag = TabletReforgeGame::DebugItemTypeTag(item.Path, item.BaseTypeName);
                    // 当前 settings 下是否会被判定为原料
                    bool asMat = TabletReforgeGame::MatchesDesiredReforgeTypeEx(
                        item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, m_settings);
                    bool asProd = TabletReforgeGame::MatchesDesiredProductTypeEx(item.Path, item.BaseTypeName, item.Rarity, item.IsIdentified, m_settings);
                    char flag[16] = "";
                    if (asMat || asProd) {
                        int p = 0;
                        if (asMat) flag[p++] = 'M';
                        if (asProd) flag[p++] = 'P';
                        flag[p] = 0;
                    }

                    char itemline[1024];
                    ::sprintf_s(itemline,
                        "   -> Slot(%d,%d) %dx%d  R%-2d[%6s] Id:%d %3s %-14s Addr=0x%llX  Screen=%.0f,%.0f %dx%d  %s",
                        item.SlotX, item.SlotY, w, h,
                        item.Rarity, rarityTag.c_str(),
                        item.IsIdentified ? 1 : 0,
                        flag,
                        tag.c_str(),
                        (unsigned long long)item.Address,
                        item.ScreenX, item.ScreenY, (int)item.ScreenW, (int)item.ScreenH,
                        baseType);
                    m_debugLog.push_back(std::string(itemline));
                    // 也打印 Path（截取最后一段，更短）
                    if (!item.Path.empty()) {
                        size_t slash = item.Path.rfind('/');
                        std::string shortPath = (slash == std::string::npos) ? item.Path : item.Path.substr(slash + 1);
                        m_debugLog.push_back("      path: " + shortPath);
                    }
                }
                if (itemCount == 0) {
                    m_debugLog.push_back("   (空)");
                }
            }
        }

        // 汇总
        m_debugLog.push_back("---");
        char summary[256];
        ::sprintf_s(summary,
            "[汇总] Inventory总数=%d   总物品数=%d   主背包物品=%d   大仓库页物品=%d   小面板(4-24格)物品=%d",
            (int)all.size(), totalItems, mainBagItems, stashItems, smallInvItems);
        m_debugLog.push_back(std::string(summary));
        m_debugLog.push_back("[提示] 先点『强制 Scan(-1) 刷新所有仓库』再点『列出 Inventory』可获取最新结果");
    }

    void SearchUiTree(uintptr_t addr, const std::string& query, int depth, int maxDepth,
                      std::vector<std::pair<uintptr_t, std::string>>& results) {
        if (depth > maxDepth || addr == 0 || results.size() >= 50) return;
        if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return;

        std::string stringId = ctx()->Ui.GetStringId(addr);
        if (!stringId.empty()) {
            std::string lower = stringId;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find(query) != std::string::npos) {
                results.emplace_back(addr, stringId);
            }
        }

        auto kids = ctx()->Ui.GetChildren(addr);
        for (uintptr_t childAddr : kids) {
            if (results.size() >= 50) break;
            SearchUiTree(childAddr, query, depth + 1, maxDepth, results);
        }
    }

    void FindPanels(uintptr_t addr, int depth, int maxDepth,
                    std::vector<std::pair<uintptr_t, std::string>>& results) {
        if (depth > maxDepth || addr == 0 || results.size() >= 100) return;
        if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFull) return;

        std::string stringId = ctx()->Ui.GetStringId(addr);
        if (!stringId.empty()) {
            bool isVisible = ctx()->Ui.IsVisible(addr);
            PluginSDK::UiElement elem = ctx()->Ui.Read(addr);
            float w, h;
            bool hasSize = ctx()->Ui.ComputeScreenRect(addr, w, h, w, h) && w > 50 && h > 50;
            
            if (isVisible && hasSize) {
                results.emplace_back(addr, stringId);
            }
        }

        auto kids = ctx()->Ui.GetChildren(addr);
        for (uintptr_t childAddr : kids) {
            if (results.size() >= 100) break;
            FindPanels(childAddr, depth + 1, maxDepth, results);
        }
    }

    void ExportStateMachineLog() {
        auto logs = m_stateMachine.GetRecentLogs(256);
        if (logs.empty()) {
            m_debugLog.push_back("错误: 状态机日志为空");
            return;
        }

        try {
            auto logDir = DirectoryPath() / "logs";
            std::filesystem::create_directories(logDir);

            SYSTEMTIME st;
            ::GetLocalTime(&st);
            char timestamp[32];
            ::sprintf_s(timestamp, "%04d%02d%02d_%02d%02d%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);

            auto logPath = logDir / ("statemachine_log_" + std::string(timestamp) + ".txt");

            std::ofstream out(logPath);
            if (out.is_open()) {
                out << "=== 状态机运行日志 ===" << std::endl;
                out << "导出时间: " << timestamp << std::endl;
                out << std::endl;
                for (const auto& entry : logs) {
                    out << "[" << entry.timestampMs << "ms] ";
                    if (entry.severity == 2) {
                        out << "[ERROR] ";
                    } else if (entry.severity == 1) {
                        out << "[WARN] ";
                    }
                    out << entry.message << std::endl;
                }
                out.close();

                m_debugLog.push_back("状态机日志已导出: " + logPath.string());
            } else {
                m_debugLog.push_back("错误: 无法打开文件进行写入");
            }
        } catch (const std::exception& e) {
            m_debugLog.push_back("导出失败: " + std::string(e.what()));
        }
    }

    void ExportDebugLog() {
        if (m_debugLog.empty()) {
            m_debugLog.push_back("错误: 日志为空，无法导出");
            return;
        }

        try {
            auto logDir = DirectoryPath() / "logs";
            std::filesystem::create_directories(logDir);

            SYSTEMTIME st;
            ::GetLocalTime(&st);
            char timestamp[32];
            ::sprintf_s(timestamp, "%04d%02d%02d_%02d%02d%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);

            auto logPath = logDir / ("test_log_" + std::string(timestamp) + ".txt");

            std::ofstream out(logPath);
            if (out.is_open()) {
                out << "=== 状态机 Mock 测试日志 ===" << std::endl;
                out << "导出时间: " << timestamp << std::endl;
                out << std::endl;
                for (const auto& line : m_debugLog) {
                    out << line << std::endl;
                }
                out.close();

                m_debugLog.push_back("日志已导出: " + logPath.string());
            } else {
                m_debugLog.push_back("错误: 无法打开文件进行写入");
            }
        } catch (const std::exception& e) {
            m_debugLog.push_back("导出失败: " + std::string(e.what()));
        }
    }

    void SaveSettings() override {
        m_settings.Save(DirectoryPath());
        m_calib.Save(DirectoryPath());
    }

private:
    TabletReforgeConfig::Settings m_settings;
    TabletReforgeConfig::CalibData m_calib;
    TabletReforgeFlow::StateMachine m_stateMachine;
    TabletReforgeUi::CalibWizardState m_calibWizard{};
    PluginSDK::EventsService::Token m_frameToken{};

    // 热键状态（检测按下边沿，不是持续按住）
    bool m_toggleKeyWasDown = false;
    bool m_f7WasDown = false;
    bool m_f8WasDown = false;

    // 调试工具状态
    bool m_debugWindowOpen = false;
    std::vector<std::string> m_debugLog;
    char m_uiSearchBuffer[256] = {0};

    // 中文字体支持（用于显示 POE2 中文 StringId 和 UI 文本）
    bool m_chineseFontLoaded = false;
    ImFont* m_chineseFont = nullptr;
    std::vector<char> m_chineseFontData;

    // 把插件的配置同步到状态机（状态机有自己的副本）
    void SyncConfigToStateMachine() {
        m_stateMachine.settings = m_settings;
        m_stateMachine.calib = m_calib;
    }

    void OnFrameTick() {
        // —— F7 热键（双用途：标定捕获 / 手动取存碑牌）——
        const bool f7Down = TabletReforgeInput::IsKeyDown(VK_F7);
        if (f7Down && !m_f7WasDown) {
            if (m_calibWizard.captureTarget != TabletReforgeUi::CaptureTarget::None) {
                // 标定向导捕获模式：写入坐标
                POINT pt;
                if (::GetCursorPos(&pt)) {
                    if (m_calibWizard.captureTarget == TabletReforgeUi::CaptureTarget::CombineButton) {
                        m_calib.combineButtonX = pt.x;
                        m_calib.combineButtonY = pt.y;
                    } else if (m_calibWizard.captureTarget == TabletReforgeUi::CaptureTarget::OutputSlot) {
                        m_calib.outputSlotX = pt.x;
                        m_calib.outputSlotY = pt.y;
                    } else if (m_calibWizard.captureTarget == TabletReforgeUi::CaptureTarget::IdentifyButton) {
                        // 鉴定按钮已改为实时扫描模式，不再需要坐标捕获
                        m_debugLog.push_back("[INFO] 鉴定按钮已切换为实时扫描模式，无需坐标捕获");
                        m_debugLog.push_back("       请在标定向导中扫描 UI 节点获取 StringId（可选）");
                    }
                    m_calibWizard.captureTarget = TabletReforgeUi::CaptureTarget::None;
                }
            } else if (m_settings.enabled) {
                // 手动模式：在鼠标位置执行 Ctrl+右键（快速移动碑牌到另一面板）
                // 用法：鼠标移到仓库碑牌上按 F7 → 取到背包
                //       鼠标移到背包碑牌上按 F7 → 存回仓库
                POINT pt;
                if (::GetCursorPos(&pt)) {
                    TabletReforgeInput::CtrlRightClickScreen(pt.x, pt.y);
                }
            }
        }
        m_f7WasDown = f7Down;

        // —— F8 热键（切换调试窗口）——
        const bool f8Down = TabletReforgeInput::IsKeyDown(VK_F8);
        if (f8Down && !m_f8WasDown) {
            m_debugWindowOpen = !m_debugWindowOpen;
        }
        m_f8WasDown = f8Down;

        // —— 启停热键 ——
        const bool keyDown = TabletReforgeInput::IsKeyDown(m_settings.toggleKey);
        if (keyDown && !m_toggleKeyWasDown) {
            if (m_stateMachine.IsRunning()) {
                m_stateMachine.Stop();
            } else if (m_settings.enabled) {
                SyncConfigToStateMachine();  // 启动前同步最新配置
                m_stateMachine.Start(ctx());
            }
        }
        m_toggleKeyWasDown = keyDown;

        // —— 驱动状态机 ——
        if (m_stateMachine.IsRunning()) {
            m_stateMachine.Tick(ctx());
        }
    }
};

// —— 插件工厂（DLL 导出）——
extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() {
    return new TabletReforgePlugin();
}

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) {
    delete p;
}
