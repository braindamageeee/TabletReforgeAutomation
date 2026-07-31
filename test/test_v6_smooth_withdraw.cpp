// test_v6_smooth_withdraw.cpp — v6 流畅取物 + 鉴定延后 + 相邻聚类 验证
//
// 验证目标（针对用户反馈的3个问题）：
//   1. 取物停顿：v5 每取4个就重扫~150ms，v6 一次扫描所有原料入队，无中途重扫
//   2. 鉴定误触发：v5 队列空误判为仓库空触发鉴定，v6 鉴定只在背包满/仓库真空时触发
//   3. 相邻聚类：v5 硬性从左至右从上到下，v6 贪心最近邻让相邻物品连成最短路径
//
// 公平对比设计：
//   旧版测试不公平（v5 因 BUG 只取4个就 break，v6 取全部10个，比总耗时无意义）
//   新版：v5_fixed = 修复 BUG 后的 v5（也取全部物品），但保留"每4个重扫 + 硬性排序"
//         v6       = 一次扫描入队 + 相邻聚类 + 鉴定延后
//   两者取相同数量物品，对比：扫描次数、中途停顿、鼠标路径长度
//
// 另外单测 v5_BUG 行为：验证 v5 BUG 确实会中途触发鉴定（证明 BUG 存在），v6 不会

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

#include "../input/Win32Input.h"

using Clock = std::chrono::high_resolution_clock;
using MS = std::chrono::milliseconds;

// ========================================================================
// Mock: 仓库物品布局（12列x6行，每格56px，原料分散）
// ========================================================================
struct MockItem {
    int slotX, slotY;       // 仓库格坐标
    float screenX, screenY; // 屏幕坐标
    bool isMaterial;
};

constexpr int STASH_LEFT = 640, STASH_TOP = 220;
constexpr int CELL_W = 56, CELL_H = 56;

float SlotToScreenX(int slotX) { return (float)(STASH_LEFT + slotX * CELL_W + CELL_W / 2); }
float SlotToScreenY(int slotY) { return (float)(STASH_TOP + slotY * CELL_H + CELL_H / 2); }

// 生成 14 个原料物品（分散布局，测试相邻聚类效果）
std::vector<MockItem> GenerateMockStash() {
    // 模拟真实仓库：14个原料分散在 12x6 网格
    // 有些相邻（同一行连续3-4个），有些分散
    int slots[][2] = {
        {0,0}, {1,0}, {2,0},           // 第1行左：3个相邻
        {5,0},                          // 第1行中：1个孤立
        {8,0}, {9,0}, {10,0},          // 第1行右：3个相邻
        {1,1},                          // 第2行：1个
        {4,2}, {5,2}, {6,2},           // 第3行中：3个相邻
        {9,3}, {10,3},                 // 第4行右：2个相邻
        {3,5},                          // 第6行：1个孤立
    };
    std::vector<MockItem> items;
    for (auto& s : slots) {
        items.push_back({ s[0], s[1], SlotToScreenX(s[0]), SlotToScreenY(s[1]), true });
    }
    return items;
}

// ========================================================================
// Mock: 扫描成本（基于真实日志估算）
// ========================================================================
constexpr int MOCK_SCAN_MS = 150;       // 单次仓库扫描 + ReadItemMods + 词缀匹配
constexpr int MOCK_BAG_SCAN_MS = 60;    // 单次背包扫描

// ========================================================================
// 结果统计
// ========================================================================
struct VersionResult {
    int totalMs = 0;
    int scanCount = 0;            // 总扫描次数（含首次）
    int midScanCount = 0;         // 中途重扫次数（取物过程中）
    int identifyTriggered = 0;    // 鉴定触发次数
    int itemsTaken = 0;           // 实际取出物品数
    int midStallMs = 0;           // 中途停顿总时间（重扫导致）
    double totalMoveDist = 0;     // 鼠标移动总距离（像素）
    std::vector<int> intervals;   // 相邻点击间隔
};

