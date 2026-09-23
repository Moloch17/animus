/*
 * This file is part of the Animus Forge project, based on AzerothCore.
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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_MOVE_BLOCK_H

#include "Block.h"

class Map;

namespace Animus::Curriculum
{
    /// Where the seat puts its feet, answered without reference to anything it is fighting.
    ///
    /// Every movement the curriculum had before this was target-relative: DuelBlock's MOVE_TO_TARGET, MOVE_TO_RANGE,
    /// MOVE_BEHIND, BACK_OFF, KEEP_RANGE, STAY_ON_TARGET and STOP all open with
    /// `if (!target || !target->IsAlive()) return false`, so a seat with nothing to fight could not move at all. The
    /// hazard drill found that the hard way: four million steps with allowed_actions at 1.00, entropy at 0 and the
    /// seats standing still while the fire burned them, until an unkillable emitter was handed to them purely so the
    /// movement actions would unmask. This block is what that hack was standing in for. It needs no target, no enemy
    /// and no objective -- only legs.
    ///
    /// **Bearings are egocentric and durative.** An action picks a compass point relative to where the seat is facing
    /// now (forward, forward-right, right, and so on round the eight), and the seat walks that way until it chooses
    /// otherwise. It is a held key, not a step: a decision is 250 ms of game time and a step measured in one would be
    /// a stutter, which is the same reason KEEP_RANGE is an option rather than a press. The destination is recomputed
    /// from the seat's current position each decision the bearing is held, so the path bends with the ground rather
    /// than aiming at a point chosen when the key went down.
    ///
    /// **Facing is chosen apart from movement**, which is what makes strafing and backpedalling expressible at all.
    /// A player can run one way and look another; a spline that sets its own orientation cannot. FACE_TARGET keeps
    /// the seat turned to what it is fighting while it moves anywhere, FACE_HEADING turns it the way it is going, and
    /// FACE_HOLD leaves it pointing where it already points. Combined with a bearing, those give every gait a player
    /// has: run in facing the enemy, circle it, back away still casting.
    ///
    /// **Resolution comes from the tick, not from more decisions.** AnimusForge.TicksPerDecision cuts a decision into
    /// several world updates, so a spline issued once per decision is still walked in fine steps by the world. Making
    /// the policy decide faster instead would cost an observation, a learner round trip and an action every time.
    ///
    /// This block does not replace the duel block's movement. Closing to melee reach and holding a caster's range are
    /// jobs about a target, and they read better as one action than as a bearing the policy has to steer. What this
    /// adds is everywhere a target cannot help: dodging what lands underfoot, taking a corner, backing out of a
    /// cleave, crossing ground with nothing on it.
    class MoveBlock final : public Block
    {
    public:
        /// Bearings, clockwise from straight ahead. Egocentric: relative to the seat's facing when it chooses, not
        /// to the world or to any enemy.
        enum Bearing : uint32
        {
            BEARING_FORWARD         = 0,
            BEARING_FORWARD_RIGHT   = 1,
            BEARING_RIGHT           = 2,
            BEARING_BACK_RIGHT      = 3,
            BEARING_BACK            = 4,
            BEARING_BACK_LEFT       = 5,
            BEARING_LEFT           = 6,
            BEARING_FORWARD_LEFT    = 7,
            BEARING_COUNT           = 8
        };

        enum Action : uint32
        {
            ACTION_BEARING_FIRST    = 0,
            ACTION_HALT             = ACTION_BEARING_FIRST + BEARING_COUNT,
            /// Face what the seat is fighting while it moves anywhere: the strafe, and the reason a caster can back
            /// away without turning its back on a cast.
            ACTION_FACE_TARGET,
            /// Face the way it is going.
            ACTION_FACE_HEADING,
            /// Leave it facing where it already faces, whatever it does with its feet.
            ACTION_FACE_HOLD,
            /// Face where it is trying to get to. With this held, BEARING_FORWARD is the exact heading to the
            /// objective every decision, which is the difference between crossing ground and zig-zagging across it.
            ACTION_FACE_OBJECTIVE,
            /// Turn on the spot, held like a key, while the feet carry on doing whatever they were told. This is
            /// the mouse-look, and it is what makes a heading between two compass points reachable at all.
            ACTION_TURN_LEFT,
            ACTION_TURN_RIGHT,
            /// Look further up or down, held the same way, and level off. Only off the ground, where a seat has a
            /// third dimension to steer in: swimming and flying.
            ACTION_PITCH_UP,
            ACTION_PITCH_DOWN,
            ACTION_PITCH_LEVEL,
            /// Jump along the heading it is facing. The one move that leaves the navmesh, and therefore the one
            /// the pathfinder can never propose: a route is built from polygons that touch, and a gap has none.
            /// A seat that jumps does it on what it can see, against the route it was given.
            ACTION_JUMP,
            ACTION_COUNT
        };

        enum Obs : uint32
        {
            OBS_MOVING              = 0,
            OBS_SPEED               = 1,    // current run speed / 7 yards a second, unmounted and unhasted
            OBS_BEARING_HELD        = 2,    // one-hot over the bearings being walked (BEARING_COUNT); all 0 if none
            OBS_BEARING_NONE        = 2 + BEARING_COUNT,
            /// Which way the seat is looking, as sin and cos of its orientation. Two features rather than one angle,
            /// because an angle wraps and a network asked to learn that 6.28 is 0.01 learns a seam instead.
            OBS_FACING_SIN          = 3 + BEARING_COUNT,
            OBS_FACING_COS          = 4 + BEARING_COUNT,
            /// Whether a turn is being held, and which way. The policy has to know its own hands are on the mouse.
            OBS_TURNING_LEFT        = 5 + BEARING_COUNT,
            OBS_TURNING_RIGHT       = 6 + BEARING_COUNT,
            /// How far up or down it is looking, in the same sin/cos pair and for the same reason.
            OBS_PITCH_SIN           = 7 + BEARING_COUNT,
            OBS_PITCH_COS           = 8 + BEARING_COUNT,
            /// Where the target is, in the seat's own frame: sin and cos of the bearing to it, and its distance.
            /// All zero without one -- which is the case this block exists for.
            OBS_TARGET_BEARING_SIN  = 9 + BEARING_COUNT,
            OBS_TARGET_BEARING_COS  = 10 + BEARING_COUNT,
            OBS_TARGET_DISTANCE     = 11 + BEARING_COUNT,   // yards / 40
            /// The nearest hostile ground effect the seat is not standing in (SeatView::NearestHazard), in the same
            /// frame: which way it lies, how far, and how wide. Without this the block can dodge only what it is
            /// already burning in.
            OBS_HAZARD_BEARING_SIN  = 12 + BEARING_COUNT,
            OBS_HAZARD_BEARING_COS  = 13 + BEARING_COUNT,
            OBS_HAZARD_DISTANCE     = 14 + BEARING_COUNT,   // yards / 40
            OBS_HAZARD_RADIUS       = 15 + BEARING_COUNT,   // yards / 40
            /// Where it is trying to get to, in the same frame. TravelBlock has these too, but this block is meant
            /// to need no other block to be useful, and steering towards something is exactly its subject.
            OBS_OBJECTIVE           = 16 + BEARING_COUNT,   // there is one
            OBS_OBJECTIVE_BEARING_SIN = 17 + BEARING_COUNT,
            OBS_OBJECTIVE_BEARING_COS = 18 + BEARING_COUNT,
            OBS_OBJECTIVE_DISTANCE  = 19 + BEARING_COUNT,   // yards / 500
            /// **What the ground ahead is like along each bearing**, 1 for ground the seat could walk onto and 0
            /// for a wall or a drop (PROBE_YARDS out, judged against MAX_STEP).
            ///
            /// Without this the seat steers blind and the pathfinder silently bends every route round what it
            /// cannot see -- which is the point order coming back one layer down, having just been taken out of
            /// the action space. A seat that holds a bearing into a cliff should be able to tell that it did.
            OBS_GROUND_FIRST        = 20 + BEARING_COUNT,
            /// **How the ground changes along each bearing**, signed, / MAX_STEP and clamped: positive is a step
            /// up, negative a drop, zero flat.
            ///
            /// The reach above collapses a wall, a cliff, a lava lake and the edge of the map into one number,
            /// and this is what tells the first two apart -- which matters because they are opposite things to a
            /// pair of legs. A step up is a wall to walk round; a drop is a shortcut worth taking when it is
            /// shallow and a death when it is not, and a seat that cannot see which is which can only treat every
            /// descent as forbidden. It used to be reported straight ahead only, so seven of the eight bearings
            /// had no sign at all.
            OBS_STEP_FIRST          = 20 + 2 * BEARING_COUNT,
            /// **How far dry ground runs along each bearing**, against OBS_GROUND_FIRST's "how far anything
            /// runs" -- so the gap between the two is the width of the water that way.
            ///
            /// This was a flag: "there is water somewhere along this bearing", sampled at five points. It said
            /// nothing about how far off the shore was or how far across the water went, and the width is
            /// precisely what deciding to swim depends on. The seat had one global number for that (OBS_DETOUR,
            /// computed once when the episode was built) and nothing directional at all -- it was choosing
            /// whether to cross while unable to see how wide the crossing was.
            ///
            /// Both come from the same navmesh raycast under different filters: NAV_GROUND alone stops at the
            /// shore, NAV_GROUND | NAV_WATER swims on. Reading the pair together is the whole encoding --
            /// equal means a wall or a cliff (or a lava edge, which OBS_BURNS_FIRST names), and shore short of
            /// reach means water that many yards away.
            ///
            /// How many yards *across* it is, this does not say, and an earlier draft of this comment claimed
            /// it did. The wet filter crosses ground as well as water, so past a shore it runs on over the far
            /// bank and stops at a wall: a two yard channel and the near edge of a forty yard lake report the
            /// same thing. The bench at the Barrens oasis is what caught it (`forge rays`). Width would need a
            /// ray starting past the shore under a water-only filter, and is not measured.
            OBS_SHORE_FIRST         = 20 + 3 * BEARING_COUNT,
            /// **How near the liquid that burns is** along each bearing -- magma or slime -- as 1 at the
            /// seat's feet falling to 0 at the far end of the march, and exactly 0 where there is none.
            ///
            /// Reported apart from water because they are not the same lesson: water is somewhere to go and be
            /// slowed, and magma is somewhere to die. Both come back as no reach, so without this the seat cannot
            /// tell a lava lake from a cliff, and the arena that teaches crossing one at its narrow point has
            /// nothing to teach with.
            ///
            /// It is a distance and not a flag because a flag was a lottery. It used to be sampled from the
            /// liquid under five fixed ranges, so an edge at nine yards sat between the cells at six and twelve
            /// and reported nothing at all -- and if the twelve-yard cell landed past the edge it returned no
            /// height, which the march read as a drop. A seat could walk into lava believing it was stepping off
            /// a ledge. It now comes from a third ray whose filter may cross magma: where that one runs past the
            /// ray that may not, the shorter one stopped at the burning edge.
            OBS_BURNS_FIRST         = 20 + 4 * BEARING_COUNT,
            /// Water it is already in. Whether it is in it, whether its head is under it, and how long its head
            /// has been under -- against the breath a character has, and zero for one that does not need to
            /// breathe. Without the last of these, going in is free and "is this crossing worth it" has no
            /// downside to weigh.
            OBS_IN_WATER            = 20 + 5 * BEARING_COUNT,
            OBS_SUBMERGED           = 21 + 5 * BEARING_COUNT,
            OBS_SUBMERGED_TIME      = 22 + 5 * BEARING_COUNT,
            OBS_SWIM_SPEED          = 23 + 5 * BEARING_COUNT,   // / 7 yards a second, so under 1 means water is slower
            /// It is off the ground -- swimming or flying -- so pitch steers and the third dimension is real.
            OBS_AIRBORNE            = 24 + 5 * BEARING_COUNT,
            /// **How much longer the way round is than the way through**: the walking route to the objective over
            /// the straight line to it, / 4 and clamped. 0 without an objective, and about 0.25 (a ratio of 1)
            /// when the straight line is the route.
            ///
            /// This is the one thing a seat cannot see for itself at any probe length: that the barrier in front
            /// of it runs for two hundred yards and the way past is backwards. Measured on foot, with water and
            /// magma excluded, so it is the ground's answer and not the pathfinder's -- a player's path filter
            /// admits both, which would have this read "straight shot" across a lake or a lava field.
            OBS_DETOUR              = 25 + 5 * BEARING_COUNT,
            /// **Whether the legs are getting anywhere.** How far the seat moved over the last second against
            /// how far running would have carried it, and how much of the distance to the objective that closed.
            ///
            /// A policy has memory, but it had nothing to remember: neither of these was observable, so a seat
            /// wedged against a rock and a seat walking freely looked identical from the inside. Roughly an
            /// eighth of the episodes a trained policy loses are spent covering five times the length of the
            /// trip and ending as far away as it started.
            OBS_MOVE_RATE           = 26 + 5 * BEARING_COUNT,
            OBS_CLOSE_RATE          = 27 + 5 * BEARING_COUNT,
            /// **The last forty yards, at a resolution that can see them.** The same distance as
            /// OBS_OBJECTIVE_DISTANCE but over YARD_SCALE rather than OBJECTIVE_SCALE, so arriving
            /// (TravelBlock::ARRIVE_DISTANCE, 6 yards) sits at 0.15 instead of 0.012 and the 20-45 yard band
            /// every lost episode dies in spans half the range instead of a twelfth of it. A coarse feature and
            /// a fine one, which is the only way one number covers both five hundred yards and six.
            OBS_OBJECTIVE_NEAR      = 28 + 5 * BEARING_COUNT,
            /// **Which way it has told itself to look**, one-hot: none chosen, then the four FACE_* modes in
            /// their action order.
            ///
            /// A FACE_* is masked once it is the mode being held, so until now the action mask was the only
            /// evidence the policy had of a state it cannot otherwise perceive -- and a mask is not an
            /// observation. Facing the objective and facing where you are going are different beliefs about the
            /// world, and a seat that cannot tell which one it is holding cannot decide to stop holding it.
            OBS_FACING_MODE_FIRST   = 29 + 5 * BEARING_COUNT,
            OBS_FACING_MODE_COUNT   = 5,
            /// **How much room the seat has**: yards to the nearest edge of walkable space, over
            /// CLEARANCE_RANGE, and which way is out -- sine and cosine of the direction away from it, in the
            /// seat's own frame.
            ///
            /// The bearings say how far it could go each way; this says how close the nearest thing already is,
            /// which is a different question and the one that matters in a corridor. It is one
            /// dtNavMeshQuery::findDistanceToWall, which returns the distance, the point and a normal pointing
            /// back at the seat -- so the direction out comes free with the distance.
            ///
            /// Measured against the navmesh, which rcErodeWalkableArea already shrank by one agent radius
            /// (walkableRadius 2 cells, about 0.53 yd), and whose edges are simplified to within
            /// maxSimplificationError (1.8 yd). It is a coarse signal by construction: it shapes where the seat
            /// puts itself, and is never allowed to forbid a move -- a doorway is narrower than any margin worth
            /// keeping in open ground.
            OBS_CLEARANCE           = 34 + 5 * BEARING_COUNT,
            OBS_CLEARANCE_SIN       = 35 + 5 * BEARING_COUNT,
            OBS_CLEARANCE_COS       = 36 + 5 * BEARING_COUNT,
            /// Whether a jump would be taken if it were pressed: on the ground, not already falling, and with
            /// somewhere to land. A masked action the seat cannot see the reason for is state it cannot learn
            /// around.
            OBS_CAN_JUMP            = 37 + 5 * BEARING_COUNT,
            OBS_COUNT               = 38 + 5 * BEARING_COUNT
        };

        /// How far ahead a held bearing aims each decision. Far enough that the seat is still walking when the next
        /// decision comes (7 yards a second unhasted, so a 250 ms decision covers under two), short enough
        /// that the path is recomputed against ground the seat can see.
        static constexpr float STEP_YARDS = 8.0f;

        /// How far a held turn swings the seat each decision.
        ///
        /// This was 45 degrees -- exactly the spacing between two bearings -- which meant the turn could not do
        /// the one thing the block's own documentation claims for it. Facing starts at the seat's spawn
        /// orientation, a turn adds a multiple of 45, and a bearing subtracts one, so every heading the seat
        /// could ever walk was `spawn + k * 45`: a lattice. FACE_OBJECTIVE and FACE_TARGET were the only escapes,
        /// because they snap the facing to an exact world angle -- which is why a trained policy found exactly
        /// one strategy (face the objective, hold forward) and nothing else worked.
        ///
        /// 15 degrees a decision matches PITCH_STEP and is 60 degrees a second, well inside what a player does
        /// with a mouse. Every heading is now reachable, which is what threading a doorway off the objective's
        /// axis requires.
        static constexpr float TURN_STEP = 0.2617994f;          // 15 degrees
        /// The same for looking up and down, and how far from level it may get. Finer than the turn because pitch
        /// is a smaller range doing more: the whole useful span is a dive and a climb.
        static constexpr float PITCH_STEP = 0.2617994f;         // 15 degrees
        static constexpr float PITCH_MAX = 1.0471976f;          // 60 degrees
        /// How far ahead the ground is read along each bearing, and the height change a seat can walk up or drop
        /// down without it counting as a wall.
        ///
        /// PROBE_YARDS was one sample, twelve yards out, compared against the seat's own height: "is the point
        /// twelve yards that way roughly level with me". That collapses a gentle rise into a wall -- three
        /// yards of climb over twelve reads as no reach at all -- and it cannot see anything at thirteen. It is
        /// kept as the nearest march cell and as the manifest's idea of a probe.
        static constexpr float PROBE_YARDS = 12.0f;
        static constexpr float MAX_STEP = 2.5f;
        /// **How far the seat can see along a bearing, and where it looks on the way.** Five cells rather than
        /// one, each judged against the cell before it, so a slope is a slope and only a real step is a step.
        /// What is reported is the distance to the first thing that stops the ray, which is a number that means
        /// the same at six yards and at forty -- unlike the old reach, where a wall and a cliff and the edge of
        /// the map were all 0 and everything else was 1.
        ///
        /// Geometry, not a route: the march says what is there, and choosing a bearing stays the policy's job.
        static constexpr uint32 MARCH_CELLS = 5;
        static constexpr float MARCH_RANGES[MARCH_CELLS] = { 6.0f, 12.0f, 20.0f, 30.0f, 40.0f };
        static constexpr float MARCH_MAX = 40.0f;
        /// When a march stops describing where the seat is. Forty map queries is too many to repeat every
        /// decision, and it does not have to be repeated: the ground does not move. Redone when the seat has
        /// walked MARCH_REFRESH_YARDS from where it was marched, turned MARCH_REFRESH_RADIANS from the heading
        /// it was marched along, or MARCH_REFRESH_MS have passed -- movement first, because at seven yards a
        /// second a clock alone goes stale inside the nearest cell.
        static constexpr float MARCH_REFRESH_YARDS = 3.0f;
        static constexpr float MARCH_REFRESH_RADIANS = 0.3926991f;      // half a bearing, 22.5 degrees
        static constexpr uint32 MARCH_REFRESH_MS = 500;
        /// How far up or down the ground is looked for at a march cell, and how much of a rise or drop between
        /// two cells is still walkable.
        ///
        /// The old probe looked for ground only within MAX_STEP of the seat and called everything else a wall,
        /// which is why broken ground read as cliffs in every direction: three yards of climb over twelve was
        /// "no reach at all". A cell is judged against the cell before it now, and the allowance grows with the
        /// gap between them -- MAX_STEP for the discontinuity a step really is, plus MARCH_SLOPE for the ground
        /// simply going uphill. Over a six yard gap that admits 5.5 yards of rise; over ten, 7.5.
        static constexpr float MARCH_SEARCH = 20.0f;
        static constexpr float MARCH_SLOPE = 0.5f;
        /// How far out clearance is measured and reported against. Kept small on purpose: findDistanceToWall
        /// searches outward through the polygon graph and the shared query has a 1024-node pool, and room
        /// beyond a few yards is not a thing a seat needs to tell apart.
        static constexpr float CLEARANCE_RANGE = 8.0f;
        /// A player's jump, which is the only one worth having: up at JUMP_SPEED_Z against
        /// Movement::gravity (19.29) is an apex of about 1.64 yards, and carried forward at run speed it covers
        /// about 5.8. That is the whole envelope.
        ///
        /// It is worth being plain about what that buys. The navmesh is built with walkableClimb 6 cells --
        /// about 1.60 yards -- so it already assumes the seat can step up everything a jump could clear, and
        /// jumping gains almost nothing vertically. What it gains is horizontal: a gap the mesh does not bridge,
        /// which no path will ever cross because off-mesh connections are the only thing that could and the
        /// shipped config declares two in the whole world.
        static constexpr float JUMP_SPEED_Z = 7.955f;

        /// How much further the ray that may cross magma must run than the ray that may not, before the gap
        /// between them is called a burning edge rather than float noise. Both rays start from one polygon and
        /// share the mesh's 1.8 yd simplification error, so that error cancels and this only has to cover the
        /// arithmetic.
        static constexpr float BURN_EDGE_MARGIN = 0.5f;

        /// The shortest jump worth making, and the shortest one that is safe to build.
        ///
        /// Both halves matter. A jump of a few inches is not a move, and a jump of none at all is a spline with
        /// no length, whose duration is zero and whose position is then whatever dividing by it produces.
        static constexpr float JUMP_MIN_YARDS = 1.0f;
        /// What a character's breath is worth, for OBS_SUBMERGED_TIME. A held breath is about a minute in this
        /// expansion; the number only has to be the right size for the feature to mean something.
        static constexpr float BREATH_SECONDS = 60.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Move; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        /// What the navmesh senses read standing at one point, as a table, for a console to print.
        ///
        /// This exists because every one of those senses is a Detour call, Detour's axes are {y, z, x} rather
        /// than the world's, and a swizzle that is wrong is completely silent: the rays simply go somewhere
        /// else and come back with plausible numbers about the wrong place. Nothing downstream can catch it.
        /// An eval cannot catch it either -- it would show only as a policy that learns worse than it should,
        /// after hours.
        ///
        /// So the rays are run here against geometry whose answer is already known -- a wall at a measured
        /// distance, a corridor, the width of a lake that has been swum -- and read directly. It calls the same
        /// NavRay and findDistanceToWall the probe calls, from the same kind of start polygon, so what it
        /// prints is what a seat standing there would sense and not a second implementation of it.
        static std::string RayReport(Map* map, float x, float y, float z, float facing);

        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// Every bearing and the halt are movement: they take the movement repeat pacing and are never charged for
        /// repeating (Actions.Repeat is not levied on movement). The facing actions are not -- turning to look at
        /// something is a press like any other, and a policy that spams it should pay.
        /// Every bearing and the halt are movement, and so are the held turn and the held pitch: they take the
        /// movement repeat pacing and are never charged for repeating (Actions.Repeat is not levied on movement).
        /// Charging a held key for being held is exactly the mistake the repeat charge exists to avoid. The four
        /// facing actions are not -- turning to look at something once is a press like any other, and a policy that
        /// spams it should pay.
        [[nodiscard]] bool IsMovement(uint32 local) const override
        {
            return local <= ACTION_HALT || (local >= ACTION_TURN_LEFT && local <= ACTION_JUMP);
        }

        /// A bearing has no fixed sense of toward or away: which way BEARING_FORWARD leads depends on where the seat
        /// is looking. The reverse-move pacing therefore does not apply, and 0 is the honest answer. IsMovement is
        /// what marks these as movement; MoveDirection only says which way a *target-relative* order went.
        [[nodiscard]] int8 MoveDirection(uint32 /*local*/) const override { return 0; }
    };
}

#endif
