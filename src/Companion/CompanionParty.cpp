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

#include "CompanionParty.h"
#include "Capture.h"
#include "OrderGoals.h"
#include "ActionCatalog.h"
#include "BotFactory.h"
#include "Chat.h"
#include "ClassAssets.h"
#include "CompanionGear.h"
#include "CompanionRegistry.h"
#include "CompanionTalents.h"
#include "DatabaseEnv.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "GoalBlock.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LifeService.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MlpPolicy.h"
#include "ModelLibrary.h"
#include "MotionMaster.h"
#include "MoveBlock.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PetBlock.h"
#include "PetTalents.h"
#include "Player.h"
#include "Random.h"
#include "SeatCharacter.h"
#include "SeatEncoder.h"
#include "TravelBlock.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>

namespace
{
    /// The least time between two cosmetic facing packets for one companion (CompanionParty::TurnShown).
    constexpr uint32 SHOWN_PACKET_MS = 100;

    using namespace Animus::Curriculum;

    /// A player character owns one companion. (The party keeps its seats: the model was trained with up to four.)
    constexpr std::size_t MAX_COMPANIONS = 1;

    /// Quiet this long between pulls and the next pull starts a new episode (the gauntlet's longest break).
    constexpr uint64 NEW_EPISODE_QUIET_MS = 20000;
    constexpr float PULL_TIME_SCALE_MS = 60000.0f;
    constexpr float COMBAT_TIME_SCALE_MS = 60000.0f;
    constexpr float QUIET_TIME_SCALE_MS = 20000.0f;
    /// A hidden target's time out of sight saturates here, as the forge's (StageScenario MAX_UNSEEN_TIME_MS).
    constexpr float MAX_UNSEEN_TIME_MS = 20000.0f;

    /// A breath, as the core times it (Player::HandleDrowning), which SeatView::BreathSpent is a fraction of.
    uint32 BreathMs()
    {
        return std::max<uint32>(1000, sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER));
    }

    /// Following the owner is the model's job (the companion block's follow press, re-aimed while it runs): nothing
    /// here leashes or teleports a companion that has fallen behind on the owner's map. Only a companion with no
    /// model to decide for it is walked behind the owner, from this far, since there is nothing else it can do.
    constexpr float FOLLOW_DISTANCE = 2.0f;
    constexpr float NO_MODEL_FOLLOW_DISTANCE = 6.0f;
    constexpr uint32 NO_MODEL_MOVE_POINT_ID = 5;

    /// A dead companion stands up again this long after the party is out of combat.
    constexpr uint32 RESURRECT_DELAY_MS = 10000;

    void MoveBehind(Player* bot, Player* owner)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        owner->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_DISTANCE,
            Position::NormalizeOrientation(owner->GetOrientation() + float(M_PI)));
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(NO_MODEL_MOVE_POINT_ID, x, y, z);
    }

    Player* FindBot(ObjectGuid guid)
    {
        // Connected rather than in world: a bot in the middle of a far teleport is still the party's.
        return ObjectAccessor::FindConnectedPlayer(guid);
    }
}

Animus::CompanionParty::CompanionParty(ObjectGuid owner) : _owner(owner)
{
}

Animus::CompanionParty::~CompanionParty() = default;

bool Animus::CompanionParty::Add(Player* owner, Layout const& layout, uint8 race, std::string const& name,
    uint32 account, std::string& message)
{
    if (Companions() >= MAX_COMPANIONS)
    {
        message = "You already have a companion.";
        return false;
    }

    ClassProfile const& profile = *layout.Profile;
    ClassAssets const& assets = *layout.Assets;

    // A class that starts above level 1 (death knights) starts there, whatever the owner's level.
    uint8 const level = std::max<uint8>(owner->GetLevel(), assets.Kit->MinLevel());

    BotFactory::BotSpec spec;
    spec.Name = name;
    spec.Race = race;
    spec.Class = profile.Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = account;

    Player* bot = BotFactory::Create(spec);
    if (!bot || !BotFactory::PlaceNear(bot, owner))
    {
        message = "The companion could not be created; see the server log.";
        return false;
    }

    // A spec of the class, as the forge's character generator draws one for a seat (StageScenario::BuildSeat)
    // when nothing is asked of it. The model is told what the character can do and what talents it has, never
    // which spec it drew.
    uint8 const specIndex = DrawSpec(ClassAssets::For(profile), AptitudeDemand::Anything());

    // As the forge builds a seat (StageScenario::BuildSeat, Configure). Talent points depend on the map for death
    // knights; the bot is on the owner's map now.
    bot->InitTalentForLevel();
    SeatCharacter::Configure(bot, layout, specIndex, false);
    // The core's autosave keeps it, as CompanionLoader sets a loaded one; Save writes it now.
    bot->SetSaveTimer(sWorld->getIntConfig(CONFIG_INTERVAL_SAVE));

    if (!Join(owner, bot, layout, specIndex, message))
    {
        BotFactory::Destroy(bot);
        return false;
    }

    LOG_INFO("module.animus", "{} created {} companion {} ({}), level {}, spec {}", owner->GetName(), profile.Name,
        bot->GetName(), bot->GetGUID().ToString(), level, profile.Specs[specIndex].Name);
    message = Acore::StringFormat("{}, a level {} {}, joins your party.", bot->GetName(), level, profile.Name);
    return true;
}

bool Animus::CompanionParty::AddFiller(Player* owner, Layout const& layout, uint8 race, std::string const& name,
    uint32 account, uint8 specIndex, std::string& message)
{
    ClassProfile const& profile = *layout.Profile;
    ClassAssets const& assets = *layout.Assets;

    BotFactory::BotSpec spec;
    spec.Name = name;
    spec.Race = race;
    spec.Class = profile.Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = std::max<uint8>(owner->GetLevel(), assets.Kit->MinLevel());
    spec.AccountId = account;

    Player* bot = BotFactory::Create(spec);
    if (!bot || !BotFactory::PlaceNear(bot, owner))
    {
        message = "A party member could not be created; see the server log.";
        return false;
    }

    // Built as a companion is (Add), in the spec its role asked for, and never given a save timer: nothing of it
    // reaches the database.
    bot->InitTalentForLevel();
    SeatCharacter::Configure(bot, layout, specIndex, false);
    if (!Join(owner, bot, layout, specIndex, message))
    {
        BotFactory::Destroy(bot);
        return false;
    }

    _members.back()->Temporary = true;
    LOG_INFO("module.animus", "{}'s party filled with {} ({} {}), level {}", owner->GetName(), bot->GetName(),
        profile.Specs[specIndex].Name, profile.Name, bot->GetLevel());
    return true;
}

bool Animus::CompanionParty::Attach(Player* owner, Player* bot, Layout const& layout, Record const& record,
    std::string& message)
{
    if (Companions() >= MAX_COMPANIONS)
    {
        message = "You already have a companion.";
        return false;
    }

    // A group it was saved in (the row survives a crash) that is not the owner's: left, so it can join theirs.
    if (Group* old = bot->GetGroup(); old && old != owner->GetGroup())
        bot->RemoveFromGroup();

    if (!BotFactory::PlaceNear(bot, owner))
    {
        message = "The companion could not be placed beside you; see the server log.";
        return false;
    }

    if (!Join(owner, bot, layout, record.Spec, message))
        return false;

    Member& member = *_members.back();
    member.Edited = record.Edited;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (record.OwnerGear & (1u << slot))
            member.OwnerGear.push_back(slot);

    LOG_INFO("module.animus", "{} summoned companion {} ({}), level {}", owner->GetName(), bot->GetName(),
        bot->GetGUID().ToString(), bot->GetLevel());
    message = Acore::StringFormat("{} joins your party.", bot->GetName());
    return true;
}

bool Animus::CompanionParty::Join(Player* owner, Player* bot, Layout const& layout, uint8 spec,
    std::string& message)
{
    ClassProfile const& profile = *layout.Profile;

    Group* group = owner->GetGroup();
    if (group && !group->IsLeader(owner->GetGUID()) && bot->GetGroup() != group)
    {
        message = "Only your group's leader can add a companion to it.";
        return false;
    }

    if (group && group->IsFull() && bot->GetGroup() != group)
    {
        message = "Your group is full.";
        return false;
    }

    auto member = std::make_unique<Member>();
    member->Bot = bot->GetGUID();
    member->Name = bot->GetName();
    member->L = &layout;
    member->Race = bot->getRace();
    member->Level = uint8(bot->GetLevel());
    member->Spec = spec;

    // Read what it can do now that it is the character it is -- talents spent, gear on, spellbook final -- exactly
    // where StageScenario::Configure reads it, and before anything is asked of it.
    RefreshBuild(*member, bot);
    member->Stable = SeatCharacter::PrepareFighter(bot, layout, member->Apt);

    member->Obs.resize(layout.ObsDim);
    member->Mask.resize(layout.NumActions);
    member->Memory.Reset(layout.NumActions);

    bool const newGroup = !group;
    if (newGroup)
    {
        group = new Group();
        if (!group->Create(owner))
        {
            delete group;
            message = "Could not create a group for your companion; see the server log.";
            return false;
        }

        sGroupMgr->AddGroup(group);
    }

    if (bot->GetGroup() != group && !group->AddMember(bot))
    {
        if (newGroup)
            group->Disband();
        message = "The companion could not join your group; see the server log.";
        return false;
    }

    // Supplies after joining: a warlock in the group hands out healthstones.
    Restock(*member, bot, owner);
    member->LastPower = bot->GetPower(bot->getPowerType());
    _members.push_back(std::move(member));
    return true;
}

