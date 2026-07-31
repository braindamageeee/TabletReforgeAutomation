// test_mouse_timing.cpp - 独立测试 WindMouse v4 的时序性能
// 编译: cl /EHsc /std:c++17 /utf-8 /I.. /I../third_party test_mouse_timing.cpp user32.lib gdi32.lib
#include <unordered_map>
#include <string>
#include <vector>
// 注意：这里不直接 #include TabletBonusCatalog.h（依赖 Settings.h 太重），
// 而是仅测试 WindMouse 时序性能；POE2 mod id 映射测试通过 DLL 内 Mock 触发
#include "../input/Win32Input.h"
#include <chrono>
#include <cstdio>
#include <vector>

using HighResClock = std::chrono::high_resolution_clock;

struct TestCase {
    const char* name;
    int distPx;         // 模拟距离
    int gravity, wind, maxStep, stepWait;
    int clicks;         // 模拟连续点击次数
    int targetPerClick; // 目标单步间隔 ms
};

int main() {
    printf("========== WindMouse v4 时序性能测试 ==========\n\n");

    TabletReforgeInput::SleepMs(50); // 预热

    std::vector<TestCase> tests = {
        // 仓库取出场景：相邻格 ~55px
        {"相邻格(55px) 快速档", 55, 13, 2, 28, 1, 10, 60},
        {"相邻格(55px) 极速档", 55, 14, 1, 32, 0, 10, 30},
        {"相邻格(55px) 平衡档", 55,  9, 3, 15, 2, 10, 100},
        // 仓库取出场景：隔一格 ~110px
        {"隔1格(110px) 快速档", 110, 13, 2, 28, 1, 8, 80},
        {"隔1格(110px) 极速档", 110, 14, 1, 32, 0, 8, 40},
        // 远距离：仓库到屏幕中央 ~400px
        {"远距离(400px) 快速档", 400, 13, 2, 28, 1, 4, 200},
        {"远距离(400px) 平衡档", 400,  9, 3, 15, 2, 4, 300},
    };

    int totalPass = 0, totalFail = 0;

    for (const auto& t : tests) {
        // 起点(100,100)，终点沿直线移动 t.distPx
        int x0 = 100, y0 = 100;
        int x1 = 100 + t.distPx, y1 = 100;
        auto seqStart = HighResClock::now();
        std::vector<int> stepTimes;

        int curX = x0, curY = y0;
        for (int i = 0; i < t.clicks; ++i) {
            auto s1 = HighResClock::now();
            // 直接调用 WindMouse，跳过 HumanLikeMoveTo 的 GetCursorScreen()（测试环境光标位置不受控）
            TabletReforgeInput::WindMouse(curX, curY, x1, y1, t.gravity, t.wind, t.maxStep, t.stepWait);
            TabletReforgeInput::SleepMs(3);   // cursorSettleMs
            TabletReforgeInput::SleepMs(15);  // postClickDelayMs
            auto s2 = HighResClock::now();

            stepTimes.push_back((int)std::chrono::duration_cast<std::chrono::milliseconds>(s2 - s1).count());
            // 反向移动模拟下一次点击
            curX = x1; curY = y1;
            std::swap(x0, x1);  // 起点终点对调
            x0 = curX; y0 = curY;
            x1 = 100 + ((i % 2 == 0) ? 0 : t.distPx);
            y1 = 100;
        }

        // 统计
        int sum = 0, mn = 9999999, mx = 0;
        for (int ms : stepTimes) {
            sum += ms;
            if (ms < mn) mn = ms;
            if (ms > mx) mx = ms;
        }
        int avg = (stepTimes.empty()) ? 0 : sum / (int)stepTimes.size();
        int totalSeq = (int)std::chrono::duration_cast<std::chrono::milliseconds>(HighResClock::now() - seqStart).count();
        int perClickEst = t.clicks > 1 ? totalSeq / t.clicks : avg;
        // ★ 容差：Windows Sleep(1) 实际调度精度 1.5~2ms，算法理论值 + 2ms 容差是合理的
        // 极速档目标严格（≤30/40ms）→ 容差2ms；宽松档（≥60ms）→ 容差3ms
        int tolerance = (t.targetPerClick <= 40) ? 2 : 3;
        bool pass = perClickEst <= t.targetPerClick + tolerance;
        if (pass) ++totalPass; else ++totalFail;

        printf("[%s] %s\n", pass ? "PASS" : "FAIL", t.name);
        printf("  dist=%dpx params=(G%d W%d S%d w%dms) clicks=%d\n",
            t.distPx, t.gravity, t.wind, t.maxStep, t.stepWait, t.clicks);
        printf("  单步: 平均=%dms 最小=%dms 最大=%dms | 总时长=%dms | 目标单步<=%dms (+%dms容差)\n\n",
            avg, mn, mx, totalSeq, t.targetPerClick, tolerance);
    }

    printf("========== 结果: PASS=%d FAIL=%d 总=%d ==========\n", totalPass, totalFail, totalPass + totalFail);
    return totalFail > 0 ? 1 : 0;
}
