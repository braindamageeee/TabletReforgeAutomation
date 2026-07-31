# 审计规则（Audit Rules）— PoeFixer 重铸台自动化插件

> **用途**：定义代码审计的"允许/禁止"边界，供 AI 助手与人工审查时引用。
> 与项目宪法 §3.5 配套使用——宪法给出原则，本文件给出可执行的检查规则。

---

## 规则 A：词缀文本字段零滥用（核心反作弊红线）

### A.1 允许的调用模式

| 调用 | 字段使用 | 风险等级 | 备注 |
|------|----------|----------|------|
| `ctx->Inventory.ReadItemMods(addr)` | `im.Rarity` / `im.IsIdentified` / `im.ItemLevel` / `im.IsCorrupted` / `im.IsMirrored` / `im.IsSplit` / `im.IsRelic` / `im.IsSynthesised` / `im.CraftedModCount` / `im.Valid` | ✅ 允许 | 这些是**汇总字段**，不含词缀文本 |
| `ctx->Inventory.ReadItemRarity(addr)` | 返回 int | ✅ 允许 | 最轻量，只读 rarity |
| `ctx->Inventory.ReadItemPath(addr)` | 返回 string | ✅ 允许 | Path 字符串，零风险 |
| `ctx->Inventory.ReadItemBaseTypeName(addr)` | 返回 string | ✅ 允许 | BaseType 字符串，零风险 |
| `ctx->Inventory.ReadItemStackCount(addr)` | 返回 int | ✅ 允许 | 堆叠数 |
| `ctx->Inventory.ReadItemUniqueName(addr)` | 返回 string | ✅ 允许 | 传奇物品名 |
| 库存快照 `item.Path` / `item.Rarity` / `item.IsIdentified` | 同上 | ✅ 允许 | 但 rarity/identified 不可靠时必须用 ReadItemMods 兜底 |

### A.2 禁止的调用模式

| 调用 | 字段使用 | 风险等级 | 备注 |
|------|----------|----------|------|
| `ctx->Inventory.ReadItemMods(addr)` 后访问 `im.ExplicitMods` / `im.ImplicitMods` / `im.EnchantMods` / `im.HellscapeMods` / `im.CrucibleMods` 的任何字段 | 任何 | ❌ 禁止 | 词缀文本内容 = 反作弊红线 |
| `enumerate_item_mods_by_entity` 回调内消费 `Mod.Text` / `Mod.StatKeys` / `Mod.Values` | 任何 | ❌ 禁止 | 同上 |
| 把词缀文本用于：筛选 / 评分 / 显示给用户 / 写入日志 / 持久化到磁盘 | 任何 | ❌ 禁止 | 哪怕只为调试也不行 |

### A.3 审计命令（每个会话或合并前执行）

```powershell
# 1. 检查业务代码零词缀文本滥用（应零命中，sdk/PluginSDK.h 内部填充除外）
findstr /S /N "ExplicitMods ImplicitMods EnchantMods HellscapeMods CrucibleMods" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\game\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\flow\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\ui\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\TabletReforgeAutomation.cpp"

# 2. 确认 ReadItemMods 调用点只用汇总字段（人工审查每个命中点）
findstr /S /N "ReadItemMods" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\game\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\flow\*.h"

# 3. 确认没有 enumerate_item_mods_by_entity 的业务消费（sdk 包装内部除外）
findstr /S /N "enumerate_item_mods_by_entity" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\game\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\flow\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\ui\*.h"
```

### A.4 当前审计基线（2026-07-30，**待用户决策**）

**初次声明撤销**：早期声明"词缀文本字段滥用 = 0 处"是**错误的**——审计命令实测发现 10 处活跃的词缀文本字段访问。

#### 实测审计结果

| 文件 | 行号 | 访问的字段 | 用途 | 触发条件 |
|------|------|------------|------|----------|
| `game/InventoryChecker.h` | 83-87 | `mods.ImplicitMods/ExplicitMods/EnchantMods/HellscapeMods/CrucibleMods` 的 `.Name` / `.AffixName` / `.StatKey` | 提取到 `BagItem::modNames/modAffixes/modStatKeys` 供筛选用 | `needMods = settings.useModifierFilterMode && !selectedModifierKeys.empty()` |
| `game/StashOps.h` | 208-212 | 同上 | 提取到 `StashTablet::modNames/modAffixes/modStatKeys` 供筛选用 | 同上 |
| `game/TabletFilter.h` | 1085-1345+ | `MatchesDesiredReforgeTypeDebug()` 接收并消费 `modNames/modAffixes/modStatKeys` | 物品筛选决策（原料 vs 产物） | 同上 |
| `config/Settings.h` | 500, 505-506, 589, 596 | `useModifierFilterMode` / `selectedModifierKeys` 持久化 | 配置开关 | 用户在 UI 勾选开启 |

