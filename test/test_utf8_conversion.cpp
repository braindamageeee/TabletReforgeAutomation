// test_utf8_conversion.cpp
// 测试 GetStringIdUtf8 相关的 UTF-8 转换函数
// 编译：cl /EHsc /std:c++17 /UTF-8 test_utf8_conversion.cpp

#include <windows.h>
#include <string>
#include <iostream>
#include <vector>
#include <utility>

// 从 ReforgeOps.h 复制的辅助函数（用于独立测试）
inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string utf8(needed - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], needed, nullptr, nullptr);
    return utf8;
}

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring wide(needed - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], needed);
    return wide;
}

inline bool IsGarbledText(const std::string& s) {
    if (s.empty()) return true;
    int garbledCount = 0;
    int totalChars = static_cast<int>(s.size());
    for (int i = 0; i < totalChars; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '?' || (c < 32 && c != '\t' && c != '\n' && c != '\r')) {
            garbledCount++;
        }
    }
    return garbledCount > totalChars * 0.3;
}

// 模拟旧的 FetchWStringNarrow 函数（有 bug 的版本）
inline std::string FetchWStringNarrow_BUGGY(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) {
        s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return s;
}

// 测试辅助函数
struct TestResult {
    std::string name;
    bool passed;
    std::string detail;
};

std::vector<TestResult> testResults;

void Test(const std::string& name, bool condition, const std::string& detail = "") {
    TestResult result;
    result.name = name;
    result.passed = condition;
    result.detail = detail;
    testResults.push_back(result);
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name;
    if (!detail.empty()) std::cout << " - " << detail;
    std::cout << std::endl;
}

// 测试 1: 英文标识符转换（StringId 通常是英文）
void TestEnglishIdentifier() {
    std::cout << "\n=== Test 1: English Identifier Conversion ===" << std::endl;
    
    std::vector<std::wstring> englishStringIds = {
        L"IdentifyButton",
        L"IdentifyItem",
        L"IdentifyItems",
        L"IdentifyAll",
        L"IdentifyPanel",
        L"Doryani",
        L"ScrollOfWisdom",
        L"IdentifyItemsBtn"
    };
    
    for (const auto& wstr : englishStringIds) {
        std::string utf8 = WideToUtf8(wstr);
        std::string buggy = FetchWStringNarrow_BUGGY(wstr);
        std::string asciiStr(wstr.begin(), wstr.end());
        
        // 新方法应该正确转换（英文在 UTF-16 和 UTF-8 中都是 ASCII）
        Test("WideToUtf8: " + asciiStr, 
             utf8 == asciiStr,
             "result: '" + utf8 + "'");
        
        // 旧方法也能处理英文（因为都是 ASCII）
        Test("FetchWStringNarrow: " + asciiStr, 
             buggy == asciiStr,
             "result: '" + buggy + "'");
    }
}

// 测试 2: 中文文本转换
void TestChineseText() {
    std::cout << "\n=== Test 2: Chinese Text Conversion ===" << std::endl;
    
    // 使用 UTF-8 字面量
    std::vector<std::pair<std::wstring, std::string>> chinesePairs = {
        {L"\u9451\u5b9a", u8"\u9451\u5b9a"},  // 鑑定
        {L"\u9274\u5b9a", u8"\u9274\u5b9a"},  // 鉴定
        {L"\u9451\u5b9a\u7269\u54c1", u8"\u9451\u5b9a\u7269\u54c1"},  // 鑑定物品
        {L"\u9274\u5b9a\u7269\u54c1", u8"\u9274\u5b9a\u7269\u54c1"},  // 鉴定物品
        {L"\u9451\u5b9a\u5168\u90e8", u8"\u9451\u5b9a\u5168\u90e8"},  // 鑑定全部
        {L"\u9274\u5b9a\u5168\u90e8", u8"\u9274\u5b9a\u5168\u90e8"},  // 鉴定全部
        {L"\u591a\u5229\u4e9a\u5c3c", u8"\u591a\u5229\u4e9a\u5c3c"},  // 多利亚尼
        {L"\u5c0d\u8a71", u8"\u5c0d\u8a71"},  // 對話
        {L"\u5bf9\u8bdd", u8"\u5bf9\u8bdd"}   // 对话
    };
    
    for (const auto& pair : chinesePairs) {
        const auto& wstr = pair.first;
        const auto& expectedUtf8 = pair.second;
        std::string utf8 = WideToUtf8(wstr);
        std::string buggy = FetchWStringNarrow_BUGGY(wstr);
        
        bool utf8Valid = !utf8.empty() && !IsGarbledText(utf8);
        Test("WideToUtf8 Chinese: len=" + std::to_string(wstr.size()), 
             utf8Valid,
             "expected utf8 len=" + std::to_string(expectedUtf8.size()) + 
             " got len=" + std::to_string(utf8.size()) +
             " match=" + std::string(utf8 == expectedUtf8 ? "yes" : "no"));
        
        bool buggyIsGarbled = IsGarbledText(buggy);
        Test("FetchWStringNarrow should be garbled", 
             buggyIsGarbled,
             "result: '" + buggy + "'");
    }
}

