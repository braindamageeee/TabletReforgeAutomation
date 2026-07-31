// MockStashTabTest.cpp — 仓库页配置与识别逻辑的Mock测试
// 编译: cl /EHsc /std:c++20 /I..\.. /I..\..\sdk /I..\..\imgui /I..\..\third_party /Fe:MockStashTabTest.exe MockStashTabTest.cpp
// 用途: 不依赖游戏运行时，验证 StashTabConfig、角色逻辑、序列化等核心代码

#include "../config/Settings.h"
#include "../game/StashOps.h"
#include "../game/VisionRecognizer.h"
#include "../game/StashTypeTable.h"
#include "../game/StashItemMapper.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace TabletReforgeConfig;

// 测试辅助
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  TEST: %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); g_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); g_failed++; } while(0)
#define CHECK(cond, msg) do { if (cond) { PASS(); } else { FAIL(msg); } } while(0)

// === 测试1: StashTabConfig 角色逻辑 ===
void test_role_logic() {
    printf("\n=== 测试1: 仓库页角色逻辑 ===\n");
    
    StashTabConfig cfg;
    cfg.inventoryId = 100;
    cfg.role = StashTabRole::Material;
    cfg.enabled = true;
    CHECK(cfg.IsMaterial(), "Material角色应返回true");
    CHECK(!cfg.IsSpecial(), "Material角色不应是Special");
    
    cfg.enabled = false;
    CHECK(!cfg.IsMaterial(), "禁用的Material应返回false");
    
    cfg.role = StashTabRole::Special;
    cfg.enabled = true;
    CHECK(cfg.IsSpecial(), "Special角色应返回true");
    CHECK(!cfg.IsMaterial(), "Special角色不应是Material");
    
    cfg.role = StashTabRole::Ignore;
    CHECK(!cfg.IsMaterial(), "Ignore角色不应是Material");
    CHECK(!cfg.IsSpecial(), "Ignore角色不应是Special");
    
    cfg.role = StashTabRole::None;
    CHECK(!cfg.IsMaterial(), "None角色不应是Material");
}

// === 测试2: StashTabConfig 序列化/反序列化 ===
void test_serialization() {
    printf("\n=== 测试2: 序列化/反序列化 ===\n");
    
    StashTabConfig cfg;
    cfg.inventoryId = 42;
    cfg.name = "Test Stash";
    cfg.role = StashTabRole::Material;
    cfg.enabled = true;
    cfg.detectedText = "Fragment";
    cfg.clickX = 120.5f;
    cfg.clickY = 340.2f;
    cfg.keywordHints.insert("fragment");
    cfg.keywordHints.insert("tablet");
    
    // 添加子页
    StashTabConfig sub1;
    sub1.inventoryId = 43;
    sub1.name = "Sub Page 1";
    sub1.role = StashTabRole::Material;
    sub1.enabled = true;
    cfg.subTabs.push_back(sub1);
    
    // 序列化
    auto json = cfg.ToJson();
    CHECK(!json.is_null(), "序列化结果不为空");
    CHECK(json.contains("inventory_id"), "包含inventory_id");
    CHECK(json["inventory_id"].get<int>() == 42, "inventory_id正确");
    CHECK(json["name"].get<std::string>() == "Test Stash", "name正确");
    CHECK(json["role"].get<int>() == 1, "role=Material(1)");
    CHECK(json["click_x"].get<float>() > 120.0f, "click_x正确");
    
    // 反序列化到新对象
    StashTabConfig restored;
    restored.FromJson(json);
    CHECK(restored.inventoryId == 42, "反序列化: inventory_id正确");
    CHECK(restored.name == "Test Stash", "反序列化: name正确");
    CHECK(restored.role == StashTabRole::Material, "反序列化: role正确");
    CHECK(restored.enabled == true, "反序列化: enabled正确");
    CHECK(restored.detectedText == "Fragment", "反序列化: detectedText正确");
    CHECK(restored.clickX == 120.5f, "反序列化: clickX正确");
    CHECK(restored.keywordHints.count("fragment") > 0, "反序列化: keywordHints正确");
    CHECK(restored.subTabs.size() == 1, "反序列化: subTabs数量正确");
    CHECK(restored.subTabs[0].inventoryId == 43, "反序列化: 子页inventoryId正确");
}