void Animus::CompanionParty::Save(Record& record)
{
    for (std::unique_ptr<Member> const& member : _members)
    {
        Player* bot = FindBot(member->Bot);
        if (!bot || member->Temporary)
            continue;

        // The save's statements exist on the asynchronous connection only (a direct commit asserts on the first
        // of them), so it goes through the worker, whose single queue also carries a load that follows it.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bot->SaveToDB(trans, false, false);
        CharacterDatabase.CommitTransaction(trans);
        CompanionRegistry::Purge(bot->GetGUID());

        record.Spec = member->Spec;
        record.Edited = member->Edited;
        record.OwnerGear = 0;
        for (uint8 slot : member->OwnerGear)
            record.OwnerGear |= 1u << slot;
    }
}

void Animus::CompanionParty::Restock(Member& member, Player* bot, Player* owner) const
{
    bool warlockInParty = owner && owner->getClass() == CLASS_WARLOCK;
    for (std::unique_ptr<Member> const& other : _members)
        if (other->L->Profile->Class == CLASS_WARLOCK)
            warlockInParty = true;

    ClassProfile const& profile = *member.L->Profile;
    ConsumablePool const& pool = ConsumablePool::Instance();
    member.Supplies = pool.Supplies(member.Level, bot->GetMaxPower(POWER_MANA) > 0, profile.Class == CLASS_WARLOCK,
        warlockInParty || profile.Class == CLASS_WARLOCK);
    StockBattleSupplies(bot, member.Supplies, profile.Specs[member.Spec].Stats);

    if (member.L->Has(BlockId::Gauntlet))
    {
        member.FoodItem = pool.Food(member.Level);
        member.DrinkItem = bot->GetMaxPower(POWER_MANA) ? pool.Drink(member.Level) : 0;
        StockConsumables(bot, member.FoodItem, member.DrinkItem);
    }
}

Animus::CompanionParty::Status Animus::CompanionParty::Update(uint32 diff, Settings const& settings,
    ModelLibrary& models)
{
    _nowMs += diff;

    Player* owner = ObjectAccessor::FindConnectedPlayer(_owner);
    if (!owner)
        return Status::Dismiss;

    // Bots only leave through DestroyAll; anything else took one away. It leaves the group too, which would otherwise
    // keep it as an offline member.
    std::erase_if(_members, [owner](std::unique_ptr<Member> const& member)
    {
        if (FindBot(member->Bot))
            return false;

        LOG_INFO("module.animus", "Companion {} ({}) is gone", member->Name, member->Bot.ToString());
        if (Group* group = owner->GetGroup(); group && group->IsMember(member->Bot))
            group->RemoveMember(member->Bot);
        return true;
    });

    // A teleport a bot started itself (its transport changing maps, a summoning spell) completes as its client would
    // acknowledge it, whatever the owner is doing. A parked bot's stays pending until the owner is back.
    for (std::unique_ptr<Member> const& member : _members)
        if (!member->Parked)
            BotFactory::CompleteTeleport(FindBot(member->Bot));

    if (!owner->IsInWorld() || owner->IsBeingTeleported())
        return Status::Active;

    // A flight path, or a vehicle (a quest's bombing run): no companion can come along, so they leave the world and
    // come back beside the owner once it is off -- wherever that is.
    if (BotFactory::IsAway(owner))
    {
        for (std::unique_ptr<Member> const& member : _members)
        {
            if (member->Parked || !BotFactory::Park(FindBot(member->Bot)))
                continue;

            member->Parked = true;
            member->SinceDecisionMs = 0;
            member->InCombat = false;
        }
        return Status::Active;
    }

    for (std::unique_ptr<Member> const& member : _members)
    {
        if (!member->Parked || !BotFactory::CanJoin(owner) || !BotFactory::TeleportNear(FindBot(member->Bot), owner))
            continue;

        member->Parked = false;
        member->StepDamage.store(0, std::memory_order_relaxed);
        member->StepDamageTaken.store(0, std::memory_order_relaxed);
    }

    std::vector<Player*> present;
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot && bot->IsInWorld() && bot->GetMap() == owner->GetMap())
            present.push_back(bot);

    UpdatePull(owner, present);
    Direct(owner, settings, models, diff);

    for (std::unique_ptr<Member> const& member : _members)
        UpdateMember(*member, FindBot(member->Bot), owner, diff, settings, models);

    // Life outside the fight, for the companions whose model carries the world block: the mail, the house, a
    // recipe when idle, a flight when the owner is far (LifeService). The block's own presses run in Decide.
    std::vector<Life::Companion> companions;
    for (std::unique_ptr<Member> const& member : _members)
    {
        Player* bot = FindBot(member->Bot);
        if (!bot || !bot->IsInWorld() || member->Parked)
            continue;
        companions.push_back({ bot, member->L ? member->L->Profile->Specs[member->Spec].Stats
            : Curriculum::StatProfile::StrengthMelee, member->L && member->L->Has(BlockId::World),
            _enemies.empty() && !bot->IsInCombat() });
    }
    sLife->Update(diff, owner, companions);

    return Status::Active;
}

std::vector<Player*> Animus::CompanionParty::PresentBots() const
{
    std::vector<Player*> bots;
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot && bot->IsInWorld())
            bots.push_back(bot);
    return bots;
}

void Animus::CompanionParty::UpdatePull(Player* owner, std::vector<Player*> const& bots)
{
    Map* map = owner->GetMap();

    _enemyUnits.fill(nullptr);
    for (uint32 slot = 0; slot < _enemies.size(); ++slot)
        if (Unit* enemy = Curriculum::Encoding::UnitThrough(*owner, _enemies[slot]); enemy && enemy->IsInWorld()
            && enemy->GetMap() == map)
            _enemyUnits[slot] = enemy;

    // Everything fighting the party: attackers of the owner, the companions and their pets, and their victims.
    std::vector<Unit*> fighting;
    auto const consider = [&](Unit* unit)
    {
        if (unit && unit->IsAlive() && unit->IsInWorld() && unit->GetMap() == map && owner->IsValidAttackTarget(unit)
            && std::find(fighting.begin(), fighting.end(), unit) == fighting.end())
            fighting.push_back(unit);
    };

    auto const collect = [&](Unit* member)
    {
        for (Unit* attacker : member->getAttackers())
            consider(attacker);
        consider(member->GetVictim());
        for (Unit* controlled : member->m_Controlled)
            for (Unit* attacker : controlled->getAttackers())
                consider(attacker);
    };

    if (owner->IsAlive())
        collect(owner);
    for (Player* bot : bots)
        if (bot->IsAlive())
            collect(bot);

    // New enemies take free slots, then slots whose enemy is dead or gone.
    for (Unit* enemy : fighting)
    {
        if (std::find(_enemies.begin(), _enemies.end(), enemy->GetGUID()) != _enemies.end())
            continue;

        uint32 slot = uint32(_enemies.size());
        if (slot >= PACK_SLOTS)
        {
            slot = 0;
            while (slot < PACK_SLOTS && _enemyUnits[slot] && _enemyUnits[slot]->IsAlive())
                ++slot;
            if (slot == PACK_SLOTS)
                continue;
        }
        else
        {
            if (_enemies.empty())
            {
                if (!_episodeStarted || _nowMs - _quietSinceMs >= NEW_EPISODE_QUIET_MS)
                    StartEpisode(owner);
                _pullStartMs = _nowMs;
            }

            _enemies.emplace_back();
        }

        _enemies[slot] = enemy->GetGUID();
        _enemyUnits[slot] = enemy;
    }

    if (_enemies.empty())
        return;

    // The pull is over once no enemy in it is alive and still fighting (dead, gone, or evaded back home).
    bool const active = std::any_of(_enemyUnits.begin(), _enemyUnits.end(), [&](Unit* enemy)
    {
        return enemy && enemy->IsAlive()
            && (enemy->IsInCombat() || std::find(fighting.begin(), fighting.end(), enemy) != fighting.end());
    });

    if (active)
        return;

    ++_pullsCleared;
    _enemies.clear();
    _enemyUnits.fill(nullptr);
    _quietSinceMs = _nowMs;
    _foughtBefore = true;
    for (std::unique_ptr<Member> const& member : _members)
        member->TargetSlot = 0;
}