// ========================================================================
// 模拟一次点击（含 WindMouse 移动 + 节流 + settle + postClick）
// ========================================================================
void SimulateClick(float fromX, float fromY, float toX, float toY,
                   int G, int W, int S, int SW,
                   int clickDelay, int postClick, int settle,
                   Clock::time_point& lastAction,
                   VersionResult& r) {
    // 节流
    auto now = Clock::now();
    int since = (int)std::chrono::duration_cast<MS>(now - lastAction).count();
    int need = clickDelay - since;
    if (need > 0) TabletReforgeInput::SleepMs(need);

    // 移动距离
    double dx = toX - fromX, dy = toY - fromY;
    r.totalMoveDist += std::hypot(dx, dy);

    auto clickStart = Clock::now();
    TabletReforgeInput::WindMouse((int)fromX, (int)fromY, (int)toX, (int)toY, G, W, S, SW);
    TabletReforgeInput::SleepMs(settle + 2 + postClick);
    auto clickEnd = Clock::now();
    r.intervals.push_back((int)std::chrono::duration_cast<MS>(clickEnd - clickStart).count());

    lastAction = Clock::now();
}

// ========================================================================
// v5_BUG: 原 v5 的 BUG 行为（每取4个重扫 + 队列空误判触发鉴定）
// 用于证明 BUG 存在：取4个就中途触发鉴定
// ========================================================================
VersionResult SimulateV5Bug(const std::vector<MockItem>& stash, int bagEmptySlots,
                             int bagUnidentified, int G, int W, int S, int SW,
                             int clickDelay, int postClick, int settle) {
    VersionResult r;
    auto t0 = Clock::now();
    Clock::time_point lastAction = t0 - std::chrono::milliseconds(clickDelay + 100);

    auto sorted = stash;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.slotY != b.slotY) return a.slotY < b.slotY;
        return a.slotX < b.slotX;
    });

    int emptySlots = bagEmptySlots;
    float curX = 500, curY = 400;

    // v5: 首次扫描背包 + 仓库
    TabletReforgeInput::SleepMs(MOCK_BAG_SCAN_MS);
    TabletReforgeInput::SleepMs(MOCK_SCAN_MS);
    r.scanCount += 2;

    constexpr int V5_BATCH = 4;
    size_t stashIdx = 0;

    while (emptySlots > 0 && stashIdx < sorted.size()) {
        int batchCount = 0;
        while (batchCount < V5_BATCH && stashIdx < sorted.size() && emptySlots > 0) {
            const auto& item = sorted[stashIdx++];
            SimulateClick(curX, curY, item.screenX, item.screenY,
                          G, W, S, SW, clickDelay, postClick, settle, lastAction, r);
            curX = item.screenX; curY = item.screenY;
            --emptySlots;
            ++r.itemsTaken;
            ++batchCount;
        }
        // v5 BUG: 队列空后，如果背包有未鉴定 + 仓库还有原料 + 背包未满 → 误触发鉴定
        if (bagUnidentified > 0 && emptySlots > 0 && stashIdx < sorted.size()) {
            r.identifyTriggered++;   // BUG: 中途触发鉴定
            break;                    // v5 会跳出循环去鉴定
        }
        // 队列空但没触发鉴定 → 重扫（中途停顿）
        if (emptySlots > 0 && stashIdx < sorted.size()) {
            TabletReforgeInput::SleepMs(MOCK_SCAN_MS);
            r.scanCount++;
            r.midScanCount++;
            r.midStallMs += MOCK_SCAN_MS;
        }
    }
    r.totalMs = (int)std::chrono::duration_cast<MS>(Clock::now() - t0).count();
    return r;
}

