// StandaloneVisionTest.cpp — 独立视觉识别测试
// 编译: cl /EHsc /std:c++20 /O2 /Fe:StandaloneVisionTest.exe StandaloneVisionTest.cpp
// 或通过 VS 开发人员命令行:
//   cl /EHsc /std:c++20 /O2 /Fe:StandaloneVisionTest.exe StandaloneVisionTest.cpp user32.lib gdi32.lib

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#define NOMINMAX
#include <windows.h>

// ============================================================
// 最小化数据结构（与 VisionRecognizer.h 中相同）
// ============================================================

struct ScreenFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> bgra;
};

struct TabIconTemplate {
    int stashId = -1;
    std::string stashName;
    std::string bmpFileName;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
};

struct MatchResult {
    bool matched = false;
    int stashId = -1;
    std::string stashName;
    int screenX = 0;
    int screenY = 0;
    double confidence = 0;
};

// ============================================================
// StashTypeTable 最小化副本（与 StashTypeTable.h 中相同）
// ============================================================

struct StashTypeEntry {
    int stashId;
    const char* id;
    const char* ddsFileName;
};

static constexpr StashTypeEntry kStashTypeTable[] = {
    { 0,  "NormalStash",          "00_NormalStash.dds" },
    { 1,  "PremiumStash",         "01_PremiumStash.dds" },
    { 2,  "TradeStash",           "02_TradeStash.dds" },
    { 3,  "CurrencyStash",        "03_CurrencyStash.dds" },
    { 4,  "UniqueStash",          "04_UniqueStash.dds" },
    { 5,  "MapStash",             "05_MapStash.dds" },
    { 6,  "DivinationCardStash",  "06_DivinationCardStash.dds" },
    { 7,  "QuadStash",            "07_QuadStash.dds" },
    { 8,  "EssenceStash",         "08_EssenceStash.dds" },
    { 9,  "FragmentStash",        "09_FragmentStash.dds" },
    { 10, "PCBangPremiumStash",   "10_PCBangPremiumStash.dds" },
    { 11, "PCBangEssenceStash",   "11_PCBangEssenceStash.dds" },
    { 12, "DelveStash",           "12_DelveStash.dds" },
    { 13, "BlightStash",          "13_BlightStash.dds" },
    { 14, "UltimatumStash",       "14_UltimatumStash.dds" },
    { 15, "DeliriumStash",        "15_DeliriumStash.dds" },
    { 16, "Folder",               "16_Folder.dds" },
    { 17, "FlaskStash",           "17_FlaskStash.dds" },
    { 18, "GemStash",             "18_GemStash.dds" },
    { 19, "SocketableStash",      "19_SocketableStash.dds" },
    { 20, "ExpeditionStash",      "20_ExpeditionStash.dds" },
    { 21, "RitualStash",          "21_RitualStash.dds" },
    { 22, "BreachStash",          "22_BreachStash.dds" },
    { 23, "AbyssStash",           "23_AbyssStash.dds" },
    { 24, "RelicStash",           "24_RelicStash.dds" },
};

// ============================================================
// 核心算法（与 VisionRecognizer.h 中相同）
// ============================================================

double ComputeImageMSE(const uint8_t* a, const uint8_t* b,
                       int w, int h, int strideA = 0, int strideB = 0) {
    if (!a || !b || w <= 0 || h <= 0) return 1e9;
    if (strideA == 0) strideA = w * 4;
    if (strideB == 0) strideB = w * 4;

    double sumSq = 0;
    int count = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* pa = a + y * strideA;
        const uint8_t* pb = b + y * strideB;
        for (int x = 0; x < w; ++x) {
            int off = x * 4;
            int dB = (int)pa[off + 0] - (int)pb[off + 0];
            int dG = (int)pa[off + 1] - (int)pb[off + 1];
            int dR = (int)pa[off + 2] - (int)pb[off + 2];
            int dA = (int)pa[off + 3] - (int)pb[off + 3];
            sumSq += (double)(dB * dB + dG * dG + dR * dR + dA * dA);
            count++;
        }
    }
    return count > 0 ? sumSq / count : 1e9;
}

