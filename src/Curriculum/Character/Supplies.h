/*
 * This file is part of the Animus project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_CURRICULUM_SUPPLIES_H
#define ANIMUS_CURRICULUM_SUPPLIES_H

#include "Define.h"
#include <utility>
#include <vector>

class Player;

/*
 * What a class/role character carries beyond its kit and gear, as in training: food and drink for its level, and a
 * hunter's stable of beasts to call.
 */
namespace Animus::Curriculum
{
    /// Vendor-sold food (health regeneration) and drink (mana regeneration) by required level.
    class ConsumablePool
    {
    public:
        static ConsumablePool const& Instance();

        /// The best food / drink a character of `level` can use, or 0.
        [[nodiscard]] uint32 Food(uint8 level) const { return Best(_food, level); }
        [[nodiscard]] uint32 Drink(uint8 level) const { return Best(_drink, level); }

    private:
        ConsumablePool();

        [[nodiscard]] static uint32 Best(std::vector<std::pair<uint8, uint32>> const& items, uint8 level);

        std::vector<std::pair<uint8, uint32>> _food;     // (required level, item), sorted
        std::vector<std::pair<uint8, uint32>> _drink;
    };

    /// Tameable, non-exotic beasts spawned somewhere in the world, by family.
    class StablePool
    {
    public:
        static StablePool const& Instance();

        /// Beast entries of `count` different random families: a hunter's stable. Fewer if the world has fewer
        /// families.
        [[nodiscard]] std::vector<uint32> Random(uint32 count) const;

    private:
        StablePool();

        std::vector<std::vector<uint32>> _beastsByFamily;
    };

    /// Top the bot's food and drink up to LayoutConstants::CONSUMABLE_COUNT each; 0 skips one.
    void StockConsumables(Player* bot, uint32 food, uint32 drink);

    /// Hunters: bring the stabled beast `entry` out as the bot's pet (Call Pet for a pet that only exists in memory;
    /// Call Pet itself loads pets from the database). Needs level 10 and no pet out. Returns false if no pet was
    /// created.
    bool CallHunterBeast(Player* bot, uint32 entry);
}

#endif