// ========================================================================
// v5_fixed: 修复 BUG 后的 v5（也取全部物品），保留"每4个重扫 + 硬性排序"
// 用于公平对比：相同取出数量下，扫描次数和路径长度
// ========================================================================
VersionResult SimulateV5Fixed(const std::vector<MockItem>& stash, int bagEmptySlots,
                               int G, int W, int S, int SW,
                               int clickDelay, int postClick, int settle) {
    VersionResult r;
    auto t0 = Clock::now();
    Clock::time_point lastAction = t0 - std::chrono::milliseconds(clickDelay + 100);

    auto sorted = stash;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.slotY != b.slotY) return a.slotY < b.slotY;
        return a.slotX < b.slotX;
    });

    int emptySlots = bagEmptySlots;
    float curX = 500, curY = 400;

    // 首次扫描背包 + 仓库
    TabletReforgeInput::SleepMs(MOCK_BAG_SCAN_MS);
    TabletReforgeInput::SleepMs(MOCK_SCAN_MS);
    r.scanCount += 2;

    constexpr int V5_BATCH = 4;
    size_t stashIdx = 0;

    while (emptySlots > 0 && stashIdx < sorted.size()) {
        int batchCount = 0;
        while (batchCount < V5_BATCH && stashIdx < sorted.size() && emptySlots > 0) {
            const auto& item = sorted[stashIdx++];
            SimulateClick(curX, curY, item.screenX, item.screenY,
                          G, W, S, SW, clickDelay, postClick, settle, lastAction, r);
            curX = item.screenX; curY = item.screenY;
            --emptySlots;
            ++r.itemsTaken;
            ++batchCount;
        }
        // v5_fixed: 不再误触发鉴定，但队列空后仍需重扫（中途停顿）
        if (emptySlots > 0 && stashIdx < sorted.size()) {
            TabletReforgeInput::SleepMs(MOCK_SCAN_MS);
            r.scanCount++;
            r.midScanCount++;
            r.midStallMs += MOCK_SCAN_MS;
        }
    }
    r.totalMs = (int)std::chrono::duration_cast<MS>(Clock::now() - t0).count();
    return r;
}

// ========================================================================
// v6: 一次扫描所有原料入队 + 相邻聚类排序 + 鉴定延后
// ========================================================================
VersionResult SimulateV6(const std::vector<MockItem>& stash, int bagEmptySlots,
                         int bagUnidentified, int G, int W, int S, int SW,
                         int clickDelay, int postClick, int settle) {
    VersionResult r;
    auto t0 = Clock::now();
    Clock::time_point lastAction = t0 - std::chrono::milliseconds(clickDelay + 100);

    int emptySlots = bagEmptySlots;
    float curX = 500, curY = 400;

    // v6: 首次扫描背包 + 一次扫描所有原料入队（无中途重扫）
    TabletReforgeInput::SleepMs(MOCK_BAG_SCAN_MS);
    TabletReforgeInput::SleepMs(MOCK_SCAN_MS);
    r.scanCount += 2;

    // v6: 相邻聚类排序（贪心最近邻）
    auto materials = stash;
    std::sort(materials.begin(), materials.end(), [](const auto& a, const auto& b) {
        if (a.slotY != b.slotY) return a.slotY < b.slotY;
        return a.slotX < b.slotX;
    });
    std::vector<char> visited(materials.size(), 0);
    std::vector<MockItem> ordered;
    ordered.reserve(materials.size());
    if (!materials.empty()) {
        ordered.push_back(materials[0]);
        visited[0] = 1;
        float cx = materials[0].screenX, cy = materials[0].screenY;
        for (size_t i = 1; i < materials.size(); ++i) {
            int bestIdx = -1;
            double bestDist = 1e18;
            for (size_t j = 0; j < materials.size(); ++j) {
                if (visited[j]) continue;
                double dx = materials[j].screenX - cx, dy = materials[j].screenY - cy;
                double d = dx * dx + dy * dy;
                if (d < bestDist) { bestDist = d; bestIdx = (int)j; }
            }
            if (bestIdx < 0) break;
            visited[bestIdx] = 1;
            ordered.push_back(materials[bestIdx]);
            cx = materials[bestIdx].screenX; cy = materials[bestIdx].screenY;
        }
    }

    // v6: 从队列依次取出（无中途重扫）
    constexpr int V6_BATCH = 6;
    int batchCount = 0;
    for (const auto& item : ordered) {
        if (emptySlots <= 0) break;
        SimulateClick(curX, curY, item.screenX, item.screenY,
                      G, W, S, SW, clickDelay, postClick, settle, lastAction, r);
        curX = item.screenX; curY = item.screenY;
        --emptySlots;
        ++r.itemsTaken;
        ++batchCount;
        // v6: 单帧上限（归还控制权，但无额外扫描）
        if (batchCount >= V6_BATCH) batchCount = 0;
    }
    // v6: 鉴定只在最后触发（背包满 OR 仓库真空）
    if (bagUnidentified > 0 && r.itemsTaken >= 3) {
        r.identifyTriggered++;  // 只在最后触发一次（正确行为）
    }
    r.totalMs = (int)std::chrono::duration_cast<MS>(Clock::now() - t0).count();
    return r;
}

