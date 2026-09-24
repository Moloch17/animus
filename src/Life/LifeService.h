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

#ifndef ANIMUS_LIFE_SERVICE_H
#define ANIMUS_LIFE_SERVICE_H

#include "ClassProfile.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <unordered_map>
#include <vector>

class Creature;
class Player;
class Quest;

namespace Animus::Curriculum
{
    struct WorldView;
}

/*
 * Life outside the fight for a companion on a live realm.
 *
 * The learned part is the WorldBlock the forge's life stages trained (stages 20-22 and the crossroads): a companion
 * whose model carries it reads the real world through the same features -- the nearest corpse, quest giver, node
 * and vendor, its own bags, gold and gear -- and its presses run through the same WorldActions the sim ran. Nothing
 * here decides when to loot, whom to talk to or what to put on: the model does.
 *
 * Scripted here is what is a lookup, or has no sim to learn it in: which quests to hold (the owner's), the
 * auction house (no sim tick ever runs one), collecting the mail the house sends, the flight path when the owner is
 * a zone away, the corpse run, and crafting what the companion knows from what it gathered. Each is a switch in
 * Animus.Life.*, and the whole service is off for a companion whose model has no world block.
 *
 * World thread only.
 */
namespace Animus::Life
{
    struct Settings
    {
        bool Enable = true;
        bool Quests = true;             // the owner's quests are the companion's too
        bool Auction = true;            // sell greens at the house, buy an upgrade under the budget
        uint32 AuctionBudgetCopper = 50 * 10000;    // 50 gold
        bool Mail = true;               // collect what the house sends
        bool Taxi = true;               // a flight path when the owner is far on the same map
        float TaxiBeyondYards = 1500.0f;
        bool CorpseRun = true;          // release and walk back to the corpse, rather than stand up in place
        bool Crafting = true;           // make what it knows from what it carries, when idle
        uint32 IdleSeconds = 20;        // out of combat this long before the house, the mail or a recipe
    };

    /// One companion the service acts for this update.
    struct Companion
    {
        Player* Bot = nullptr;
        Curriculum::StatProfile Stats = Curriculum::StatProfile::StrengthMelee;
        bool HasWorldBlock = false;     // its model carries the world block: the service works for it
        bool Quiet = false;             // the party has nothing to fight and the bot is out of combat
    };

    class LifeService
    {
    public:
        static LifeService* Instance();

        void Configure(Settings const& settings) { _settings = settings; }
        [[nodiscard]] Settings const& Current() const { return _settings; }
        /// A runtime switch (`.animus life <feature> on|off`): true when `feature` names one.
        bool Toggle(std::string_view feature, bool on);
        [[nodiscard]] std::vector<std::string> Status() const;

        /// The world block's view for a companion, from the real world: what WorldActions::Sense reads, plus the
        /// quest state from the companion's own log (the quest it holds that is furthest along).
        void Sense(Player* bot, Curriculum::WorldView& world) const;

        /// The owner took, abandoned or handed in a quest: the companions follow (the model does the objectives, the
        /// looting and the turn-in itself).
        void OnOwnerQuestAccept(Player* owner, Quest const* quest, std::vector<Player*> const& bots) const;
        void OnOwnerQuestAbandon(Player* owner, uint32 questId, std::vector<Player*> const& bots) const;

        /// Each update of a party: the mail, the house, a recipe when idle; a flight when the owner is far.
        void Update(uint32 diff, Player* owner, std::vector<Companion> const& companions);

        /// A dead companion's way back, when CorpseRun is on: release, walk to the corpse, reclaim. True while the
        /// service is handling it (the party's own stand-up then waits); false when it is not (CorpseRun off, no
        /// world block, or the corpse is somewhere it cannot walk to), and the party stands it up as before.
        bool RunToCorpse(uint32 diff, Player* bot, Player* owner, bool hasWorldBlock);

        /// A companion left: forget it.
        void Forget(ObjectGuid bot) { _bots.erase(bot); }

    private:
        struct BotState
        {
            uint32 IdleMs = 0;
            uint32 SinceMailMs = 0;
            uint32 SinceAuctionMs = 0;
            uint32 SinceCraftMs = 0;
            uint32 SinceTaxiMs = 0;
            uint32 GhostMs = 0;             // time since the corpse run began
            bool Released = false;
            uint32 Flights = 0;
            uint32 Listed = 0;
            uint32 Bought = 0;
            uint32 MailsTaken = 0;
            uint32 Crafted = 0;
        };

        BotState& StateOf(Player const* bot);
        void CollectMail(Player* bot, BotState& state) const;
        void Auction(Player* bot, Creature* auctioneer, Curriculum::StatProfile stats, BotState& state) const;
        bool Fly(Player* bot, Player* owner, BotState& state) const;
        void Craft(Player* bot, BotState& state) const;
        [[nodiscard]] static Creature* NearbyAuctioneer(Player* bot);

        Settings _settings;
        std::unordered_map<ObjectGuid, BotState> _bots;
    };
}

#define sLife Animus::Life::LifeService::Instance()

#endif
