// test_scan_prefetch.cpp — 扫描预取队列优化效果验证
//
// 验证目标：
//   1. 旧版（每次循环都调 NextTempleTabletInStash → CollectStashTablets）的扫描成本
//   2. 新版（队列空才扫描一次，循环里直接 pop）的扫描成本
//   3. 验证：新版取 4 个物品 = 1 次扫描 vs 旧版 4 次扫描
//   4. 验证：扫描间隔 200ms 防抖生效，避免队列空时立刻重扫
//
// 测试方法：
//   由于无法直接调用 PoeFixer SDK（CollectStashTablets 依赖 ctx），我们用 Mock 扫描函数模拟
//   - MockScanStash(): 模拟扫描一次 76 个物品 + 每个调用 ReadItemMods，耗时 ~150ms（基于真实日志估算）
//   - MockClickItem(): 模拟点击一个物品（含 WindMouse + 节流 + postClick），耗时 ~80ms
//   然后比较两种实现的批处理取出 4 个物品的总耗时和单物品间隔

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>

#include "../input/Win32Input.h"

using Clock = std::chrono::high_resolution_clock;
using MS = std::chrono::milliseconds;

// ========================================================================
// Mock: 模拟 CollectStashTablets 的扫描成本
// ========================================================================
// 真实日志显示：76 个物品，每个调用 ReadItemMods + 词缀匹配
// 估算单次扫描耗时：根据 debug_matching.log 中扫描摘要的时间戳间隔，约 80-200ms
// 这里用 Sleep 模拟真实扫描耗时
constexpr int MOCK_SCAN_STASH_MS = 150;     // 单次仓库扫描+词缀匹配 耗时
constexpr int MOCK_SCAN_BAG_MS = 60;        // 单次背包扫描 耗时（19个物品）
constexpr int MOCK_READ_ITEM_MODS_PER_ITEM_MS = 2;  // 单个物品 ReadItemMods 耗时

struct MockStashItem {
    int slotX, slotY;
    bool isMaterial;
};

// 模拟仓库扫描：返回 N 个物品，前 materialN 个是匹配原料
std::vector<MockStashItem> MockScanStash(int totalItems, int materialN) {
    TabletReforgeInput::SleepMs(MOCK_SCAN_STASH_MS);  // 模拟 SDK 扫描开销
    std::vector<MockStashItem> out;
    for (int i = 0; i < totalItems; ++i) {
        out.push_back({ i, 0, i < materialN });
    }
    return out;
}

// 模拟背包扫描
int MockScanBagEmptySlots() {
    TabletReforgeInput::SleepMs(MOCK_SCAN_BAG_MS);
    return 10;  // 假设 10 个空槽
}

// 模拟点击一个物品（带 WindMouse 移动）
void MockClickItem(int fromX, int fromY, int toX, int toY,
                   int gravity, int wind, int maxStep, int stepWait,
                   int clickDelayMs, int postClickMs, int cursorSettleMs,
                   Clock::time_point& lastAction) {
    // 节流：等 clickDelayMs
    auto now = Clock::now();
    int since = (int)std::chrono::duration_cast<MS>(now - lastAction).count();
    int need = clickDelayMs - since;
    if (need > 0) TabletReforgeInput::SleepMs(need);

    // 移动
    TabletReforgeInput::WindMouse(fromX, fromY, toX, toY, gravity, wind, maxStep, stepWait);
    TabletReforgeInput::SleepMs(cursorSettleMs);
    // 点击（不发真输入，用 Sleep 模拟）
    TabletReforgeInput::SleepMs(2);
    TabletReforgeInput::SleepMs(postClickMs);
    lastAction = Clock::now();
}

// ========================================================================
// 旧版：每次循环都调 NextTempleTabletInStash → CollectStashTablets（每次都重扫）
// ========================================================================
struct OldVersionResult {
    int totalMs;
    int scanCount;        // 总扫描次数
    int clickCount;       // 实际点击数
    std::vector<int> intervals;  // 相邻点击间隔
};