uint32 Animus::CompanionParty::DirectedSlot(ObjectGuid bot) const
{
    for (uint32 slot = 0; slot < _directed.size(); ++slot)
        if (_directed[slot] == bot)
            return slot;
    return Curriculum::DirectorLayout::DIRECTOR_SEATS;
}

void Animus::CompanionParty::Direct(Player* owner, Settings const& settings, ModelLibrary& models, uint32 diff)
{
    namespace DL = Curriculum::DirectorLayout;
    _sinceDirectorMs += diff;
    if (_sinceDirectorMs < settings.DecisionMs)
        return;
    _sinceDirectorMs %= settings.DecisionMs;

    // Who it commands: the companions in the world whose layouts read orders -- two or more, or there is no group.
    std::array<Unit*, DL::DIRECTOR_SEATS> members{};
    std::array<Member const*, DL::DIRECTOR_SEATS> of{};
    uint32 own = 0;
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot && bot->IsInWorld() && !member->Parked && member->L
            && member->L->Has(Curriculum::BlockId::Order) && own < members.size())
        {
            of[own] = member.get();
            members[own++] = bot;
        }
    std::string error;
    MlpPolicy* policy = settings.DirectorLayout && own >= 2 ? models.Find(*settings.DirectorLayout, error) : nullptr;
    if (!policy)
    {
        _directed.clear();
        _orders = Curriculum::DirectorOrders();
        return;
    }
    _directed.assign(own, ObjectGuid::Empty);
    for (uint32 slot = 0; slot < own; ++slot)
        _directed[slot] = members[slot]->GetGUID();

    std::array<Unit*, Curriculum::PACK_SLOTS> enemies{};
    uint32 const count = std::min<uint32>(uint32(_enemies.size()), Curriculum::PACK_SLOTS);
    for (uint32 slot = 0; slot < count; ++slot)
        enemies[slot] = Curriculum::Encoding::UnitThrough(*owner, _enemies[slot]);

    // Orders to the dead, or about the dead, end.
    auto const alive = [&](ObjectGuid guid)
    {
        Unit const* unit = guid ? Curriculum::Encoding::UnitThrough(*owner, guid) : nullptr;
        return unit && unit->IsAlive();
    };
    if (_orders.Focus && !alive(_orders.Focus))
    {
        _orders.Focus.Clear();
        _orders.Changed(_directorSteps);
    }
    for (uint32 slot = 0; slot < own; ++slot)
    {
        Curriculum::DirectorOrders::MemberOrder& order = _orders.Members[slot];
        if (order.Kind != Curriculum::OrderKind::None && (!members[slot]->IsAlive()
            || (order.Target && !alive(order.Target))))
            order = Curriculum::DirectorOrders::MemberOrder();
    }

    Curriculum::DirectorRules::PrepareTurn(_orders, _directorSteps, members.data(), own, enemies.data(), count,
        settings.Director.ClockDecisions, settings.Director.LowHealth);

    // What it sees: the party and its enemies, from the world as it is (a companion's director sees what the party
    // sees; there is no fog to remember through).
    DL::DirectorView view;
    view.Active = true;
    view.MayCall = _orders.CallsLeft > 0;
    view.CallsLeft = _orders.CallsLeft;
    view.ByEvent = _orders.ByEvent;
    view.Raid = own > Curriculum::GROUP_SEATS;
    view.Groups = (own + Curriculum::GROUP_SEATS - 1) / Curriculum::GROUP_SEATS;
    view.EpisodeTime = _enemies.empty() ? 0.0f : std::min(1.0f, float(_nowMs - _pullStartMs) / 300000.0f);
    view.Posture = _orders.Posture;
    view.Rally = _orders.Rally;
    view.HasFocus = bool(_orders.Focus);
    view.Anchor = _orders.Anchor;
    view.Offset = _orders.Offset;
    view.Ring = _orders.Ring;
    view.PlacesAllowed = false;             // the module resolves no places
    view.Address = _orders.Address;
    view.AddressGroup = _orders.AddressGroup;
    view.SinceCall = std::min(1.0f, float(_directorSteps - std::min(_directorSteps, _orders.CalledStep))
        / DL::CALL_AGE_SCALE);

    float centreX = 0.0f, centreY = 0.0f, health = 0.0f;
    uint32 standing = 0;
    for (uint32 slot = 0; slot < own; ++slot)
        if (members[slot]->IsAlive())
        {
            centreX += members[slot]->GetPositionX();
            centreY += members[slot]->GetPositionY();
            ++standing;
        }
    if (standing)
    {
        centreX /= float(standing);
        centreY /= float(standing);
    }
    float enemyX = 0.0f, enemyY = 0.0f;
    uint32 seen = 0;
    for (uint32 slot = 0; slot < count; ++slot)
        if (enemies[slot])
        {
            enemyX += enemies[slot]->GetPositionX();
            enemyY += enemies[slot]->GetPositionY();
            ++seen;
        }
    float const axis = seen ? std::atan2(enemyY / float(seen) - centreY, enemyX / float(seen) - centreX) : 0.0f;
    auto const place = [&](Unit const* unit, float& spread, float& sine, float& cosine)
    {
        float const dx = unit->GetPositionX() - centreX, dy = unit->GetPositionY() - centreY;
        spread = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / DL::DISTANCE_SCALE);
        float const angle = std::atan2(dy, dx) - axis;
        sine = std::sin(angle);
        cosine = std::cos(angle);
    };
    Unit const* focus = _orders.Focus ? Curriculum::Encoding::UnitThrough(*owner, _orders.Focus) : nullptr;

    view.SeatCount = own;
    for (uint32 slot = 0; slot < own; ++slot)
    {
        DL::DirectorView::SeatSlot& out = view.Seats[slot];
        Player* bot = members[slot]->ToPlayer();
        out.Present = true;
        out.Alive = bot->IsAlive();
        out.Health = bot->GetHealthPct() / 100.0f;
        Powers const power = bot->getPowerType();
        out.Power = bot->GetMaxPower(power) ? float(bot->GetPower(power)) / float(bot->GetMaxPower(power)) : 0.0f;
        out.Apt = of[slot]->Apt;
        out.InCombat = bot->IsInCombat();
        out.Casting = bot->IsNonMeleeSpellCast(false, false, true);
        place(bot, out.Spread, out.BearingSin, out.BearingCos);
        out.Group = slot / Curriculum::GROUP_SEATS;
        out.Addressed = _orders.Address != Curriculum::OrderSource::Side
            && Curriculum::DirectorRules::Addressed(_orders, slot);
        uint32 attackers = 0;
        for (uint32 e = 0; e < count; ++e)
            attackers += enemies[e] && enemies[e]->IsAlive() && enemies[e]->GetVictim() == bot ? 1 : 0;
        out.Attacked = float(attackers) / float(Curriculum::PACK_SLOTS);
        out.Order = _orders.Members[slot].Kind;
        out.OrderAge = out.Order == Curriculum::OrderKind::None ? 0.0f : std::min(1.0f,
            float(_directorSteps - std::min(_directorSteps, _orders.Members[slot].IssuedStep)) / DL::CALL_AGE_SCALE);
        if (focus)
        {
            out.ToFocus = std::min(1.0f, bot->GetExactDist2d(focus) / DL::DISTANCE_SCALE);
            out.OnFocus = bot->GetVictim() == focus;
        }
        if (out.Alive)
            health += out.Health;
    }
    view.OwnStanding = float(standing) / float(own);
    view.OwnHealth = standing ? health / float(standing) : 0.0f;

    std::array<ObjectGuid, Curriculum::PACK_SLOTS> callable{};
    view.EnemyCount = count;
    float enemyHealth = 0.0f;
    uint32 enemyStanding = 0;
    for (uint32 slot = 0; slot < count; ++slot)
    {
        Unit* enemy = enemies[slot];
        DL::DirectorView::EnemySlot& out = view.Enemies[slot];
        if (!enemy)
            continue;
        out.Present = true;
        out.Seen = true;
        out.Alive = enemy->IsAlive();
        out.Health = enemy->GetHealthPct() / 100.0f;
        out.InCombat = enemy->IsInCombat();
        out.Casting = enemy->IsNonMeleeSpellCast(false, false, true);
        out.IsFocus = enemy->GetGUID() == _orders.Focus;
        place(enemy, out.Spread, out.BearingSin, out.BearingCos);
        uint32 ordered = 0;
        for (uint32 m = 0; m < own; ++m)
        {
            out.OnSeat = out.OnSeat || enemy->GetVictim() == members[m];
            ordered += _orders.Members[m].Kind != Curriculum::OrderKind::None
                && _orders.Members[m].Target == enemy->GetGUID() ? 1 : 0;
        }
        out.Ordered = std::min(1.0f, float(ordered) / float(Curriculum::GROUP_SEATS));
        if (out.Alive)
        {
            callable[slot] = enemy->GetGUID();
            enemyHealth += out.Health;
            ++enemyStanding;
        }
    }
    view.EnemyStanding = count ? float(enemyStanding) / float(count) : 0.0f;
    view.EnemyHealth = enemyStanding ? enemyHealth / float(enemyStanding) : 0.0f;

    // Every decision, so its memory runs as a forge director's does; its call counts only on its turn.
    _directorObs.assign(DL::OBS_COUNT, 0.0f);
    _directorMask.assign(DL::ACTION_COUNT, 0);
    DL::Observe(view, _directorObs.data(), _directorMask.data());
    int32 const action = policy->Decide(_directorObs.data(), _directorMask.data(), &_directorState);
    Curriculum::DirectorRules::Apply(_orders, _directorSteps, action, members.data(), own, callable.data(), count,
        true);
    ++_directorSteps;
}

