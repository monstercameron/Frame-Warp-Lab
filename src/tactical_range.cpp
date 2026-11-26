#include "tactical_range.h"

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

struct TargetSlot {
    Float3 base_pos{};
    JPH::BodyID anchor_id{};
    JPH::BodyID plate_id{};
    JPH::Ref<JPH::HingeConstraint> hinge;
    bool knocked_down = false;
    float reset_timer = 0.0f;
    float hit_flash = 0.0f;
    Mat4x4 prev_model{};
    bool has_prev = false;
};

struct CasingSlot {
    JPH::BodyID body_id{};
    bool active = false;
    float age = 0.0f;
    Mat4x4 prev_model{};
    bool has_prev = false;
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

    std::array<TargetSlot, k_range_target_count> targets{};
    std::array<CasingSlot, k_range_casing_count> casings{};
    std::uint32_t next_casing = 0;

    double phys_accum = 0.0;
    float range_time = 0.0f;

    std::int32_t hp = 100;
    std::int32_t armor = 100;
    std::int32_t ammo_in_mag = 12;
    std::int32_t ammo_reserve = 48;
    std::int32_t shots_fired = 0;
    std::int32_t hits = 0;
    std::int32_t headshots = 0;
    std::int32_t score = 0;

    bool ads_requested = false;
    bool fire_queued = false;
    bool reload_queued = false;

    float ads_alpha = 0.0f;
    float fire_cooldown = 0.0f;
    float slide_blowback = 0.0f;
    float trigger_pull = 0.0f;
    float recoil_kick = 0.0f;
    float recoil_pitch = 0.0f;
    float recoil_yaw = 0.0f;
    float recoil_roll = 0.0f;
    float crosshair_bloom = 0.0f;
    float reload_timer = 0.0f;
    bool reloading = false;

    float muzzle_flash_timer = 0.0f;
    float muzzle_flash_spin = 0.0f;
    Float3 muzzle_world_pos{0.0f, 1.4f, 2.0f};

    float tracer_timer = 0.0f;
    Float3 tracer_start{};
    Float3 tracer_end{};

    float impact_timer = 0.0f;
    Float3 impact_pos{};
    bool impact_was_target = false;

    float hit_marker_timer = 0.0f;
    bool hit_marker_headshot = false;

    std::array<Mat4x4, 8> prev_vm_models{};
    bool has_prev_vm = false;