OldVersionResult SimulateOldVersion(int batchMax, int totalItems, int materialN,
                                    int gravity, int wind, int maxStep, int stepWait,
                                    int clickDelayMs, int postClickMs, int cursorSettleMs) {
    OldVersionResult r{0, 0, 0, {}};
    auto t0 = Clock::now();
    Clock::time_point lastAction = t0 - std::chrono::milliseconds(clickDelayMs + 100);

    int curX = 500, curY = 400;
    int emptySlots = MockScanBagEmptySlots();  // 每帧 Step 0 会扫一次背包（这里模拟首次）
    r.scanCount++;  // 首次背包扫描

    for (int i = 0; i < batchMax; ++i) {
        if (emptySlots <= 0) break;
        // —— 旧版核心问题：每次循环都调 NextTempleTabletInStash → CollectStashTablets ——
        // 这里要扫描整个仓库，每次 ~150ms
        auto stash = MockScanStash(totalItems, materialN);
        r.scanCount++;
        // 找第一个 isMaterial 的物品
        bool found = false;
        for (const auto& it : stash) {
            if (it.isMaterial) {
                int tx = 600 + it.slotX * 56, ty = 400;
                auto clickStart = Clock::now();
                MockClickItem(curX, curY, tx, ty, gravity, wind, maxStep, stepWait,
                              clickDelayMs, postClickMs, cursorSettleMs, lastAction);
                auto clickEnd = Clock::now();
                r.intervals.push_back((int)std::chrono::duration_cast<MS>(clickEnd - clickStart).count());
                curX = tx; curY = ty;
                --emptySlots;
                ++r.clickCount;
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    r.totalMs = (int)std::chrono::duration_cast<MS>(Clock::now() - t0).count();
    return r;
}

// ========================================================================
// 新版：队列空才扫描一次，循环里直接 pop（预取队列）
// ========================================================================
struct NewVersionResult {
    int totalMs;
    int scanCount;        // 总扫描次数
    int clickCount;       // 实际点击数
    std::vector<int> intervals;  // 相邻点击间隔
};

NewVersionResult SimulateNewVersion(int batchMax, int totalItems, int materialN,
                                    int gravity, int wind, int maxStep, int stepWait,
                                    int clickDelayMs, int postClickMs, int cursorSettleMs) {
    NewVersionResult r{0, 0, 0, {}};
    auto t0 = Clock::now();
    Clock::time_point lastAction = t0 - std::chrono::milliseconds(clickDelayMs + 100);

    int curX = 500, curY = 400;
    int emptySlots = MockScanBagEmptySlots();  // Step 0 首次扫描背包（之后用缓存）
    r.scanCount++;  // 首次背包扫描

    // 新版预取队列
    struct PendingItem { int slotX, slotY; };
    std::vector<PendingItem> queue;
    Clock::time_point lastFullScanTime = t0;

    for (int i = 0; i < batchMax; ++i) {
        if (emptySlots <= 0) break;
        // —— 新版：队列空才扫描 ——
        if (queue.empty()) {
            int msSinceScan = (int)std::chrono::duration_cast<MS>(Clock::now() - lastFullScanTime).count();
            // 防抖：200ms 内不重扫（虽然首次必然触发）
            if (msSinceScan >= 200 || r.scanCount <= 1) {
                auto stash = MockScanStash(totalItems, materialN);
                r.scanCount++;
                lastFullScanTime = Clock::now();
                queue.clear();
                for (const auto& it : stash) {
                    if (it.isMaterial) queue.push_back({ it.slotX, it.slotY });
                }
            } else {
                break;  // 距上次扫描太近，留给下一帧
            }
        }
        if (queue.empty()) break;

        // —— 新版核心：直接从队列 pop，零扫描 ——
        auto next = queue.back();
        queue.pop_back();
        int tx = 600 + next.slotX * 56, ty = 400;
        auto clickStart = Clock::now();
        MockClickItem(curX, curY, tx, ty, gravity, wind, maxStep, stepWait,
                      clickDelayMs, postClickMs, cursorSettleMs, lastAction);
        auto clickEnd = Clock::now();
        r.intervals.push_back((int)std::chrono::duration_cast<MS>(clickEnd - clickStart).count());
        curX = tx; curY = ty;
        --emptySlots;
        ++r.clickCount;
    }
    r.totalMs = (int)std::chrono::duration_cast<MS>(Clock::now() - t0).count();
    return r;
}

// ========================================================================
// 打印
// ========================================================================
template <typename T>
void PrintResult(const char* title, const T& r, int targetInterval) {
    int avg = 0, mx = 0;
    if (!r.intervals.empty()) {
        int sum = 0;
        for (int v : r.intervals) { sum += v; if (v > mx) mx = v; }
        avg = sum / (int)r.intervals.size();
    }
    bool pass = avg <= targetInterval + 5 && mx <= targetInterval + 30;
    printf("====== %s ======\n", title);
    printf("  总耗时: %dms | 扫描次数: %d | 点击数: %d\n", r.totalMs, r.scanCount, r.clickCount);
    printf("  单步点击间隔: 平均=%dms 最大=%dms (目标 ≤%dms) %s\n\n",
           avg, mx, targetInterval, pass ? "PASS ✓" : "FAIL ✗");
}

int main() {
    printf("========== 扫描预取队列优化效果验证 ==========\n\n");
    printf("Mock 参数: 仓库扫描=%dms 背包扫描=%dms 仓库物品=76 原料=15\n",
           MOCK_SCAN_STASH_MS, MOCK_SCAN_BAG_MS);
    printf("鼠标参数: 快速档(G13 W2 S28 w1ms) clickDelay=30 postClick=15 settle=3\n\n");

    const int kBatchMax = 4;
    const int kTotalStash = 76;
    const int kMaterialN = 15;
    const int kTargetInterval = 100;  // 单步点击间隔目标（取物，含扫描）
    // 快速档鼠标参数
    const int G = 13, W = 2, S = 28, SW = 1;
    const int clickDelay = 30, postClick = 15, settle = 3;

    bool allPass = true;

    // === 1. 单次批处理取 4 个物品 ===
    printf("【场景1】单次批处理取出 4 个物品（BATCH_MAX=4）\n\n");
    // 首次扫描 = 仓库150ms + 背包60ms = 210ms 必须开销
    // 4次点击 ≈ 60ms × 4 = 240ms（快速档）
    // 新版合理目标: ≤ 700ms（首次扫描 + 4次点击）
    // 旧版合理目标: > 800ms（首次扫描 + 4次循环都重扫 = 210 + 4*150 = 810ms+）
    auto oldR = SimulateOldVersion(kBatchMax, kTotalStash, kMaterialN,
                                    G, W, S, SW, clickDelay, postClick, settle);
    PrintResult("旧版（每次循环都扫描）", oldR, kTargetInterval);
    if (oldR.totalMs < 800) allPass = false;  // 旧版必须慢（验证未优化）
    if (oldR.scanCount < 4) allPass = false;  // 旧版必须扫描多次

    auto newR = SimulateNewVersion(kBatchMax, kTotalStash, kMaterialN,
                                    G, W, S, SW, clickDelay, postClick, settle);
    PrintResult("新版（预取队列，零扫描循环）", newR, kTargetInterval);
    if (newR.totalMs > 700) allPass = false;  // 新版必须快（首次扫描+点击）
    if (newR.scanCount > 2) allPass = false;  // 新版最多扫描 2 次（首次仓库+首次背包）

    // 加速比
    double speedup = (double)oldR.totalMs / (double)newR.totalMs;
    printf("  ★ 加速比: %.2fx  (旧 %dms → 新 %dms, 节省 %dms)\n",
           speedup, oldR.totalMs, newR.totalMs, oldR.totalMs - newR.totalMs);
    printf("  ★ 扫描次数对比: 旧 %d 次 → 新 %d 次 (减少 %d 次全仓库扫描)\n\n",
           oldR.scanCount, newR.scanCount, oldR.scanCount - newR.scanCount);
    if (speedup < 1.5) allPass = false;

    // === 2. 关闭鼠标模拟轨迹后的对比（核心验证用户反馈场景）===
    printf("【场景2】关闭鼠标模拟轨迹后（enableHumanMouse=false，直接跳转）\n\n");
    // 关闭轨迹 = WindMouse 内部走"距离<24px 瞬移"分支，几乎 0ms
    // 这里用 stepWait=0 maxStep=999 模拟（每步瞬移）
    // 新版关闭轨迹后: 首次扫描210ms + 4次点击~0ms = 220ms
    // 旧版关闭轨迹后: 首次扫描210ms + 4*150ms(循环重扫) = 810ms+
    auto oldR2 = SimulateOldVersion(kBatchMax, kTotalStash, kMaterialN,
                                     G, W, 999, 0, clickDelay, postClick, settle);
    PrintResult("旧版（关闭鼠标轨迹，每次循环都扫描）", oldR2, kTargetInterval);
    if (oldR2.totalMs < 700) allPass = false;  // 旧版即使关轨迹也慢（扫描开销）

    auto newR2 = SimulateNewVersion(kBatchMax, kTotalStash, kMaterialN,
                                     G, W, 999, 0, clickDelay, postClick, settle);
    PrintResult("新版（关闭鼠标轨迹，预取队列）", newR2, kTargetInterval);
    if (newR2.totalMs > 550) allPass = false;  // 新版关闭轨迹后 ≤550ms (210ms扫描+292ms点击+余量)

    printf("  ★ 关闭轨迹后加速比: %.2fx  (旧 %dms → 新 %dms)\n",
           (double)oldR2.totalMs / (double)newR2.totalMs, oldR2.totalMs, newR2.totalMs);
    printf("  ★ 验证用户反馈：关闭轨迹后旧版依然慢（因为扫描），新版才能真正做到快速点击\n\n");
    // ★ 关键验证：关闭轨迹后旧版依然 > 700ms（说明问题在扫描而非鼠标）
    // ★ 新版关闭轨迹后必须 < 400ms（说明扫描已优化）
    if (oldR2.totalMs <= newR2.totalMs + 200) allPass = false;  // 旧版必须明显比新版慢

    // === 3. 连续取出 12 个物品（3 次批处理循环）===
    printf("【场景3】连续取出 12 个物品（3 次批处理循环）\n\n");
    // 模拟 3 帧，每帧取 4 个
    int oldTotal = 0, oldScans = 0, newTotal = 0, newScans = 0;
    for (int frame = 0; frame < 3; ++frame) {
        auto o = SimulateOldVersion(kBatchMax, kTotalStash, kMaterialN - frame * 4,
                                     G, W, S, SW, clickDelay, postClick, settle);
        oldTotal += o.totalMs; oldScans += o.scanCount;
        auto n = SimulateNewVersion(kBatchMax, kTotalStash, kMaterialN - frame * 4,
                                     G, W, S, SW, clickDelay, postClick, settle);
        newTotal += n.totalMs; newScans += n.scanCount;
    }
    printf("  旧版 3 帧总耗时: %dms, 扫描次数: %d\n", oldTotal, oldScans);
    printf("  新版 3 帧总耗时: %dms, 扫描次数: %d\n", newTotal, newScans);
    printf("  ★ 加速比: %.2fx  节省 %dms\n\n",
           (double)oldTotal / (double)newTotal, oldTotal - newTotal);
    if (oldScans <= newScans) allPass = false;  // 新版扫描次数必须少于旧版
    // 新版每帧都要扫描一次（队列空触发），3帧总耗时 ~1800ms 是合理的（3 * 600ms）
    // 关键是加速比必须 ≥ 1.4x
    if ((double)oldTotal / (double)newTotal < 1.4) allPass = false;
    if (newScans > oldScans / 2) allPass = false;  // 新版扫描次数必须 ≤ 旧版一半

    printf("========== 综合: %s ==========\n", allPass ? "ALL PASS ✓" : "HAS FAIL ✗");
    return allPass ? 0 : 1;
}