// === 测试3: Settings 仓库页管理方法 ===
void test_settings_management() {
    printf("\n=== 测试3: Settings仓库页管理 ===\n");
    
    Settings s;
    
    // 添加配置
    StashTabConfig cfg1;
    cfg1.inventoryId = 1;
    cfg1.name = "Fragment Stash";
    cfg1.role = StashTabRole::Material;
    s.stashTabConfigs.push_back(cfg1);
    
    StashTabConfig cfg2;
    cfg2.inventoryId = 2;
    cfg2.name = "Currency Stash";
    cfg2.role = StashTabRole::Special;
    s.stashTabConfigs.push_back(cfg2);
    
    StashTabConfig cfg3;
    cfg3.inventoryId = 3;
    cfg3.name = "Normal Stash";
    cfg3.role = StashTabRole::Material;
    cfg3.enabled = false;  // 禁用
    s.stashTabConfigs.push_back(cfg3);
    
    // 测试FindStashTabConfig
    auto* found = s.FindStashTabConfig(1);
    CHECK(found != nullptr, "FindStashTabConfig: 找到存在的ID");
    CHECK(found->name == "Fragment Stash", "找到的名称正确");
    
    found = s.FindStashTabConfig(99);
    CHECK(found == nullptr, "FindStashTabConfig: 返回nullptr for不存在的ID");
    
    // 测试IsMaterialTab
    CHECK(s.IsMaterialTab(1), "IsMaterialTab: 原料页返回true");
    CHECK(!s.IsMaterialTab(2), "IsMaterialTab: 特殊页返回false");
    CHECK(!s.IsMaterialTab(3), "IsMaterialTab: 禁用的原料页返回false");
    
    // 测试GetMaterialTabIds
    auto matIds = s.GetMaterialTabIds();
    CHECK(matIds.size() == 1, "GetMaterialTabIds: 只有1个启用的原料页");
    CHECK(matIds.count(1) > 0, "GetMaterialTabIds: 包含ID=1");
    
    // 测试StashTabConfigCount
    CHECK(s.StashTabConfigCount() == 3, "StashTabConfigCount: 3个配置");
    
    // 测试Upsert
    StashTabConfig update;
    update.inventoryId = 1;
    update.name = "Updated Fragment";
    update.role = StashTabRole::Ignore;
    s.UpsertStashTabConfig(update);
    CHECK(s.stashTabConfigs.size() == 3, "Upsert: 没有增加新条目");
    CHECK(s.stashTabConfigs[0].name == "Updated Fragment", "Upsert: 更新了名称");
    CHECK(s.stashTabConfigs[0].role == StashTabRole::Ignore, "Upsert: 更新了角色");
    
    // 测试DeleteStashTabConfig
    s.DeleteStashTabConfig(2);
    CHECK(s.stashTabConfigs.size() == 2, "Delete: 删除后数量正确");
    CHECK(s.FindStashTabConfig(2) == nullptr, "Delete: 已删除的ID找不到");
}