// 测试 3: 混合文本（英文 + 中文）
void TestMixedText() {
    std::cout << "\n=== Test 3: Mixed Text ===" << std::endl;
    
    std::vector<std::wstring> mixedTexts = {
        L"IdentifyButton_\u9451\u5b9a",  // IdentifyButton_鑑定
        L"Identify_\u9274\u5b9a",  // Identify_鉴定
        L"ScrollOfWisdom_\u591a\u5229\u4e9a\u5c3c"  // ScrollOfWisdom_多利亚尼
    };
    
    for (const auto& wstr : mixedTexts) {
        std::string utf8 = WideToUtf8(wstr);
        std::string buggy = FetchWStringNarrow_BUGGY(wstr);
        
        bool utf8Valid = !utf8.empty() && !IsGarbledText(utf8);
        Test("WideToUtf8 Mixed: wchar_len=" + std::to_string(wstr.size()), 
             utf8Valid,
             "utf8_len: " + std::to_string(utf8.size()));
        
        // 检查旧方法丢失了多少信息
        int lostChars = 0;
        for (wchar_t c : wstr) {
            if (c >= 0x80) lostChars++;
        }
        Test("FetchWStringNarrow lost chars", 
             lostChars > 0 && buggy.find('?') != std::string::npos,
             "lost " + std::to_string(lostChars) + " non-ASCII chars");
    }
}

// 测试 4: 往返转换一致性
void TestRoundtripConversion() {
    std::cout << "\n=== Test 4: Roundtrip Conversion ===" << std::endl;
    
    std::vector<std::string> utf8Inputs = {
        u8"\u9274\u5b9a",  // 鉴定
        u8"\u9451\u5b9a\u7269\u54c1",  // 鑑定物品
        u8"\u591a\u5229\u4e9a\u5c3c",  // 多利亚尼
        "IdentifyButton",
        "Identify_" u8"\u9274\u5b9a"  // Identify_鉴定
    };
    
    for (const auto& input : utf8Inputs) {
        std::wstring wide = Utf8ToWide(input);
        std::string roundtrip = WideToUtf8(wide);
        
        bool match = (input == roundtrip);
        Test("Roundtrip convert", 
             match,
             match ? "OK" : "mismatch");
    }
}

// 测试 5: IsGarbledText 检测准确性
void TestGarbledDetection() {
    std::cout << "\n=== Test 5: IsGarbledText Detection ===" << std::endl;
    
    // 应该被检测为乱码的字符串
    std::vector<std::pair<std::string, bool>> testCases = {
        {"????", true},
        {u8"\u9451\u5b9a", false},  // 鑑定
        {u8"\u9274\u5b9a", false},  // 鉴定
        {"Identify", false},
        {"Identify????", true},
        {"a?b?c?d?", true},
        {"", true},
        {"Hello", false},
    };
    
    for (const auto& testCase : testCases) {
        const auto& input = testCase.first;
        bool expected = testCase.second;
        bool result = IsGarbledText(input);
        bool pass = (result == expected);
        std::string display = input.empty() ? "(empty)" : input;
        Test("IsGarbledText check", 
             pass,
             "expected=" + std::string(expected ? "garbled" : "normal") + 
             " actual=" + std::string(result ? "garbled" : "normal") +
             " input_len=" + std::to_string(input.size()));
    }
}

