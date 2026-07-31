// AtomicWrite.h — 原子写入 JSON 配置文件
//
// 解决问题：Settings::Save() / CalibData::Save() 早期用 std::ofstream 直接写目标文件，
// 写入过程中崩溃（断电 / 异常 / 用户强杀）会导致 settings.json 半写损坏。
//
// 策略（参考 Gamehelper/Utils/JsonHelper.cs 的 SafeToFile）：
//   1. 先写到临时文件 <target>.tmp（同一目录，确保同卷）
//   2. flush + close 后，用 std::filesystem::rename 原子替换目标
//      （Windows 上 rename = MoveFileExW(MOVEFILE_REPLACE_EXISTING)，原子覆盖）
//   3. rename 失败兜底：先 remove 目标再 rename；仍失败则保留旧文件不覆盖，
//      仅删除 .tmp（避免堆积垃圾），并通过返回值报告失败让调用方记 diag.Error
//
// 安全：所有操作只读写本地配置文件，零游戏交互，无封号风险。
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace TabletReforgeConfig {

// 原子写入文本内容到 target 路径。
// 返回 true 表示成功；false 表示最终未能更新 target（旧文件保留或也不存在）。
inline bool AtomicWriteText(const std::filesystem::path& target,
                            const std::string& content) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // 确保父目录存在（Save() 旧版也做了 create_directories，这里保留以兼容）
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        // 忽略 ec：目录已存在不算错
    }

    // 临时文件路径：同目录同卷，确保 rename 是原子操作
    const auto tmp = target.string() + ".tmp";

    // 步骤 1：写 .tmp（二进制 + trunc，避免被前次遗留内容污染）
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            // .tmp 打不开（只读目录？）—— 回退到直写 target，至少保证能写就写
            std::ofstream fallback(target, std::ios::binary | std::ios::trunc);
            if (fallback.is_open()) {
                fallback << content;
                fallback.flush();
            }
            return false;
        }
        out << content;
        out.flush();
        // 析构会 close，但显式 close 以便立即拿到失败状态
        out.close();
        if (!out) {
            // 写入过程中失败：清理 .tmp，保留旧 target
            fs::remove(tmp, ec);
            return false;
        }
    }

    // 步骤 2：rename(.tmp, target) —— Windows 上原子覆盖
    fs::rename(tmp, target, ec);
    if (!ec) {
        return true;
    }

    // 步骤 3 兜底：rename 失败（极少见，可能是目标被另一进程持有）
    // 尝试先 remove(target) 再 rename
    fs::remove(target, ec);
    fs::rename(tmp, target, ec);
    if (!ec) {
        return true;
    }

    // 仍失败：保留旧 target（如果存在），清理 .tmp
    fs::remove(tmp, ec);
    return false;
}

} // namespace TabletReforgeConfig
