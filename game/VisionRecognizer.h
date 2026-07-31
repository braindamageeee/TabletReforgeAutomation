// VisionRecognizer.h — 基于屏幕截图+OCR的视觉识别
//
// 允许引入 OpenCV (core/imgproc/imgcodecs/highgui) + Tesseract OCR
// 作为 SDK Inventory API 的辅助验证与 Tab 定位手段
//
// 设计要点：
//   - 仅当 USE_VISION_RECOGNITION 宏被定义时才启用（防止未装 OpenCV/Tesseract 时直接报错）
//   - 使用 Windows GDI BitBlt 做屏幕截获，零第三方依赖即可拿到原始像素
//   - 可单独调用 CaptureScreenToBmp() 输出 BMP 用于调试
//   - OCR 部分需要外部安装 Tesseract DLL + chi_sim.traineddata
//
#pragma once

#include "../sdk/PluginSDK.h"
#include "../config/CalibData.h"
#include "StashTypeTable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace VisionRecogNS {

// ============================================================
// 1. 屏幕截获：BitBlt 抓取指定屏幕区域
// ============================================================

// BGRA 原始像素缓存
struct ScreenFrame {
    int width = 0;
    int height = 0;
    int stride = 0;  // 每行字节数（对齐到 4 字节）
    std::vector<uint8_t> bgra;  // BGRA 顺序，逐行扫描
    double captureTimeMs = 0;
};

// 从指定屏幕坐标矩形抓取 BGRA 像素
inline bool CaptureScreenRegion(int screenX, int screenY, int width, int height, ScreenFrame& out) {
    if (width <= 0 || height <= 0) return false;

    HDC hdcScreen = ::GetDC(nullptr);
    if (!hdcScreen) return false;

    HDC hdcMem = ::CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ::ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    HBITMAP hBmp = ::CreateCompatibleBitmap(hdcScreen, width, height);
    if (!hBmp) {
        ::DeleteDC(hdcMem);
        ::ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    ::SelectObject(hdcMem, hBmp);
    ::BitBlt(hdcMem, 0, 0, width, height, hdcScreen, screenX, screenY, SRCCOPY | CAPTUREBLT);

    // 读取像素到 BGRA
    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = -height;  // 负值：从上到下（屏幕坐标方向）
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    int stride = ((width * 4) + 3) & ~3;
    out.bgra.resize(stride * height, 0);
    out.width = width;
    out.height = height;
    out.stride = stride;

    ::GetDIBits(hdcMem, hBmp, 0, height, out.bgra.data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);

    ::DeleteObject(hBmp);
    ::DeleteDC(hdcMem);
    ::ReleaseDC(nullptr, hdcScreen);

    return true;
}

// 截获整个游戏窗口（通过 PluginSDK 获取窗口位置）
inline bool CaptureGameWindow(const PluginSDK::Context* ctx, ScreenFrame& out) {
    if (!ctx) return false;

    HWND hWnd = ctx->Game.GetGameWindow();
    if (!hWnd) return false;

    RECT rc = {};
    if (!::GetClientRect(hWnd, &rc)) return false;
    POINT pt = { rc.left, rc.top };
    if (!::ClientToScreen(hWnd, &pt)) return false;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    return CaptureScreenRegion(pt.x, pt.y, w, h, out);
}

// 保存为 BMP 文件（便于调试/交给 Tesseract 或 OpenCV）
inline bool SaveFrameToBmp(const ScreenFrame& frame, const std::filesystem::path& outPath) {
    if (frame.bgra.empty() || frame.width <= 0 || frame.height <= 0) return false;

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42;  // "BM"
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + frame.stride * frame.height;

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = frame.width;
    bih.biHeight = frame.height;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    std::ofstream out(outPath, std::ios::binary);
    if (!out) return false;

    out.write((const char*)&bfh, sizeof(bfh));
    out.write((const char*)&bih, sizeof(bih));
    out.write((const char*)frame.bgra.data(), frame.stride * frame.height);
    return out.good();
}

// 从屏幕坐标矩形抓一帧（便捷封装）
inline bool CaptureScreenRect(const PluginSDK::Context* /*ctx*/,
                              int screenX, int screenY, int w, int h,
                              ScreenFrame& out) {
    return CaptureScreenRegion(screenX, screenY, w, h, out);
}

// ============================================================
// 2. 调色板/亮度工具（无需 OpenCV）
// ============================================================

// 获取指定像素（x 列, y 行）的 BGRA 分量
inline void GetPixelBGRA(const ScreenFrame& frame, int x, int y,
                         uint8_t& b, uint8_t& g, uint8_t& r, uint8_t& a) {
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
        b = g = r = a = 0;
        return;
    }
    size_t offset = (size_t)y * frame.stride + (size_t)x * 4;
    b = frame.bgra[offset + 0];
    g = frame.bgra[offset + 1];
    r = frame.bgra[offset + 2];
    a = frame.bgra[offset + 3];
}

// 计算子区域平均亮度 + 像素直方图
struct RegionStats {
    int pixelCount = 0;
    double avgBrightness = 0;
    std::vector<int> hueHistogram;  // 简单的红色/绿色/蓝色/白色/黑色计数
};

inline RegionStats ComputeRegionStats(const ScreenFrame& frame,
                                      int x0, int y0, int x1, int y1) {
    RegionStats s;
    int sum = 0;
    s.hueHistogram.assign(5, 0);  // R/G/B/W/K
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            uint8_t b, g, r, a;
            GetPixelBGRA(frame, x, y, b, g, r, a);
            int br = (r + g + b) / 3;
            sum += br;
            s.pixelCount++;

            // 颜色分类（粗略）
            int maxc = (std::max)({(int)r, (int)g, (int)b});
            int minc = (std::min)({(int)r, (int)g, (int)b});
            if (maxc < 40) s.hueHistogram[4]++;       // 黑
            else if (maxc - minc < 25) s.hueHistogram[3]++;  // 白/灰
            else if (r > g && r > b) s.hueHistogram[0]++;   // 红
            else if (g > r && g > b) s.hueHistogram[1]++;   // 绿
            else s.hueHistogram[2]++;                        // 蓝
        }
    }
    s.avgBrightness = s.pixelCount > 0 ? (double)sum / s.pixelCount : 0;
    return s;
}

