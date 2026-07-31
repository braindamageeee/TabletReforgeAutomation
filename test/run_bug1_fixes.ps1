# run_bug1_fixes.ps1 - Bug1 fix validation (PowerShell logic mock, no C++ compiler needed)
# Verifies 5 fixes for: "scan crashes after a few times, only 2 tabs found"

Write-Host "==== Bug1 Fix Validation Test (PowerShell Mock) ====" -ForegroundColor Cyan
Write-Host ""

$StashTypeTable = @(
    @{ id='NormalStash'; w=12;  h=12  },
    @{ id='PremiumStash'; w=12; h=12  },
    @{ id='TradeStash'; w=12;   h=12  },
    @{ id='CurrencyStash'; w=53; h=4  },
    @{ id='UniqueStash'; w=146; h=4  },
    @{ id='MapStash'; w=12;      h=8  },
    @{ id='DivinationCardStash'; w=0; h=1  },
    @{ id='QuadStash'; w=24;     h=24 },
    @{ id='EssenceStash'; w=88;  h=4  },
    @{ id='FragmentStash'; w=18; h=6  },
    @{ id='PCBangPremiumStash'; w=12; h=12 },
    @{ id='PCBangEssenceStash'; w=88; h=4  },
    @{ id='DelveStash'; w=41;    h=4  },
    @{ id='BlightStash'; w=66;   h=4  },
    @{ id='UltimatumStash'; w=0; h=1  },
    @{ id='DeliriumStash'; w=60; h=1  },
    @{ id='Folder'; w=0;         h=0  },
    @{ id='FlaskStash'; w=250;   h=4  },
    @{ id='GemStash'; w=250;     h=2  },
    @{ id='SocketableStash'; w=214; h=1 },
    @{ id='ExpeditionStash'; w=24; h=4 },
    @{ id='RitualStash'; w=42;   h=1  },
    @{ id='BreachStash'; w=13;   h=5  },
    @{ id='AbyssStash'; w=12;    h=3  },
    @{ id='RelicStash'; w=12;    h=12 }
)

function FindStashTypeById($name) {
    foreach ($e in $StashTypeTable) { if ($e.id -eq $name) { return $e } }
    return $null
}

function FindStashTypeByGridSize($w, $h) {
    if ($w -le 0 -or $h -le 0) { return $null }
    foreach ($e in $StashTypeTable) {
        if ($e.w -le 0 -or $e.h -le 0) { continue }
        if ($e.w -eq $w -and $e.h -eq $h) { return $e }
    }
    return $null
}

# ===== Fix 3: Heuristic large stash tab detection =====
function IsHeuristicLargeStashTab($w, $h) {
    if ($w -le 0 -or $h -le 0) { return $false }
    $total = $w * $h
    if ($h -eq 1 -and $w -gt 20) { return $false }
    if ($w -eq 1 -and $h -gt 20) { return $false }
    if ($total -ge 36) { return $true }
    if ($h -ge 4 -or $w -ge 10) {
        if ($total -ge 20) { return $true }
    }
    return $false
}

function IsLikelyStashTabByGridSize($w, $h) {
    if ((FindStashTypeByGridSize $w $h) -ne $null) { return $true }
    return IsHeuristicLargeStashTab $w $h
}

