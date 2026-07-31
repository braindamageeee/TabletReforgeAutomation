# -*- coding: utf-8 -*-
"""
mock_stash_filter_test.py — 独立Python mock测试：验证仓库Tab格子尺寸过滤逻辑
等价于 StashTypeTable.h + IsLikelyStashTabByGridSize 测试
"""
import json
import os

# StashTypeTable.h 25 entries, with v3 corrections from stashtype.json
KSTASH_TYPE_TABLE = [
    {"stashId": 0,  "id": "NormalStash",          "storageSlots": 12,  "gridHeight": 12},
    {"stashId": 1,  "id": "PremiumStash",         "storageSlots": 12,  "gridHeight": 12},
    {"stashId": 2,  "id": "TradeStash",           "storageSlots": 12,  "gridHeight": 12},
    {"stashId": 3,  "id": "CurrencyStash",        "storageSlots": 41,  "gridHeight": 4},   # v3 corrected
    {"stashId": 4,  "id": "UniqueStash",          "storageSlots": 94,  "gridHeight": 4},   # v3 corrected
    {"stashId": 5,  "id": "MapStash",             "storageSlots": 12,  "gridHeight": 6},   # v3 corrected
    {"stashId": 6,  "id": "DivinationCardStash",  "storageSlots": 0,   "gridHeight": 1},
    {"stashId": 7,  "id": "QuadStash",            "storageSlots": 24,  "gridHeight": 24},
    {"stashId": 8,  "id": "EssenceStash",         "storageSlots": 30,  "gridHeight": 4},   # v3 corrected
    {"stashId": 9,  "id": "FragmentStash",        "storageSlots": 152, "gridHeight": 3},   # v3 corrected
    {"stashId": 10, "id": "PCBangPremiumStash",   "storageSlots": 12,  "gridHeight": 12},
    {"stashId": 11, "id": "PCBangEssenceStash",   "storageSlots": 30,  "gridHeight": 4},   # v3 corrected
    {"stashId": 12, "id": "DelveStash",           "storageSlots": 41,  "gridHeight": 4},
    {"stashId": 13, "id": "BlightStash",          "storageSlots": 66,  "gridHeight": 4},
    {"stashId": 14, "id": "MetamorphStash",       "storageSlots": 62,  "gridHeight": 5},
    {"stashId": 15, "id": "DeliriumStash",        "storageSlots": 40,  "gridHeight": 1},   # v3 corrected
    {"stashId": 16, "id": "Folder",               "storageSlots": 0,   "gridHeight": 0},
    {"stashId": 17, "id": "FlaskStash",           "storageSlots": 250, "gridHeight": 4},
    {"stashId": 18, "id": "GemStash",             "storageSlots": 250, "gridHeight": 2},
    {"stashId": 19, "id": "SocketableStash",      "storageSlots": 214, "gridHeight": 1},
    {"stashId": 20, "id": "ExpeditionStash",      "storageSlots": 24,  "gridHeight": 4},
    {"stashId": 21, "id": "RitualStash",          "storageSlots": 42,  "gridHeight": 1},
    {"stashId": 22, "id": "BreachStash",          "storageSlots": 13,  "gridHeight": 5},
    {"stashId": 23, "id": "AbyssStash",           "storageSlots": 12,  "gridHeight": 3},
    {"stashId": 24, "id": "RelicStash",           "storageSlots": 12,  "gridHeight": 12},
]


def find_stash_type_by_grid_size(width, height):
    """C++ FindStashTypeByGridSize equivalent"""
    if width <= 0 or height <= 0:
        return None
    for e in KSTASH_TYPE_TABLE:
        if e["storageSlots"] <= 0 or e["gridHeight"] <= 0:
            continue
        if e["storageSlots"] == width and e["gridHeight"] == height:
            return e
    return None


