// CalibData.h — 标定数据（calib.json，可分享给同版本玩家）
//
// 与 Settings.h 分离的原因：
//   - Settings 是个人偏好（时序、热键），换机器要重调
//   - CalibData 是游戏版本相关（Path/StringId/坐标），同版本玩家可共享
//
// 设计要点：
//   - StringId 优先（游戏更新后更稳定），坐标作为兜底
//   - useManualCoords=true 时强制用坐标，绕过 UI 树遍历（v0.2 标定向导用）
//   - complete 标志：所有必填项都有值才能启动自动化
//   - 兜底坐标用 -1 表示未设置，避免和合法的 0,0 混淆
#pragma once

#include "../third_party/json.hpp"
#include "AtomicWrite.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace TabletReforgeConfig {

struct CalibData {
    // —— 重铸台实体（WorldToScreen 用）——
    std::string benchEntityPath;      // 重铸台实体的 Path（如 "Metadata/.../reforge_bench"）

    // —— 仓库实体（从重铸台回仓库时点击用，可选——留空则需用户手动开仓库）——
    std::string stashEntityPath;      // 仓库实体的 Path（如 "Metadata/.../stash"）

    // —— 合成按钮（重铸台面板里的"合成"按钮）——
    std::string combineButtonStringId; // UI StringId（优先）
    int combineButtonX = -1;           // 兜底坐标 X（屏幕坐标）
    int combineButtonY = -1;           // 兜底坐标 Y

    // —— 产物槽（合成产物出现的位置）——
    std::string outputSlotStringId;    // UI StringId（优先）
    int outputSlotX = -1;              // 兜底坐标 X
    int outputSlotY = -1;              // 兜底坐标 Y

    // —— 重铸台面板（用于检测面板是否打开）——
    std::string benchPanelStringId;    // 重铸台面板的 StringId

    // —— NPC 鉴定（多利亚尼 Doryani，用于鉴定未鉴定的魔法/稀有碑牌）——
    std::string npcEntityPath;         // NPC 实体 Path（如 "Metadata/.../Doryani"）
    std::string npcDialogStringId;     // NPC 对话面板 StringId
    std::string identifyButtonStringId;// "鉴定/Identify" 按钮 StringId
    int identifyButtonX = -1;          // 鉴定按钮兜底坐标 X
    int identifyButtonY = -1;          // 鉴定按钮兜底坐标 Y

    // —— 模式控制 ——
    bool useManualCoords = false;      // true=强制用坐标，false=优先用 StringId

    // —— 标定完整性检查 ——
    bool IsComplete() const {
        // 重铸台 Path 必填
        if (benchEntityPath.empty()) return false;

        // 合成按钮：StringId 或坐标二选一
        const bool hasCombineId = !combineButtonStringId.empty();
        const bool hasCombineXY = combineButtonX >= 0 && combineButtonY >= 0;
        if (!hasCombineId && !hasCombineXY) return false;

        // 产物槽：StringId 或坐标二选一
        const bool hasOutputId = !outputSlotStringId.empty();
        const bool hasOutputXY = outputSlotX >= 0 && outputSlotY >= 0;
        if (!hasOutputId && !hasOutputXY) return false;

        return true;
    }

    // 缺失项描述（给 UI 显示用）
    std::string MissingDescription() const {
        std::string missing;
        if (benchEntityPath.empty()) missing += "重铸台实体 Path; ";
        if (combineButtonStringId.empty() && combineButtonX < 0) missing += "合成按钮位置; ";
        if (outputSlotStringId.empty() && outputSlotX < 0) missing += "产物槽位置; ";
        return missing;
    }

    // —— 文件路径 ——
    std::filesystem::path CalibPath(const std::filesystem::path& pluginDir) const {
        return pluginDir / "config" / "calib.json";
    }

    // —— 加载 ——
    void Load(const std::filesystem::path& pluginDir) {
        const auto path = CalibPath(pluginDir);
        if (!std::filesystem::exists(path)) return;
        std::ifstream in(path);
        if (!in.is_open()) return;
        try {
            nlohmann::json j;
            in >> j;

            benchEntityPath       = j.value("bench_entity_path", benchEntityPath);
            stashEntityPath       = j.value("stash_entity_path", stashEntityPath);
            combineButtonStringId = j.value("combine_button_string_id", combineButtonStringId);
            combineButtonX        = j.value("combine_button_x", combineButtonX);
            combineButtonY        = j.value("combine_button_y", combineButtonY);
            outputSlotStringId    = j.value("output_slot_string_id", outputSlotStringId);
            outputSlotX           = j.value("output_slot_x", outputSlotX);
            outputSlotY           = j.value("output_slot_y", outputSlotY);
            benchPanelStringId    = j.value("bench_panel_string_id", benchPanelStringId);
            useManualCoords       = j.value("use_manual_coords", useManualCoords);

            // NPC 鉴定配置
            npcEntityPath         = j.value("npc_entity_path", npcEntityPath);
            npcDialogStringId     = j.value("npc_dialog_string_id", npcDialogStringId);
            identifyButtonStringId= j.value("identify_button_string_id", identifyButtonStringId);
            identifyButtonX       = j.value("identify_button_x", identifyButtonX);
            identifyButtonY       = j.value("identify_button_y", identifyButtonY);
        } catch (...) {
            // 保留默认值
        }
    }

    // —— 保存 ——
    void Save(const std::filesystem::path& pluginDir) const {
        std::error_code ec;
        std::filesystem::create_directories(pluginDir / "config", ec);

        nlohmann::json j;
        j["bench_entity_path"]          = benchEntityPath;
        j["stash_entity_path"]          = stashEntityPath;
        j["combine_button_string_id"]   = combineButtonStringId;
        j["combine_button_x"]           = combineButtonX;
        j["combine_button_y"]           = combineButtonY;
        j["output_slot_string_id"]      = outputSlotStringId;
        j["output_slot_x"]              = outputSlotX;
        j["output_slot_y"]              = outputSlotY;
        j["bench_panel_string_id"]      = benchPanelStringId;
        j["use_manual_coords"]          = useManualCoords;

        // NPC 鉴定配置
        j["npc_entity_path"]            = npcEntityPath;
        j["npc_dialog_string_id"]       = npcDialogStringId;
        j["identify_button_string_id"]  = identifyButtonStringId;
        j["identify_button_x"]          = identifyButtonX;
        j["identify_button_y"]          = identifyButtonY;

        // 原子写入：tmp → rename，避免半写损坏（参考 AtomicWrite.h）
        const auto target = CalibPath(pluginDir);
        const std::string content = j.dump(2);
        if (!AtomicWriteText(target, content)) {
            // 原子写失败兜底：AtomicWriteText 内部已尝试直写，
            // 这里不再重复。调用方可通过下次 Load 验证内容是否完好。
        }
    }
};

} // namespace TabletReforgeConfig