// === 测试4: 子页管理 ===
void test_subtabs() {
    printf("\n=== 测试4: 子页管理 ===\n");
    
    Settings s;
    
    // 主页带3个子页
    StashTabConfig main;
    main.inventoryId = 10;
    main.name = "Fragment Main";
    main.role = StashTabRole::Material;
    
    StashTabConfig subA;
    subA.inventoryId = 11;
    subA.name = "Sub A";
    subA.role = StashTabRole::Material;
    
    StashTabConfig subB;
    subB.inventoryId = 12;
    subB.name = "Sub B";
    subB.role = StashTabRole::Ignore;
    subB.enabled = false;
    
    StashTabConfig subC;
    subC.inventoryId = 13;
    subC.name = "Sub C";
    subC.role = StashTabRole::Material;
    
    main.subTabs.push_back(subA);
    main.subTabs.push_back(subB);
    main.subTabs.push_back(subC);
    s.stashTabConfigs.push_back(main);
    
    // 查找子页
    auto* found = s.FindStashTabConfig(11);
    CHECK(found != nullptr, "FindSubTab: 找到子页A");
    CHECK(found->name == "Sub A", "子页A名称正确");
    
    found = s.FindStashTabConfig(12);
    CHECK(found != nullptr, "FindSubTab: 找到子页B");
    
    found = s.FindStashTabConfig(99);
    CHECK(found == nullptr, "FindSubTab: 不存在的返回nullptr");
    
    // GetMaterialTabIds 应包含主页+子页A+子页C
    auto matIds = s.GetMaterialTabIds();
    CHECK(matIds.size() == 3, "GetMaterialTabIds: 包含3个原料页(主+A+C)");
    CHECK(matIds.count(10) > 0, "包含主页ID=10");
    CHECK(matIds.count(11) > 0, "包含子页A ID=11");
    CHECK(matIds.count(13) > 0, "包含子页C ID=13");
    CHECK(matIds.count(12) == 0, "不包含禁用的子页B");
    
    // StashTabConfigCount 应该包含所有子页
    CHECK(s.StashTabConfigCount() == 4, "StashTabConfigCount: 主页+3子页=4");
    
    // 序列化/反序列化带子页
    auto json = main.ToJson();
    StashTabConfig restored;
    restored.FromJson(json);
    CHECK(restored.subTabs.size() == 3, "反序列化: 子页数量正确");
    CHECK(restored.subTabs[0].inventoryId == 11, "反序列化: 子页A ID正确");
    CHECK(restored.subTabs[2].name == "Sub C", "反序列化: 子页C名称正确");
}

// === 测试5: StashTabRoleName ===
void test_role_names() {
    printf("\n=== 测试5: 角色名称 ===\n");
    
    CHECK(std::string(StashTabRoleName(StashTabRole::None)) == "未分配", "None名称正确");
    CHECK(std::string(StashTabRoleName(StashTabRole::Material)) == "原料", "Material名称正确");
    CHECK(std::string(StashTabRoleName(StashTabRole::Special)) == "特殊", "Special名称正确");
    CHECK(std::string(StashTabRoleName(StashTabRole::Ignore)) == "忽略", "Ignore名称正确");
}

// === 测试6: 设置JSON加载/保存 ===
void test_settings_json() {
    printf("\n=== 测试6: Settings JSON加载/保存 ===\n");
    
    Settings s;
    s.stashTabConfigs.clear();
    
    // 预先添加一些仓库页配置
    StashTabConfig cfg1;
    cfg1.inventoryId = 100;
    cfg1.name = "Test Material Tab";
    cfg1.role = StashTabRole::Material;
    cfg1.clickX = 200.0f;
    cfg1.clickY = 150.0f;
    s.stashTabConfigs.push_back(cfg1);
    
    // 子页
    StashTabConfig sub;
    sub.inventoryId = 101;
    sub.name = "Sub Material";
    sub.role = StashTabRole::Material;
    s.stashTabConfigs[0].subTabs.push_back(sub);
    
    // 生成JSON
    nlohmann::json j;
    j["stash_tab_configs"] = nlohmann::json::array();
    for (const auto& c : s.stashTabConfigs) {
        j["stash_tab_configs"].push_back(c.ToJson());
    }
    
    // 验证JSON结构
    CHECK(j.contains("stash_tab_configs"), "JSON包含stash_tab_configs");
    CHECK(j["stash_tab_configs"].is_array(), "stash_tab_configs是数组");
    CHECK(j["stash_tab_configs"].size() == 1, "数组大小为1");
    
    auto& firstCfg = j["stash_tab_configs"][0];
    CHECK(firstCfg["inventory_id"].get<int>() == 100, "第一个配置inventory_id正确");
    CHECK(firstCfg["role"].get<int>() == 1, "第一个配置role=Material");
    CHECK(firstCfg["sub_tabs"].size() == 1, "子页数量正确");
    
    // 反序列化回Settings
    Settings restored;
    restored.stashTabConfigs.clear();
    if (j.contains("stash_tab_configs") && j["stash_tab_configs"].is_array()) {
        for (const auto& tc : j["stash_tab_configs"]) {
            StashTabConfig cfg;
            cfg.FromJson(tc);
            restored.stashTabConfigs.push_back(cfg);
        }
    }
    
    CHECK(restored.stashTabConfigs.size() == 1, "反序列化: 配置数量正确");
    CHECK(restored.stashTabConfigs[0].inventoryId == 100, "反序列化: inventory_id正确");
    CHECK(restored.stashTabConfigs[0].name == "Test Material Tab", "反序列化: name正确");
    CHECK(restored.stashTabConfigs[0].subTabs.size() == 1, "反序列化: 子页数量正确");
    CHECK(restored.stashTabConfigs[0].subTabs[0].inventoryId == 101, "反序列化: 子页ID正确");
}

