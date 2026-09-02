#pragma once

#include "asset/asset.h"
#include "core/common.h"
#include "core/config.h"
#include "math/linalg.h"
#include "platform/input.h"
#include "scene/entity.h"

// The game's player, and the first thing in src/game: everything the engine
// deliberately has no opinion about. How fast walking is, what gravity is worth
// here, whether there is a sprint at all, where the camera sits, which actions
// drive any of it -- all decisions about this game, none of them the engine's.
//
// It is a plain struct rather than a component because component types are
// declared in engine code today, so a game cannot add one. That is a real limit
// and worth fixing, but not by putting the player in the engine. When the
// registry opens up this becomes a component, and when there is a scripting
// language it becomes a script. Neither move changes the boundary it sits
// behind: read input, choose a velocity, ask the engine to move a body, place a
// camera.
//
// The body must be a root entity. Movement is resolved in world space, and a
// parented body would have its result written back as a local offset.

namespace game {

struct Player {
    entity::Handle body;

    f32 move_speed;   // metres per second on the ground
    f32 sprint_scale; // multiplied in while sprint is held

    f32 look_sensitivity; // degrees per pixel of mouse movement
    b32 look_invert_y;
    f32 pitch_min; // degrees; negative looks up
    f32 pitch_max;

    // The body is swept as a sphere, so this is half a shoulder width.
    f32 radius;

    f32 gravity; // metres per second squared, downward

    // Apex of a standing jump. The launch speed is derived from it and gravity,
    // so tuning one does not quietly change the other.
    f32 jump_height;

    // Stand-in floor, until a level carries geometry to stand on. A level with a
    // real floor puts this under it and never thinks about it again.
    f32 ground_height;

    // The camera's place in the body's yawed frame: straight up for first
    // person, up and behind for third. This is the whole difference between
    // them -- neither is a mode the code knows about.
    linalg::Vec3 eye_offset;

    linalg::Vec3 velocity;
    f32 yaw;
    f32 pitch;
    b32 grounded;

    input::Action move_forward;
    input::Action move_back;
    input::Action move_left;
    input::Action move_right;
    input::Action jump;
    input::Action sprint;
};

// The `player` block of engine.cfg, with the actions it drives resolved once.
// An action the input map does not declare is reported there and then never
// fires, so a missing line costs that one control and nothing else.
Player create(config::Node *block, input::State *in, entity::Handle body);

// Aim, at frame rate: a mouse delta is per frame by nature, and spreading one
// across a variable number of fixed steps would tie sensitivity to frame rate.
void aim(Player *p, input::State *in);

// One fixed step: choose a velocity, apply gravity, let the engine resolve the
// move. `in` may be NULL, which is a player standing still under gravity.
void step(Player *p, entity::World *w, asset::Manager *m, input::State *in, f32 dt);

// Places the level's active camera, after the fixed steps have moved the body.
void place_camera(const Player *p, entity::World *w);

} // namespace game
