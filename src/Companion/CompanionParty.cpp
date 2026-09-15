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
#include "BotFactory.h"
#include "ClassRoleAssets.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "MlpPolicy.h"
#include "ModelLibrary.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"
#include "Supplies.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    enum PartySpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    /// A party holds the owner and up to four companions, like the party stage's four seats.
    constexpr std::size_t MAX_COMPANIONS = LayoutConstants::PARTY_MEMBERS + 1;

    /// Quiet this long between pulls and the next pull starts a new episode (the gauntlet's longest break).
    constexpr uint64 NEW_EPISODE_QUIET_MS = 20000;
    constexpr float PULL_TIME_SCALE_MS = 60000.0f;
    constexpr float COMBAT_TIME_SCALE_MS = 60000.0f;
    constexpr float QUIET_TIME_SCALE_MS = 20000.0f;

    /// Out of combat between pulls: companions further than this run back to the owner (training's follow shaping
    /// starts at 25 yd), and further than TELEPORT_DISTANCE (or on another map) they are teleported.
    constexpr float LEASH_DISTANCE = 30.0f;
    constexpr float TELEPORT_DISTANCE = 100.0f;
    constexpr float FOLLOW_DISTANCE = 2.0f;
    constexpr float NO_MODEL_FOLLOW_DISTANCE = 6.0f;
    constexpr uint32 LEASH_MOVE_POINT_ID = 5;

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
        bot->GetMotionMaster()->MovePoint(LEASH_MOVE_POINT_ID, x, y, z);
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

bool Animus::CompanionParty::Add(Player* owner, Curriculum::Layout const& layout, std::string& message)
{
    if (_members.size() >= MAX_COMPANIONS)
    {
        message = Acore::StringFormat("You already have {} companions.", MAX_COMPANIONS);
        return false;
    }

    ClassRoleProfile const& profile = *layout.Profile;
    ClassRoleAssets const& assets = *layout.Assets;
    uint8 const level = owner->GetLevel();
    if (level < assets.Kit->MinLevel())
    {
        message = Acore::StringFormat("A {} companion needs level {}.", profile.ScenarioName, assets.Kit->MinLevel());
        return false;
    }

    if (assets.Races.empty() || profile.Specs.empty())
    {
        message = Acore::StringFormat("No {} companion can be built.", profile.ScenarioName);
        return false;
    }

    Group* group = owner->GetGroup();
    if (group && !group->IsLeader(owner->GetGUID()))
    {
        message = "Only your group's leader can add companions to it.";
        return false;
    }

    if (group && group->IsFull())
    {
        message = "Your group is full.";
        return false;
    }

    // A race of the owner's faction, so the companion is a friend to the owner and everyone the owner groups with.
    std::vector<uint8> races;
    for (uint8 race : assets.Races)
        if (Player::TeamIdForRace(race) == owner->GetTeamId())
            races.push_back(race);
    if (races.empty())
        races = assets.Races;

    BotFactory::BotSpec spec;
    spec.Race = races[urand(0, uint32(races.size()) - 1)];
    spec.Class = profile.Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;

    Player* bot = BotFactory::Create(spec);
    if (!bot || !BotFactory::PlaceNear(bot, owner))
    {
        message = "The companion could not be created; see the server log.";
        return false;
    }

    auto member = std::make_unique<Member>();
    member->Bot = bot->GetGUID();
    member->Name = bot->GetName();
    member->L = &layout;
    member->Race = spec.Race;
    member->Level = level;
    member->Spec = uint8(urand(0, uint32(profile.Specs.size()) - 1));

    // As the forge builds a seat (StageScenario::BuildSeat, Configure, StartDuel, StartSeatPack). Talent points
    // depend on the map for death knights; the bot is on the owner's map now.
    SpecProfile const& specProfile = profile.Specs[member->Spec];
    bot->InitTalentForLevel();
    GearBuilder::LearnProficiencies(bot);
    member->Build = assets.Talents->Random(specProfile.TabPage, bot->GetFreeTalentPoints());
    assets.Talents->Apply(bot, member->Build);
    assets.Kit->Learn(bot);
    assets.Gear->Equip(bot, specProfile);

    bot->UpdateAllStats();
    bot->SetFullHealth();
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));
    bot->SetPower(POWER_RAGE, 0);
    bot->SetPower(POWER_RUNIC_POWER, 0);

    // Levelling up would change the character under the model.
    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    if (profile.Class == CLASS_HUNTER)
        member->Stable = StablePool::Instance().Random(LayoutConstants::STABLE_SLOTS);

    // A warrior has no stance until one is cast (a first login casts it), and nothing works without one.
    if (profile.Class == CLASS_WARRIOR)
        bot->CastSpell(bot, profile.PlayRole == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);

    if (layout.Has(Stage::Gauntlet))
    {
        ConsumablePool const& consumables = ConsumablePool::Instance();
        member->FoodItem = consumables.Food(level);
        member->DrinkItem = bot->GetMaxPower(POWER_MANA) ? consumables.Drink(level) : 0;
        StockConsumables(bot, member->FoodItem, member->DrinkItem);
    }

    member->Obs.resize(layout.ObsDim);
    member->Mask.resize(layout.NumActions);
    member->LastPower = bot->GetPower(bot->getPowerType());

    bool const newGroup = !group;
    if (newGroup)
    {
        group = new Group();
        if (!group->Create(owner))
        {
            delete group;
            BotFactory::Destroy(bot);
            message = "Could not create a group for your companions; see the server log.";
            return false;
        }

        sGroupMgr->AddGroup(group);
    }

    if (!group->AddMember(bot))
    {
        if (newGroup)
            group->Disband();
        BotFactory::Destroy(bot);
        message = "The companion could not join your group; see the server log.";
        return false;
    }

    LOG_INFO("module.animus", "{} summoned {} companion {} ({}), level {}, spec {}", owner->GetName(),
        profile.ScenarioName, bot->GetName(), bot->GetGUID().ToString(), level, member->Spec);

    message = Acore::StringFormat("{}, a level {} {}, joins your party.", bot->GetName(), level, profile.ScenarioName);
    _members.push_back(std::move(member));
    return true;
}

