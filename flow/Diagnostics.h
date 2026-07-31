// Diagnostics.h — ring buffer 调试日志
//
// 状态机运行时记录每一步的日志，UI 可以读取最近 N 条显示。
// 用 ring buffer 避免无限增长内存。
#pragma once

#include <array>
#include <mutex>
#include <string>
#include <chrono>
#include <vector>

namespace TabletReforgeFlow {

struct LogEntry {
    std::string message;
    long long timestampMs = 0; // 距启动的毫秒
    int severity = 0;          // 0=info, 1=warn, 2=error
};

class Diagnostics {
public:
    static constexpr size_t kCapacity = 256;

    void Log(const std::string& msg, int severity = 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        LogEntry& e = m_buffer[m_head];
        e.message = msg;
        e.timestampMs = ElapsedMs();
        e.severity = severity;
        m_head = (m_head + 1) % kCapacity;
        if (m_count < kCapacity) ++m_count;
    }

    void Info(const std::string& msg)    { Log(msg, 0); }
    void Warn(const std::string& msg)    { Log(msg, 1); }
    void Error(const std::string& msg)   { Log(msg, 2); }

    // 读取最近 N 条日志（按时间正序）
    std::vector<LogEntry> Recent(size_t n) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogEntry> out;
        size_t count = (m_count < n) ? m_count : n;
        out.reserve(count);
        // m_head 指向下一个写入位置，最新的是 m_head-1
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (m_head + kCapacity - count + i) % kCapacity;
            out.push_back(m_buffer[idx]);
        }
        return out;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head = 0;
        m_count = 0;
    }

    void ResetTimer() {
        m_start = std::chrono::steady_clock::now();
    }

    long long ElapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_start).count();
    }

private:
    mutable std::mutex m_mutex;
    std::array<LogEntry, kCapacity> m_buffer{};
    size_t m_head = 0;
    size_t m_count = 0;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace TabletReforgeFlow