void Animus::CompanionParty::StartEpisode(Player* owner)
{
    _episodeStarted = true;
    // A new fight: the director starts with nothing said and nothing remembered, as a forge episode does.
    _orders = Curriculum::DirectorOrders();
    _directorState.Clear();
    _directorSteps = 0;
    _episodeStartMs = _nowMs;
    _pullsCleared = 0;

    // Training starts every episode with full bags.
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot && bot->IsInWorld() && bot->IsAlive())
            Restock(*member, bot, owner);
}

void Animus::CompanionParty::TurnShown(Member& member, Player* bot, uint32 diff, Settings const& settings)
{
    // Cosmetic only: a decision turns a standing seat up to 45 degrees in one step (MoveBlock::TURN_RATE), and a
    // standing turn reached clients only with the next run, as a snap. Clients are shown the turn swung across
    // the world ticks at the same rate instead. The seat's own orientation -- what casts, facing checks and the
    // next observation read -- is the decided one throughout, so play still matches training.
    float const decided = bot->GetOrientation();
    if (!member.ShownSeeded || !bot->IsAlive() || !bot->movespline->Finalized())
    {
        member.ShownFacing = decided;
        member.ShownSeeded = true;
        return;
    }

    float const left = Position::NormalizeOrientation(decided - member.ShownFacing + float(M_PI)) - float(M_PI);
    if (std::fabs(left) < 0.01f)
    {
        member.ShownSinceMs = 0;
        return;             // not turning: nothing is sent
    }

    float const step = Curriculum::MoveBlock::TURN_RATE * float(diff) / float(std::max<uint32>(1, settings.DecisionMs));
    member.ShownFacing = Position::NormalizeOrientation(member.ShownFacing + std::clamp(left, -step, step));

    // A few packets a decision at most, for realms with hundreds of companions: one every SHOWN_PACKET_MS while a
    // turn is under way, and the last when it arrives.
    member.ShownSinceMs += diff;
    bool const arrived = std::fabs(left) <= step;
    if (!arrived && member.ShownSinceMs < SHOWN_PACKET_MS)
        return;
    member.ShownSinceMs = 0;

    // The shown angle goes out in an ordinary facing packet; the unit's orientation is put back at once.
    bot->SetOrientation(member.ShownFacing);
    WorldPacket data(MSG_MOVE_SET_FACING, 64);
    data << bot->GetPackGUID();
    bot->BuildMovementPacket(&data);
    bot->SetOrientation(decided);
    bot->SendMessageToSet(&data, false);
}

void Animus::CompanionParty::UpdateMember(Member& member, Player* bot, Player* owner, uint32 diff,
    Settings const& settings, ModelLibrary& models)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    // Through the owner's loading screens: onto the owner's map or into its instance, whatever the fight. A
    // battleground or arena only takes queued players, so there the companions wait where they are. The same for
    // the owner's transport: on when the owner boards, off when the owner steps off, so no ship leaves one behind.
    if (bot->GetMap() != owner->GetMap() || bot->GetTransport() != owner->GetTransport())
    {
        if (BotFactory::CanJoin(owner))
            BotFactory::TeleportNear(bot, owner);
        return;
    }

    TurnShown(member, bot, diff, settings);

    bool const quiet = _enemies.empty() && !bot->IsInCombat();

    if (!bot->IsAlive() && member.L && member.L->Has(BlockId::Death)
        && !bot->GetMap()->IsBattlegroundOrArena())
    {
        // A model with the death block plays its own death, as it trained to (DeathBlock): release, run back and
        // rise, take the spirit healer, or accept a friend's resurrection.
        member.DeadMs += diff;
        std::string error;
        MlpPolicy* policy = models.Find(*member.L, error);
        member.SinceDecisionMs += diff;
        if (policy && member.SinceDecisionMs >= settings.DecisionMs)
        {
            member.SinceDecisionMs %= settings.DecisionMs;
            Decide(member, bot, owner, *policy, settings);
        }
        if (bot->IsAlive())
        {
            member.DeadMs = 0;
            member.StepDamage.store(0, std::memory_order_relaxed);
            member.StepDamageTaken.store(0, std::memory_order_relaxed);
        }
        return;
    }

    if (!bot->IsAlive())
    {
        // A resurrection another companion cast is accepted, as a client does.
        if (bot->isResurrectRequested())
        {
            bot->ResurectUsingRequestData();
            bot->clearResurrectRequestData();
            member.DeadMs = 0;
            return;
        }

        member.DeadMs += diff;
        // A companion with the world block walks back to its corpse as a player does (LifeService::RunToCorpse,
        // Animus.Life.CorpseRun); the rest stand up in place as before.
        if (sLife->RunToCorpse(diff, bot, owner, member.L && member.L->Has(BlockId::World)))
        {
            if (bot->IsAlive())
            {
                member.DeadMs = 0;
                member.SinceDecisionMs = 0;
                member.StepDamage.store(0, std::memory_order_relaxed);
                member.StepDamageTaken.store(0, std::memory_order_relaxed);
            }
            return;
        }
        if (quiet && owner->IsAlive() && !owner->IsInCombat() && member.DeadMs >= RESURRECT_DELAY_MS)
        {
            bot->ResurrectPlayer(0.5f);
            member.DeadMs = 0;
            member.SinceDecisionMs = 0;
            member.StepDamage.store(0, std::memory_order_relaxed);
            member.StepDamageTaken.store(0, std::memory_order_relaxed);
        }
        return;
    }

    member.DeadMs = 0;

    // On a flight path (LifeService::Fly) nothing is decided: the client would show a taxi ride.
    if (bot->IsInFlight())
        return;

    // The owner levelled: the companion follows between pulls, as a new character of that level.
    if (quiet && LevelFor(member, owner) > member.Level)
        LevelUp(member, bot, owner);

    std::string error;
    MlpPolicy* policy = models.Find(*member.L, error);
    if (!policy)
    {
        if (!member.ModelErrorLogged)
        {
            LOG_ERROR("module.animus", "Companion {} has no model and only follows: {}", member.Name, error);
            member.ModelErrorLogged = true;
        }

        if (quiet && bot->GetDistance(owner) > NO_MODEL_FOLLOW_DISTANCE && bot->movespline->Finalized())
            MoveBehind(bot, owner);
        return;
    }

    member.ModelErrorLogged = false;

    member.SinceDecisionMs += diff;
    if (member.SinceDecisionMs < settings.DecisionMs)
        return;

    member.SinceDecisionMs %= settings.DecisionMs;
    Decide(member, bot, owner, *policy, settings);
}

void Animus::CompanionParty::ObserveGoalSignals(Member& member, Curriculum::SeatView& view, Player* bot,
    Player* owner)
{
    // As a forge seat's (StageScenario::ObserveGoalSignals): what it achieved since the last decision whatever it
    // pursued -- an enemy in one of its slots killed, else its health and mana back past Recover's line -- and
    // the event that makes the model choose again now: newly below the escape line, more enemies in the fight,
    // the owner newly attacked.
    std::array<uint8, Curriculum::PACK_SLOTS> alive{};
    uint32 enemies = 0;
    for (uint32 slot = 0; slot < view.EnemyCount && slot < Curriculum::PACK_SLOTS; ++slot)
        if (Unit const* enemy = view.Enemies[slot]; enemy && enemy->IsAlive())
        {
            alive[slot] = 1;
            if (enemy->IsInCombat())
                ++enemies;
        }
    view.Achieved = Curriculum::NO_GOAL;
    for (uint32 slot = 0; slot < Curriculum::PACK_SLOTS && view.Achieved == Curriculum::NO_GOAL; ++slot)
        if (member.EnemySeenAlive[slot] && !alive[slot])
            view.Achieved = Curriculum::MakeGoal(Curriculum::SeatGoal::Fight,
                Curriculum::GOAL_TARGET_ENEMY_FIRST + slot);
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    float const resource = std::min(bot->GetHealthPct() / 100.0f,
        maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f);
    bool const below = resource < 0.8f;
    if (view.Achieved == Curriculum::NO_GOAL && member.BelowRecover && !below && !bot->IsInCombat())
        view.Achieved = Curriculum::MakeGoal(Curriculum::SeatGoal::Recover, Curriculum::GOAL_TARGET_NONE);
    member.EnemySeenAlive = alive;
    member.BelowRecover = below;

    bool const low = bot->GetHealthPct() < 35.0f;
    bool const ownerAttacked = owner && owner->IsAlive() && !owner->getAttackers().empty();
    view.GoalEvent = (low && !member.EventLow) || enemies > member.EventEnemies
        || (ownerAttacked && !member.EventOwnerAttacked);
    member.EventLow = low;
    member.EventEnemies = enemies;
    member.EventOwnerAttacked = ownerAttacked;
}

