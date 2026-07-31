// StateMachine.h — 核心状态机
//
// 实现"取碑牌→合成→存回"的完整自动化循环。每帧由 OnFrame 调用 Tick(ctx)。
//
// 状态流（详见计划文档）：
//   Idle → PreCheck → ScanStash → WaitForStashScan → Withdrawing →
//   CloseStashAndOpenBench → WaitForBenchPanel → PlacingTablets →
//   VerifySlotsFilled → ClickCombine → WaitForCombine → VerifyCombineOutput →
//   ExtractOutput → CloseBenchAndOpenStash → WaitForStashPanel → Depositing →
//   LoopCheck → (回 ScanStash 或 Idle)
//
// Ctrl 会话：Withdrawing/PlacingTablets/Depositing 期间 CtrlDown，退出时 CtrlUp。
// Abort() 无条件 CtrlUp + 光标移角落 + 回 Idle。
//
// 安全：每帧 Tick 开头检查门控，命中即 Abort。全程 try/catch 防崩宿主。
// 只读零风险字段（Path/Rarity/IsIdentified/ItemLevel 等汇总字段），绝不读词缀文本内容
// （ExplicitMods/ImplicitMods 等）。ReadItemMods 调用本身允许但仅限取汇总字段，
// 详见项目宪法 §3.5 与 test/AUDIT_RULES.md 规则 A。
#pragma once

#include "../config/CalibData.h"
#include "../config/Settings.h"
#include "../game/InventoryChecker.h"
#include "../game/PanelDetector.h"
#include "../game/ReformatoryFinder.h"
#include "../game/ReforgeOps.h"
#include "../game/StashOps.h"
#include "../game/TabletFilter.h"
#include "../game/UiTreeWalker.h"
#include "../input/Win32Input.h"
#include "Clock.h"
#include "Diagnostics.h"
#include "Gates.h"
#include "../sdk/PluginSDK.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace TabletReforgeFlow {

enum class State {
    Idle,
    PreCheck,
    EnsureStashOpen,
    ScanStash,
    WaitForStashScan,
    Withdrawing,
    CloseStashAndOpenBench,
    WaitForBenchPanel,
    PlacingTablets,
    VerifySlotsFilled,
    ClickCombine,
    WaitForCombine,
    VerifyCombineOutput,
    ExtractOutput,
    CloseBenchAndOpenStash,
    WaitForStashPanel,
    Depositing,
    // NPC 鉴定流程（当背包有未鉴定的魔法/稀有碑牌时触发）
    InteractWithNPC,         // 走向NPC并互动（多利亚尼）
    WaitForNPCDialog,        // 等待NPC对话界面打开
    ClickIdentifyButton,     // 点击鉴定按钮
    WaitForIdentification,   // 等待鉴定完成
    // 循环控制
    LoopCheck,
    Aborting,
    ErrorWait,
};

inline const char* StateName(State s) {
    switch (s) {
        case State::Idle:                   return "空闲";
        case State::PreCheck:               return "预检查";
        case State::EnsureStashOpen:        return "打开仓库面板";
        case State::ScanStash:              return "扫描仓库";
        case State::WaitForStashScan:       return "等待扫描完成";
        case State::Withdrawing:            return "取出物品";
        case State::CloseStashAndOpenBench: return "关仓库开重铸台";
        case State::WaitForBenchPanel:      return "等待合成面板";
        case State::PlacingTablets:         return "放入物品";
        case State::VerifySlotsFilled:      return "验证放入";
        case State::ClickCombine:           return "点击合成";
        case State::WaitForCombine:         return "等待合成完成";
        case State::VerifyCombineOutput:    return "验证产物";
        case State::ExtractOutput:          return "取出产物";
        case State::CloseBenchAndOpenStash: return "关重铸台开仓库";
        case State::WaitForStashPanel:      return "等待仓库面板";
        case State::Depositing:             return "存回物品";
        case State::InteractWithNPC:        return "与NPC互动";
        case State::WaitForNPCDialog:       return "等待NPC对话";
        case State::ClickIdentifyButton:    return "点击鉴定";
        case State::WaitForIdentification:  return "等待鉴定";
        case State::LoopCheck:              return "循环检查";
        case State::Aborting:               return "中止中";
        case State::ErrorWait:              return "错误等待";
    }
    return "未知";
}

class StateMachine {
public:
    // —— 配置（主入口在 OnEnable 时设置）——
    TabletReforgeConfig::Settings settings;
    TabletReforgeConfig::CalibData calib;
    Diagnostics diag;

    // —— 启停 ——
    void Start(const PluginSDK::Context* ctx) {
        if (m_state != State::Idle && m_state != State::ErrorWait) return;
        if (!ctx) return;
        if (!calib.IsComplete()) {
            diag.Error("标定不完整: " + calib.MissingDescription());
            m_lastError = "标定不完整，请先完成标定";
            Transition(State::ErrorWait);
            return;
        }
        m_loopCount = 0;
        m_placedThisRound = 0;
        m_withdrawnThisLoop = 0;
        m_identifyCheckDoneThisLoop = false;
        m_bagSnapshot.clear();
        m_productAddresses.clear();
        m_ctrlHeld = false;
        m_npcEscPressed = false;
        m_escPressedByMachine = false;
        ResetStackableTracking();
        // 启动时先记录区域基线，避免第一帧门控检查就报"切换了区域"
        m_areaBaseline = ctx->Game.GetAreaChangeCounter();
        diag.ResetTimer();
        diag.Info("=== 启动重铸自动化 ===");
        Transition(State::PreCheck);
        // 注意：PreCheck 通过后会切到 EnsureStashOpen，再到 ScanStash
    }

    void Stop() {
        if (m_state != State::Idle) {
            diag.Info("用户停止");
            Abort("用户停止");
        }
    }

    // 测试模式：跑 1 轮后自动停止（标定向导验证用）
    void StartTest(const PluginSDK::Context* ctx) {
        m_testMode = true;
        diag.Info("=== 测试标定（仅跑 1 轮）===");
        Start(ctx);
    }

    bool IsTestMode() const { return m_testMode; }

    bool IsRunning() const {
        return m_state != State::Idle && m_state != State::ErrorWait;
    }

    State CurrentState() const { return m_state; }
    const std::string& LastError() const { return m_lastError; }
    int LoopCount() const { return m_loopCount; }

    std::vector<LogEntry> GetRecentLogs(size_t n) const {
        return diag.Recent(n);
    }

    void ClearError() {
        if (m_state == State::ErrorWait) {
            m_lastError.clear();
            Transition(State::Idle);
        }
    }

    // —— 每帧驱动（OnFrame 调用）——
    void Tick(const PluginSDK::Context* ctx) {
        if (!ctx) return;

        // === 调试热键 F9：即使 Idle 状态也能触发 Mock 筛选/排序测试 ===
        // 测试结果通过 OutputDebugStringA 输出，用 DebugView 或
        // F:\Trae\chuxue\debug\bug1.log 查看 [MOCK TEST] 开头的行
        // 注：原 F8 已被其他调试面板占用，故改用 F9
        if ((::GetAsyncKeyState(VK_F9) & 0x0001) != 0) {
            try {
                TabletReforgeGame::RunMockFilterTests();
            } catch (...) {
                OutputDebugStringA("[MOCK TEST] 异常: RunMockFilterTests 抛出\n");
            }
        }

        if (m_state == State::Idle || m_state == State::ErrorWait) return;

        try {
            // 全局门控检查（Aborting/ErrorWait 例外）
            if (m_state != State::Aborting && m_state != State::ErrorWait) {
                if (!CheckGates(ctx)) return;
            }

            // 状态超时检查
            if (m_state != State::Aborting
                && m_state != State::ErrorWait
                && m_stateTimer.Expired(settings.stateTimeoutMs)) {
                // P3：超时前先 dump 完整 ctx 快照（门控/面板/计数器），便于事后排查
                DumpTimeoutSnapshot(ctx);
                Abort("状态超时: " + std::string(StateName(m_state)));
                return;
            }

            switch (m_state) {
                case State::Idle:                                      break;
                case State::PreCheck:               TickPreCheck(ctx);              break;
                case State::EnsureStashOpen:        TickEnsureStashOpen(ctx);       break;
                case State::ScanStash:              TickScanStash(ctx);             break;
                case State::WaitForStashScan:       TickWaitForStashScan(ctx);      break;
                case State::Withdrawing:            TickWithdrawing(ctx);           break;
                case State::CloseStashAndOpenBench: TickCloseStashAndOpenBench(ctx);break;
                case State::WaitForBenchPanel:      TickWaitForBenchPanel(ctx);     break;
                case State::PlacingTablets:         TickPlacingTablets(ctx);        break;
                case State::VerifySlotsFilled:      TickVerifySlotsFilled(ctx);     break;
                case State::ClickCombine:           TickClickCombine(ctx);          break;
                case State::WaitForCombine:         TickWaitForCombine(ctx);        break;
                case State::VerifyCombineOutput:    TickVerifyCombineOutput(ctx);   break;
                case State::ExtractOutput:          TickExtractOutput(ctx);         break;
                case State::CloseBenchAndOpenStash: TickCloseBenchAndOpenStash(ctx);break;
                case State::WaitForStashPanel:      TickWaitForStashPanel(ctx);     break;
                case State::Depositing:             TickDepositing(ctx);            break;
                case State::InteractWithNPC:        TickInteractWithNPC(ctx);       break;
                case State::WaitForNPCDialog:       TickWaitForNPCDialog(ctx);      break;
                case State::ClickIdentifyButton:    TickClickIdentifyButton(ctx);  break;
                case State::WaitForIdentification:  TickWaitForIdentification(ctx); break;
                case State::LoopCheck:              TickLoopCheck(ctx);             break;
                case State::Aborting:               TickAborting(ctx);              break;
                case State::ErrorWait:                                               break;
            }
        } catch (...) {
            // 任何异常都 Abort，绝不崩宿主
            Abort("Tick 异常");
        }
    }

private:
    // —— 状态与计时 ——
    State m_state = State::Idle;
    StateTimer m_stateTimer;
    Clock::TimePoint m_lastAction = Clock::Now();
    Clock::TimePoint m_npcDialogWaitStart = Clock::Now();  // NPC 对话等待开始时间

    // 【方案 B v1.3】RandomBackoff 频控成员（宪法修正案 v1.3 频控约束）
    // 事件触发后随机退让 800-1500ms 再允许 ReadItemMods 的 mod 容器遍历
    RandomBackoff m_stashBackoff;   // 仓库面板打开事件
    RandomBackoff m_benchBackoff;   // 重铸台面板打开事件
    RandomBackoff m_bagBackoff;     // 背包扫描事件

    // —— 运行时状态 ——
    bool m_ctrlHeld = false;
    bool m_testMode = false;  // 测试模式：跑 1 轮自动停
    bool m_escPressedByMachine = false;  // 状态机自己按的 Esc，避免门控误判
    bool m_npcEscPressed = false;  // NPC 交互前是否已按过 ESC
    Clock::TimePoint m_npcEscPressTime = Clock::Now();  // NPC 交互前按 ESC 的时间
    int m_loopCount = 0;
    uint64_t m_areaBaseline = 0;
    int m_placedThisRound = 0;
    int m_withdrawnThisLoop = 0;
    int m_openStashAttempts = 0;  // 打开仓库尝试次数（防止无限循环点击）
    int m_openBenchAttempts = 0;  // 打开重铸台尝试次数（失败则重试点击）
    Clock::TimePoint m_stashOpenWaitStart;  // 仓库打开后等待加载的起始时间
    std::unordered_set<uintptr_t> m_bagSnapshot;
    std::unordered_set<uintptr_t> m_productAddresses;  // 合成产物的地址集合（POE2 需按地址区分产物）
    bool m_identifyCheckDoneThisLoop = false;  // 本轮循环是否已检查过未鉴定物品
    Clock::TimePoint m_identifyWaitStart;  // 鉴定等待开始时间
    bool m_identifyWaitInitialized = false;  // 鉴定等待是否已初始化
    bool m_identifyNotificationSeen = false;       // 是否检测到"已鉴定 N 个物品"系统通知
    Clock::TimePoint m_identifyCompleteTime;       // 系统通知检测到的时间（用于等待鉴定效果生效）
    std::string m_lastError;

    // —— v5 扫描预取队列：避免每次取物都全仓库扫描+ReadItemMods ——
    // 根因：原 TickWithdrawing 批处理循环每次都调用 NextTempleTabletInStash → CollectStashTablets
    //      → 遍历 76 个仓库物品 + 每个调用 ReadItemMods，4 次循环 = 304 次 SDK 调用，单帧阻塞 >500ms
    // 优化：进入 TickWithdrawing 第一次 Tick 时预取所有匹配原料（按从左至右排序）存入队列，
    //      后续批处理循环直接 pop 队列，零扫描；队列空或 800ms 过期才重扫
    struct PendingWithdrawItem {
        TabletReforgeGame::ScreenRect rect;  // 屏幕坐标（点击位置）
        int slotX = 0, slotY = 0;            // 仓库格坐标（用于排序）
        std::string path;                    // 物品Path（决定点击类型：左键/右键）
        std::string baseType;                // 物品BaseType
        int  rarity = 0;                     // 物品稀有度
    };
    std::vector<PendingWithdrawItem> m_pendingWithdrawQueue;
    Clock::TimePoint m_lastFullScanTime;          // 上次完整扫描时间
    int m_cachedStashMaterialCount = -1;          // 缓存：仓库匹配原料数（-1 表示待重扫）

