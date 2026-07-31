// test_full_synth_mock.cpp — 完整重铸台合成流程 Mock 场景
// 目标：模拟真实使用场景的端到端时序，验证批处理 + 加速 WindMouse 能否达到：
//   仓库取物：单物品 <= 250ms（极速档）
//   合成1次（打开仓库→取4个→关仓库→去重铸台→合成→确认）：总耗时 < 3000ms
//   合成流水线：合成N次循环无异常/无Abort/状态机在 Abort 前走完
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>

#include "../input/Win32Input.h"
#include "../config/Settings.h"

using Clock = std::chrono::high_resolution_clock;
using MS = std::chrono::milliseconds;

// ========================================================================
// Mock: 模拟 PoeFixer SDK 的坐标系统（1920x1080 基准分辨率）
// ========================================================================
struct CalibData {
    // 仓库面板
    int stashLeftX = 640, stashTopY = 220;
    int stashCellW = 56, stashCellH = 56;
    int stashCols = 12, stashRows = 6;
    // 玩家背包
    int invLeftX = 1040, invTopY = 680;
    int invCellW = 56, invCellH = 56;
    int invCols = 12, invRows = 5;
    // 重铸台
    int reforgeNpcX = 960, reforgeNpcY = 540;  // 重铸台NPC点击点
    int reforgeSlotX[4] = { 720, 830, 940, 1050 };
    int reforgeSlotY = 500;
    int reforgeCraftBtnX = 960, reforgeCraftBtnY = 620;
    int reforgeOkBtnX = 960, reforgeOkBtnY = 680;
} calib;

inline void StashCellPos(int col, int row, int& x, int& y) {
    x = calib.stashLeftX + col * calib.stashCellW + calib.stashCellW / 2;
    y = calib.stashTopY + row * calib.stashCellH + calib.stashCellH / 2;
}
inline void InvCellPos(int col, int row, int& x, int& y) {
    x = calib.invLeftX + col * calib.invCellW + calib.invCellW / 2;
    y = calib.invTopY + row * calib.invCellH + calib.invCellH / 2;
}

// ========================================================================
// Mock: 模拟一次合成循环（端到端流程）
// 包含状态：OpenStash → BatchTake4 → CloseStash → WalkToReforge → Put4Slots
//          → ClickCraft → ConfirmOk → (可选鉴定)
// ========================================================================
struct SynthStepLog {
    std::string name;
    int ms;
};

struct SynthCycleResult {
    int totalMs = 0;
    int take4Ms = 0;          // 仓库取4个(含移动)
    int takePerItemMs = 0;    // 取出单物品平均
    int walkStashToInvMs = 0; // 仓库第4格 → 背包首格 (距离代表"关闭仓库→走过去")
    int put4Ms = 0;           // 放入重铸台4格
    int craftMs = 0;          // 点合成+确认
    std::vector<SynthStepLog> steps;
    bool passThrottle = true;  // 所有取物点击间隔均 <= 250ms
};