Animus::CompanionParty::Status Animus::CompanionParty::Update(uint32 diff, Settings const& settings,
    ModelLibrary& models)
{
    _nowMs += diff;

    Player* owner = ObjectAccessor::FindConnectedPlayer(_owner);
    if (!owner)
        return Status::Dismiss;

    // Bots only leave through DestroyAll; anything else took one away.
    std::erase_if(_members, [](std::unique_ptr<Member> const& member) { return !FindBot(member->Bot); });

    if (!owner->IsInWorld() || owner->IsBeingTeleported())
        return Status::Active;

    std::vector<Player*> present;
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot->IsInWorld() && bot->GetMap() == owner->GetMap())
            present.push_back(bot);

    UpdatePull(owner, present);

    for (std::unique_ptr<Member> const& member : _members)
        UpdateMember(*member, FindBot(member->Bot), owner, diff, settings, models);

    return Status::Active;
}

void Animus::CompanionParty::UpdatePull(Player* owner, std::vector<Player*> const& bots)
{
    Map* map = owner->GetMap();

    _enemyUnits.fill(nullptr);
    for (uint32 slot = 0; slot < _enemies.size(); ++slot)
        if (Unit* enemy = ObjectAccessor::GetUnit(*owner, _enemies[slot]); enemy && enemy->IsInWorld()
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
        if (slot >= LayoutConstants::PACK_SLOTS)
        {
            slot = 0;
            while (slot < LayoutConstants::PACK_SLOTS && _enemyUnits[slot] && _enemyUnits[slot]->IsAlive())
                ++slot;
            if (slot == LayoutConstants::PACK_SLOTS)
                continue;
        }
        else
        {
            if (_enemies.empty())
            {
                if (!_episodeStarted || _nowMs - _quietSinceMs >= NEW_EPISODE_QUIET_MS)
                    StartEpisode();
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

void Animus::CompanionParty::StartEpisode()
{
    _episodeStarted = true;
    _pullsCleared = 0;

    // Training starts every episode with full bags.
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot && bot->IsInWorld())
            StockConsumables(bot, member->FoodItem, member->DrinkItem);
}

void Animus::CompanionParty::UpdateMember(Member& member, Player* bot, Player* owner, uint32 diff,
    Settings const& settings, ModelLibrary& models)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return;

    // Waiting outside while the owner is in an instance; back to the owner when they return to the open world.
    if (bot->GetMap() != owner->GetMap())
    {
        if (!owner->GetMap()->Instanceable())
            BotFactory::TeleportNear(bot, owner);
        return;
    }

    bool const quiet = _enemies.empty() && !bot->IsInCombat();

    if (!bot->IsAlive())
    {
        member.DeadMs += diff;
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

    float const distance = bot->GetDistance(owner);
    if (quiet && distance > TELEPORT_DISTANCE)
    {
        BotFactory::TeleportNear(bot, owner);
        return;
    }

    std::string error;
    MlpPolicy* policy = models.Find(*member.L, error);
    if (!policy)
    {
        if (!member.ModelErrorLogged)
        {
            LOG_ERROR("module.animus", "Companion {} has no model and only follows: {}", member.Name, error);
            member.ModelErrorLogged = true;
        }

        if (quiet && distance > NO_MODEL_FOLLOW_DISTANCE && bot->movespline->Finalized())
            MoveBehind(bot, owner);
        return;
    }

    member.ModelErrorLogged = false;
    if (quiet && distance > LEASH_DISTANCE && bot->movespline->Finalized())
        MoveBehind(bot, owner);

    member.SinceDecisionMs += diff;
    if (member.SinceDecisionMs < settings.DecisionMs)
        return;

    member.SinceDecisionMs %= settings.DecisionMs;
    Decide(member, bot, owner, *policy);
}

void Animus::CompanionParty::Decide(Member& member, Player* bot, Player* owner, MlpPolicy& policy)
{
    bool const inCombat = bot->IsInCombat();
    if (inCombat && !member.InCombat)
        member.CombatStartMs = _nowMs;
    member.InCombat = inCombat;

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
    SeatView view = View(member, bot, owner, target);
    SeatEncoder::Observe(view, member.Obs.data(), member.Mask.data());

    int32 const action = policy.Decide(member.Obs.data(), member.Mask.data());

    SeatActionResult result;
    SeatEncoder::Apply(view, action, result);
    member.TargetSlot = view.TargetSlot;

    if (result.CallBeast && CallHunterBeast(bot, result.CallBeast))
        SeatEncoder::StartCallBeastCooldown(bot);
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
    Unit* target) const
{
    Layout const& layout = *member.L;

    SeatView view;
    view.L = &layout;
    view.Bot = bot;
    view.Target = target;
    view.Level = member.Level;
    view.Race = member.Race;
    view.Spec = member.Spec;
    view.Build = &member.Build;
    view.LastStepDamage = member.LastStepDamage;
    view.LastStepPowerDelta = member.LastStepPowerDelta;
    view.LastStepDamageTaken = member.LastStepDamageTaken;
    view.CombatTime = member.InCombat
        ? std::min(1.0f, float(_nowMs - member.CombatStartMs) / COMBAT_TIME_SCALE_MS) : 0.0f;

    view.StableCount = uint32(std::min<std::size_t>(member.Stable.size(), LayoutConstants::STABLE_SLOTS));
    std::copy_n(member.Stable.begin(), view.StableCount, view.Stable.begin());

    view.EnemyCount = uint32(_enemies.size());
    view.Enemies = _enemyUnits;
    view.TargetSlot = member.TargetSlot;

    if (layout.Has(Stage::Gauntlet))
    {
        bool elite = false;
        for (Unit* enemy : _enemyUnits)
            if (enemy && ((enemy->ToCreature() && enemy->ToCreature()->isElite()) || enemy->GetLevel() > bot->GetLevel()))
                elite = true;

        view.PullsCleared = _pullsCleared;
        view.QuietTime = _foughtBefore ? std::min(1.0f, float(_nowMs - _quietSinceMs) / QUIET_TIME_SCALE_MS) : 1.0f;
        view.PullTime = _enemies.empty() ? 0.0f : std::min(1.0f, float(_nowMs - _pullStartMs) / PULL_TIME_SCALE_MS);
        view.ElitePull = elite;
    }

    view.FoodItem = member.FoodItem;
    view.DrinkItem = member.DrinkItem;

    if (layout.Has(Stage::Companion))
        view.Owner = owner;

    if (layout.Has(Stage::Party))
    {
        uint32 slot = 0;
        for (std::unique_ptr<Member> const& other : _members)
        {
            if (other.get() == &member || slot >= LayoutConstants::PARTY_MEMBERS)
                continue;

            Player* teammate = FindBot(other->Bot);
            if (teammate && (!teammate->IsInWorld() || teammate->GetMap() != bot->GetMap()))
                teammate = nullptr;

            view.Teammates[slot++] = { teammate, other->L->PlayRole(), other->L->Profile->Class };
        }

        // As the forge's PartyTank: the first living tank of the party, the bot itself included.
        for (std::unique_ptr<Member> const& other : _members)
        {
            if (other->L->PlayRole() != Role::Tank)
                continue;

            Player* tank = other.get() == &member ? bot : FindBot(other->Bot);
            if (tank && tank->IsInWorld() && tank->GetMap() == bot->GetMap() && tank->IsAlive())
            {
                view.Tank = tank;
                break;
            }
        }
    }

    if (layout.Has(Stage::Pvp) && target && target->IsPlayer())
    {
        view.Opponent = target->ToPlayer();
        view.OpponentClass = view.Opponent->getClass();
        view.OpponentRole = Role::Dps;
    }

    return view;
}

void Animus::CompanionParty::RecordDamage(ObjectGuid attacker, ObjectGuid victim, uint32 dealt, uint32 taken)
{
    for (std::unique_ptr<Member> const& member : _members)
    {
        if (dealt && member->Bot == attacker)
            member->StepDamage.fetch_add(dealt, std::memory_order_relaxed);
        if (taken && member->Bot == victim)
            member->StepDamageTaken.fetch_add(taken, std::memory_order_relaxed);
    }
}

std::vector<ObjectGuid> Animus::CompanionParty::GetBotGUIDs() const
{
    std::vector<ObjectGuid> bots;
    for (std::unique_ptr<Member> const& member : _members)
        bots.push_back(member->Bot);
    return bots;
}

std::vector<std::string> Animus::CompanionParty::Describe(ModelLibrary& models) const
{
    std::vector<std::string> lines;
    for (std::unique_ptr<Member> const& member : _members)
    {
        std::string error;
        bool const loaded = models.Find(*member->L, error) != nullptr;
        lines.push_back(Acore::StringFormat("{}: level {} {} (spec {}), model {}: {}", member->Name, member->Level,
            member->L->Profile->ScenarioName, member->Spec, member->L->ModelName(), loaded ? "loaded" : error));
    }
    return lines;
}

void Animus::CompanionParty::DestroyAll()
{
    std::vector<std::unique_ptr<Member>> members = std::move(_members);
    _members.clear();

    for (std::unique_ptr<Member> const& member : members)
        if (Player* bot = FindBot(member->Bot))
            Destroy(bot);
}

void Animus::CompanionParty::Destroy(Player* bot)
{
    LOG_INFO("module.animus", "Removing companion {} ({})", bot->GetName(), bot->GetGUID().ToString());

    // Out of the group first: logging out would leave it an offline member. A group left with one player disbands.
    if (Group* group = bot->GetGroup())
        group->RemoveMember(bot->GetGUID());

    BotFactory::Destroy(bot);
}