    // —— 三槽同物 + 可堆叠合成追踪 ——
    std::string m_firstPlacedPath;          // 首个放入物品的Path（三槽同物锚点）
    bool m_isStackableSynthesis = false;    // 当前是否可堆叠合成（催化剂/精髓等）
    int  m_reforgePressCount = 0;           // 本轮已按合成按钮次数
    int  m_noProductReforgeCount = 0;       // 连续无产物合成次数

    // —— 状态转换 ——
    void Transition(State newState) {
        diag.Info(std::string(StateName(m_state)) + " -> " + StateName(newState));
        
        // 更新面板打开标志：状态机主动打开面板或正在切换面板的状态
        bool hasPanel = false;
        switch (newState) {
            case State::EnsureStashOpen:
            case State::ScanStash:
            case State::WaitForStashScan:
            case State::Withdrawing:
            case State::WaitForStashPanel:
            case State::Depositing:
                hasPanel = true;  // 仓库面板开着
                break;
            case State::WaitForBenchPanel:
            case State::PlacingTablets:
            case State::VerifySlotsFilled:
            case State::ClickCombine:
            case State::WaitForCombine:
            case State::VerifyCombineOutput:
            case State::ExtractOutput:
                hasPanel = true;  // 重铸台面板开着
                break;
            case State::CloseStashAndOpenBench:
            case State::CloseBenchAndOpenStash:
            case State::InteractWithNPC:
            case State::WaitForNPCDialog:
            case State::ClickIdentifyButton:
            case State::WaitForIdentification:
                hasPanel = true;  // 正在切换面板/NPC对话
                break;
            default:
                hasPanel = false;
                break;
        }
        TabletReforgeFlow::SetStateMachinePanelOpen(hasPanel);
        
        m_state = newState;
        m_stateTimer.Reset();
        m_lastAction = Clock::Now();
        
        // 重置鉴定等待开始时间
        if (newState == State::WaitForIdentification) {
            m_identifyWaitInitialized = false;
        }
        
        if (newState == State::EnsureStashOpen || newState == State::WaitForStashPanel) {
            m_openStashAttempts = 0;
        }
        if (newState == State::WaitForBenchPanel) {
            m_openBenchAttempts = 0;
        }
        if (newState == State::PlacingTablets) {
            m_placedThisRound = 0;
            m_firstPlacedPath.clear();  // 重置锚点，新一轮放置时重新确定
        }
        if (newState == State::Withdrawing) {
            m_withdrawnThisLoop = 0;
        }
    }

    // —— Ctrl 会话管理 ——
    void CtrlDown() {
        if (!m_ctrlHeld) {
            TabletReforgeInput::CtrlDown();
            m_ctrlHeld = true;
        }
    }

    void CtrlUp() {
        if (m_ctrlHeld) {
            TabletReforgeInput::CtrlUp();
            m_ctrlHeld = false;
        }
    }

    // —— 节流：距上次操作是否已过 ms ——
    bool Throttled(int ms) {
        return !Clock::Expired(m_lastAction, ms);
    }

    // —— 仓库操作点击类型（左键/右键）——
    // 可堆叠白色品质物品（催化剂/精髓/液态情感等）→ Ctrl+右键
    // 碑牌/珠宝（非堆叠，需鉴定）→ Ctrl+左键
    enum class StashClickType { LeftClick, RightClick };
    StashClickType ClickTypeForItem(int rarity, const std::string& path,
                                    const std::string& baseType) const {
        if (TabletReforgeGame::IsNormalRarityCraftable(rarity, path, baseType))
            return StashClickType::RightClick;
        return StashClickType::LeftClick;
    }

    // —— 清零可堆叠追踪状态 ——
    void ResetStackableTracking() {
        m_firstPlacedPath.clear();
        m_isStackableSynthesis = false;
        m_reforgePressCount = 0;
        m_noProductReforgeCount = 0;
    }

    // —— 可堆叠合成结束条件判定 ——
    // ALL: 1)按合成>=3次 2)上次无产物 3)背包无同Path材料 4)仅1槽有材料
    bool CheckStackableEndCondition(const PluginSDK::Context* ctx) {
        if (!m_isStackableSynthesis) return false;
        if (m_reforgePressCount < 3) return false;
        if (m_noProductReforgeCount < 1) return false;

        // 条件3: 背包无同Path材料
        int bagSamePath = 0;
        for (const auto& bi : TabletReforgeGame::CollectBagItems(ctx)) {
            if (bi.path == m_firstPlacedPath) ++bagSamePath;
        }
        if (bagSamePath > 0) return false;

        // 条件4: 仅1槽有材料
        auto slots = TabletReforgeGame::CollectBenchInputSlotItems(ctx);
        int slotsWithMaterial = 0;
        for (const auto& s : slots) {
            if (s.stackCount > 0) ++slotsWithMaterial;
        }
        if (slotsWithMaterial != 1) return false;

        return true;
    }

    // —— 从重铸台3槽取出剩余材料 ——
    void ExtractRemainingStackableMaterials(const PluginSDK::Context* ctx) {
        auto slots = TabletReforgeGame::CollectBenchInputSlotItems(ctx);
        CtrlDown();
        for (const auto& s : slots) {
            if (s.stackCount <= 0) continue;
            if (s.rect.w <= 0.f || s.rect.h <= 0.f) continue;
            float cx, cy;
            TabletReforgeGame::RectCenter(s.rect, cx, cy);
            TabletReforgeInput::MoveCursorScreen(static_cast<int>(cx), static_cast<int>(cy));
            if (settings.cursorSettleMs > 0)
                TabletReforgeInput::SleepMs(settings.cursorSettleMs);
            TabletReforgeInput::LeftClickAtCursor();  // 重铸台取出统一Ctrl+左键
            if (settings.postClickDelayMs > 0)
                TabletReforgeInput::SleepMs(settings.postClickDelayMs);
            diag.Info("[可堆叠] 取出剩余材料 (stack=" + std::to_string(s.stackCount) + ")");
        }
        CtrlUp();
        m_lastAction = Clock::Now();
    }

    // —— 中止 ——
    void Abort(const std::string& reason) {
        diag.Warn("中止: " + reason);
        m_lastError = reason;
        CtrlUp();
        ResetStackableTracking();
        m_testMode = false; // 清除测试模式
        // 注意：不移鼠标到角落，以免吓到用户。只释放按键并记录错误。
        Transition(State::Aborting);
    }

    void Error(const std::string& msg) {
        diag.Error(msg);
        CtrlUp();
        m_lastError = msg;
        Transition(State::ErrorWait);
    }

    // —— 状态超时诊断快照（P3）——
    // 在状态超时 Abort 前调用，把当前 ctx/门控/面板/计数器全量 dump 到 diag + DebugView，
    // 排查"为什么这个状态卡死了"时不再两眼一抹黑。
    // 安全：纯只读查询，全程 try/catch，自身异常不抛出（绝不二次触发 Abort）。
    void DumpTimeoutSnapshot(const PluginSDK::Context* ctx) {
        auto emit = [this](const std::string& line) {
            diag.Error(line);
            OutputDebugStringA(("[" + line + "]\n").c_str());
        };

        // 头行：状态名 + 停留时长 + 循环次数
        char header[256];
        ::sprintf_s(header, "[超时诊断] state=%s elapsed=%lldms loop=%d",
                    StateName(m_state), m_stateTimer.ElapsedMs(), m_loopCount);
        emit(header);

        if (!ctx) {
            emit("[超时诊断] ctx=nullptr，无法获取更多信息");
            return;
        }

        try {
            // —— 1. 7 项门控逐项状态 ——
            auto snap = ctx->Game.GetSnapshot();
            char gates[256];
            ::sprintf_s(gates,
                "[超时诊断][gates] InGame&Fg=%d TownHideout=%d NoEnemy=%d NoMenu=%d "
                "AreaUnchanged=%d NoRightClick=%d NoEsc=%d",
                TabletReforgeFlow::IsInGameAndForeground(ctx, settings) ? 1 : 0,
                TabletReforgeFlow::InTownOrHideout(snap, settings) ? 1 : 0,
                TabletReforgeFlow::NoEnemyNear(snap, settings) ? 1 : 0,
                TabletReforgeFlow::NoMenuVisible(ctx, settings) ? 1 : 0,
                TabletReforgeFlow::AreaUnchanged(m_areaBaseline, snap.AreaChangeCounter) ? 1 : 0,
                TabletReforgeFlow::NoRightClickCancel(settings) ? 1 : 0,
                (TabletReforgeFlow::NoEscCancel(settings) || m_escPressedByMachine) ? 1 : 0);
            emit(gates);

            // —— 2. 面板布尔矩阵 ——
            bool invOpen       = TabletReforgeGame::IsInventoryOpen(ctx);
            bool stashOpen     = TabletReforgeGame::IsStashOpen(ctx);
            bool benchInv      = TabletReforgeGame::IsBenchInventoryOpen(ctx);
            bool benchCtx      = TabletReforgeGame::IsBenchContextOpen(ctx);
            bool anyPanel      = TabletReforgeGame::AnyItemPanelOpen(ctx);
            bool trulyOpen     = TabletReforgeGame::IsBenchPanelTrulyOpen(ctx, calib);
            bool benchPanelOpen= TabletReforgeGame::IsBenchPanelOpen(ctx, calib);
            bool likelyOpen    = TabletReforgeGame::IsBenchPanelLikelyOpen(ctx, calib);
            char panels[256];
            ::sprintf_s(panels,
                "[超时诊断][panels] inv=%d stash=%d benchInv=%d benchCtx=%d anyPanel=%d "
                "truly=%d benchPanel=%d likely=%d",
                invOpen?1:0, stashOpen?1:0, benchInv?1:0, benchCtx?1:0, anyPanel?1:0,
                trulyOpen?1:0, benchPanelOpen?1:0, likelyOpen?1:0);
            emit(panels);

            // —— 3. ctx 快照：区域、玩家位置、附近实体统计 ——
            int monsterCount = 0;
            int enemyNearCount = 0;
            const float px = snap.Player.GridPositionX;
            const float py = snap.Player.GridPositionY;
            const float range = static_cast<float>(settings.enemyRange);
            for (const auto& e : snap.Entities) {
                if (!e.IsValid) continue;
                if (e.EntityType == PluginSDK::EntityType::Monster) {
                    ++monsterCount;
                    if (e.CurrentHP > 0 &&
                        e.EntityState != PluginSDK::EntityState::MonsterFriendly) {
                        const float dx = e.GridPositionX - px;
                        const float dy = e.GridPositionY - py;
                        if (std::sqrt(dx*dx + dy*dy) <= range) ++enemyNearCount;
                    }
                }
            }
            char snapLine[256];
            ::sprintf_s(snapLine,
                "[超时诊断][snap] areaBase=%llu areaNow=%llu isTown=%d isHideout=%d "
                "entities=%zu monsters=%d enemyNear=%d",
                (unsigned long long)m_areaBaseline,
                (unsigned long long)snap.AreaChangeCounter,
                snap.IsTown?1:0, snap.IsHideout?1:0,
                snap.Entities.size(), monsterCount, enemyNearCount);
            emit(snapLine);

            // —— 4. 状态机内部计数器 ——
            char counters[384];
            ::sprintf_s(counters,
                "[超时诊断][counters] openStash=%d openBench=%d "
                "placedRnd=%d withdrawnLoop=%d ctrlHeld=%d escByMachine=%d "
                "bagSnap=%zu prodAddr=%zu",
                m_openStashAttempts, m_openBenchAttempts,
                m_placedThisRound, m_withdrawnThisLoop,
                m_ctrlHeld?1:0, m_escPressedByMachine?1:0,
                m_bagSnapshot.size(), m_productAddresses.size());
            emit(counters);

            // —— 4b. NPC 鉴定流程状态变量（P3 增强：排查鉴定子流程卡死）——
            char ident[256];
            ::sprintf_s(ident,
                "[超时诊断][identify] checkDoneLoop=%d waitInit=%d notifSeen=%d "
                "npcEsc=%d testMode=%d",
                m_identifyCheckDoneThisLoop?1:0,
                m_identifyWaitInitialized?1:0,
                m_identifyNotificationSeen?1:0,
                m_npcEscPressed?1:0,
                m_testMode?1:0);
            emit(ident);

            // —— 5. 标定完整性（超时也可能是 calib 不全导致 UI 找不到）——
            char calibLine[128];
            ::sprintf_s(calibLine,
                "[超时诊断][calib] complete=%d useManualCoords=%d",
                calib.IsComplete()?1:0, calib.useManualCoords?1:0);
            emit(calibLine);
        } catch (...) {
            emit("[超时诊断] 快照采集异常，已忽略");
        }

        emit("[超时诊断] ===== 诊断结束 =====");
    }

