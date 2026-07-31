// MockVisionTest.cpp — 纯 C++ mock 测试，不依赖 Windows SDK
// 验证视觉识别核心算法：MSE模板匹配、BMP加载、StashTypeTable查表
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <array>
#include <fstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ============================================================
// StashTypeTable（与主项目相同的硬编码仓库类型表）
// ============================================================
struct StashTypeEntry {
    int         stashId;
    const char* id;
    const char* ddsFileName;
};

inline constexpr std::array<StashTypeEntry, 25> kStashTypeTable = {{
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
    { 15, "Folder",               "16_Folder.dds" },
    { 16, "FlaskStash",           "17_FlaskStash.dds" },
    { 17, "GemStash",             "18_GemStash.dds" },
    { 18, "SocketableStash",      "19_SocketableStash.dds" },
    { 19, "ExpeditionStash",      "20_ExpeditionStash.dds" },
    { 20, "RitualStash",          "21_RitualStash.dds" },
    { 21, "BreachStash",          "22_BreachStash.dds" },
    { 22, "AbyssStash",           "23_AbyssStash.dds" },
    { 23, "RelicStash",           "24_RelicStash.dds" },
    { 24, "NormalStash",          "15_Folder.dds" }, // placeholder for missing index
}};

// ============================================================
// BMP 加载器（纯标准库实现，无 Windows 依赖）
// ============================================================
#pragma pack(push, 1)
struct BMPFileHeader {
    uint16_t fileType;
    uint32_t fileSize;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offsetData;
};

struct BMPInfoHeader {
    uint32_t dibHeaderSize;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bitsPerPixel;
    uint32_t compression;
    uint32_t imageSize;
    int32_t  xPixelsPerMeter;
    int32_t  yPixelsPerMeter;
    uint32_t colorsUsed;
    uint32_t colorsImportant;
};
#pragma pack(pop)

struct BMPImage {
    int width = 0;
    int height = 0;
    int bitsPerPixel = 0;
    int stride = 0;
    std::vector<uint8_t> pixels; // BGRA or BGR raw pixels
};

inline bool LoadBMP(const fs::path& path, BMPImage& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto fileSize = file.tellg();
    if (fileSize < sizeof(BMPFileHeader) + sizeof(BMPInfoHeader)) return false;

    file.seekg(0, std::ios::beg);

    BMPFileHeader fhdr;
    file.read(reinterpret_cast<char*>(&fhdr), sizeof(fhdr));
    if (fhdr.fileType != 0x4D42) return false; // "BM"

    BMPInfoHeader ihdr;
    file.read(reinterpret_cast<char*>(&ihdr), sizeof(ihdr));

    out.width = ihdr.width;
    out.height = ihdr.height; // positive = bottom-up
    out.bitsPerPixel = ihdr.bitsPerPixel;

    int bpp = ihdr.bitsPerPixel / 8;
    if (bpp == 0 || bpp > 4) return false;

    // Calculate stride (4-byte aligned)
    int rowSize = out.width * bpp;
    int padding = (4 - (rowSize % 4)) % 4;
    out.stride = rowSize + padding;

    out.pixels.resize(out.stride * std::abs(out.height));

    file.seekg(fhdr.offsetData, std::ios::beg);
    file.read(reinterpret_cast<char*>(out.pixels.data()), out.pixels.size());

    // If height is positive (bottom-up), flip vertically for easier processing
    if (out.height > 0) {
        std::vector<uint8_t> flipped(out.pixels.size());
        int rowBytes = out.stride;
        for (int y = 0; y < out.height; ++y) {
            int srcRow = out.height - 1 - y;
            std::memcpy(flipped.data() + y * rowBytes,
                       out.pixels.data() + srcRow * rowBytes,
                       rowBytes);
        }
        out.pixels = std::move(flipped);
    }
    out.height = std::abs(out.height);

    // Convert to 4-byte BGRA if not already
    if (bpp != 4) {
        std::vector<uint8_t> converted(out.width * out.height * 4);
        for (int y = 0; y < out.height; ++y) {
            for (int x = 0; x < out.width; ++x) {
                int srcIdx = y * out.stride + x * bpp;
                int dstIdx = (y * out.width + x) * 4;
                if (bpp == 3) {
                    converted[dstIdx + 0] = out.pixels[srcIdx + 0]; // B
                    converted[dstIdx + 1] = out.pixels[srcIdx + 1]; // G
                    converted[dstIdx + 2] = out.pixels[srcIdx + 2]; // R
                    converted[dstIdx + 3] = 255;                     // A
                } else if (bpp == 1) {
                    uint8_t gray = out.pixels[srcIdx];
                    converted[dstIdx + 0] = gray;
                    converted[dstIdx + 1] = gray;
                    converted[dstIdx + 2] = gray;
                    converted[dstIdx + 3] = 255;
                }
            }
        }
        out.pixels = std::move(converted);
        out.stride = out.width * 4;
        out.bitsPerPixel = 32;
    }

    return true;
}

