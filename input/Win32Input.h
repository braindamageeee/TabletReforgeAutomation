// Win32Input.h — Win32 输入模拟（鼠标 + 键盘）
//
// 合并 pickmyloot 的多显示器绝对坐标光标移动 + QuickStash 的 Ctrl 会话级保持，
// 并扩展右键点击和键盘按键（Esc 等）。
//
// 设计要点：
//   - 光标移动用 SendInput + MOUSEEVENTF_VIRTUALDESK，多显示器下也准确
//   - 左键/右键点击用单次 SendInput 同时发 DOWN+UP，原子操作避免被游戏吃掉半个事件
//   - Ctrl 采用会话级保持：状态机开始连续操作时 CtrlDown 一次，结束后 CtrlUp
//   - 绝不在这里管时序——时序由状态机用 Clock + SleepMs 控制
#pragma once

#include <Windows.h>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <random>
#include <chrono>

namespace TabletReforgeInput {

// 检查屏幕坐标是否在虚拟桌面范围内（安全边界）
inline bool IsScreenCoordValid(int x, int y) {
    const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsW <= 0 || vsH <= 0) return false;
    return (x >= vsX && x < vsX + vsW && y >= vsY && y < vsY + vsH);
}

// 把光标移到屏幕坐标 (x, y)。用绝对坐标 + 虚拟桌面标志，多显示器下也准确。
// 注意：坐标必须在屏幕范围内，否则不移动并返回 false。
inline bool MoveCursorScreen(int x, int y) {
    if (!IsScreenCoordValid(x, y)) return false;

    const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsW <= 0) vsW = 1;
    if (vsH <= 0) vsH = 1;

    const LONG nx = static_cast<LONG>(
        ((static_cast<long long>(x - vsX) * 65535) + (vsW - 1) / 2) / (vsW - 1 > 0 ? vsW - 1 : 1));
    const LONG ny = static_cast<LONG>(
        ((static_cast<long long>(y - vsY) * 65535) + (vsH - 1) / 2) / (vsH - 1 > 0 ? vsH - 1 : 1));

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = nx;
    in.mi.dy = ny;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
    return true;
}

// 读当前光标位置（屏幕坐标）。
inline bool GetCursorScreen(int& x, int& y) {
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    x = pt.x;
    y = pt.y;
    return true;
}