MatchResult MatchTemplateByPixelMSE(const ScreenFrame& frame,
                                     const std::vector<TabIconTemplate>& templates,
                                     int searchX, int searchY, int searchW, int searchH,
                                     double threshold = 0.5) {
    MatchResult result;
    if (templates.empty() || searchW <= 0 || searchH <= 0 || frame.bgra.empty()) {
        return result;
    }

    MatchResult best;
    double bestScore = threshold;

    for (const auto& tpl : templates) {
        if (tpl.width <= 0 || tpl.height <= 0) continue;
        if (tpl.width > searchW || tpl.height > searchH) continue;

        int step = std::max(1, tpl.width / 4);
        int maxY = searchH - tpl.height;
        int maxX = searchW - tpl.width;

        for (int oy = 0; oy <= maxY; oy += step) {
            for (int ox = 0; ox <= maxX; ox += step) {
                double mse = ComputeImageMSE(
                    frame.bgra.data() + (searchY + oy) * frame.stride + (searchX + ox) * 4,
                    tpl.bgra.data(),
                    tpl.width, tpl.height,
                    frame.stride,
                    tpl.width * 4
                );

                double confidence = 1.0 / (1.0 + mse / (255.0 * 255.0));

                if (confidence > bestScore) {
                    bestScore = confidence;
                    best.matched = true;
                    best.stashId = tpl.stashId;
                    best.stashName = tpl.stashName;
                    best.screenX = searchX + ox + tpl.width / 2;
                    best.screenY = searchY + oy + tpl.height / 2;
                    best.confidence = confidence;
                }
            }
        }
    }
    return best;
}

// ============================================================
// BMP 加载（GDI）
// ============================================================

bool LoadBmpAsBGRA(const std::filesystem::path& bmpPath, TabIconTemplate& out) {
    HBITMAP hBmp = (HBITMAP)::LoadImageW(nullptr, bmpPath.c_str(), IMAGE_BITMAP,
                                         0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!hBmp) return false;

    BITMAP bm = {};
    ::GetObject(hBmp, sizeof(bm), &bm);
    if (bm.bmBitsPixel != 32 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        ::DeleteObject(hBmp);
        return false;
    }

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = bm.bmWidth;
    bih.biHeight = -bm.bmHeight;  // 负值 → 顶向下
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
    out.bgra.resize((size_t)out.width * out.height * 4, 0);

    HDC hdcMem = ::CreateCompatibleDC(nullptr);
    if (hdcMem) {
        ::GetDIBits(hdcMem, hBmp, 0, (UINT)bm.bmHeight,
                    out.bgra.data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);
        ::DeleteDC(hdcMem);
    }

    ::DeleteObject(hBmp);
    return true;
}

int LoadTabIconTemplates(const std::filesystem::path& pluginDir,
                         std::vector<TabIconTemplate>& outTemplates) {
    outTemplates.clear();

    std::filesystem::path iconsDir = pluginDir / "resources" / "stash_icons";
    if (!std::filesystem::exists(iconsDir)) {
        // 回退：尝试其他路径
        std::filesystem::path fallback = pluginDir.parent_path() / "resources" / "stash_icons";
        if (std::filesystem::exists(fallback)) {
            iconsDir = fallback;
        } else {
            return 0;
        }
    }

    int loaded = 0;
    for (const auto& entry : kStashTypeTable) {
        std::string bmpName(entry.ddsFileName);
        bmpName = bmpName.substr(0, bmpName.size() - 4) + ".bmp";
        std::filesystem::path bmpPath = iconsDir / bmpName;

        if (!std::filesystem::exists(bmpPath)) continue;

        TabIconTemplate tpl;
        tpl.stashId = entry.stashId;
        tpl.stashName = entry.id;
        tpl.bmpFileName = bmpName;

        if (LoadBmpAsBGRA(bmpPath, tpl)) {
            outTemplates.push_back(std::move(tpl));
            loaded++;
        }
    }

    return loaded;
}

// ============================================================
// 测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  TEST: %s ... ", name); } while(0)
#define PASS() do { printf("PASS\n"); g_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); g_failed++; } while(0)
#define CHECK(cond, msg) do { if (cond) { PASS(); } else { FAIL(msg); } } while(0)

// ============================================================
// 测试 1: MSE 基本正确性
// ============================================================
void test_mse_basic() {
    printf("\n=== 测试1: MSE 基本正确性 ===\n");

    int w = 28, h = 28;
    std::vector<uint8_t> imgA(w * h * 4, 128);
    std::vector<uint8_t> imgB(w * h * 4, 128);

    double mseSame = ComputeImageMSE(imgA.data(), imgB.data(), w, h);
    CHECK(mseSame == 0.0, "相同图像 MSE = 0");

    std::vector<uint8_t> imgC(w * h * 4, 200);
    double mseDiff = ComputeImageMSE(imgA.data(), imgC.data(), w, h);
    CHECK(mseDiff > 0.0, "不同图像 MSE > 0");
    CHECK(mseDiff < 255.0 * 255.0, "MSE 在合理范围内");

    // 置信度
    double conf = 1.0 / (1.0 + mseDiff / (255.0 * 255.0));
    CHECK(conf < 1.0 && conf > 0.0, "置信度在 0-1 之间");

    // 空指针
    double mseNull = ComputeImageMSE(nullptr, imgA.data(), w, h);
    CHECK(mseNull > 1e8, "空指针 MSE 返回大值");

    // stride 测试
    int stride = w * 4;
    double mseStrided = ComputeImageMSE(imgA.data(), imgB.data(), w, h, stride, stride);
    CHECK(mseStrided == 0.0, "带 stride 参数 MSE 正确");
}