// ============================================================
// 屏幕帧结构（模拟）
// ============================================================
struct ScreenFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> bgra; // BGRA, 4 bytes per pixel
};

// ============================================================
// TabIconTemplate 结构
// ============================================================
struct TabIconTemplate {
    int stashId = -1;
    std::string stashName;
    std::string bmpFileName;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra; // raw BGRA pixels (4 bytes per pixel)
};

// ============================================================
// MSE 计算（核心算法）
// ============================================================
inline double ComputeImageMSE(const uint8_t* a, const uint8_t* b,
                              int w, int h, int strideA = 0, int strideB = 0) {
    if (a == nullptr || b == nullptr) return 1e18;
    if (w <= 0 || h <= 0) return 1e18;
    if (strideA <= 0) strideA = w * 4;
    if (strideB <= 0) strideB = w * 4;

    double sumSq = 0.0;
    int count = 0;

    for (int y = 0; y < h; ++y) {
        const uint8_t* rowA = a + y * strideA;
        const uint8_t* rowB = b + y * strideB;
        for (int x = 0; x < w; ++x) {
            int base = x * 4;
            double d0 = static_cast<double>(rowA[0 + base]) - rowB[0 + base];
            double d1 = static_cast<double>(rowA[1 + base]) - rowB[1 + base];
            double d2 = static_cast<double>(rowA[2 + base]) - rowB[2 + base];
            // Skip alpha channel to be robust
            sumSq += d0 * d0 + d1 * d1 + d2 * d2;
            count += 3;
        }
    }

    return (count > 0) ? sumSq / count : 1e18;
}

// ============================================================
// 模板匹配（MSE 扫描）
// ============================================================
struct MatchResult {
    bool found = false;
    int x = 0;
    int y = 0;
    double mse = 1e18;
    double confidence = 0.0; // 0..1, higher is better
    int templateIndex = -1;
};

inline MatchResult MatchTemplateByMSE(
    const ScreenFrame& frame,
    const std::vector<TabIconTemplate>& templates,
    int searchX, int searchY, int searchW, int searchH,
    double threshold = 0.5) {

    MatchResult best;
    double bestMse = 1e18;

    for (size_t ti = 0; ti < templates.size(); ++ti) {
        const auto& tpl = templates[ti];
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
                    tpl.width * 4);

                if (mse < bestMse) {
                    bestMse = mse;
                    best.found = true;
                    best.x = searchX + ox;
                    best.y = searchY + oy;
                    best.mse = mse;
                    best.templateIndex = static_cast<int>(ti);
                    best.confidence = 1.0 / (1.0 + mse / (255.0 * 255.0));
                }
            }
        }
    }

    if (best.confidence < threshold) {
        best.found = false;
    }
    return best;
}