// ============================================================
// 3. Tab 区域粗定位（基于游戏分辨率比例）
// ============================================================

// 按游戏窗口分辨率比例推算 "Tab 区域"
// POE2 仓库 Tab 通常在仓库窗口顶部，占约 28px 高度
struct StashTabRegion {
    int screenX = 0;
    int screenY = 0;
    int width = 0;
    int height = 0;
    int tabCount = 0;
    int tabWidthPx = 0;
};

inline bool EstimateStashTabRegion(const PluginSDK::Context* ctx, StashTabRegion& out) {
    if (!ctx) return false;
    HWND hWnd = ctx->Game.GetGameWindow();
    if (!hWnd) return false;

    RECT rc = {};
    if (!::GetClientRect(hWnd, &rc)) return false;
    POINT pt = { rc.left, rc.top };
    if (!::ClientToScreen(hWnd, &pt)) return false;

    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;

    // 仓库通常在屏幕中央，宽度约 48%，左上角距顶约 18%
    // Tab 栏位于仓库顶部，高度约 28-32px
    int stashW = (int)(clientW * 0.48f);
    int stashH = (int)(clientH * 0.42f);
    int stashX = pt.x + (clientW - stashW) / 2;
    int stashY = pt.y + (int)(clientH * 0.18f);

    out.screenX = stashX;
    out.screenY = stashY;
    out.width = stashW;
    out.height = 32;  // Tab 栏高度约 32px

    // 估算 Tab 数量：按 56px/Tab 推算
    out.tabWidthPx = 56;
    out.tabCount = (std::max)(1, stashW / out.tabWidthPx);
    return true;
}

// ============================================================
// 4. 可选：OpenCV + Tesseract 识别（需要安装第三方库）
// ============================================================
//
// 若用户已安装 Tesseract (C:\Program Files\Tesseract-OCR) 与 OpenCV，
// 可在项目属性里添加预处理定义 USE_VISION_RECOGNITION，并链接相应 .lib。
//
// 由于本文件需要独立编译，这里只提供运行时动态加载的桥接函数，
// 未定义 USE_VISION_RECOGNITION 时这些函数直接返回空结果。
// ============================================================

struct OCRResult {
    bool success = false;
    std::string text;
    double confidence = 0;
};

#ifndef USE_VISION_RECOGNITION

// 默认：无 OpenCV/Tesseract，仅返回空
inline OCRResult RecognizeTextInRegion(const ScreenFrame& /*frame*/,
                                       int /*x*/, int /*y*/, int /*w*/, int /*h*/) {
    return OCRResult{};
}

#else
// 真正实现放在 VisionOCR.cpp 里；此处仅声明占位
OCRResult RecognizeTextInRegion(const ScreenFrame& frame, int x, int y, int w, int h);
#endif

// ============================================================
// 5. Stash Tab 图标模板匹配视觉识别
// ============================================================
//
// 从 Content.ggpk 的 data/balance/stashtype.datc64 中提取的 25 个 Tab 图标
// （Art/2DArt/UIImages/InGame/MTX/<X>TabIcon），已预先转为 28×28 BMP。
// 位于插件根目录的 resources/stash_icons/*.bmp
//
// 流程：加载 BMP 模板列表 → 截屏 Tab 栏 → OpenCV matchTemplate 逐模板匹配
// 识别当前激活 Tab（或存在的所有 Tab），返回屏幕坐标供点击。
// ============================================================

// 单个 Tab 图标模板
struct TabIconTemplate {
    int stashId = -1;
    std::string stashName;          // "CurrencyStash", "UniqueStash", ...
    std::string bmpFileName;        // "03_CurrencyStash.bmp"
    int width = 0;                  // 28
    int height = 0;                 // 28
    int screenOffX = 0;             // 屏幕坐标（相对 Tab 栏左上角）
    int screenOffY = 0;
    std::vector<uint8_t> bgra;      // 原始 BGRA 像素（每像素 4 字节）
};