// 执行一次合成循环（不发真实 SendInput 到游戏，仅测量鼠标移动时序）
SynthCycleResult RunOneSynthCycle(int cycleIdx,
                                  int gravity, int wind, int maxStep, int stepWait,
                                  int clickDelayMs, int postClickMs, int cursorSettleMs)
{
    SynthCycleResult r;
    auto t0 = Clock::now();
    auto addStep = [&](const char* name, const Clock::time_point& s, const Clock::time_point& e) {
        int ms = (int)std::chrono::duration_cast<MS>(e - s).count();
        r.steps.push_back({ name, ms });
    };

    // 起点：先移动到仓库面板左上角附近
    int startX = calib.stashLeftX - 50, startY = calib.stashTopY - 20;
    TabletReforgeInput::WindMouse(0, 0, startX, startY, gravity, wind, maxStep, stepWait);

    // ===== Phase 1: 取出 4 个相邻物品（1行连续 4 列）=====
    int invCol0 = 0, invRow0 = 0;
    int lastClickX = startX, lastClickY = startY;
    auto p1s = Clock::now();

    std::vector<int> takeIntervals;  // 相邻两次点击之间的间隔
    Clock::time_point lastClickTime = p1s - std::chrono::milliseconds(clickDelayMs + 100);
    for (int i = 0; i < 4; ++i) {
        auto item_s = Clock::now();
        // 节流：模拟 StateMachine 中的点击节流
        int since = (int)std::chrono::duration_cast<MS>(item_s - lastClickTime).count();
        int need = clickDelayMs - since;
        if (need > 0) TabletReforgeInput::SleepMs(need);

        int sx, sy; StashCellPos(i, 0, sx, sy);
        TabletReforgeInput::WindMouse(lastClickX, lastClickY, sx, sy, gravity, wind, maxStep, stepWait);
        // cursorSettle + click (不发真点击，sleep代替)
        TabletReforgeInput::SleepMs(cursorSettleMs);
        // Ctrl+Click：移动物品到背包（假设 CtrlDown）
        TabletReforgeInput::SleepMs(2);  // 按下
        TabletReforgeInput::SleepMs(postClickMs);  // 等待UI转移

        auto now = Clock::now();
        int intv = (int)std::chrono::duration_cast<MS>(now - lastClickTime).count();
        if (i > 0) takeIntervals.push_back(intv);
        lastClickTime = now;
        lastClickX = sx; lastClickY = sy;
    }
    auto p1e = Clock::now();
    r.take4Ms = (int)std::chrono::duration_cast<MS>(p1e - p1s).count();
    r.takePerItemMs = r.take4Ms / 4;
    addStep("取出4个", p1s, p1e);

    // 节流检查：所有相邻点击均 <= 250ms (用户要求 300ms 以内，取安全余量)
    const int kTargetInterval = 250;  // 目标：单物品间隔<250ms (300ms内有安全余量)
    for (int v : takeIntervals) {
        if (v > kTargetInterval) r.passThrottle = false;
    }

    // ===== Phase 2: 模拟"仓库最后一格 → 重铸台NPC"距离（代表关面板+走过去）=====
    auto p2s = Clock::now();
    // 最后一格到重铸台 NPC（跨屏幕约 350-450px）
    TabletReforgeInput::WindMouse(lastClickX, lastClickY, calib.reforgeNpcX, calib.reforgeNpcY,
                                  gravity, wind, maxStep, stepWait);
    TabletReforgeInput::SleepMs(300);  // 面板切换固定等待 300ms（宪法规定）
    auto p2e = Clock::now();
    r.walkStashToInvMs = (int)std::chrono::duration_cast<MS>(p2e - p2s).count();
    addStep("移动到重铸台+等面板", p2s, p2e);

    // ===== Phase 3: 从背包相邻4格放入重铸台4槽 =====
    auto p3s = Clock::now();
    lastClickX = calib.reforgeNpcX; lastClickY = calib.reforgeNpcY;
    for (int i = 0; i < 4; ++i) {
        // 节流
        auto s1 = Clock::now();
        int since = (int)std::chrono::duration_cast<MS>(s1 - lastClickTime).count();
        int need = clickDelayMs - since;
        if (need > 0) TabletReforgeInput::SleepMs(need);

        int ix, iy; InvCellPos(i, 0, ix, iy);
        TabletReforgeInput::WindMouse(lastClickX, lastClickY, ix, iy, gravity, wind, maxStep, stepWait);
        TabletReforgeInput::SleepMs(cursorSettleMs + 2 + postClickMs);  // 拿起
        int slotX = calib.reforgeSlotX[i], slotY = calib.reforgeSlotY;
        TabletReforgeInput::WindMouse(ix, iy, slotX, slotY, gravity, wind, maxStep, stepWait);
        TabletReforgeInput::SleepMs(cursorSettleMs + 2 + postClickMs);  // 放入
        lastClickTime = Clock::now();
        lastClickX = slotX; lastClickY = slotY;
    }
    auto p3e = Clock::now();
    r.put4Ms = (int)std::chrono::duration_cast<MS>(p3e - p3s).count();
    addStep("放入4个到重铸台", p3s, p3e);

    // ===== Phase 4: 点合成 + 确认 OK =====
    auto p4s = Clock::now();
    TabletReforgeInput::WindMouse(lastClickX, lastClickY, calib.reforgeCraftBtnX, calib.reforgeCraftBtnY,
                                  gravity, wind, maxStep, stepWait);
    TabletReforgeInput::SleepMs(cursorSettleMs + postClickMs + 40);  // 合成按钮+弹窗加载
    TabletReforgeInput::WindMouse(calib.reforgeCraftBtnX, calib.reforgeCraftBtnY,
                                  calib.reforgeOkBtnX, calib.reforgeOkBtnY,
                                  gravity, wind, maxStep, stepWait);
    TabletReforgeInput::SleepMs(cursorSettleMs + postClickMs + 100);  // 确认+物品落地
    auto p4e = Clock::now();
    r.craftMs = (int)std::chrono::duration_cast<MS>(p4e - p4s).count();
    addStep("合成+确认", p4s, p4e);

    r.totalMs = (int)std::chrono::duration_cast<MS>(Clock::now() - t0).count();
    return r;
}