// === 测试7: StashTypeTable 查表 ===
void test_stash_type_table() {
    printf("\n=== 测试7: StashTypeTable 查表 ===\n");

    // 测试通过 ID 查找
    auto* e1 = TabletReforgeGame::FindStashTypeById("CurrencyStash");
    CHECK(e1 != nullptr, "FindStashTypeById: CurrencyStash 找到");
    CHECK(e1 && std::string(e1->id) == "CurrencyStash", "id 字段正确");
    CHECK(e1 && e1->stashId == 3, "stashId = 3");
    CHECK(e1 && e1->storageSlots == 53, "storageSlots = 53");
    CHECK(e1 && std::string(e1->ddsFileName) == "03_CurrencyStash.dds", "ddsFileName 正确");

    // 测试通过 stashId 查找
    auto* e2 = TabletReforgeGame::FindStashTypeByStashId(4);
    CHECK(e2 != nullptr, "FindStashTypeByStashId: id=4 找到");
    CHECK(e2 && std::string(e2->id) == "UniqueStash", "id=4 是 UniqueStash");

    // 测试不存在的 ID
    auto* e3 = TabletReforgeGame::FindStashTypeById("NonExistentStash");
    CHECK(e3 == nullptr, "FindStashTypeById: 不存在返回 nullptr");

    // 测试 ResolveStashType
    auto* e4 = TabletReforgeGame::ResolveStashType("MapStash");
    CHECK(e4 != nullptr, "ResolveStashType: MapStash 找到");

    // 总数验证
    CHECK(TabletReforgeGame::StashTypeCount() == 25, "StashTypeCount = 25");
}