// 加载 resources/stash_icons/ 下的所有 BMP 模板
// pluginDir: 插件根目录（resources/ 子目录所在）
// 返回加载的模板数量；模板存入 outTemplates（按 stashId 排序）
inline int LoadTabIconTemplates(const std::filesystem::path& pluginDir,
                                std::vector<TabIconTemplate>& outTemplates) {
    outTemplates.clear();
    using namespace TabletReforgeGame;

    // 从 StashTypeTable 预构造 {stashId, filename} 清单
    struct Item { int stashId; const char* bmpName; };
    std::vector<Item> items;
    items.reserve(kStashTypeTable.size());
    for (const auto& e : kStashTypeTable) {
        Item it;
        it.stashId = e.stashId;
        // ddsFileName 形如 "03_CurrencyStash.dds" → "03_CurrencyStash.bmp"
        std::string dds = e.ddsFileName;
        std::string bmp = dds.substr(0, dds.size() - 4) + ".bmp";
        it.bmpName = _strdup(bmp.c_str());
        items.push_back(it);
    }

    // 排序：stashId 升序
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.stashId < b.stashId; });

    // 尝试多个可能的路径（按优先级排序）
    std::vector<std::filesystem::path> candidatePaths;
    
    // 路径1: pluginDir/resources/stash_icons (标准部署 - Fixer插件目录)
    candidatePaths.push_back(pluginDir / "resources" / "stash_icons");
    
    // 路径2: 开发环境路径 F:\Trae\chuxue\Plugins\TabletReforgeAutomation\resources\stash_icons
    candidatePaths.push_back("F:\\Trae\\chuxue\\Plugins\\TabletReforgeAutomation\\resources\\stash_icons");
    
    // 路径3: Fixer部署路径 D:\PoeFixer\Plugins\TabletReforgeAutomation\resources\stash_icons
    candidatePaths.push_back("D:\\PoeFixer\\Plugins\\TabletReforgeAutomation\\resources\\stash_icons");
    
    // 路径4: pluginDir.parent_path()/resources/stash_icons (bin/Release 模式)
    if (pluginDir.has_parent_path()) {
        candidatePaths.push_back(pluginDir.parent_path() / "resources" / "stash_icons");
    }
    
    // 路径5: pluginDir.parent_path().parent_path()/resources/stash_icons (bin/Release/DLL 模式)
    if (pluginDir.has_parent_path() && pluginDir.parent_path().has_parent_path()) {
        candidatePaths.push_back(pluginDir.parent_path().parent_path() / "resources" / "stash_icons");
    }
    
    // 路径6: 尝试相对于当前工作目录的路径
    {
        wchar_t currentDir[MAX_PATH];
        ::GetCurrentDirectoryW(MAX_PATH, currentDir);
        if (wcslen(currentDir) > 0) {
            candidatePaths.push_back(std::filesystem::path(currentDir) / "resources" / "stash_icons");
            candidatePaths.push_back(std::filesystem::path(currentDir) / "Plugins" / "TabletReforgeAutomation" / "resources" / "stash_icons");
        }
    }

    // 选择第一个存在的路径
    std::filesystem::path iconsDir;
    bool foundPath = false;
    std::string pathDebugInfo;
    
    for (size_t idx = 0; idx < candidatePaths.size(); ++idx) {
        const auto& candidate = candidatePaths[idx];
        bool exists = std::filesystem::exists(candidate);
        bool hasBmpFiles = false;
        
        // 检查目录中是否有BMP文件
        if (exists) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(candidate, ec)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension();
                    if (ext == ".bmp" || ext == ".BMP") {
                        hasBmpFiles = true;
                        break;
                    }
                }
            }
        }
        
        char pathInfo[512];
        sprintf_s(pathInfo, "  [%zu] %s -> %s %s\n", 
            idx + 1,
            candidate.string().c_str(), 
            exists ? "存在" : "不存在",
            hasBmpFiles ? "(含BMP)" : "");
        pathDebugInfo += pathInfo;
        
        if (exists && hasBmpFiles && !foundPath) {
            iconsDir = candidate;
            foundPath = true;
        }
    }
    
    // 输出路径调试信息
    {
        char logMsg[4096];
        sprintf_s(logMsg, "[VisionRecog] LoadTabIconTemplates 路径检查 (共%zu个候选):\n%s  pluginDir=%s\n  选中路径: %s\n",
            candidatePaths.size(),
            pathDebugInfo.c_str(), pluginDir.string().c_str(),
            foundPath ? iconsDir.string().c_str() : "(未找到含BMP的目录)");
        OutputDebugStringA(logMsg);
    }
    
    if (!foundPath) {
        OutputDebugStringA("[VisionRecog] LoadTabIconTemplates: 未找到任何图标目录\n");
        return 0;
    }

    int loaded = 0;
    int failed = 0;
    int missing = 0;
    std::string failedDetails;
    
    for (const auto& it : items) {
        std::filesystem::path bmpPath = iconsDir / it.bmpName;
        
        // 检查文件是否存在
        if (!std::filesystem::exists(bmpPath)) {
            missing++;
            char detail[256];
            sprintf_s(detail, "    [缺失] %s\n", it.bmpName);
            failedDetails += detail;
            continue;
        }

        // 用 GDI 加载 BMP → BGRA 像素
        HBITMAP hBmp = (HBITMAP)::LoadImageW(nullptr, bmpPath.c_str(), IMAGE_BITMAP,
                                             0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (!hBmp) {
            failed++;
            char detail[256];
            sprintf_s(detail, "    [Load失败] %s (GetLastError=%lu)\n", 
                it.bmpName, ::GetLastError());
            failedDetails += detail;
            continue;
        }

        BITMAP bm = {};
        ::GetObject(hBmp, sizeof(bm), &bm);
        
        // 记录BMP信息用于调试
        {
            char bmpInfo[256];
            sprintf_s(bmpInfo, "    [BMP] %s: %dx%d, %dbpp\n", 
                it.bmpName, bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
            // 仅首次或失败时输出详细信息
            if (loaded < 3 || bm.bmBitsPixel != 32) {
                OutputDebugStringA(bmpInfo);
            }
        }
        
        // 检查BMP格式（允许32位和24位转换）
        int targetBitCount = 32;
        if (bm.bmBitsPixel == 24) {
            // 24位BMP需要转换为32位BGRA
            targetBitCount = 32;
        } else if (bm.bmBitsPixel != 32) {
            char errMsg[256];
            sprintf_s(errMsg, "    [格式错误] %s: 不支持的色深 %dbpp (需要24或32位)\n", 
                it.bmpName, bm.bmBitsPixel);
            OutputDebugStringA(errMsg);
            ::DeleteObject(hBmp);
            failed++;
            char detail[256];
            sprintf_s(detail, "    [格式错误] %s\n", it.bmpName);
            failedDetails += detail;
            continue;
        }

        // 用 GetDIBits 获取像素数据（DIB 顶部向下）
        BITMAPINFOHEADER bih = {};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = bm.bmWidth;
        bih.biHeight = -bm.bmHeight;  // 负值 → 顶向下
        bih.biPlanes = 1;
        bih.biBitCount = targetBitCount;
        bih.biCompression = BI_RGB;

        TabIconTemplate tpl;
        tpl.stashId = it.stashId;
        const StashTypeEntry* entry = FindStashTypeByStashId(it.stashId);
        if (entry) tpl.stashName = entry->id;
        tpl.bmpFileName = it.bmpName;
        tpl.width = bm.bmWidth;
        tpl.height = bm.bmHeight;
        tpl.bgra.resize((size_t)tpl.width * tpl.height * 4, 0);

        HDC hdcMem = ::CreateCompatibleDC(nullptr);
        if (hdcMem) {
            int scanLines = ::GetDIBits(hdcMem, hBmp, 0, (UINT)bm.bmHeight,
                        tpl.bgra.data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);
            
            if (scanLines == 0) {
                char errMsg[256];
                sprintf_s(errMsg, "    [GetDIBits失败] %s: scanLines=0, GetLastError=%lu\n", 
                    it.bmpName, ::GetLastError());
                OutputDebugStringA(errMsg);
                ::DeleteDC(hdcMem);
                ::DeleteObject(hBmp);
                failed++;
                char detail[256];
                sprintf_s(detail, "    [GetDIBits失败] %s\n", it.bmpName);
                failedDetails += detail;
                continue;
            }
            ::DeleteDC(hdcMem);
        } else {
            OutputDebugStringA("    [错误] CreateCompatibleDC 返回 NULL\n");
            ::DeleteObject(hBmp);
            failed++;
            char detail[256];
            sprintf_s(detail, "    [DC创建失败] %s\n", it.bmpName);
            failedDetails += detail;
            continue;
        }

        outTemplates.push_back(std::move(tpl));
        loaded++;
        ::DeleteObject(hBmp);
    }
    
    // 输出加载结果（含详细失败信息）
    {
        char logMsg[4096];
        sprintf_s(logMsg, "[VisionRecog] LoadTabIconTemplates 加载完成:\n  成功: %d\n  失败: %d\n  缺失: %d\n  总计: %zu\n",
            loaded, failed, missing, items.size());
        OutputDebugStringA(logMsg);
        
        if (!failedDetails.empty()) {
            char detailMsg[4096];
            sprintf_s(detailMsg, "[VisionRecog] 失败详情:\n%s", failedDetails.c_str());
            OutputDebugStringA(detailMsg);
        }
    }
    
    return loaded;
}

// ============================================================
// 6. OpenCV 模板匹配（需 USE_VISION_RECOGNITION）
// ============================================================

struct MatchResult {
    bool matched = false;
    int stashId = -1;
    std::string stashName;
    int screenX = 0;
    int screenY = 0;
    double confidence = 0;  // 0..1, 越高越像
};

#ifdef USE_VISION_RECOGNITION
// OpenCV 头（仅在启用宏时引入）
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

// 在屏幕截图 frame 中，搜索所有 templates，返回最佳匹配
// searchX/Y/W/H: 要搜索的屏幕区域（一般为 Tab 栏）
// 返回置信度 >= threshold 的最佳匹配；若无匹配 matched=false
inline MatchResult MatchTemplateBest(const ScreenFrame& frame,
                                    const std::vector<TabIconTemplate>& templates,
                                    int searchX, int searchY, int searchW, int searchH,
                                    double threshold = 0.6) {
    MatchResult result;
    if (templates.empty() || searchW <= 0 || searchH <= 0 || frame.bgra.empty()) {
        return result;
    }
    // 将屏幕区域 BGRA → cv::Mat (H×W×4, BGRA)
    cv::Mat screen(searchH, searchW, CV_8UC4, (void*)(frame.bgra.data()
                   + searchY * frame.stride + searchX * 4, frame.stride);

    // 灰度化屏幕 + 模板（对亮度差异容忍度更高）
    cv::Mat screenGray;
    cv::cvtColor(screen, screenGray, cv::COLOR_BGRA2GRAY);

    MatchResult best;
    double bestScore = threshold;

    for (const auto& tpl : templates) {
        if (tpl.width <= 0 || tpl.height <= 0) continue;
        if (tpl.width > searchW || tpl.height > searchH) continue;

        cv::Mat tmpl(tpl.height, tpl.width, CV_8UC4, (void*)tpl.bgra.data());
        cv::Mat tmplGray;
        cv::cvtColor(tmpl, tmplGray, cv::COLOR_BGRA2GRAY);

        cv::Mat resultMat;
        // TM_CCOEFF_NORMED: 越接近 1 越匹配（容忍亮度偏移）
        cv::matchTemplate(screenGray, tmplGray, resultMat, cv::TM_CCOEFF_NORMED);

        double minVal, maxVal;
        cv::Point minLoc, maxLoc;
        cv::minMaxLoc(resultMat, &minVal, &maxVal, &minLoc, &maxLoc);

        if (maxVal > bestScore) {
            bestScore = maxVal;
            best.matched = true;
            best.stashId = tpl.stashId;
            best.stashName = tpl.stashName;
            // 屏幕坐标：搜索区域左上角 + 匹配位置 + 模板中心
            best.screenX = searchX + maxLoc.x + tpl.width / 2;
            best.screenY = searchY + maxLoc.y + tpl.height / 2;
            best.confidence = maxVal;
        }
    }
    return best;
}
#else
// 未启用 USE_VISION_RECOGNITION 时：返回空匹配
inline MatchResult MatchTemplateBest(const ScreenFrame& /*frame*/,
                                    const std::vector<TabIconTemplate>& /*templates*/,
                                    int /*searchX*/, int /*searchY*/,
                                    int /*searchW*/, int /*searchH*/,
                                    double /*threshold*/ = 0.6) {
    return MatchResult{};
}
#endif

// ============================================================
// 6.5 用户截图模板匹配（用于切换仓库Tab）
// ============================================================

// 从图片文件加载截图模板（支持 BMP/JPG/PNG/GIF/TIFF 等格式）
#ifdef USE_VISION_RECOGNITION

// GDI+ 初始化包装（线程安全，只初始化一次）
struct GdiPlusInitializer {
    ULONG_PTR gdiplusToken = 0;
    GdiPlusInitializer() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr);
    }
    ~GdiPlusInitializer() {
        if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
    }
};

