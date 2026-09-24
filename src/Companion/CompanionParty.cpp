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
#include "Chat.h"
#include "ClassAssets.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "MlpPolicy.h"
#include "ModelLibrary.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PetBlock.h"
#include "Player.h"
#include "Random.h"
#include "SeatCharacter.h"
#include "SeatEncoder.h"
#include "StringFormat.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    /// Companion accounts: below the library's scenario bots (BotAccounts::BASE) and far above any real realm's.
    constexpr uint32 COMPANION_ACCOUNT_BASE = 0x7E000000;

    /// A party holds the owner and up to four companions, like the party stage's four seats.
    constexpr std::size_t MAX_COMPANIONS = PARTY_MEMBERS + 1;

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

    /// Companions created this run. Names carry the number: real character names cannot contain digits, so a
    /// companion never collides with a player in ObjectAccessor's name map.
    uint32 CompanionCounter = 0;

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

bool Animus::CompanionParty::Add(Player* owner, Layout const& layout, AptitudeDemand demand, uint8 race,
    std::string& message)
{
    if (_members.size() >= MAX_COMPANIONS)
    {
        message = Acore::StringFormat("You already have {} companions.", MAX_COMPANIONS);
        return false;
    }

    ClassProfile const& profile = *layout.Profile;
    ClassAssets const& assets = *layout.Assets;

    // A class that starts above level 1 (death knights) starts there, whatever the owner's level.
    uint8 const level = std::max<uint8>(owner->GetLevel(), assets.Kit->MinLevel());

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

    uint32 const number = ++CompanionCounter;

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Animus{}", number);
    spec.Race = race;
    spec.Class = profile.Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = COMPANION_ACCOUNT_BASE + number;

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
    member->Race = race;
    member->Level = level;
    // A spec that can do what was asked of it, as the forge's character generator does when it builds a seat
    // (StageScenario::BuildSeat). The model is told what the character can do and what talents it has, never which
    // spec it drew.
    member->Spec = DrawSpec(ClassAssets::For(profile), demand);

    // As the forge builds a seat (StageScenario::BuildSeat, Configure, PrepareFighter, StockSeats). Talent points
    // depend on the map for death knights; the bot is on the owner's map now.
    bot->InitTalentForLevel();
    member->Build = SeatCharacter::Configure(bot, layout, member->Spec, false).Build;
    // Read what it can do now that it is the character it is going to be -- talents spent, gear on, spellbook
    // final -- exactly where StageScenario::Configure reads it, and before anything is asked of it.
    member->Apt = Aptitude::Of(ClassAssets::For(profile), member->Build, bot);
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

    // Supplies after joining: a warlock in the group hands out healthstones.
    Restock(*member, bot, owner);
    member->LastPower = bot->GetPower(bot->getPowerType());

    LOG_INFO("module.animus", "{} summoned {} companion {} ({}), level {}, spec {}", owner->GetName(), profile.Name,
        bot->GetName(), bot->GetGUID().ToString(), level, profile.Specs[member->Spec].Name);

    message = Acore::StringFormat("{}, a level {} {}, joins your party.", bot->GetName(), level, profile.Name);
    _members.push_back(std::move(member));
    return true;
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

void Animus::CompanionParty::StartEpisode(Player* owner)
{
    _episodeStarted = true;
    _episodeStartMs = _nowMs;
    _pullsCleared = 0;

    // Training starts every episode with full bags.
    for (std::unique_ptr<Member> const& member : _members)
        if (Player* bot = FindBot(member->Bot); bot && bot->IsInWorld() && bot->IsAlive())
            Restock(*member, bot, owner);
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

    bool const quiet = _enemies.empty() && !bot->IsInCombat();

    if (!bot->IsAlive())
    {
        // A resurrection another companion cast is accepted, as a client does.
        if (bot->isResurrectRequested())
        {
            bot->ResurectUsingRequestData();
            member.DeadMs = 0;
            return;
        }

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

    // The owner levelled: the companion follows between pulls, as a new character of that level.
    if (quiet && LevelFor(member, owner) > member.Level)
        LevelUp(member, bot, owner);

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
    Decide(member, bot, owner, *policy, settings);
}

void Animus::CompanionParty::Decide(Member& member, Player* bot, Player* owner, MlpPolicy& policy,
    Settings const& settings)
{
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
    member.Memory.Observe(bot, target, _nowMs);
    SeatView view = View(member, bot, owner, target, settings);
    // The durative action it is running: one press that stands for many decisions (rest, hold an interrupt, keep
    // range). SeatEncoder starts, runs and stops it as it does for a forge seat.
    view.Option = &member.Option;
    SeatEncoder::Observe(view, member.Obs.data(), member.Mask.data());

    // Paced and locked actions, as a forge seat's mask has them.
    Layout const& layout = *member.L;
    for (uint32 action = 1; action < layout.NumActions; ++action)
        if (member.Mask[action] && member.Memory.Paced(layout, action, _nowMs, settings.Actions))
            member.Mask[action] = 0;

    // As a forge seat: nothing to act on between pulls unless the layout acts without a target (food, drink).
    if (!target && !SeatEncoder::ActsWithoutTarget(*member.L))
        return;

    int32 const action = policy.Decide(member.Obs.data(), member.Mask.data(), &member.Policy);
    // What it is pursuing, for its teammates to see, as a forge party seat's goal reaches the others.
    member.Goal = policy.GoalCount() ? int32(member.Policy.Goal) : Curriculum::NO_GOAL;

    SeatActionResult result;
    SeatEncoder::Apply(view, action, result);
    if (action > 0)
        member.Memory.Press(layout, uint32(action), _nowMs, settings.Actions, bot, nullptr);
    member.TargetSlot = view.TargetSlot;
    member.FriendSlot = view.FriendSlot;
    member.RankTier = view.RankTier;

    if (result.CallBeast && CallHunterBeast(bot, result.CallBeast))
        Encoding::StartCallBeastCooldown(bot);
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

    return view;
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