// === 测试8: 加载 BMP 图标模板 ===
void test_load_icon_templates() {
    printf("\n=== 测试8: 加载 BMP 图标模板 ===\n");

    // 模板目录相对于测试二进制位置
    // 测试二进制在 bin/<config>/ 下，图标在 ../../resources/stash_icons/
    std::filesystem::path exeDir = std::filesystem::current_path();
    // 尝试多个可能的路径
    std::filesystem::path iconDir;
    std::vector<std::filesystem::path> candidates = {
        exeDir / "resources" / "stash_icons",
        exeDir.parent_path().parent_path() / "resources" / "stash_icons",
        std::filesystem::path(F("Trae\\chuxue\\Plugins\\TabletReforgeAutomation\\resources\\stash_icons")),
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) {
            iconDir = c;
            break;
        }
    }

    if (iconDir.empty()) {
        printf("  SKIP: 找不到图标目录（尝试了多个路径）\n");
        printf("        当前路径: %s\n", exeDir.string().c_str());
        return;
    }

    printf("  图标目录: %s\n", iconDir.string().c_str());

    // 构造一个假的 pluginDir（iconDir 的父目录）
    std::filesystem::path pluginDir = iconDir.parent_path().parent_path();

    std::vector<VisionRecogNS::TabIconTemplate> templates;
    int loaded = VisionRecogNS::LoadTabIconTemplates(pluginDir, templates);

    CHECK(loaded > 0, "至少加载了 1 个模板");
    CHECK(loaded == 25, "加载了全部 25 个模板");
    CHECK(templates.size() == 25, "templates 向量大小正确");

    // 验证每个模板的基本属性
    bool allValid = true;
    for (const auto& t : templates) {
        if (t.width <= 0 || t.height <= 0 || t.bgra.empty() || t.stashId < 0) {
            printf("  模板 #%d (%s) 无效: %dx%d, %zu bytes\n",
                t.stashId, t.stashName.c_str(), t.width, t.height, t.bgra.size());
            allValid = false;
        }
    }
    CHECK(allValid, "所有模板尺寸/像素数据有效");

    // 验证模板按 stashId 排序
    bool sorted = true;
    for (size_t i = 1; i < templates.size(); ++i) {
        if (templates[i-1].stashId > templates[i].stashId) {
            sorted = false;
            break;
        }
    }
    CHECK(sorted, "模板按 stashId 升序排列");

    // 验证特定模板
    auto currency = templates[3];  // stashId=3 = CurrencyStash
    CHECK(currency.stashId == 3, "CurrencyStash stashId = 3");
    CHECK(currency.stashName == "CurrencyStash", "CurrencyStash 名称正确");
    CHECK(currency.width == 28, "CurrencyStash 宽度 = 28");
    CHECK(currency.height == 28, "CurrencyStash 高度 = 28");
    CHECK(currency.bgra.size() == 28 * 28 * 4, "CurrencyStash BGRA 像素数 = 28*28*4");
}

// === 测试9: MSE 像素匹配正确性 ===
void test_mse_matching() {
    printf("\n=== 测试9: MSE 像素匹配正确性 ===\n");

    // 创建简单的测试图像
    int w = 28, h = 28;
    std::vector<uint8_t> imgA(w * h * 4, 128);  // 全灰色
    std::vector<uint8_t> imgB(w * h * 4, 128);  // 同样全灰色

    // 完全相同的图像 MSE 应为 0
    double mseSame = VisionRecogNS::ComputeImageMSE(
        imgA.data(), imgB.data(), w, h);
    CHECK(mseSame == 0.0, "相同图像 MSE = 0");

    // 稍有不同的图像
    std::vector<uint8_t> imgC(w * h * 4, 200);  // 更亮
    double mseDiff = VisionRecogNS::ComputeImageMSE(
        imgA.data(), imgC.data(), w, h);
    CHECK(mseDiff > 0.0, "不同图像 MSE > 0");
    CHECK(mseDiff < 255.0 * 255.0, "MSE 在合理范围内");

    // 置信度计算：相同图像应为 1.0
    double confSame = 1.0 / (1.0 + mseSame / (255.0 * 255.0));
    CHECK(confSame == 1.0, "相同图像置信度 = 1.0");

    // 完全不同的图像
    std::vector<uint8_t> imgD(w * h * 4, 0);    // 全黑
    for (int i = 0; i < w * h; ++i) {
        imgD[i * 4 + 0] = 0;   // B
        imgD[i * 4 + 1] = 0;   // G
        imgD[i * 4 + 2] = 255; // R (全红)
        imgD[i * 4 + 3] = 255; // A
    }
    double mseRed = VisionRecogNS::ComputeImageMSE(
        imgA.data(), imgD.data(), w, h);
    CHECK(mseRed > 1000.0, "灰色vs全红 MSE 较大");

    // 置信度
    double confRed = 1.0 / (1.0 + mseRed / (255.0 * 255.0));
    CHECK(confRed < 0.5, "灰色vs全红 置信度 < 0.5");

    // 测试 stride 参数
    int stride = w * 4;
    double mseStrided = VisionRecogNS::ComputeImageMSE(
        imgA.data(), imgB.data(), w, h, stride, stride);
    CHECK(mseStrided == 0.0, "带 stride 参数 MSE 正确");

    // 测试空指针
    double mseNull = VisionRecogNS::ComputeImageMSE(nullptr, imgA.data(), w, h);
    CHECK(mseNull > 1e8, "空指针 MSE 返回大值");
}