// 从 GDI+ Bitmap 获取 BGRA 像素数据
inline bool GdiplusBitmapToBgra(Gdiplus::Bitmap* bmp, TabIconTemplate& outTemplate) {
    if (!bmp) return false;
    
    int width = bmp->GetWidth();
    int height = bmp->GetHeight();
    if (width <= 0 || height <= 0) return false;
    
    // 获取像素数据（锁定为 32bpp BGRA）
    Gdiplus::PixelFormat format = bmp->GetPixelFormat();
    
    // 创建一个 32bpp 的副本用于统一处理
    Gdiplus::Bitmap* bmp32 = nullptr;
    if (format != PixelFormat32bppARGB) {
        bmp32 = new Gdiplus::Bitmap(width, height, PixelFormat32bppARGB);
        Gdiplus::Graphics g(bmp32);
        g.DrawImage(bmp, 0, 0, width, height);
        bmp = bmp32;
    }
    
    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData bmpData;
    bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData);
    
    // 转换为 BGRA (Windows 顺序为 BGRA)
    int stride = ((width * 4) + 3) & ~3;
    outTemplate.width = width;
    outTemplate.height = height;
    outTemplate.bgra.resize(stride * height);
    
    uint8_t* src = static_cast<uint8_t*>(bmpData.Scan0);
    int srcStride = bmpData.Stride;
    
    // GDI+ Bitmap 是从上到下存储的
    for (int y = 0; y < height; y++) {
        memcpy(outTemplate.bgra.data() + y * stride, src + y * srcStride, width * 4);
    }
    
    bmp->UnlockBits(&bmpData);
    delete bmp32;
    return true;
}