void Animus::CompanionParty::Decide(Member& member, Player* bot, Player* owner, MlpPolicy& policy,
    Settings const& settings)
{
    // Where it is at every decision, in the players' movement format, so its motion compares with theirs.
    Capture::CompanionSample(bot);

    bool const inCombat = bot->IsInCombat();
    if (inCombat && !member.InCombat)
    {
        member.CombatStartMs = _nowMs;

        // A fight is its own episode as far as the model is concerned: it starts with nothing remembered, as the
        // seats it trained as do.
        member.Policy.Clear();
    }
    member.InCombat = inCombat;

    // A pet it summoned starts defensive, as a forge seat's does.
    Curriculum::PetBlock::DefaultStance(Curriculum::PetBlock::FindPet(bot), member.LastPetGuid);

    // What the forge's reward step records for the next observation.
    member.LastStepDamage = float(member.StepDamage.exchange(0, std::memory_order_relaxed)) / DamageScale(member.Level);
    member.LastStepDamageTaken = float(member.StepDamageTaken.exchange(0, std::memory_order_relaxed))
        / float(std::max<uint32>(1, bot->GetMaxHealth()));

    Powers const power = bot->getPowerType();
    uint32 const current = bot->GetPower(power);
    member.LastStepPowerDelta = (float(current) - float(member.LastPower))
        / float(std::max<uint32>(1, bot->GetMaxPower(power)));
    member.LastPower = current;

    Unit* target = CurrentTarget(member, bot);
    Track(member, bot, owner, target, settings);
    member.Memory.Observe(bot, target, _nowMs);
    SeatView view = View(member, bot, owner, target, settings);
    // The durative action it is running: one press that stands for many decisions (rest, hold an interrupt, keep
    // range). SeatEncoder starts, runs and stops it as it does for a forge seat.
    view.Option = &member.Option;
    // Under its director, a member or group order is its primary goal (OrderGoals.h), as a forge seat's is under a
    // learned director; the model reads it from the goal block and holds it.
    member.OrderGoal = Curriculum::NO_GOAL;
    if (view.Order.Active && view.Order.Kind != Curriculum::OrderKind::None
        && view.Order.Source != Curriculum::OrderSource::Side)
    {
        int32 enemySlot = -1;
        for (uint32 slot = 0; slot < view.EnemyCount && slot < Curriculum::PACK_SLOTS && enemySlot < 0; ++slot)
            if (view.Enemies[slot] && view.Enemies[slot] == view.Order.Target)
                enemySlot = int32(slot);
        member.OrderGoal = Curriculum::OrderGoal(view.Order.Kind, enemySlot, view.Order.Objective);
    }
    int32 const primary = member.OrderGoal != Curriculum::NO_GOAL ? member.OrderGoal : member.Goal;
    // The goals it holds shape what the core block offers, as they do for a forge seat (CoreBlock::GoalCloses).
    view.Goal = primary;
    view.Goal2 = member.Goal2 != primary ? member.Goal2 : Curriculum::NO_GOAL;
    view.OrderGoal = member.OrderGoal;
    // Ended as a forge seat's goals end (GoalBlock::Status): reached, or no longer possible -- the model then
    // promotes its queue or chooses again at this decision rather than at its clock, and drops a secondary.
    auto const ended = [&view](int32 goal, int32& checked, bool& satisfied, bool& reachedOut)
    {
        bool reached = false;
        bool possible = false;
        Curriculum::GoalBlock::Status(view, goal, reached, possible);
        // True already when it was chosen: held, not ended, as a forge seat's is (GoalBlock::Earned).
        bool fresh = checked != goal;
        checked = goal;
        reachedOut = Curriculum::GoalBlock::Earned(reached, fresh, satisfied);
        return reachedOut || !possible;
    };
    if (primary != Curriculum::NO_GOAL)
        view.GoalEnded = ended(primary, member.GoalChecked, member.GoalSatisfiedAtChoice, view.GoalReached);
    if (view.Goal2 != Curriculum::NO_GOAL)
    {
        bool reached = false;
        view.Goal2Ended = ended(view.Goal2, member.Goal2Checked, member.Goal2SatisfiedAtChoice, reached);
    }
    ObserveGoalSignals(member, view, bot, owner);
    SeatEncoder::Observe(view, member.Obs.data(), member.Mask.data());

    // Paced and locked actions, as a forge seat's mask has them.
    Layout const& layout = *member.L;
    for (uint32 action = 1; action < layout.NumActions; ++action)
        if (member.Mask[action] && member.Memory.Paced(layout, action, _nowMs, settings.Actions))
            member.Mask[action] = 0;

    // As a forge seat: nothing to act on between pulls unless the layout acts without a target (food, drink). The
    // dead act on their own death.
    if (!target && !SeatEncoder::ActsWithoutTarget(*member.L) && bot->IsAlive())
        return;

    int32 const action = policy.Decide(member.Obs.data(), member.Mask.data(), &member.Policy);
    // What it is pursuing, for its teammates to see, as a forge party seat's goal reaches the others.
    member.Goal = policy.GoalCount() ? policy.PrimaryOf(member.Policy) : Curriculum::NO_GOAL;
    member.Goal2 = policy.GoalCount() ? policy.SecondaryOf(member.Policy) : Curriculum::NO_GOAL;
    Capture::CompanionDecision(bot, _owner, member.L->ModelName(), member.Obs.data(), member.Obs.size(), action,
        member.Goal, member.Goal2);

    SeatActionResult result;
    SeatEncoder::Apply(view, action, result);
    // Steering is state, not a one-off order: what the feet and the head were told is what the next decision
    // continues from.
    member.HeldBearing = view.HeldBearing;
    member.FacingMode = view.FacingMode;
    member.TurnLeft = view.TurnLeft;
    member.PitchTarget = view.PitchTarget;
    member.Pitch = view.Pitch;
    member.Facing = view.Facing;
    if (action > 0)
        member.Memory.Press(layout, uint32(action), _nowMs, settings.Actions, bot, &member.KnownRanks);
    member.TargetSlot = view.TargetSlot;
    member.FriendSlot = view.FriendSlot;
    member.RankTier = view.RankTier;

    if (result.CallBeast && CallHunterBeast(bot, result.CallBeast))
        Encoding::StartCallBeastCooldown(bot);

    // Its accept of a friend's resurrection (DeathBlock), taken as a client does. Nothing in the core clears the
    // request afterwards (StageScenario::AcceptResurrections), so it is cleared here.
    if (result.AcceptResurrection && bot->isResurrectRequested())
    {
        bot->ResurectUsingRequestData();
        bot->clearResurrectRequestData();
    }
}

Unit* Animus::CompanionParty::CurrentTarget(Member& member, Player* bot) const
{
    if (member.TargetSlot < _enemies.size())
        if (Unit* selected = _enemyUnits[member.TargetSlot]; selected && selected->IsAlive())
            return selected;

    // The selection died or despawned: the nearest living enemy, like a player tabbing to the next one.
    Unit* nearest = nullptr;
    for (uint32 slot = 0; slot < _enemies.size(); ++slot)
    {
        Unit* enemy = _enemyUnits[slot];
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!nearest || bot->GetDistance(enemy) < bot->GetDistance(nearest))
        {
            nearest = enemy;
            member.TargetSlot = slot;
        }
    }

    return nearest;
}

