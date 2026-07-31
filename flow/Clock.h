// Clock.h — 时间戳节流工具
//
// 状态机每帧被 OnFrame 调用，但很多操作需要节流（避免每帧点击）。
// Clock 提供轻量的时间戳比较，基于 std::chrono::steady_clock。
//
// 设计要点：
//   - Now() 返回 steady_clock 时间点，不受系统时间调整影响
//   - ElapsedMs() 返回距上次 Mark 过了多少毫秒
//   - WaitFor() 判断是否已等待够指定毫秒
#pragma once

#include <chrono>
#include <random>
#include <thread>

namespace TabletReforgeFlow {

class Clock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    static TimePoint Now() { return std::chrono::steady_clock::now(); }

    // 距 from 过了多少毫秒
    static long long ElapsedMs(TimePoint from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Now() - from).count();
    }

    // 距 from 是否已超过 ms 毫秒
    static bool Expired(TimePoint from, long long ms) {
        return ElapsedMs(from) >= ms;
    }

    // 阻塞睡眠（在 OnFrame 里用，PoeFixer 容忍短时阻塞）
    static void SleepMs(int ms) {
        if (ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

// 状态计时器：记录进入状态的时间，提供超时检查
class StateTimer {
public:
    void Reset() { m_start = Clock::Now(); }

    long long ElapsedMs() const { return Clock::ElapsedMs(m_start); }

    bool Expired(long long timeoutMs) const {
        return timeoutMs > 0 && Clock::Expired(m_start, timeoutMs);
    }

private:
    Clock::TimePoint m_start = Clock::Now();
};

// 节流器：限制操作频率
class Throttle {
public:
    explicit Throttle(long long intervalMs) : m_intervalMs(intervalMs) {}

    bool ShouldFire() {
        if (Clock::Expired(m_last, m_intervalMs)) {
            m_last = Clock::Now();
            return true;
        }
        return false;
    }

    void Reset() { m_last = Clock::Now(); }

private:
    long long m_intervalMs;
    Clock::TimePoint m_last = Clock::Now();
};

// ============================================================
// 【方案 B v1.3】事件触发的随机退让（宪法修正案 v1.3 频控约束）
//
// 用途：在仓库打开/重铸台打开等事件触发后，随机等待 800-1500ms 再调
// ReadItemMods 的 mod 容器遍历，防止周期性轮询被反作弊检测。
//
// 使用方式：
//   1. 事件触发时（如仓库面板打开）：调用 Arm() 激活随机退让
//   2. 扫描函数入口调用 ShouldFire()，true=可执行，false=本帧跳过
//   3. ShouldFire() 首次触发需等待 800-1500ms 随机时长
//   4. 首次触发后，后续 500ms 节流防短期重复
// ============================================================
class RandomBackoff {
public:
    RandomBackoff() : m_armTime(Clock::Now()), m_last(Clock::Now()) {}

    // 事件触发：重置退让计时器，随机 800-1500ms 后允许首次执行
    void Arm() {
        m_fired = false;
        m_armTime = Clock::Now();
        // std::random_device 生成真随机数（非确定性种子）
        std::random_device rd;
        std::uniform_int_distribution<int> dist(kMinDelayMs, kMaxDelayMs);
        m_delayMs = dist(rd);
    }

    // 是否可以执行扫描（true=可以，false=本帧跳过）
    bool ShouldFire() {
        if (!m_fired) {
            // 首次触发：等待随机退让时长
            if (Clock::ElapsedMs(m_armTime) >= m_delayMs) {
                m_fired = true;
                m_last = Clock::Now();
                return true;
            }
            return false;
        }
        // 后续：500ms 节流防短期重复
        if (Clock::Expired(m_last, kRepeatIntervalMs)) {
            m_last = Clock::Now();
            return true;
        }
        return false;
    }

    // 是否已 Arm 但未首次触发（用于诊断日志）
    bool IsArming() const { return !m_fired; }

    // 当前退让时长（用于诊断日志，ms）
    int DelayMs() const { return m_delayMs; }

    // 用于 Mock 测试：强制设置退让时长（跳过随机）
    void ArmWithDelayForTest(int delayMs) {
        m_fired = false;
        m_armTime = Clock::Now();
        m_delayMs = delayMs;
    }

    // 用于 Mock 测试：强制标记已触发
    void ForceFiredForTest() {
        m_fired = true;
        m_last = Clock::Now();
    }

private:
    static constexpr int kMinDelayMs = 800;
    static constexpr int kMaxDelayMs = 1500;
    static constexpr long long kRepeatIntervalMs = 500;

    bool m_fired = false;
    Clock::TimePoint m_armTime = Clock::Now();
    Clock::TimePoint m_last = Clock::Now();
    int m_delayMs = kMinDelayMs;
};

} // namespace TabletReforgeFlow
