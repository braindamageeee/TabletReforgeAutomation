#pragma once

#include "TabletBonusCatalog.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace TabletReforgeGame {

struct ModRange {
    int min = 0;
    int max = 0;
    std::string unit;
};

struct RangeSeed { const char* id; int min; int max; const char* unit; };

inline const std::vector<RangeSeed>& BuiltInRanges() {
    static const std::vector<RangeSeed> v = {
        {"TowerDroppedItemRarityIncrease", 8, 12, "percent"},
        {"TowerMonsterEffectiveness", 10, 15, "percent"},
        {"TowerRareChestCount", 2, 3, "flat"},
        {"TowerExperienceGainIncrease", 12, 18, "percent"},
        {"TowerDroppedGoldIncrease", 25, 35, "percent"},
        {"TowerMonsterRarityIncrease", 15, 20, "percent"},
        {"TowerRarePackIncrease", 25, 35, "percent"},
        {"TowerMagicPackIncrease", 30, 40, "percent"},
        {"TowerPackSizeIncrease", 5, 7, "percent"},
        {"TowerMapDroppedMapsIncrease", 30, 40, "percent"},
        {"TowerRareMonsterSurpassing", 50, 80, "percent"},
        {"TowerMapAdditionalModifier", 1, 2, "flat"},
        {"TowerAdditionalAzmeriWisp", 1, 2, "flat"},
        {"TowerAdditionalEssence", 1, 2, "flat"},
        {"TowerAdditionalShrine", 1, 2, "flat"},
        {"TowerAdditionalStrongbox", 1, 2, "flat"},
        {"TowerAdditionalExileChance", 70, 100, "percent"},
        {"TowerAdditionalSpiritChance", 70, 100, "percent"},
        {"TowerAdditionalEssenceChance", 70, 100, "percent"},
        {"TowerAdditionalShrineChance", 70, 100, "percent"},
        {"TowerAdditionalStrongboxChance", 70, 100, "percent"},
        {"TowerStoneCircleChance", 70, 100, "percent"},

        {"TowerMapBossExperience", 40, 80, "percent"},
        {"TowerMapBossWaystoneChance", 18, 30, "percent"},
        {"TowerMapBossRarity", 35, 60, "percent"},
        {"TowerMapBossQuantity", 13, 20, "percent"},

        {"TowerBreachBossChance", 20, 50, "percent"},
        {"TowerBreachWombgiftLevelChance", 10, 30, "percent"},
        {"TowerBreachWombgiftQuantity", 30, 60, "percent"},
        {"TowerBreachHivebloodQuantity", 30, 60, "percent"},
        {"TowerBreachRareMonsterPotency", 5, 20, "percent"},
        {"TowerBreachMonsterQuantity", 5, 15, "percent"},
        {"TowerBreachAdditionalRares", 1, 3, "flat"},

        {"TowerDeliriumAdditionalShardsChance", 12, 26, "percent"},
        {"TowerDeliriumRareMonsterPause", 3, 5, "seconds"},
        {"TowerDeliriumDoodadsIncrease", 15, 30, "percent"},
        {"TowerDeliriumPackSizeIncrease", 15, 30, "percent"},
        {"TowerDeliriumDifficultyIncrease", 15, 30, "percent"},
        {"TowerDeliriumFogPersistence", 20, 30, "percent"},
        {"TowerDeliriumFogDissipationDelayNew", 6, 12, "seconds"},
        {"TowerDeliriumMonsterSplinterIncrease", 15, 30, "percent"},
        {"TowerDeliriumBossChance", 15, 30, "percent"},

        {"TowerAbyss4AdditionalChance", 20, 40, "percent"},
        {"TowerAbyssExtraTickets", 20, 30, "percent"},
        {"TowerAbyssExtraModifiers", 20, 30, "percent"},
        {"TowerAbyssDepthsChance", 10, 20, "percent"},
        {"TowerAbyssEffectivenessPerChasm", 8, 12, "percent"},
        {"TowerAbyssRareMonsterIncrease", 1, 2, "flat"},
        {"TowerAbyssMonsterIncrease", 20, 30, "percent"},

        {"TowerRitualOmenChance", 35, 70, "percent"},
        {"TowerRitualMagicMonsters", 25, 40, "percent"},
        {"TowerRitualRareMonsters", 35, 70, "percent"},
        {"TowerRitualChanceForNoCost", 3, 6, "percent"},
        {"TowerRitualAdditionalReroll", 1, 3, "flat"},
        {"TowerRitualDeferSpeed", 25, 40, "percent"},
        {"TowerRitualDeferCostIncrease", 20, 30, "percent"},
        {"TowerRitualRerollCostIncrease", 20, 30, "percent"},
        {"TowerRitualTributeIncrease", 18, 30, "percent"},

        {"TowerIncursionRareChestChance", 30, 60, "percent"},
        {"TowerIncursionBossChance", 10, 25, "percent"},
        {"TowerIncursionTokenChance", 5, 10, "percent"},
        {"TowerIncursionSecondaryEncounters", 25, 50, "percent"},
        {"TowerIncursionExtraPacksChance", 30, 60, "percent"},
        {"TowerIncursionPackSize", 10, 30, "percent"},

        {"UniqueBreachHiveAdditionalWaves", 2, 5, "flat"},
        {"UniqueBreachUnstableAdditionalRares", 2, 5, "flat"},
        {"UniqueTowerBreachDensityIncrease", -10, 20, "percent"},
        {"UniqueDeliriumDifficultyIncrease", -10, 10, "percent"},
        {"UniqueRitualTributeCostIncrease", 10, 15, "percent"},
    };
    return v;
}

inline const ModRange* FindRange(const std::string& normId) {
    static bool initialized = false;
    static std::unordered_map<std::string, ModRange> ranges;

    if (!initialized) {
        for (const auto& s : BuiltInRanges()) {
            std::string n = NormalizeIdentifier(s.id);
            if (n.empty()) continue;
            ModRange r{s.min, s.max, s.unit};
            ranges[n] = r;
            std::string stripped = StripTrailingDigits(n);
            if (!stripped.empty() && stripped != n) {
                ranges[stripped] = r;
            }
        }
        initialized = true;
    }

    auto it = ranges.find(normId);
    return it == ranges.end() ? nullptr : &it->second;
}

}  // namespace TabletReforgeGame