// ========================================================================
// 打印 & 统计
// ========================================================================
void PrintResult(const char* title, const std::vector<SynthCycleResult>& cycles,
                 int targetPerItem, int targetCycle)
{
    printf("====== %s (N=%zu cycles) ======\n", title, cycles.size());
    int totTot = 0, totTake4 = 0, totTakePer = 0, totWalk = 0, totPut4 = 0, totCraft = 0;
    int failThrottle = 0, failCycle = 0, failItem = 0;
    for (const auto& c : cycles) {
        totTot += c.totalMs;
        totTake4 += c.take4Ms;
        totTakePer += c.takePerItemMs;
        totWalk += c.walkStashToInvMs;
        totPut4 += c.put4Ms;
        totCraft += c.craftMs;
        if (!c.passThrottle) failThrottle++;
        if (c.takePerItemMs > targetPerItem) failItem++;
        if (c.totalMs > targetCycle) failCycle++;
    }
    int n = (int)cycles.size();
    printf("  Avg 总耗时:      %dms (目标 < %dms)  %s 超限: %d/%d\n",
           totTot / n, targetCycle, (failCycle == 0 ? "✓" : "✗"), failCycle, n);
    printf("  Avg 取出4个:     %dms → 单物品 %dms (目标 < %dms)  %s 超限物品: %d/%d\n",
           totTake4 / n, totTakePer / n, targetPerItem,
           (failItem == 0 && failThrottle == 0 ? "✓" : "✗"), failItem, n);
    printf("  Avg 移动到重铸台: %dms (含300ms面板切换等待)\n", totWalk / n);
    printf("  Avg 放入4个:     %dms\n", totPut4 / n);
    printf("  Avg 合成+确认:   %dms\n", totCraft / n);
    printf("  取物点击间隔合规: %s (throttle fail: %d/%d)\n\n",
           (failThrottle == 0 ? "PASS ✓" : "FAIL ✗"), failThrottle, n);
}

int main() {
    printf("========== 完整重铸台合成 Mock 场景测试 v4 ==========\n\n");

    const int N = 5;  // 每个预设跑 5 个循环 (每次合成1次)
    const int kTargetPerItem = 250;  // 用户要求 300ms 内 → 取安全 250ms
    const int kTargetCycle = 2800;   // 单次合成全流程目标：< 2800ms

    // 定义预设
    struct {
        const char* name;
        int g, w, s, wait;
        int click, post, settle;
    } presets[] = {
        {"极速档(吞吐量极限)", 14, 1, 32, 0,  10, 3,  1},
        {"快速档(流水线取物★)", 13, 2, 28, 1,  30, 15, 3},
        {"平衡档(自然感)",       9, 3, 15, 2,  50, 30, 5},
    };

    bool allPass = true;
    for (const auto& p : presets) {
        std::vector<SynthCycleResult> cycles;
        for (int i = 0; i < N; ++i) {
            cycles.push_back(RunOneSynthCycle(i, p.g, p.w, p.s, p.wait,
                                              p.click, p.post, p.settle));
        }
        PrintResult(p.name, cycles, kTargetPerItem, kTargetCycle);
        // 判定这个预设是否整体PASS
        for (const auto& c : cycles) {
            if (!c.passThrottle || c.takePerItemMs > kTargetPerItem || c.totalMs > kTargetCycle) {
                allPass = false;
            }
        }
    }

    // ===== 附加: 批处理取出 模拟 (StateMachine 中 BATCH_MAX=4) =====
    printf("====== 批处理取出 (单帧连续取4个) 基准 vs 旧版Throttled跨帧 ======\n");
    // 旧版：Throttled + 跨帧 → 理论上 4 个物品最少 4*16ms = 64ms 纯跨帧
    // 新版：单帧批处理 → 理论上 0ms 跨帧开销
    int sumNew = 0;
    for (int i = 0; i < N; ++i) {
        // 模拟：连续 4 个相邻 56px
        auto s = Clock::now();
        int cx = 500, cy = 400;
        Clock::time_point lastT = s - std::chrono::milliseconds(100);
        for (int j = 0; j < 4; ++j) {
            auto now = Clock::now();
            int diff = (int)std::chrono::duration_cast<MS>(now - lastT).count();
            int ns = 30 - diff;  // clickDelayMs=30 (快速档)
            if (ns > 0) TabletReforgeInput::SleepMs(ns);
            int tx = cx + 56, ty = cy;
            TabletReforgeInput::WindMouse(cx, cy, tx, ty, 13, 2, 28, 1);
            TabletReforgeInput::SleepMs(3 + 15);  // settle + post
            cx = tx; cy = ty;
            lastT = Clock::now();
        }
        sumNew += (int)std::chrono::duration_cast<MS>(Clock::now() - s).count();
    }
    int avgNew = sumNew / N;
    int oldEstimate = 64 + avgNew;  // 旧版跨帧开销约 4*16ms
    printf("  新版批处理(单帧) 4个平均: %dms\n", avgNew);
    printf("  旧版Throttled跨帧  4个≈: %dms (估算)\n", oldEstimate);
    printf("  加速比: %.1fx  (节省 %dms / 4个)\n\n",
           oldEstimate * 1.0 / avgNew, oldEstimate - avgNew);

    printf("========== 综合结果: %s ==========\n", allPass ? "ALL PASS ✓" : "HAS FAIL ✗");
    return allPass ? 0 : 1;
}