Animus::Curriculum::SeatView Animus::CompanionParty::View(Member const& member, Player* bot, Player* owner,
    Unit* target, Settings const& settings) const
{
    Layout const& layout = *member.L;

    SeatView view;
    view.L = &layout;
    view.Bot = bot;
    view.Target = target;
    view.Level = member.Level;
    view.Race = member.Race;
    view.Spec = member.Spec;
    view.Apt = member.Apt;
    view.Build = &member.Build;
    view.Memory = &member.Memory;
    view.Trail = &member.Trail;
    view.Probe = &member.Probe;
    view.HeldBearing = member.HeldBearing;
    view.FacingMode = member.FacingMode;
    view.TurnLeft = member.TurnLeft;
    view.Steering = &member.Steering;
    view.PitchTarget = member.PitchTarget;
    view.Pitch = member.Pitch;
    view.Facing = member.Facing;
    view.MoveRate = member.MoveRate;
    view.CloseRate = member.CloseRate;
    view.SubmergedTime = member.SubmergedSinceMs && _nowMs > member.SubmergedSinceMs
        ? float(_nowMs - member.SubmergedSinceMs) / 1000.0f : 0.0f;
    view.BreathSpent = float(member.BreathSpentMs) / float(BreathMs());
    view.KnownRanks = &member.KnownRanks;
    view.JumpDropSearch = settings.Actions.JumpDropSearch;
    view.Options = settings.Options;
    view.NowMs = _nowMs;
    view.LastStepDamage = member.LastStepDamage;
    view.LastStepPowerDelta = member.LastStepPowerDelta;
    view.LastStepDamageTaken = member.LastStepDamageTaken;
    // The forge's episode clock, from the episode this party's fighting belongs to (StartEpisode).
    view.EpisodeTime = _episodeStarted
        ? std::min(1.0f, float(_nowMs - _episodeStartMs) / EPISODE_TIME_SCALE_MS) : 0.0f;
    view.CombatTime = member.InCombat
        ? std::min(1.0f, float(_nowMs - member.CombatStartMs) / COMBAT_TIME_SCALE_MS) : 0.0f;
    view.Supplies = member.Supplies;
    view.SelfResurrectAllowed = !bot->GetMap()->IsBattlegroundOrArena();
    view.DeathRuns = member.L && member.L->Has(BlockId::Death) && !bot->GetMap()->IsBattlegroundOrArena();
    view.DeadSeconds = bot->IsAlive() ? 0.0f : float(member.DeadMs) / 1000.0f;

    view.StableCount = uint32(std::min<std::size_t>(member.Stable.size(), STABLE_SLOTS));
    std::copy_n(member.Stable.begin(), view.StableCount, view.Stable.begin());

    view.EnemyCount = uint32(_enemies.size());
    view.Enemies = _enemyUnits;
    view.TargetSlot = member.TargetSlot;
    view.FriendSlot = member.FriendSlot;
    view.RankTier = member.RankTier;

    if (layout.Has(BlockId::Gauntlet))
    {
        bool const elite = std::any_of(_enemyUnits.begin(), _enemyUnits.end(), [bot](Unit* enemy)
        {
            return enemy && ((enemy->ToCreature() && enemy->ToCreature()->isElite())
                || enemy->GetLevel() > bot->GetLevel());
        });

        view.PullsCleared = _pullsCleared;
        view.QuietTime = _foughtBefore ? std::min(1.0f, float(_nowMs - _quietSinceMs) / QUIET_TIME_SCALE_MS) : 1.0f;
        view.PullTime = _enemies.empty() ? 0.0f : std::min(1.0f, float(_nowMs - _pullStartMs) / PULL_TIME_SCALE_MS);
        view.ElitePull = elite;
        view.FoodItem = member.FoodItem;
        view.DrinkItem = member.DrinkItem;
    }

    // The owner is the player the companions fight for (the forge's scripted owner).
    view.Owner = owner;

    // The other companions are the party's other seats.
    uint32 slot = 0;
    for (std::unique_ptr<Member> const& other : _members)
    {
        if (other.get() == &member || slot >= PARTY_MEMBERS)
            continue;

        Player* teammate = FindBot(other->Bot);
        if (teammate && (!teammate->IsInWorld() || teammate->GetMap() != bot->GetMap()))
            teammate = nullptr;

        // The goal a teammate is pursuing, as a forge party seat sees it: what its own model last chose.
        view.Teammates[slot++] = { teammate, other->Goal, other->Apt, other->L->Profile->Class };
    }

    // As the forge's PartyTank: the first living tank of the party, the bot itself included.
    for (std::unique_ptr<Member> const& other : _members)
    {
        // Whoever can hold a pull, which is the same question PartyEncounter asks of a forge party.
        if (!AptitudeDemand::HoldsThePull().MetBy(other->Apt))
            continue;

        Player* tank = other.get() == &member ? bot : FindBot(other->Bot);
        if (tank && tank->IsInWorld() && tank->GetMap() == bot->GetMap() && tank->IsAlive())
        {
            view.Tank = tank;
            break;
        }
    }

    if (target && target->IsPlayer())
    {
        view.Opponent = target->ToPlayer();
        view.OpponentClass = view.Opponent->getClass();
        // Left at nothing on purpose: the forge fills this from the opponent's own build, and a real player's
        // build is not something this module can read. An all-zero aptitude says "nothing is known of it", which
        // is true, rather than guessing at damage and being wrong about a healer.
        view.OpponentApt = Curriculum::Aptitude();
    }

    // A movement model travels to an objective (the forge's travel encounter gives it one each episode); a
    // companion's is its owner, so the model walks, jumps, swims and flies to where the player is with what it
    // learned. The detour is unknown here and read as the straight line; arriving is the encounter's distance,
    // closer indoors.
    if (Unit const* objective = TravelObjective(member, bot, owner))
    {
        view.HasObjective = true;
        view.Objective.Relocate(objective);
        view.Detour = 1.0f;
        view.CloseRate = member.CloseRate;
        view.MountsAllowed = !bot->GetMap()->IsDungeon() && !bot->GetMap()->IsBattlegroundOrArena();
        view.GroundMountAllowed = true;
        view.ArriveWithin = bot->IsOutdoors() ? Curriculum::TravelBlock::ARRIVE_DISTANCE
            : Curriculum::TravelBlock::ARRIVE_INDOORS;
    }

    // Life outside the fight: the world block's features from the real world, for a model that carries it.
    if (layout.Has(BlockId::World))
        sLife->Sense(bot, view.World);

    // What a player could not know, as the forge hides it from its seats: a target it cannot see is only where it
    // was last seen, and enemies it cannot see are not in its view.
    if (bot->IsAlive())
    {
        auto const hidden = [bot](Unit const* unit) { return unit && unit != bot && !bot->CanSeeOrDetect(unit); };
        if (hidden(target))
        {
            view.HiddenTarget = target;
            view.Target = nullptr;
            view.TargetSeen = member.LastSeenGuid == target->GetGUID();
            if (view.TargetSeen)
            {
                view.LastSeen = member.LastSeen;
                view.TargetUnseenTime = std::min(1.0f, float(_nowMs - member.LastSeenMs) / MAX_UNSEEN_TIME_MS);
            }
        }
        for (uint32 i = 0; i < view.EnemyCount; ++i)
            if (hidden(view.Enemies[i]))
                view.Enemies[i] = nullptr;
        view.OpponentHidden = hidden(view.Opponent);
    }

    // What the party's director asks of this companion (Direct), as a forge seat reads its side's order.
    if (uint32 const slot = DirectedSlot(member.Bot); slot < _directed.size())
    {
        Curriculum::DirectorOrders::MemberOrder const& own = _orders.Members[slot];
        SeatView::TeamOrder& order = view.Order;
        order.Active = true;
        order.Posture = _orders.Posture;
        order.Rally = _orders.Rally;
        order.Focus = _orders.Focus ? Curriculum::Encoding::UnitThrough(*bot, _orders.Focus) : nullptr;
        order.Kind = own.Kind;
        order.Target = own.Target ? Curriculum::Encoding::UnitThrough(*bot, own.Target) : nullptr;
        order.Objective = own.Objective;
        order.Source = own.Source;
        order.Age = std::min(1.0f, float(_directorSteps - std::min(_directorSteps, own.IssuedStep))
            / Curriculum::DirectorLayout::CALL_AGE_SCALE);
        order.IsDuty = own.Kind == Curriculum::OrderKind::Interrupt || own.Kind == Curriculum::OrderKind::Control;
        if (order.Focus && !bot->CanSeeOrDetect(order.Focus))
        {
            order.FocusUnseen = true;
            order.Focus = nullptr;
        }
    }

    return view;
}

Unit const* Animus::CompanionParty::TravelObjective(Member const& member, Player* bot, Player* owner)
{
    if (!member.L || !member.L->Has(BlockId::Travel) || !owner || !owner->IsInWorld() || owner->GetMap() != bot->GetMap())
        return nullptr;
    return owner;
}