// ============================================================
// 测试 2: StashTypeTable 查表
// ============================================================
void test_stash_type_table() {
    printf("\n=== 测试2: StashTypeTable 查表 ===\n");

    int count = sizeof(kStashTypeTable) / sizeof(kStashTypeTable[0]);
    CHECK(count == 25, "StashTypeTable 有 25 条记录");

    // 验证特定条目
    bool foundCurrency = false;
    bool foundUnique = false;
    for (const auto& e : kStashTypeTable) {
        if (e.stashId == 3 && std::string(e.id) == "CurrencyStash") foundCurrency = true;
        if (e.stashId == 4 && std::string(e.id) == "UniqueStash") foundUnique = true;
    }
    CHECK(foundCurrency, "CurrencyStash (id=3) 存在");
    CHECK(foundUnique, "UniqueStash (id=4) 存在");
}

// ============================================================
// 测试 3: 加载 BMP 模板
// ============================================================
void test_load_templates() {
    printf("\n=== 测试3: 加载 BMP 图标模板 ===\n");

    std::filesystem::path exeDir = std::filesystem::current_path();

    // 尝试多个路径
    std::filesystem::path pluginDir;
    std::vector<std::filesystem::path> candidates = {
        exeDir.parent_path().parent_path(),
        exeDir,
        exeDir.parent_path(),
        std::filesystem::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation"),
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "resources" / "stash_icons")) {
            pluginDir = c;
            break;
        }
    }

    if (pluginDir.empty()) {
        printf("  SKIP: 找不到图标目录\n");
        printf("        当前路径: %s\n", exeDir.string().c_str());
        return;
    }

    printf("  插件目录: %s\n", pluginDir.string().c_str());

    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);

    CHECK(n > 0, "至少加载了 1 个模板");
    printf("  加载了 %d 个模板\n", n);

    if (n == 0) return;

    // 验证模板有效性
    bool allValid = true;
    for (const auto& t : templates) {
        if (t.width != 28 || t.height != 28) {
            printf("    模板 %s 尺寸异常: %dx%d\n", t.stashName.c_str(), t.width, t.height);
            allValid = false;
        }
        if (t.bgra.size() != 28 * 28 * 4) {
            printf("    模板 %s 像素数据大小异常: %zu (期望 %d)\n",
                t.stashName.c_str(), t.bgra.size(), 28 * 28 * 4);
            allValid = false;
        }
    }
    CHECK(allValid, "所有模板尺寸=28x28，像素数据正确");

    // 验证模板按 stashId 排序
    bool sorted = true;
    for (size_t i = 1; i < templates.size(); ++i) {
        if (templates[i-1].stashId > templates[i].stashId) {
            sorted = false;
            printf("    排序错误: [%d].stashId > [%d].stashId\n", i-1, i);
            break;
        }
    }
    CHECK(sorted, "模板按 stashId 升序排列");
}