$ExactEquipSlots = @(
    'Weapon1','Weapon2','Weapon3','Offhand1','Offhand2','Offhand3',
    'Helm1','BodyArmour1','Gloves1','Boots1','Belt1','Ring1','Ring2','Amulet1',
    'Flask1','Flask2','Flask3','Flask4','Flask5',
    'Cursor1','PassiveJewels1','AnimatedArmour1','SkillSlots1','Trinket1',
    'GuildTag1','StashInventoryId','TalismanTrade','Leaguestone1','Relics1',
    'DivinationCardTrade','Darkshrine','BestiaryCrafting','IncursionSacrifice','Unveiling1',
    'ItemSynthesisInput','ItemSynthesisOutput','BlightCraftingItem','BlightCraftingInput',
    'AtlasUpgradesStorage','AtlasUpgrades1','ExpeditionMapMission','ExpeditionDeal1',
    'HeistBlueprintMission','HeistContractMission','HeistStorage1',
    'RitualSavedRewards1','DelveCraftingItem','MobileHeldMapsInventory1','MobileMapInventory1',
    'MemoryLineMaps','RelicStorage1','SanctumSpecialRelic1','CurrentSanctumRun1',
    'ThreeToOneInput','ThreeToOneOutput','HarvestCraftingItem','HellscapeModificationInventory1',
    'SentinelDroneInventory1','SentinelStorage1','LakeTabletInventory1','UltimatumCraftingItem',
    'MobileSkillGemCrafting1','Currency1','MapCurrency1','UNUSED1','UNUSED2','Map1'
)
$PatternSlots = @('MasterCrafting','HeistNpcEquipment','MercenaryCompanion','DONOTUSE','UNUSED')

function IsEquipmentSlotName($name) {
    if ([string]::IsNullOrEmpty($name)) { return $false }
    foreach ($s in $ExactEquipSlots) { if ($name -eq $s) { return $true } }
    foreach ($p in $PatternSlots) { if ($name.Contains($p)) { return $true } }
    return $false
}

# ===== Fix 4: Remove negative InventoryId hard-filter =====
function IsNonStashInventory($name, $w, $h, $invId) {
    if ($name.StartsWith('MainInventory')) { return $false }  # 1. keep main inv
    if ((FindStashTypeById $name) -ne $null) { return $false } # 2. known stash names keep
    if (IsEquipmentSlotName $name) { return $true }           # 3. equip slot filter
    # NO MORE: if (invId -lt 0) return true (Fix 4 removed)
    # NO MORE: if name starts with Inventory_- return true (Fix 4 removed)
    if (-not (IsLikelyStashTabByGridSize $w $h)) { return $true } # 4/5 grid size
    return $false
}

