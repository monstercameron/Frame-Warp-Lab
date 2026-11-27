#include "tactical_range.h"
#include "ecs.h"
#include "event_bus.h"
#include "job_system.h"
#include "lua_engine.h"
#include "range_mesh.h"
#include "skinning.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

namespace frame_warp_lab {

namespace {

namespace Layers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM_LAYERS(2);
}

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        switch (a) {
            case Layers::NON_MOVING: return b == Layers::MOVING;
            case Layers::MOVING: return true;
            default: return false;
        }
    }
};

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        map_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        map_[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return map_[layer]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(layer)) {
            case 0: return "NON_MOVING";
            case 1: return "MOVING";
            default: return "INVALID";
        }
    }
#endif
private:
    JPH::BroadPhaseLayer map_[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        switch (a) {
            case Layers::NON_MOVING: return b == BroadPhaseLayers::MOVING;
            case Layers::MOVING: return true;
            default: return false;
        }
    }
};

struct TargetPhysicsBinding {
    EntityHandle entity = k_invalid_entity;
    Float3 base_pos{};
    JPH::BodyID anchor_id{};
    JPH::BodyID plate_id{};
    JPH::Ref<JPH::HingeConstraint> hinge;
};

struct CasingPhysicsBinding {
    EntityHandle entity = k_invalid_entity;
    JPH::BodyID body_id{};
};

struct BotRagdollState {
    bool active = false;
    float age = 0.0f;
    std::array<JPH::BodyID, k_ragdoll_limb_count> bodies{};
    std::array<JPH::Ref<JPH::Constraint>, k_ragdoll_limb_count - 1U> constraints{};
};

class PlayerCollisionBodyFilter final : public JPH::BodyFilter {
public:
    explicit PlayerCollisionBodyFilter(
        const std::array<CasingPhysicsBinding, k_range_casing_count>& casings,
        const std::array<BotRagdollState, 8>* ragdolls = nullptr)
        : casings_(casings), ragdolls_(ragdolls) {}