// ============================================================
// 测试 4: 图标自匹配
// ============================================================
void test_template_self_match() {
    printf("\n=== 测试4: 图标模板自匹配 ===\n");

    std::filesystem::path exeDir = std::filesystem::current_path();
    std::filesystem::path pluginDir;
    std::vector<std::filesystem::path> candidates = {
        exeDir.parent_path().parent_path(),
        exeDir,
        exeDir.parent_path(),
        std::filesystem::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation"),
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "resources" / "stash_icons")) {
            pluginDir = c;
            break;
        }
    }

    if (pluginDir.empty()) {
        printf("  SKIP: 找不到图标目录\n");
        return;
    }

    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);
    if (n == 0) {
        printf("  SKIP: 没有加载到模板\n");
        return;
    }

    printf("  已加载 %d 个模板，开始自匹配测试...\n", n);

    int highConfCount = 0;
    int correctMatchCount = 0;
    int totalTemplates = (int)templates.size();

    for (const auto& t : templates) {
        // 用模板自身构造 ScreenFrame
        ScreenFrame fakeFrame;
        fakeFrame.width = t.width;
        fakeFrame.height = t.height;
        fakeFrame.stride = t.width * 4;
        fakeFrame.bgra = t.bgra;

        auto result = MatchTemplateByPixelMSE(
            fakeFrame, templates, 0, 0, t.width, t.height, 0.0);

        if (result.matched) {
            if (result.confidence > 0.8) highConfCount++;
            if (result.stashId == t.stashId) correctMatchCount++;

            printf("    模板 %-25s (id=%2d): bestMatch=%-25s (id=%2d) conf=%.4f\n",
                t.stashName.c_str(), t.stashId,
                result.stashName.c_str(), result.stashId,
                result.confidence);
        } else {
            printf("    模板 %-25s (id=%2d): 无匹配\n", t.stashName.c_str(), t.stashId);
        }
    }

    printf("  统计:\n");
    printf("    高置信度(>0.8)匹配: %d / %d\n", highConfCount, totalTemplates);
    printf("    正确类型匹配: %d / %d\n", correctMatchCount, totalTemplates);

    CHECK(highConfCount > 0, "至少有1个模板自匹配高置信度");
    CHECK(correctMatchCount > 0, "至少有1个模板能正确匹配自身类型");

    // 计算平均置信度
    double avgConf = 0;
    int matchedCount = 0;
    for (const auto& t : templates) {
        ScreenFrame fakeFrame;
        fakeFrame.width = t.width;
        fakeFrame.height = t.height;
        fakeFrame.stride = t.width * 4;
        fakeFrame.bgra = t.bgra;

        auto result = MatchTemplateByPixelMSE(
            fakeFrame, templates, 0, 0, t.width, t.height, 0.0);

        if (result.matched) {
            avgConf += result.confidence;
            matchedCount++;
        }
    }
    if (matchedCount > 0) {
        avgConf /= matchedCount;
        printf("    平均置信度: %.4f\n", avgConf);
        CHECK(avgConf > 0.5, "平均置信度 > 0.5");
    }
}

// ============================================================
// 测试 5: 区分不同图标
// ============================================================
void test_template_distinction() {
    printf("\n=== 测试5: 区分不同图标 ===\n");

    std::filesystem::path exeDir = std::filesystem::current_path();
    std::filesystem::path pluginDir;
    std::vector<std::filesystem::path> candidates = {
        exeDir.parent_path().parent_path(),
        exeDir,
        exeDir.parent_path(),
        std::filesystem::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation"),
    };

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "resources" / "stash_icons")) {
            pluginDir = c;
            break;
        }
    }

    if (pluginDir.empty()) {
        printf("  SKIP: 找不到图标目录\n");
        return;
    }

    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);
    if (n < 2) {
        printf("  SKIP: 模板数量不足\n");
        return;
    }

    // 对每对不同类型的模板，计算 MSE
    int distCount = 0;
    for (size_t i = 0; i < templates.size(); ++i) {
        for (size_t j = i + 1; j < templates.size(); ++j) {
            double mse = ComputeImageMSE(
                templates[i].bgra.data(),
                templates[j].bgra.data(),
                templates[i].width, templates[i].height,
                templates[i].width * 4,
                templates[j].width * 4
            );

            double conf = 1.0 / (1.0 + mse / (255.0 * 255.0));
            if (conf > 0.9) {
                // 这两个图标可能非常相似
                printf("    警告: %s 与 %s 相似度较高 (conf=%.4f)\n",
                    templates[i].stashName.c_str(),
                    templates[j].stashName.c_str(),
                    conf);
            }
            distCount++;
        }
    }

    printf("  比较了 %d 对不同模板\n", distCount);
    CHECK(distCount > 0, "至少比较了 1 对模板");

    // 验证：相同模板与自身的 MSE 应为 0
    for (const auto& t : templates) {
        double mse = ComputeImageMSE(t.bgra.data(), t.bgra.data(), t.width, t.height);
        if (mse != 0.0) {
            printf("    错误: %s 与自身 MSE = %.2f (应为 0)\n", t.stashName.c_str(), mse);
            FAIL("相同模板与自身 MSE 不为 0");
            return;
        }
    }
    PASS();
}

// ============================================================
// 主函数
// ============================================================
int main() {
    printf("=== 视觉识别 Mock 测试 (Standalone) ===\n");
    printf("测试时间: %s\n\n", __DATE__);

    test_mse_basic();
    test_stash_type_table();
    test_load_templates();
    test_template_self_match();
    test_template_distinction();

    printf("\n========================================\n");
    printf("测试完成: %d 通过, %d 失败\n", g_passed, g_failed);
    printf("========================================\n");

    return g_failed > 0 ? 1 : 0;
}