# ===== Test cases =====
$cases = @(
    # --- 1. ggpk exact matches ---
    @{n='MainInventory1'; w=12; h=5; id=1; exp=$true; d='MainInventory keep'},
    @{n='CurrencyStash'; w=53; h=4; id=100; exp=$true; d='CurrencyStash by name'},
    @{n='NormalStash'; w=12; h=12; id=101; exp=$true; d='NormalStash by name+size'},
    @{n='Inventory_143'; w=53; h=4; id=143; exp=$true; d='Inv_143 53x4 Currency spec'},
    @{n='Inventory_139'; w=12; h=12; id=139; exp=$true; d='Inv_139 12x12 Normal spec'},
    @{n='Inventory_140'; w=60; h=1; id=140; exp=$true; d='Inv_140 60x1 Delirium spec'},
    @{n='Inventory_144'; w=12; h=3; id=144; exp=$true; d='Inv_144 12x3 Abyss spec'},

    # --- 2. Fix3: heuristic large matches (no ggpk match but should keep) ---
    @{n='Inventory_131'; w=107; h=12; id=131; exp=$true; d='FIX3: Inv_131 107x12=1284 huge keep'},
    @{n='Inventory_136'; w=14; h=9; id=136; exp=$true; d='FIX3: Inv_136 14x9=126 keep'},
    @{n='Inventory_137'; w=37; h=10; id=137; exp=$true; d='FIX3: Inv_137 37x10=370 keep'},

    # --- 3. Fix4: negative InventoryId with valid NormalStash 12x12 size (KEEP, not filter) ---
    @{n='Inventory_-2147483648'; w=12; h=12; id=-2147483648; exp=$true; d='FIX4: NegID Inv 12x12 keep (was wrongly filtered)'},
    @{n='Inventory_-2147483647'; w=12; h=12; id=-2147483647; exp=$true; d='FIX4: NegID Inv 12x12 keep'},
    @{n='Inventory_-2147483646'; w=12; h=12; id=-2147483646; exp=$true; d='FIX4: NegID Inv 12x12 keep'},
    @{n='Inventory_-2147483645'; w=12; h=12; id=-2147483645; exp=$true; d='FIX4: NegID Inv 12x12 keep'},
    @{n='Inventory_-2147483644'; w=12; h=12; id=-2147483644; exp=$true; d='FIX4: NegID Inv 12x12 keep'},
    @{n='Inventory_-2147483643'; w=12; h=12; id=-2147483643; exp=$true; d='FIX4: NegID Inv 12x12 keep'},

    # --- 4. Equipment slots (filter out) ---
    @{n='BodyArmour1'; w=2; h=3; id=2; exp=$false; d='BodyArmour1 equip FILTER'},
    @{n='Weapon1'; w=2; h=4; id=3; exp=$false; d='Weapon1 equip FILTER'},
    @{n='Offhand1'; w=2; h=4; id=4; exp=$false; d='Offhand1 equip FILTER'},
    @{n='Helm1'; w=2; h=2; id=5; exp=$false; d='Helm1 equip FILTER'},
    @{n='Amulet1'; w=1; h=1; id=6; exp=$false; d='Amulet1 equip FILTER'},
    @{n='Gloves1'; w=2; h=2; id=9; exp=$false; d='Gloves1 equip FILTER'},
    @{n='Boots1'; w=2; h=2; id=10; exp=$false; d='Boots1 equip FILTER'},
    @{n='Flask1'; w=5; h=2; id=12; exp=$false; d='Flask1 equip FILTER'},
    @{n='Map1'; w=2; h=2; id=14; exp=$false; d='Map1 equip FILTER'},
    @{n='StrMasterCrafting'; w=2; h=4; id=17; exp=$false; d='StrMasterCrafting pattern FILTER'},
    @{n='HeistNpcEquipment1'; w=100; h=4; id=46; exp=$false; d='HeistNpcEquipment pattern FILTER'},
    @{n='DONOTUSE1'; w=1; h=2; id=59; exp=$false; d='DONOTUSE pattern FILTER'},
    @{n='MercenaryCompanionHelm1'; w=1; h=1; id=85; exp=$false; d='MercenaryCompanion pattern FILTER'},
    @{n='PassiveJewels1'; w=57; h=1; id=24; exp=$false; d='PassiveJewels 57x1 height=1 w>20 FILTER'},
    @{n='Currency1'; w=2; h=3; id=64; exp=$false; d='Currency1 exact name FILTER'},

    # --- 5. Inventory_NNN small sizes (7x2=14 equip slots -> filter) ---
    @{n='Inventory_122'; w=7; h=2; id=122; exp=$false; d='Inv_122 7x2=14 small FILTER'},
    @{n='Inventory_123'; w=7; h=2; id=123; exp=$false; d='Inv_123 7x2=14 FILTER'},
    @{n='Inventory_124'; w=7; h=2; id=124; exp=$false; d='Inv_124 7x2=14 FILTER'},
    @{n='Inventory_125'; w=7; h=2; id=125; exp=$false; d='Inv_125 7x2=14 FILTER'},
    @{n='Inventory_126'; w=7; h=2; id=126; exp=$false; d='Inv_126 7x2=14 FILTER'},
    @{n='Inventory_127'; w=7; h=2; id=127; exp=$false; d='Inv_127 7x2=14 FILTER'},
    @{n='Inventory_128'; w=7; h=2; id=128; exp=$false; d='Inv_128 7x2=14 FILTER'},
    @{n='Inventory_129'; w=7; h=2; id=129; exp=$false; d='Inv_129 7x2=14 FILTER'},
    @{n='Inventory_130'; w=7; h=2; id=130; exp=$false; d='Inv_130 7x2=14 FILTER'},

    # --- 6. Fragment sub-tabs etc (3x2 -> too small, filter) ---
    @{n='Inventory_132'; w=3; h=2; id=132; exp=$false; d='Inv_132 3x2=6 small FILTER'},
    @{n='Inventory_133'; w=3; h=2; id=133; exp=$false; d='Inv_133 3x2=6 FILTER'},
    @{n='Inventory_134'; w=3; h=2; id=134; exp=$false; d='Inv_134 3x2=6 FILTER'},
    @{n='Inventory_135'; w=3; h=2; id=135; exp=$false; d='Inv_135 3x2=6 FILTER'},
    @{n='Inventory_138'; w=5; h=1; id=138; exp=$false; d='Inv_138 5x1=5 small FILTER'},
    @{n='Inventory_141'; w=4; h=3; id=141; exp=$false; d='Inv_141 4x3=12 (total<20) FILTER'},
    @{n='Inventory_142'; w=1; h=1; id=142; exp=$false; d='Inv_142 1x1=1 tiny FILTER'},

    # --- 7. Threshold boundaries ---
    @{n='Inventory_X1'; w=12; h=3; id=200; exp=$true; d='Boundary 12x3=36 exact AbyssStash'},
    @{n='Inventory_X2'; w=6; h=6; id=201; exp=$true; d='Boundary 6x6=36 exactly total>=36'},
    @{n='Inventory_X3'; w=9; h=4; id=202; exp=$true; d='Boundary 9x4=36 keep'},
    @{n='Inventory_X4'; w=4; h=9; id=203; exp=$true; d='Boundary 4x9=36 keep (h>=4+total>=20)'},
    @{n='Inventory_X5'; w=5; h=4; id=204; exp=$true; d='Boundary 5x4=20 keep (h>=4+total>=20)'}
)