void Animus::CompanionParty::Track(Member& member, Player* bot, Player* owner, Unit* target,
    Settings const& settings) const
{
    // The facing it steers by is its own, seeded from the character the first time: a default of 0 would aim it
    // due east.
    if (!member.FacingSeeded)
    {
        member.Facing = bot->GetOrientation();
        member.FacingSeeded = true;
    }

    // In water, as the client would say: Player::SetInWater is only called from the movement opcode handler, and a
    // companion sends none, so the core would think it dry for good -- and launch every swim at run speed.
    LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(),
        bot->GetPositionY(), bot->GetPositionZ(), bot->GetCollisionHeight(), {});
    bool const swimming = (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) != 0;
    bot->SetInWater(swimming);
    bot->SetSwim(swimming);

    // Breath, as the forge's seats keep it: spent under water, back ten times as fast above it, none spent under
    // a water-breathing aura.
    if (bot->IsUnderWater())
    {
        if (!member.SubmergedSinceMs)
            member.SubmergedSinceMs = std::max<uint64>(1, _nowMs);
        if (bot->HasWaterBreathingAura())
            member.BreathSpentMs = 0;
        else
            member.BreathSpentMs += settings.DecisionMs;
    }
    else
    {
        member.SubmergedSinceMs = 0;
        member.BreathSpentMs -= std::min(member.BreathSpentMs, 10 * settings.DecisionMs);
    }

    // Where its target was last seen.
    if (target && bot->CanSeeOrDetect(target))
    {
        member.LastSeenGuid = target->GetGUID();
        member.LastSeen.Relocate(target);
        member.LastSeenMs = _nowMs;
    }

    // Whether its legs are getting anywhere (the forge's StageScenario::TrackMotion).
    constexpr uint64 MARK_MS = 1000;
    constexpr float RUN_SPEED = 7.0f;
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    if (member.MotionHasLast)
        member.MotionTravelled += std::hypot(x - member.MotionLastX, y - member.MotionLastY);
    member.MotionLastX = x;
    member.MotionLastY = y;
    member.MotionHasLast = true;

    // The closing rate is toward what the model is heading for: its objective where it travels (the forge's travel
    // encounter replaces the target's with the objective's the same way), else its target.
    Unit const* toward = TravelObjective(member, bot, owner);
    if (!toward)
        toward = target;
    float const range = toward ? bot->GetExactDist2d(toward) : -1.0f;
    if (!member.MotionMarkMs || _nowMs < member.MotionMarkMs)
    {
        member.MotionMarkMs = std::max<uint64>(1, _nowMs);
        member.MotionMarkTravelled = member.MotionTravelled;
        member.MotionMarkRange = range;
        return;
    }
    if (_nowMs - member.MotionMarkMs < MARK_MS)
        return;

    float const seconds = float(_nowMs - member.MotionMarkMs) / 1000.0f;
    member.MoveRate = (member.MotionTravelled - member.MotionMarkTravelled) / (seconds * RUN_SPEED);
    member.CloseRate = member.MotionMarkRange >= 0.0f && range >= 0.0f
        ? (member.MotionMarkRange - range) / (seconds * RUN_SPEED) : 0.0f;
    member.MotionMarkMs = _nowMs;
    member.MotionMarkTravelled = member.MotionTravelled;
    member.MotionMarkRange = range;
}

void Animus::CompanionParty::RecordDamageDealt(ObjectGuid bot, ObjectGuid victim, uint32 damage)
{
    if (std::find(_enemies.begin(), _enemies.end(), victim) == _enemies.end())
        return;

    for (std::unique_ptr<Member> const& member : _members)
        if (member->Bot == bot)
            member->StepDamage.fetch_add(damage, std::memory_order_relaxed);
}

void Animus::CompanionParty::RecordDamageTaken(ObjectGuid bot, uint32 damage)
{
    for (std::unique_ptr<Member> const& member : _members)
        if (member->Bot == bot)
            member->StepDamageTaken.fetch_add(damage, std::memory_order_relaxed);
}

bool Animus::CompanionParty::HasBot(ObjectGuid bot) const
{
    return std::any_of(_members.begin(), _members.end(),
        [bot](std::unique_ptr<Member> const& member) { return member->Bot == bot; });
}

uint8 Animus::CompanionParty::LevelFor(Member const& member, Player* owner) const
{
    return std::max<uint8>(owner->GetLevel(), member.L->Assets->Kit->MinLevel());
}

void Animus::CompanionParty::LevelUp(Member& member, Player* bot, Player* owner) const
{
    if (member.Edited)
    {
        LevelUpEdited(member, bot, owner);
        return;
    }

    Layout const& layout = *member.L;
    uint8 const level = LevelFor(member, owner);
    bool const petOut = !bot->GetPetGUID().IsEmpty();

    // A character of the new level, built as Add builds one: its talents spent again from all its points (resetting
    // them puts the pet away), the trainer spells it now has, gear of its level (which empties the bags), then its
    // pet back out if it had one (from the same stable), and supplies.
    bot->GiveLevel(level);
    bot->resetTalents(true);
    bot->InitTalentForLevel();
    member.Build = SeatCharacter::Configure(bot, layout, member.Spec, false).Build;
    if (petOut)
        SeatCharacter::GivePet(bot, member.Stable);

    member.Level = level;
    member.LastPetGuid.Clear();
    Restock(member, bot, owner);
    member.LastPower = bot->GetPower(bot->getPowerType());

    LOG_INFO("module.animus", "Companion {} ({}) is now level {}", member.Name, layout.Profile->Name, level);
    ChatHandler(owner->GetSession()).SendSysMessage(Acore::StringFormat("{} is now level {}.", member.Name, level));
}

void Animus::CompanionParty::LevelUpEdited(Member& member, Player* bot, Player* owner) const
{
    Layout const& layout = *member.L;
    ClassAssets const& assets = *layout.Assets;
    SpecProfile const& spec = layout.Profile->Specs[member.Spec];
    uint8 const level = LevelFor(member, owner);

    // What the owner chose, to take again at the new level: the talents, the pet's, and their gear. The talents are
    // reset rather than kept because unlearning one rank at a time leaves the core's private count of spent points
    // wrong (CompanionTalents::Unlearn), and resetting is the one thing that puts it right; it also puts the pet
    // away, as the plain LevelUp does.
    CompanionTalents::Snapshot const talents = CompanionTalents::Take(bot);
    uint32 const oldPoints = uint32(std::accumulate(talents.begin(), talents.end(), 0u,
        [](uint32 sum, auto const& entry) { return sum + entry.second; }));
    ::Pet* pet = bot->GetPet();
    bool const petOut = pet != nullptr;
    CompanionTalents::Snapshot const petTalents = pet ? CompanionTalents::TakePet(pet) : CompanionTalents::Snapshot{};
    std::vector<Item*> const ownerGear = CompanionGear::Detach(bot, member.OwnerGear);

    bot->GiveLevel(level);
    bot->resetTalents(true);
    bot->InitTalentForLevel();
    CompanionTalents::Replay(bot, talents);

    // The new points go where the standard build would put them next; what the build spends first was already the
    // owner's to keep or drop, so those steps are skipped. What the build cannot place stays for the owner.
    TalentBuilder::Build const standard = assets.Talents->Standard(spec.Name, spec.TabPage,
        bot->GetFreeTalentPoints() + oldPoints);
    for (std::size_t i = oldPoints; i < standard.Order.size() && bot->GetFreeTalentPoints(); ++i)
        bot->LearnTalent(assets.Talents->Talents()[standard.Order[i].Index].TalentId, standard.Order[i].Rank);

    // Configure builds the rest (trainer spells, glyphs, gear of the level) and would spend every free point on a
    // build of its own: it sees none, and gets them back after. Its empty build is not the member's; that is read
    // off the character once the gear is back on.
    uint32 const free = bot->GetFreeTalentPoints();
    bot->SetFreeTalentPoints(0);
    SeatCharacter::Configure(bot, layout, member.Spec, false);
    bot->SetFreeTalentPoints(free);
    CompanionGear::Reattach(bot, owner, ownerGear, member.OwnerGear);
    RefreshBuild(member, bot);

    if (petOut && SeatCharacter::GivePet(bot, member.Stable))
        if (::Pet* given = bot->GetPet())
        {
            // GivePet spends the pet's points on the standard build; the owner's choices replace it.
            given->resetTalents();
            CompanionTalents::ReplayPet(bot, given, petTalents);
            PetTalents::Spend(bot, given);
        }

    member.Level = level;
    member.LastPetGuid.Clear();
    Restock(member, bot, owner);
    member.LastPower = bot->GetPower(bot->getPowerType());

    LOG_INFO("module.animus", "Companion {} ({}) is now level {}, keeping what its owner chose", member.Name,
        layout.Profile->Name, level);
    ChatHandler(owner->GetSession()).SendSysMessage(Acore::StringFormat("{} is now level {}.", member.Name, level));
}

std::vector<ObjectGuid> Animus::CompanionParty::GetBotGUIDs() const
{
    std::vector<ObjectGuid> bots;
    for (std::unique_ptr<Member> const& member : _members)
        bots.push_back(member->Bot);
    return bots;
}

std::size_t Animus::CompanionParty::MaxSize()
{
    return MAX_COMPANIONS;
}