inline bool LoadScreenshotTemplate(const std::filesystem::path& imgPath,
                                    TabIconTemplate& outTemplate) {
    std::string ext = imgPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    // BMP: 走快速路径
    if (ext == ".bmp") {
        // 读取BMP文件
        std::ifstream file(imgPath, std::ios::binary);
        if (!file.is_open()) return false;
        
        BITMAPFILEHEADER bfh;
        file.read(reinterpret_cast<char*>(&bfh), sizeof(BITMAPFILEHEADER));
        if (bfh.bfType != 0x4D42) return false;  // "BM"
        
        BITMAPINFOHEADER bih;
        file.read(reinterpret_cast<char*>(&bih), sizeof(BITMAPINFOHEADER));
        
        int width = bih.biWidth;
        int height = abs(bih.biHeight);
        int bpp = bih.biBitCount;
        
        if (width <= 0 || height <= 0) return false;
        
        int stride = ((width * 4) + 3) & ~3;
        std::vector<uint8_t> pixels(stride * height);
        
        if (bpp == 32) {
            // 32bpp 直接读取
            file.read(reinterpret_cast<char*>(pixels.data()), stride * height);
            if (!file.good() && !file.eof()) return false;
            
            outTemplate.width = width;
            outTemplate.height = height;
            outTemplate.bgra.resize(stride * height);
            for (int y = 0; y < height; y++) {
                int dstRow = y * stride;
                int srcRow = (height - 1 - y) * stride;
                memcpy(outTemplate.bgra.data() + dstRow, pixels.data() + srcRow, stride);
            }
        } else {
            // 非32bpp BMP: 回退到GDI+处理
            file.close();
            // 继续到 GDI+ 路径
            goto use_gdiplus;
        }
    } else {
        // 非 BMP 格式: JPG/PNG/GIF/TIFF 等 → 使用 GDI+
        use_gdiplus:
        {
            static GdiPlusInitializer init;  // 只初始化一次
            
            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(imgPath.wstring().c_str());
            if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
                if (bmp) delete bmp;
                OutputDebugStringA(("[VisionRecog] GDI+ 加载图片失败: " + imgPath.string() + "\n").c_str());
                return false;
            }
            
            bool ok = GdiplusBitmapToBgra(bmp, outTemplate);
            delete bmp;
            
            if (!ok) {
                OutputDebugStringA(("[VisionRecog] GDI+ 图片转换失败: " + imgPath.string() + "\n").c_str());
                return false;
            }
        }
    }
    
    // 设置模板名称
    std::string filename = imgPath.filename().string();
    outTemplate.stashName = filename;
    outTemplate.bmpFileName = filename;
    
    {
        char logMsg[256];
        sprintf_s(logMsg, "[VisionRecog] 截图模板加载成功: %s (%dx%d, %zu bytes)\n",
            filename.c_str(), outTemplate.width, outTemplate.height, outTemplate.bgra.size());
        OutputDebugStringA(logMsg);
    }
    
    return true;
}

// 使用用户提供的截图模板匹配当前屏幕
inline MatchResult MatchScreenshotTemplate(
    const std::filesystem::path& screenshotPath,
    int searchX, int searchY, int searchW, int searchH,
    double threshold = 0.5) {
    
    MatchResult empty;
    
    // 加载用户截图模板
    TabIconTemplate userTemplate;
    if (!LoadScreenshotTemplate(screenshotPath, userTemplate)) {
        OutputDebugStringA(("[VisionRecog] 加载截图模板失败: " + screenshotPath.string() + "\n").c_str());
        return empty;
    }
    
    // 截取屏幕搜索区域
    ScreenFrame screenFrame;
    if (!CaptureScreenRegion(searchX, searchY, searchW, searchH, screenFrame)) {
        OutputDebugStringA("[VisionRecog] 截取屏幕区域失败\n");
        return empty;
    }
    
    // 构建单模板列表
    std::vector<TabIconTemplate> templates = { userTemplate };
    
    // 执行模板匹配
    MatchResult result = MatchTemplateBest(screenFrame, templates, 
                                            searchX, searchY, searchW, searchH, threshold);
    
    if (result.matched) {
        char log[256];
        sprintf_s(log, "[VisionRecog] 截图匹配成功: %s, 置信度=%.4f, 位置=(%d,%d)\n",
            screenshotPath.filename().string().c_str(),
            result.confidence, result.screenX, result.screenY);
        OutputDebugStringA(log);
    }
    
    return result;
}

// 使用截图模板匹配屏幕上的仓库Tab（自动计算Tab栏位置）
inline MatchResult MatchStashTabByScreenshot(
    const PluginSDK::Context* ctx,
    const std::filesystem::path& screenshotPath,
    double threshold = 0.5) {
    
    MatchResult empty;
    if (!ctx) return empty;
    
    // 计算Tab栏位置
    int tabX, tabY, tabW, tabH;
    if (!ComputeTabBarRectFromGrid(ctx, tabX, tabY, tabW, tabH)) {
        return empty;
    }
    
    // 扩大搜索区域（Tab栏周围可能有子Tab）
    int searchX = tabX - 20;
    int searchY = tabY - 20;
    int searchW = tabW + 40;
    int searchH = tabH + 40;
    
    // 执行截图匹配
    MatchResult result = MatchScreenshotTemplate(screenshotPath, 
                                                  searchX, searchY, searchW, searchH, threshold);
    
    if (result.matched) {
        // 调整为屏幕绝对坐标
        result.screenX += searchX;
        result.screenY += searchY;
    }
    
    return result;
}
#else
// 未启用 USE_VISION_RECOGNITION 时的空实现
inline bool LoadScreenshotTemplate(const std::filesystem::path& /*bmpPath*/,
                                    TabIconTemplate& /*outTemplate*/) {
    return false;
}