// === 测试10: 图标模板自匹配 ===
void test_template_self_match() {
    printf("\n=== 测试10: 图标模板自匹配 ===\n");

    // 加载模板
    std::filesystem::path exeDir = std::filesystem::current_path();
    std::filesystem::path pluginDir;
    std::vector<std::filesystem::path> candidates = {
        exeDir.parent_path().parent_path(),
        exeDir,
        std::filesystem::path(F("Trae\\chuxue\\Plugins\\TabletReforgeAutomation")),
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "resources" / "stash_icons")) {
            pluginDir = c;
            break;
        }
    }

    if (pluginDir.empty()) {
        printf("  SKIP: 找不到插件目录\n");
        return;
    }

    std::vector<VisionRecogNS::TabIconTemplate> templates;
    int n = VisionRecogNS::LoadTabIconTemplates(pluginDir, templates);
    if (n == 0) {
        printf("  SKIP: 没有加载到模板\n");
        return;
    }

    printf("  已加载 %d 个模板\n", n);

    // 每个模板与自己比对，应有高置信度
    int highConfCount = 0;
    for (const auto& t : templates) {
        // 用模板自身作为"屏幕截图"来匹配
        VisionRecogNS::ScreenFrame fakeFrame;
        fakeFrame.width = t.width;
        fakeFrame.height = t.height;
        fakeFrame.stride = t.width * 4;
        fakeFrame.bgra = t.bgra;

        auto result = VisionRecogNS::MatchTemplateByPixelMSE(
            fakeFrame, templates, 0, 0, t.width, t.height, 0.5);

        if (result.matched && result.confidence > 0.8) {
            highConfCount++;
        }

        // 日志
        if (t.stashId <= 5 || (result.matched && result.stashId != t.stashId)) {
            printf("    模板 %s (id=%d): matched=%d bestMatch=%s (id=%d) conf=%.3f\n",
                t.stashName.c_str(), t.stashId,
                result.matched ? 1 : 0,
                result.stashName.c_str(), result.stashId,
                result.confidence);
        }
    }

    printf("  自匹配高置信度(>0.8)模板数: %d / %zu\n", highConfCount, templates.size());
    CHECK(highConfCount > 0, "至少有1个模板自匹配成功");

    // 最佳匹配应该是自己（或同名模板）
    bool selfMatchCorrect = false;
    for (const auto& t : templates) {
        VisionRecogNS::ScreenFrame fakeFrame;
        fakeFrame.width = t.width;
        fakeFrame.height = t.height;
        fakeFrame.stride = t.width * 4;
        fakeFrame.bgra = t.bgra;

        auto result = VisionRecogNS::MatchTemplateByPixelMSE(
            fakeFrame, templates, 0, 0, t.width, t.height, 0.0);

        if (result.matched && result.stashId == t.stashId && result.confidence > 0.9) {
            selfMatchCorrect = true;
            break;
        }
    }
    CHECK(selfMatchCorrect, "至少1个模板能与自身高置信度匹配");
}