def is_heuristic_large_stash_tab(width, height):
    """C++ IsHeuristicLargeStashTab equivalent"""
    if width <= 0 or height <= 0:
        return False
    total = width * height
    # exclude single-row or single-column long strips
    if height == 1 and width > 20:
        return False  # e.g. PassiveJewels1 57x1
    if width == 1 and height > 20:
        return False
    # Hard threshold: AbyssStash 12x3=36 is our floor
    if total >= 36:
        return True
    # Lower threshold: height>=4 or width>=10 + total>=20
    if height >= 4 or width >= 10:
        if total >= 20:
            return True
    return False


def is_likely_stash_tab_by_grid_size(width, height):
    """C++ IsLikelyStashTabByGridSize equivalent"""
    if find_stash_type_by_grid_size(width, height) is not None:
        return True
    return is_heuristic_large_stash_tab(width, height)


EQUIPMENT_EXACT_NAMES = set([
    "MainInventory1","BodyArmour1","Weapon1","Offhand1","Helm1","Amulet1","Ring1","Ring2",
    "Gloves1","Boots1","Belt1","Flask1","Cursor1","Map1","Weapon2","Offhand2","Weapon3",
    "Offhand3","Trinket1","GuildTag1","StashInventoryId","SkillSlots1","PassiveJewels1",
    "AnimatedArmour1","Leaguestone1","Currency1","MapCurrency1","MobileHeldMapsInventory1",
    "Relics1","RelicStorage1","SanctumSpecialRelic1","CurrentSanctumRun1",
    "LakeTabletInventory1","MemoryLineMaps","SentinelDroneInventory1","SentinelStorage1",
    "AtlasUpgrades1","AtlasUpgradesStorage","DefaultAttackSkills1","AscendancySkills1",
    "Tower1","ExpandedInventory1","UltimatumKey1","UltimatumKeySacrifice1",
    "DONOTUSE1","DONOTUSE2","DONOTUSE3","DONOTUSE4","DONOTUSE5","DONOTUSE6","DONOTUSE7",
    "ThreeToOneOutput","ThreeToOneInput","UNUSED1","UNUSED2",
])


def is_equipment_slot_name(name):
    if name in EQUIPMENT_EXACT_NAMES:
        return True
    if name.startswith("HeistNpcEquipment") and len(name) > 17 and name[17:].isdigit():
        return True
    return False


def is_non_stash_inventory(name, width, height, inventory_id=None):
    """Simplified C++ IsNonStashInventory equivalent.
    Returns True=Should filter (NOT stash), False=Keep (stash or main)."""
    # 1. Keep main inventory
    if name.startswith("MainInventory"):
        return False
    # 2. Keep by known type name
    for e in KSTASH_TYPE_TABLE:
        if e["id"] == name:
            return False
    # 3. Filter known equipment slot names
    if is_equipment_slot_name(name):
        return True
    # 6&7. Grid size filter (inventory_id no longer hard-filtered)
    if not is_likely_stash_tab_by_grid_size(width, height):
        return True
    return False


def print_result(pass_count, fail_count, title):
    total = pass_count + fail_count
    status = "OK" if fail_count == 0 else "FAIL"
    print(f"\n[{status}] {title}: PASS={pass_count} FAIL={fail_count} (total {total})")
    return fail_count == 0


def test_1_ggpk_exact_match():
    """Test1: Verify all ggpk stashtype.json known types get exact match"""
    print("===== Test1: FindStashTypeByGridSize exact match (ggpk 18 types) =====")
    # All 25 entries (including 7 not-in-stashtype.json)
    cases = [(e["id"], e["storageSlots"], e["gridHeight"], True) for e in KSTASH_TYPE_TABLE
             if e["storageSlots"] > 0 and e["gridHeight"] > 0]
    # Add the 18 stashtype.json specific ones too
    pass_n = 0
    fail_n = 0
    for name, w, h, should in cases:
        hit = find_stash_type_by_grid_size(w, h)
        matched = hit is not None
        ok = matched == should
        tag = "PASS" if ok else "FAIL"
        print(f"  [{tag}] {name} ({w}x{h}) -> matched={matched} hit={hit['id'] if hit else None}")
        if ok:
            pass_n += 1
        else:
            fail_n += 1
    return print_result(pass_n, fail_n, "Test1 ggpk exact match")