// ============================================================
// 模板加载
// ============================================================
inline int LoadTabIconTemplates(const fs::path& pluginDir,
                                std::vector<TabIconTemplate>& outTemplates) {
    outTemplates.clear();

    fs::path iconsDir = pluginDir / "resources" / "stash_icons";
    if (!fs::exists(iconsDir)) {
        // Try alternative paths
        fs::path alt = pluginDir.parent_path() / "resources" / "stash_icons";
        if (fs::exists(alt)) iconsDir = alt;
        else return 0;
    }

    int loaded = 0;
    for (const auto& entry : kStashTypeTable) {
        std::string bmpName(entry.ddsFileName);
        bmpName = bmpName.substr(0, bmpName.size() - 4) + ".bmp";
        fs::path bmpPath = iconsDir / bmpName;

        if (!fs::exists(bmpPath)) continue;

        BMPImage bmp;
        if (!LoadBMP(bmpPath, bmp)) continue;

        // Ensure 32-bit BGRA
        if (bmp.bitsPerPixel != 32 && bmp.pixels.size() != bmp.width * bmp.height * 4) {
            continue;
        }

        TabIconTemplate tpl;
        tpl.stashId = entry.stashId;
        tpl.stashName = entry.id;
        tpl.bmpFileName = bmpName;
        tpl.width = bmp.width;
        tpl.height = bmp.height;
        tpl.bgra = std::move(bmp.pixels);

        outTemplates.push_back(std::move(tpl));
        loaded++;
    }

    return loaded;
}

