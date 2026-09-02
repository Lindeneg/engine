#include "game/player.h"

#include <math.h>

#include "core/logger.h"
#include "scene/motion.h"

#define DEFAULT_MOVE_SPEED 4.0f
#define DEFAULT_SPRINT_SCALE 1.8f
#define DEFAULT_LOOK_SENSITIVITY 0.12f
#define DEFAULT_PITCH_MIN -89.0f
#define DEFAULT_PITCH_MAX 89.0f
#define DEFAULT_RADIUS 0.35f
#define DEFAULT_GRAVITY 9.81f
#define DEFAULT_JUMP_HEIGHT 1.1f
#define DEFAULT_GROUND_HEIGHT 0.0f

namespace game {

// eyes of someone about 1.8 m tall, looking forward
static const linalg::Vec3 DEFAULT_EYE_OFFSET = {0.0f, 1.65f, 0.0f};

Player create(config::Node *block, input::State *in, entity::Handle body) {
    ASSERT(in);

    config::Node *b = block ? block->first_child : NULL;

    Player p{};
    p.body = body;

    p.move_speed = config::get_f32(b, "move_speed", DEFAULT_MOVE_SPEED);
    p.sprint_scale = config::get_f32(b, "sprint_scale", DEFAULT_SPRINT_SCALE);

    p.look_sensitivity = config::get_f32(b, "look_sensitivity", DEFAULT_LOOK_SENSITIVITY);
    p.look_invert_y = config::get_b32(b, "look_invert_y", false);
    p.pitch_min = config::get_f32(b, "pitch_min", DEFAULT_PITCH_MIN);
    p.pitch_max = config::get_f32(b, "pitch_max", DEFAULT_PITCH_MAX);

    p.radius = config::get_f32(b, "radius", DEFAULT_RADIUS);
    p.gravity = config::get_f32(b, "gravity", DEFAULT_GRAVITY);
    p.jump_height = config::get_f32(b, "jump_height", DEFAULT_JUMP_HEIGHT);
    p.ground_height = config::get_f32(b, "ground_height", DEFAULT_GROUND_HEIGHT);

    p.eye_offset = config::value_vec3(config::find(b, "eye_offset"), DEFAULT_EYE_OFFSET);

    if (p.pitch_min > p.pitch_max) {
        LOG_WARN("player: pitch_min %.1f is above pitch_max %.1f; swapping them", (f64)p.pitch_min,
                 (f64)p.pitch_max);
        f32 swap = p.pitch_min;
        p.pitch_min = p.pitch_max;
        p.pitch_max = swap;
    }

    p.move_forward = input::find(in, "move_forward");
    p.move_back = input::find(in, "move_back");
    p.move_left = input::find(in, "move_left");
    p.move_right = input::find(in, "move_right");
    p.jump = input::find(in, "jump");
    p.sprint = input::find(in, "sprint");

    return p;
}

void aim(Player *p, input::State *in) {
    ASSERT(p && in);

    // with the cursor loose the mouse belongs to whatever is on screen
    if (!input::mouse_look(in))
        return;

    linalg::Vec2 d = input::cursor_delta(in);

    p->yaw -= d.x * p->look_sensitivity;
    p->pitch += (p->look_invert_y ? -d.y : d.y) * p->look_sensitivity;

    // one turn is enough; a long session should not spend precision on laps
    while (p->yaw >= 360.0f)
        p->yaw -= 360.0f;
    while (p->yaw < 0.0f)
        p->yaw += 360.0f;

    if (p->pitch < p->pitch_min)
        p->pitch = p->pitch_min;
    if (p->pitch > p->pitch_max)
        p->pitch = p->pitch_max;
}

void step(Player *p, entity::World *w, asset::Manager *m, input::State *in, f32 dt) {
    ASSERT(p && w && m);

    entity::Entity *e = entity::get(w, p->body);
    if (!e) {
        LOG_WARN_ONCE("player: the body entity is gone");
        return;
    }

    if (e->parent.v)
        LOG_WARN_ONCE("player: \"%s\" is parented; movement is resolved in world space", e->name);

    // -Z is forward, in the body's own frame
    linalg::Vec3 wish{};
    if (in) {
        if (input::down(in, p->move_forward))
            wish.z -= 1.0f;
        if (input::down(in, p->move_back))
            wish.z += 1.0f;
        if (input::down(in, p->move_left))
            wish.x -= 1.0f;
        if (input::down(in, p->move_right))
            wish.x += 1.0f;
    }

    // normalized, or walking a diagonal would be faster than walking straight
    wish = linalg::vec3_normalize_or(wish, linalg::Vec3{0.0f, 0.0f, 0.0f});

    linalg::Quat yaw = linalg::quat_from_euler(linalg::Vec3{0.0f, p->yaw, 0.0f});
    linalg::Vec3 dir = linalg::quat_rotate(yaw, wish);

    f32 speed = p->move_speed;
    if (in && input::down(in, p->sprint))
        speed *= p->sprint_scale;

    p->velocity.x = dir.x * speed;
    p->velocity.z = dir.z * speed;

    // consume_pressed rather than pressed: this runs on the fixed clock, and a
    // frame that ran two steps must not jump twice
    if (p->grounded && in && input::consume_pressed(in, p->jump) && p->gravity > 0.0f)
        p->velocity.y = sqrtf(2.0f * p->gravity * p->jump_height);

    p->velocity.y -= p->gravity * dt;

    linalg::Vec3 motion = p->velocity * dt;

    // The stand-in floor limits the move rather than the result. Clamping the
    // resolved position instead was a teleport that ignored geometry, and it
    // put the body straight inside anything lying on the floor.
    f32 floor = p->ground_height + p->radius;
    b32 on_floor = false;
    if (e->local.position.y + motion.y < floor) {
        motion.y = floor - e->local.position.y;
        on_floor = true;
    }

    motion::Result r = motion::move_and_slide(w, m, e->local.position, p->radius, motion, p->body);

    p->grounded = r.grounded || on_floor;

    if (on_floor && p->velocity.y < 0.0f)
        p->velocity.y = 0.0f;

    // cancel the velocity into whatever stopped it, or gravity builds up against
    // a floor and walking into a wall keeps accelerating
    if (r.hit)
        p->velocity = p->velocity - r.normal * linalg::vec3_dot(p->velocity, r.normal);

    // wedged: without this, gravity keeps adding to a velocity that is going
    // nowhere, and the body is launched the moment it comes free
    if (r.stuck) {
        LOG_WARN_ONCE("player: wedged in geometry at %.2f %.2f %.2f", (f64)r.position.x,
                      (f64)r.position.y, (f64)r.position.z);
        p->velocity = linalg::Vec3{0.0f, 0.0f, 0.0f};
    }

    e->local.position = r.position;

    // the body carries the yaw; pitch stays on the camera, so looking up does
    // not tip the model onto its back
    entity::set_euler(&e->local, linalg::Vec3{0.0f, p->yaw, 0.0f});
}

void place_camera(const Player *p, entity::World *w) {
    ASSERT(p && w);

    entity::Entity *e = entity::get(w, p->body);
    if (!e)
        return;

    entity::Entity *ce = entity::get(w, entity::active_camera(w));
    if (!ce) {
        LOG_WARN_ONCE("player: the level has no active camera to drive");
        return;
    }

    if (ce->parent.v)
        LOG_WARN_ONCE("player: camera \"%s\" is parented; it is placed in world space anyway",
                      ce->name);

    linalg::Quat yaw = linalg::quat_from_euler(linalg::Vec3{0.0f, p->yaw, 0.0f});

    ce->local.position = e->local.position + linalg::quat_rotate(yaw, p->eye_offset);
    entity::set_euler(&ce->local, linalg::Vec3{p->pitch, p->yaw, 0.0f});
}

} // namespace game