inline MatchResult MatchScreenshotTemplate(
    const std::filesystem::path& /*screenshotPath*/,
    int /*searchX*/, int /*searchY*/, int /*searchW*/, int /*searchH*/,
    double /*threshold*/ = 0.5) {
    return MatchResult{};
}

inline MatchResult MatchStashTabByScreenshot(
    const PluginSDK::Context* /*ctx*/,
    const std::filesystem::path& /*screenshotPath*/,
    double /*threshold*/ = 0.5) {
    return MatchResult{};
}
#endif

// ============================================================
// 7. 组合：基于 GridScreenY 估算 Tab 栏 + 模板匹配
// ============================================================

// 从 SDK Inventory 的 Grid 位置反推 Tab 栏 Y 坐标
// 仓库 Tab 栏通常在物品网格正上方，间距约 40px
inline bool ComputeTabBarRectFromGrid(const PluginSDK::Context* ctx,
                                      int& outTabBarX, int& outTabBarY,
                                      int& outTabBarW, int& outTabBarH) {
    if (!ctx) return false;
    auto inv = ctx->Inventory.Get(0);  // 主背包（仓库）
    if (!inv.Address) return false;

    // Grid 宽度 = 总格子数 × 单格像素
    float gridW = (float)inv.TotalBoxesX * inv.Grid.CellSize;
    // Tab 栏宽度 = Grid 宽度 * 1.08（两侧稍宽出网格）
    float tabW = gridW * 1.08f;
    float tabX = inv.Grid.GridScreenX + (gridW - tabW) / 2.0f;
    // Tab 栏 Y = Grid 顶部 - 40px（实测值）
    float tabY = inv.Grid.GridScreenY - 40.0f;
    float tabH = 32.0f;  // Tab 栏高度 32px

    outTabBarX = (int)tabX;
    outTabBarY = (int)tabY;
    outTabBarW = (int)tabW;
    outTabBarH = (int)tabH;
    return true;
}

// 便捷函数：抓取 Tab 栏 → 模板匹配 → 返回最佳匹配
inline MatchResult MatchStashTabByIcon(const PluginSDK::Context* ctx,
                                       const std::vector<TabIconTemplate>& templates,
                                       double threshold = 0.6) {
    MatchResult empty;
    if (!ctx || templates.empty()) return empty;

    int tx, ty, tw, th;
    if (!ComputeTabBarRectFromGrid(ctx, tx, ty, tw, th)) return empty;

    // 保证搜索区域在屏幕内
    if (tw <= 0 || th <= 0) return empty;

    ScreenFrame frame;
    if (!CaptureScreenRegion(tx, ty, tw, th, frame)) return empty;

    return MatchTemplateBest(frame, templates, tx, ty, tw, th, threshold);
}

// ============================================================
// 8. 无 OpenCV 依赖的像素均方差匹配（回退方案）
// ============================================================
//
// 当 USE_VISION_RECOGNITION 未定义时，使用简单的逐像素 BGRA 均方差
// 来识别仓库 Tab 图标。精度略低于 matchTemplate，但无需第三方库。
//

// 计算两个等尺寸 BGRA 图像的平均绝对差（0 = 完美匹配）
inline double ComputeImageMSE(const uint8_t* a, const uint8_t* b,
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

// 计算两个等尺寸 BGRA 图像的归一化相关系数 (NCC)
// 范围 [-1, 1], 1=完全匹配, 对亮度/对比度变化不敏感
inline double ComputeImageNCC(const uint8_t* a, const uint8_t* b,
                              int w, int h, int strideA = 0, int strideB = 0) {
    if (!a || !b || w <= 0 || h <= 0) return -1.0;
    if (strideA == 0) strideA = w * 4;
    if (strideB == 0) strideB = w * 4;

    // 计算均值
    double meanA = 0, meanB = 0;
    int count = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* pa = a + y * strideA;
        const uint8_t* pb = b + y * strideB;
        for (int x = 0; x < w; ++x) {
            int off = x * 4;
            meanA += (double)(pa[off + 0] + pa[off + 1] + pa[off + 2]) / 3.0;
            meanB += (double)(pb[off + 0] + pb[off + 1] + pb[off + 2]) / 3.0;
            count++;
        }
    }
    if (count == 0) return -1.0;
    meanA /= count;
    meanB /= count;

    // 计算 NCC
    double num = 0, denA = 0, denB = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* pa = a + y * strideA;
        const uint8_t* pb = b + y * strideB;
        for (int x = 0; x < w; ++x) {
            int off = x * 4;
            double ax = (double)(pa[off + 0] + pa[off + 1] + pa[off + 2]) / 3.0 - meanA;
            double bx = (double)(pb[off + 0] + pb[off + 1] + pb[off + 2]) / 3.0 - meanB;
            num += ax * bx;
            denA += ax * ax;
            denB += bx * bx;
        }
    }
    if (denA < 1e-6 || denB < 1e-6) return -1.0;
    return num / sqrt(denA * denB);
}