int main() {
    printf("=== 仓库页配置与视觉识别Mock测试 ===\n");
    printf("测试时间: %s\n\n", __DATE__);
    
    test_role_logic();
    test_serialization();
    test_settings_management();
    test_subtabs();
    test_role_names();
    test_settings_json();
    test_stash_type_table();
    test_load_icon_templates();
    test_mse_matching();
    test_template_self_match();

    // —— 图标归类新增配置项验证 ——
    {
        printf("\n=== 测试11: 图标归类配置项序列化 ===\n");
        TabletReforgeConfig::Settings s;
        s.autoClassifyOnScan = true;
        s.useClassifiedClick = true;
        s.autoClickScannedStash = false;
        s.preferUiTreeOverVision = true;

        nlohmann::json j;
        j["auto_classify_on_scan"] = s.autoClassifyOnScan;
        j["use_classified_click"] = s.useClassifiedClick;
        j["auto_click_scanned_stash"] = s.autoClickScannedStash;
        j["prefer_ui_tree_over_vision"] = s.preferUiTreeOverVision;

        CHECK(j["auto_classify_on_scan"].get<bool>() == true, "auto_classify_on_scan 序列化");
        CHECK(j["use_classified_click"].get<bool>() == true, "use_classified_click 序列化");

        TabletReforgeConfig::Settings restored;
        restored.autoClassifyOnScan = j.value("auto_classify_on_scan", false);
        restored.useClassifiedClick = j.value("use_classified_click", false);
        CHECK(restored.autoClassifyOnScan == true, "auto_classify_on_scan 反序列化");
        CHECK(restored.useClassifiedClick == true, "use_classified_click 反序列化");
    }

    // —— 图标归类数据结构验证 ——
    {
        printf("\n=== 测试12: StashIconClassifyResult 结构 ===\n");
        TabletReforgeGame::StashIconClassifyResult r;
        r.inventoryId = 7;
        r.stashTypeId = 9;
        r.stashTypeName = "FragmentStash";
        r.chineseName = "碎片/碑牌仓库";
        r.confidence = 0.92;
        r.clickX = 450;
        r.clickY = 180;
        r.clickable = true;

        CHECK(r.inventoryId == 7, "inventoryId 字段");
        CHECK(r.stashTypeId == 9, "stashTypeId 字段");
        CHECK(r.stashTypeName == "FragmentStash", "stashTypeName 字段");
        CHECK(r.chineseName == "碎片/碑牌仓库", "chineseName 字段");
        CHECK(r.confidence > 0.9, "confidence 字段");
        CHECK(r.clickable == true, "clickable 字段");

        // 默认构造状态
        TabletReforgeGame::StashIconClassifyResult def;
        CHECK(def.inventoryId == 0, "默认 inventoryId = 0");
        CHECK(def.stashTypeId == -1, "默认 stashTypeId = -1 (未识别)");
        CHECK(def.clickable == false, "默认 clickable = false");
    }

    // —— 图标归类缓存机制验证（无需真实 ctx）——
    {
        printf("\n=== 测试13: 图标归类缓存空指针安全性 ===\n");
        // GetIconClassifyForInventory 传入 nullptr 应安全返回 nullptr
        auto* r = TabletReforgeGame::GetIconClassifyForInventory(nullptr, 1, std::filesystem::path());
        CHECK(r == nullptr, "空 ctx 应安全返回 nullptr");

        // 空 pluginDir + nullptr ctx
        auto results = TabletReforgeGame::ClassifyAllStashTabsByIcon(nullptr, std::filesystem::path());
        CHECK(results.empty(), "空 ctx 应返回空 map");
    }

    // —— StashItemMapper 配置可写访问 ——
    {
        printf("\n=== 测试14: StashItemMapper 可写配置访问 ===\n");
        TabletReforgeGame::StashMappingManager mgr;
        auto& cfg = mgr.GetConfigMutable();
        TabletReforgeGame::StashTabItemMapping m;
        m.inventoryId = 100;
        m.stashTypeId = 3;
        m.stashTypeName = "CurrencyStash";
        cfg.tabMappings.push_back(m);

        auto& readOnly = mgr.GetConfig();
        CHECK(readOnly.tabMappings.size() == 1, "tabMappings 数量正确");
        CHECK(readOnly.tabMappings[0].inventoryId == 100, "inventoryId 正确");
        CHECK(readOnly.tabMappings[0].stashTypeId == 3, "stashTypeId 正确");

        // 再次写入
        auto& cfg2 = mgr.GetConfigMutable();
        cfg2.tabMappings[0].stashTypeId = 9;
        cfg2.tabMappings[0].stashTypeName = "FragmentStash";
        CHECK(mgr.GetConfig().tabMappings[0].stashTypeId == 9, "再次写入后 stashTypeId 更新");
    }
    
    printf("\n========================================\n");
    printf("测试完成: %d 通过, %d 失败\n", g_passed, g_failed);
    printf("========================================\n");
    
    return g_failed > 0 ? 1 : 0;
}