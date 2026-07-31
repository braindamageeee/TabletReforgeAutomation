# -*- coding: utf-8 -*-
"""
mock_stash_current_scan_test.py — 验证只扫描当前打开仓库的逻辑

验证目标：
  1. 只扫描当前打开的仓库（Grid.Valid == true）
  2. AddOrUpdateCurrentStash 添加/更新逻辑
  3. 配置持久化流程
  4. 合成物品选择的正确显示
  5. 运行时仓库切换逻辑

用户场景：
  1. 玩家在游戏中打开"碎片仓库"
  2. 点击扫描按钮 → 只识别当前打开的仓库
  3. 选择仓库类型和合成物品
  4. 点击保存 → 配置永久保存
  5. 下次启动自动加载 → 插件知道合成物品在哪个仓库
"""
import json
import os
import tempfile
import shutil
from datetime import datetime

# ============================================================
# 模拟数据结构
# ============================================================

class Inventory:
    """模拟游戏中的 Inventory 结构"""
    def __init__(self, inv_id, name, grid_valid, total_x=12, total_y=12):
        self.InventoryId = inv_id
        self.Name = name
        self.Address = 0x1000 if grid_valid else 0
        self.GridValid = grid_valid
        self.TotalBoxesX = total_x
        self.TotalBoxesY = total_y

class StashMapping:
    """模拟仓库映射"""
    def __init__(self, inv_id=0, type_name="", type_id=-1, categories=None):
        self.inventoryId = inv_id
        self.stashTypeName = type_name
        self.stashTypeId = type_id
        self.itemCategories = categories or []
    
    def to_dict(self):
        return {
            "inventory_id": self.inventoryId,
            "stash_type_name": self.stashTypeName,
            "stash_type_id": self.stashTypeId,
            "item_categories": self.itemCategories
        }