    Impl() {
        ensure_jolt_initialized();
        physics.Init(512, 0, 512, 512, bp_interface, obj_vs_bp_filter, obj_pair_filter);
        physics.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        build_physics_world();
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

    void build_physics_world() {
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

        auto& bi = physics.GetBodyInterface();
        for (std::uint32_t i = 0; i < k_range_target_count; ++i) {
            TargetSlot& slot = targets[i];
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
        }

        physics.OptimizeBroadPhase();
    }

    void reset_state() {
        auto& bi = physics.GetBodyInterface();
        for (auto& slot : targets) {
            slot.knocked_down = false;
            slot.reset_timer = 0.0f;
            slot.hit_flash = 0.0f;
            slot.has_prev = false;
            bi.SetPositionAndRotation(
                slot.plate_id,
                JPH::RVec3(slot.base_pos.x, slot.base_pos.y + 0.72f, slot.base_pos.z),
                JPH::Quat::sIdentity(),
                JPH::EActivation::Activate);
            bi.SetLinearAndAngularVelocity(slot.plate_id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            slot.hinge->SetMotorState(JPH::EMotorState::Position);
            slot.hinge->SetTargetAngle(0.0f);
        }
        for (std::uint32_t i = 0; i < k_range_casing_count; ++i) {
            casings[i].active = false;
            casings[i].age = 0.0f;
            casings[i].has_prev = false;
            bi.DeactivateBody(casings[i].body_id);
            bi.SetPosition(casings[i].body_id, JPH::RVec3(0.0f, -5.0f - static_cast<float>(i), 0.0f), JPH::EActivation::DontActivate);
        }
        hp = 100;
        armor = 100;
        ammo_in_mag = 12;
        ammo_reserve = 48;
        shots_fired = 0;
        hits = 0;
        headshots = 0;
        score = 0;
        ads_alpha = 0.0f;
        fire_cooldown = 0.0f;
        slide_blowback = 0.0f;
        trigger_pull = 0.0f;
        recoil_kick = 0.0f;
        recoil_pitch = 0.0f;
        recoil_yaw = 0.0f;
        recoil_roll = 0.0f;
        crosshair_bloom = 0.0f;
        reload_timer = 0.0f;
        reloading = false;
        muzzle_flash_timer = 0.0f;
        tracer_timer = 0.0f;
        impact_timer = 0.0f;
        hit_marker_timer = 0.0f;
        has_prev_vm = false;
        phys_accum = 0.0;
    }

    Mat4x4 compute_pistol_root_local() const {
        const float t = ads_alpha;
        const float smooth_t = t * t * (3.0f - 2.0f * t);
        const float sway_scale = 1.0f - 0.80f * smooth_t;
        const float sway_x = std::sin(range_time * 1.7f) * 0.0045f * sway_scale;
        const float sway_y = std::cos(range_time * 3.4f) * 0.0032f * sway_scale;

        float px = 0.175f * (1.0f - smooth_t) + sway_x;
        float py = -0.155f * (1.0f - smooth_t) + (-0.068f) * smooth_t + sway_y;
        float pz = 0.42f * (1.0f - smooth_t) + 0.34f * smooth_t;

        pz -= recoil_kick * (0.042f - 0.015f * smooth_t);
        py += recoil_kick * 0.010f;

        float pitch = -recoil_kick * (0.24f - 0.10f * smooth_t);
        float roll = recoil_roll * 0.12f;
        float yaw = -0.025f * (1.0f - smooth_t);

        if (reloading) {
            const float u = std::clamp(1.0f - reload_timer / 1.30f, 0.0f, 1.0f);
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

    void execute_fire(const CameraBasis& camera) {
        if (reloading || fire_cooldown > 0.0f) return;
        if (ammo_in_mag <= 0) {
            if (ammo_reserve > 0) {
                reloading = true;
                reload_timer = 1.30f;
            }
            return;
        }

        --ammo_in_mag;
        ++shots_fired;
        fire_cooldown = 0.135f;
        slide_blowback = 1.0f;
        trigger_pull = 1.0f;
        recoil_kick = std::min(recoil_kick + 0.85f, 1.25f);
        recoil_pitch = std::min(recoil_pitch + (0.024f - 0.008f * ads_alpha), 0.075f);
        const float dir_sign = ((shots_fired & 1) != 0) ? 1.0f : -1.0f;
        recoil_yaw += dir_sign * (0.0045f * (1.0f - 0.45f * ads_alpha));
        recoil_roll = dir_sign * 0.85f;
        crosshair_bloom = std::min(crosshair_bloom + 0.72f, 1.35f);
        muzzle_flash_timer = 0.048f;
        muzzle_flash_spin = static_cast<float>(shots_fired) * 1.17f;

        const Mat4x4 cam_world = camera_basis_to_world_mat4(camera);
        const Mat4x4 gun_world = mul_mat4(cam_world, compute_pistol_root_local());
        muzzle_world_pos = transform_point(gun_world, {0.0f, 0.042f, 0.145f});

        const Float3 eject_pos = transform_point(gun_world, {0.028f, 0.050f, 0.025f});
        {
            CasingSlot& c = casings[next_casing % k_range_casing_count];
            ++next_casing;
            c.active = true;
            c.age = 0.0f;
            c.has_prev = false;
            auto& bi = physics.GetBodyInterface();
            bi.SetPositionAndRotation(
                c.body_id,
                JPH::RVec3(eject_pos.x, eject_pos.y, eject_pos.z),
                JPH::Quat::sIdentity(),
                JPH::EActivation::Activate);
            const JPH::Vec3 eject_vel(
                camera.right.x * 2.4f + camera.up.x * 1.8f - camera.forward.x * 0.4f,
                camera.right.y * 2.4f + camera.up.y * 1.8f - camera.forward.y * 0.4f,
                camera.right.z * 2.4f + camera.up.z * 1.8f - camera.forward.z * 0.4f);
            bi.SetLinearAndAngularVelocity(c.body_id, eject_vel, JPH::Vec3(18.0f, 11.0f, -24.0f));
        }

        const float spread_angle = (1.0f - ads_alpha) * (0.0035f + crosshair_bloom * 0.005f);
        const float sx = std::sin(static_cast<float>(shots_fired) * 12.9898f) * spread_angle;
        const float sy = std::cos(static_cast<float>(shots_fired) * 78.233f) * spread_angle;
        Float3 ray_dir{
            camera.forward.x + camera.right.x * sx + camera.up.x * sy,
            camera.forward.y + camera.right.y * sx + camera.up.y * sy,
            camera.forward.z + camera.right.z * sx + camera.up.z * sy};
        const float rlen = std::sqrt(ray_dir.x * ray_dir.x + ray_dir.y * ray_dir.y + ray_dir.z * ray_dir.z);
        ray_dir = {ray_dir.x / rlen, ray_dir.y / rlen, ray_dir.z / rlen};

        constexpr float max_ray_dist = 45.0f;
        const JPH::RRayCast ray{
            JPH::RVec3(camera.position.x, camera.position.y, camera.position.z),
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

            for (auto& slot : targets) {
                if (slot.plate_id == hit_result.mBodyID) {
                    impact_was_target = true;
                    const float local_h = tracer_end.y - slot.base_pos.y;
                    const bool is_headshot = !slot.knocked_down && local_h >= 1.06f;
                    ++hits;
                    if (is_headshot) {
                        ++headshots;
                        score += 250;
                    } else {
                        score += 100;
                    }
                    hit_marker_timer = 0.22f;
                    hit_marker_headshot = is_headshot;
                    slot.hit_flash = 1.0f;
                    slot.knocked_down = true;
                    slot.reset_timer = 2.4f;
                    slot.hinge->SetMotorState(JPH::EMotorState::Off);

                    auto& bi = physics.GetBodyInterface();
                    bi.ActivateBody(slot.plate_id);
                    const float impulse_mag = is_headshot ? 135.0f : 105.0f;
                    bi.AddImpulse(
                        slot.plate_id,
                        JPH::Vec3(ray_dir.x * impulse_mag, ray_dir.y * impulse_mag * 0.25f, ray_dir.z * impulse_mag),
                        hp_jolt);
                    break;
                }
            }
        } else {
            tracer_end = {
                camera.position.x + ray_dir.x * max_ray_dist,
                camera.position.y + ray_dir.y * max_ray_dist,
                camera.position.z + ray_dir.z * max_ray_dist};
        }
    }

    void step_sim(double delta_seconds, bool paused, const CameraBasis& camera) {
        if (paused || delta_seconds <= 0.0) return;
        const float dt = static_cast<float>(std::min(delta_seconds, 0.05));
        range_time += dt;

        const float ads_target = (ads_requested && !reloading) ? 1.0f : 0.0f;
        const float ads_blend = 1.0f - std::exp(-dt * 18.0f);
        ads_alpha += (ads_target - ads_alpha) * ads_blend;

        if (fire_cooldown > 0.0f) fire_cooldown = std::max(0.0f, fire_cooldown - dt);
        if (muzzle_flash_timer > 0.0f) muzzle_flash_timer = std::max(0.0f, muzzle_flash_timer - dt);
        if (tracer_timer > 0.0f) tracer_timer = std::max(0.0f, tracer_timer - dt);
        if (impact_timer > 0.0f) impact_timer = std::max(0.0f, impact_timer - dt);
        if (hit_marker_timer > 0.0f) hit_marker_timer = std::max(0.0f, hit_marker_timer - dt);

        if (reload_queued && !reloading && ammo_in_mag < 12 && ammo_reserve > 0) {
            reloading = true;
            reload_timer = 1.30f;
        }
        reload_queued = false;

        if (reloading) {
            reload_timer -= dt;
            if (reload_timer <= 0.0f) {
                const std::int32_t needed = 12 - ammo_in_mag;
                const std::int32_t load = std::min(needed, ammo_reserve);
                ammo_in_mag += load;
                ammo_reserve -= load;
                if (ammo_reserve < 12) ammo_reserve = 48;
                reloading = false;
                reload_timer = 0.0f;
                slide_blowback = 0.0f;
            }
        }

        if (fire_queued) {
            execute_fire(camera);
            fire_queued = false;
        }

        const float slide_target = (ammo_in_mag == 0 && !reloading) ? 0.92f : 0.0f;
        slide_blowback += (slide_target - slide_blowback) * (1.0f - std::exp(-dt * 28.0f));
        trigger_pull *= std::exp(-dt * 24.0f);
        recoil_kick *= std::exp(-dt * 15.0f);
        recoil_pitch *= std::exp(-dt * 11.0f);
        recoil_yaw *= std::exp(-dt * 11.0f);
        recoil_roll *= std::exp(-dt * 14.0f);
        crosshair_bloom *= std::exp(-dt * 9.0f);

        for (auto& slot : targets) {
            if (slot.hit_flash > 0.0f) {
                slot.hit_flash = std::max(0.0f, slot.hit_flash - dt * 4.5f);
            }
            if (slot.knocked_down) {
                slot.reset_timer -= dt;
                if (slot.reset_timer <= 0.0f) {
                    slot.knocked_down = false;
                    slot.hinge->SetMotorState(JPH::EMotorState::Position);
                    slot.hinge->SetTargetAngle(0.0f);
                    physics.GetBodyInterface().ActivateBody(slot.plate_id);
                }
            }
        }
        for (auto& c : casings) {
            if (c.active) {
                c.age += dt;
                if (c.age > 6.0f) {
                    c.active = false;
                    physics.GetBodyInterface().DeactivateBody(c.body_id);
                }
            }
        }

        constexpr double fixed_step = 1.0 / 240.0;
        phys_accum = std::min(phys_accum + static_cast<double>(dt), fixed_step * 6.0);
        int substeps = 0;
        while (phys_accum >= fixed_step && substeps < 6) {
            physics.Update(static_cast<float>(fixed_step), 1, &temp_allocator, &job_system);
            phys_accum -= fixed_step;
            ++substeps;
        }
    }
};

TacticalRangeSimulation::TacticalRangeSimulation() : impl_(std::make_unique<Impl>()) {}
TacticalRangeSimulation::~TacticalRangeSimulation() = default;

void TacticalRangeSimulation::reset() {
    impl_->reset_state();
}

void TacticalRangeSimulation::set_ads_requested(bool active) {
    impl_->ads_requested = active;
}

void TacticalRangeSimulation::request_fire() {
    impl_->fire_queued = true;
}

void TacticalRangeSimulation::request_reload() {
    impl_->reload_queued = true;
}

void TacticalRangeSimulation::step(double delta_seconds, bool paused, const CameraBasis& camera) {
    impl_->step_sim(delta_seconds, paused, camera);
}

float TacticalRangeSimulation::current_vertical_fov_radians() const {
    const float t = impl_->ads_alpha;
    const float smooth_t = t * t * (3.0f - 2.0f * t);
    return 1.15f * (1.0f - smooth_t) + 0.84f * smooth_t;
}

float TacticalRangeSimulation::camera_recoil_pitch_radians() const {
    return impl_->recoil_pitch;
}

float TacticalRangeSimulation::camera_recoil_yaw_radians() const {
    return impl_->recoil_yaw;
}

TacticalHudSnapshot TacticalRangeSimulation::hud_snapshot(const CameraBasis& camera) const {
    TacticalHudSnapshot h{};
    h.hp = impl_->hp;
    h.armor = impl_->armor;
    h.ammo_in_mag = impl_->ammo_in_mag;
    h.ammo_reserve = impl_->ammo_reserve;
    h.shots_fired = impl_->shots_fired;
    h.hits = impl_->hits;
    h.headshots = impl_->headshots;
    h.score = impl_->score;
    h.ads_alpha = impl_->ads_alpha;
    h.crosshair_spread = std::clamp(0.18f * (1.0f - 0.72f * impl_->ads_alpha) + impl_->crosshair_bloom * 0.55f, 0.05f, 1.5f);
    h.hit_marker_alpha = std::clamp(impl_->hit_marker_timer / 0.22f, 0.0f, 1.0f);
    h.hit_marker_headshot = impl_->hit_marker_headshot ? 1.0f : 0.0f;
    h.reload_progress = impl_->reloading ? std::clamp(1.0f - impl_->reload_timer / 1.30f, 0.0f, 1.0f) : 0.0f;
    h.muzzle_flash_alpha = std::clamp(impl_->muzzle_flash_timer / 0.048f, 0.0f, 1.0f);
    h.muzzle_world_pos = impl_->muzzle_world_pos;
    h.player_pos = camera.position;
    h.player_yaw_rad = std::atan2(camera.forward.x, camera.forward.z);
    std::int32_t up_count = 0;
    for (std::uint32_t i = 0; i < k_range_target_count; ++i) {
        const auto& slot = impl_->targets[i];
        const bool is_up = !slot.knocked_down;
        if (is_up) ++up_count;
        h.target_radar_xy_state[i * 4U + 0U] = slot.base_pos.x;
        h.target_radar_xy_state[i * 4U + 1U] = slot.base_pos.z;
        h.target_radar_xy_state[i * 4U + 2U] = is_up ? 1.0f : 0.0f;
        h.target_radar_xy_state[i * 4U + 3U] = slot.hit_flash;
    }
    h.targets_up = up_count;
    return h;
}

void TacticalRangeSimulation::build_render_frame(
    const CameraBasis& camera,
    const CameraBasis& previous_camera,
    TacticalRangeFrame& out_frame) {
    out_frame.draw_count = 0;
    auto push_instance = [&](RangeMeshKind mesh, const Mat4x4& model, const Mat4x4& prev_model, float tr, float tg, float tb, float em, bool viewmodel) {
        if (out_frame.draw_count >= out_frame.instances.size()) return;
        const std::uint32_t idx = out_frame.draw_count++;
        std::memcpy(out_frame.instances[idx].model, model.m, sizeof(model.m));
        std::memcpy(out_frame.instances[idx].prev_model, prev_model.m, sizeof(prev_model.m));
        out_frame.instances[idx].tint_emissive[0] = tr;
        out_frame.instances[idx].tint_emissive[1] = tg;
        out_frame.instances[idx].tint_emissive[2] = tb;
        out_frame.instances[idx].tint_emissive[3] = em;
        out_frame.draws[idx] = RangeDrawCall{mesh, idx, viewmodel};
    };

    const Mat4x4 id = identity_mat4();
    push_instance(RangeMeshKind::static_world, id, id, 1.0f, 1.0f, 1.0f, 0.0f, false);

    const auto& bi = impl_->physics.GetBodyInterface();
    for (std::uint32_t i = 0; i < k_range_target_count; ++i) {
        auto& slot = impl_->targets[i];
        const JPH::RMat44 body_xform = bi.GetCenterOfMassTransform(slot.plate_id);
        const Mat4x4 model = mul_mat4(from_jolt_mat44(body_xform), make_translation_mat4(0.0f, -0.72f, 0.0f));
        const Mat4x4 prev_m = slot.has_prev ? slot.prev_model : model;
        slot.prev_model = model;
        slot.has_prev = true;
        push_instance(
            RangeMeshKind::target_plate,
            model,
            prev_m,
            1.0f + slot.hit_flash * 0.8f,
            1.0f + slot.hit_flash * 0.35f,
            1.0f,
            slot.hit_flash * 0.55f,
            false);
    }

    for (std::uint32_t i = 0; i < k_range_casing_count; ++i) {
        auto& c = impl_->casings[i];
        if (!c.active) continue;
        const Mat4x4 model = from_jolt_mat44(bi.GetCenterOfMassTransform(c.body_id));
        const Mat4x4 prev_m = c.has_prev ? c.prev_model : model;
        c.prev_model = model;
        c.has_prev = true;
        push_instance(RangeMeshKind::brass_casing, model, prev_m, 1.0f, 1.0f, 1.0f, 0.0f, false);
    }

    if (impl_->tracer_timer > 0.0f) {
        const float alpha = impl_->tracer_timer / 0.085f;
        const Mat4x4 tr_m = make_segment_mat4(impl_->tracer_start, impl_->tracer_end, 0.008f * (0.5f + 0.5f * alpha));
        push_instance(RangeMeshKind::bullet_tracer, tr_m, tr_m, 1.0f, 0.82f, 0.32f, 8.5f * alpha, false);
    }

    if (impl_->impact_timer > 0.0f) {
        const float alpha = impl_->impact_timer / 0.18f;
        const float sc = (0.045f + 0.095f * (1.0f - alpha)) * (impl_->impact_was_target ? 1.45f : 0.85f);
        const Mat4x4 sp_m = mul_mat4(
            make_translation_mat4(impl_->impact_pos.x, impl_->impact_pos.y, impl_->impact_pos.z),
            mul_mat4(make_rot_z(alpha * 4.5f), make_scale_mat4(sc, sc, sc)));
        push_instance(RangeMeshKind::impact_spark, sp_m, sp_m, 1.0f, 0.65f, 0.18f, 9.0f * alpha, false);
    }

    const Mat4x4 cam_world = camera_basis_to_world_mat4(camera);
    const Mat4x4 prev_cam_world = camera_basis_to_world_mat4(previous_camera);
    const Mat4x4 gun_local = impl_->compute_pistol_root_local();
    const Mat4x4 gun_world = mul_mat4(cam_world, gun_local);
    const Mat4x4 prev_gun_world = impl_->has_prev_vm ? impl_->prev_vm_models[0] : mul_mat4(prev_cam_world, gun_local);

    const Mat4x4 slide_local = make_translation_mat4(0.0f, 0.0f, -0.048f * impl_->slide_blowback);
    const Mat4x4 slide_world = mul_mat4(gun_world, slide_local);
    const Mat4x4 prev_slide_world = impl_->has_prev_vm ? impl_->prev_vm_models[1] : mul_mat4(prev_gun_world, slide_local);

    float mag_drop = 0.0f;
    if (impl_->reloading) {
        const float u = std::clamp(1.0f - impl_->reload_timer / 1.30f, 0.0f, 1.0f);
        if (u < 0.35f) mag_drop = (u / 0.35f) * 0.14f;
        else if (u < 0.65f) mag_drop = 0.14f;
        else if (u < 0.88f) mag_drop = (1.0f - (u - 0.65f) / 0.23f) * 0.14f;
    }
    const Mat4x4 mag_local = make_translation_mat4(0.0f, -mag_drop, -mag_drop * 0.28f);
    const Mat4x4 mag_world = mul_mat4(gun_world, mag_local);
    const Mat4x4 prev_mag_world = impl_->has_prev_vm ? impl_->prev_vm_models[2] : mul_mat4(prev_gun_world, mag_local);

    impl_->prev_vm_models[0] = gun_world;
    impl_->prev_vm_models[1] = slide_world;
    impl_->prev_vm_models[2] = mag_world;
    impl_->has_prev_vm = true;

    push_instance(RangeMeshKind::pistol_frame, gun_world, prev_gun_world, 1.0f, 1.0f, 1.0f, 0.0f, true);
    push_instance(RangeMeshKind::pistol_barrel, gun_world, prev_gun_world, 1.0f, 1.0f, 1.0f, 0.0f, true);
    push_instance(RangeMeshKind::pistol_slide, slide_world, prev_slide_world, 1.0f, 1.0f, 1.0f, 0.0f, true);
    push_instance(RangeMeshKind::pistol_sights, slide_world, prev_slide_world, 0.18f, 1.0f, 0.35f, 3.6f, true);
    push_instance(RangeMeshKind::pistol_magazine, mag_world, prev_mag_world, 1.0f, 1.0f, 1.0f, 0.0f, true);

    if (impl_->muzzle_flash_timer > 0.0f) {
        const float fa = impl_->muzzle_flash_timer / 0.048f;
        const float fs = 0.45f + 0.40f * fa;
        const Mat4x4 flash_local = mul_mat4(
            make_translation_mat4(0.0f, 0.045f, 0.160f),
            mul_mat4(make_rot_z(impl_->muzzle_flash_spin), make_scale_mat4(fs, fs, fs)));
        const Mat4x4 flash_world = mul_mat4(gun_world, flash_local);
        push_instance(RangeMeshKind::muzzle_flash, flash_world, flash_world, 1.0f, 0.56f, 0.14f, 4.2f * fa, true);
    }

    out_frame.hud = hud_snapshot(camera);
}

}