def test_2_equipment_slots_filtered():
    """Test2: All equipment slots from inventories.json should get filtered"""
    print("\n===== Test2: Equipment slots filtered (IsNonStashInventory true) =====")
    # Typical equipment slots from inventories.json
    equipment_cases = [
        ("BodyArmour1",      3, 6,    True),   # should be filtered (non-stash)
        ("Weapon1",          4, 1,    True),
        ("Helm1",            2, 8,    True),
        ("Amulet1",          1, 15,   True),
        ("Ring1",            1, 12,   True),
        ("Ring2",            1, 13,   True),
        ("Gloves1",          2, 9,    True),
        ("Boots1",           2, 10,   True),
        ("Belt1",            1, 14,   True),
        ("Weapon2",          4, 2,    True),
        ("Offhand2",         4, 3,    True),
        ("Trinket1",         1, 1,    True),
        ("PassiveJewels1",   57, 1,   True),   # single row: blocked by heuristic
        ("AnimatedArmour1",  4, 4,    True),
        ("HeistNpcEquipment1", 10, 4, True),
        ("SanctumSpecialRelic1", 2, 2, True),
        # MainInventory: kept
        ("MainInventory1",   12, 5,   False),  # don't filter
        # Known stash type names: kept
        ("NormalStash",      12, 12,  False),
        ("CurrencyStash",    41, 4,   False),
        ("MapStash",         12, 6,   False),
    ]
    pass_n = 0
    fail_n = 0
    for name, w, h, should_filter in equipment_cases:
        filtered = is_non_stash_inventory(name, w, h)
        ok = filtered == should_filter
        tag = "PASS" if ok else "FAIL"
        print(f"  [{tag}] {name} ({w}x{h}) filtered={filtered} (expect={should_filter})")
        if ok:
            pass_n += 1
        else:
            fail_n += 1
    return print_result(pass_n, fail_n, "Test2 equipment filter")


def test_3_heuristic_large_size():
    """Test3: bug1.log-style big inventories should pass heuristic"""
    print("\n===== Test3: Heuristic big-size stash matching =====")
    big_cases = [
        ("bug_107x12_huge", 107, 12, True),   # real stash from bug1.log
        ("bug_14x9_normal",  14, 9,  True),   # another real one
        ("bug_37x10_wide",   37, 10, True),   # currency-like
        ("Abyss_floor_12x3", 12, 3,  True),   # minimum real type
        ("small_3x2",         3, 2,  False),  # tiny (sub 36)
        ("small_4x1",         4, 1,  False),  # tiny
        ("Passive_57x1_long",57, 1,  False),  # single-row strip blocked
        ("3x12_col",          3, 12, True),   # 36 total >=36
    ]
    pass_n = 0
    fail_n = 0
    for name, w, h, should_pass in big_cases:
        matched = is_likely_stash_tab_by_grid_size(w, h)
        ok = matched == should_pass
        tag = "PASS" if ok else "FAIL"
        reason = None
        hit = find_stash_type_by_grid_size(w, h)
        if hit:
            reason = f"ggpkExact:{hit['id']}"
        elif is_heuristic_large_stash_tab(w, h):
            reason = "heuristic"
        else:
            reason = "none"
        print(f"  [{tag}] {name} ({w}x{h}) likely={matched} (via {reason})")
        if ok:
            pass_n += 1
        else:
            fail_n += 1
    return print_result(pass_n, fail_n, "Test3 heuristic match")


def main():
    all_ok = True
    all_ok &= test_1_ggpk_exact_match()
    all_ok &= test_2_equipment_slots_filtered()
    all_ok &= test_3_heuristic_large_size()
    print("\n" + ("="*60))
    print(f"FINAL RESULT: {'ALL TESTS PASSED' if all_ok else 'SOME TESTS FAILED'}")
    print("="*60)
    return 0 if all_ok else 1


if __name__ == "__main__":
    exit(main())
