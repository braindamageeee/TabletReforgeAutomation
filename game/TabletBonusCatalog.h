#pragma once

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <Windows.h>

#include "../sdk/PluginSDK.h"

namespace TabletReforgeGame {

struct Bonus {
    std::string Id;
    std::string Label;
    std::string Category;
    std::string NormId;
    std::string NormIdStripped;
};

inline std::string ToLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

inline std::string NormalizeIdentifier(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value)
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

inline std::string StripTrailingDigits(const std::string& v) {
    size_t end = v.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(v[end - 1]))) --end;
    return v.substr(0, end);
}

inline bool BonusMatches(const Bonus& b,
                         const std::unordered_set<std::string>& keys) {
    if (keys.find(b.NormId) != keys.end()) return true;
    if (!b.NormIdStripped.empty() && b.NormIdStripped != b.NormId
        && keys.find(b.NormIdStripped) != keys.end())
        return true;
    return false;
}

namespace detail {

inline constexpr const char* kCommon   = "通用";
inline constexpr const char* kMechanic = "机制专属";
inline constexpr const char* kUnique   = "传奇石板";

inline Bonus B(const char* id, const char* label, const char* category) {
    Bonus b;
    b.Id = id;
    b.Label = label;
    b.Category = category;
    b.NormId = NormalizeIdentifier(id);
    b.NormIdStripped = StripTrailingDigits(b.NormId);
    return b;
}

// ============================================================
// POE2 游戏 mod id (snake_case) 映射表
// ============================================================
// 问题背景：TabletBonusCatalog 的 Bonus.Id 是自定义 CamelCase（如 TowerMonsterEffectiveness），
// 但游戏 ReadItemMods 返回的是 POE2 内部 mod id（snake_case，如 map_monster_potency_+.）。
// 两者命名空间完全不同，子串匹配永远失败，导致词缀筛选形同虚设。
//
// 解决方案：为每个 Bonus.Id 提供对应的 POE2 mod id 核心片段（去掉 _+. 后缀），
// 加入 selectedModifierKeys 后，ContainsCI(mod, key) 即可命中。
//
// 数据来源：poe2db.tw/tw/Temple_Tablet + 实际游戏日志中观察到的 mod id
inline std::vector<std::string> GetPoe2ModIdKeywords(const std::string& bonusId) {
    // —— 通用词缀（CommonBonuses）——
    if (bonusId == "TowerDroppedItemRarityIncrease")
        return {"map_item_drop_rarity", "map_dropped_item_rarity"};
    if (bonusId == "TowerMapDroppedMapsIncrease")
        return {"map_map_item_drop_chance", "map_dropped_maps", "map_waystone_drop"};
    if (bonusId == "TowerDroppedGoldIncrease")
        return {"map_extra_gold_piles", "map_dropped_gold", "map_gold_find"};
    if (bonusId == "TowerExperienceGainIncrease")
        return {"map_experience_gain"};
    if (bonusId == "TowerMonsterEffectiveness")
        return {"map_monster_potency", "map_monster_effectiveness"};
    if (bonusId == "TowerMonsterRarityIncrease")
        return {"map_monster_rarity"};
    if (bonusId == "TowerRarePackIncrease")
        return {"map_number_of_rare_packs", "map_rare_pack", "map_rare_monster"};
    if (bonusId == "TowerMagicPackIncrease")
        return {"map_number_of_magic_packs", "map_magic_pack"};
    if (bonusId == "TowerPackSizeIncrease")
        return {"map_pack_size", "map_incursion_vaal_beacon_pack_size"};
    if (bonusId == "TowerRareMonsterSurpassing")
        return {"map_rare_monster_modifier", "map_surpassing"};
    if (bonusId == "TowerReducedPackSize")
        return {"map_reduced_pack_size"};
    if (bonusId == "TowerRareChestCount")
        return {"map_additional_chests", "map_rare_chest", "map_chest_rare"};
    if (bonusId == "TowerAdditionalStoneCircle")
        return {"map_stone_circle", "map_summoning_circle"};
    if (bonusId == "TowerAdditionalExile")
        return {"map_exile", "map_rogue_exile"};
    if (bonusId == "TowerAdditionalAzmeriWisp")
        return {"map_azmeri", "map_spirit", "map_wisp"};
    if (bonusId == "TowerAdditionalEssence")
        return {"map_essence", "map_additional_essence"};
    if (bonusId == "TowerAdditionalShrine")
        return {"map_shrine", "map_additional_shrine"};
    if (bonusId == "TowerAdditionalStrongbox")
        return {"map_strongbox", "map_additional_strongbox"};
    if (bonusId == "TowerStoneCircleChance")
        return {"map_stone_circle_chance", "map_summoning_circle_chance"};
    if (bonusId == "TowerAdditionalExileChance")
        return {"map_exile_chance", "map_rogue_exile_chance"};
    if (bonusId == "TowerAdditionalSpiritChance")
        return {"map_azmeri_chance", "map_spirit_chance"};
    if (bonusId == "TowerAdditionalEssenceChance")
        return {"map_essence_chance"};
    if (bonusId == "TowerAdditionalShrineChance")
        return {"map_shrine_chance"};
    if (bonusId == "TowerAdditionalStrongboxChance")
        return {"map_strongbox_chance"};
    if (bonusId == "TowerMapAdditionalModifier")
        return {"map_additional_modifier", "map_extra_modifier", "map_random_modifier"};
    if (bonusId == "TowerMapAdditionalUniqueMonsterModifier")
        return {"map_unique_monster_modifier", "map_rare_modifier"};

    // —— Temple（神庙碑牌）专属词缀 ——
    if (bonusId == "TowerIncursionPackSize")
        return {"map_incursion_vaal_beacon_pack_size"};
    if (bonusId == "TowerIncursionExtraPacksChance")
        return {"map_incursion_vaal_beacon_extra_packs_chance", "map_incursion_vaal_beacon_extra_pack"};
    if (bonusId == "TowerIncursionExtraPacks")
        return {"map_incursion_vaal_beacon_extra_packs"};
    if (bonusId == "TowerIncursionSecondaryEncounters")
        return {"map_incursion_vaal_beacon_secondary_encounter"};
    if (bonusId == "TowerIncursionTokenChance")
        return {"map_incursion_vaal_beacon_crystal", "map_incursion_token_chance"};
    if (bonusId == "TowerIncursionBossChance")
        return {"map_incursion_vaal_beacon_boss", "map_incursion_vaal_beacon_unique"};
    if (bonusId == "TowerIncursionRareChestChance")
        return {"map_incursion_vaal_beacon_chest_rare"};

    // —— Breach（裂痕碑牌）专属词缀 ——
    if (bonusId == "TowerBreachAdditionalRares")
        return {"map_breach_additional_rares", "map_breach_rare"};
    if (bonusId == "TowerBreachBossChance")
        return {"map_breach_boss", "map_breach_chance"};
    if (bonusId == "TowerBreachWombgiftLevelChance")
        return {"map_breach_wombgift_level", "map_breach_wombgift"};
    if (bonusId == "TowerBreachWombgiftQuantity")
        return {"map_breach_wombgift_quantity", "map_breach_wombgift"};
    if (bonusId == "TowerBreachHivebloodQuantity")
        return {"map_breach_hiveblood", "map_breach_hive"};
    if (bonusId == "TowerBreachRareMonsterPotency")
        return {"map_breach_rare_monster_potency", "map_breach_potency"};
    if (bonusId == "TowerBreachMonsterQuantity")
        return {"map_breach_monster_quantity", "map_breach_monster"};

    // —— Expedition（探险碑牌）专属词缀 ——
    if (bonusId == "TowerExpeditionArtifactQuantity")
        return {"map_expedition_artifact", "map_expedition_quantity"};
    if (bonusId == "TowerExpeditionBombPlacementDistance")
        return {"map_expedition_bomb", "map_expedition_distance"};
    if (bonusId == "TowerExpeditionNumberOfRunes")
        return {"map_expedition_runes", "map_expedition_number_of_runes"};
    if (bonusId == "TowerExpeditionExplosionRadius")
        return {"map_expedition_explosion", "map_expedition_radius"};
    if (bonusId == "TowerExpeditionJournalQuantity")
        return {"map_expedition_journal", "map_expedition_logbook"};
    if (bonusId == "TowerExpeditionRareMonsterCount")
        return {"map_expedition_rare_monster", "map_expedition_rare"};
    if (bonusId == "TowerExpeditionEffectiveness")
        return {"map_expedition_effectiveness"};
    if (bonusId == "TowerExpeditionMarkerQuantity")
        return {"map_expedition_marker", "map_expedition_markers"};

    // —— Delirium（谵妄碑牌）专属词缀 ——
    if (bonusId == "TowerDeliriumAdditionalShardsChance")
        return {"map_delirium_shards", "map_delirium_additional_shards"};
    if (bonusId == "TowerDeliriumRareMonsterPause")
        return {"map_delirium_rare_monster_pause", "map_delirium_pause"};
    if (bonusId == "TowerDeliriumDoodadsIncrease")
        return {"map_delirium_doodads", "map_delirium_mirror"};
    if (bonusId == "TowerDeliriumPackSizeIncrease")
        return {"map_delirium_pack_size", "map_delirium_pack"};
    if (bonusId == "TowerDeliriumDifficultyIncrease")
        return {"map_delirium_difficulty", "map_delirium_increase"};
    if (bonusId == "TowerDeliriumFogPersistence")
        return {"map_delirium_fog_persistence", "map_delirium_fog"};
    if (bonusId == "TowerDeliriumFogDissipationDelayNew")
        return {"map_delirium_fog_dissipation", "map_delirium_delay"};
    if (bonusId == "TowerDeliriumMonsterSplinterIncrease")
        return {"map_delirium_splinter", "map_delirium_monster_splinter"};
    if (bonusId == "TowerDeliriumBossChance")
        return {"map_delirium_boss", "map_delirium_chance"};

    // —— Abyss（深渊碑牌）专属词缀 ——
    if (bonusId == "TowerAbyssAdditionalChance")
        return {"map_abyss_additional", "map_abyss_chance"};
    if (bonusId == "TowerAbyss4AdditionalChance")
        return {"map_abyss_4_additional", "map_abyss_four"};
    if (bonusId == "TowerAbyssExtraTickets")
        return {"map_abyss_extra_tickets", "map_abyss_tickets"};
    if (bonusId == "TowerAbyssExtraModifiers")
        return {"map_abyss_extra_modifiers", "map_abyss_modifiers"};
    if (bonusId == "TowerAbyssIncreasedRewards")
        return {"map_abyss_increased_rewards", "map_abyss_rewards"};
    if (bonusId == "TowerAbyssDepthsChance")
        return {"map_abyss_depths", "map_abyss_depths_chance"};
    if (bonusId == "TowerAbyssEffectivenessPerChasm")
        return {"map_abyss_effectiveness", "map_abyss_chasm"};
    if (bonusId == "TowerAbyssRareMonsterIncrease")
        return {"map_abyss_rare_monster", "map_abyss_rare"};
    if (bonusId == "TowerAbyssMonsterIncrease")
        return {"map_abyss_monster_increase", "map_abyss_monster"};

    // —— Ritual（祭祀碑牌）专属词缀 ——
    if (bonusId == "TowerRitualOmenChance")
        return {"map_ritual_omen", "map_ritual_chance"};
    if (bonusId == "TowerRitualMagicMonsters")
        return {"map_ritual_magic_monsters", "map_ritual_magic"};
    if (bonusId == "TowerRitualRareMonsters")
        return {"map_ritual_rare_monsters", "map_ritual_rare"};
    if (bonusId == "TowerRitualChanceForNoCost")
        return {"map_ritual_no_cost", "map_ritual_cost"};
    if (bonusId == "TowerRitualAdditionalReroll")
        return {"map_ritual_additional_reroll", "map_ritual_reroll"};
    if (bonusId == "TowerRitualDeferSpeed")
        return {"map_ritual_defer_speed", "map_ritual_defer"};
    if (bonusId == "TowerRitualDeferCostIncrease")
        return {"map_ritual_defer_cost", "map_ritual_defer"};
    if (bonusId == "TowerRitualRerollCostIncrease")
        return {"map_ritual_reroll_cost", "map_ritual_reroll"};
    if (bonusId == "TowerRitualTributeIncrease")
        return {"map_ritual_tribute", "map_ritual_tribute_increase"};

    // —— Overseer（总督碑牌）专属词缀 ——
    if (bonusId == "TowerMapBossExperience")
        return {"map_boss_experience", "map_map_boss_experience"};
    if (bonusId == "TowerMapBossWaystoneChance")
        return {"map_boss_waystone", "map_boss_waystone_chance"};
    if (bonusId == "TowerMapBossRarity")
        return {"map_boss_rarity", "map_boss_drop_rarity"};
    if (bonusId == "TowerMapBossQuantity")
        return {"map_boss_quantity", "map_boss_drop_quantity"};
    if (bonusId == "TowerMapBossAdditionalSpirit")
        return {"map_boss_additional_spirit", "map_boss_spirit"};
    if (bonusId == "TowerMapBossAdditionalEssence")
        return {"map_boss_additional_essence", "map_boss_essence"};
    if (bonusId == "TowerMapBossAdditionalShrine")
        return {"map_boss_additional_shrine", "map_boss_shrine"};
    if (bonusId == "TowerMapBossAdditionalStrongbox")
        return {"map_boss_additional_strongbox", "map_boss_strongbox"};

    // 默认：返回空，表示无映射
    return {};
}

inline const std::vector<Bonus>& CommonBonuses() {
    static const std::vector<Bonus> v = {
        B("TowerDroppedItemRarityIncrease", "地图中物品稀有度提高", kCommon),
        B("TowerMapDroppedMapsIncrease", "地图中路径石数量提高", kCommon),
        B("TowerDroppedGoldIncrease", "地图中黄金提高", kCommon),
        B("TowerExperienceGainIncrease", "地图中经验获取提高", kCommon),
        B("TowerMonsterEffectiveness", "怪物效能提高", kCommon),
        B("TowerMonsterRarityIncrease", "地图怪物稀有度提高", kCommon),
        B("TowerRarePackIncrease", "地图稀有怪物数量提高", kCommon),
        B("TowerMagicPackIncrease", "地图魔法怪物数量提高", kCommon),
        B("TowerPackSizeIncrease", "地图群组规模提高", kCommon),
        B("TowerRareMonsterSurpassing", "稀有怪物有超越几率获得额外词缀", kCommon),
        B("TowerReducedPackSize", "地图群组规模降低", kCommon),
        B("TowerRareChestCount", "地图包含额外稀有宝箱", kCommon),
        B("TowerAdditionalStoneCircle", "地图包含1个额外召唤法阵", kCommon),
        B("TowerAdditionalExile", "地图有1个额外流放者", kCommon),
        B("TowerAdditionalAzmeriWisp", "地图包含1个额外阿兹莫里灵体", kCommon),
        B("TowerAdditionalEssence", "地图包含1个额外精华", kCommon),
        B("TowerAdditionalShrine", "地图包含1个额外神坛", kCommon),
        B("TowerAdditionalStrongbox", "地图包含1个额外保险箱", kCommon),
        B("TowerStoneCircleChance", "地图包含召唤法阵的几率提高", kCommon),
        B("TowerAdditionalExileChance", "地图包含流放者的几率提高", kCommon),
        B("TowerAdditionalSpiritChance", "地图包含阿兹莫里灵体的几率提高", kCommon),
        B("TowerAdditionalEssenceChance", "地图包含精华的几率提高", kCommon),
        B("TowerAdditionalShrineChance", "地图包含神坛的几率提高", kCommon),
        B("TowerAdditionalStrongboxChance", "地图包含保险箱的几率提高", kCommon),
        B("TowerMapAdditionalModifier", "地图有额外随机词缀", kCommon),
        B("TowerMapAdditionalUniqueMonsterModifier", "传奇怪物有1个额外稀有词缀", kCommon),
    };
    return v;
}

inline std::vector<Bonus> SpecificBonuses(const std::string& typeKey) {
    if (typeKey == "Irradiated") {
        return {
            B("UniqueBiomeTabletForest", "地圖同時視為森林地圖", kUnique),
            B("UniqueBiomeTabletMountain", "地圖同時視為山脈地圖", kUnique),
            B("UniqueBiomeTabletWater", "地圖同時視為水域地圖", kUnique),
            B("UniqueBiomeTabletDesert", "地圖同時視為沙漠地圖", kUnique),
            B("UniqueBiomeTabletGrass", "地圖同時視為草原地圖", kUnique),
            B("UniqueMapsAddIrridiationWhenCompleting", "完成非輻照地圖時改為添加輻照", kUnique),
        };
    }
    if (typeKey == "Breach") {
        return {
            B("TowerPackSizeIncrease", "地圖中裂隙的群組規模提高", kCommon),
            B("TowerBreachAdditionalRares", "不穩定裂隙穩定時額外生成稀有怪物", kMechanic),
            B("TowerBreachBossChance", "不穩定裂隙包含弗倫·謝什元帥的幾率提高", kMechanic),
            B("TowerBreachWombgiftLevelChance", "子宮贈禮有幾率掉落高一級", kMechanic),
            B("TowerBreachWombgiftQuantity", "地圖中子宮贈禮數量提高", kMechanic),
            B("TowerBreachHivebloodQuantity", "地圖中巢血數量提高", kMechanic),
            B("TowerBreachRareMonsterPotency", "稀有裂隙怪物效能提高", kMechanic),
            B("TowerBreachMonsterQuantity", "裂隙怪物密度提高", kMechanic),
            B("UniqueBreachHiveAdditionalWaves", "裂隙巢穴有額外波次的巢生怪物", kUnique),
            B("UniqueBreachMinimumRadius", "不穩定裂隙在計時填滿後額外數秒才坍塌", kUnique),
            B("UniqueBreachUnstableAdditionalRares", "不穩定裂隙穩定時額外生成稀有怪物", kUnique),
            B("UniqueTowerBreachDensityIncrease", "裂隙怪物密度發生變化", kUnique),
        };
    }
    if (typeKey == "Expedition") {
        return {
            B("TowerExpeditionArtifactQuantity", "探險事件怪物掉落的文物數量提高", kMechanic),
            B("TowerExpeditionBombPlacementDistance", "探險事件放置炸藥的距離提高", kMechanic),
            B("TowerExpeditionNumberOfRunes", "探險事件含有額外魔符", kMechanic),
            B("TowerExpeditionExplosionRadius", "探險事件爆炸範圍提高", kMechanic),
            B("TowerExpeditionJournalQuantity", "探險事件符文怪物掉落的探險日誌數量提高", kMechanic),
            B("TowerExpeditionRareMonsterCount", "探險事件含有稀有怪物數量提高", kMechanic),
            B("TowerExpeditionEffectiveness", "探險事件魔符效果提高", kMechanic),
            B("TowerExpeditionMarkerQuantity", "探險事件符文怪物印記數量提高", kMechanic),
        };
    }
    if (typeKey == "Delirium") {
        return {
            B("TowerDeliriumAdditionalShardsChance", "譫妄迷霧生成更多鏡碎片", kMechanic),
            B("TowerDeliriumRareMonsterPause", "擊殺稀有怪物會暫停譫妄鏡計時", kMechanic),
            B("TowerDeliriumDoodadsIncrease", "譫妄迷霧生成更多碎裂鏡", kMechanic),
            B("TowerDeliriumPackSizeIncrease", "譫妄怪物群組規模提高", kMechanic),
            B("TowerDeliriumDifficultyIncrease", "譫妄迷霧對玩家施加更高譫妄度", kMechanic),
            B("TowerDeliriumFogPersistence", "譫妄迷霧消散更慢", kMechanic),
            B("TowerDeliriumFogDissipationDelayNew", "譫妄迷霧消散前額外持續數秒", kMechanic),
            B("TowerDeliriumMonsterSplinterIncrease", "地圖中模擬幻境碎片堆疊數量提高", kMechanic),
            B("TowerDeliriumBossChance", "譫妄遭遇更可能生成傳奇首領", kMechanic),
            B("UniqueDeliriumDifficultyIncrease", "譫妄迷霧改變對玩家施加的譫妄度", kUnique),
            B("UniqueDeliriumEndlessFog", "你的地圖中譫妄迷霧永不消散", kUnique),
        };
    }
    if (typeKey == "Abyss") {
        return {
            B("TowerAbyssAdditionalChance", "地圖包含一個額外深淵", kMechanic),
            B("TowerAbyss4AdditionalChance", "地圖有幾率包含四個額外深淵", kMechanic),
            B("TowerAbyssExtraTickets", "深淵掉落褻瀆通貨的幾率提高", kMechanic),
            B("TowerAbyssExtraModifiers", "深淵怪物帶有深淵詞綴的幾率提高", kMechanic),
            B("TowerAbyssIncreasedRewards", "深淵坑洞有獎勵的幾率翻倍", kMechanic),
            B("TowerAbyssDepthsChance", "深淵通向深淵深處的幾率提高", kMechanic),
            B("TowerAbyssEffectivenessPerChasm", "每關閉一個坑洞，深淵怪物難度與獎勵提高", kMechanic),
            B("TowerAbyssRareMonsterIncrease", "深淵額外生成稀有怪物", kMechanic),
            B("TowerAbyssMonsterIncrease", "深淵生成更多怪物", kMechanic),
        };
    }
    if (typeKey == "Ritual") {
        return {
            B("TowerRitualOmenChance", "祭禮恩惠為預兆的幾率提高", kMechanic),
            B("TowerRitualMagicMonsters", "祭禮祭壇復活的怪物為稀有的幾率提高", kMechanic),
            B("TowerRitualRareMonsters", "祭禮祭壇復活的怪物為魔法的幾率提高", kMechanic),
            B("TowerRitualChanceForNoCost", "重擲的恩惠有幾率不消耗貢品", kMechanic),
            B("TowerRitualAdditionalReroll", "祭禮祭壇允許額外重擲恩惠次數", kMechanic),
            B("TowerRitualDeferSpeed", "暫緩的恩惠更早再次出現", kMechanic),
            B("TowerRitualDeferCostIncrease", "暫緩恩惠消耗的貢品減少", kMechanic),
            B("TowerRitualRerollCostIncrease", "重擲恩惠消耗的貢品減少", kMechanic),
            B("TowerRitualTributeIncrease", "在祭禮祭壇獻祭的怪物提供更多貢品", kMechanic),
            B("UniqueRitualTributeCostIncrease", "祭禮祭壇的恩惠消耗更多貢品", kUnique),
            B("UniqueRitualUnlimitedRerolls", "可在祭禮祭壇重擲恩惠的次數翻倍", kUnique),
        };
    }
    if (typeKey == "Overseer") {
        return {
            B("TowerMapBossExperience", "地圖首領提供的經驗提高", kMechanic),
            B("TowerMapBossWaystoneChance", "地圖首領掉落的路徑石數量提高", kMechanic),
            B("TowerMapBossRarity", "地圖首領掉落的物品稀有度提高", kMechanic),
            B("TowerMapBossQuantity", "地圖首領掉落的物品數量提高", kMechanic),
            B("TowerMapBossAdditionalSpirit", "有強大地圖首領的區域包含額外阿茲莫里靈體", kMechanic),
            B("TowerMapBossAdditionalEssence", "有強大地圖首領的區域包含額外精華", kMechanic),
            B("TowerMapBossAdditionalShrine", "有強大地圖首領的區域包含額外神壇", kMechanic),
            B("TowerMapBossAdditionalStrongbox", "有強大地圖首領的區域包含額外保險箱", kMechanic),
            B("UniqueMapBossAdditionalModifier", "地圖首領有額外詞綴", kUnique),
            B("UniqueMapBossPossession", "地圖首領被阿茲莫里靈體獵殺", kUnique),
        };
    }
    if (typeKey == "Temple") {
        return {
            B("TowerIncursionPackSize", "瓦爾烽塔周遭的怪物群大小提高", kMechanic),
            B("TowerIncursionExtraPacksChance", "瓦爾烽塔周遭有幾率出現額外怪物群組", kMechanic),
            B("TowerIncursionExtraPacks", "瓦爾烽塔周遭有1個額外怪物群組", kMechanic),
            B("TowerIncursionSecondaryEncounters", "瓦爾烽塔召喚額外怪物的幾率提高", kMechanic),
            B("TowerIncursionTokenChance", "從瓦爾烽塔獲得額外水晶的幾率提高", kMechanic),
            B("TowerIncursionBossChance", "有幾率新增瓦爾烽塔傳奇怪物", kMechanic),
            B("TowerIncursionRareChestChance", "瓦爾烽塔寶箱為稀有的幾率提高", kMechanic),
        };
    }
    return {};
}

inline std::vector<Bonus> DistinctSorted(std::vector<Bonus> in) {
    std::vector<Bonus> out;
    std::unordered_set<std::string> seen;
    for (auto& b : in) {
        std::string idLow = ToLowerCopy(b.Id);
        if (!seen.insert(idLow).second) continue;
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const Bonus& a, const Bonus& b) {
        std::string ca = ToLowerCopy(a.Category), cb = ToLowerCopy(b.Category);
        if (ca != cb) return ca < cb;
        return ToLowerCopy(a.Label) < ToLowerCopy(b.Label);
    });
    return out;
}