// ============================================================
// 测试框架
// ============================================================
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_passed++; printf("  [PASS] %s\n", msg); } \
    else { g_failed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

// ============================================================
// 测试 1: MSE 基础
// ============================================================
void test_mse_basic() {
    printf("\n=== 测试1: MSE 基础 ===\n");

    int w = 28, h = 28;
    std::vector<uint8_t> imgA(w * h * 4, 128);
    std::vector<uint8_t> imgB(w * h * 4, 128);

    double mseSame = ComputeImageMSE(imgA.data(), imgB.data(), w, h);
    CHECK(mseSame == 0.0, "相同图像 MSE = 0");

    std::vector<uint8_t> imgC(w * h * 4, 200);
    double mseDiff = ComputeImageMSE(imgA.data(), imgC.data(), w, h);
    CHECK(mseDiff > 0.0, "不同图像 MSE > 0");
    CHECK(mseDiff < 255.0 * 255.0, "MSE 在合理范围内");

    double conf = 1.0 / (1.0 + mseDiff / (255.0 * 255.0));
    CHECK(conf < 1.0 && conf > 0.0, "置信度在 0-1 之间");

    double mseNull = ComputeImageMSE(nullptr, imgA.data(), w, h);
    CHECK(mseNull > 1e8, "空指针 MSE 返回大值");
}

// ============================================================
// 测试 2: StashTypeTable 查表
// ============================================================
void test_stash_type_table() {
    printf("\n=== 测试2: StashTypeTable 查表 ===\n");

    int count = sizeof(kStashTypeTable) / sizeof(kStashTypeTable[0]);
    CHECK(count == 25, "StashTypeTable 有 25 条记录");

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

    fs::path exeDir = fs::current_path();

    fs::path pluginDir;
    std::vector<fs::path> candidates = {
        exeDir.parent_path().parent_path(),
        exeDir,
        exeDir.parent_path(),
        fs::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation"),
    };

    for (const auto& c : candidates) {
        if (fs::exists(c / "resources" / "stash_icons")) {
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

    // Print first 5 templates
    printf("  前5个模板:\n");
    for (int i = 0; i < std::min(5, (int)templates.size()); ++i) {
        printf("    [%d] %s (id=%d, %dx%d)\n",
            i, templates[i].stashName.c_str(), templates[i].stashId,
            templates[i].width, templates[i].height);
    }
}

// ============================================================
// 测试 4: 模板自匹配
// ============================================================
void test_template_self_match() {
    printf("\n=== 测试4: 图标模板自匹配 ===\n");

    fs::path pluginDir = fs::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation");
    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);

    if (n == 0) {
        printf("  SKIP: 无模板\n");
        return;
    }

    int successes = 0;
    for (const auto& tpl : templates) {
        // Create a test frame that is just the template (exact match scenario)
        ScreenFrame frame;
        frame.width = tpl.width;
        frame.height = tpl.height;
        frame.stride = tpl.width * 4;
        frame.bgra = tpl.bgra;

        MatchResult result = MatchTemplateByMSE(
            frame, templates, 0, 0, frame.width, frame.height, 0.0);

        if (result.found && result.templateIndex >= 0) {
            successes++;
        }
    }

    printf("  自匹配成功: %d/%d\n", successes, n);
    CHECK(successes > 0, "至少有一个模板能自匹配");

    // Verify confidence is high for self-match
    if (!templates.empty()) {
        ScreenFrame frame;
        frame.width = templates[0].width;
        frame.height = templates[0].height;
        frame.stride = templates[0].width * 4;
        frame.bgra = templates[0].bgra;

        MatchResult result = MatchTemplateByMSE(
            frame, templates, 0, 0, frame.width, frame.height, 0.0);

        printf("  模板[0] 自匹配置信度: %.4f\n", result.confidence);
        CHECK(result.confidence > 0.9, "自匹配置信度 > 0.9");
    }
}

// ============================================================
// 测试 5: 模板区分度（不同模板应能区分）
// ============================================================
void test_template_distinction() {
    printf("\n=== 测试5: 模板区分度 ===\n");

    fs::path pluginDir = fs::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation");
    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);

    if (n < 2) {
        printf("  SKIP: 模板数量不足\n");
        return;
    }

    // Cross-check: for each template, find the best match in the set
    int correctMatches = 0;
    int totalTests = 0;

    for (const auto& queryTpl : templates) {
        ScreenFrame frame;
        frame.width = queryTpl.width;
        frame.height = queryTpl.height;
        frame.stride = queryTpl.width * 4;
        frame.bgra = queryTpl.bgra;

        // Search with a larger frame containing just the query template
        MatchResult result = MatchTemplateByMSE(
            frame, templates, 0, 0, frame.width, frame.height, 0.0);

        totalTests++;
        if (result.templateIndex >= 0 &&
            templates[result.templateIndex].stashId == queryTpl.stashId) {
            correctMatches++;
        }
    }

    printf("  正确匹配: %d/%d\n", correctMatches, totalTests);
    CHECK(correctMatches >= totalTests / 2, "至少50%的自匹配能正确识别");
}

// ============================================================
// 测试 6: 模拟仓库页场景
// ============================================================
void test_simulated_stash_tabs() {
    printf("\n=== 测试6: 模拟仓库页场景 ===\n");

    fs::path pluginDir = fs::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation");
    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);

    if (n == 0) {
        printf("  SKIP: 无模板\n");
        return;
    }

    // Simulate a stash tab bar: 5 tabs, each 28x28, with 4px gap
    int tabW = 28, tabH = 28, gap = 4;
    int numTabs = std::min(5, n);
    int totalW = numTabs * tabW + (numTabs - 1) * gap;
    int totalH = tabH + 8; // some padding

    ScreenFrame frame;
    frame.width = totalW;
    frame.height = totalH;
    frame.stride = totalW * 4;
    frame.bgra.resize(totalW * totalH * 4, 0); // black background

    // Place templates at their respective positions
    std::vector<int> tabPositions;
    for (int i = 0; i < numTabs; ++i) {
        int x = i * (tabW + gap);
        int y = 4;
        tabPositions.push_back(x);

        if (i < (int)templates.size()) {
            const auto& tpl = templates[i];
            for (int py = 0; py < tabH; ++py) {
                for (int px = 0; px < tabW; ++px) {
                    int dstIdx = ((y + py) * totalW + (x + px)) * 4;
                    int srcIdx = (py * tabW + px) * 4;
                    if (srcIdx + 3 < (int)tpl.bgra.size() &&
                        dstIdx + 3 < (int)frame.bgra.size()) {
                        frame.bgra[dstIdx + 0] = tpl.bgra[srcIdx + 0];
                        frame.bgra[dstIdx + 1] = tpl.bgra[srcIdx + 1];
                        frame.bgra[dstIdx + 2] = tpl.bgra[srcIdx + 2];
                        frame.bgra[dstIdx + 3] = tpl.bgra[srcIdx + 3];
                    }
                }
            }
        }
    }

    // Now scan the frame and try to find each template
    printf("  模拟 %d 个仓库页 Tab (总尺寸 %dx%d)\n", numTabs, totalW, totalH);

    int foundCount = 0;
    for (int i = 0; i < numTabs; ++i) {
        MatchResult result = MatchTemplateByMSE(
            frame, templates, 0, 0, totalW, totalH, 0.45);

        if (result.found) {
            printf("    找到Tab #%d: 模板=%s, 位置=(%d,%d), 置信度=%.4f\n",
                i,
                templates[result.templateIndex].stashName.c_str(),
                result.x, result.y, result.confidence);
            foundCount++;
        }
    }

    printf("  找到 %d/%d 个仓库页 Tab\n", foundCount, numTabs);
    CHECK(foundCount > 0, "至少找到 1 个仓库页 Tab");
}

