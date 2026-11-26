#include "tactical_range.h"
#include "ecs.h"
#include "event_bus.h"
#include "lua_engine.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

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
    JPH::JobSystemSingleThreaded job_system{JPH::cMaxPhysicsJobs};
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
    EntityHandle muzzle_flash_entity = k_invalid_entity;
    EntityHandle tracer_entity = k_invalid_entity;
    EntityHandle spark_entity = k_invalid_entity;

    std::array<TargetPhysicsBinding, k_range_target_count> targets{};
    std::array<CasingPhysicsBinding, k_range_casing_count> casings{};

    bool hero_pistol_gltf_loaded = false;
    Float3 pistol_slide_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_hammer_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_trigger_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_magazine_offset{0.0f, 0.0f, 0.0f};
    Float3 pistol_muzzle_offset{0.0f, 0.042f, 0.145f};
    Float3 pistol_eject_offset{0.028f, 0.050f, 0.025f};
    float pistol_ads_sight_y = 0.068f;

    double phys_accum = 0.0;
    float range_time = 0.0f;

    std::int32_t hits = 0;
    std::int32_t headshots = 0;
    std::int32_t score = 0;

    CameraBasis active_camera{};
    Float3 muzzle_world_pos{0.0f, 1.4f, 2.0f};
    float tracer_timer = 0.0f;
    Float3 tracer_start{};
    Float3 tracer_end{};
    float impact_timer = 0.0f;
    Float3 impact_pos{};
    bool impact_was_target = false;
    float hit_marker_timer = 0.0f;
    bool hit_marker_headshot = false;

    Impl() {
        ensure_jolt_initialized();
        physics.Init(512, 0, 512, 512, bp_interface, obj_vs_bp_filter, obj_pair_filter);
        physics.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        build_physics_and_ecs();
        register_engine_event_bridges();
        initialize_lua();
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
        bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
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

        player_entity = ecs.create_entity("player", "player", "");
        ecs.transforms().emplace(player_entity, TransformComponent{});
        ecs.healths().emplace(player_entity, HealthComponent{});

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
        muzzle_flash_entity = spawn_render_entity("pistol_muzzle_flash", "vfx", "procedural://muzzle_flash", RangeMeshKind::muzzle_flash, false, true);
        tracer_entity = spawn_render_entity("bullet_tracer", "vfx", "procedural://bullet_tracer", RangeMeshKind::bullet_tracer, false, false);
        spark_entity = spawn_render_entity("impact_spark", "vfx", "procedural://impact_spark", RangeMeshKind::impact_spark, false, false);

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

    Mat4x4 compute_pistol_root_local(const WeaponComponent& w) const {
        const float t = w.ads_alpha;
        const float smooth_t = t * t * (3.0f - 2.0f * t);
        const float sway_scale = 1.0f - 0.80f * smooth_t;
        const float sway_x = std::sin(range_time * 1.7f) * 0.0045f * sway_scale;
        const float sway_y = std::cos(range_time * 3.4f) * 0.0032f * sway_scale;

        const float ads_y = -pistol_ads_sight_y;
        const float hip_x = hero_pistol_gltf_loaded ? 0.165f : 0.175f;
        const float hip_y = hero_pistol_gltf_loaded ? -0.150f : -0.155f;
        const float hip_z = hero_pistol_gltf_loaded ? 0.38f : 0.42f;
        const float ads_z = hero_pistol_gltf_loaded ? 0.30f : 0.34f;

        float px = hip_x * (1.0f - smooth_t) + sway_x;
        float py = hip_y * (1.0f - smooth_t) + ads_y * smooth_t + sway_y;
        float pz = hip_z * (1.0f - smooth_t) + ads_z * smooth_t;

        pz -= w.recoil_kick * (0.042f - 0.015f * smooth_t);
        py += w.recoil_kick * 0.010f;

        float pitch = -w.recoil_kick * (0.24f - 0.10f * smooth_t);
        float roll = w.recoil_roll * 0.12f;
        float yaw = -0.025f * (1.0f - smooth_t);

        if (w.reloading) {
            const float u = std::clamp(1.0f - w.reload_timer / std::max(w.reload_duration, 0.1f), 0.0f, 1.0f);
            const float dip = std::sin(u * 3.14159265f);
            py -= dip * 0.075f;
            px -= dip * 0.025f;
            roll += dip * 0.52f;
            pitch -= dip * 0.28f;
        }

        const Mat4x4 tr = make_translation_mat4(px, py, pz);
        const Mat4x4 rot = mul_mat4(mul_mat4(make_rot_y(yaw), make_rot_x(pitch)), make_rot_z(roll));
        return mul_mat4(tr, rot);
    }

    void register_engine_event_bridges() {
        events.subscribe("shell.eject_requested", [this](const std::string&, const EventPayload& payload) {
            const WeaponComponent* w = ecs.weapons().try_get(weapon_entity);
            if (w == nullptr) return;
            const Mat4x4 cam_world = camera_basis_to_world_mat4(active_camera);
            const Mat4x4 gun_world = mul_mat4(cam_world, compute_pistol_root_local(*w));
            const Float3 eject_pos = transform_point(gun_world, pistol_eject_offset);
            const Float3 eject_vel{
                active_camera.right.x * 2.4f + active_camera.up.x * 1.8f - active_camera.forward.x * 0.4f,
                active_camera.right.y * 2.4f + active_camera.up.y * 1.8f - active_camera.forward.y * 0.4f,
                active_camera.right.z * 2.4f + active_camera.up.z * 1.8f - active_camera.forward.z * 0.4f};
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
            const Mat4x4 cam_world = camera_basis_to_world_mat4(active_camera);
            const Mat4x4 gun_world = mul_mat4(cam_world, compute_pistol_root_local(*w));
            muzzle_world_pos = transform_point(gun_world, pistol_muzzle_offset);

            const auto shot = static_cast<float>(payload.get_int("shot", 1));
            const auto spread_angle = static_cast<float>(payload.get_number("spread", 0.0));
            const float sx = std::sin(shot * 12.9898f) * spread_angle;
            const float sy = std::cos(shot * 78.233f) * spread_angle;
            Float3 ray_dir{
                active_camera.forward.x + active_camera.right.x * sx + active_camera.up.x * sy,
                active_camera.forward.y + active_camera.right.y * sx + active_camera.up.y * sy,
                active_camera.forward.z + active_camera.right.z * sx + active_camera.up.z * sy};
            const float rlen = std::sqrt(ray_dir.x * ray_dir.x + ray_dir.y * ray_dir.y + ray_dir.z * ray_dir.z);
            ray_dir = {ray_dir.x / std::max(rlen, 1e-5f), ray_dir.y / std::max(rlen, 1e-5f), ray_dir.z / std::max(rlen, 1e-5f)};

            constexpr float max_ray_dist = 45.0f;
            const JPH::RRayCast ray{
                JPH::RVec3(active_camera.position.x, active_camera.position.y, active_camera.position.z),
                JPH::Vec3(ray_dir.x * max_ray_dist, ray_dir.y * max_ray_dist, ray_dir.z * max_ray_dist)};
            JPH::RayCastResult hit_result;
            const bool did_hit = physics.GetNarrowPhaseQuery().CastRay(ray, hit_result);

            tracer_start = muzzle_world_pos;
            tracer_timer = 0.085f;
            if (did_hit) {
                const JPH::RVec3 hp_jolt = ray.GetPointOnRay(hit_result.mFraction);
                tracer_end = {static_cast<float>(hp_jolt.GetX()), static_cast<float>(hp_jolt.GetY()), static_cast<float>(hp_jolt.GetZ())};
                impact_pos = tracer_end;
                impact_timer = 0.18f;
                impact_was_target = false;

                const EntityHandle struck = ecs.find_by_jolt_body(hit_result.mBodyID.GetIndexAndSequenceNumber());
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
        lua.initialize(&ecs, &events, std::move(cb));
    }

    void reset_state() {
        events.emit("range.reset");
        tracer_timer = 0.0f;
        impact_timer = 0.0f;
        hit_marker_timer = 0.0f;
        phys_accum = 0.0;
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

        if (tracer_timer > 0.0f) tracer_timer = std::max(0.0f, tracer_timer - dt);
        if (impact_timer > 0.0f) impact_timer = std::max(0.0f, impact_timer - dt);
        if (hit_marker_timer > 0.0f) hit_marker_timer = std::max(0.0f, hit_marker_timer - dt);

        events.flush();
        lua.step(static_cast<double>(dt));
        events.flush();

        constexpr double fixed_step = 1.0 / 240.0;
        phys_accum = std::min(phys_accum + static_cast<double>(dt), fixed_step * 6.0);
        int substeps = 0;
        while (phys_accum >= fixed_step && substeps < 6) {
            physics.Update(static_cast<float>(fixed_step), 1, &temp_allocator, &job_system);
            phys_accum -= fixed_step;
            ++substeps;
        }
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

        const auto& bi = physics.GetBodyInterface();
        for (std::uint32_t i = 0; i < k_range_target_count; ++i) {
            const TargetPhysicsBinding& binding = targets[i];
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
        }

        for (std::uint32_t i = 0; i < k_range_casing_count; ++i) {
            const CasingPhysicsBinding& binding = casings[i];
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
        const Mat4x4 cam_world = camera_basis_to_world_mat4(camera);
        const Mat4x4 prev_cam_world = camera_basis_to_world_mat4(previous_camera);
        const Mat4x4 gun_local = compute_pistol_root_local(*w);
        const Mat4x4 gun_world = mul_mat4(cam_world, gun_local);
        const Mat4x4 prev_gun_world = mul_mat4(prev_cam_world, gun_local);

        const float slide_z_back = -w->slide_blowback * 0.042f;
        const Mat4x4 slide_local = make_translation_mat4(
            pistol_slide_offset.x,
            pistol_slide_offset.y,
            pistol_slide_offset.z + slide_z_back);
        const Mat4x4 slide_world = mul_mat4(gun_world, slide_local);
        const Mat4x4 prev_slide_world = mul_mat4(prev_gun_world, slide_local);

        const Mat4x4 hammer_local = mul_mat4(
            make_translation_mat4(pistol_hammer_offset.x, pistol_hammer_offset.y, pistol_hammer_offset.z),
            make_rot_x(-w->hammer_cock * 0.82f));
        const Mat4x4 hammer_world = mul_mat4(gun_world, hammer_local);
        const Mat4x4 prev_hammer_world = mul_mat4(prev_gun_world, hammer_local);

        const Mat4x4 trigger_local = mul_mat4(
            make_translation_mat4(pistol_trigger_offset.x, pistol_trigger_offset.y, pistol_trigger_offset.z),
            make_rot_x(w->trigger_pull * 0.34f));
        const Mat4x4 trigger_world = mul_mat4(gun_world, trigger_local);
        const Mat4x4 prev_trigger_world = mul_mat4(prev_gun_world, trigger_local);

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
        const Mat4x4 prev_mag_world = mul_mat4(prev_gun_world, mag_local);

        update_entity_xform(pistol_frame_entity, gun_world, true, prev_gun_world);
        update_entity_xform(pistol_slide_entity, slide_world, true, prev_slide_world);
        update_entity_xform(pistol_barrel_entity, gun_world, true, prev_gun_world);
        update_entity_xform(pistol_hammer_entity, hammer_world, true, prev_hammer_world);
        update_entity_xform(pistol_trigger_entity, trigger_world, true, prev_trigger_world);
        update_entity_xform(pistol_mag_entity, mag_world, true, prev_mag_world);
        update_entity_xform(pistol_sights_entity, slide_world, true, prev_slide_world);

        if (auto* br = ecs.renderables().try_get(pistol_barrel_entity)) {
            br->visible = !hero_pistol_gltf_loaded;
        }
        if (auto* sr = ecs.renderables().try_get(pistol_sights_entity)) {
            sr->tint = {0.22f, 1.0f, 0.42f};
            sr->emissive = 2.6f;
        }

        if (auto* mf = ecs.renderables().try_get(muzzle_flash_entity)) {
            mf->visible = w->muzzle_flash_timer > 0.0f;
            if (mf->visible) {
                const float fa = std::clamp(w->muzzle_flash_timer / 0.048f, 0.0f, 1.0f);
                const float s = 0.55f + 0.55f * fa;
                const Mat4x4 fl = mul_mat4(
                    make_translation_mat4(pistol_muzzle_offset.x, pistol_muzzle_offset.y, pistol_muzzle_offset.z),
                    mul_mat4(make_rot_z(w->muzzle_flash_spin), make_scale_mat4(s, s, s)));
                const Mat4x4 fw = mul_mat4(gun_world, fl);
                update_entity_xform(muzzle_flash_entity, fw, true, fw);
                mf->tint = {1.0f, 0.56f, 0.14f};
                mf->emissive = 4.2f * fa;
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

void TacticalRangeSimulation::reset() {
    impl_->reset_state();
}

void TacticalRangeSimulation::set_ads_requested(bool active) {
    EventPayload ev{};
    ev.set_bool("active", active);
    impl_->events.emit("weapon.ads_changed", ev);
}

void TacticalRangeSimulation::request_fire() {
    impl_->events.enqueue("weapon.fire_requested");
}

void TacticalRangeSimulation::request_reload() {
    impl_->events.enqueue("weapon.reload_requested");
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
    impl_->step_sim(delta_seconds, paused, camera);
}

float TacticalRangeSimulation::current_vertical_fov_radians() const {
    const WeaponComponent* w = impl_->ecs.weapons().try_get(impl_->weapon_entity);
    const float t = w != nullptr ? w->ads_alpha : 0.0f;
    const float smooth_t = t * t * (3.0f - 2.0f * t);
    return 1.15f * (1.0f - smooth_t) + 0.84f * smooth_t;
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
    const float bloom = w != nullptr ? w->crosshair_bloom : 0.0f;
    h.ads_alpha = ads_alpha;
    h.crosshair_spread = std::clamp(0.18f * (1.0f - 0.72f * ads_alpha) + bloom * 0.55f, 0.05f, 1.5f);
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
    };
}

void TacticalRangeSimulation::build_render_frame(
    const CameraBasis& camera,
    const CameraBasis& previous_camera,
    TacticalRangeFrame& out_frame) {
    impl_->sync_ecs_render_transforms(camera, previous_camera);
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
        out_frame.draws[idx] = RangeDrawCall{r.mesh, idx, r.viewmodel};
    }

    out_frame.hud = hud_snapshot(camera);
}

}
