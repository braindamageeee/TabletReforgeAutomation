#include "../sdk/PluginSDK.h"
#include "../config/CalibData.h"
#include "../config/Settings.h"
#include "../flow/StateMachine.h"
#include "MockGameState.h"

#include <iostream>
#include <fstream>
#include <filesystem>

int main() {
    std::vector<std::string> logLines;

    TabletReforgeTest::RunAllTests([&logLines](const std::string& msg) {
        logLines.push_back(msg);
        std::cout << msg << std::endl;
    });

    auto logDir = std::filesystem::current_path() / "logs";
    std::filesystem::create_directories(logDir);

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char timestamp[32];
    ::sprintf_s(timestamp, "%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    auto logPath = logDir / ("test_log_" + std::string(timestamp) + ".txt");

    std::ofstream out(logPath);
    if (out.is_open()) {
        out << "=== 状态机 Mock 测试日志 ===" << std::endl;
        out << "导出时间: " << timestamp << std::endl;
        out << std::endl;
        for (const auto& line : logLines) {
            out << line << std::endl;
        }
        out.close();
        std::cout << std::endl << "日志已导出: " << logPath << std::endl;
    } else {
        std::cerr << "错误: 无法打开文件进行写入" << std::endl;
        return 1;
    }

    return 0;
}