    bool ShouldCollide(const JPH::BodyID& id) const override {
        for (const auto& c : casings_) {
            if (!c.body_id.IsInvalid() && c.body_id == id) {
                return false;
            }
        }
        if (ragdolls_ != nullptr) {
            for (const auto& rd : *ragdolls_) {
                if (!rd.active) continue;
                for (const auto& bid : rd.bodies) {
                    if (!bid.IsInvalid() && bid == id) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

private:
    const std::array<CasingPhysicsBinding, k_range_casing_count>& casings_;
    const std::array<BotRagdollState, 8>* ragdolls_;
};

Mat4x4 identity_mat4() {
    Mat4x4 r{};
    r.m[0] = 1.0f;
    r.m[5] = 1.0f;
    r.m[10] = 1.0f;
    r.m[15] = 1.0f;
    return r;
}

Mat4x4 mul_mat4(const Mat4x4& a, const Mat4x4& b) {
    Mat4x4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

Mat4x4 make_translation_mat4(float x, float y, float z) {
    Mat4x4 r = identity_mat4();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4x4 make_scale_mat4(float sx, float sy, float sz) {
    Mat4x4 r{};
    r.m[0] = sx;
    r.m[5] = sy;
    r.m[10] = sz;
    r.m[15] = 1.0f;
    return r;
}

Mat4x4 make_rot_x(float a) {
    Mat4x4 r = identity_mat4();
    const float c = std::cos(a);
    const float s = std::sin(a);
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

Mat4x4 make_rot_y(float a) {
    Mat4x4 r = identity_mat4();
    const float c = std::cos(a);
    const float s = std::sin(a);
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

Mat4x4 make_rot_z(float a) {
    Mat4x4 r = identity_mat4();
    const float c = std::cos(a);
    const float s = std::sin(a);
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    return r;
}

Mat4x4 from_jolt_mat44(const JPH::RMat44& jm) {
    Mat4x4 r{};
    for (int col = 0; col < 4; ++col) {
        const JPH::Vec4 c = jm.GetColumn4(col);
        r.m[col * 4 + 0] = c.GetX();
        r.m[col * 4 + 1] = c.GetY();
        r.m[col * 4 + 2] = c.GetZ();
        r.m[col * 4 + 3] = c.GetW();
    }
    return r;
}

Mat4x4 camera_basis_to_world_mat4(const CameraBasis& cam) {
    Mat4x4 r = identity_mat4();
    r.m[0] = cam.right.x;
    r.m[1] = cam.right.y;
    r.m[2] = cam.right.z;
    r.m[4] = cam.up.x;
    r.m[5] = cam.up.y;
    r.m[6] = cam.up.z;
    r.m[8] = cam.forward.x;
    r.m[9] = cam.forward.y;
    r.m[10] = cam.forward.z;
    r.m[12] = cam.position.x;
    r.m[13] = cam.position.y;
    r.m[14] = cam.position.z;
    return r;
}

Mat4x4 make_segment_mat4(Float3 a, Float3 b, float radius) {
    const Float3 d{b.x - a.x, b.y - a.y, b.z - a.z};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-4f) return make_scale_mat4(0.0f, 0.0f, 0.0f);
    const Float3 fwd{d.x / len, d.y / len, d.z / len};
    const Float3 up_ref = std::fabs(fwd.y) < 0.95f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
    Float3 right{
        up_ref.y * fwd.z - up_ref.z * fwd.y,
        up_ref.z * fwd.x - up_ref.x * fwd.z,
        up_ref.x * fwd.y - up_ref.y * fwd.x};
    const float rlen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    right = {right.x / std::max(rlen, 1e-5f), right.y / std::max(rlen, 1e-5f), right.z / std::max(rlen, 1e-5f)};
    const Float3 up{
        fwd.y * right.z - fwd.z * right.y,
        fwd.z * right.x - fwd.x * right.z,
        fwd.x * right.y - fwd.y * right.x};
    const Float3 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
    Mat4x4 r = identity_mat4();
    r.m[0] = right.x * radius;
    r.m[1] = right.y * radius;
    r.m[2] = right.z * radius;
    r.m[4] = up.x * radius;
    r.m[5] = up.y * radius;
    r.m[6] = up.z * radius;
    r.m[8] = fwd.x * len;
    r.m[9] = fwd.y * len;
    r.m[10] = fwd.z * len;
    r.m[12] = mid.x;
    r.m[13] = mid.y;
    r.m[14] = mid.z;
    return r;
}

Float3 transform_point(const Mat4x4& m, Float3 p) {
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
    };
}

void ensure_jolt_initialized() {
    static bool initialized = false;
    if (!initialized) {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        initialized = true;
    }
}

constexpr std::array<Float3, k_range_target_count> k_target_bases = {{
    { 0.0f, 0.14f,  9.8f},
    {-2.5f, 0.14f, 10.2f},
    { 2.5f, 0.14f, 10.2f},
    {-3.6f, 0.14f, 15.8f},
    {-1.0f, 0.14f, 16.2f},
    { 2.1f, 0.14f, 16.0f},
    {-2.1f, 0.14f, 22.6f},
    { 2.6f, 0.14f, 22.8f},
}};

std::uint32_t choose_jolt_worker_count() {
    const std::uint32_t hw = std::max(1U, std::thread::hardware_concurrency());
    return std::clamp<std::uint32_t>(hw / 2U, 2U, 6U);
}

}

struct JoltRuntimeGuard {
    JoltRuntimeGuard() {
        ensure_jolt_initialized();
    }
};

struct TacticalRangeSimulation::Impl {
    JoltRuntimeGuard jolt_guard;
    BPLayerInterfaceImpl bp_interface;
    ObjectVsBroadPhaseLayerFilterImpl obj_vs_bp_filter;
    ObjectLayerPairFilterImpl obj_pair_filter;
    JPH::TempAllocatorImpl temp_allocator{4U * 1024U * 1024U};
    std::uint32_t jolt_worker_count = choose_jolt_worker_count();
    JPH::JobSystemThreadPool job_system{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(jolt_worker_count)};
    JPH::PhysicsSystem physics;

    EcsWorld ecs;
    EventBus events;
    LuaScriptEngine lua;

    EntityHandle player_entity = k_invalid_entity;
    EntityHandle range_world_entity = k_invalid_entity;
    EntityHandle director_entity = k_invalid_entity;
    EntityHandle weapon_entity = k_invalid_entity;
    EntityHandle pistol_frame_entity = k_invalid_entity;
    EntityHandle pistol_slide_entity = k_invalid_entity;
    EntityHandle pistol_barrel_entity = k_invalid_entity;
    EntityHandle pistol_hammer_entity = k_invalid_entity;
    EntityHandle pistol_trigger_entity = k_invalid_entity;
    EntityHandle pistol_mag_entity = k_invalid_entity;
    EntityHandle pistol_sights_entity = k_invalid_entity;
    EntityHandle weapon_smg_entity = k_invalid_entity;
    EntityHandle weapon_rifle_entity = k_invalid_entity;
    EntityHandle weapon_scout_entity = k_invalid_entity;
    EntityHandle weapon_shotgun_entity = k_invalid_entity;
    EntityHandle weapon_dual_right_entity = k_invalid_entity;
    EntityHandle weapon_dual_left_entity = k_invalid_entity;
    EntityHandle cs_match_entity = k_invalid_entity;
    std::array<EntityHandle, 8> bot_entities{};
    std::array<EntityHandle, 8> bot_weapon_entities{};
    std::array<BotRagdollState, 8> bot_ragdolls{};
    std::array<std::array<RagdollLimbPose, k_ragdoll_limb_count>, 8> bot_last_limb_poses{};
    std::array<bool, 8> bot_has_limb_poses{};
    std::array<Float3, 8> bot_hand_r_local{};
    JPH::Ref<JPH::GroupFilterTable> ragdoll_group_filter;
    EntityHandle muzzle_flash_entity = k_invalid_entity;
    EntityHandle tracer_entity = k_invalid_entity;
    EntityHandle spark_entity = k_invalid_entity;

    std::array<TargetPhysicsBinding, k_range_target_count> targets{};
    std::array<CasingPhysicsBinding, k_range_casing_count> casings{};
    std::vector<JPH::BodyID> range_static_bodies;
    JPH::BodyID dust2_mesh_body{};
    bool dust2_world_active = false;
    bool range_bodies_in_world = true;
    bool dust2_body_in_world = false;

    bool hero_pistol_gltf_loaded = false;
    Float3 pistol_slide_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_hammer_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_trigger_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_magazine_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_muzzle_offset{0.0f, 0.042f, 0.145f};
    Float3 pistol_eject_offset{0.028f, 0.050f, 0.025f};
    float pistol_ads_sight_y = 0.068f;
    std::array<Float3, k_cs_weapon_count> weapon_muzzle_offsets{};
    std::array<Float3, k_cs_weapon_count> weapon_eject_offsets{};
    std::array<float, k_cs_weapon_count> weapon_ads_sight_y{};
    std::array<bool, k_cs_weapon_count> weapon_loaded{};
    const SkinnedCharacterTemplate* ct_bot_template = nullptr;
    const SkinnedCharacterTemplate* t_bot_template = nullptr;
    std::array<std::uint32_t, 8> bot_vb_offsets{};
    std::array<std::uint32_t, 8> bot_vb_counts{};
    std::uint32_t total_bot_vertices = 0U;
    std::vector<Float3> dust2_waypoints;
    std::int32_t bot_kills = 0;

    double phys_accum = 0.0;
    float range_time = 0.0f;

    std::int32_t hits = 0;
    std::int32_t headshots = 0;
    std::int32_t score = 0;

    CameraBasis active_camera{};
    Float3 prev_camera_pos{0.0f, 1.55f, 1.4f};
    bool has_prev_camera_pos = false;
    bool has_prev_pistol_local = false;
    std::int32_t prev_rendered_weapon_id = -1;
    Mat4x4 prev_gun_local{};
    Mat4x4 prev_dual_left_local{};
    Mat4x4 prev_slide_local{};
    Mat4x4 prev_hammer_local{};
    Mat4x4 prev_trigger_local{};
    Mat4x4 prev_mag_local{};
    Float3 muzzle_world_pos{0.0f, 1.4f, 2.0f};
    float tracer_timer = 0.0f;
    Float3 tracer_start{};
    Float3 tracer_end{};
    float impact_timer = 0.0f;
    Float3 impact_pos{};
    bool impact_was_target = false;
    float hit_marker_timer = 0.0f;
    bool hit_marker_headshot = false;
    std::int64_t last_physics_step_us = 0;
    std::int64_t last_ecs_sync_us = 0;

    Impl() {
        ensure_jolt_initialized();
        physics.Init(1024, 0, 2048, 2048, bp_interface, obj_vs_bp_filter, obj_pair_filter);
        physics.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        ragdoll_group_filter = new JPH::GroupFilterTable(k_ragdoll_limb_count);
        for (JPH::CollisionGroup::SubGroupID a = 0; a < k_ragdoll_limb_count; ++a) {
            for (JPH::CollisionGroup::SubGroupID b = a + 1; b < k_ragdoll_limb_count; ++b) {
                ragdoll_group_filter->DisableCollision(a, b);
            }
        }
        build_physics_and_ecs();
        register_engine_event_bridges();
        initialize_lua();
    }

    ~Impl() {
        destroy_all_ragdolls();
    }

    void destroy_bot_ragdoll(std::size_t bi) {
        if (bi >= bot_ragdolls.size()) return;
        BotRagdollState& rd = bot_ragdolls[bi];
        if (!rd.active) return;
        for (auto& c : rd.constraints) {
            if (c != nullptr) {
                physics.RemoveConstraint(c);
                c = nullptr;
            }
        }
        auto& body_if = physics.GetBodyInterface();
        for (auto& bid : rd.bodies) {
            if (!bid.IsInvalid()) {
                body_if.RemoveBody(bid);
                body_if.DestroyBody(bid);
                bid = JPH::BodyID();
            }
        }
        rd.active = false;
        rd.age = 0.0f;
    }

    void destroy_all_ragdolls() {
        for (std::size_t bi = 0; bi < bot_ragdolls.size(); ++bi) {
            destroy_bot_ragdoll(bi);
        }
    }

    void spawn_bot_ragdoll(std::size_t bi, Float3 hit_dir, float damage, bool headshot) {
        if (bi >= bot_ragdolls.size()) return;
        destroy_bot_ragdoll(bi);
        const BotComponent* bc = ecs.bots().try_get(bot_entities[bi]);
        const SkinnedCharacterTemplate* tpl = (bi < 4U) ? ct_bot_template : t_bot_template;
        if (bc == nullptr || tpl == nullptr || !tpl->loaded) return;

        const Mat4x4 bot_world = mul_mat4(
            make_translation_mat4(bc->pos.x, bc->pos.y, bc->pos.z),
            make_rot_y(bc->yaw));
        const JPH::Quat bot_q = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), bc->yaw);

        struct LimbSpec {
            bool is_sphere;
            float r;
            float hh;
            float mass;
            bool horiz_x;
        };
        const std::array<LimbSpec, k_ragdoll_limb_count> specs = {{
            {false, 0.13f, 0.06f, 16.0f, false},
            {false, 0.14f, 0.14f, 22.0f, false},
            {true,  0.115f, 0.0f,  5.0f, false},
            {true,  0.075f, 0.0f,  3.5f, false},
            {true,  0.065f, 0.0f,  2.5f, false},
            {true,  0.075f, 0.0f,  3.5f, false},
            {true,  0.065f, 0.0f,  2.5f, false},
            {false, 0.075f, 0.14f, 9.0f, false},
            {false, 0.062f, 0.14f, 5.5f, false},
            {false, 0.075f, 0.14f, 9.0f, false},
            {false, 0.062f, 0.14f, 5.5f, false},
        }};

        auto& body_if = physics.GetBodyInterface();
        BotRagdollState& rd = bot_ragdolls[bi];
        std::array<JPH::Body*, k_ragdoll_limb_count> created_bodies{};
        std::array<JPH::RVec3, k_ragdoll_limb_count> world_centers{};
        std::array<JPH::Quat, k_ragdoll_limb_count> world_rots{};

        for (std::uint32_t li = 0; li < k_ragdoll_limb_count; ++li) {
            RagdollLimbPose lp{};
            if (bot_has_limb_poses[bi]) {
                lp = bot_last_limb_poses[bi][li];
            } else {
                lp.pos = tpl->ragdoll_rest_center[li];
            }
            const Float3 wp = transform_point(bot_world, lp.pos);
            const JPH::Quat lq = JPH::Quat(lp.qx, lp.qy, lp.qz, lp.qw).Normalized();
            const JPH::Quat wq = (bot_q * lq).Normalized();
            world_centers[li] = JPH::RVec3(wp.x, wp.y, wp.z);
            world_rots[li] = wq;

            JPH::RefConst<JPH::Shape> shape;
            if (specs[li].is_sphere) {
                shape = new JPH::SphereShape(specs[li].r);
            } else {
                shape = new JPH::CapsuleShape(specs[li].hh, specs[li].r);
            }
            JPH::BodyCreationSettings bcs(
                shape,
                world_centers[li],
                wq,
                JPH::EMotionType::Dynamic,
                Layers::MOVING);
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = specs[li].mass;
            bcs.mLinearDamping = 0.28f;
            bcs.mAngularDamping = 0.55f;
            bcs.mFriction = 0.78f;
            bcs.mRestitution = 0.06f;
            bcs.mCollisionGroup = JPH::CollisionGroup(
                ragdoll_group_filter,
                static_cast<JPH::CollisionGroup::GroupID>(100U + bi),
                static_cast<JPH::CollisionGroup::SubGroupID>(li));
            JPH::Body* body = body_if.CreateBody(bcs);
            if (body != nullptr) {
                body_if.AddBody(body->GetID(), JPH::EActivation::Activate);
                rd.bodies[li] = body->GetID();
                created_bodies[li] = body;
            }
        }

        struct JointDef {
            std::uint32_t parent;
            std::uint32_t child;
            float cone_rad;
            float twist_rad;
        };
        const std::array<JointDef, k_ragdoll_limb_count - 1U> jdefs = {{
            {0, 1, 0.38f, 0.32f},
            {1, 2, 0.45f, 0.45f},
            {1, 3, 0.85f, 0.55f},
            {3, 4, 0.65f, 0.25f},
            {1, 5, 0.85f, 0.55f},
            {5, 6, 0.65f, 0.25f},
            {0, 7, 0.72f, 0.30f},
            {7, 8, 0.68f, 0.15f},
            {0, 9, 0.72f, 0.30f},
            {9, 10, 0.68f, 0.15f},
        }};

        for (std::size_t ji = 0; ji < jdefs.size(); ++ji) {
            const auto& jd = jdefs[ji];
            JPH::Body* pb = created_bodies[jd.parent];
            JPH::Body* cb = created_bodies[jd.child];
            if (pb == nullptr || cb == nullptr) continue;
            const Float3 rest_joint = tpl->ragdoll_rest_joint_pos[jd.child];
            const Float3 rest_c = tpl->ragdoll_rest_center[jd.child];
            const JPH::Vec3 off(rest_joint.x - rest_c.x, rest_joint.y - rest_c.y, rest_joint.z - rest_c.z);
            const JPH::RVec3 pivot = world_centers[jd.child] + world_rots[jd.child] * off;
            JPH::Vec3 twist_axis = JPH::Vec3(world_centers[jd.child] - world_centers[jd.parent]);
            if (twist_axis.LengthSq() < 1e-5f) {
                twist_axis = JPH::Vec3::sAxisY();
            } else {
                twist_axis = twist_axis.Normalized();
            }
            JPH::Vec3 ref_axis = std::fabs(twist_axis.GetY()) < 0.9f ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisX();
            JPH::Vec3 plane_axis = twist_axis.Cross(ref_axis).Normalized();

            JPH::SwingTwistConstraintSettings cs;
            cs.mSpace = JPH::EConstraintSpace::WorldSpace;
            cs.mPosition1 = pivot;
            cs.mPosition2 = pivot;
            cs.mTwistAxis1 = twist_axis;
            cs.mTwistAxis2 = twist_axis;
            cs.mPlaneAxis1 = plane_axis;
            cs.mPlaneAxis2 = plane_axis;
            cs.mNormalHalfConeAngle = jd.cone_rad;
            cs.mPlaneHalfConeAngle = jd.cone_rad;
            cs.mTwistMinAngle = -jd.twist_rad;
            cs.mTwistMaxAngle = jd.twist_rad;
            cs.mMaxFrictionTorque = 6.5f;
            JPH::Constraint* c = cs.Create(*pb, *cb);
            if (c != nullptr) {
                physics.AddConstraint(c);
                rd.constraints[ji] = c;
            }
        }

        const float dlen = std::sqrt(hit_dir.x * hit_dir.x + hit_dir.y * hit_dir.y + hit_dir.z * hit_dir.z);
        const Float3 dn = dlen > 1e-4f
            ? Float3{hit_dir.x / dlen, hit_dir.y / dlen, hit_dir.z / dlen}
            : Float3{0.0f, 0.2f, 1.0f};
        const float strength = std::clamp(damage * 0.65f, 38.0f, 115.0f);
        if (headshot && !rd.bodies[2].IsInvalid()) {
            body_if.AddImpulse(rd.bodies[2], JPH::Vec3(dn.x * strength * 0.55f, (dn.y + 0.32f) * strength * 0.40f, dn.z * strength * 0.55f));
            if (!rd.bodies[1].IsInvalid()) {
                body_if.AddImpulse(rd.bodies[1], JPH::Vec3(dn.x * strength * 0.70f, 12.0f, dn.z * strength * 0.70f));
            }
        } else {
            if (!rd.bodies[1].IsInvalid()) {
                body_if.AddImpulse(rd.bodies[1], JPH::Vec3(dn.x * strength, (dn.y + 0.22f) * strength * 0.45f, dn.z * strength));
            }
            if (!rd.bodies[0].IsInvalid()) {
                body_if.AddImpulse(rd.bodies[0], JPH::Vec3(dn.x * strength * 0.55f, 8.0f, dn.z * strength * 0.55f));
            }
        }
        rd.active = true;
        rd.age = 0.0f;
    }

    void add_static_box(Float3 center, Float3 half_extents) {
        auto& bi = physics.GetBodyInterface();
        JPH::BodyCreationSettings bcs(
            new JPH::BoxShape(JPH::Vec3(half_extents.x, half_extents.y, half_extents.z)),
            JPH::RVec3(center.x, center.y, center.z),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            Layers::NON_MOVING);
        bcs.mFriction = 0.65f;
        bcs.mRestitution = 0.25f;
        const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
        range_static_bodies.push_back(id);
    }

    void build_dust2_collision_mesh(
        const std::vector<RangeVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        std::uint32_t lod0_index_count) {
        if (vertices.empty() || lod0_index_count < 3U || lod0_index_count > indices.size()) {
            return;
        }
        auto& bi = physics.GetBodyInterface();
        if (!dust2_mesh_body.IsInvalid()) {
            if (dust2_body_in_world) {
                bi.RemoveBody(dust2_mesh_body);
                dust2_body_in_world = false;
            }
            bi.DestroyBody(dust2_mesh_body);
            dust2_mesh_body = JPH::BodyID();
        }
        JPH::VertexList jverts;
        jverts.reserve(vertices.size());
        for (const auto& v : vertices) {
            jverts.push_back(JPH::Float3(v.px, v.py, v.pz));
        }
        JPH::IndexedTriangleList jtris;
        jtris.reserve(lod0_index_count / 3U);
        for (std::uint32_t i = 0; i + 2U < lod0_index_count; i += 3U) {
            const std::uint32_t i0 = indices[i + 0U];
            const std::uint32_t i1 = indices[i + 1U];
            const std::uint32_t i2 = indices[i + 2U];
            if (i0 < vertices.size() && i1 < vertices.size() && i2 < vertices.size() && i0 != i1 && i1 != i2 && i2 != i0) {
                jtris.push_back(JPH::IndexedTriangle(i0, i1, i2, 0));
            }
        }
        if (jtris.empty()) {
            return;
        }
        JPH::MeshShapeSettings mesh_settings(jverts, jtris);
        auto shape_res = mesh_settings.Create();
        if (shape_res.HasError()) {
            return;
        }
        JPH::BodyCreationSettings bcs(
            shape_res.Get(),
            JPH::RVec3::sZero(),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            Layers::NON_MOVING);
        bcs.mFriction = 0.70f;
        bcs.mRestitution = 0.10f;
        JPH::Body* b = bi.CreateBody(bcs);
        if (b != nullptr) {
            dust2_mesh_body = b->GetID();
            if (dust2_world_active) {
                bi.AddBody(dust2_mesh_body, JPH::EActivation::DontActivate);
                dust2_body_in_world = true;
                physics.OptimizeBroadPhase();
            }
        }
    }

    void set_active_collision_world(bool is_dust2) {
        if (dust2_world_active == is_dust2) {
            return;
        }
        destroy_all_ragdolls();
        dust2_world_active = is_dust2;
        auto& bi = physics.GetBodyInterface();
        if (is_dust2) {
            if (range_bodies_in_world) {
                for (const auto& id : range_static_bodies) {
                    if (!id.IsInvalid()) bi.RemoveBody(id);
                }
                for (const auto& t : targets) {
                    if (!t.anchor_id.IsInvalid()) bi.RemoveBody(t.anchor_id);
                    if (!t.plate_id.IsInvalid()) bi.RemoveBody(t.plate_id);
                }
                range_bodies_in_world = false;
            }
            if (!dust2_mesh_body.IsInvalid() && !dust2_body_in_world) {
                bi.AddBody(dust2_mesh_body, JPH::EActivation::DontActivate);
                dust2_body_in_world = true;
            }
        } else {
            if (!dust2_mesh_body.IsInvalid() && dust2_body_in_world) {
                bi.RemoveBody(dust2_mesh_body);
                dust2_body_in_world = false;
            }
            if (!range_bodies_in_world) {
                for (const auto& id : range_static_bodies) {
                    if (!id.IsInvalid()) bi.AddBody(id, JPH::EActivation::DontActivate);
                }
                for (const auto& t : targets) {
                    if (!t.anchor_id.IsInvalid()) bi.AddBody(t.anchor_id, JPH::EActivation::DontActivate);
                    if (!t.plate_id.IsInvalid()) bi.AddBody(t.plate_id, JPH::EActivation::Activate);
                }
                range_bodies_in_world = true;
            }
        }
        physics.OptimizeBroadPhase();
    }

    EntityHandle spawn_render_entity(const char* name, const char* tag, const char* uri, RangeMeshKind mesh, bool visible, bool viewmodel) {
        const EntityHandle e = ecs.create_entity(name, tag, uri);
        TransformComponent tr{};
        tr.model = identity_mat4();
        tr.prev_model = tr.model;
        tr.matrix_driven = true;
        ecs.transforms().emplace(e, tr);
        RenderableComponent r{};
        r.mesh = mesh;
        r.visible = visible;
        r.viewmodel = viewmodel;
        ecs.renderables().emplace(e, r);
        return e;
    }

    void build_physics_and_ecs() {
        add_static_box({0.0f, -0.25f, 15.0f}, {7.5f, 0.25f, 19.0f});
        add_static_box({-6.75f, 2.4f, 15.0f}, {0.25f, 2.6f, 19.0f});
        add_static_box({6.75f, 2.4f, 15.0f}, {0.25f, 2.6f, 19.0f});
        add_static_box({0.0f, 5.05f, 15.0f}, {7.5f, 0.25f, 19.0f});
        add_static_box({0.0f, 2.4f, 32.25f}, {7.5f, 2.6f, 0.25f});
        add_static_box({0.0f, 2.4f, -2.25f}, {7.5f, 2.6f, 0.25f});
        add_static_box({0.0f, 0.48f, 3.6f}, {6.4f, 0.48f, 0.35f});

        add_static_box({-3.8f, 0.55f, 12.6f}, {0.65f, 0.55f, 0.65f});
        add_static_box({3.8f, 0.55f, 12.8f}, {0.65f, 0.55f, 0.65f});
        add_static_box({3.8f, 1.55f, 12.8f}, {0.50f, 0.45f, 0.50f});
        add_static_box({0.8f, 0.48f, 13.6f}, {0.55f, 0.48f, 0.55f});
        add_static_box({-4.2f, 0.60f, 19.2f}, {0.75f, 0.60f, 0.75f});
        add_static_box({4.0f, 0.60f, 19.4f}, {0.75f, 0.60f, 0.75f});

        player_entity = ecs.create_entity("player", "player", "assets/scripts/player_controller.lua");
        ecs.transforms().emplace(player_entity, TransformComponent{});
        ecs.healths().emplace(player_entity, HealthComponent{});
        ecs.players().emplace(player_entity, PlayerControllerComponent{});
        ecs.scripts().emplace(player_entity, ScriptComponent{"player_controller.lua", true, false});

        range_world_entity = spawn_render_entity("range_architecture", "environment", "procedural://tactical_range_bay", RangeMeshKind::static_world, true, false);

        director_entity = ecs.create_entity("range_director", "director", "assets/scripts/range_director.lua");
        ecs.scripts().emplace(director_entity, ScriptComponent{"range_director.lua", true, false});

        weapon_entity = ecs.create_entity("pistol_weapon", "weapon", "assets/models/service_pistol/service_pistol.gltf");
        ecs.transforms().emplace(weapon_entity, TransformComponent{});
        ecs.weapons().emplace(weapon_entity, WeaponComponent{});
        ecs.scripts().emplace(weapon_entity, ScriptComponent{"pistol_behavior.lua", true, false});

        const char* pistol_uri = "assets/models/service_pistol/service_pistol.gltf";
        pistol_frame_entity = spawn_render_entity("pistol_frame", "pistol_part", pistol_uri, RangeMeshKind::pistol_frame, true, true);
        pistol_slide_entity = spawn_render_entity("pistol_slide", "pistol_part", pistol_uri, RangeMeshKind::pistol_slide, true, true);
        pistol_barrel_entity = spawn_render_entity("pistol_barrel", "pistol_part", pistol_uri, RangeMeshKind::pistol_barrel, true, true);
        pistol_hammer_entity = spawn_render_entity("pistol_hammer", "pistol_part", pistol_uri, RangeMeshKind::pistol_hammer, true, true);
        pistol_trigger_entity = spawn_render_entity("pistol_trigger", "pistol_part", pistol_uri, RangeMeshKind::pistol_trigger, true, true);
        pistol_mag_entity = spawn_render_entity("pistol_magazine", "pistol_part", pistol_uri, RangeMeshKind::pistol_magazine, true, true);
        pistol_sights_entity = spawn_render_entity("pistol_sights", "pistol_part", "procedural://tritium_sights", RangeMeshKind::pistol_sights, true, true);
        weapon_smg_entity = spawn_render_entity("weapon_smg", "weapon_vm", "assets/models/weapons/mp5.glb", RangeMeshKind::weapon_smg, false, true);
        weapon_rifle_entity = spawn_render_entity("weapon_rifle", "weapon_vm", "assets/models/weapons/ak47.glb", RangeMeshKind::weapon_rifle, false, true);
        weapon_scout_entity = spawn_render_entity("weapon_scout", "weapon_vm", "assets/models/weapons/scout.glb", RangeMeshKind::weapon_scout, false, true);
        weapon_shotgun_entity = spawn_render_entity("weapon_shotgun", "weapon_vm", "assets/models/weapons/shotgun.glb", RangeMeshKind::weapon_shotgun, false, true);
        weapon_dual_right_entity = spawn_render_entity("weapon_dual_right", "weapon_vm", "assets/models/weapons/dual_pistol.glb", RangeMeshKind::weapon_dual_pistol, false, true);
        weapon_dual_left_entity = spawn_render_entity("weapon_dual_left", "weapon_vm", "assets/models/weapons/dual_pistol.glb", RangeMeshKind::weapon_dual_pistol, false, true);
        muzzle_flash_entity = spawn_render_entity("pistol_muzzle_flash", "vfx", "procedural://muzzle_flash", RangeMeshKind::muzzle_flash, false, true);
        tracer_entity = spawn_render_entity("bullet_tracer", "vfx", "procedural://bullet_tracer", RangeMeshKind::bullet_tracer, false, false);
        spark_entity = spawn_render_entity("impact_spark", "vfx", "procedural://impact_spark", RangeMeshKind::impact_spark, false, false);

        cs_match_entity = ecs.create_entity("cs_match_director", "director", "assets/scripts/cs_match.lua");
        ecs.scripts().emplace(cs_match_entity, ScriptComponent{"cs_match.lua", true, false});
        for (std::uint32_t bi = 0; bi < 8U; ++bi) {
            const bool is_ct = (bi < 4U);
            const std::string bname = (is_ct ? "bot_fbi_" : "bot_terrorist_") + std::to_string(bi % 4U);
            const char* buri = is_ct ? "assets/models/characters/fbi_ct.glb" : "assets/models/characters/terrorist.glb";
            const auto bmesh = static_cast<RangeMeshKind>(static_cast<std::uint32_t>(RangeMeshKind::bot_slot_0) + bi);
            const EntityHandle be = spawn_render_entity(bname.c_str(), "cs_bot", buri, bmesh, false, false);
            BotComponent bc{};
            bc.slot_index = static_cast<std::int32_t>(bi);
            bc.team = is_ct ? 0 : 1;
            bc.active = false;
            ecs.bots().emplace(be, bc);
            bot_entities[bi] = be;
            const std::string bwname = (is_ct ? "bot_fbi_gun_" : "bot_t_gun_") + std::to_string(bi % 4U);
            const RangeMeshKind gwkind = is_ct ? RangeMeshKind::weapon_smg : RangeMeshKind::weapon_rifle;
            const char* gwuri = is_ct ? "assets/models/weapons/mp5.glb" : "assets/models/weapons/ak47.glb";
            bot_weapon_entities[bi] = spawn_render_entity(bwname.c_str(), "bot_weapon", gwuri, gwkind, false, false);
            bot_hand_r_local[bi] = {0.14f, 1.24f, 0.24f};
        }

        auto& bi = physics.GetBodyInterface();
        for (std::uint32_t i = 0; i < k_range_target_count; ++i) {
            TargetPhysicsBinding& slot = targets[i];
            slot.base_pos = k_target_bases[i];

            JPH::BodyCreationSettings anchor_settings(
                new JPH::BoxShape(JPH::Vec3(0.08f, 0.03f, 0.08f)),
                JPH::RVec3(slot.base_pos.x, 0.03f, slot.base_pos.z),
                JPH::Quat::sIdentity(),
                JPH::EMotionType::Static,
                Layers::NON_MOVING);
            JPH::Body* anchor_body = bi.CreateBody(anchor_settings);
            bi.AddBody(anchor_body->GetID(), JPH::EActivation::DontActivate);
            slot.anchor_id = anchor_body->GetID();

            JPH::BodyCreationSettings plate_settings(
                new JPH::BoxShape(JPH::Vec3(0.28f, 0.64f, 0.032f)),
                JPH::RVec3(slot.base_pos.x, slot.base_pos.y + 0.72f, slot.base_pos.z),
                JPH::Quat::sIdentity(),
                JPH::EMotionType::Dynamic,
                Layers::MOVING);
            plate_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            plate_settings.mMassPropertiesOverride.mMass = 10.0f;
            plate_settings.mAngularDamping = 0.45f;
            plate_settings.mLinearDamping = 0.10f;
            plate_settings.mAllowSleeping = false;
            JPH::Body* plate_body = bi.CreateBody(plate_settings);
            bi.AddBody(plate_body->GetID(), JPH::EActivation::Activate);
            slot.plate_id = plate_body->GetID();

            JPH::HingeConstraintSettings hinge_settings;
            hinge_settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            hinge_settings.mPoint1 = JPH::RVec3(slot.base_pos.x, slot.base_pos.y, slot.base_pos.z);
            hinge_settings.mPoint2 = JPH::RVec3(slot.base_pos.x, slot.base_pos.y, slot.base_pos.z);
            hinge_settings.mHingeAxis1 = JPH::Vec3::sAxisX();
            hinge_settings.mHingeAxis2 = JPH::Vec3::sAxisX();
            hinge_settings.mNormalAxis1 = JPH::Vec3::sAxisY();
            hinge_settings.mNormalAxis2 = JPH::Vec3::sAxisY();
            hinge_settings.mLimitsMin = -0.04f;
            hinge_settings.mLimitsMax = 1.44f;
            hinge_settings.mMaxFrictionTorque = 0.25f;
            hinge_settings.mMotorSettings = JPH::MotorSettings(3.8f, 0.78f);
            hinge_settings.mMotorSettings.SetTorqueLimit(220.0f);

            slot.hinge = static_cast<JPH::HingeConstraint*>(hinge_settings.Create(*anchor_body, *plate_body));
            slot.hinge->SetMotorState(JPH::EMotorState::Position);
            slot.hinge->SetTargetAngle(0.0f);
            physics.AddConstraint(slot.hinge);

            const std::string tname = "target_" + std::to_string(i);
            const EntityHandle te = spawn_render_entity(tname.c_str(), "target", "procedural://ar500_hinge_target", RangeMeshKind::target_plate, true, false);
            slot.entity = te;
            ecs.physics_bodies().emplace(te, PhysicsBodyComponent{
                slot.plate_id.GetIndexAndSequenceNumber(),
                PhysicsBodyKind::hinge_target_plate,
                static_cast<std::int32_t>(i),
                true,
            });
            TargetComponent tc{};
            tc.slot_index = static_cast<std::int32_t>(i);
            tc.base_pos = slot.base_pos;
            ecs.targets().emplace(te, tc);
            ecs.scripts().emplace(te, ScriptComponent{"target_behavior.lua", true, false});
        }

        for (std::uint32_t i = 0; i < k_range_casing_count; ++i) {
            JPH::BodyCreationSettings casing_bcs(
                new JPH::CylinderShape(0.014f, 0.006f, 0.002f),
                JPH::RVec3(0.0f, -5.0f - static_cast<float>(i), 0.0f),
                JPH::Quat::sIdentity(),
                JPH::EMotionType::Dynamic,
                Layers::MOVING);
            casing_bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            casing_bcs.mMassPropertiesOverride.mMass = 0.015f;
            casing_bcs.mRestitution = 0.45f;
            casing_bcs.mFriction = 0.5f;
            casings[i].body_id = bi.CreateAndAddBody(casing_bcs, JPH::EActivation::DontActivate);

            const std::string cname = "casing_" + std::to_string(i);
            const EntityHandle ce = spawn_render_entity(cname.c_str(), "casing", "procedural://brass_9mm_casing", RangeMeshKind::brass_casing, false, false);
            casings[i].entity = ce;
            ecs.physics_bodies().emplace(ce, PhysicsBodyComponent{
                casings[i].body_id.GetIndexAndSequenceNumber(),
                PhysicsBodyKind::dynamic_casing,
                static_cast<std::int32_t>(i),
                false,
            });
            CasingComponent cc{};
            cc.slot_index = static_cast<std::int32_t>(i);
            ecs.casings().emplace(ce, cc);
            ecs.scripts().emplace(ce, ScriptComponent{"shell_casing.lua", true, false});
        }

        physics.OptimizeBroadPhase();
    }

    TargetPhysicsBinding* find_target_binding(EntityHandle entity) {
        const TargetComponent* tc = ecs.targets().try_get(entity);
        if (tc == nullptr || tc->slot_index < 0 || tc->slot_index >= static_cast<std::int32_t>(k_range_target_count)) {
            return nullptr;
        }
        return &targets[static_cast<std::size_t>(tc->slot_index)];
    }

    CasingPhysicsBinding* find_casing_binding(EntityHandle entity) {
        const CasingComponent* cc = ecs.casings().try_get(entity);
        if (cc == nullptr || cc->slot_index < 0 || cc->slot_index >= static_cast<std::int32_t>(k_range_casing_count)) {
            return nullptr;
        }
        return &casings[static_cast<std::size_t>(cc->slot_index)];
    }

    Mat4x4 compute_weapon_root_local(const WeaponComponent& w, bool left_dual_hand = false) const {
        const std::int32_t wid = std::clamp(w.weapon_id, 0, k_cs_weapon_count - 1);
        const float t = w.ads_alpha;
        const float smooth_t = t * t * (3.0f - 2.0f * t);
        const float sway_scale = 1.0f - 0.80f * smooth_t;
        const float sway_x = std::sin(range_time * 1.7f) * 0.0045f * sway_scale;
        const float sway_y = std::cos(range_time * 3.4f) * 0.0032f * sway_scale;

        float hip_x = hero_pistol_gltf_loaded ? 0.165f : 0.175f;
        float hip_y = hero_pistol_gltf_loaded ? -0.150f : -0.155f;
        float hip_z = hero_pistol_gltf_loaded ? 0.38f : 0.42f;
        float ads_z = hero_pistol_gltf_loaded ? 0.30f : 0.34f;
        float ads_x = 0.0f;
        float ads_y = -weapon_ads_sight_y[static_cast<std::size_t>(wid)];
        float kick = w.recoil_kick;

        if (wid == 1) {
            hip_x = 0.165f; hip_y = -0.150f; hip_z = 0.44f; ads_z = 0.30f;
        } else if (wid == 2) {
            hip_x = 0.185f; hip_y = -0.165f; hip_z = 0.42f; ads_z = 0.29f;
        } else if (wid == 3) {
            hip_x = 0.185f; hip_y = -0.165f; hip_z = 0.46f; ads_z = 0.26f;
        } else if (wid == 4) {
            hip_x = 0.175f; hip_y = -0.155f; hip_z = 0.41f; ads_z = 0.28f;
        } else if (wid == 5) {
            const float sign = left_dual_hand ? -1.0f : 1.0f;
            hip_x = sign * 0.185f;
            ads_x = sign * 0.095f;
            hip_y = -0.145f;
            ads_y = -0.095f;
            hip_z = 0.38f;
            ads_z = 0.32f;
            kick = left_dual_hand ? w.dual_left_kick : w.dual_right_kick;
        } else {
            ads_y = -pistol_ads_sight_y;
        }

        float px = hip_x * (1.0f - smooth_t) + ads_x * smooth_t + sway_x;
        float py = hip_y * (1.0f - smooth_t) + ads_y * smooth_t + sway_y;
        float pz = hip_z * (1.0f - smooth_t) + ads_z * smooth_t;

        pz -= kick * (0.046f - 0.014f * smooth_t);
        py += kick * 0.011f;

        float pitch = -kick * (0.24f - 0.09f * smooth_t);
        float roll = w.recoil_roll * (left_dual_hand ? -0.11f : 0.11f);
        float yaw = (left_dual_hand ? 0.032f : -0.025f) * (1.0f - smooth_t);

        if (w.reloading) {
            const float u = std::clamp(1.0f - w.reload_timer / std::max(w.reload_duration, 0.1f), 0.0f, 1.0f);
            const float dip = std::sin(u * 3.14159265f);
            py -= dip * 0.085f;
            px -= (left_dual_hand ? -dip : dip) * 0.028f;
            roll += (left_dual_hand ? -dip : dip) * 0.54f;
            pitch -= dip * 0.30f;
        }

        const Mat4x4 tr = make_translation_mat4(px, py, pz);
        const Mat4x4 rot = mul_mat4(mul_mat4(make_rot_y(yaw), make_rot_x(pitch)), make_rot_z(roll));
        return mul_mat4(tr, rot);
    }

    Float3 active_muzzle_local(const WeaponComponent& w) const {
        const std::int32_t wid = std::clamp(w.weapon_id, 0, k_cs_weapon_count - 1);
        return wid == 0 ? pistol_muzzle_offset : weapon_muzzle_offsets[static_cast<std::size_t>(wid)];
    }

    Float3 active_eject_local(const WeaponComponent& w) const {
        const std::int32_t wid = std::clamp(w.weapon_id, 0, k_cs_weapon_count - 1);
        return wid == 0 ? pistol_eject_offset : weapon_eject_offsets[static_cast<std::size_t>(wid)];
    }

    void register_engine_event_bridges() {
        events.subscribe("bot.killed", [this](const std::string&, const EventPayload& payload) {
            const auto ent = static_cast<EntityHandle>(payload.get_u64("entity", k_invalid_entity));
            for (std::size_t bi = 0; bi < bot_entities.size(); ++bi) {
                if (bot_entities[bi] == ent) {
                    const Float3 dir{
                        static_cast<float>(payload.get_number("hit_dir_x", 1.0)),
                        static_cast<float>(payload.get_number("hit_dir_y", 0.18)),
                        static_cast<float>(payload.get_number("hit_dir_z", 0.0))};
                    const auto dmg = static_cast<float>(payload.get_number("damage", 80.0));
                    const bool hs = payload.get_bool("headshot", false);
                    spawn_bot_ragdoll(bi, dir, dmg, hs);
                    break;
                }
            }
        }, 0U);

        events.subscribe("shell.eject_requested", [this](const std::string&, const EventPayload& payload) {
            const WeaponComponent* w = ecs.weapons().try_get(weapon_entity);
            if (w == nullptr) return;
            const bool is_left = (w->weapon_id == 5 && payload.get_int("dual_side", 0) == 1);
            const Mat4x4 cam_world = camera_basis_to_world_mat4(active_camera);
            const Mat4x4 gun_world = mul_mat4(cam_world, compute_weapon_root_local(*w, is_left));
            const Float3 eject_pos = transform_point(gun_world, active_eject_local(*w));
            const float side_s = is_left ? -1.0f : 1.0f;
            const Float3 eject_vel{
                active_camera.right.x * (2.4f * side_s) + active_camera.up.x * 1.8f - active_camera.forward.x * 0.4f,
                active_camera.right.y * (2.4f * side_s) + active_camera.up.y * 1.8f - active_camera.forward.y * 0.4f,
                active_camera.right.z * (2.4f * side_s) + active_camera.up.z * 1.8f - active_camera.forward.z * 0.4f};
            EventPayload spawn_ev{};
            spawn_ev.set_int("shot", payload.get_int("shot", 1));
            spawn_ev.set_number("px", eject_pos.x);
            spawn_ev.set_number("py", eject_pos.y);
            spawn_ev.set_number("pz", eject_pos.z);
            spawn_ev.set_number("vx", eject_vel.x);
            spawn_ev.set_number("vy", eject_vel.y);
            spawn_ev.set_number("vz", eject_vel.z);
            events.emit("shell.eject_spawn", spawn_ev);
        }, 0U);

        events.subscribe("ballistics.raycast_requested", [this](const std::string&, const EventPayload& payload) {
            const WeaponComponent* w = ecs.weapons().try_get(weapon_entity);
            if (w == nullptr) return;
            const bool is_left = (w->weapon_id == 5 && w->dual_hand_toggle == 1);
            const Mat4x4 cam_world = camera_basis_to_world_mat4(active_camera);
            const Mat4x4 gun_world = mul_mat4(cam_world, compute_weapon_root_local(*w, is_left));
            muzzle_world_pos = transform_point(gun_world, active_muzzle_local(*w));

            const auto sx = static_cast<float>(payload.get_number("spread_x", 0.0));
            const auto sy = static_cast<float>(payload.get_number("spread_y", 0.0));
            const auto base_dmg = static_cast<float>(payload.get_number("damage", w->base_damage));
            Float3 ray_dir{
                active_camera.forward.x + active_camera.right.x * sx + active_camera.up.x * sy,
                active_camera.forward.y + active_camera.right.y * sx + active_camera.up.y * sy,
                active_camera.forward.z + active_camera.right.z * sx + active_camera.up.z * sy};
            const float rlen = std::sqrt(ray_dir.x * ray_dir.x + ray_dir.y * ray_dir.y + ray_dir.z * ray_dir.z);
            ray_dir = {ray_dir.x / std::max(rlen, 1e-5f), ray_dir.y / std::max(rlen, 1e-5f), ray_dir.z / std::max(rlen, 1e-5f)};

            constexpr float max_ray_dist = 95.0f;
            const JPH::RRayCast ray{
                JPH::RVec3(active_camera.position.x, active_camera.position.y, active_camera.position.z),
                JPH::Vec3(ray_dir.x * max_ray_dist, ray_dir.y * max_ray_dist, ray_dir.z * max_ray_dist)};
            JPH::RayCastSettings rcs{};
            rcs.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
            JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
            PlayerCollisionBodyFilter bfilter(casings, &bot_ragdolls);
            physics.GetNarrowPhaseQuery().CastRay(ray, rcs, collector, {}, {}, bfilter);
            const bool did_hit = collector.HadHit();
            float best_dist = did_hit ? (collector.mHit.mFraction * max_ray_dist) : max_ray_dist;

            EntityHandle hit_bot = k_invalid_entity;
            bool bot_headshot = false;
            const Float3 ro = active_camera.position;
            auto ray_sphere = [&](Float3 c, float rad, float& out_t) -> bool {
                const Float3 oc{ro.x - c.x, ro.y - c.y, ro.z - c.z};
                const float b_term = oc.x * ray_dir.x + oc.y * ray_dir.y + oc.z * ray_dir.z;
                const float c_term = (oc.x * oc.x + oc.y * oc.y + oc.z * oc.z) - rad * rad;
                const float disc = b_term * b_term - c_term;
                if (disc < 0.0f) return false;
                const float t = -b_term - std::sqrt(disc);
                if (t > 0.15f && t < out_t) {
                    out_t = t;
                    return true;
                }
                return false;
            };
            for (EntityHandle be : bot_entities) {
                const BotComponent* bc = ecs.bots().try_get(be);
                if (bc == nullptr || !bc->active || !bc->alive) continue;
                float ht = best_dist;
                if (ray_sphere(bc->head_world_pos, 0.22f, ht)) {
                    best_dist = ht;
                    hit_bot = be;
                    bot_headshot = true;
                } else {
                    float bt = best_dist;
                    const Float3 mid_body{bc->chest_world_pos.x, bc->pos.y + 0.92f, bc->chest_world_pos.z};
                    const Float3 low_body{bc->pos.x, bc->pos.y + 0.50f, bc->pos.z};
                    if (ray_sphere(mid_body, 0.36f, bt) || ray_sphere(low_body, 0.32f, bt)) {
                        best_dist = bt;
                        hit_bot = be;
                        bot_headshot = false;
                    }
                }
            }

            tracer_start = muzzle_world_pos;
            tracer_timer = 0.085f;
            if (hit_bot != k_invalid_entity) {
                tracer_end = {
                    ro.x + ray_dir.x * best_dist,
                    ro.y + ray_dir.y * best_dist,
                    ro.z + ray_dir.z * best_dist};
                impact_pos = tracer_end;
                impact_timer = 0.18f;
                impact_was_target = true;
                hit_marker_timer = 0.24f;
                hit_marker_headshot = bot_headshot;
                ++hits;
                if (bot_headshot) ++headshots;
                score += bot_headshot ? 250 : 100;
                EventPayload bev{};
                bev.set_u64("entity", hit_bot);
                bev.set_number("damage", base_dmg);
                bev.set_bool("headshot", bot_headshot);
                bev.set_number("hit_x", tracer_end.x);
                bev.set_number("hit_y", tracer_end.y);
                bev.set_number("hit_z", tracer_end.z);
                bev.set_number("hit_dir_x", ray_dir.x);
                bev.set_number("hit_dir_y", ray_dir.y);
                bev.set_number("hit_dir_z", ray_dir.z);
                events.emit("bot.damaged", bev);
            } else if (did_hit) {
                const JPH::RVec3 hp_jolt = ray.GetPointOnRay(collector.mHit.mFraction);
                tracer_end = {static_cast<float>(hp_jolt.GetX()), static_cast<float>(hp_jolt.GetY()), static_cast<float>(hp_jolt.GetZ())};
                impact_pos = tracer_end;
                impact_timer = 0.18f;
                impact_was_target = false;

                const EntityHandle struck = ecs.find_by_jolt_body(collector.mHit.mBodyID.GetIndexAndSequenceNumber());
                if (struck != k_invalid_entity && ecs.targets().has(struck)) {
                    impact_was_target = true;
                    EventPayload hit_ev{};
                    hit_ev.set_u64("entity", struck);
                    hit_ev.set_number("hit_x", tracer_end.x);
                    hit_ev.set_number("hit_y", tracer_end.y);
                    hit_ev.set_number("hit_z", tracer_end.z);
                    hit_ev.set_number("dir_x", ray_dir.x);
                    hit_ev.set_number("dir_y", ray_dir.y);
                    hit_ev.set_number("dir_z", ray_dir.z);
                    events.emit("target.hit", hit_ev);
                } else {
                    EventPayload imp_ev{};
                    imp_ev.set_number("hit_x", tracer_end.x);
                    imp_ev.set_number("hit_y", tracer_end.y);
                    imp_ev.set_number("hit_z", tracer_end.z);
                    events.emit("world.impact", imp_ev);
                }
            } else {
                tracer_end = {
                    active_camera.position.x + ray_dir.x * max_ray_dist,
                    active_camera.position.y + ray_dir.y * max_ray_dist,
                    active_camera.position.z + ray_dir.z * max_ray_dist};
            }
        }, 0U);
    }

    void initialize_lua() {
        LuaHostCallbacks cb{};
        cb.apply_target_impulse = [this](EntityHandle entity, Float3 impulse, Float3 point) {
            TargetPhysicsBinding* binding = find_target_binding(entity);
            if (binding == nullptr) return;
            auto& bi = physics.GetBodyInterface();
            bi.ActivateBody(binding->plate_id);
            bi.AddImpulse(
                binding->plate_id,
                JPH::Vec3(impulse.x, impulse.y, impulse.z),
                JPH::RVec3(point.x, point.y, point.z));
        };
        cb.set_target_motor_upright = [this](EntityHandle entity, bool upright) {
            TargetPhysicsBinding* binding = find_target_binding(entity);
            if (binding == nullptr) return;
            if (upright) {
                binding->hinge->SetMotorState(JPH::EMotorState::Position);
                binding->hinge->SetTargetAngle(0.0f);
                physics.GetBodyInterface().ActivateBody(binding->plate_id);
            } else {
                binding->hinge->SetMotorState(JPH::EMotorState::Off);
            }
        };
        cb.reset_target_pose = [this](EntityHandle entity) {
            TargetPhysicsBinding* binding = find_target_binding(entity);
            if (binding == nullptr) return;
            auto& bi = physics.GetBodyInterface();
            bi.SetPositionAndRotation(
                binding->plate_id,
                JPH::RVec3(binding->base_pos.x, binding->base_pos.y + 0.72f, binding->base_pos.z),
                JPH::Quat::sIdentity(),
                JPH::EActivation::Activate);
            bi.SetLinearAndAngularVelocity(binding->plate_id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            binding->hinge->SetMotorState(JPH::EMotorState::Position);
            binding->hinge->SetTargetAngle(0.0f);
            if (auto* tr = ecs.transforms().try_get(entity)) {
                tr->has_prev = false;
            }
        };
        cb.launch_casing = [this](EntityHandle entity, Float3 pos, Float3 vel, Float3 ang) {
            CasingPhysicsBinding* binding = find_casing_binding(entity);
            if (binding == nullptr) return;
            auto& bi = physics.GetBodyInterface();
            bi.SetPositionAndRotation(
                binding->body_id,
                JPH::RVec3(pos.x, pos.y, pos.z),
                JPH::Quat::sIdentity(),
                JPH::EActivation::Activate);
            bi.SetLinearAndAngularVelocity(
                binding->body_id,
                JPH::Vec3(vel.x, vel.y, vel.z),
                JPH::Vec3(ang.x, ang.y, ang.z));
            if (auto* tr = ecs.transforms().try_get(entity)) {
                tr->has_prev = false;
            }
        };
        cb.deactivate_casing = [this](EntityHandle entity) {
            CasingPhysicsBinding* binding = find_casing_binding(entity);
            if (binding == nullptr) return;
            auto& bi = physics.GetBodyInterface();
            bi.DeactivateBody(binding->body_id);
            const auto* cc = ecs.casings().try_get(entity);
            const float y_park = -5.0f - static_cast<float>(cc != nullptr ? cc->slot_index : 0);
            bi.SetPosition(binding->body_id, JPH::RVec3(0.0f, y_park, 0.0f), JPH::EActivation::DontActivate);
        };
        cb.trigger_hit_marker = [this](bool headshot) {
            hit_marker_timer = 0.22f;
            hit_marker_headshot = headshot;
        };
        cb.add_score = [this](std::int32_t pts, bool headshot) {
            ++hits;
            if (headshot) {
                ++headshots;
            }
            score += pts;
        };
        cb.reset_score = [this]() {
            hits = 0;
            headshots = 0;
            score = 0;
        };
        cb.raycast_world = [this](Float3 origin, Float3 dir, float max_dist) -> LuaRaycastResult {
            LuaRaycastResult out{};
            const float dlen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (dlen < 1e-5f || max_dist <= 1e-4f) {
                return out;
            }
            const Float3 nd{dir.x / dlen, dir.y / dlen, dir.z / dlen};
            const JPH::RRayCast ray{
                JPH::RVec3(origin.x, origin.y, origin.z),
                JPH::Vec3(nd.x * max_dist, nd.y * max_dist, nd.z * max_dist)};
            JPH::RayCastSettings rcs{};
            rcs.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
            JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
            PlayerCollisionBodyFilter bfilter(casings, &bot_ragdolls);
            physics.GetNarrowPhaseQuery().CastRay(ray, rcs, collector, {}, {}, bfilter);
            if (collector.HadHit()) {
                out.hit = true;
                out.dist = collector.mHit.mFraction * max_dist;
                const JPH::RVec3 hp = ray.GetPointOnRay(collector.mHit.mFraction);
                out.point = {static_cast<float>(hp.GetX()), static_cast<float>(hp.GetY()), static_cast<float>(hp.GetZ())};
                JPH::BodyLockRead lock(physics.GetBodyLockInterface(), collector.mHit.mBodyID);
                if (lock.Succeeded()) {
                    JPH::Vec3 wn = lock.GetBody().GetWorldSpaceSurfaceNormal(collector.mHit.mSubShapeID2, hp);
                    if (wn.Dot(JPH::Vec3(nd.x, nd.y, nd.z)) > 0.0f) {
                        wn = -wn;
                    }
                    out.normal = {wn.GetX(), wn.GetY(), wn.GetZ()};
                }
            }
            return out;
        };
        cb.sweep_capsule = [this](Float3 center, Float3 disp, float radius, float half_height) -> LuaCapsuleSweepResult {
            LuaCapsuleSweepResult out{};
            const float dlen = std::sqrt(disp.x * disp.x + disp.y * disp.y + disp.z * disp.z);
            if (dlen < 1e-6f) {
                return out;
            }
            const JPH::Vec3 disp_dir(disp.x / dlen, disp.y / dlen, disp.z / dlen);
            JPH::CapsuleShape cap(std::max(0.05f, half_height), std::max(0.05f, radius));
            const JPH::RShapeCast shape_cast = JPH::RShapeCast::sFromWorldTransform(
                &cap,
                JPH::Vec3::sReplicate(1.0f),
                JPH::RMat44::sTranslation(JPH::RVec3(center.x, center.y, center.z)),
                JPH::Vec3(disp.x, disp.y, disp.z));
            JPH::ShapeCastSettings scs{};
            scs.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
            scs.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideOnlyWithActive;
            scs.mActiveEdgeMovementDirection = disp_dir;
            scs.mReturnDeepestPoint = true;

            class OpposingShapeCastCollector final : public JPH::CastShapeCollector {
            public:
                explicit OpposingShapeCastCollector(JPH::Vec3 dir) : sweep_dir_(dir) {}
                void AddHit(const JPH::ShapeCastResult& hit) override {
                    JPH::Vec3 n = -hit.mPenetrationAxis;
                    const float nlen = n.Length();
                    if (nlen <= 1e-5f) return;
                    n = n / nlen;
                    if (n.Dot(sweep_dir_) >= -0.005f) return;
                    if (hit.mFraction < best_fraction_) {
                        best_fraction_ = hit.mFraction;
                        best_point_ = hit.mContactPointOn2;
                        best_normal_ = n;
                        has_hit_ = true;
                        UpdateEarlyOutFraction(hit.mFraction);
                    }
                }
                bool has_hit_ = false;
                float best_fraction_ = 2.0f;
                JPH::Vec3 best_point_ = JPH::Vec3::sZero();
                JPH::Vec3 best_normal_ = JPH::Vec3::sZero();
            private:
                JPH::Vec3 sweep_dir_;
            };

            OpposingShapeCastCollector collector(disp_dir);
            PlayerCollisionBodyFilter bfilter(casings, &bot_ragdolls);
            physics.GetNarrowPhaseQuery().CastShape(shape_cast, scs, JPH::RVec3::sZero(), collector, {}, {}, bfilter);
            if (collector.has_hit_) {
                out.hit = true;
                out.fraction = std::clamp(collector.best_fraction_, 0.0f, 1.0f);
                out.point = {
                    static_cast<float>(collector.best_point_.GetX()),
                    static_cast<float>(collector.best_point_.GetY()),
                    static_cast<float>(collector.best_point_.GetZ())};
                out.normal = {
                    collector.best_normal_.GetX(),
                    collector.best_normal_.GetY(),
                    collector.best_normal_.GetZ()};
            }
            return out;
        };
        cb.resolve_capsule_overlap = [this](Float3 center, float radius, float half_height) -> LuaOverlapResult {
            LuaOverlapResult out{};
            JPH::CapsuleShape cap(std::max(0.05f, half_height), std::max(0.05f, radius));
            JPH::CollideShapeSettings css{};
            css.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
            css.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideOnlyWithActive;
            css.mMaxSeparationDistance = 0.0f;
            PlayerCollisionBodyFilter bfilter(casings, &bot_ragdolls);
            JPH::RVec3 cur_center(center.x, center.y, center.z);
            bool any_hit = false;
            for (int iter = 0; iter < 4; ++iter) {
                JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
                physics.GetNarrowPhaseQuery().CollideShapeWithInternalEdgeRemoval(
                    &cap,
                    JPH::Vec3::sReplicate(1.0f),
                    JPH::RMat44::sTranslation(cur_center),
                    css,
                    JPH::RVec3::sZero(),
                    collector,
                    {},
                    {},
                    bfilter);
                float max_depth = 0.0008f;
                JPH::Vec3 best_dir = JPH::Vec3::sZero();
                for (const auto& h : collector.mHits) {
                    if (h.mPenetrationDepth <= max_depth) continue;
                    JPH::Vec3 dir = -h.mPenetrationAxis;
                    const float len = dir.Length();
                    if (len > 1e-5f) {
                        max_depth = h.mPenetrationDepth;
                        best_dir = dir / len;
                    }
                }
                if (best_dir.LengthSq() < 0.5f) {
                    break;
                }
                const float step = std::min(max_depth + 0.001f, 0.25f);
                cur_center += JPH::RVec3(best_dir * step);
                any_hit = true;
            }
            if (any_hit) {
                JPH::Vec3 total_push = JPH::Vec3(cur_center - JPH::RVec3(center.x, center.y, center.z));
                const float plen = total_push.Length();
                if (plen > 1e-4f) {
                    if (plen > 0.35f) {
                        total_push = total_push * (0.35f / plen);
                    }
                    out.penetrated = true;
                    out.push = {total_push.GetX(), total_push.GetY(), total_push.GetZ()};
                }
            }
            return out;
        };
        cb.get_waypoints = [this]() -> std::vector<Float3> {
            return dust2_waypoints;
        };
        cb.is_dust2_active = [this]() -> bool {
            return dust2_world_active;
        };
        cb.record_bot_kill = [this](std::int32_t) {
            ++bot_kills;
        };
        lua.initialize(&ecs, &events, std::move(cb));
    }

    void reset_state() {
        destroy_all_ragdolls();
        events.emit("range.reset");
        tracer_timer = 0.0f;
        impact_timer = 0.0f;
        hit_marker_timer = 0.0f;
        phys_accum = 0.0;
        has_prev_pistol_local = false;
        for (auto& tr : ecs.transforms().components()) {
            tr.has_prev = false;
        }
    }

    void step_sim(double delta_seconds, bool paused, const CameraBasis& camera) {
        active_camera = camera;
        if (paused || delta_seconds <= 0.0) {
            return;
        }
        const float dt = static_cast<float>(std::min(delta_seconds, 0.05));
        range_time += dt;

        if (auto* w = ecs.weapons().try_get(weapon_entity)) {
            const PlayerControllerComponent* pl = ecs.players().try_get(player_entity);
            if (pl != nullptr && pl->manual_active) {
                w->move_speed += (std::min(pl->horizontal_speed, 8.0f) - w->move_speed) * (1.0f - std::exp(-dt * 16.0f));
            } else if (has_prev_camera_pos && dt > 1e-5f) {
                const float dx = camera.position.x - prev_camera_pos.x;
                const float dz = camera.position.z - prev_camera_pos.z;
                const float inst_speed = std::sqrt(dx * dx + dz * dz) / dt;
                w->move_speed += (std::min(inst_speed, 8.0f) - w->move_speed) * (1.0f - std::exp(-dt * 16.0f));
            } else {
                w->move_speed = 0.0f;
            }
        }
        prev_camera_pos = camera.position;
        has_prev_camera_pos = true;

        if (tracer_timer > 0.0f) tracer_timer = std::max(0.0f, tracer_timer - dt);
        if (impact_timer > 0.0f) impact_timer = std::max(0.0f, impact_timer - dt);
        if (hit_marker_timer > 0.0f) hit_marker_timer = std::max(0.0f, hit_marker_timer - dt);

        events.flush();
        lua.step(static_cast<double>(dt));
        events.flush();

        for (std::size_t bi = 0; bi < bot_entities.size(); ++bi) {
            BotComponent* bc = ecs.bots().try_get(bot_entities[bi]);
            if (bc == nullptr) continue;
            if (bc->anim_blend < 1.0f) {
                bc->anim_blend = std::min(1.0f, bc->anim_blend + dt * 7.5f);
            }
            if (bot_ragdolls[bi].active) {
                if (!bc->active || bc->alive) {
                    destroy_bot_ragdoll(bi);
                } else {
                    bot_ragdolls[bi].age += dt;
                }
            }
        }

        const auto phys_t0 = std::chrono::steady_clock::now();
        constexpr double fixed_step = 1.0 / 240.0;
        phys_accum = std::min(phys_accum + static_cast<double>(dt), fixed_step * 6.0);
        int substeps = 0;
        while (phys_accum >= fixed_step && substeps < 6) {
            physics.Update(static_cast<float>(fixed_step), 1, &temp_allocator, &job_system);
            phys_accum -= fixed_step;
            ++substeps;
        }
        const auto phys_t1 = std::chrono::steady_clock::now();
        last_physics_step_us = std::chrono::duration_cast<std::chrono::microseconds>(phys_t1 - phys_t0).count();
    }

    void sync_ecs_render_transforms(const CameraBasis& camera, const CameraBasis& previous_camera) {
        auto update_entity_xform = [&](EntityHandle e, const Mat4x4& model, bool explicit_prev, const Mat4x4& prev_override) {
            TransformComponent* tr = ecs.transforms().try_get(e);
            if (tr == nullptr) return;
            if (explicit_prev) {
                tr->prev_model = tr->has_prev ? prev_override : model;
            } else {
                tr->prev_model = tr->has_prev ? tr->model : model;
            }
            tr->model = model;
            tr->has_prev = true;
        };

        const Mat4x4 id = identity_mat4();
        update_entity_xform(range_world_entity, id, true, id);

        const auto sync_t0 = std::chrono::steady_clock::now();
        const auto& bi = physics.GetBodyInterface();
        const std::size_t dynamic_total = static_cast<std::size_t>(k_range_target_count) + static_cast<std::size_t>(k_range_casing_count);
        EngineJobSystem::global().parallel_for(dynamic_total, 4U, [&](std::size_t begin, std::size_t end) {
            for (std::size_t idx = begin; idx < end; ++idx) {
                if (idx < k_range_target_count) {
                    const TargetPhysicsBinding& binding = targets[idx];
                    const TargetComponent* tc = ecs.targets().try_get(binding.entity);
                    const Mat4x4 plate_world = mul_mat4(
                        from_jolt_mat44(bi.GetWorldTransform(binding.plate_id)),
                        make_translation_mat4(0.0f, -0.72f, 0.0f));
                    update_entity_xform(binding.entity, plate_world, false, plate_world);
                    if (auto* r = ecs.renderables().try_get(binding.entity)) {
                        const float flash = tc != nullptr ? tc->hit_flash : 0.0f;
                        r->tint = {1.0f, 1.0f - 0.35f * flash, 1.0f - 0.45f * flash};
                        r->emissive = flash * 1.4f;
                        r->visible = true;
                    }
                } else {
                    const std::size_t ci = idx - k_range_target_count;
                    const CasingPhysicsBinding& binding = casings[ci];
                    const CasingComponent* cc = ecs.casings().try_get(binding.entity);
                    const bool active = cc != nullptr && cc->active;
                    if (auto* r = ecs.renderables().try_get(binding.entity)) {
                        r->visible = active;
                        r->tint = {1.0f, 0.92f, 0.72f};
                        r->emissive = 0.0f;
                    }
                    if (active) {
                        const Mat4x4 cw = from_jolt_mat44(bi.GetWorldTransform(binding.body_id));
                        update_entity_xform(binding.entity, cw, false, cw);
                    }
                }
            }
        });
        const auto sync_t1 = std::chrono::steady_clock::now();
        last_ecs_sync_us = std::chrono::duration_cast<std::chrono::microseconds>(sync_t1 - sync_t0).count();

        if (auto* tr_r = ecs.renderables().try_get(tracer_entity)) {
            tr_r->visible = tracer_timer > 0.0f;
            if (tr_r->visible) {
                const float a = std::clamp(tracer_timer / 0.085f, 0.0f, 1.0f);
                const Mat4x4 tm = make_segment_mat4(tracer_start, tracer_end, 0.0045f * (0.4f + 0.6f * a));
                update_entity_xform(tracer_entity, tm, true, tm);
                tr_r->tint = {1.0f, 0.85f, 0.38f};
                tr_r->emissive = 4.5f * a;
            }
        }

        if (auto* sp_r = ecs.renderables().try_get(spark_entity)) {
            sp_r->visible = impact_timer > 0.0f;
            if (sp_r->visible) {
                const float a = std::clamp(impact_timer / 0.18f, 0.0f, 1.0f);
                const float sc = 0.045f + 0.055f * (1.0f - a);
                const Mat4x4 sm = mul_mat4(make_translation_mat4(impact_pos.x, impact_pos.y, impact_pos.z), make_scale_mat4(sc, sc, sc));
                update_entity_xform(spark_entity, sm, true, sm);
                sp_r->tint = impact_was_target ? Float3{1.0f, 0.55f, 0.18f} : Float3{0.92f, 0.90f, 0.85f};
                sp_r->emissive = 5.5f * a;
            }
        }

        const WeaponComponent* w = ecs.weapons().try_get(weapon_entity);
        if (w == nullptr) return;
        const std::int32_t wid = std::clamp(w->weapon_id, 0, k_cs_weapon_count - 1);
        const bool same_weapon = has_prev_pistol_local && (prev_rendered_weapon_id == wid);
        const Mat4x4 cam_world = camera_basis_to_world_mat4(camera);
        const Mat4x4 prev_cam_world = camera_basis_to_world_mat4(previous_camera);
        const Mat4x4 gun_local = compute_weapon_root_local(*w, false);
        const Mat4x4 p_gun_l = same_weapon ? prev_gun_local : gun_local;
        const Mat4x4 gun_world = mul_mat4(cam_world, gun_local);
        const Mat4x4 prev_gun_world = mul_mat4(prev_cam_world, p_gun_l);

        const Mat4x4 dual_left_local = compute_weapon_root_local(*w, true);
        const Mat4x4 p_dual_l = same_weapon ? prev_dual_left_local : dual_left_local;
        const Mat4x4 dual_left_world = mul_mat4(cam_world, dual_left_local);
        const Mat4x4 prev_dual_left_world = mul_mat4(prev_cam_world, p_dual_l);

        const float slide_z_back = -w->slide_blowback * 0.042f;
        const Mat4x4 slide_local = make_translation_mat4(
            pistol_slide_offset.x,
            pistol_slide_offset.y,
            pistol_slide_offset.z + slide_z_back);
        const Mat4x4 slide_world = mul_mat4(gun_world, slide_local);
        const Mat4x4 prev_slide_world = mul_mat4(prev_gun_world, same_weapon ? prev_slide_local : slide_local);

        const Mat4x4 hammer_local = mul_mat4(
            make_translation_mat4(pistol_hammer_offset.x, pistol_hammer_offset.y, pistol_hammer_offset.z),
            make_rot_x(-w->hammer_cock * 0.82f));
        const Mat4x4 hammer_world = mul_mat4(gun_world, hammer_local);
        const Mat4x4 prev_hammer_world = mul_mat4(prev_gun_world, same_weapon ? prev_hammer_local : hammer_local);

        const Mat4x4 trigger_local = mul_mat4(
            make_translation_mat4(pistol_trigger_offset.x, pistol_trigger_offset.y, pistol_trigger_offset.z),
            make_rot_x(w->trigger_pull * 0.34f));
        const Mat4x4 trigger_world = mul_mat4(gun_world, trigger_local);
        const Mat4x4 prev_trigger_world = mul_mat4(prev_gun_world, same_weapon ? prev_trigger_local : trigger_local);

        float mag_drop = 0.0f;
        if (w->reloading) {
            const float u = std::clamp(1.0f - w->reload_timer / std::max(w->reload_duration, 0.1f), 0.0f, 1.0f);
            if (u > 0.14f && u < 0.84f) {
                const float mu = (u - 0.14f) / 0.70f;
                mag_drop = std::sin(mu * 3.14159265f) * 0.145f;
            }
        }
        const Mat4x4 mag_local = make_translation_mat4(
            pistol_magazine_offset.x,
            pistol_magazine_offset.y - mag_drop,
            pistol_magazine_offset.z - mag_drop * 0.32f);
        const Mat4x4 mag_world = mul_mat4(gun_world, mag_local);
        const Mat4x4 prev_mag_world = mul_mat4(prev_gun_world, same_weapon ? prev_mag_local : mag_local);

        prev_gun_local = gun_local;
        prev_dual_left_local = dual_left_local;
        prev_slide_local = slide_local;
        prev_hammer_local = hammer_local;
        prev_trigger_local = trigger_local;
        prev_mag_local = mag_local;
        has_prev_pistol_local = true;
        prev_rendered_weapon_id = wid;

        const bool show_usp = (wid == 0);
        auto set_vis = [&](EntityHandle e, bool v) {
            if (auto* r = ecs.renderables().try_get(e)) r->visible = v;
        };
        set_vis(pistol_frame_entity, show_usp);
        set_vis(pistol_slide_entity, show_usp);
        set_vis(pistol_barrel_entity, show_usp && !hero_pistol_gltf_loaded);
        set_vis(pistol_hammer_entity, show_usp);
        set_vis(pistol_trigger_entity, show_usp);
        set_vis(pistol_mag_entity, show_usp);
        set_vis(pistol_sights_entity, show_usp);

        const bool hide_scoped = (wid == 3 && w->scope_alpha > 0.72f);
        set_vis(weapon_smg_entity, wid == 1);
        set_vis(weapon_rifle_entity, wid == 2);
        set_vis(weapon_scout_entity, wid == 3 && !hide_scoped);
        set_vis(weapon_shotgun_entity, wid == 4);
        set_vis(weapon_dual_right_entity, wid == 5);
        set_vis(weapon_dual_left_entity, wid == 5);

        update_entity_xform(pistol_frame_entity, gun_world, true, prev_gun_world);
        update_entity_xform(pistol_slide_entity, slide_world, true, prev_slide_world);
        update_entity_xform(pistol_barrel_entity, gun_world, true, prev_gun_world);
        update_entity_xform(pistol_hammer_entity, hammer_world, true, prev_hammer_world);
        update_entity_xform(pistol_trigger_entity, trigger_world, true, prev_trigger_world);
        update_entity_xform(pistol_mag_entity, mag_world, true, prev_mag_world);
        update_entity_xform(pistol_sights_entity, slide_world, true, prev_slide_world);
        update_entity_xform(weapon_smg_entity, gun_world, true, prev_gun_world);
        update_entity_xform(weapon_rifle_entity, gun_world, true, prev_gun_world);
        update_entity_xform(weapon_scout_entity, gun_world, true, prev_gun_world);
        update_entity_xform(weapon_shotgun_entity, gun_world, true, prev_gun_world);
        update_entity_xform(weapon_dual_right_entity, gun_world, true, prev_gun_world);
        update_entity_xform(weapon_dual_left_entity, dual_left_world, true, prev_dual_left_world);

        if (auto* sr = ecs.renderables().try_get(pistol_sights_entity)) {
            sr->tint = {0.22f, 1.0f, 0.42f};
            sr->emissive = 2.6f;
        }

        if (auto* mf = ecs.renderables().try_get(muzzle_flash_entity)) {
            mf->visible = (w->muzzle_flash_timer > 0.0f) && !hide_scoped;
            if (mf->visible) {
                const float fa = std::clamp(w->muzzle_flash_timer / 0.048f, 0.0f, 1.0f);
                const float s = (wid == 4 ? 0.95f : 0.60f) + 0.55f * fa;
                const Float3 moz = active_muzzle_local(*w);
                const bool use_left = (wid == 5 && w->dual_hand_toggle == 0);
                const Mat4x4& flash_parent = use_left ? dual_left_world : gun_world;
                const Mat4x4 fl = mul_mat4(
                    make_translation_mat4(moz.x, moz.y, moz.z),
                    mul_mat4(make_rot_z(w->muzzle_flash_spin), make_scale_mat4(s, s, s)));
                const Mat4x4 fw = mul_mat4(flash_parent, fl);
                update_entity_xform(muzzle_flash_entity, fw, true, fw);
                mf->tint = {1.0f, 0.56f, 0.14f};
                mf->emissive = 4.2f * fa;
            }
        }

        for (std::uint32_t bot_idx = 0; bot_idx < 8U; ++bot_idx) {
            const EntityHandle be = bot_entities[bot_idx];
            const EntityHandle bwe = bot_weapon_entities[bot_idx];
            BotComponent* bc = ecs.bots().try_get(be);
            if (bc == nullptr) continue;
            const bool is_vis = bc->active && (bot_vb_counts[bot_idx] > 0U);
            if (auto* br = ecs.renderables().try_get(be)) {
                br->visible = is_vis;
                const float hf = bc->hit_flash;
                br->tint = {1.0f, 1.0f - 0.55f * hf, 1.0f - 0.60f * hf};
                br->emissive = hf * 1.1f;
            }
            if (auto* bwr = ecs.renderables().try_get(bwe)) {
                bwr->visible = is_vis;
                bwr->tint = {1.0f, 1.0f, 1.0f};
                bwr->emissive = 0.0f;
            }
            if (bc->active) {
                if (bot_ragdolls[bot_idx].active) {
                    const Mat4x4 idm = identity_mat4();
                    update_entity_xform(be, idm, false, idm);
                    const JPH::BodyID rhand_body = bot_ragdolls[bot_idx].bodies[6];
                    if (!rhand_body.IsInvalid()) {
                        const Mat4x4 rw = from_jolt_mat44(bi.GetWorldTransform(rhand_body));
                        const Mat4x4 gw = mul_mat4(rw, make_translation_mat4(0.0f, -0.05f, 0.16f));
                        update_entity_xform(bwe, gw, false, gw);
                    }
                } else {
                    const Mat4x4 bw = mul_mat4(
                        make_translation_mat4(bc->pos.x, bc->pos.y, bc->pos.z),
                        make_rot_y(bc->yaw));
                    update_entity_xform(be, bw, false, bw);
                    const Float3 hl = bot_hand_r_local[bot_idx];
                    const Mat4x4 gun_l = mul_mat4(
                        make_translation_mat4(hl.x - 0.02f, hl.y + 0.03f, hl.z + 0.10f),
                        mul_mat4(make_rot_x(-bc->aim_pitch), make_scale_mat4(0.95f, 0.95f, 0.95f)));
                    const Mat4x4 gw = mul_mat4(bw, gun_l);
                    update_entity_xform(bwe, gw, false, gw);
                }
            }
        }
    }
};

TacticalRangeSimulation::TacticalRangeSimulation() : impl_(std::make_unique<Impl>()) {}
TacticalRangeSimulation::~TacticalRangeSimulation() = default;

void TacticalRangeSimulation::set_hero_pistol_geometry_meta(
    bool loaded_gltf,
    Float3 slide_offset,
    Float3 hammer_offset,
    Float3 trigger_offset,
    Float3 magazine_offset,
    Float3 muzzle_offset,
    Float3 eject_offset,
    float ads_sight_y) {
    impl_->hero_pistol_gltf_loaded = loaded_gltf;
    impl_->pistol_slide_offset = slide_offset;
    impl_->pistol_hammer_offset = hammer_offset;
    impl_->pistol_trigger_offset = trigger_offset;
    impl_->pistol_magazine_offset = magazine_offset;
    impl_->pistol_muzzle_offset = muzzle_offset;
    impl_->pistol_eject_offset = eject_offset;
    impl_->pistol_ads_sight_y = ads_sight_y;
}

void TacticalRangeSimulation::set_arsenal_geometry_meta(
    const std::array<Float3, k_cs_weapon_count>& muzzle_offsets,
    const std::array<Float3, k_cs_weapon_count>& eject_offsets,
    const std::array<float, k_cs_weapon_count>& ads_sight_y,
    const std::array<bool, k_cs_weapon_count>& loaded_flags) {
    impl_->weapon_muzzle_offsets = muzzle_offsets;
    impl_->weapon_eject_offsets = eject_offsets;
    impl_->weapon_ads_sight_y = ads_sight_y;
    impl_->weapon_loaded = loaded_flags;
}

void TacticalRangeSimulation::set_bot_character_templates(
    const SkinnedCharacterTemplate* ct_template,
    const SkinnedCharacterTemplate* t_template,
    const std::array<std::uint32_t, 8>& bot_vb_offsets,
    const std::array<std::uint32_t, 8>& bot_vb_counts,
    std::uint32_t total_bot_vertices) {
    impl_->ct_bot_template = ct_template;
    impl_->t_bot_template = t_template;
    impl_->bot_vb_offsets = bot_vb_offsets;
    impl_->bot_vb_counts = bot_vb_counts;
    impl_->total_bot_vertices = total_bot_vertices;
}

void TacticalRangeSimulation::set_dust2_waypoints(const std::vector<Float3>& waypoints) {
    impl_->dust2_waypoints = waypoints;
}

void TacticalRangeSimulation::reset() {
    impl_->reset_state();
}

void TacticalRangeSimulation::set_dust2_collision_mesh(
    const std::vector<RangeVertex>& vertices,
    const std::vector<std::uint32_t>& indices,
    std::uint32_t lod0_index_count) {
    impl_->build_dust2_collision_mesh(vertices, indices, lod0_index_count);
}

void TacticalRangeSimulation::set_active_world(bool is_dust2) {
    impl_->set_active_collision_world(is_dust2);
}

void TacticalRangeSimulation::sync_player_spawn(Float3 eye_pos, float yaw, float pitch, bool is_dust2) {
    impl_->set_active_collision_world(is_dust2);
    if (auto* p = impl_->ecs.players().try_get(impl_->player_entity)) {
        const float stand_eye = p->stand_eye_height;
        const float spawn_feet_y = is_dust2
            ? (eye_pos.y - stand_eye + 0.04f)
            : std::max(0.02f, eye_pos.y - stand_eye + 0.03f);
        p->feet_pos = {eye_pos.x, spawn_feet_y, eye_pos.z};
        p->velocity = {0.0f, 0.0f, 0.0f};
        p->yaw = yaw;
        p->pitch = pitch;
        p->eye_height = stand_eye;
        p->capsule_height = 1.72f;
        p->grounded = true;
        p->touching_wall = false;
        p->crouched = false;
        p->is_dust2 = is_dust2;
        p->coyote_timer = 0.10f;
        p->jump_buffer_timer = 0.0f;
        p->step_phase = 0.0f;
        p->bob_x = 0.0f;
        p->bob_y = 0.0f;
        p->landing_dip = 0.0f;
        p->horizontal_speed = 0.0f;
        p->collision_count = 0;
    }
    EventPayload ev{};
    if (const auto* p = impl_->ecs.players().try_get(impl_->player_entity)) {
        ev.set_number("feet_x", p->feet_pos.x);
        ev.set_number("feet_y", p->feet_pos.y);
        ev.set_number("feet_z", p->feet_pos.z);
    }
    impl_->events.emit("player.teleport", ev);
}

void TacticalRangeSimulation::set_player_input(
    const ManualCameraInput& input,
    float yaw,
    float pitch,
    bool manual_active,
    bool is_dust2) {
    impl_->set_active_collision_world(is_dust2);
    if (auto* p = impl_->ecs.players().try_get(impl_->player_entity)) {
        p->move_forward = input.forward_axis;
        p->move_right = input.right_axis;
        if (input.jump_pressed) {
            p->jump_pressed = true;
        }
        p->crouch_held = input.crouch_held;
        p->sprint_held = input.sprint_held;
        p->yaw = yaw;
        p->pitch = pitch;
        p->manual_active = manual_active;
        p->is_dust2 = is_dust2;
    }
}

Float3 TacticalRangeSimulation::player_camera_position() const {
    const PlayerControllerComponent* p = impl_->ecs.players().try_get(impl_->player_entity);
    if (p == nullptr) {
        return {0.0f, 1.42f, 1.2f};
    }
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    const float ads_atten = 1.0f - 0.78f * (w != nullptr ? w->ads_alpha : 0.0f);
    const float cos_y = std::cos(p->yaw);
    const float sin_y = std::sin(p->yaw);
    const float bx = p->bob_x * ads_atten;
    const float by = p->bob_y * ads_atten - p->landing_dip;
    return {
        p->feet_pos.x + cos_y * bx,
        p->feet_pos.y + p->eye_height + by,
        p->feet_pos.z - sin_y * bx,
    };
}

void TacticalRangeSimulation::set_ads_requested(bool active) {
    EventPayload ev{};
    ev.set_bool("active", active);
    impl_->events.emit("weapon.ads_changed", ev);
}

void TacticalRangeSimulation::set_firing_held(bool held) {
    if (auto* w = impl_->ecs.weapons().try_get(impl_->weapon_entity)) {
        w->firing_held = held;
    }
}

void TacticalRangeSimulation::request_fire() {
    impl_->events.enqueue("weapon.fire_requested");
}

void TacticalRangeSimulation::request_reload() {
    impl_->events.enqueue("weapon.reload_requested");
}

void TacticalRangeSimulation::cycle_weapon(std::int32_t delta) {
    EventPayload ev{};
    ev.set_int("delta", delta);
    impl_->events.enqueue("weapon.cycle_requested", ev);
}

void TacticalRangeSimulation::select_weapon(std::int32_t weapon_id) {
    EventPayload ev{};
    ev.set_int("weapon_id", std::clamp(weapon_id, 0, k_cs_weapon_count - 1));
    impl_->events.enqueue("weapon.switch_requested", ev);
}

std::int32_t TacticalRangeSimulation::active_weapon_id() const {
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    return w != nullptr ? w->weapon_id : 0;
}

bool TacticalRangeSimulation::reload_lua_scripts() {
    return impl_->lua.reload_all();
}

bool TacticalRangeSimulation::emit_named_event(const std::string& event_name) {
    if (event_name.empty()) {
        return false;
    }
    impl_->events.enqueue(event_name);
    return true;
}

void TacticalRangeSimulation::step(double delta_seconds, bool paused, const CameraBasis& camera) {
    if (!paused && delta_seconds > 0.0) {
        const float dt = static_cast<float>(std::min(delta_seconds, 0.05));
        for (EntityHandle be : impl_->bot_entities) {
            if (auto* bc = impl_->ecs.bots().try_get(be)) {
                if (bc->anim_blend < 1.0f) {
                    bc->anim_blend = std::min(1.0f, bc->anim_blend + dt * 7.5f);
                }
            }
        }
    }
    impl_->step_sim(delta_seconds, paused, camera);
}

float TacticalRangeSimulation::current_vertical_fov_radians() const {
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    const float t = w != nullptr ? w->ads_alpha : 0.0f;
    const float ads_fov = w != nullptr ? std::clamp(w->ads_fov_rad, 0.32f, 1.05f) : 0.84f;
    const float smooth_t = t * t * (3.0f - 2.0f * t);
    return 1.15f * (1.0f - smooth_t) + ads_fov * smooth_t;
}

float TacticalRangeSimulation::camera_recoil_pitch_radians() const {
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    return w != nullptr ? w->recoil_pitch : 0.0f;
}

float TacticalRangeSimulation::camera_recoil_yaw_radians() const {
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    return w != nullptr ? w->recoil_yaw : 0.0f;
}

TacticalHudSnapshot TacticalRangeSimulation::hud_snapshot(const CameraBasis& camera) const {
    TacticalHudSnapshot h{};
    const HealthComponent* hc = impl_->ecs.healths().try_get(impl_->player_entity);
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    h.hp = hc != nullptr ? hc->hp : 100;
    h.armor = hc != nullptr ? hc->armor : 100;
    h.ammo_in_mag = w != nullptr ? w->ammo_in_mag : 12;
    h.ammo_reserve = w != nullptr ? w->ammo_reserve : 48;
    h.shots_fired = w != nullptr ? w->shots_fired : 0;
    h.hits = impl_->hits;
    h.headshots = impl_->headshots;
    h.score = impl_->score;
    const float ads_alpha = w != nullptr ? w->ads_alpha : 0.0f;
    const float spread_rad = w != nullptr ? w->current_spread_rad : 0.011f;
    h.active_weapon_id = w != nullptr ? w->weapon_id : 0;
    h.ads_alpha = ads_alpha;
    h.scope_alpha = w != nullptr ? w->scope_alpha : 0.0f;
    h.crosshair_spread = std::clamp(spread_rad * 28.0f, 0.06f, 1.85f);
    h.hit_marker_alpha = std::clamp(impl_->hit_marker_timer / 0.22f, 0.0f, 1.0f);
    h.hit_marker_headshot = impl_->hit_marker_headshot ? 1.0f : 0.0f;
    h.reload_progress = (w != nullptr && w->reloading)
        ? std::clamp(1.0f - w->reload_timer / std::max(w->reload_duration, 0.1f), 0.0f, 1.0f)
        : 0.0f;
    h.muzzle_flash_alpha = w != nullptr ? std::clamp(w->muzzle_flash_timer / 0.048f, 0.0f, 1.0f) : 0.0f;
    h.muzzle_world_pos = impl_->muzzle_world_pos;
    h.player_pos = camera.position;
    h.player_yaw_rad = std::atan2(camera.forward.x, camera.forward.z);
    std::int32_t up_count = 0;
    for (std::uint32_t i = 0; i < k_range_target_count; ++i) {
        const TargetComponent* tc = impl_->ecs.targets().try_get(impl_->targets[i].entity);
        const bool is_up = tc != nullptr ? !tc->knocked_down : true;
        if (is_up) ++up_count;
        const Float3 bp = tc != nullptr ? tc->base_pos : k_target_bases[i];
        h.target_radar_xy_state[i * 4U + 0U] = bp.x;
        h.target_radar_xy_state[i * 4U + 1U] = bp.z;
        h.target_radar_xy_state[i * 4U + 2U] = is_up ? 1.0f : 0.0f;
        h.target_radar_xy_state[i * 4U + 3U] = tc != nullptr ? tc->hit_flash : 0.0f;
    }
    h.targets_up = up_count;
    return h;
}

EcsLuaTelemetry TacticalRangeSimulation::ecs_lua_telemetry() const {
    const EcsWorldStats es = impl_->ecs.stats();
    const LuaEngineStats ls = impl_->lua.stats();
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    const PlayerControllerComponent* p = impl_->ecs.players().try_get(impl_->player_entity);
    std::int32_t alive_ct = 0;
    std::int32_t alive_t = 0;
    std::int32_t active_ragdolls = 0;
    for (std::size_t bi = 0; bi < impl_->bot_entities.size(); ++bi) {
        if (const auto* bc = impl_->ecs.bots().try_get(impl_->bot_entities[bi])) {
            if (bc->active && bc->alive) {
                if (bc->team == 0) ++alive_ct;
                else ++alive_t;
            }
        }
        if (impl_->bot_ragdolls[bi].active) {
            ++active_ragdolls;
        }
    }
    return EcsLuaTelemetry{
        es.entity_count,
        es.renderable_count,
        es.physics_body_count,
        es.script_count,
        ls.loaded_scripts,
        ls.reload_count,
        ls.error_count,
        ls.last_step_us,
        impl_->events.dispatched_count(),
        impl_->events.subscription_count(),
        impl_->hero_pistol_gltf_loaded,
        EngineJobSystem::global().worker_count(),
        impl_->jolt_worker_count,
        EngineJobSystem::global().jobs_executed(),
        impl_->last_physics_step_us,
        impl_->last_ecs_sync_us,
        w != nullptr ? w->current_spread_rad * 1000.0f : 0.0f,
        w != nullptr ? w->last_shot_offset_rad * 1000.0f : 0.0f,
        p != nullptr ? p->grounded : true,
        p != nullptr ? p->touching_wall : false,
        p != nullptr ? p->crouched : false,
        p != nullptr ? p->horizontal_speed : 0.0f,
        p != nullptr ? p->collision_count : 0,
        p != nullptr ? p->feet_pos : Float3{0.0f, 0.0f, 1.2f},
        w != nullptr ? w->weapon_id : 0,
        alive_ct,
        alive_t,
        impl_->bot_kills,
        active_ragdolls,
    };
}

void TacticalRangeSimulation::build_render_frame(
    const CameraBasis& camera,
    const CameraBasis& previous_camera,
    TacticalRangeFrame& out_frame) {
    impl_->sync_ecs_render_transforms(camera, previous_camera);

    if (impl_->total_bot_vertices > 0U && (impl_->ct_bot_template != nullptr || impl_->t_bot_template != nullptr)) {
        out_frame.skinned_bot_vertices.resize(impl_->total_bot_vertices);
        EngineJobSystem::global().parallel_for(8U, 1U, [&](std::size_t begin, std::size_t end) {
            for (std::size_t bi = begin; bi < end; ++bi) {
                const std::uint32_t vcnt = impl_->bot_vb_counts[bi];
                if (vcnt == 0U) continue;
                BotComponent* bc = impl_->ecs.bots().try_get(impl_->bot_entities[bi]);
                if (bc == nullptr || !bc->active) continue;
                const SkinnedCharacterTemplate* tpl = (bi < 4U) ? impl_->ct_bot_template : impl_->t_bot_template;
                if (tpl == nullptr || !tpl->loaded) continue;
                if (impl_->bot_ragdolls[bi].active) {
                    const auto& body_if = impl_->physics.GetBodyInterfaceNoLock();
                    std::array<RagdollLimbPose, k_ragdoll_limb_count> world_limbs{};
                    for (std::uint32_t li = 0; li < k_ragdoll_limb_count; ++li) {
                        const JPH::BodyID bid = impl_->bot_ragdolls[bi].bodies[li];
                        if (!bid.IsInvalid()) {
                            const JPH::RVec3 p = body_if.GetCenterOfMassPosition(bid);
                            const JPH::Quat q = body_if.GetRotation(bid);
                            world_limbs[li] = {
                                {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ())},
                                q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
                        }
                    }
                    Float3 hw{}, cw{}, handw{};
                    evaluate_ragdoll_skinned_character(
                        *tpl,
                        world_limbs,
                        out_frame.skinned_bot_vertices.data() + impl_->bot_vb_offsets[bi],
                        hw,
                        cw,
                        handw);
                    bc->head_world_pos = hw;
                    bc->chest_world_pos = cw;
                    bc->muzzle_world_pos = handw;
                } else {
                    SkinnedPoseParams pose{};
                    pose.clip_index = tpl->find_clip(bc->anim_clip);
                    pose.clip_time = bc->anim_time;
                    pose.loop_clip = bc->anim_loop;
                    pose.prev_clip_index = tpl->find_clip(bc->prev_anim_clip);
                    pose.prev_clip_time = bc->prev_anim_time;
                    pose.blend_alpha = bc->anim_blend;
                    pose.aim_pitch_rad = bc->aim_pitch;
                    Float3 head_l{}, chest_l{}, muz_l{}, hand_l{};
                    evaluate_skinned_character(
                        *tpl,
                        pose,
                        out_frame.skinned_bot_vertices.data() + impl_->bot_vb_offsets[bi],
                        head_l,
                        chest_l,
                        muz_l,
                        &impl_->bot_last_limb_poses[bi],
                        &hand_l);
                    impl_->bot_has_limb_poses[bi] = true;
                    impl_->bot_hand_r_local[bi] = hand_l;
                    const Mat4x4 bw = mul_mat4(
                        make_translation_mat4(bc->pos.x, bc->pos.y, bc->pos.z),
                        make_rot_y(bc->yaw));
                    bc->head_world_pos = transform_point(bw, head_l);
                    bc->chest_world_pos = transform_point(bw, chest_l);
                    bc->muzzle_world_pos = transform_point(bw, muz_l);
                }
            }
        });
    } else {
        out_frame.skinned_bot_vertices.clear();
    }

    out_frame.draw_count = 0;

    const auto& ents = impl_->ecs.renderables().entities();
    const auto& rends = impl_->ecs.renderables().components();
    for (std::size_t i = 0; i < rends.size(); ++i) {
        const RenderableComponent& r = rends[i];
        if (!r.visible) continue;
        const TransformComponent* tr = impl_->ecs.transforms().try_get(ents[i]);
        if (tr == nullptr) continue;
        if (out_frame.draw_count >= out_frame.instances.size()) break;
        const std::uint32_t idx = out_frame.draw_count++;
        std::memcpy(out_frame.instances[idx].model, tr->model.m, sizeof(tr->model.m));
        std::memcpy(out_frame.instances[idx].prev_model, tr->prev_model.m, sizeof(tr->prev_model.m));
        out_frame.instances[idx].tint_emissive[0] = r.tint.x;
        out_frame.instances[idx].tint_emissive[1] = r.tint.y;
        out_frame.instances[idx].tint_emissive[2] = r.tint.z;
        out_frame.instances[idx].tint_emissive[3] = r.emissive;
        const std::uint32_t uid = ((entity_generation(ents[i]) & 0xFFFFU) << 16U) | (entity_index(ents[i]) & 0xFFFFU);
        out_frame.instances[idx].entity_meta[0] = uid;
        out_frame.instances[idx].entity_meta[1] = static_cast<std::uint32_t>(r.mesh);
        out_frame.instances[idx].entity_meta[2] = r.viewmodel ? 1U : 0U;
        out_frame.instances[idx].entity_meta[3] = 0U;
        out_frame.draws[idx] = RangeDrawCall{r.mesh, idx, r.viewmodel};
    }

    out_frame.hud = hud_snapshot(camera);
}

}