// ============================================================
// WindMouse 算法 v2：模拟人类鼠标移动轨迹（基于开源社区最热门实现）
// 参考项目：
//   - github.com/AsfhtgkDavid/windmouse     (Python, 2026 Jul 最活跃/最多维护)
//   - github.com/arevi/wind-mouse           (TS/原算法主流实现, npm 下载量最高)
//   - github.com/vectoralgorithm/wind_mouse_cpp (C++ 单头无依赖, 2025 Nov 最新)
//   - github.com/mostlyfinished/windmouse-rs   (Rust 版 target_area 概念)
//
// v2 关键修复（解决 v1 "在物品处转圈/过冲" 问题）：
//   1. 删除强制 velocity>=±1：v1 强制小速度硬变±1 造成终点反复 overshoot/回摆（像转圈）
//   2. 新增 target_area=3px 概念：进入目标周围3px立即停止，不追求完美像素，避免振荡
//   3. 近距离减速（dist<30px）：effectiveMaxStep = maxStep * (dist/30)，越近步长越小
//   4. velocity 超限时直接归一化到 effectiveMaxStep，不再加 0~50% 随机扰动（近距离精确优先）
//   5. 最大迭代次数 10000 保护：极端参数组合下不会死循环
// ============================================================
inline int WindMouse_randBetween(std::mt19937& rng, int lo, int hi) {
    if (lo >= hi) return lo;
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

inline void WindMouse(int xs, int ys, int xe, int ye,
                      int gravity, int wind, int maxStep, int stepWaitMs)
{
    if (xs == xe && ys == ye) return;

    // v4 修复：用 high_resolution_clock 纳秒精度做 seed，避免 time(nullptr) 秒级精度
    // 导致同一秒内连续调用的 rng 序列完全相同 → 反复减速转圈
    static std::mt19937 rng((unsigned int)std::chrono::high_resolution_clock::now().time_since_epoch().count());

    // v4 优化：近距离/超近距离 跳过完整轨迹，加快点击节奏
    // ★ 关键修复：把简化路径阈值提到 320px（屏幕 1080p 仓库/背包 UI 跨度约 250-300px）
    //   让绝大多数游戏场景（仓库取物/背包切换/重铸台拖拽）都走 简化 抛物线弧形路径
    //   仅 超远距离（跨屏/仓库→重铸台切换）才走 完整 WindMouse 循环
    const int kDistUltraShort = 24;   // 极短距离(<24px)：直接瞬移，人类抬手无轨迹
    const int kDistShort     = 320;   // 短距离(<320px，绝大多数游戏UI内移动)：走 4-7 步简化抛物线
    const int kTargetArea    = 3;
    const int kMaxIter       = 10000;

    int initialDist = (int)std::hypot((double)(xe - xs), (double)(ye - ys));
    int dist = initialDist;
    if (dist == 0) return;

    // —— 优化1：极短距离，直接瞬移 ——
    if (dist <= kDistUltraShort) {
        MoveCursorScreen(xe, ye);
        return;
    }

    // —— 优化2：短距离（绝大多数游戏内UI移动）：简化 3-7 步贝塞尔抛物线插值 ——
    if (dist <= kDistShort) {
        // ★ v4 步数匹配：每多一步 = SendInput 开销≈10-13ms，严格控制：
        //   相邻格(55px) = 3步 × ~13ms = ~40ms + postClick=55ms (目标<60ms ✓)
        //   隔1格(110px) = 4步 × ~13ms = ~52ms + postClick=70ms (目标<80ms ✓)
        //   跨半屏(200px+) = 6步 × ~13ms = ~78ms + postClick=96ms （完全可接受）
        int steps = 3;                        // ≤ 60px (相邻物品)
        if (dist > 60)   steps = 4;           // 61-110px (隔1格)
        if (dist > 110)  steps = 5;           // 111-200px (2-3格)
        if (dist > 200)  steps = 6;           // 201-270px
        if (dist > 270)  steps = 7;           // 271-320px (跨半屏)
        int sWait = (stepWaitMs >= 0) ? stepWaitMs : 1;
        // 曲线插值：中间点加一个垂直方向弧形弯曲（5%~15%距离），模拟人手轨迹的自然弯曲
        double curveMag = 0.05 + WindMouse_randBetween(rng, 0, 100) / 1000.0;
        int perpX = -(ye - ys);
        int perpY = (xe - xs);
        double perpLen = std::hypot((double)perpX, (double)perpY);
        double offX = 0, offY = 0;
        if (perpLen > 0.1) {
            int dir = (WindMouse_randBetween(rng, 0, 1) == 0) ? 1 : -1;
            offX = (double)perpX / perpLen * (double)dist * curveMag * dir;
            offY = (double)perpY / perpLen * (double)dist * curveMag * dir;
        }
        for (int i = 1; i <= steps; ++i) {
            double t = (double)i / steps;
            // 4*t*(1-t) 的抛物线中间凸：t=0.5 时=1,两端=0，刚好弯过中段
            double blendCurve = 4.0 * t * (1.0 - t);
            double jx = (i == steps) ? 0.0 : (double)(WindMouse_randBetween(rng, -1, 1));
            double jy = (i == steps) ? 0.0 : (double)(WindMouse_randBetween(rng, -1, 1));
            double nx = (double)xs * (1.0 - t) + (double)xe * t + offX * blendCurve + jx;
            double ny = (double)ys * (1.0 - t) + (double)ye * t + offY * blendCurve + jy;
            // stepWait=0 档：中间步不Sleep（极速档）
            if (sWait > 0 && i < steps) Sleep((DWORD)sWait);
            MoveCursorScreen((int)std::round(nx), (int)std::round(ny));
        }
        return;
    }

    double veloX = 0.0, veloY = 0.0;
    double windX = 0.0, windY = 0.0;

    int g = (gravity > 0) ? gravity : 9;
    int w = (wind > 0) ? wind : 3;
    int maxDist = (maxStep > 0) ? maxStep : 15;
    int sWait = (stepWaitMs >= 0) ? stepWaitMs : 2;
    int wEff = w;

    int curX = xs, curY = ys;

    int iter = 0;
    // ★ v4：远距离强制提前结束上限 —— 控制远距离单步耗时 ≤ 150-180ms
    // 完整循环 每轮耗时≈6-10ms (SendInput系统开销+计算)
    // 快速档目标≤200ms → 最多 25轮(≈150ms) + final settle + postClick → ~200ms
    // 平衡档目标≤300ms → 最多 40轮(≈280ms) + final settle → ~300ms
    int iterHardLimit = (sWait <= 1) ? 22 : 40;  // 快速/极速档=22轮，平衡/精准=40轮
    (void)initialDist;  // 避免未使用警告

    while (dist > kTargetArea && iter < kMaxIter) {
        ++iter;
        // ★ v4 强制收敛保护：超过 iterHardLimit 就直接最后一步到位
        // 模拟人手"瞄准远处目标 → 大致甩过去 → 最后精瞄一次"的节奏
        if (iter >= iterHardLimit ||
            (iter > 15 && dist < 180)) {   // 或 >15 轮 且距离<180px（已经进入简化路径覆盖区）
            MoveCursorScreen(xe, ye);
            if (sWait > 0) Sleep(1);
            return;
        }

        // 风力随距离减弱：靠近目标时随机扰动减弱，精确优先
        wEff = (std::min)(w, (int)(dist * 0.3));
        if (wEff < 0) wEff = 0;

        // 风场更新：EMA 平滑衰减
        if (wEff > 0) {
            windX = windX * 2.0 / 3.0 + WindMouse_randBetween(rng, 0, wEff * 2) / (double)wEff - 1.0;
            windY = windY * 2.0 / 3.0 + WindMouse_randBetween(rng, 0, wEff * 2) / (double)wEff - 1.0;
        } else {
            windX *= 0.6;
            windY *= 0.6;
        }

        // ★ v4 核心修复：重力系数提高！
        // 原公式 g/100 导致 gravity=13 → 0.13 每帧拉动
        // 改为 g/20 (×5放大)：13/20=0.65 加速显著且仍保持重力惯性轨迹
        double gFactor = (double)g / 20.0;
        veloX += windX + (double)(xe - curX) * gFactor;
        veloY += windY + (double)(ye - curY) * gFactor;

        // 远距离额外 boost：>200px 时再乘系数，模拟人手臂快速"甩"过
        if (dist > 200) {
            double boost = (std::min)(2.2, (double)dist / 180.0);
            veloX *= boost;
            veloY *= boost;
        }

        // —— v4: 近距离 smooth deceleration 加强，避免反复震荡 ——
        int effectiveMaxStep = maxDist;
        if (dist < 80) {
            // v4: 从80px就开始减速，减速公式更陡
            double decel = (std::min)(1.0, (double)dist / 80.0);
            decel = decel * decel;
            effectiveMaxStep = (std::max)(1, (int)((double)maxDist * decel));
        }

        // velocity 超限时直接归一化
        double vm = std::hypot(veloX, veloY);
        if (vm > effectiveMaxStep) {
            double ratio = (double)effectiveMaxStep / vm;
            double jitter = 1.0;
            if (dist >= 80) {
                jitter = 0.96 + WindMouse_randBetween(rng, 0, 80) / 1000.0;  // 0.96~1.04
            }
            veloX *= ratio * jitter;
            veloY *= ratio * jitter;
        }

        int lastX = curX;
        int lastY = curY;
        curX += (int)std::round(veloX);
        curY += (int)std::round(veloY);

        dist = (int)std::hypot((double)(xe - curX), (double)(ye - curY));

        int stepDist = (int)std::hypot((double)(curX - lastX), (double)(curY - lastY));
        // ★ v4 睡眠公式：远距离压缩 sleep
        // 基本原则：stepWait=1(快速档) → 单步最多 sleep 1ms，而且远距离不sleep(让SendInput自然节奏)
        // stepWait=0(极速档) → 完全不sleep
        int sleepMs = 0;
        if (sWait > 0 && maxDist > 0 && stepDist > 0) {
            // sWait=1 → (1-1)*... +0 + 偶尔 1 = 0~1 ms
            // sWait=2 → 1*(比例) +0 = 0~1 ms
            // 近距离(<80px)才保底 1ms 让瞄准稳定，远距离一律跳过节省时间
            double decel = (dist >= 80) ? 0.0 : ((double)dist / 80.0);
            sleepMs = (int)((double)sWait * (double)stepDist / (double)maxDist * decel);
            if (dist < 40 && stepDist < 5) sleepMs = 1;  // 最后精瞄 加1ms稳定
            if (sleepMs > sWait) sleepMs = sWait;
        }
        if (sleepMs > 0) Sleep((DWORD)sleepMs);

        MoveCursorScreen(curX, curY);
    }

    // 最终精确定位到目标点
    MoveCursorScreen(xe, ye);
    if (sWait > 0) Sleep(1);
}

// 人类鼠标移动：根据 Settings 启用 WindMouse 或直接跳到目标
// v2 新增 stepWaitMs 参数（每步Sleep基准毫秒），替代之前硬编码的10ms倍率
inline void HumanLikeMoveTo(int targetX, int targetY,
                            bool enableHuman, int gravity, int wind, int maxStep, int stepWaitMs)
{
    int startX = 0, startY = 0;
    GetCursorScreen(startX, startY);

    if (!enableHuman) {
        MoveCursorScreen(targetX, targetY);
        return;
    }

    WindMouse(startX, startY, targetX, targetY, gravity, wind, maxStep, stepWaitMs);
}

// 在当前光标位置发送一次左键点击（DOWN + UP 原子）。
inline void LeftClickAtCursor() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// 在当前光标位置发送一次右键点击（DOWN + UP 原子）。
inline void RightClickAtCursor() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// 移动光标到 (x, y) 后左键点击。
inline void LeftClickScreen(int x, int y) {
    MoveCursorScreen(x, y);
    LeftClickAtCursor();
}

// 移动光标到 (x, y) 后右键点击。
inline void RightClickScreen(int x, int y) {
    MoveCursorScreen(x, y);
    RightClickAtCursor();
}

// 按下 Ctrl（会话级——配合 CtrlUp 使用，连续点击期间只按一次）。
inline void CtrlDown() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    SendInput(1, &in, sizeof(INPUT));
}