    // —— 门控检查 ——
    bool CheckGates(const PluginSDK::Context* ctx) {
        if (!TabletReforgeFlow::IsInGameAndForeground(ctx, settings)) {
            Abort("安全门控: 不在游戏内或游戏窗口非前台");
            return false;
        }
        auto snap = ctx->Game.GetSnapshot();
        if (!InTownOrHideout(snap, settings)) {
            Abort("安全门控: 不在城镇/藏身处");
            return false;
        }
        if (!NoEnemyNear(snap, settings)) {
            Abort("安全门控: 附近有敌人");
            return false;
        }
        if (!NoMenuVisible(ctx, settings)) {
            Abort("安全门控: 有菜单/面板遮挡");
            return false;
        }
        if (!AreaUnchanged(m_areaBaseline, snap.AreaChangeCounter)) {
            Abort("安全门控: 切换了区域");
            return false;
        }
        if (!NoRightClickCancel(settings)) {
            Abort("用户取消: 右键按下");
            return false;
        }
        if (!NoEscCancel(settings) && !m_escPressedByMachine) {
            Abort("用户取消: Esc 按下");
            return false;
        }
        return true;
    }

    // —— 各状态 Tick 实现 ——

    void TickPreCheck(const PluginSDK::Context* ctx) {
        // 记录区域基线
        m_areaBaseline = ctx->Game.GetAreaChangeCounter();

        // 如果用户已经有面板开着（仓库/背包/重铸台），先按 Esc 关闭它
        // 这解决了"按F6时明明面板正常打开但报面板遮挡"的问题
        bool anyPanelOpen = TabletReforgeGame::AnyItemPanelOpen(ctx);
        bool invOpen = TabletReforgeGame::IsInventoryOpen(ctx);
        if (anyPanelOpen || invOpen) {
            diag.Info("检测到已有面板打开，先按 Esc 关闭（面板遮挡修复）");
            m_escPressedByMachine = true;
            TabletReforgeInput::PressKey(0x1B);
            if (settings.uiWaitMs > 0)
                TabletReforgeInput::SleepMs(settings.uiWaitMs * 2);
            m_lastAction = Clock::Now();
            return;
        }

        // 面板已关闭，重置 Esc 标志
        m_escPressedByMachine = false;

        // 检查重铸台是否可找到
        auto bench = TabletReforgeGame::FindBench(ctx, calib);
        if (!bench) {
            Error("找不到重铸台实体，请检查标定或调整相机");
            return;
        }
        if (!bench->onScreen) {
            Error("重铸台不在屏幕内，请调整相机");
            return;
        }

        diag.Info("预检查通过，重铸台在屏幕上");
        Transition(State::EnsureStashOpen);
    }

    void TickEnsureStashOpen(const PluginSDK::Context* ctx) {
        // 仓库面板已经打开了？（给面板加载一点时间）
        if (TabletReforgeGame::IsStashOpen(ctx)) {
            diag.Info("仓库面板已打开，延迟等待加载...");
            m_stashOpenWaitStart = Clock::Now();
            // 【方案 B v1.3】仓库打开事件触发 RandomBackoff（频控约束）
            m_stashBackoff.Arm();
            Transition(State::ScanStash);
            return;
        }

        // 没有标定仓库实体 → 等待用户手动打开
        if (calib.stashEntityPath.empty()) {
            // 每5秒提示一次
            if (Throttled(5000)) {
                diag.Warn("仓库未打开，等待用户手动打开（按F6或点击仓库）...");
                m_lastAction = Clock::Now();
            }
            return;
        }

        // 超过最大尝试次数 → 报错
        if (m_openStashAttempts >= 10) {
            Error("打开仓库失败：已尝试 " + std::to_string(m_openStashAttempts) + " 次仍未打开，请检查仓库实体标定是否正确");
            return;
        }

        // 节流：避免每帧都点
        if (Throttled(settings.clickDelayMs * 5)) return;

        // 尝试点击仓库实体打开
        if (TabletReforgeGame::OpenStashPanel(ctx, calib)) {
            diag.Info("点击仓库实体，等待面板打开（第 " + std::to_string(m_openStashAttempts + 1) + " 次尝试）");
            ++m_openStashAttempts;
            m_lastAction = Clock::Now();
        } else {
            Error("找不到仓库实体（不在屏幕内或标定有误），请手动打开仓库面板");
        }
    }

    void TickScanStash(const PluginSDK::Context* ctx) {
        // 等待仓库面板加载完成（给 500ms 延迟）
        constexpr int kStashLoadWaitMs = 500;
        auto elapsed = Clock::ElapsedMs(m_stashOpenWaitStart);
        if (elapsed < kStashLoadWaitMs) {
            if (elapsed % 100 < 20) {
                diag.Info("等待仓库面板加载... (" + std::to_string(elapsed) + "/" + std::to_string(kStashLoadWaitMs) + "ms)");
            }
            return;
        }

        diag.Info("触发仓库扫描");
        TabletReforgeGame::RefreshStash(ctx);
        Transition(State::WaitForStashScan);
    }

    void TickWaitForStashScan(const PluginSDK::Context* ctx) {
        if (Throttled(settings.scanSettleMs)) return;
        m_lastAction = Clock::Now();

        try {
            // 输出当前设置的详细信息
            {
                char settingsLog[512];
                sprintf_s(settingsLog, "[状态机] 扫描配置: itemType=%d, subCategoryId=%d, requireIdentified=%d, withdrawRequireIdentified=%d",
                    settings.itemType, settings.subCategoryId,
                    settings.requireIdentified ? 1 : 0,
                    settings.withdrawRequireIdentified ? 1 : 0);
                OutputDebugStringA(settingsLog);
            }

            OutputDebugStringA("[状态机] 开始检查仓库材料...\n");

            // 检查仓库是否有原料
            bool hasMaterial = TabletReforgeGame::HasTabletMaterialInStash(ctx, settings);
            OutputDebugStringA(hasMaterial ? "[状态机] 仓库中找到所需材料\n" : "[状态机] 仓库中未找到所需材料\n");

            if (!hasMaterial) {
                OutputDebugStringA("[状态机] 开始详细诊断扫描...\n");

                // 输出更多诊断信息
                auto allTablets = TabletReforgeGame::CollectStashTablets(ctx, settings);
                char diagLog[256];
                sprintf_s(diagLog, "[状态机] 仓库扫描诊断: 可见物品=%zu个, itemType=%d, subCategoryId=%d",
                    allTablets.size(), settings.itemType, settings.subCategoryId);
                OutputDebugStringA(diagLog);

                // 如果没有找到材料，尝试用不同的设置重新扫描
                TabletReforgeConfig::Settings debugSettings = settings;
                debugSettings.itemType = static_cast<int>(TabletReforgeConfig::ReforgeItemType::AllTablets);
                debugSettings.subCategoryId = 0;
                auto allItems = TabletReforgeGame::CollectStashTablets(ctx, debugSettings);

                char debugLog[512];
                int itemCount = static_cast<int>(allItems.size());
                sprintf_s(debugLog, "[状态机] 诊断: AllTablets scan found %d items", itemCount);
                OutputDebugStringA(debugLog);

                if (!allItems.empty()) {
                    OutputDebugStringA("[状态机] 仓库物品列表:\n");
                    int idx = 0;
                    for (const auto& st : allItems) {
                        if (idx++ >= 20) {
                            OutputDebugStringA("[状态机] ... (更多物品省略)\n");
                            break;
                        }
                        char itemLog[512];
                        sprintf_s(itemLog, "  [%d] path='%s', bt='%s', rarity=%d, identified=%d, isMaterial=%d",
                            idx, st.path.c_str(), st.baseType.c_str(),
                            st.rarity, st.identified ? 1 : 0, st.isMaterial ? 1 : 0);
                        OutputDebugStringA(itemLog);
                    }
                } else {
                    OutputDebugStringA("[状态机] 仓库中没有任何可见物品\n");
                }

                // 诊断建议
                OutputDebugStringA("[状态机] 诊断建议:\n");
                OutputDebugStringA("  1. 确认仓库面板已完全加载（等待1-2秒）\n");
                OutputDebugStringA("  2. 确认合成物品类型选择正确\n");
                OutputDebugStringA("  3. 确认仓库中有符合条件的物品\n");
                OutputDebugStringA("  4. 如需更多诊断信息，请查看OutputDebugString输出\n");

                int bagMat = TabletReforgeGame::CountMaterialTablets(ctx, settings);
                if (bagMat >= 3) {
                    diag.Info("仓库无原料，但背包已有 " + std::to_string(bagMat) + " 个匹配原料，直接进入合成流程");
                    m_placedThisRound = 0;
                    Transition(State::CloseStashAndOpenBench);
                    return;
                } else if (bagMat > 0) {
                    diag.Info("仓库无原料，背包有 " + std::to_string(bagMat) + " 个原料(不足3)，尝试继续合成");
                    m_placedThisRound = 0;
                    Transition(State::CloseStashAndOpenBench);
                    return;
                } else {
                    diag.Info("仓库无原料且背包无匹配原料，结束循环");
                }
                Transition(State::Idle);
                return;
            }

            int material = TabletReforgeGame::CountTabletMaterialInStash(ctx, settings);
            diag.Info("仓库原料数: " + std::to_string(material));
            m_withdrawnThisLoop = 0;
            Transition(State::Withdrawing);
        } catch (const std::exception& e) {
            char exceptLog[256];
            sprintf_s(exceptLog, "[状态机] TickWaitForStashScan 异常: %s", e.what());
            OutputDebugStringA(exceptLog);
            Abort("扫描仓库异常: " + std::string(e.what()));
        } catch (...) {
            OutputDebugStringA("[状态机] TickWaitForStashScan 未知异常\n");
            Abort("扫描仓库未知异常");
        }
    }

    void TickWithdrawing(const PluginSDK::Context* ctx) {
        // ========================================================================
        // === v6 重构：流畅一次性取物 + 相邻聚类排序 + 鉴定延后 ===
        //
        // 用户反馈问题（v5 的 BUG）：
        //   1. 每取少数几个就停顿（v5 队列大小=BATCH_MAX=4，取完4个就要重扫~150ms）
        //   2. 中途背包未满+仓库还有原料，却因背包有未鉴定触发鉴定流程
        //      （v5 把"队列空+距上次扫描<200ms"误判为"仓库空"，进入结束分支触发鉴定）
        //
        // v6 修复策略：
        //   ★ 预取队列扩容：首次扫描把【所有】匹配原料入队（不限4个），避免中途重扫
        //   ★ 相邻聚类排序：用贪心最近邻算法排序，让相邻物品连成最短路径
        //     （不硬性从左至右从上到下，相邻2-3个为一组自然形成取物路径）
        //   ★ 鉴定延后：鉴定检查只在【背包满 OR 仓库真的空】时触发，不在队列空时触发
        //   ★ 队列空==仓库真的空：只有扫描后队列仍为空才走结束分支
        // ========================================================================

        constexpr int   BATCH_MAX          = 6;     // 单帧最多取出物品数（v6: 4→6，更流畅）
        constexpr int   BATCH_MAX_MS       = 900;   // 单帧总耗时上限
        constexpr int   RESCAN_DEBOUNCE_MS = 500;   // 队列空后重扫防抖（避免立即重扫）

        // ========== Step 0: 首次扫描 + 预取所有原料入队 ==========
        auto now = Clock::Now();
        static auto s_lastBagScanTime = now;
        static int  s_last_emptySlots = 0;
        static int  s_last_bagTablets = 0;
        static int  s_last_bagAllTablets = 0;
        static int  s_last_bagAllItems = 0;

        // 背包扫描：每 800ms 重扫一次（用于更新空槽数和背包物品数）
        int msSinceBagScan = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            now - s_lastBagScanTime).count();
        if (msSinceBagScan >= 800 || s_lastBagScanTime == now /* 首次 */) {
            const auto bagScan = TabletReforgeGame::ScanBagCombined(ctx, settings);
            s_last_bagTablets    = bagScan.materialCount;
            s_last_bagAllTablets = bagScan.allTabletsCount;
            s_last_bagAllItems   = bagScan.totalItems;
            s_last_emptySlots    = bagScan.emptySlots1x1;
            s_lastBagScanTime    = now;
        }
        int bagTablets    = s_last_bagTablets;
        int emptySlots    = s_last_emptySlots;
        int bagAllTablets = s_last_bagAllTablets;
        int bagAllItems   = s_last_bagAllItems;