inline const std::vector<Bonus>& GetBonusesForType(const std::string& typeKey) {
    static const std::vector<Bonus> kEmpty;
    static bool initialized = false;
    static std::unordered_map<std::string, std::vector<Bonus>> cat;
    
    if (!initialized) {
        auto initType = [&](const std::string& key) {
            std::vector<Bonus> list = SpecificBonuses(key);
            const auto& common = CommonBonuses();
            list.insert(list.end(), common.begin(), common.end());
            cat.emplace(key, DistinctSorted(std::move(list)));
        };
        initType("Irradiated");
        initType("Breach");
        initType("Expedition");
        initType("Delirium");
        initType("Abyss");
        initType("Ritual");
        initType("Overseer");
        initType("Temple");
        initialized = true;
    }
    
    auto it = cat.find(typeKey);
    return it != cat.end() ? it->second : kEmpty;
}

inline std::string ClassifyTabletType(const std::string& path,
                                       const std::string& baseType) {
    auto has = [&](const char* kw) {
        std::string kwl = kw;
        for (auto& c : kwl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string pathl = path;
        for (auto& c : pathl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string btl = baseType;
        for (auto& c : btl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return pathl.find(kwl) != std::string::npos || btl.find(kwl) != std::string::npos;
    };
    
    if (has("abyss")) return "Abyss";
    if (has("breach")) return "Breach";
    if (has("expedition")) return "Expedition";
    if (has("incursion") || has("temple") || has("vaal")) return "Temple";
    if (has("delirium")) return "Delirium";
    if (has("ritual")) return "Ritual";
    if (has("boss") || has("overseer") || has("mapboss")) return "Overseer";
    if (has("generic") || has("irradiated") || has("toweraugment")) return "Irradiated";
    return "";
}

inline const char* GetTypeDisplayName(const std::string& typeKey) {
    if (typeKey == "Irradiated")  return "輻照碑牌";
    if (typeKey == "Breach")      return "裂痕碑牌";
    if (typeKey == "Expedition")  return "探險碑牌";
    if (typeKey == "Delirium")    return "譫妄碑牌";
    if (typeKey == "Abyss")       return "深淵碑牌";
    if (typeKey == "Ritual")      return "祭祀碑牌";
    if (typeKey == "Overseer")    return "總督碑牌";
    if (typeKey == "Temple")      return "神廟碑牌";
    return "未知碑牌";
}

inline int GetSubCategoryIdForType(const std::string& typeKey) {
    if (typeKey == "Irradiated") return 101;
    if (typeKey == "Breach") return 102;
    if (typeKey == "Expedition") return 103;
    if (typeKey == "Delirium") return 104;
    if (typeKey == "Abyss") return 105;
    if (typeKey == "Ritual") return 106;
    if (typeKey == "Overseer") return 107;
    if (typeKey == "Temple") return 108;
    return 0;
}

inline std::string GetTypeKeyForSubCategory(int subId) {
    switch (subId) {
        case 101: return "Irradiated";
        case 102: return "Breach";
        case 103: return "Expedition";
        case 104: return "Delirium";
        case 105: return "Abyss";
        case 106: return "Ritual";
        case 107: return "Overseer";
        case 108: return "Temple";
        default: return "";
    }
}

}  // namespace detail

// —— 简易日志（用于调试输出）——
inline void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ============================================================
// 【方案 B v1.3】合规词缀 Id 读取（宪法修正案 v1.3）
// 只读 Mod.Id + Mod.Hash32，绝不读 Mod.Name/AffixName/StatKey
// ============================================================

// 运行时学习的 Hash32 缓存容量上限
inline constexpr size_t kMaxHashCacheSize = 10000;

// 正向白名单：从 TabletBonusCatalog 的所有 Bonus.Id 经 GetPoe2ModIdKeywords 派生
// 只承认白名单内的 Mod.Id，未知 Id 立即丢弃（不入内存持有）
inline const std::unordered_set<std::string>& GetWhitelistModIds() {
    static const auto s_set = [] {
        std::unordered_set<std::string> s;
        auto addBonus = [&](const Bonus& b) {
            s.insert(b.Id);
            s.insert(ToLowerCopy(b.Id));
            s.insert(b.NormId);
            if (!b.NormIdStripped.empty()) s.insert(b.NormIdStripped);
            for (const auto& kw : detail::GetPoe2ModIdKeywords(b.Id)) {
                s.insert(kw);
                s.insert(ToLowerCopy(kw));
            }
        };
        for (const auto& b : detail::CommonBonuses()) addBonus(b);
        for (const char* typeKey : {"Irradiated","Breach","Expedition","Delirium",
                                    "Abyss","Ritual","Overseer","Temple"}) {
            for (const auto& b : detail::GetBonusesForType(typeKey)) addBonus(b);
        }
        return s;
    }();
    return s_set;
}

// Hash32 正向缓存（Id 已确认匹配白名单时的 Hash32）
inline std::unordered_set<uint32_t>& GetKnownGoodHashes() {
    static std::unordered_set<uint32_t> s;
    return s;
}

// Hash32 负向缓存（Id 已确认不匹配白名单时的 Hash32）
inline std::unordered_set<uint32_t>& GetKnownBadHashes() {
    static std::unordered_set<uint32_t> s;
    return s;
}

// 清空 Hash32 缓存（仅用于 Mock 测试）
inline void ClearHashCachesForTest() {
    GetKnownGoodHashes().clear();
    GetKnownBadHashes().clear();
}

// 判断 mod.Id 是否匹配白名单（双向子串匹配，与 HasMatchingModifier 语义一致）
inline bool IsModIdWhitelisted(const std::string& modId) {
    if (modId.empty()) return false;
    const auto& whitelist = GetWhitelistModIds();
    std::string idLow = ToLowerCopy(modId);
    for (const auto& wl : whitelist) {
        if (wl.empty()) continue;
        // 双向子串匹配：mod.Id 包含白名单词，或白名单词包含 mod.Id
        if (idLow.find(wl) != std::string::npos) return true;
        if (wl.find(idLow) != std::string::npos) return true;
    }
    return false;
}

// ============================================================
// ExtractModIds：从 ItemMods 提取白名单内的 Mod.Id + Mod.Hash32
//
// 安全保证（宪法修正案 v1.3 绝对红线）：
//   - 只读 m.Id 和 m.Hash32 两个字段
//   - 绝不读 m.Name / m.AffixName / m.StatKey（任何路径下均禁止）
//   - 未知 Id 立即丢弃，不入 outModIds（仅其 Hash32 可入负向缓存）
//
// 性能优化：
//   - Hash32 快速预检：known_good → 直接保留；known_bad → 直接丢弃
//   - 未命中缓存 → Id 子串匹配白名单 → 命中则入 goodHashes，未命中入 badHashes
//
// 参数：
//   mods          - ReadItemMods 返回的 ItemMods
//   outModIds     - 输出：白名单内的 Mod.Id 列表（已过滤）
//   outModHashes  - 输出：与 outModIds 一一对应的 Hash32 列表
//   silentLog     - 静默测试模式时为 true，输出详细日志但不影响判定
//   itemPath      - 用于日志标识的物品 Path（可选）
// 返回：是否成功提取（false 表示 ItemMods 无效）
// ============================================================
inline bool ExtractModIds(const PluginSDK::ItemMods& mods,
                          std::vector<std::string>& outModIds,
                          std::vector<uint32_t>&    outModHashes,
                          bool silentLog = false,
                          const std::string& itemPath = "") {
    outModIds.clear();
    outModHashes.clear();
    if (!mods.Valid) return false;

    auto& goodHashes = GetKnownGoodHashes();
    auto& badHashes  = GetKnownBadHashes();
    const bool logThis = silentLog;

    if (logThis) {
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
            "[BonusMatch:Silent] Item path=%s mods_valid=1\n", itemPath.c_str());
        OutputDebugStringA(hdr);
    }

    size_t modIdx = 0;
    auto processModList = [&](const std::vector<PluginSDK::Mod>& modList) {
        for (const auto& m : modList) {
            const char* cacheState = "new";
            bool matched = false;

            // —— Hash32 快速预检 ——
            if (m.Hash32 != 0) {
                if (goodHashes.count(m.Hash32)) {
                    matched = true;
                    cacheState = "cached_good";
                } else if (badHashes.count(m.Hash32)) {
                    matched = false;
                    cacheState = "cached_bad";
                }
            }

            // —— 未命中缓存 → Id 精确匹配白名单 ——
            if (cacheState[0] == 'n') {  // "new"
                matched = IsModIdWhitelisted(m.Id);
            }

            // —— 缓存学习 + 输出 ——
            if (matched) {
                outModIds.push_back(m.Id);
                outModHashes.push_back(m.Hash32);
                if (m.Hash32 != 0 && cacheState[0] == 'n' &&
                    goodHashes.size() < kMaxHashCacheSize) {
                    goodHashes.insert(m.Hash32);
                }
            } else {
                if (m.Hash32 != 0 && cacheState[0] == 'n' &&
                    badHashes.size() < kMaxHashCacheSize) {
                    badHashes.insert(m.Hash32);
                }
                // 不匹配 → 不入 outModIds（"未知 Id 直接丢弃，不在内存持有"）
            }

            if (logThis) {
                char line[512];
                std::snprintf(line, sizeof(line),
                    "  Mod[%zu] Id='%s' Hash=0x%08X -> whitelist_match=%s (%s)\n",
                    modIdx, m.Id.c_str(), m.Hash32,
                    matched ? "YES" : "NO", cacheState);
                OutputDebugStringA(line);
            }
            ++modIdx;
        }
    };

    // 遍历 3 类词缀容器（不读 HellscapeMods/CrucibleMods，碑牌不会出现）
    processModList(mods.ImplicitMods);
    processModList(mods.ExplicitMods);
    processModList(mods.EnchantMods);

    if (logThis) {
        char tail[128];
        std::snprintf(tail, sizeof(tail),
            "  -> mods_extracted=%zu\n", outModIds.size());
        OutputDebugStringA(tail);
    }

    return true;
}

}  // namespace TabletReforgeGame