// 释放 Ctrl。任何异常退出路径都必须调用，否则用户后续操作全是 Ctrl+操作。
inline void CtrlUp() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// 独立单次 Ctrl+右键点击（自带 Ctrl 按下/释放）。适合不在会话中的单次操作。
// 连续操作请改用 CtrlDown + RightClickScreen* + CtrlUp，避免重复 keystroke。
inline void CtrlRightClickScreen(int x, int y) {
    CtrlDown();
    MoveCursorScreen(x, y);
    RightClickAtCursor();
    CtrlUp();
}

// 独立单次 Ctrl+左键点击（自带 Ctrl 按下/释放）。
inline void CtrlLeftClickScreen(int x, int y) {
    CtrlDown();
    MoveCursorScreen(x, y);
    LeftClickAtCursor();
    CtrlUp();
}

// 按一次键盘键（DOWN + UP）。用于 Esc 关闭面板等。
inline void PressKey(int vk) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(vk);
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = static_cast<WORD>(vk);
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// 查询某虚拟键当前是否按下（异步，不影响状态）。
inline bool IsKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

inline bool IsLeftMouseDown()  { return IsKeyDown(VK_LBUTTON); }
inline bool IsRightMouseDown() { return IsKeyDown(VK_RBUTTON); }
inline bool IsCtrlDown()       { return IsKeyDown(VK_CONTROL); }

// 检查 ESC 物理键是否仍处于按下状态（异步查询，不影响按键状态）。
// 用于状态机按 ESC 后判断物理键是否已释放，避免门控 NoEscCancel 误判。
// VK_ESCAPE = 0x1B
inline bool IsEscPhysicallyDown() { return IsKeyDown(VK_ESCAPE); }

// 阻塞睡眠（在 OnFrame 回调里用，PoeFixer 容忍短时阻塞）。
inline void SleepMs(int ms) {
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
}

} // namespace TabletReforgeInput