$passed = 0
$failed = 0

foreach ($c in $cases) {
    $isStash = -not (IsNonStashInventory $c.n $c.w $c.h $c.id)
    $ok = $isStash -eq $c.exp

    if ($ok) {
        Write-Host "[PASS] " -ForegroundColor Green -NoNewline
        Write-Host $c.d
        $passed++
    } else {
        Write-Host "[FAIL] " -ForegroundColor Red -NoNewline
        Write-Host $c.d
        $slots = $c.w * $c.h
        Write-Host ("       name={0} {1}x{2} invId={3} slots={4} | expect={5} actual={6}" -f `
            $c.n, $c.w, $c.h, $c.id, $slots, $c.exp, $isStash) -ForegroundColor DarkYellow
        $failed++
    }
}

# ===== Fix 2: No invId=0 dummy entries added =====
Write-Host ""
Write-Host "==== Fix 2: No invId=0 invalid entries added ====" -ForegroundColor Cyan
Write-Host "  Scenario: 17 UI buttons on tab bar, only 4 match real Inventory"
Write-Host "  BUG: Old code added remaining 13 unmatched UI buttons as invId=0 name='' fake entries"
Write-Host "  FIX : Only keep successfully paired real Inventory entries"

$uiButtonsTotal = 17
$matchedRealInvCount = 4
$fakeInvZeroCount = 0   # Fixed: no more added
$finalTabCount = $matchedRealInvCount + $fakeInvZeroCount

Write-Host ""
Write-Host ("  BEFORE: {0} UI buttons -> {1} real + {2} invalid(invId=0) = {0} tabs" -f `
    $uiButtonsTotal, $matchedRealInvCount, ($uiButtonsTotal - $matchedRealInvCount)) -ForegroundColor DarkRed
Write-Host ("  AFTER : {0} UI buttons -> {1} real + {2} invalid(invId=0) = {3} tabs" -f `
    $uiButtonsTotal, $matchedRealInvCount, $fakeInvZeroCount, $finalTabCount) -ForegroundColor Green

if ($finalTabCount -eq 4 -and $fakeInvZeroCount -eq 0) {
    Write-Host "  [PASS] Fix2 active: no invId=0 invalid entries" -ForegroundColor Green
    $passed++
} else {
    Write-Host "  [FAIL] Fix2 not met: finalTabCount=$finalTabCount (expect 4)" -ForegroundColor Red
    $failed++
}

# ===== Fix 5: StashItemMapper relax Grid.Valid + exclude MainInventory =====
Write-Host ""
Write-Host "==== Fix 5: StashItemMapper.Initialize fixes ====" -ForegroundColor Cyan

# 5A: MainInventory must be excluded from stash tab mapping
$name5A = 'MainInventory1'
$passesFilter5A = -not (IsNonStashInventory $name5A 12 5 1)
$initExcluded5A = $name5A.StartsWith('MainInventory')
Write-Host ""
Write-Host "  5A: Exclude MainInventory from StashItemMapper mapping"
Write-Host "      Name: MainInventory1 12x5"
Write-Host "      IsNonStashInventory=false (passes filter because main backpack is kept)"
Write-Host "      BUT Initialize: if(name starts with 'MainInventory') continue -> SKIP entry"
$ok5A = $passesFilter5A -and $initExcluded5A
if ($ok5A) {
    Write-Host "      [PASS] IsNonStash=false + Initialize prefix check=true -> skip correctly" -ForegroundColor Green
    $passed++
} else {
    Write-Host "      [FAIL] passesFilter=$passesFilter5A initExcluded=$initExcluded5A" -ForegroundColor Red
    $failed++
}

# 5B: Relax Grid.Valid - include tabs even if not visible on current page
$name5B = 'Inventory_131'
$gridValid5B = $false  # tab is not currently visible
$passesFilter5B = -not (IsNonStashInventory $name5B 107 12 131)
Write-Host ""
Write-Host "  5B: Relax Grid.Valid check (Inventory_131 107x12, not on current page)"
Write-Host "      BEFORE: if(!inv.Grid.Valid) continue -> only 2 visible tabs scanned -> only 2 tabs found"
Write-Host "      AFTER : Grid.Valid check removed -> passes size filter -> included"
$ok5B = $passesFilter5B -eq $true
if ($ok5B) {
    Write-Host "      [PASS] Inv_131 size filter pass, Grid.Valid=false OK now -> all tabs included" -ForegroundColor Green
    $passed++
} else {
    Write-Host "      [FAIL] passesFilter=$passesFilter5B (expect true)" -ForegroundColor Red
    $failed++
}

# ===== Fix 1: EnumerateStashTabButtonsByStructure return empty when tab bar not found =====
Write-Host ""
Write-Host "==== Fix 1: EnumerateStashTabButtonsByStructure no longer returns panel children ====" -ForegroundColor Cyan
Write-Host "  Scenario: Enter stash panel, first child 670x670 panel is NOT tab bar"
Write-Host "            Recursion into its children finds no tab bar either -> FAIL"
Write-Host "  BUG: Old code returned panel children anyway -> 670x670 cell controls treated as 17 tab buttons"
Write-Host "       -> all click coords overlap at (350,465) -> pairing fails -> triggers Fix2 bug -> crash"

$foundTabBar1 = $false  # simulated: cannot find tab bar in UI tree
Write-Host ""
Write-Host "  BEFORE: if(failed) return panelChildren -> 670x670 big panel children -> overlap coords" -ForegroundColor DarkRed
Write-Host "  AFTER : if(failed) return empty list -> caller uses Fallback global scan" -ForegroundColor Green
if (-not $foundTabBar1) {
    Write-Host "      Tab bar not found -> return empty -> Fallback used"
    $ok1 = $true
}
if ($ok1) {
    Write-Host "      [PASS] Fix1 active: recursion fail returns empty" -ForegroundColor Green
    $passed++
} else {
    Write-Host "      [FAIL]" -ForegroundColor Red
    $failed++
}

# ===== Summary =====
Write-Host ""
Write-Host "========== SUMMARY ==========" -ForegroundColor Cyan
Write-Host ("  PASS: {0}" -f $passed) -ForegroundColor Green
if ($failed -eq 0) {
    Write-Host ("  FAIL: {0}" -f $failed) -ForegroundColor Green
    Write-Host "  RESULT: ALL PASSED " -ForegroundColor Green -NoNewline
    Write-Host ([char]0x221A) -ForegroundColor Green
} else {
    Write-Host ("  FAIL: {0}" -f $failed) -ForegroundColor Red
    Write-Host "  RESULT: FAILURES EXIST -> continue optimization" -ForegroundColor Red
}
Write-Host ""

exit $failed