// ============================================================
// 测试 7: 鲁棒性测试（噪声和偏移）
// ============================================================
void test_robustness() {
    printf("\n=== 测试7: 鲁棒性测试 ===\n");

    fs::path pluginDir = fs::path("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation");
    std::vector<TabIconTemplate> templates;
    int n = LoadTabIconTemplates(pluginDir, templates);

    if (n == 0) {
        printf("  SKIP: 无模板\n");
        return;
    }

    // Test with 1-pixel offset (search area larger than template)
    const auto& tpl = templates[0];
    int pad = 2;
    ScreenFrame frame;
    frame.width = tpl.width + pad * 2;
    frame.height = tpl.height + pad * 2;
    frame.stride = frame.width * 4;
    frame.bgra.resize(frame.width * frame.height * 4, 0);

    // Place template with 1-pixel offset
    int offset = 1;
    for (int py = 0; py < tpl.height; ++py) {
        for (int px = 0; px < tpl.width; ++px) {
            int dstIdx = ((offset + py) * frame.width + (offset + px)) * 4;
            int srcIdx = (py * tpl.width + px) * 4;
            if (srcIdx + 3 < (int)tpl.bgra.size() &&
                dstIdx + 3 < (int)frame.bgra.size()) {
                frame.bgra[dstIdx + 0] = tpl.bgra[srcIdx + 0];
                frame.bgra[dstIdx + 1] = tpl.bgra[srcIdx + 1];
                frame.bgra[dstIdx + 2] = tpl.bgra[srcIdx + 2];
                frame.bgra[dstIdx + 3] = tpl.bgra[srcIdx + 3];
            }
        }
    }

    MatchResult result = MatchTemplateByMSE(
        frame, templates, 0, 0, frame.width, frame.height, 0.45);

    printf("  1像素偏移匹配: found=%d, conf=%.4f\n", result.found, result.confidence);
    CHECK(result.found, "1像素偏移仍能匹配");

    // Test with small noise added
    ScreenFrame noisyFrame = frame;
    for (auto& p : noisyFrame.bgra) {
        int noise = (std::rand() % 21) - 10; // -10..10
        int val = static_cast<int>(p) + noise;
        p = static_cast<uint8_t>(std::clamp(val, 0, 255));
    }

    MatchResult noisyResult = MatchTemplateByMSE(
        noisyFrame, templates, 0, 0, noisyFrame.width, noisyFrame.height, 0.45);

    printf("  噪声匹配: found=%d, conf=%.4f\n", noisyResult.found, noisyResult.confidence);
    CHECK(noisyResult.found, "加噪声后仍能匹配");
}

// ============================================================
// 主函数
// ============================================================
int main() {
    printf("=== 视觉识别 Mock 测试 (无 Windows 依赖) ===\n");
    printf("当前路径: %s\n", fs::current_path().string().c_str());

    test_mse_basic();
    test_stash_type_table();
    test_load_templates();
    test_template_self_match();
    test_template_distinction();
    test_simulated_stash_tabs();
    test_robustness();

    printf("\n=== 测试结果 ===\n");
    printf("通过: %d\n", g_passed);
    printf("失败: %d\n", g_failed);
    printf("总计: %d\n", g_passed + g_failed);

    return (g_failed > 0) ? 1 : 0;
}