// BGRA 缓冲区双线性缩放（用于模板尺寸与屏幕不匹配时）
inline std::vector<uint8_t> ResizeBilinearBGRA(const uint8_t* src, int srcW, int srcH,
                                                int dstW, int dstH, int srcStride = 0) {
    if (!src || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return {};
    if (srcStride == 0) srcStride = srcW * 4;
    std::vector<uint8_t> dst(dstW * dstH * 4, 0);

    for (int dy = 0; dy < dstH; ++dy) {
        double sy = (double)dy * (double)srcH / (double)dstH;
        int y0 = (int)sy;
        int y1 = (std::min)(y0 + 1, srcH - 1);
        double fy = sy - (double)y0;

        for (int dx = 0; dx < dstW; ++dx) {
            double sx = (double)dx * (double)srcW / (double)dstW;
            int x0 = (int)sx;
            int x1 = (std::min)(x0 + 1, srcW - 1);
            double fx = sx - (double)x0;

            uint8_t* dstPixel = dst.data() + (dy * dstW + dx) * 4;

            for (int c = 0; c < 4; ++c) {
                double v00 = src[y0 * srcStride + x0 * 4 + c];
                double v01 = src[y0 * srcStride + x1 * 4 + c];
                double v10 = src[y1 * srcStride + x0 * 4 + c];
                double v11 = src[y1 * srcStride + x1 * 4 + c];

                double top = v00 * (1.0 - fx) + v01 * fx;
                double bot = v10 * (1.0 - fx) + v11 * fx;
                double val = top * (1.0 - fy) + bot * fy;
                dstPixel[c] = (uint8_t)(std::clamp)(val, 0.0, 255.0);
            }
        }
    }
    return dst;
}

// 无 OpenCV 版本：在屏幕截图中搜索最佳模板匹配
// 使用滑动窗口 + 均方差，返回最佳匹配（confidence = 1 / (1 + MSE)）
inline MatchResult MatchTemplateByPixelMSE(const ScreenFrame& frame,
                                           const std::vector<TabIconTemplate>& templates,
                                           int searchX, int searchY, int searchW, int searchH,
                                           double threshold = 0.5) {
    MatchResult result;
    if (templates.empty() || searchW <= 0 || searchH <= 0 || frame.bgra.empty()) {
        return result;
    }

    // 在搜索区域内对每个模板做滑动窗口匹配
    // 步长 = 模板宽度/4，减少计算量
    MatchResult best;
    double bestScore = threshold;

    for (const auto& tpl : templates) {
        if (tpl.width <= 0 || tpl.height <= 0) continue;
        if (tpl.width > searchW || tpl.height > searchH) continue;

        int step = (std::max)(1, tpl.width / 4);
        int maxY = searchH - tpl.height;
        int maxX = searchW - tpl.width;

        for (int oy = 0; oy <= maxY; oy += step) {
            for (int ox = 0; ox <= maxX; ox += step) {
                // 从 frame 中提取 tpl.width × tpl.height 的子区域
                double mse = ComputeImageMSE(
                    frame.bgra.data() + (searchY + oy) * frame.stride + (searchX + ox) * 4,
                    tpl.bgra.data(),
                    tpl.width, tpl.height,
                    frame.stride,  // 步长来自 frame
                    tpl.width * 4  // 模板紧凑排列
                );

                // 置信度 = 1/(1+MSE)，MSE 越小置信度越高
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

// 多尺度模板匹配 - 对每个模板尝试多种缩放比例
// 使用 NCC (归一化相关系数) + MSE 混合评分
// 返回最佳匹配结果（置信度 0..1）
inline MatchResult MatchTemplateMultiScale(const ScreenFrame& frame,
                                           const std::vector<TabIconTemplate>& templates,
                                           int searchX, int searchY, int searchW, int searchH,
                                           double threshold = 0.5) {
    MatchResult result;
    if (templates.empty() || searchW <= 0 || searchH <= 0 || frame.bgra.empty()) {
        return result;
    }

    MatchResult best;
    double bestScore = threshold;

    // 候选缩放比例 (相对于模板原始尺寸)
    static const float scales[] = { 0.75f, 0.85f, 1.0f, 1.15f, 1.3f, 1.5f };
    static const int numScales = sizeof(scales) / sizeof(scales[0]);

    for (const auto& tpl : templates) {
        if (tpl.width <= 0 || tpl.height <= 0) continue;

        for (int si = 0; si < numScales; ++si) {
            int tw = (int)(tpl.width * scales[si]);
            int th = (int)(tpl.height * scales[si]);
            if (tw < 4 || th < 4) continue;
            if (tw > searchW || th > searchH) continue;

            // 如果缩放比例接近1.0，直接使用原模板；否则缩放
            const uint8_t* tplData = tpl.bgra.data();
            int tplStride = tpl.width * 4;
            std::vector<uint8_t> scaledTpl;
            if (std::fabs(scales[si] - 1.0f) > 0.05f) {
                scaledTpl = ResizeBilinearBGRA(tpl.bgra.data(), tpl.width, tpl.height, tw, th);
                if (scaledTpl.empty()) continue;
                tplData = scaledTpl.data();
                tplStride = tw * 4;
            }

            int step = (std::max)(1, tw / 4);
            int maxY = searchH - th;
            int maxX = searchW - tw;

            for (int oy = 0; oy <= maxY; oy += step) {
                for (int ox = 0; ox <= maxX; ox += step) {
                    const uint8_t* screenPtr = frame.bgra.data() + (searchY + oy) * frame.stride + (searchX + ox) * 4;

                    // 双评分: NCC + MSE
                    double ncc = ComputeImageNCC(screenPtr, tplData, tw, th, frame.stride, tplStride);
                    double mse = ComputeImageMSE(screenPtr, tplData, tw, th, frame.stride, tplStride);

                    // 置信度融合: NCC直接使用(已归一化), MSE转为置信度
                    double mseConf = 1.0 / (1.0 + mse / (255.0 * 255.0));
                    double score = ncc * 0.7 + mseConf * 0.3;

                    if (score > bestScore) {
                        bestScore = score;
                        best.matched = true;
                        best.stashId = tpl.stashId;
                        best.stashName = tpl.stashName;
                        best.screenX = searchX + ox + tw / 2;
                        best.screenY = searchY + oy + th / 2;
                        best.confidence = score;
                    }
                }
            }
        }
    }

    return best;
}

// 便捷版：Load 模板 + 截屏 + 像素匹配（无 OpenCV 依赖）
// 结果存入缓存，避免每帧重复 Load BMP
struct IconMatchCache {
    std::vector<TabIconTemplate> templates;
    std::filesystem::path lastPluginDir;
    bool loaded = false;
};

// 详细识别过程的耗时统计结构
struct VisionTimingReport {
    double loadTemplatesMs = 0;   // 模板加载耗时
    double computeRectMs = 0;     // Tab栏坐标计算耗时
    double captureMs = 0;         // 屏幕截获耗时
    double matchMs = 0;           // 模板匹配耗时
    double totalMs = 0;           // 总耗时
    int templateCount = 0;        // 使用的模板数量
    int searchAreaW = 0;          // 搜索区域宽度
    int searchAreaH = 0;          // 搜索区域高度
    double confidence = 0;        // 匹配置信度
    bool matched = false;         // 是否匹配成功
    int matchedStashId = -1;      // 匹配到的仓库类型ID
    std::string matchedStashName;  // 匹配到的仓库类型名称
    int matchScreenX = 0;         // 匹配位置屏幕X
    int matchScreenY = 0;         // 匹配位置屏幕Y
};

// 记录耗时报告到 DebugView
inline void LogVisionTimingReport(const VisionTimingReport& r) {
    char buf[1024];
    sprintf_s(buf,
        "[VisionRecog] ===== 视觉识别耗时报告 =====\n"
        "[VisionRecog]   模板加载: %.2f ms (n=%d)\n"
        "[VisionRecog]   坐标计算: %.2f ms\n"
        "[VisionRecog]   屏幕截获: %.2f ms (%dx%d)\n"
        "[VisionRecog]   模板匹配: %.2f ms\n"
        "[VisionRecog]   总耗时:   %.2f ms\n"
        "[VisionRecog]   匹配结果: %s (id=%d, name=%s, conf=%.4f, pos=(%d,%d))\n"
        "[VisionRecog] ================================\n",
        r.loadTemplatesMs, r.templateCount,
        r.computeRectMs,
        r.captureMs, r.searchAreaW, r.searchAreaH,
        r.matchMs,
        r.totalMs,
        r.matched ? "FOUND" : "NOT FOUND",
        r.matchedStashId,
        r.matchedStashName.c_str(),
        r.confidence,
        r.matchScreenX, r.matchScreenY);
    OutputDebugStringA(buf);
}

// 带详细日志埋点的仓库Tab图标识别
inline MatchResult MatchStashTabByIconNoCV(const PluginSDK::Context* ctx,
                                           IconMatchCache& cache,
                                           const std::filesystem::path& pluginDir,
                                           double threshold = 0.5,
                                           VisionTimingReport* timingOut = nullptr) {
    MatchResult empty;
    if (!ctx) {
        if (timingOut) {
            timingOut->matched = false;
            timingOut->totalMs = 0;
        }
        return empty;
    }

    auto tTotalStart = std::chrono::high_resolution_clock::now();
    VisionTimingReport report;

    // --- Step 1: 懒加载模板 ---
    auto tLoadStart = std::chrono::high_resolution_clock::now();
    if (!cache.loaded || cache.lastPluginDir != pluginDir) {
        cache.lastPluginDir = pluginDir;
        cache.loaded = false;
        cache.templates.clear();
        int n = LoadTabIconTemplates(pluginDir, cache.templates);
        cache.loaded = (n > 0);

        char logMsg[256];
        sprintf_s(logMsg, "[VisionRecog] LoadTabIconTemplates: %d 个模板\n", n);
        OutputDebugStringA(logMsg);

        if (n == 0) {
            auto tLoadEnd = std::chrono::high_resolution_clock::now();
            report.loadTemplatesMs = std::chrono::duration<double, std::milli>(tLoadEnd - tLoadStart).count();
            report.templateCount = 0;
            if (timingOut) *timingOut = report;
            return empty;
        }
    }
    auto tLoadEnd = std::chrono::high_resolution_clock::now();
    report.loadTemplatesMs = std::chrono::duration<double, std::milli>(tLoadEnd - tLoadStart).count();
    report.templateCount = (int)cache.templates.size();

    // --- Step 2: 计算 Tab 栏坐标 ---
    auto tRectStart = std::chrono::high_resolution_clock::now();
    int tx, ty, tw, th;
    if (!ComputeTabBarRectFromGrid(ctx, tx, ty, tw, th)) {
        auto tRectEnd = std::chrono::high_resolution_clock::now();
        report.computeRectMs = std::chrono::duration<double, std::milli>(tRectEnd - tRectStart).count();
        auto tTotalEnd = std::chrono::high_resolution_clock::now();
        report.totalMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (timingOut) *timingOut = report;
        return empty;
    }
    auto tRectEnd = std::chrono::high_resolution_clock::now();
    report.computeRectMs = std::chrono::duration<double, std::milli>(tRectEnd - tRectStart).count();

    if (tw <= 0 || th <= 0) {
        auto tTotalEnd = std::chrono::high_resolution_clock::now();
        report.totalMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (timingOut) *timingOut = report;
        return empty;
    }

    report.searchAreaW = tw;
    report.searchAreaH = th;

    // --- Step 3: 屏幕截获 ---
    auto tCaptureStart = std::chrono::high_resolution_clock::now();
    ScreenFrame frame;
    if (!CaptureScreenRegion(tx, ty, tw, th, frame)) {
        auto tCaptureEnd = std::chrono::high_resolution_clock::now();
        report.captureMs = std::chrono::duration<double, std::milli>(tCaptureEnd - tCaptureStart).count();
        auto tTotalEnd = std::chrono::high_resolution_clock::now();
        report.totalMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
        if (timingOut) *timingOut = report;
        return empty;
    }
    auto tCaptureEnd = std::chrono::high_resolution_clock::now();
    report.captureMs = std::chrono::duration<double, std::milli>(tCaptureEnd - tCaptureStart).count();

    // --- Step 4: 模板匹配 ---
    auto tMatchStart = std::chrono::high_resolution_clock::now();
    MatchResult result = MatchTemplateByPixelMSE(frame, cache.templates, tx, ty, tw, th, threshold);
    auto tMatchEnd = std::chrono::high_resolution_clock::now();
    report.matchMs = std::chrono::duration<double, std::milli>(tMatchEnd - tMatchStart).count();

    // --- 汇总 ---
    auto tTotalEnd = std::chrono::high_resolution_clock::now();
    report.totalMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();
    report.matched = result.matched;
    report.confidence = result.confidence;
    report.matchedStashId = result.stashId;
    report.matchedStashName = result.stashName;
    report.matchScreenX = result.screenX;
    report.matchScreenY = result.screenY;

    // 输出详细日志
    LogVisionTimingReport(report);

    if (timingOut) *timingOut = report;
    return result;
}

} // namespace VisionRecogNS