        // ========== Step 1: 背包满？走结束分支（含鉴定检查） ==========
        // v6 优化：用缓存判断，不再每帧调 BagCanFitOneMoreTablet
        if (emptySlots <= settings.reservedBagSlots) {
            char fullBuf[256];
            sprintf_s(fullBuf, "[背包已满] 材料=%d, 碑牌=%d, 物品=%d, 空槽=%d → 停止取出，进入合成",
                bagTablets, bagAllTablets, bagAllItems, emptySlots);
            diag.Info(fullBuf);
            OutputDebugStringA(fullBuf);
            CtrlUp();
            // 清空预取队列（背包满后下次进入会重新扫描）
            m_pendingWithdrawQueue.clear();
            m_cachedStashMaterialCount = -1;

            if (bagTablets >= 3) {
                // v6: 鉴定检查只在背包满 + 材料充足时触发（用户要求优先取物，鉴定延后）
                if (settings.autoIdentifyOutput) {
                    int bagUnidentified = TabletReforgeGame::CountUnidentifiedInBag(ctx, settings);
                    if (bagUnidentified > 0) {
                        char identBuf[256];
                        sprintf_s(identBuf, "[决策] 背包已满 + 检测到 %d 个未鉴定物品 → 先去多利亚尼鉴定",
                            bagUnidentified);
                        diag.Info(identBuf);
                        OutputDebugStringA(identBuf);
                        m_identifyCheckDoneThisLoop = true;
                        Transition(State::InteractWithNPC);
                        return;
                    }
                }
                char synthBuf[256];
                sprintf_s(synthBuf, "[决策] 背包已满，材料充足(%d >= 3) → 开始合成", bagTablets);
                diag.Info(synthBuf);
                OutputDebugStringA(synthBuf);
                Transition(State::CloseStashAndOpenBench);
            } else {
                char noSynthBuf[256];
                sprintf_s(noSynthBuf, "[决策] 背包已满，但材料不足(%d < 3) → 无法合成", bagTablets);
                diag.Info(noSynthBuf);
                OutputDebugStringA(noSynthBuf);
                CtrlUp();
                if (bagTablets > 0) {
                    Error("背包已满，但匹配原料只有 " + std::to_string(bagTablets) + " 个，不足 3 个无法合成");
                } else {
                    Transition(State::Idle);
                }
            }
            return;
        }

        // ========== Step 2: 预取队列空？触发一次全仓库扫描（把所有原料入队） ==========
        // v6 关键修复：队列空 ≠ 仓库空！必须重扫确认，不能误判
        CtrlDown();

        if (m_pendingWithdrawQueue.empty()) {
            int msSinceFullScan = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastFullScanTime).count();

            // 防抖：距上次扫描 <500ms 不重扫，等下一帧（避免队列空立即重扫的循环）
            // 但首次扫描（m_cachedStashMaterialCount == -1）必须立即执行
            if (msSinceFullScan < RESCAN_DEBOUNCE_MS && m_cachedStashMaterialCount >= 0) {
                // 队列空 + 距上次扫描<500ms + 缓存有效 → 本帧不取物，等下一帧
                // 注意：这里不再误判为"仓库空"，避免触发鉴定流程
                return;
            }

            // 触发预取扫描：一次调用 CollectStashTablets，把【所有】匹配原料入队
            char scanBuf[256];
            sprintf_s(scanBuf, "[预取扫描] 触发全仓库扫描（队列空，距上次扫描=%dms）", msSinceFullScan);
            diag.Info(scanBuf);
            OutputDebugStringA(scanBuf);

            std::vector<TabletReforgeGame::StashTablet> stashTablets;
            if (settings.autoIdentifyOutput) {
                TabletReforgeConfig::Settings tempSettings = settings;
                tempSettings.requireIdentifiedForMaterial = false;
                stashTablets = TabletReforgeGame::CollectStashTablets(ctx, tempSettings);
            } else {
                stashTablets = TabletReforgeGame::CollectStashTablets(ctx, settings);
            }
            m_lastFullScanTime = now;

            // 把所有 isMaterial 的物品按"相邻聚类"排序后入队
            m_pendingWithdrawQueue.clear();
            std::vector<TabletReforgeGame::StashTablet> materials;
            for (const auto& st : stashTablets) {
                if (st.isMaterial) materials.push_back(st);
            }
            m_cachedStashMaterialCount = (int)materials.size();

            // ★ v6 相邻聚类排序：贪心最近邻算法
            // 不硬性按 slotY/slotX 排序，而是从第一个物品开始，每次找距离最近的下一个
            // 这样相邻2-3个物品自然连成一组，鼠标移动距离最小
            if (!materials.empty()) {
                // 先按 slotY, slotX 排序（作为初始顺序）
                std::sort(materials.begin(), materials.end(), [](const auto& a, const auto& b) {
                    if (a.slotY != b.slotY) return a.slotY < b.slotY;
                    return a.slotX < b.slotX;
                });

                // 贪心最近邻：从第一个物品开始，每次找最近的未访问物品
                std::vector<char> visited(materials.size(), 0);
                std::vector<TabletReforgeGame::StashTablet> ordered;
                ordered.reserve(materials.size());
                ordered.push_back(materials[0]);
                visited[0] = 1;

                // 用屏幕坐标计算距离（更准确反映鼠标移动距离）
                float curX = 0, curY = 0;
                TabletReforgeGame::RectCenter(materials[0].rect, curX, curY);

                for (size_t i = 1; i < materials.size(); ++i) {
                    int bestIdx = -1;
                    double bestDist = 1e18;
                    for (size_t j = 0; j < materials.size(); ++j) {
                        if (visited[j]) continue;
                        float tx = 0, ty = 0;
                        TabletReforgeGame::RectCenter(materials[j].rect, tx, ty);
                        double dx = tx - curX, dy = ty - curY;
                        double d = dx * dx + dy * dy;  // 平方距离（省去 sqrt）
                        if (d < bestDist) {
                            bestDist = d;
                            bestIdx = (int)j;
                        }
                    }
                    if (bestIdx < 0) break;
                    visited[bestIdx] = 1;
                    ordered.push_back(materials[bestIdx]);
                    TabletReforgeGame::RectCenter(materials[bestIdx].rect, curX, curY);
                }

                // 入队（按取物顺序，front=先取）
                for (const auto& st : ordered) {
                    m_pendingWithdrawQueue.push_back({ st.rect, st.slotX, st.slotY,
                                                       st.path, st.baseType, st.rarity });
                }
            }

            char prefBuf[256];
            sprintf_s(prefBuf, "[预取扫描] 完成：仓库匹配原料 %d 个，已按相邻聚类排序入队",
                (int)m_pendingWithdrawQueue.size());
            diag.Info(prefBuf);
            OutputDebugStringA(prefBuf);