std::vector<Animus::CompanionParty::Summary> Animus::CompanionParty::Summarize(ModelLibrary& models) const
{
    std::vector<Summary> summaries;
    summaries.reserve(_members.size());
    for (std::unique_ptr<Member> const& member : _members)
    {
        if (member->Temporary)
            continue;
        std::string error;
        bool const loaded = models.Find(*member->L, error) != nullptr;
        summaries.push_back({ member->Name, member->L->Profile->Name, member->L->Profile->Specs[member->Spec].Name,
            member->Level, member->L->ModelName(), loaded ? "loaded" : error, member->Parked });
    }
    return summaries;
}

std::vector<std::string> Animus::CompanionParty::Describe(ModelLibrary& models) const
{
    std::vector<std::string> lines;
    for (Summary const& companion : Summarize(models))
        lines.push_back(Acore::StringFormat("{}: level {} {} ({}), model {}: {}{}", companion.Name, companion.Level,
            companion.Class, companion.Spec, companion.ModelName, companion.Model,
            companion.Parked ? " (waiting for you to land)" : ""));
    return lines;
}

void Animus::CompanionParty::RemoveMembers(bool temporary)
{
    std::vector<std::unique_ptr<Member>> gone;
    for (auto itr = _members.begin(); itr != _members.end();)
    {
        if ((*itr)->Temporary == temporary)
        {
            gone.push_back(std::move(*itr));
            itr = _members.erase(itr);
        }
        else
            ++itr;
    }
    for (std::unique_ptr<Member> const& member : gone)
        Destroy(*member, nullptr);
}

std::size_t Animus::CompanionParty::Companions() const
{
    return std::size_t(std::count_if(_members.begin(), _members.end(),
        [](std::unique_ptr<Member> const& member) { return !member->Temporary; }));
}

Animus::Curriculum::Aptitude const* Animus::CompanionParty::AptitudeOf(ObjectGuid bot) const
{
    for (std::unique_ptr<Member> const& member : _members)
        if (member->Bot == bot)
            return &member->Apt;
    return nullptr;
}

void Animus::CompanionParty::DestroyAll()
{
    std::vector<std::unique_ptr<Member>> members = std::move(_members);
    _members.clear();

    for (std::unique_ptr<Member> const& member : members)
        Destroy(*member, nullptr);
}

bool Animus::CompanionParty::Remove(std::string_view name, std::string& message)
{
    Member* member = Find(name);
    if (!member)
    {
        message = Acore::StringFormat("{} is not one of your companions.", name);
        return false;
    }

    auto const itr = std::find_if(_members.begin(), _members.end(),
        [member](std::unique_ptr<Member> const& m) { return m.get() == member; });
    std::unique_ptr<Member> const gone = std::move(*itr);
    _members.erase(itr);
    Destroy(*gone, nullptr);
    message = Acore::StringFormat("{} dismissed.", gone->Name);
    return true;
}

void Animus::CompanionParty::Destroy(Member& member, Player* owner)
{
    Player* bot = FindBot(member.Bot);
    if (!bot)
        return;

    // The owner's companion keeps its character (and its place in the character cache) to be summoned again; a
    // temporary member is gone for good.
    Destroy(bot, member.Temporary);
}

Animus::CompanionParty::Member* Animus::CompanionParty::Find(std::string_view name) const
{
    for (std::unique_ptr<Member> const& member : _members)
        if (std::equal(name.begin(), name.end(), member->Name.begin(), member->Name.end(),
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a))
                == std::tolower(static_cast<unsigned char>(b)); }))
            return member.get();
    return nullptr;
}

Player* Animus::CompanionParty::BotOf(Member const& member, std::string& message) const
{
    Player* bot = FindBot(member.Bot);
    if (!bot || !bot->IsInWorld() || member.Parked)
    {
        message = Acore::StringFormat("{} is not here right now.", member.Name);
        return nullptr;
    }
    return bot;
}

bool Animus::CompanionParty::Talent(std::string_view name, uint32 talentId, bool learn, std::string& message)
{
    Member* member = Find(name);
    if (!member)
    {
        message = Acore::StringFormat("{} is not one of your companions.", name);
        return false;
    }

    Player* bot = BotOf(*member, message);
    if (!bot)
        return false;

    if (!(learn ? CompanionTalents::Learn(bot, talentId, message) : CompanionTalents::Unlearn(bot, talentId, message)))
        return false;

    member->Edited = true;
    bot->UpdateAllStats();
    RefreshBuild(*member, bot);
    message = learn ? "Learned." : "Unlearned.";
    return true;
}

void Animus::CompanionParty::RefreshBuild(Member& member, Player* bot) const
{
    // The highest rank it knows of each catalog spell, which the encoders ask for three times an action a decision;
    // only a new build (talents, a level, trainer spells) changes it.
    std::vector<ActionCatalog::Action> const& actions = member.L->Catalog().Actions();
    member.KnownRanks.assign(actions.size(), nullptr);
    for (ActionCatalog::Action const& action : actions)
        if (action.Type == ActionCatalog::Kind::Spell)
            member.KnownRanks[action.Index] = ActionCatalog::KnownRank(bot, action.FirstRank);

    member.Build = ReadBuild(*member.L->Assets, bot);
    member.Apt = Aptitude::Of(*member.L->Assets, member.Build, bot);
}

Animus::Curriculum::TalentBuilder::Build Animus::CompanionParty::ReadBuild(ClassAssets const& assets, Player* player)
{
    TalentBuilder const& talents = *assets.Talents;
    CompanionTalents::Snapshot const known = CompanionTalents::Take(player);

    TalentBuilder::Build build;
    build.Ranks.assign(talents.Talents().size(), 0);
    for (uint32 i = 0; i < talents.Talents().size(); ++i)
    {
        TalentBuilder::Talent const& talent = talents.Talents()[i];
        auto const itr = known.find(talent.TalentId);
        if (itr == known.end())
            continue;

        build.Ranks[i] = itr->second;
        if (talent.Tab < build.TreePoints.size())
            build.TreePoints[talent.Tab] += itr->second;
        for (uint8 rank = 0; rank < itr->second; ++rank)
            build.Order.push_back({ i, rank });
    }
    return build;
}

bool Animus::CompanionParty::PetTalent(std::string_view name, uint32 talentId, bool learn, std::string& message)
{
    Member* member = Find(name);
    if (!member)
    {
        message = Acore::StringFormat("{} is not one of your companions.", name);
        return false;
    }

    Player* bot = BotOf(*member, message);
    if (!bot)
        return false;

    ::Pet* pet = bot->GetPet();
    if (!pet)
    {
        message = Acore::StringFormat("{} has no pet out.", member->Name);
        return false;
    }

    if (!(learn ? CompanionTalents::LearnPet(bot, pet, talentId, message)
        : CompanionTalents::UnlearnPet(bot, pet, talentId, message)))
        return false;

    member->Edited = true;
    message = learn ? "Learned." : "Unlearned.";
    return true;
}

bool Animus::CompanionParty::Equip(Player* owner, std::string_view name, uint8 bag, uint8 slot, uint8 equipSlot,
    std::string& message)
{
    Member* member = Find(name);
    if (!member)
    {
        message = Acore::StringFormat("{} is not one of your companions.", name);
        return false;
    }

    Player* bot = BotOf(*member, message);
    if (!bot)
        return false;

    uint8 equipped = 0;
    if (!CompanionGear::Give(owner, bot, bag, slot, equipSlot, equipped, message))
        return false;

    member->Edited = true;
    if (std::find(member->OwnerGear.begin(), member->OwnerGear.end(), equipped) == member->OwnerGear.end())
        member->OwnerGear.push_back(equipped);
    RefreshBuild(*member, bot);
    message = Acore::StringFormat("{} equips it.", member->Name);
    return true;
}

bool Animus::CompanionParty::Pet(std::string_view name, PetView& view, std::string& message) const
{
    Member const* member = Find(name);
    if (!member)
    {
        message = Acore::StringFormat("{} is not one of your companions.", name);
        return false;
    }

    Player* bot = BotOf(*member, message);
    if (!bot)
        return false;

    ::Pet const* pet = bot->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET)
    {
        message = Acore::StringFormat("{} has no pet with talents out.", member->Name);
        return false;
    }

    view.PetName = pet->GetName();
    view.Level = uint8(pet->GetLevel());
    view.FreePoints = const_cast<::Pet*>(pet)->GetFreeTalentPoints();
    view.Talents = CompanionTalents::PetTree(pet);
    return true;
}

void Animus::CompanionParty::Destroy(Player* bot, bool forget)
{
    LOG_INFO("module.animus", "Removing companion {} ({})", bot->GetName(), bot->GetGUID().ToString());

    // Out of the group first: logging out would leave it an offline member. A group left with one player disbands.
    if (Group* group = bot->GetGroup())
        group->RemoveMember(bot->GetGUID());

    BotFactory::Destroy(bot, false, forget);
}