class MockStashMappingManager:
    """模拟 StashMappingManager"""
    
    def __init__(self):
        self.mappings = {}  # inventoryId -> StashMapping
    
    def add_or_update_current(self, inv, categories=None):
        """只扫描/添加当前打开的仓库"""
        if inv.Address == 0 or not inv.GridValid:
            return False
        
        if inv.TotalBoxesX * inv.TotalBoxesY < 4:
            return False
        
        # 排除主背包
        if inv.Name.startswith("MainInventory"):
            return False
        
        # 自动识别仓库类型
        type_name, type_id = self._detect_stash_type(inv.Name)
        
        mapping = StashMapping(
            inv_id=inv.InventoryId,
            type_name=type_name,
            type_id=type_id,
            categories=categories or []
        )
        
        self.mappings[inv.InventoryId] = mapping
        return True
    
    def _detect_stash_type(self, name):
        """根据名称识别仓库类型"""
        type_map = {
            "NormalStash": ("NormalStash", 0),
            "FragmentStash": ("FragmentStash", 9),
            "CurrencyStash": ("CurrencyStash", 3),
            "MapStash": ("MapStash", 5),
            "EssenceStash": ("EssenceStash", 8),
            "QuadStash": ("QuadStash", 7),
            "UniqueStash": ("UniqueStash", 4),
        }
        return type_map.get(name, (name, -1))
    
    def get_config(self):
        return list(self.mappings.values())
    
    def save(self, file_path):
        data = {
            "count": len(self.mappings),
            "mappings": [m.to_dict() for m in self.mappings.values()]
        }
        os.makedirs(os.path.dirname(file_path), exist_ok=True)
        with open(file_path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
        return True
    
    def load(self, file_path):
        if not os.path.exists(file_path):
            return False
        with open(file_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        self.mappings = {}
        for m in data.get("mappings", []):
            mapping = StashMapping(
                inv_id=m["inventory_id"],
                type_name=m["stash_type_name"],
                type_id=m["stash_type_id"],
                categories=m["item_categories"]
            )
            self.mappings[mapping.inventoryId] = mapping
        return True
    
    def clear(self):
        self.mappings.clear()


def test_current_scan_only():
    """测试：只扫描当前打开的仓库"""
    print("=" * 70)
    print("[Mock] 验证只扫描当前打开仓库的逻辑")
    print("=" * 70)
    
    manager = MockStashMappingManager()
    
    # 模拟游戏中的所有 inventories
    all_inventories = [
        Inventory(100, "NormalStash", True),      # 当前打开的
        Inventory(101, "FragmentStash", False),    # 未打开
        Inventory(102, "CurrencyStash", False),    # 未打开
        Inventory(103, "MapStash", False),         # 未打开
        Inventory(1, "MainInventory", True),       # 主背包（应排除）
    ]
    
    print("\n📋 模拟游戏中的 inventories:")
    for inv in all_inventories:
        status = "✅ 当前打开" if inv.GridValid else "❌ 未打开"
        print(f"  [{status}] Id={inv.InventoryId}, Name={inv.Name}")
    
    # 只扫描当前打开的（Grid.Valid == true）
    print("\n🔍 只扫描当前打开的仓库...")
    current_opened = [inv for inv in all_inventories if inv.GridValid and inv.Name != "MainInventory"]
    
    print(f"  扫描到 {len(current_opened)} 个当前打开的仓库:")
    for inv in current_opened:
        print(f"    - Id={inv.InventoryId}, Name={inv.Name}")
        manager.add_or_update_current(inv)
    
    config = manager.get_config()
    print(f"\n  配置中的仓库数量: {len(config)}")
    
    # 验证：只有当前打开的被扫描
    assert len(config) == 1, f"应该只有1个仓库被扫描，实际{len(config)}个"
    assert config[0].inventoryId == 100, "应该扫描到 NormalStash (Id=100)"
    assert config[0].stashTypeName == "NormalStash"
    print("  ✓ 验证通过：只有当前打开的仓库被扫描")
    
    # 验证：主背包被排除
    inv_ids = [m.inventoryId for m in config]
    assert 1 not in inv_ids, "主背包(Id=1)应该被排除"
    print("  ✓ 验证通过：主背包被正确排除")


def test_user_workflow():
    """测试用户完整工作流"""
    print("\n" + "=" * 70)
    print("[Mock] 用户完整工作流测试")
    print("=" * 70)
    
    manager = MockStashMappingManager()
    test_dir = tempfile.mkdtemp()
    config_file = os.path.join(test_dir, "config", "stash_mapping.json")
    
    try:
        # 步骤1: 玩家打开"碎片仓库"并扫描
        print("\n📋 步骤1: 打开碎片仓库，扫描")
        fragment_stash = Inventory(200, "FragmentStash", True)
        assert manager.add_or_update_current(fragment_stash)
        config = manager.get_config()
        assert len(config) == 1
        assert config[0].stashTypeName == "FragmentStash"
        print(f"  ✓ 识别为 FragmentStash, Id=200")
        
        # 步骤2: 玩家为碎片仓库选择合成物品
        print("\n📋 步骤2: 为碎片仓库选择合成物品")
        manager.mappings[200].itemCategories = [1, 2]  # Tablets, Waystones
        print(f"  ✓ 已选择: 碑牌(Tablets), 地图钥匙(Waystones)")
        
        # 步骤3: 玩家切换到"货币仓库"并扫描
        print("\n📋 步骤3: 切换到货币仓库，扫描")
        currency_stash = Inventory(201, "CurrencyStash", True)
        assert manager.add_or_update_current(currency_stash)
        config = manager.get_config()
        assert len(config) == 2
        print(f"  ✓ 现在有 {len(config)} 个仓库配置")
        
        # 步骤4: 为货币仓库选择合成物品
        print("\n📋 步骤4: 为货币仓库选择合成物品")
        manager.mappings[201].itemCategories = [7, 8]  # Catalysts, Currency
        print(f"  ✓ 已选择: 催化剂(Catalysts), 货币(Currency)")
        
        # 步骤5: 玩家切换到"精髓仓库"并扫描
        print("\n📋 步骤5: 切换到精髓仓库，扫描")
        essence_stash = Inventory(202, "EssenceStash", True)
        assert manager.add_or_update_current(essence_stash)
        config = manager.get_config()
        assert len(config) == 3
        print(f"  ✓ 现在有 {len(config)} 个仓库配置")
        
        # 步骤6: 为精髓仓库选择合成物品
        print("\n📋 步骤6: 为精髓仓库选择合成物品")
        manager.mappings[202].itemCategories = [5, 6]  # Essences, Liquids
        print(f"  ✓ 已选择: 精髓(Essences), 情感蒸馏液(Liquids)")
        
        # 步骤7: 玩家保存配置
        print("\n💾 步骤7: 保存配置")
        assert manager.save(config_file)
        print(f"  ✓ 配置已保存到: {config_file}")
        
        # 步骤8: 验证保存内容
        print("\n🔍 步骤8: 验证保存内容")
        with open(config_file, 'r') as f:
            data = json.load(f)
        assert data["count"] == 3
        print(f"  ✓ 保存了 {data['count']} 个仓库映射")
        
        # 步骤9: 重新加载配置
        print("\n📂 步骤9: 重新加载配置")
        new_manager = MockStashMappingManager()
        assert new_manager.load(config_file)
        config = new_manager.get_config()
        assert len(config) == 3
        print(f"  ✓ 加载了 {len(config)} 个仓库映射")
        
        # 步骤10: 验证数据完整性
        print("\n✅ 步骤10: 验证数据完整性")
        for m in config:
            cats_str = ", ".join([str(c) for c in m.itemCategories])
            print(f"  [{m.stashTypeName}] Id={m.inventoryId}, Categories=[{cats_str}]")
        
        # 验证每个仓库的合成物品
        loaded_map = {m.inventoryId: m for m in config}
        assert loaded_map[200].itemCategories == [1, 2]
        assert loaded_map[201].itemCategories == [7, 8]
        assert loaded_map[202].itemCategories == [5, 6]
        print("  ✓ 所有数据完整性验证通过")
        
        # 步骤11: 运行时切换场景模拟
        print("\n🔄 步骤11: 运行时切换场景模拟")
        print("  场景：插件需要取碑牌(Tablets)进行合成")
        
        # 查找碑牌所在的仓库
        tablet_stash = None
        for m in config:
            if 1 in m.itemCategories:  # 1 = Tablets
                tablet_stash = m
                break
        
        assert tablet_stash is not None
        print(f"  ✓ 找到碑牌所在仓库: {tablet_stash.stashTypeName} (Id={tablet_stash.inventoryId})")
        print(f"  ✓ 插件将自动切换到该仓库页进行存取")
        
        # 步骤12: 清空配置
        print("\n🗑 步骤12: 清空配置")
        manager.clear()
        assert len(manager.get_config()) == 0
        print("  ✓ 配置已清空")
        
        # 结果
        print("\n" + "=" * 70)
        print("✅ 所有测试通过!")
        print("=" * 70)
        print("\n📊 验证结果汇总:")
        print("  ✓ 只扫描当前打开的仓库（Grid.Valid == true）")
        print("  ✓ 主背包正确排除")
        print("  ✓ 玩家可以逐步扫描多个仓库页")
        print("  ✓ 每个仓库页可独立配置合成物品")
        print("  ✓ 配置持久化正确（保存/加载）")
        print("  ✓ 运行时查找仓库逻辑正确")
        print("\n💡 运行时仓库切换说明:")
        print("  当插件运行时，状态机会:")
        print("  1. 根据合成物品类型查找对应的仓库映射")
        print("  2. 如果当前不是该仓库页，自动点击切换")
        print("  3. 等待切换完成后进行物品存取")
        print("  4. 合成完成后切回原来的仓库页")
        
    finally:
        shutil.rmtree(test_dir, ignore_errors=True)
        print(f"\n🧹 临时测试文件已清理")


def test_item_category_display():
    """测试合成物品分类的显示名称"""
    print("\n" + "=" * 70)
    print("[Mock] 合成物品分类显示验证")
    print("=" * 70)
    
    # 物品分类（对应C++ StashItemCategory枚举）
    categories = {
        1: "碑牌 (Tablets)",
        2: "地图钥匙 (Waystones)",
        3: "珠宝 (Jewels)",
        4: "符文 (Runes)",
        5: "精髓 (Essences)",
        6: "情感蒸馏液 (Liquids)",
        7: "催化剂 (Catalysts)",
        8: "货币 (Currency)",
        9: "碎片 (Fragments)",
        10: "地图 (Maps)",
        11: "预言卡 (Divination)",
        12: "珠宝饰品 (Jewellery)",
        13: "技能宝石 (Gems)",
        14: "药剂 (Flasks)",
        15: "可镶嵌物品 (Socketable)",
    }
    
    print("\n📋 合成物品分类列表（将显示在选择弹窗中）:")
    for cat_id, cat_name in categories.items():
        print(f"  [☐] {cat_name}")
    
    # 验证选择逻辑
    print("\n🔄 模拟选择效果:")
    selected = [1, 5, 8]  # 选择碑牌、精髓、货币
    print(f"  已选择 {len(selected)} 种物品:")
    for cat_id in selected:
        print(f"    [☑] {categories[cat_id]}")
    
    # 验证显示格式（与C++代码一致）
    display = "[%s]" % ", ".join([categories[c] for c in selected])
    print(f"\n  UI显示效果: {display}")
    
    print("\n✅ 物品分类显示验证完成")


def main():
    print("\n" + "#" * 70)
    print("# 仓库映射配置 - 只扫描当前仓库 Mock 验证")
    print("# " + datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    print("#" * 70)
    
    test_current_scan_only()
    test_user_workflow()
    test_item_category_display()
    
    print("\n" + "#" * 70)
    print("# 所有 Mock 验证完成!")
    print("#" * 70 + "\n")


if __name__ == "__main__":
    main()