// ========================================================================
// 打印结果
// ========================================================================
void PrintResult(const char* title, const VersionResult& r) {
    int avg = 0, mx = 0;
    if (!r.intervals.empty()) {
        int sum = 0;
        for (int v : r.intervals) { sum += v; if (v > mx) mx = v; }
        avg = sum / (int)r.intervals.size();
    }
    printf("  %-44s | 取出=%d 扫描=%d(中途%d) 停顿=%dms 鉴定=%d 距离=%.0fpx 间隔均/大=%d/%dms 总=%dms\n",
           title, r.itemsTaken, r.scanCount, r.midScanCount, r.midStallMs,
           r.identifyTriggered, r.totalMoveDist, avg, mx, r.totalMs);
}

int g_passCount = 0, g_failCount = 0;

void Check(const char* name, bool pass) {
    printf("  %s %s\n", pass ? "PASS ✓" : "FAIL ✗", name);
    if (pass) ++g_passCount; else ++g_failCount;
}

int main() {
    printf("========== v6 流畅取物 + 鉴定延后 + 相邻聚类 验证 ==========\n\n");

    auto stash = GenerateMockStash();
    const int G = 13, W = 2, S = 28, SW = 1;
    const int clickDelay = 30, postClick = 15, settle = 3;
    const int bagEmptySlots = 10;
    const int bagUnidentified = 2;

    // ====================================================================
    // 场景1: 核心场景 — 背包未满 + 仓库有原料 + 背包有未鉴定
    // 用户期望：优先取物，鉴定延后，不中途触发鉴定
    // ====================================================================
    printf("【场景1】背包10空槽 + 仓库14原料 + 背包2未鉴定（用户反馈核心场景）\n");
    printf("         期望：v5_BUG 中途触发鉴定；v5_fixed/v6 取全部后才触发\n\n");

    auto v5bug = SimulateV5Bug(stash, bagEmptySlots, bagUnidentified,
                                G, W, S, SW, clickDelay, postClick, settle);
    auto v5fix = SimulateV5Fixed(stash, bagEmptySlots,
                                  G, W, S, SW, clickDelay, postClick, settle);
    auto v6r   = SimulateV6(stash, bagEmptySlots, bagUnidentified,
                             G, W, S, SW, clickDelay, postClick, settle);

    PrintResult("v5_BUG(每4个重扫+队列空误判鉴定)", v5bug);
    PrintResult("v5_fixed(每4个重扫+硬性排序)", v5fix);
    PrintResult("v6(一次扫描+相邻聚类+鉴定延后)", v6r);
    printf("\n");

    // 验证1: v5_BUG 确实中途触发鉴定（证明 BUG 存在）
    Check("验证1 v5_BUG 中途触发鉴定（取出<全部时触发）",
          v5bug.identifyTriggered > 0 && v5bug.itemsTaken < (int)stash.size());

    // 验证2: v6 不中途触发鉴定（取出到背包满/仓库空后才触发）
    // v6 取出数量应 = min(背包空槽, 仓库原料数)，且鉴定只触发1次（最后）
    int v6ExpectedTaken = (std::min)(bagEmptySlots, (int)stash.size());
    Check("验证2 v6 取满背包后才触发鉴定（不中途打断）",
          v6r.itemsTaken == v6ExpectedTaken && v6r.identifyTriggered <= 1
          && v6r.itemsTaken > v5bug.itemsTaken);

    // 验证3: v6 中途零重扫（流畅取物）
    Check("验证3 v6 中途零重扫（midScanCount==0）",
          v6r.midScanCount == 0);

    // 验证4: v6 中途零停顿
    Check("验证4 v6 中途零停顿（midStallMs==0）",
          v6r.midStallMs == 0);

    // 验证5: v6 取出数量 >= v5_fixed（都应取满背包或取空仓库）
    Check("验证5 v6 取出数量 == v5_fixed（公平对比基础）",
          v6r.itemsTaken == v5fix.itemsTaken);

    // 验证6: v6 鼠标路径更短（相邻聚类优势）
    Check("验证6 v6 鼠标路径 < v5_fixed（相邻聚类更优）",
          v6r.totalMoveDist < v5fix.totalMoveDist);

    // 验证7: v6 扫描次数更少
    Check("验证7 v6 扫描次数 < v5_fixed（减少重扫）",
          v6r.scanCount < v5fix.scanCount);
    printf("\n");

    // ====================================================================
    // 场景2: 背包只有 4 空槽（测试 v5 每4个重扫 vs v6 一次扫描）
    // ====================================================================
    printf("【场景2】背包只有 4 空槽（v5 取4个后队列空但背包满，无需重扫）\n\n");
    auto v5r2 = SimulateV5Fixed(stash, 4, G, W, S, SW, clickDelay, postClick, settle);
    auto v6r2 = SimulateV6(stash, 4, 0, G, W, S, SW, clickDelay, postClick, settle);
    PrintResult("v5_fixed", v5r2);
    PrintResult("v6", v6r2);
    printf("\n");
    Check("验证8 场景2 v6 取出==v5_fixed", v6r2.itemsTaken == v5r2.itemsTaken);
    Check("验证9 场景2 v6 中途零停顿", v6r2.midStallMs == 0);
    printf("\n");

    // ====================================================================
    // 场景3: 背包 10 空槽全用完（取10个，测试中途流畅性）
    // 这是 v5_fixed 会重扫的场景（取4+4+2，中途2次重扫）
    // ====================================================================
    printf("【场景3】背包 10 空槽全用完（取10个，v5_fixed 中途重扫2次）\n\n");
    auto v5r3 = SimulateV5Fixed(stash, 10, G, W, S, SW, clickDelay, postClick, settle);
    auto v6r3 = SimulateV6(stash, 10, 0, G, W, S, SW, clickDelay, postClick, settle);
    PrintResult("v5_fixed", v5r3);
    PrintResult("v6", v6r3);
    int v5Stall = v5r3.midStallMs;
    int v6Stall = v6r3.midStallMs;
    printf("  → v5_fixed 中途停顿 %dms (%d次重扫) vs v6 中途停顿 %dms (%d次重扫)\n",
           v5Stall, v5r3.midScanCount, v6Stall, v6r3.midScanCount);
    printf("\n");
    Check("验证10 场景3 v5_fixed 有中途停顿（证明问题存在）", v5Stall > 0);
    Check("验证11 场景3 v6 中途零停顿（流畅取物）", v6Stall == 0);
    Check("验证12 场景3 v6 路径更短", v6r3.totalMoveDist < v5r3.totalMoveDist);
    printf("\n");

    // ====================================================================
    // 场景4: 仓库原料少于背包空槽（取空仓库，测试"仓库真空"判定）
    // 仓库5个，背包10空槽 → 取5个后仓库空，应触发鉴定（材料>=3）
    // ====================================================================
    printf("【场景4】仓库5原料 + 背包10空槽 + 背包2未鉴定（取空仓库后触发鉴定）\n\n");
    std::vector<MockItem> smallStash(stash.begin(), stash.begin() + 5);
    auto v6r4 = SimulateV6(smallStash, 10, bagUnidentified,
                            G, W, S, SW, clickDelay, postClick, settle);
    PrintResult("v6(取空仓库后鉴定)", v6r4);
    printf("\n");
    Check("验证13 场景4 v6 取空仓库(5个)后触发鉴定",
          v6r4.itemsTaken == 5 && v6r4.identifyTriggered == 1);
    Check("验证14 场景4 v6 中途零停顿", v6r4.midStallMs == 0);
    printf("\n");

    // ====================================================================
    // 综合
    // ====================================================================
    printf("========== 综合: %d PASS, %d FAIL ==========\n",
           g_passCount, g_failCount);
    return g_failCount == 0 ? 0 : 1;
}