            // 仓库真的空了（扫描后队列仍为空）→ 走结束分支（含鉴定检查）
            if (m_pendingWithdrawQueue.empty()) {
                CtrlUp();
                if (bagTablets >= 3) {
                    // v6: 鉴定检查只在仓库真的空 + 材料充足时触发
                    if (settings.autoIdentifyOutput) {
                        int bagUnidentified = TabletReforgeGame::CountUnidentifiedInBag(ctx, settings);
                        if (bagUnidentified > 0) {
                            char identBuf[256];
                            sprintf_s(identBuf, "[决策] 仓库已空 + 检测到 %d 个未鉴定物品 → 先去多利亚尼鉴定",
                                bagUnidentified);
                            diag.Info(identBuf);
                            OutputDebugStringA(identBuf);
                            m_identifyCheckDoneThisLoop = true;
                            Transition(State::InteractWithNPC);
                            return;
                        }
                    }
                    char synthBuf[256];
                    sprintf_s(synthBuf, "[决策] 仓库已空，材料充足(%d >= 3) → 开始合成", bagTablets);
                    diag.Info(synthBuf);
                    OutputDebugStringA(synthBuf);
                    Transition(State::CloseStashAndOpenBench);
                } else if (bagTablets > 0) {
                    Error("仓库已空，背包有 " + std::to_string(bagTablets) + " 个匹配原料，不足 3 个无法合成");
                } else {
                    Transition(State::Idle);
                }
                return;
            }
        }

        // ========== Step 3: 批处理循环取出多个物品（零扫描，直接 pop 队列） ==========
        auto batchStart = Clock::Now();
        for (int itemI = 0; itemI < BATCH_MAX; ++itemI) {
            // 队列空了？本帧不再取，留给下一帧（下一帧 Step 2 会重扫确认）
            if (m_pendingWithdrawQueue.empty()) break;

            // —— 3a: 从队列头部取下一个物品（按相邻聚类顺序）——
            auto nextItem = m_pendingWithdrawQueue.front();
            m_pendingWithdrawQueue.erase(m_pendingWithdrawQueue.begin());
            auto nextTablet = nextItem.rect;

            // —— 3b: 节流：等「距离上次点击 >= clickDelayMs」再发下一次 ——
            if (settings.clickDelayMs > 0 && m_lastAction.time_since_epoch().count() > 0) {
                int sinceLast = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::Now() - m_lastAction).count();
                int needSleep = settings.clickDelayMs - sinceLast;
                if (needSleep > 0) TabletReforgeInput::SleepMs(needSleep);
            }

            // —— 3c: 计算目标坐标 + 执行移动+点击 ——
            float cx, cy;
            TabletReforgeGame::RectCenter(nextTablet, cx, cy);
            TabletReforgeInput::HumanLikeMoveTo(static_cast<int>(cx), static_cast<int>(cy),
                settings.enableHumanMouse, settings.mouseGravity, settings.mouseWind,
                settings.mouseMaxStep, settings.mouseStepWaitMs);
            if (settings.cursorSettleMs > 0)
                TabletReforgeInput::SleepMs(settings.cursorSettleMs);
            // 根据物品类型选择点击类型：可堆叠→右键，碑牌/珠宝→左键
            StashClickType clickType = ClickTypeForItem(nextItem.rarity, nextItem.path, nextItem.baseType);
            if (clickType == StashClickType::RightClick) {
                TabletReforgeInput::RightClickAtCursor();
            } else {
                TabletReforgeInput::LeftClickAtCursor();
            }
            if (settings.postClickDelayMs > 0)
                TabletReforgeInput::SleepMs(settings.postClickDelayMs);

            ++m_withdrawnThisLoop;
            m_lastAction = Clock::Now();

            // 缓存近似更新（取出一个物品：空位-1，背包+1，仓库-1）
            if (s_last_emptySlots > 0) --s_last_emptySlots;
            ++s_last_bagAllItems;
            ++s_last_bagAllTablets;
            ++s_last_bagTablets;
            if (m_cachedStashMaterialCount > 0) --m_cachedStashMaterialCount;

            char actBuf[512];
            sprintf_s(actBuf, "[批处理 %d/%d] Ctrl+左键 取物品（累计 %d, 背包匹配=%d/3, 仓库剩余=%d, 空槽=%d, 队列=%d）",
                itemI + 1, BATCH_MAX, (int)m_withdrawnThisLoop,
                s_last_bagTablets, m_cachedStashMaterialCount, s_last_emptySlots,
                (int)m_pendingWithdrawQueue.size());
            diag.Info(actBuf);
            OutputDebugStringA(actBuf);

            // —— 3d: 背包满？立即结束批处理（用缓存判断）——
            if (s_last_emptySlots <= settings.reservedBagSlots) break;

            // —— 3e: 单帧累计耗时上限？归还控制权 ——
            int batchMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::Now() - batchStart).count();
            if (batchMs >= BATCH_MAX_MS) break;
        }

        // ========== Step 4: 批处理结束，留待下一帧继续 ==========
        // （Ctrl 保持按下，不释放，下次 Tick 继续批处理）
    }

    void TickCloseStashAndOpenBench(const PluginSDK::Context* ctx) {
        if (Throttled(settings.clickDelayMs)) return;

        // —— 按用户思路：任何方向切换面板都先按 Esc 关闭当前面板！——
        // 用户经验：仓库或重铸台面板都是弹窗，点击时都会遮挡住画面中央的实体。
        // 为避免点击穿透失败，统一策略是：
        //   1. 若当前有任何物品面板开着 → 先 Esc 关闭
        //   2. 等一帧确认面板关了 → 再点击目标实体
        // 这种策略简单可靠，没有方向差异。
        //
        // 上一版"直接点击让游戏自动切换面板"在仓库→重铸台方向有时会失败：
        // 仓库页大面板遮挡时游戏虽然会关仓库，但偶尔因 UI 层次导致点击被吞掉。
        // 先 Esc 再点实体，虽然多走一帧，但成功率 100%。

        CtrlUp();

        // —— 第 1 步：有物品面板开着 → 先 Esc 关掉它 ——
        bool anyPanelOpen = TabletReforgeGame::AnyItemPanelOpen(ctx);
        if (anyPanelOpen) {
            // 防重复：若 ESC 物理键还按着，不重复按（避免触发系统菜单）
            if (TabletReforgeInput::IsEscPhysicallyDown()) {
                diag.Info("ESC 物理键尚未释放，等待...");
                m_lastAction = Clock::Now();
                return;
            }
            m_escPressedByMachine = true;
            TabletReforgeInput::PressKey(0x1B);
            diag.Info("按 Esc 关闭仓库/背包面板（避免遮挡重铸台实体点击）");
            // 等待 ESC 完全释放（至少 300ms），避免连续按键触发系统菜单
            const int escWaitMs = (settings.uiWaitMs * 2 > 300) ? settings.uiWaitMs * 2 : 300;
            TabletReforgeInput::SleepMs(escWaitMs);
            m_lastAction = Clock::Now();
            // 只执行按 Esc，不立即点击——下一帧确认面板关闭了再点
            return;
        }

        // —— 第 2 步：面板已经关了 → 点击重铸台 ——
        // 确认 ESC 物理键已释放，避免点击时 ESC 还按着导致打开系统菜单
        if (TabletReforgeInput::IsEscPhysicallyDown()) {
            diag.Info("ESC 物理键尚未释放，等待后点击...");
            m_lastAction = Clock::Now();
            return;
        }

        auto bench = TabletReforgeGame::FindBench(ctx, calib);
        if (!bench || !bench->onScreen) {
            Error("找不到重铸台或不在屏幕内，请调整相机后重试");
            return;
        }

        const int bx = static_cast<int>(bench->screenX + 0.5f);
        const int by = static_cast<int>(bench->screenY + 0.5f);
        TabletReforgeInput::MoveCursorScreen(bx, by);
        if (settings.cursorSettleMs > 0)
            TabletReforgeInput::SleepMs(settings.cursorSettleMs);
        TabletReforgeInput::LeftClickAtCursor();
        m_lastAction = Clock::Now();
        diag.Info("点击重铸台实体（仓库面板已关闭）");
        Transition(State::WaitForBenchPanel);
    }

    void TickWaitForBenchPanel(const PluginSDK::Context* ctx) {
        if (Throttled(settings.uiWaitMs)) return;
        m_lastAction = Clock::Now();

        // 重置 ESC 标志前确认物理键已释放，避免门控 NoEscCancel 误判
        if (m_escPressedByMachine && !TabletReforgeInput::IsEscPhysicallyDown()) {
            m_escPressedByMachine = false;
        }

        bool invOpen   = TabletReforgeGame::IsInventoryOpen(ctx);
        bool stashOpen = TabletReforgeGame::IsStashOpen(ctx);
        bool benchInv  = TabletReforgeGame::IsBenchInventoryOpen(ctx);

        static int waitCounter = 0;
        ++waitCounter;
        if (waitCounter % 5 == 0) {
            diag.Info("WaitForBenchPanel [每5tick]: 背包:" + std::string(invOpen ? "开" : "关")
                + ", 仓库:" + std::string(stashOpen ? "开" : "关")
                + ", 合成Inv:" + std::string(benchInv ? "开" : "关")
                + ", 尝试:" + std::to_string(m_openBenchAttempts));
        }

        // 如果重铸台开着但背包没开 → 按 I 打开背包
        if (!invOpen && !stashOpen && benchInv) {
            diag.Info("检测到合成面板但背包未开，按 I 键打开背包...");
            TabletReforgeInput::PressKey('I');
            if (settings.uiWaitMs > 0)
                TabletReforgeInput::SleepMs(settings.uiWaitMs);
            m_lastAction = Clock::Now();
            return;
        }

        // 如果背包和合成面板都没开 → 按 I 确保打开
        if (!invOpen && !benchInv && m_openBenchAttempts > 2
            && m_openBenchAttempts % 3 == 0) {
            diag.Info("背包和合成面板都未开，按 I 键确保背包打开...");
            TabletReforgeInput::PressKey('I');
            if (settings.uiWaitMs > 0)
                TabletReforgeInput::SleepMs(settings.uiWaitMs);
            m_lastAction = Clock::Now();
        }

        bool trulyOpen = TabletReforgeGame::IsBenchPanelTrulyOpen(ctx, calib);
        bool benchPanelOpen = TabletReforgeGame::IsBenchPanelOpen(ctx, calib);
        bool likelyOpen = TabletReforgeGame::IsBenchPanelLikelyOpen(ctx, calib);
        bool benchCtx = TabletReforgeGame::IsBenchContextOpen(ctx);

        if (waitCounter % 5 == 0) {
            diag.Info("  检测: TrulyOpen=" + std::string(trulyOpen ? "是" : "否")
                + ", PanelOpen(StringId)=" + std::string(benchPanelOpen ? "是" : "否")
                + ", LikelyOpen=" + std::string(likelyOpen ? "是" : "否")
                + ", ContextOpen=" + std::string(benchCtx ? "是" : "否")
                + ", 背包开=" + std::string(invOpen ? "是" : "否"));
        }

        // 严格的转换条件：必须至少满足以下之一
        // 1. trulyOpen（最严格：仓库关+检测到合成上下文+按钮有效）
        // 2. benchPanelOpen + 背包开（StringId检测到面板且背包可见）
        // 3. likelyOpen + 背包开（启发式检测到且背包可见）
        // 4. benchCtx + 背包开（合成上下文可见且背包可见）
        // 关键改进：必须确保背包是开着的，物品才有屏幕坐标可点击！
        bool canTransition = trulyOpen
            || (benchPanelOpen && invOpen)
            || (likelyOpen && invOpen)
            || (benchCtx && invOpen);

        // 如果合成上下文已打开但背包没开 → 多等一会让 I 键生效
        if ((benchCtx || benchInv) && !invOpen) {
            // 背包正在加载中，不转换
            if (waitCounter % 5 == 0) {
                diag.Info("合成面板已打开但背包尚未加载，等待背包打开...");
            }
            // 不增加 openBenchAttempts，不重试点击
            return;
        }

        if (canTransition) {
            diag.Info("重铸台面板已打开（检测通过），开始放入碑牌 [背包开=" + std::string(invOpen ? "是" : "否") + "]");
            m_placedThisRound = 0;
            m_bagSnapshot = TabletReforgeGame::SnapshotBag(ctx);
            // 【方案 B v1.3】重铸台打开事件触发 RandomBackoff（频控约束）
            m_benchBackoff.Arm();
            m_bagBackoff.Arm();
            waitCounter = 0;
            Transition(State::PlacingTablets);
            return;
        }

        ++m_openBenchAttempts;
        const int kRetryInterval = 3;
        const int kMaxRetries = 5;
        if (m_openBenchAttempts % kRetryInterval == 0
            && m_openBenchAttempts / kRetryInterval < kMaxRetries) {

            auto bench = TabletReforgeGame::FindBench(ctx, calib);
            if (bench && bench->onScreen) {
                diag.Warn("重铸台面板未检测到，重试点击重铸台（第 "
                    + std::to_string(m_openBenchAttempts / kRetryInterval) + "/"
                    + std::to_string(kMaxRetries) + " 次）");
                const int bx = static_cast<int>(bench->screenX + 0.5f);
                const int by = static_cast<int>(bench->screenY + 0.5f);
                TabletReforgeInput::LeftClickScreen(bx, by);
                m_lastAction = Clock::Now();
            } else {
                diag.Warn("重铸台面板未检测到，且重铸台实体已不在屏幕内");
            }
        }
    }

    void TickPlacingTablets(const PluginSDK::Context* ctx) {
        // 每 10 tick 输出一次诊断信息
        static int placeDiagCounter = 0;
        if (++placeDiagCounter % 10 == 0) {
            auto stats = TabletReforgeGame::GetBagItemStats(ctx, settings);
            int materialCount = TabletReforgeGame::CountMaterialTablets(ctx, settings);
            diag.Info("PlacingTablets [诊断]: 总=" + std::to_string(stats.totalItems)
                + " 可点击=" + std::to_string(stats.itemsWithCoords)
                + " 匹配=" + std::to_string(stats.itemsMatchingType)
                + " 有Path=" + std::to_string(stats.itemsWithPath)
                + " 有BT=" + std::to_string(stats.itemsWithBT)
                + " 原料匹配=" + std::to_string(materialCount)
                + " 背包面板=" + std::to_string(stats.bagLikePanels)
                + "/" + std::to_string(stats.totalPanels));
        }

        if (m_placedThisRound >= 3) {
            CtrlUp();
            diag.Info("已放入 3 个物品 (path=" + m_firstPlacedPath + ")");
            Transition(State::VerifySlotsFilled);
            return;
        }

        // 首次放入 → 确定锚点Path + 可堆叠性
        if (m_placedThisRound == 0 && m_firstPlacedPath.empty()) {
            std::string firstPath;
            int firstRarity = 0;
            std::string firstBt;
            for (const auto& bi : TabletReforgeGame::CollectBagItems(ctx)) {
                int r = bi.rarity; bool id = bi.identified;
                if (bi.address != 0) {
                    auto m = ctx->Inventory.ReadItemMods(bi.address);
                    if (m.Valid) { if (m.Rarity > 0) r = m.Rarity; id = m.IsIdentified; }
                }
                if (TabletReforgeGame::MatchesDesiredReforgeTypeEx(bi.path, bi.baseType, r, id, settings)) {
                    firstPath = bi.path;
                    firstRarity = r;
                    firstBt = bi.baseType;
                    break;
                }
            }
            m_firstPlacedPath = firstPath;
            m_isStackableSynthesis = TabletReforgeGame::IsStackableCraftable(firstRarity, firstPath, firstBt);
            m_reforgePressCount = 0;
            m_noProductReforgeCount = 0;
            char anchorBuf[512];
            sprintf_s(anchorBuf, "[放置锚点] Path=%s 可堆叠=%s",
                firstPath.c_str(), m_isStackableSynthesis ? "是" : "否");
            diag.Info(anchorBuf);
        }

        // 先统计匹配的原料物品数（包括无坐标的）
        int materialCount = TabletReforgeGame::CountMaterialTablets(ctx, settings);

        // 背包没有匹配的原料 → 直接结束
        if (materialCount <= 0) {
            CtrlUp();
            auto stats = TabletReforgeGame::GetBagItemStats(ctx, settings);
            std::string warnMsg = "背包已无匹配的原料物品";
            warnMsg += " [总物品=" + std::to_string(stats.totalItems);
            warnMsg += "/可点击=" + std::to_string(stats.itemsWithCoords);
            warnMsg += "/匹配类型=" + std::to_string(stats.itemsMatchingType);
            warnMsg += "/有Path=" + std::to_string(stats.itemsWithPath);
            warnMsg += "/有BT=" + std::to_string(stats.itemsWithBT) + "]";
            diag.Warn(warnMsg);
            Transition(State::CloseBenchAndOpenStash);
            return;
        }

        // 背包有匹配的原料，尝试找可点击的
        // 三槽同物规则：m_placedThisRound > 0 时按 m_firstPlacedPath 过滤
        auto nextTablet = (m_placedThisRound > 0 && !m_firstPlacedPath.empty())
            ? TabletReforgeGame::NextMaterialTabletByPath(ctx, settings, m_firstPlacedPath)
            : TabletReforgeGame::NextMaterialTablet(ctx, settings);
        if (!nextTablet) {
            // 关键改进：物品存在但没有屏幕坐标 → 可能是背包还没完全加载
            // 等待更多 tick 让坐标加载出来，而不是直接放弃
            auto stats = TabletReforgeGame::GetBagItemStats(ctx, settings);
            if (stats.itemsWithCoords == 0 && materialCount > 0) {
                // 物品存在但都没有坐标，等待背包完全渲染
                if (placeDiagCounter % 5 == 0) {
                    diag.Info("背包有 " + std::to_string(materialCount)
                        + " 个匹配原料但暂无坐标，等待背包渲染..."
                        + " [总=" + std::to_string(stats.totalItems)
                        + "/可点击=" + std::to_string(stats.itemsWithCoords)
                        + "/面板=" + std::to_string(stats.bagLikePanels) + "]");
                }
                m_lastAction = Clock::Now();
                return;  // 不转换，等待下一个 tick
            }
            
            CtrlUp();
            diag.Warn("背包原料无坐标且无法恢复（匹配=" + std::to_string(materialCount)
                + ", 可点击=" + std::to_string(stats.itemsWithCoords) + ")"
                + " [总物品=" + std::to_string(stats.totalItems)
                + "/匹配类型=" + std::to_string(stats.itemsMatchingType) + "]");
            Transition(State::CloseBenchAndOpenStash);
            return;
        }

        if (Throttled(settings.clickDelayMs)) return;

        // Ctrl+左键放碑牌到合成槽
        CtrlDown();
        float cx, cy;
        TabletReforgeGame::RectCenter(*nextTablet, cx, cy);
        TabletReforgeInput::MoveCursorScreen(static_cast<int>(cx), static_cast<int>(cy));
        if (settings.cursorSettleMs > 0)
            TabletReforgeInput::SleepMs(settings.cursorSettleMs);
        TabletReforgeInput::LeftClickAtCursor();
        if (settings.postClickDelayMs > 0)
            TabletReforgeInput::SleepMs(settings.postClickDelayMs);

        ++m_placedThisRound;
        m_lastAction = Clock::Now();
        diag.Info("放入物品 " + std::to_string(m_placedThisRound) + "/3 (path=" + m_firstPlacedPath + ")");
    }

    void TickVerifySlotsFilled(const PluginSDK::Context* ctx) {
        if (Throttled(settings.uiWaitMs)) return;
        m_lastAction = Clock::Now();

        auto currentSnap = TabletReforgeGame::SnapshotBag(ctx);
        auto removed = TabletReforgeGame::DiffRemoved(m_bagSnapshot, currentSnap);
        if (static_cast<int>(removed.size()) >= 3) {
            diag.Info("验证通过：已放入 " + std::to_string(removed.size()) + " 个碑牌");
        } else if (removed.size() > 0) {
            diag.Info("放入验证：仅减少 " + std::to_string(removed.size()) + " 个（不足3个但仍继续）");
        } else {
            // 即使验证失败，仍然继续流程（物品可能已移动但地址未变）
            diag.Warn("放入验证失败，物品地址未变化，但仍继续流程");
        }
        m_bagSnapshot = currentSnap;
        Transition(State::ClickCombine);
    }

    void TickClickCombine(const PluginSDK::Context* ctx) {
        if (Throttled(settings.clickDelayMs)) return;

        auto btn = TabletReforgeGame::ResolveCombineButton(ctx, calib);
        if (!btn.valid) {
            Error("无法解析合成按钮坐标");
            return;
        }
        
        char logBuf[256];
        sprintf_s(logBuf, "[合成] 点击合成按钮: (%d, %d)", btn.x, btn.y);
        diag.Info(logBuf);
        OutputDebugStringA(logBuf);

        TabletReforgeInput::LeftClickScreen(btn.x, btn.y);
        m_lastAction = Clock::Now();
        if (m_isStackableSynthesis) ++m_reforgePressCount;
        Transition(State::WaitForCombine);
    }

    void TickWaitForCombine(const PluginSDK::Context* ctx) {
        if (Throttled(settings.combineWaitMs)) return;
        m_lastAction = Clock::Now();
        diag.Info("[合成] 等待合成完成...");
        OutputDebugStringA("[合成] 合成等待完成\n");
        Transition(State::VerifyCombineOutput);
    }

    void TickVerifyCombineOutput(const PluginSDK::Context* ctx) {
        if (Throttled(settings.uiWaitMs)) return;
        m_lastAction = Clock::Now();

        size_t prevProductCount = m_productAddresses.size();
        auto currentSnap = TabletReforgeGame::SnapshotBag(ctx);
        auto added = TabletReforgeGame::DiffAdded(m_bagSnapshot, currentSnap);

        char logBuf[256];
        sprintf_s(logBuf, "[合成验证] 产物检测: 新增=%zu, 产物总数=%zu",
            added.size(), m_productAddresses.size());
        OutputDebugStringA(logBuf);

        if (!added.empty()) {
            diag.Info("验证通过：产物已生成（新增 " + std::to_string(added.size()) + " 个物品）");
            for (uintptr_t addr : added) {
                m_productAddresses.insert(addr);
            }
            sprintf_s(logBuf, "[合成验证] 产物已生成: 新增=%zu, 总产物=%zu", added.size(), m_productAddresses.size());
            OutputDebugStringA(logBuf);
        } else {
            diag.Warn("产物验证失败（未检测到新物品），但仍继续流程");
            OutputDebugStringA("[合成验证] 产物验证失败，继续流程\n");
        }

        // 可堆叠合成追踪：连续无产物计数
        if (m_isStackableSynthesis) {
            if (m_productAddresses.size() > prevProductCount) {
                m_noProductReforgeCount = 0;  // 有新产物，重置
            } else {
                ++m_noProductReforgeCount;   // 无新产物，累加
            }
        }

        m_bagSnapshot = currentSnap;
        Transition(State::ExtractOutput);
    }

    void TickExtractOutput(const PluginSDK::Context* ctx) {
        if (Throttled(settings.clickDelayMs)) return;

        // Ctrl+左键点击产物槽取出产物
        auto slot = TabletReforgeGame::ResolveOutputSlot(ctx, calib);
        if (!slot.valid) {
            Error("无法解析产物槽坐标");
            return;
        }

        TabletReforgeInput::CtrlDown();
        TabletReforgeInput::MoveCursorScreen(slot.x, slot.y);
        if (settings.cursorSettleMs > 0)
            TabletReforgeInput::SleepMs(settings.cursorSettleMs);
        TabletReforgeInput::LeftClickAtCursor();
        if (settings.postClickDelayMs > 0)
            TabletReforgeInput::SleepMs(settings.postClickDelayMs);
        TabletReforgeInput::CtrlUp();
        m_lastAction = Clock::Now();
        diag.Info("Ctrl+左键取出产物");

        // === 可堆叠合成流程分叉 ===
        if (m_isStackableSynthesis) {
            // 安全阀：所有槽位已空 → 直接去存回
            auto benchSlots = TabletReforgeGame::CollectBenchInputSlotItems(ctx);
            int slotsWithMaterial = 0;
            for (const auto& s : benchSlots) {
                if (s.stackCount > 0) ++slotsWithMaterial;
            }
            if (slotsWithMaterial == 0) {
                diag.Info("[可堆叠] 所有槽位已空，去存回");
                Transition(State::CloseBenchAndOpenStash);
                return;
            }
            // 检查结束条件
            if (CheckStackableEndCondition(ctx)) {
                diag.Info("[可堆叠] 终止条件满足，取出剩余材料并去存回");
                ExtractRemainingStackableMaterials(ctx);
                Transition(State::CloseBenchAndOpenStash);
                return;
            }
            // 继续合成（跳过PlacingTablets，槽位仍有材料）
            diag.Info("[可堆叠] 继续合成 (已按=" + std::to_string(m_reforgePressCount)
                + ", 无产物=" + std::to_string(m_noProductReforgeCount) + ") → ClickCombine");
            m_bagSnapshot = TabletReforgeGame::SnapshotBag(ctx);
            Transition(State::ClickCombine);
            return;
        }

        // === 普通合成（碑牌/珠宝）：原有逻辑 ===
        // 流程图要求：先检查材料数，材料 >= 3 时继续合成（步骤7）===
        // 只有材料 < 3 时才检查未鉴定物品（步骤8-9），避免合成后立即中断去鉴定
        int materialCount = TabletReforgeGame::CountMaterialTablets(ctx, settings);
        if (materialCount >= 3) {
            diag.Info("背包还有 " + std::to_string(materialCount) + " 个匹配原料，继续合成");
            m_placedThisRound = 0;
            m_bagSnapshot = TabletReforgeGame::SnapshotBag(ctx);
            Transition(State::PlacingTablets);
            return;
        }

        // === 材料 < 3 → 检查未鉴定物品（流程图步骤8-9）===
        if (settings.autoIdentifyOutput) {
            auto bagUnidentified = TabletReforgeGame::CountUnidentifiedInBag(ctx, settings);
            if (bagUnidentified > 0) {
                diag.Info("材料不足(" + std::to_string(materialCount) + "<3)且检测到 "
                    + std::to_string(bagUnidentified) + " 个未鉴定物品 → 前往NPC鉴定");
                m_identifyCheckDoneThisLoop = true;
                Transition(State::InteractWithNPC);
                return;
            }
        }

        // === 材料 < 3 且无未鉴定 → 尝试切换到下一个合成目标 ===
        if (settings.AdvanceToNextTarget()) {
            diag.Info("材料不足(" + std::to_string(materialCount) + "<3)，切换到下一个合成目标: "
                + (settings.GetActiveTarget() ? settings.GetActiveTarget()->name : "unknown"));
            // 重新检查新材料下的背包数量
            int newMaterial = TabletReforgeGame::CountMaterialTablets(ctx, settings);
            diag.Info("新目标下背包原料数: " + std::to_string(newMaterial));
            if (newMaterial >= 3) {
                m_placedThisRound = 0;
                Transition(State::PlacingTablets);
                return;
            }
            // 新目标也不足，继续走存回流程
        }

        diag.Info("匹配原料不足 3 个，去存回产物");
        Transition(State::CloseBenchAndOpenStash);
    }

    void TickCloseBenchAndOpenStash(const PluginSDK::Context* ctx) {
        if (Throttled(settings.clickDelayMs)) return;

        // —— 反向流程必须先按 Esc 关重铸台大面板！——
        // 原因：重铸台面板（REFORGING BENCH）是一个巨大的弹窗，
        // 会完全挡住后面地图上的仓库实体。如果不先关闭它，
        // 鼠标"点击仓库实体"的操作只会点到重铸台 UI 的空白区域，完全无效！
        // 这和仓库→重铸台方向不同：仓库面板在一侧，不会挡住画面中央的重铸台实体。
        //
        // 先关 UI 再点击实体，就不会有时序冲突（因为 Esc 之后我们还要 Sleep 一段，
        // 确保面板关闭动画完成，再把鼠标移到仓库位置点下去）。

        // 确保 Ctrl 已释放（合成操作期间可能还按着）
        CtrlUp();

        // —— 第 1 步：如果重铸台面板还开着 → 按 Esc 关闭它 ——
        bool benchStillOpen = TabletReforgeGame::IsBenchContextOpen(ctx);
        if (benchStillOpen) {
            // 防重复：若 ESC 物理键还按着，不重复按（避免触发系统菜单）
            if (TabletReforgeInput::IsEscPhysicallyDown()) {
                diag.Info("ESC 物理键尚未释放，等待...");
                m_lastAction = Clock::Now();
                return;
            }
            m_escPressedByMachine = true;
            TabletReforgeInput::PressKey(0x1B);
            diag.Info("按 Esc 关闭重铸台面板（防止 UI 遮挡仓库实体点击）");
            // 等待 ESC 完全释放（至少 300ms），避免连续按键触发系统菜单
            const int escWaitMs = (settings.uiWaitMs * 2 > 300) ? settings.uiWaitMs * 2 : 300;
            TabletReforgeInput::SleepMs(escWaitMs);
            m_lastAction = Clock::Now();
            // 只执行按 Esc，下一帧继续判断是否已经关闭；避免同一帧内"按 Esc+立即点击"打架
            // 让 OnFrameTick 的超时机制兜底：如果一直关不掉就报错
            return;
        }

        // —— 第 2 步：重铸台面板关了 → 找仓库实体并点击 ——
        // 确认 ESC 物理键已释放，避免点击时 ESC 还按着导致打开系统菜单
        if (TabletReforgeInput::IsEscPhysicallyDown()) {
            diag.Info("ESC 物理键尚未释放，等待后点击仓库...");
            m_lastAction = Clock::Now();
            return;
        }

        if (!calib.stashEntityPath.empty()) {
            auto stash = TabletReforgeGame::FindStash(ctx, calib);
            if (!stash || !stash->onScreen) {
                Error("找不到仓库实体或不在屏幕内，请调整相机后重试");
                return;
            }

            // 先移鼠标悬停一下，再点击
            const int sx = static_cast<int>(stash->screenX + 0.5f);
            const int sy = static_cast<int>(stash->screenY + 0.5f);
            TabletReforgeInput::MoveCursorScreen(sx, sy);
            if (settings.cursorSettleMs > 0)
                TabletReforgeInput::SleepMs(settings.cursorSettleMs);

            TabletReforgeInput::LeftClickAtCursor();
            diag.Info("点击仓库实体（重铸台面板已关闭）");
        } else {
            diag.Info("未标定仓库实体，等待用户手动打开仓库");
        }

        m_lastAction = Clock::Now();
        Transition(State::WaitForStashPanel);
    }

    void TickWaitForStashPanel(const PluginSDK::Context* ctx) {
        if (Throttled(settings.uiWaitMs)) return;
        m_lastAction = Clock::Now();

        // 重置 ESC 标志前确认物理键已释放，避免门控 NoEscCancel 误判
        if (m_escPressedByMachine && !TabletReforgeInput::IsEscPhysicallyDown()) {
            m_escPressedByMachine = false;
        }

        // 超过最大等待次数 → 报错
        if (m_openStashAttempts >= 15) {
            Error("等待仓库面板超时：已等待 " + std::to_string(m_openStashAttempts) + " 次仍未打开，请手动打开仓库");
            return;
        }
        ++m_openStashAttempts;

        bool stashOpen = TabletReforgeGame::IsStashOpen(ctx);
        diag.Info("WaitForStashPanel - 仓库:" + std::string(stashOpen ? "开" : "关")
            + ", 尝试:" + std::to_string(m_openStashAttempts));

        if (stashOpen) {
            diag.Info("仓库面板已打开，开始存回产物");
            Transition(State::Depositing);
            return;
        }

        // 如果标定了仓库实体，每隔几次轮询重试点击一次（最多 5 次）
        if (!calib.stashEntityPath.empty()
            && m_openStashAttempts % 3 == 0
            && m_openStashAttempts / 3 < 5) {
            auto stash = TabletReforgeGame::FindStash(ctx, calib);
            if (stash && stash->onScreen) {
                diag.Warn("仓库面板未检测到，重试点击仓库（第 "
                    + std::to_string(m_openStashAttempts / 3) + "/5 次）");
                const int sx = static_cast<int>(stash->screenX + 0.5f);
                const int sy = static_cast<int>(stash->screenY + 0.5f);
                TabletReforgeInput::LeftClickScreen(sx, sy);
                m_lastAction = Clock::Now();
            }
        }
    }

    void TickDepositing(const PluginSDK::Context* ctx) {
        // === 子类模式：存入所有选择的物品 ===
        // 日志：记录存入阶段的开始
        char startLog[256];
        sprintf_s(startLog, "[存入物品] 开始存入流程: 待存产物=%zu, 子类模式=%d",
            m_productAddresses.size(),
            settings.useSubCategoryMode && !settings.selectedSubCategories.empty() ? 1 : 0);
        diag.Info(startLog);
        OutputDebugStringA(startLog);
        
        if (settings.useSubCategoryMode && !settings.selectedSubCategories.empty()) {
            // 先存产物（合成输出）
            std::optional<TabletReforgeGame::ScreenRect> nextProductRect;
            uintptr_t nextProductAddr = 0;
            for (uintptr_t addr : m_productAddresses) {
                auto rect = TabletReforgeGame::FindBagItemByAddress(ctx, addr);
                if (rect) {
                    nextProductRect = rect;
                    nextProductAddr = addr;
                    break;
                }
            }

            if (nextProductRect.has_value()) {
                if (Throttled(settings.clickDelayMs)) return;
                CtrlDown();
                float cx, cy;
                TabletReforgeGame::RectCenter(*nextProductRect, cx, cy);
                TabletReforgeInput::HumanLikeMoveTo(static_cast<int>(cx), static_cast<int>(cy),
                    settings.enableHumanMouse, settings.mouseGravity, settings.mouseWind, settings.mouseMaxStep, settings.mouseStepWaitMs);
                if (settings.cursorSettleMs > 0)
                    TabletReforgeInput::SleepMs(settings.cursorSettleMs);
                if (m_isStackableSynthesis) TabletReforgeInput::RightClickAtCursor();
                else TabletReforgeInput::LeftClickAtCursor();
                if (settings.postClickDelayMs > 0)
                    TabletReforgeInput::SleepMs(settings.postClickDelayMs);
                m_productAddresses.erase(nextProductAddr);
                m_lastAction = Clock::Now();
                diag.Info("子类模式存产物（剩余 " + std::to_string(m_productAddresses.size()) + " 个）");
                return;
            }

            // 产物已存完，现在存背包里所有"选择的"物品
            auto wantedItem = TabletReforgeGame::NextWantedTabletInBag(ctx, settings);
            if (wantedItem) {
                if (Throttled(settings.clickDelayMs)) return;
                CtrlDown();
                float cx, cy;
                TabletReforgeGame::RectCenter(*wantedItem, cx, cy);
                TabletReforgeInput::HumanLikeMoveTo(static_cast<int>(cx), static_cast<int>(cy),
                    settings.enableHumanMouse, settings.mouseGravity, settings.mouseWind, settings.mouseMaxStep, settings.mouseStepWaitMs);
                if (settings.cursorSettleMs > 0)
                    TabletReforgeInput::SleepMs(settings.cursorSettleMs);
                if (m_isStackableSynthesis) TabletReforgeInput::RightClickAtCursor();
                else TabletReforgeInput::LeftClickAtCursor();
                if (settings.postClickDelayMs > 0)
                    TabletReforgeInput::SleepMs(settings.postClickDelayMs);
                m_lastAction = Clock::Now();
                diag.Info("子类模式存选择的物品");
                return;
            }

            // 全部存完
            CtrlUp();
            diag.Info("子类模式：所有选择的物品已存入仓库");
            m_productAddresses.clear();
            Transition(State::LoopCheck);
            return;
        }

        // === 旧版模式 ===
        // 找一个产物（只存回 m_productAddresses 里的物品）
        std::optional<TabletReforgeGame::ScreenRect> nextProductRect;
        uintptr_t nextProductAddr = 0;
        for (uintptr_t addr : m_productAddresses) {
            auto rect = TabletReforgeGame::FindBagItemByAddress(ctx, addr);
            if (rect) {
                nextProductRect = rect;
                nextProductAddr = addr;
                break;
            }
        }

        if (!nextProductRect.has_value()) {
            CtrlUp();
            char doneLog[256];
            sprintf_s(doneLog, "[存入物品] 产物已全部存回（剩余 %zu 个待存地址已不在背包）", m_productAddresses.size());
            diag.Info(doneLog);
            OutputDebugStringA(doneLog);
            m_productAddresses.clear();
            diag.Info("[存入物品] 存入完成，进入循环检查");
            OutputDebugStringA("[存入物品] 存入完成 → 循环检查\n");
            Transition(State::LoopCheck);
            return;
        }

        if (Throttled(settings.clickDelayMs)) return;

        // Ctrl+左键存回产物（旧版模式）
        CtrlDown();
        float cx, cy;
        TabletReforgeGame::RectCenter(*nextProductRect, cx, cy);
        TabletReforgeInput::HumanLikeMoveTo(static_cast<int>(cx), static_cast<int>(cy),
            settings.enableHumanMouse, settings.mouseGravity, settings.mouseWind, settings.mouseMaxStep, settings.mouseStepWaitMs);
        if (settings.cursorSettleMs > 0)
            TabletReforgeInput::SleepMs(settings.cursorSettleMs);
        if (m_isStackableSynthesis) TabletReforgeInput::RightClickAtCursor();
        else TabletReforgeInput::LeftClickAtCursor();
        if (settings.postClickDelayMs > 0)
            TabletReforgeInput::SleepMs(settings.postClickDelayMs);

        // 从产物集合中移除（已存回）
        m_productAddresses.erase(nextProductAddr);
        m_lastAction = Clock::Now();
        
        char progressLog[256];
        sprintf_s(progressLog, "[存入物品] 存回产物: addr=%llX, 剩余=%zu",
            (unsigned long long)nextProductAddr, m_productAddresses.size());
        diag.Info(progressLog);
        OutputDebugStringA(progressLog);
    }

    void TickLoopCheck(const PluginSDK::Context* ctx) {
        ++m_loopCount;
        m_identifyCheckDoneThisLoop = false;
        
        diag.Info("========================================");
        diag.Info("=== 完成第 " + std::to_string(m_loopCount) + " 轮循环 ===");
        diag.Info("========================================");
        
        OutputDebugStringA("\n========== 循环检查 ==========\n");

        // === 统计当前状态 ===
        auto bagScan = TabletReforgeGame::ScanBagCombined(ctx, settings);
        int bagMaterialCount = bagScan.materialCount;
        int bagUnidentifiedCount = TabletReforgeGame::CountUnidentifiedInBag(ctx, settings);
        int totalBagItems = bagScan.totalItems;
        int stashMaterialCount = TabletReforgeGame::CountTabletMaterialInStash(ctx, settings);
        
        char statsBuf[512];
        sprintf_s(statsBuf, "[循环统计] 背包: 材料=%d, 未鉴定=%d, 总物品=%d | 仓库: 材料=%d",
            bagMaterialCount, bagUnidentifiedCount, totalBagItems, stashMaterialCount);
        diag.Info(statsBuf);
        OutputDebugStringA(statsBuf);
        
        // === 决策树 ===
        
        // 1. 背包材料充足 (>3) → 继续合成
        if (bagMaterialCount > 3) {
            char buf[256];
            sprintf_s(buf, "[决策] 背包材料充足(%d > 3) → 继续合成", bagMaterialCount);
            diag.Info(buf);
            OutputDebugStringA(buf);
            Transition(State::ScanStash);  // 回到仓库，存入/取出后继续合成
            return;
        }
        
        // 2. 背包材料不足 (<3) → 检查未鉴定
        if (bagMaterialCount <= 3) {
            char buf[256];
            sprintf_s(buf, "[决策] 背包材料不足(%d <= 3) → 检查未鉴定物品", bagMaterialCount);
            diag.Info(buf);
            OutputDebugStringA(buf);
            
            // 2a. 有未鉴定物品 → 去鉴定
            if (bagUnidentifiedCount > 0) {
                char buf2[256];
                sprintf_s(buf2, "[决策] 背包有未鉴定物品(%d) → 前往NPC鉴定", bagUnidentifiedCount);
                diag.Info(buf2);
                OutputDebugStringA(buf2);
                m_identifyCheckDoneThisLoop = true;
                Transition(State::InteractWithNPC);
                return;
            }
            
            // 2b. 无未鉴定物品 → 检查仓库
            if (bagUnidentifiedCount == 0) {
                char buf2[256];
                sprintf_s(buf2, "[决策] 背包无未鉴定物品 → 检查仓库是否有材料");
                diag.Info(buf2);
                OutputDebugStringA(buf2);
                
                // 仓库有材料 → 取出材料装满背包
                if (stashMaterialCount > 0) {
                    char buf3[256];
                    sprintf_s(buf3, "[决策] 仓库有材料(%d) → 取出材料装满背包", stashMaterialCount);
                    diag.Info(buf3);
                    OutputDebugStringA(buf3);
                    Transition(State::ScanStash);
                    return;
                }
                
                // 仓库无材料 → 检查背包
                if (stashMaterialCount == 0) {
                    char buf3[256];
                    sprintf_s(buf3, "[决策] 仓库无材料 → 检查背包是否有可合成物品");
                    diag.Info(buf3);
                    OutputDebugStringA(buf3);
                    
                    // 背包有可合成物品 → 继续合成
                    if (bagMaterialCount >= 3) {
                        char buf4[256];
                        sprintf_s(buf4, "[决策] 背包有可合成物品(%d >= 3) → 继续合成", bagMaterialCount);
                        diag.Info(buf4);
                        OutputDebugStringA(buf4);
                        Transition(State::ScanStash);
                        return;
                    }
                    
                    // 背包物品不足 → 结束
                    if (bagMaterialCount < 3) {
                        char buf4[256];
                        sprintf_s(buf4, "[决策] 背包物品不足(%d < 3) → 本轮结束", bagMaterialCount);
                        diag.Info(buf4);
                        OutputDebugStringA(buf4);
                        
                        // 测试模式：跑 1 轮就停
                        if (m_testMode) {
                            diag.Info("测试模式完成，自动停止");
                            m_testMode = false;
                            Transition(State::Idle);
                            return;
                        }
                        
                        // 达到最大循环数？
                        if (settings.maxLoops > 0 && m_loopCount >= settings.maxLoops) {
                            diag.Info("达到最大循环数 " + std::to_string(settings.maxLoops) + "，结束");
                            Transition(State::Idle);
                            return;
                        }
                        
                        // 子类模式检查停止条件
                        if (settings.useSubCategoryMode && !settings.selectedSubCategories.empty()) {
                            int unwantedCount = TabletReforgeGame::CountMaterialTablets(ctx, settings);
                            int wantedCount = TabletReforgeGame::CountProductItemsInBag(ctx, settings);
                            
                            char buf5[256];
                            sprintf_s(buf5, "[子类模式] 检查停止条件: 非选择=%d, 选择=%d", unwantedCount, wantedCount);
                            diag.Info(buf5);
                            OutputDebugStringA(buf5);
                            
                            if (unwantedCount < settings.minUnwantedBeforeStop) {
                                diag.Info("非选择物品数量(" + std::to_string(unwantedCount) + ") 低于阈值(" + std::to_string(settings.minUnwantedBeforeStop) + ")");
                                
                                if (wantedCount > 0 && settings.autoDepositWanted) {
                                    diag.Info("将 " + std::to_string(wantedCount) + " 个选择的物品存入仓库");
                                    m_productAddresses.clear();
                                    Transition(State::CloseBenchAndOpenStash);
                                    return;
                                }
                                
                                // 尝试切换到下一个合成目标
                                if (settings.AdvanceToNextTarget()) {
                                    diag.Info("子类模式无更多物品，切换到下一个合成目标: "
                                        + (settings.GetActiveTarget() ? settings.GetActiveTarget()->name : "unknown"));
                                    Transition(State::ScanStash);
                                    return;
                                }
                                
                                diag.Info("已完成筛选合成，结束循环");
                                Transition(State::Idle);
                                return;
                            }
                        }

                        // 尝试切换到下一个合成目标（多合成物依次合成）
                        if (settings.AdvanceToNextTarget()) {
                            char buf5[256];
                            sprintf_s(buf5, "[决策] 背包不足(%d < 3)，切换到下一个合成目标: %s",
                                bagMaterialCount,
                                settings.GetActiveTarget() ? settings.GetActiveTarget()->name.c_str() : "unknown");
                            diag.Info(buf5);
                            OutputDebugStringA(buf5);
                            m_placedThisRound = 0;  // 重置放置计数，避免残留状态
                            Transition(State::ScanStash);
                            return;
                        }
                        
                        // 无法继续，结束
                        diag.Info("=== 循环结束：无更多可处理的物品 ===");
                        Transition(State::Idle);
                        return;
                    }
                }
            }
        }
    }

    void TickAborting(const PluginSDK::Context* ctx) {
        (void)ctx;
        // 等一帧让操作稳定，然后回 Idle
        if (Throttled(settings.uiWaitMs)) return;
        CtrlUp();
        Transition(State::Idle);
    }

    // ============================================================
    // NPC 鉴定流程（多利亚尼 Doryani）
    // 触发条件：仓库已空 + 背包有原料但不足 3 个 + 有需要鉴定的未鉴定物品
    // 流程：InteractWithNPC → WaitForNPCDialog → ClickIdentifyButton → WaitForIdentification
    // 完成后 → CloseBenchAndOpenStash（按 Esc 关 NPC 对话 → 重新开仓库取料/结束）
    // ============================================================

    // 走向 NPC 并左键点击互动
    void TickInteractWithNPC(const PluginSDK::Context* ctx) {
        // === 只按一次 ESC，避免连续按 ESC 进入系统设置 ===
        bool anyPanelOpen = TabletReforgeGame::AnyItemPanelOpen(ctx);
        bool stashOpen = TabletReforgeGame::IsStashOpen(ctx);
        bool benchCtx = TabletReforgeGame::IsBenchContextOpen(ctx);
        bool needClosePanel = anyPanelOpen || stashOpen || benchCtx;
        
        if (needClosePanel && !m_npcEscPressed) {
            // 第一次检测到面板开着 → 按一次 ESC
            m_escPressedByMachine = true;
            m_npcEscPressed = true;
            m_npcEscPressTime = Clock::Now();
            TabletReforgeInput::PressKey(0x1B);
            diag.Info("NPC 互动前按 Esc 关闭面板（仅一次）");
            m_lastAction = Clock::Now();
            return;
        }
        
        if (m_npcEscPressed) {
            // 已按过 ESC，等待面板关闭
            constexpr int kEscWaitMs = 600;  // 等待 600ms 让面板关闭
            auto elapsed = Clock::ElapsedMs(m_npcEscPressTime);
            
            if (elapsed < kEscWaitMs) {
                // 等待中
                if (elapsed % 200 < 30) {
                    diag.Info("等待 ESC 关闭面板... (" + std::to_string(elapsed) + "ms)");
                }
                return;
            }
            
            // 等待时间到，检查面板是否已关
            bool stillOpen = TabletReforgeGame::AnyItemPanelOpen(ctx)
                          || TabletReforgeGame::IsStashOpen(ctx)
                          || TabletReforgeGame::IsBenchContextOpen(ctx);
            if (stillOpen) {
                diag.Warn("ESC 后面板仍开着，但不再重复按 ESC，继续流程");
            }
            m_npcEscPressed = false;  // 重置，下次可以再按
            m_escPressedByMachine = false;
        } else {
            m_escPressedByMachine = false;
        }

        // 检查是否配置了 NPC Path
        if (calib.npcEntityPath.empty()) {
            Error("未标定 NPC 实体 Path（多利亚尼），请在标定界面添加 NPC 实体标定");
            return;
        }

        if (Throttled(settings.clickDelayMs * 3)) return;

        auto npc = TabletReforgeGame::FindNPC(ctx, calib);
        if (!npc) {
            // 找不到 NPC：可能不在屏幕内
            diag.Warn("未找到多利亚尼 NPC，尝试搜索...");
            // 遍历附近 NPC 实体（不限定 Path 的兜底）
            auto nearby = TabletReforgeGame::ListNearbyEntities(ctx, 80.f, PluginSDK::EntityType::NPC);
            if (!nearby.empty()) {
                auto& nearest = nearby.front();
                auto loc = TabletReforgeGame::FindEntityByPath(ctx, nearest.path);
                if (loc && loc->onScreen) {
                    const int nx = static_cast<int>(loc->screenX + 0.5f);
                    const int ny = static_cast<int>(loc->screenY + 0.5f);
                    TabletReforgeInput::MoveCursorScreen(nx, ny);
                    if (settings.cursorSettleMs > 0)
                        TabletReforgeInput::SleepMs(settings.cursorSettleMs);
                    TabletReforgeInput::LeftClickAtCursor();
                    m_lastAction = Clock::Now();
                    diag.Info("点击多利亚尼（兜底：附近最近 NPC）");
                    Transition(State::WaitForNPCDialog);
                    return;
                }
            }
            // 不在屏幕内：只能提示用户
            static int npcNotFoundCounter = 0;
            if (++npcNotFoundCounter % 20 == 0) {
                diag.Warn("多利亚尼不在屏幕内，请调整相机面向 NPC 或确认在城镇/藏身处");
            }
            m_lastAction = Clock::Now();
            return;
        }

        if (!npc->onScreen) {
            static int offScreenCounter = 0;
            if (++offScreenCounter % 20 == 0) {
                char buf[128];
                sprintf_s(buf, "多利亚尼在屏幕外（世界坐标 %.1f, %.1f），请调整相机",
                    npc->worldX, npc->worldY);
                diag.Warn(buf);
            }
            m_lastAction = Clock::Now();
            return;
        }

        // NPC 在屏幕内 → 点击互动
        const int nx = static_cast<int>(npc->screenX + 0.5f);
        const int ny = static_cast<int>(npc->screenY + 0.5f);
        TabletReforgeInput::MoveCursorScreen(nx, ny);
        if (settings.cursorSettleMs > 0)
            TabletReforgeInput::SleepMs(settings.cursorSettleMs);
        TabletReforgeInput::LeftClickAtCursor();
        m_lastAction = Clock::Now();
        char clickBuf[128];
        sprintf_s(clickBuf, "点击多利亚尼 NPC (%.1f, %.1f)", npc->screenX, npc->screenY);
        diag.Info(clickBuf);
        m_npcDialogWaitStart = Clock::Now();  // 记录对话等待开始时间
        Transition(State::WaitForNPCDialog);
    }

    // 等待 NPC 对话界面打开
    void TickWaitForNPCDialog(const PluginSDK::Context* ctx) {
        if (Throttled(settings.uiWaitMs)) return;
        m_lastAction = Clock::Now();

        // 先检查是否已有 NPC 对话面板打开
        if (TabletReforgeGame::IsNpcDialogOpen(ctx, calib)) {
            diag.Info("NPC 对话面板已打开");
            Transition(State::ClickIdentifyButton);
            return;
        }

        // 兜底等待：不管有没有配置 StringId，都超时后继续
        // 这样即使文字/StringId 检测失败，也能尝试点击鉴定按钮
        constexpr int kFallbackWaitMs = 2000;  // 2 秒兜底
        auto elapsed = Clock::ElapsedMs(m_npcDialogWaitStart);
        
        // 每秒输出一次等待日志
        if (elapsed > 0 && elapsed % 500 == 0) {
            diag.Info("等待 NPC 对话打开... (" + std::to_string(elapsed) + "ms)");
        }
        
        if (elapsed >= kFallbackWaitMs) {
            diag.Info("兜底等待完成 (" + std::to_string(elapsed) + "ms)，尝试点击鉴定按钮");
            Transition(State::ClickIdentifyButton);
        }
    }

    // 点击 NPC 对话界面的"鉴定/Identify"按钮
    void TickClickIdentifyButton(const PluginSDK::Context* ctx) {
        if (Throttled(settings.clickDelayMs)) return;

        auto btn = TabletReforgeGame::ResolveIdentifyButton(ctx, calib);
        if (!btn.valid) {
            static int btnFailCounter = 0;
            if (++btnFailCounter % 5 == 0) {
                diag.Warn("无法解析鉴定按钮：StringId/文字匹配均失败");
                std::string warnStr = "[鉴定按钮] 解析失败 (第" + std::to_string(btnFailCounter) + "次), StringId='";
                warnStr += calib.identifyButtonStringId;
                warnStr += "'\n";
                OutputDebugStringA(warnStr.c_str());
            }
            m_lastAction = Clock::Now();
            return;
        }

        // 调试日志：显示坐标来源
        char buf[256];
        sprintf_s(buf, "实时扫描鉴定按钮 → 点击 (X=%d, Y=%d)", btn.x, btn.y);
        diag.Info(buf);
        OutputDebugStringA(buf);

        TabletReforgeInput::MoveCursorScreen(btn.x, btn.y);
        if (settings.cursorSettleMs > 0)
            TabletReforgeInput::SleepMs(settings.cursorSettleMs);
        TabletReforgeInput::LeftClickAtCursor();
        if (settings.postClickDelayMs > 0)
            TabletReforgeInput::SleepMs(settings.postClickDelayMs);
        m_lastAction = Clock::Now();
        char btnBuf[128];
        sprintf_s(btnBuf, "点击鉴定按钮 (%d, %d)", btn.x, btn.y);
        diag.Info(btnBuf);
        OutputDebugStringA(btnBuf);
        Transition(State::WaitForIdentification);
    }

    // 等待鉴定完成（最多等待 10 秒）
    // 检测策略：
    //   1. 优先检测系统通知"已鉴定 N 个物品"（UI 文本遍历）—— 用户要求的方案
    //   2. 若通知未出现，10 秒超时后强制结束（兜底，避免卡死）
    void TickWaitForIdentification(const PluginSDK::Context* ctx) {
        // 鉴定通常在 0.5-2s 内完成，但最长等待 10 秒
        constexpr int kIdentifyWaitMs = 500;       // 每 500ms 检查一次通知
        constexpr int kMaxIdentifyWaitMs = 10000;  // 最长等待 10 秒
        constexpr int kPostNotificationDelayMs = 500; // 通知后等待 500ms 让鉴定生效

        // 初始化等待开始时间
        if (!m_identifyWaitInitialized) {
            m_identifyWaitStart = Clock::Now();
            m_identifyWaitInitialized = true;
            m_identifyNotificationSeen = false;
            diag.Info("[鉴定等待] 开始等待鉴定完成，监测系统通知...");
            OutputDebugStringA("[鉴定等待] 开始等待鉴定完成，监测系统通知\n");
        }

        int elapsedMs = (int)Clock::ElapsedMs(m_identifyWaitStart);

        // === 阶段 2：已检测到通知，等待 500ms 让鉴定效果完全生效 ===
        if (m_identifyNotificationSeen) {
            if (Clock::Expired(m_identifyCompleteTime, kPostNotificationDelayMs)) {
                diag.Info("[鉴定完成] 通知后等待完成，开始后处理");
                OutputDebugStringA("[鉴定完成] 通知后等待完成，进入后处理\n");
                m_identifyWaitInitialized = false;
                m_identifyNotificationSeen = false;
                HandlePostIdentification(ctx);
                return;
            }
            m_lastAction = Clock::Now();
            return;
        }

        // === 阶段 1：检测系统通知"已鉴定 N 个物品" ===
        // 每 500ms 执行一次 UI 树遍历（避免每帧扫描影响性能）
        if (!Throttled(kIdentifyWaitMs)) {
            std::string notification = TabletReforgeGame::FindIdentifyNotification(ctx);
            if (!notification.empty()) {
                m_identifyNotificationSeen = true;
                m_identifyCompleteTime = Clock::Now();
                diag.Info("[鉴定完成] 检测到系统通知: " + notification);
                OutputDebugStringA(("[鉴定完成] 通知: " + notification + "\n").c_str());
                m_lastAction = Clock::Now();
                return;
            }
        }

        // === 阶段 3：超时兜底 ===
        if (elapsedMs >= kMaxIdentifyWaitMs) {
            diag.Warn("[鉴定等待] 超时（已等待 " + std::to_string(elapsedMs) + "ms），强制进入后处理");
            OutputDebugStringA("[鉴定等待] 超时，强制进入后处理\n");
            m_identifyWaitInitialized = false;
            m_identifyNotificationSeen = false;
            HandlePostIdentification(ctx);
            return;
        }

        // 周期性输出等待进度
        if (Throttled(kIdentifyWaitMs)) {
            int bagUnidentified = TabletReforgeGame::CountUnidentifiedInBag(ctx, settings);
            char waitBuf[256];
            sprintf_s(waitBuf, "[鉴定等待] 等待中... 已等待=%dms, 未鉴定物品=%d", elapsedMs, bagUnidentified);
            diag.Info(waitBuf);
            OutputDebugStringA(waitBuf);
            m_lastAction = Clock::Now();
        }
    }

    // === 鉴定完成后的处理逻辑（流程图步骤 11-12）===
    // 1. 关闭 NPC 对话（按 Esc）
    // 2. 扫描背包，统计未鉴定物品数
    // 3. 若仍有未鉴定 → 继续鉴定（回到 InteractWithNPC）
    // 4. 若无未鉴定 → 进入仓库流程（CloseBenchAndOpenStash），存入不符合词缀的物品
    void HandlePostIdentification(const PluginSDK::Context* ctx) {
        // 统计背包状态
        auto bagScan = TabletReforgeGame::ScanBagCombined(ctx, settings);
        int bagMaterialCount = bagScan.materialCount;
        int bagUnidentifiedCount = TabletReforgeGame::CountUnidentifiedInBag(ctx, settings);
        int totalBagItems = bagScan.totalItems;

        char statsBuf[256];
        sprintf_s(statsBuf, "[鉴定完成] 背包统计: 材料=%d, 未鉴定=%d, 总物品=%d",
            bagMaterialCount, bagUnidentifiedCount, totalBagItems);
        diag.Info(statsBuf);
        OutputDebugStringA(statsBuf);

        // 关闭 NPC 对话（防重复：检查 ESC 物理键）
        if (!TabletReforgeInput::IsEscPhysicallyDown()) {
            diag.Info("关闭 NPC 对话...");
            OutputDebugStringA("[鉴定完成] 关闭NPC对话\n");
            m_escPressedByMachine = true;
            TabletReforgeInput::PressKey(0x1B);
            // 等待 ESC 完全释放（至少 300ms）
            const int escWaitMs = (settings.uiWaitMs * 2 > 300) ? settings.uiWaitMs * 2 : 300;
            TabletReforgeInput::SleepMs(escWaitMs);
        }
        m_lastAction = Clock::Now();

        // 若仍有未鉴定物品 → 继续鉴定
        if (bagUnidentifiedCount > 0) {
            diag.Info("鉴定后仍有 " + std::to_string(bagUnidentifiedCount) + " 个未鉴定物品，继续鉴定流程");
            OutputDebugStringA("[鉴定完成] 仍有未鉴定物品，继续鉴定\n");
            Transition(State::InteractWithNPC);
            return;
        }

        // 鉴定完成，进入仓库流程：存入不符合词缀的物品 → 取出材料 → 继续合成
        diag.Info("开始仓库处理流程：存入不符合要求的物品 → 取出材料 → 继续合成");
        OutputDebugStringA("[鉴定完成] 进入仓库处理流程\n");
        Transition(State::CloseBenchAndOpenStash);
    }
};

} // namespace TabletReforgeFlow