#### 完整特性链路

这是一个**完整的词缀筛选特性**（不是调试代码）：
1. 用户在 Settings UI 勾选"词缀筛选模式" → `useModifierFilterMode = true` + `selectedModifierKeys` 填充
2. `StashOps::ScanStash` / `InventoryChecker::CollectBagItemsWithInfo` 检测到 `needMods=true`
3. 调用 `ReadItemMods(addr)` 拿到 `ItemMods`
4. 通过 `ExtractModNames()` 把 5 类词缀的 `Name` / `AffixName` / `StatKey` 文本全部提取到 `BagItem` / `StashTablet`
5. 传给 `MatchesDesiredReforgeTypeDebug()` 做原料/产物筛选决策

#### 当前状态

- **代码现状**：违反宪法 §3.5 新政策（"绝不读词缀文本内容"）
- **用户决策（2026-07-30 第一轮）**：选了"保留现状，只更新宪法与 P2 文档"——但该决策基于我错误的前提"代码只用 Rarity/Identified 字段"
- **真正所需决策**：用户需在了解完整范围后重新选择：
  - C1：进一步演进 §3.5，明确允许"词缀筛选模式"读词缀文本（特性继续可用）
  - C2：移除词缀筛选特性（删 `ExtractModNames` / `MatchesDesiredReforgeTypeDebug` 词缀路径 / `useModifierFilterMode` 配置，回退到纯 Path/Rarity 筛选）
  - C3：保留代码但默认关闭 + 加显眼警告（`useModifierFilterMode` 默认 false + UI 红字提示"启用此模式会读取物品词缀文本，存在理论风险"）
- **审计结论**：❌ **不合规**（与 §3.5 政策冲突），待用户决策后修正

---

## 规则 B：配置文件原子写入（P1 落地后）

### B.1 强制要求

所有写 `settings.json` / `calib.json` 的代码路径必须经过 `TabletReforgeConfig::AtomicWriteText()`（见 `config/AtomicWrite.h`）。禁止业务代码直接 `std::ofstream out(target)` 写这两个文件。

### B.2 审计命令

```powershell
# 直接 ofstream 写 settings.json / calib.json 的代码（应零命中）
findstr /S /N "settings.json calib.json" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\*.h" `
  "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\*.cpp"
```

合法命中应只在 `config/Settings.h` 的 `SettingsPath()` 和 `config/CalibData.h` 的 `CalibPath()` 内出现（用于构造路径），实际写入必须走 `AtomicWriteText`。

### B.3 例外

`ui/SettingsPanel.h` 的"导出碑牌数据"功能（exportPath）是用户主动导出，路径由用户选择，不在原子写入强制范围内。但若导出失败，UI 必须明确提示用户。

---

## 规则 C：状态超时诊断完整性（P3 落地后）

### C.1 强制要求

任何状态超时触发的 `Abort("状态超时: ...")` 必须先调用 `DumpTimeoutSnapshot(ctx)` 输出完整 ctx 快照（门控状态、面板布尔值、计数器、附近实体数等），便于事后排查。

### C.2 审计命令

```powershell
# 状态超时 Abort 调用点（应只有一处，且前面紧跟 DumpTimeoutSnapshot）
findstr /N "状态超时" "f:\Trae\chuxue\Plugins\TabletReforgeAutomation\flow\StateMachine.h"
```

---

## 维护说明

- 每次落地新的"P 级"任务（如 P4 并行扫描、P5 产物评分）后，**必须**同步更新本文件添加对应规则
- 项目宪法 §3.5 引用本文件，本文件是宪法原则的可执行细则
- 若审计发现违规，**禁止**直接修改业务代码绕过——必须先回到项目宪法 §3.5 评估是否需要政策演进，再统一调整