// 测试 6: 模拟中文客户端 StringId 读取场景
void TestSimulatedChineseClient() {
    std::cout << "\n=== Test 6: Simulated Chinese Client StringId Reading ===" << std::endl;
    
    struct SimulatedNode {
        std::wstring stringId;
        std::wstring text;
    };
    
    std::vector<SimulatedNode> uiTree = {
        {L"RootPanel", L"\u6839\u9762\u677f"},  // 根面板
        {L"NpcDialog", L"NPC\u5bf9\u8bdd"},  // NPC对话
        {L"IdentifyButton", L"\u9274\u5b9a"},  // 鉴定
        {L"IdentifyAllButton", L"\u9274\u5b9a\u5168\u90e8"},  // 鉴定全部
        {L"CloseButton", L"\u5173\u95ed"},  // 关闭
        {L"DoryaniPortrait", L"\u591a\u5229\u4e9a\u5c3c\u8096\u50cf"},  // 多利亚尼肖像
        {L"DialogBackground", L"\u5bf9\u8bdd\u80cc\u666f"}  // 对话背景
    };
    
    std::cout << "\n--- Simulating UI tree traversal ---" << std::endl;
    
    int foundByStringId = 0;
    int foundByText = 0;
    
    for (size_t i = 0; i < uiTree.size(); ++i) {
        const auto& node = uiTree[i];
        
        // 通过宽字符读取 StringId（模拟 GetStringIdUtf8 的方式）
        std::string stringIdUtf8 = WideToUtf8(node.stringId);
        std::string textUtf8 = WideToUtf8(node.text);
        
        bool isIdentify = false;
        
        // 策略 1: 通过 StringId 匹配
        if (!IsGarbledText(stringIdUtf8)) {
            if (stringIdUtf8.find("Identify") != std::string::npos &&
                stringIdUtf8.find("Button") != std::string::npos) {
                isIdentify = true;
                foundByStringId++;
                std::cout << "  [StringId Match] node_idx=" << i 
                         << " stringId='" << stringIdUtf8 << "'" << std::endl;
            }
        }
        
        // 策略 2: 通过文本匹配（中文）
        if (!isIdentify && !IsGarbledText(textUtf8)) {
            if (textUtf8.find(u8"\u9274\u5b9a") != std::string::npos ||  // 鉴定
                textUtf8.find(u8"\u9451\u5b9a") != std::string::npos) {  // 鑑定
                isIdentify = true;
                foundByText++;
                std::cout << "  [Text Match] node_idx=" << i 
                         << " text_utf8_len=" << textUtf8.size() << std::endl;
            }
        }
        
        // 如果用旧方法（FetchWStringNarrow）
        std::string buggyStringId = FetchWStringNarrow_BUGGY(node.stringId);
        std::string buggyText = FetchWStringNarrow_BUGGY(node.text);
        
        if (IsGarbledText(buggyStringId) || IsGarbledText(buggyText)) {
            std::cout << "  [OLD METHOD FAIL] node_idx=" << i 
                     << " stringId='" << buggyStringId << "'" 
                     << " text='" << buggyText << "'" << std::endl;
        }
    }
    
    Test("Found identify button by StringId", foundByStringId > 0,
         "found " + std::to_string(foundByStringId) + " node(s)");
    Test("Found identify button by text or StringId", foundByText > 0 || foundByStringId > 0,
         "stringId_hits=" + std::to_string(foundByStringId) + 
         " text_hits=" + std::to_string(foundByText));
}

// 测试 7: 边界情况
void TestEdgeCases() {
    std::cout << "\n=== Test 7: Edge Cases ===" << std::endl;
    
    // 空字符串
    Test("WideToUtf8 empty string", WideToUtf8(L"").empty());
    Test("Utf8ToWide empty string", Utf8ToWide("").empty());
    Test("IsGarbledText empty string", IsGarbledText(""));
    
    // 纯 ASCII
    std::wstring ascii = L"Hello World 123";
    std::string asciiUtf8 = WideToUtf8(ascii);
    Test("WideToUtf8 pure ASCII", asciiUtf8 == "Hello World 123");
    
    // 特殊字符
    std::wstring special = L"!@#$%^&*()_+-=[]{}|;':\",./<>?";
    std::string specialUtf8 = WideToUtf8(special);
    std::string specialExpected(special.begin(), special.end());
    Test("WideToUtf8 special chars", specialUtf8 == specialExpected);
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "GetStringIdUtf8 Function Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    
    TestEnglishIdentifier();
    TestChineseText();
    TestMixedText();
    TestRoundtripConversion();
    TestGarbledDetection();
    TestSimulatedChineseClient();
    TestEdgeCases();
    
    // 汇总结果
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int passed = 0;
    int failed = 0;
    for (const auto& r : testResults) {
        if (r.passed) passed++;
        else failed++;
    }
    
    std::cout << "Total tests: " << testResults.size() << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    
    if (failed > 0) {
        std::cout << "\nFailed tests:" << std::endl;
        for (const auto& r : testResults) {
            if (!r.passed) {
                std::cout << "  - " << r.name;
                if (!r.detail.empty()) std::cout << " (" << r.detail << ")";
                std::cout << std::endl;
            }
        }
    } else {
        std::cout << "\nAll tests PASSED!" << std::endl;
    }
    
    return failed == 0 ? 0 : 1;
}