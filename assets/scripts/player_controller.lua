local M = {}

local GRAVITY = -21.0
local WALK_SPEED = 4.8
local SPRINT_SPEED = 7.2
local CROUCH_SPEED = 2.35
local GROUND_ACCEL = 42.0
local GROUND_FRICTION = 10.5
local AIR_ACCEL = 12.0
local JUMP_SPEED = 6.6
local MAX_STEP_UP = 0.46
local MAX_STEP_DOWN = 0.44
local SLOPE_MIN_NY = 0.65
local SKIN = 0.012
local COYOTE_MAX = 0.10
local JUMP_BUF_MAX = 0.10
local STAND_CAP_H = 1.72
local CROUCH_CAP_H = 1.15

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function capsule_center_y(feet_y, cap_h)
    return feet_y + cap_h * 0.5
end

local function capsule_half_seg(radius, cap_h)
    return math.max(0.08, cap_h * 0.5 - radius)
end

local function apply_ground_friction(vx, vz, dt)
    local speed = math.sqrt(vx * vx + vz * vz)
    if speed < 1e-4 then
        return 0.0, 0.0
    end
    local control = math.max(speed, 1.35)
    local drop = control * GROUND_FRICTION * dt
    local new_speed = math.max(0.0, speed - drop)
    local scale = new_speed / speed
    return vx * scale, vz * scale
end

local function accelerate(vx, vz, wish_x, wish_z, wish_speed, accel, dt)
    local cur_proj = vx * wish_x + vz * wish_z
    local add_speed = wish_speed - cur_proj
    if add_speed <= 0.0 then
        return vx, vz
    end
    local accel_speed = math.min(accel * dt * wish_speed, add_speed)
    return vx + wish_x * accel_speed, vz + wish_z * accel_speed
end

local function try_step_up(fx, fy, fz, dx, dz, radius, cap_h)
    local dlen = math.sqrt(dx * dx + dz * dz)
    if dlen < 1e-4 then
        return false, fx, fy, fz
    end
    local half_seg = capsule_half_seg(radius, cap_h)
    local up_hit = physics.sweep_capsule(fx, capsule_center_y(fy, cap_h), fz, 0.0, MAX_STEP_UP, 0.0, radius, half_seg)
    local up_dist = MAX_STEP_UP
    if up_hit.hit then
        up_dist = math.max(0.0, up_hit.fraction * MAX_STEP_UP - SKIN)
    end
    if up_dist < 0.05 then
        return false, fx, fy, fz
    end

    local high_y = fy + up_dist
    local fwd_hit = physics.sweep_capsule(fx, capsule_center_y(high_y, cap_h), fz, dx, 0.0, dz, radius, half_seg)
    local move_frac = 1.0
    if fwd_hit.hit then
        move_frac = math.max(0.0, fwd_hit.fraction - (SKIN / dlen))
    end
    local moved_dist = move_frac * dlen
    if moved_dist < 0.004 then
        return false, fx, fy, fz
    end

    local nx = fx + dx * move_frac
    local nz = fz + dz * move_frac
    local down_dist = up_dist + 0.06
    local down_hit = physics.sweep_capsule(nx, capsule_center_y(high_y, cap_h), nz, 0.0, -down_dist, 0.0, radius, half_seg)
    if down_hit.hit and down_hit.ny >= SLOPE_MIN_NY then
        local land_y = high_y - down_hit.fraction * down_dist + 0.003
        if land_y > fy + 0.008 and land_y <= fy + MAX_STEP_UP + 0.02 then
            return true, nx, land_y, nz
        end
    end
    return false, fx, fy, fz
end

function M.on_init(entity_id)
    events.on("player.teleport", function(payload)
        local p = ecs.get_player(entity_id)
        if p == nil then
            return
        end
        ecs.set_player(entity_id, {
            feet_x = payload.feet_x or p.feet_x,
            feet_y = payload.feet_y or p.feet_y,
            feet_z = payload.feet_z or p.feet_z,
            vx = 0.0,
            vy = 0.0,
            vz = 0.0,
            grounded = true,
            touching_wall = false,
            crouched = false,
            eye_height = p.stand_eye_height,
            capsule_height = STAND_CAP_H,
            coyote_timer = COYOTE_MAX,
            jump_buffer_timer = 0.0,
            bob_x = 0.0,
            bob_y = 0.0,
            landing_dip = 0.0,
            horizontal_speed = 0.0,
        })
    end)

    events.on("player.jump", function()
        local p = ecs.get_player(entity_id)
        if p ~= nil then
            ecs.set_player(entity_id, { jump_buffer_timer = JUMP_BUF_MAX })
        end
    end)
end

function M.on_update(entity_id, dt)
    local p = ecs.get_player(entity_id)
    if p == nil or not p.manual_active then
        return
    end

    dt = math.min(dt, 0.033)
    local radius = p.capsule_radius
    local fx, fy, fz = p.feet_x, p.feet_y, p.feet_z
    local vx, vy, vz = p.vx, p.vy, p.vz
    local grounded = p.grounded
    local crouched = p.crouched
    local cap_h = p.capsule_height
    local eye_h = p.eye_height
    local coyote = p.coyote_timer
    local jump_buf = p.jump_buffer_timer
    local collisions = p.collision_count
    local touching_wall = false

    local want_crouch = p.crouch_held
    if not want_crouch and crouched then
        local head_ray = physics.raycast(fx, fy + CROUCH_CAP_H - 0.05, fz, 0.0, 1.0, 0.0, STAND_CAP_H - CROUCH_CAP_H + 0.10)
        if head_ray.hit then
            want_crouch = true
        end
    end
    crouched = want_crouch
    cap_h = crouched and CROUCH_CAP_H or STAND_CAP_H
    local target_eye = crouched and p.crouch_eye_height or p.stand_eye_height
    eye_h = eye_h + (target_eye - eye_h) * (1.0 - math.exp(-dt * 14.0))

    local half_seg = capsule_half_seg(radius, cap_h)
    local pre_ov = physics.resolve_capsule_overlap(fx, capsule_center_y(fy, cap_h), fz, radius, half_seg)
    if pre_ov.penetrated then
        fx = fx + pre_ov.push_x
        fy = fy + pre_ov.push_y
        fz = fz + pre_ov.push_z
    end

    if p.jump_pressed then
        jump_buf = JUMP_BUF_MAX
    else
        jump_buf = math.max(0.0, jump_buf - dt)
    end
    if grounded then
        coyote = COYOTE_MAX
    else
        coyote = math.max(0.0, coyote - dt)
    end

    local sin_y = math.sin(p.yaw)
    local cos_y = math.cos(p.yaw)
    local wish_x = sin_y * p.move_forward + cos_y * p.move_right
    local wish_z = cos_y * p.move_forward - sin_y * p.move_right
    local wish_len = math.sqrt(wish_x * wish_x + wish_z * wish_z)
    if wish_len > 1e-4 then
        wish_x = wish_x / wish_len
        wish_z = wish_z / wish_len
    end

    local max_speed = WALK_SPEED
    if crouched then
        max_speed = CROUCH_SPEED
    elseif p.sprint_held then
        max_speed = SPRINT_SPEED
    end
    local wish_speed = math.min(wish_len, 1.0) * max_speed

    if grounded then
        vx, vz = apply_ground_friction(vx, vz, dt)
        vx, vz = accelerate(vx, vz, wish_x, wish_z, wish_speed, GROUND_ACCEL, dt)
    else
        vx, vz = accelerate(vx, vz, wish_x, wish_z, math.min(wish_speed, 3.2), AIR_ACCEL, dt)
    end

    local jumped_this_frame = false
    if jump_buf > 0.0 and coyote > 0.0 then
        vy = JUMP_SPEED
        grounded = false
        jumped_this_frame = true
        coyote = 0.0
        jump_buf = 0.0
        events.emit("player.jumped", { entity = entity_id, y = fy })
    end

    local rem_x = vx * dt
    local rem_y = 0.0
    local rem_z = vz * dt
    local wall_hits = 0
    local prev_wnx, prev_wnz = 0.0, 0.0

    for iter = 1, 5 do
        local dist3 = math.sqrt(rem_x * rem_x + rem_y * rem_y + rem_z * rem_z)
        if dist3 < 1e-5 then
            break
        end
        local sweep = physics.sweep_capsule(fx, capsule_center_y(fy, cap_h), fz, rem_x, rem_y, rem_z, radius, half_seg)
        if not sweep.hit then
            fx = fx + rem_x
            fy = fy + rem_y
            fz = fz + rem_z
            break
        end

        local safe_frac = math.max(0.0, sweep.fraction - (SKIN / dist3))
        fx = fx + rem_x * safe_frac
        fy = fy + rem_y * safe_frac
        fz = fz + rem_z * safe_frac
        rem_x = rem_x * (1.0 - safe_frac)
        rem_y = rem_y * (1.0 - safe_frac)
        rem_z = rem_z * (1.0 - safe_frac)

        if sweep.ny >= SLOPE_MIN_NY then
            local slope_y = -(rem_x * sweep.nx + rem_z * sweep.nz) / math.max(sweep.ny, SLOPE_MIN_NY)
            if slope_y > rem_y then
                rem_y = slope_y + 0.002
            else
                local dot3 = rem_x * sweep.nx + rem_y * sweep.ny + rem_z * sweep.nz
                if dot3 < 0.0 then
                    rem_x = rem_x - sweep.nx * dot3
                    rem_y = rem_y - sweep.ny * dot3
                    rem_z = rem_z - sweep.nz * dot3
                end
            end
            collisions = collisions + 1
        else
            local stepped = false
            if grounded then
                local sx, sy, sz
                stepped, sx, sy, sz = try_step_up(fx, fy, fz, rem_x, rem_z, radius, cap_h)
                if stepped then
                    fx, fy, fz = sx, sy, sz
                    collisions = collisions + 1
                    break
                end
            end

            local nlen = math.sqrt(sweep.nx * sweep.nx + sweep.nz * sweep.nz)
            if nlen > 1e-4 then
                local wnx = sweep.nx / nlen
                local wnz = sweep.nz / nlen
                local dot_rem = rem_x * wnx + rem_z * wnz
                if dot_rem < 0.0 then
                    rem_x = rem_x - wnx * dot_rem
                    rem_z = rem_z - wnz * dot_rem
                end
                local dot_v = vx * wnx + vz * wnz
                if dot_v < 0.0 then
                    vx = vx - wnx * dot_v
                    vz = vz - wnz * dot_v
                end
                if wall_hits > 0 and (wnx * prev_wnx + wnz * prev_wnz) < 0.85 then
                    if (rem_x * prev_wnx + rem_z * prev_wnz) < 0.0 then
                        rem_x = 0.0
                        rem_z = 0.0
                        vx = 0.0
                        vz = 0.0
                    end
                end
                prev_wnx = wnx
                prev_wnz = wnz
                wall_hits = wall_hits + 1
                fx = fx + wnx * 0.002
                fz = fz + wnz * 0.002
            else
                rem_x = 0.0
                rem_z = 0.0
            end
            rem_y = 0.0
            touching_wall = true
            collisions = collisions + 1
        end
    end

    local was_grounded = grounded
    local prev_vy = vy

    if grounded and not jumped_this_frame then
        local probe_up = 0.04
        local total_down = probe_up + MAX_STEP_DOWN
        local snap = physics.sweep_capsule(fx, capsule_center_y(fy + probe_up, cap_h), fz, 0.0, -total_down, 0.0, radius, half_seg)
        if snap.hit and snap.ny >= SLOPE_MIN_NY then
            fy = (fy + probe_up) - snap.fraction * total_down + 0.003
            grounded = true
            vy = 0.0
        else
            grounded = false
            vy = math.max(vy + GRAVITY * dt, -28.0)
            local dy = vy * dt
            local fall_sweep = physics.sweep_capsule(fx, capsule_center_y(fy, cap_h), fz, 0.0, dy, 0.0, radius, half_seg)
            if fall_sweep.hit then
                fy = fy + dy * fall_sweep.fraction + 0.003
                if fall_sweep.ny >= SLOPE_MIN_NY then
                    grounded = true
                    vy = 0.0
                    collisions = collisions + 1
                else
                    vx = vx + fall_sweep.nx * 2.4 * dt
                    vz = vz + fall_sweep.nz * 2.4 * dt
                end
            else
                fy = fy + dy
            end
        end
    else
        vy = math.max(vy + GRAVITY * dt, -28.0)
        local dy = vy * dt
        local v_sweep = physics.sweep_capsule(fx, capsule_center_y(fy, cap_h), fz, 0.0, dy, 0.0, radius, half_seg)
        if v_sweep.hit then
            if dy < 0.0 then
                fy = fy + dy * v_sweep.fraction + 0.003
                if v_sweep.ny >= SLOPE_MIN_NY then
                    grounded = true
                    vy = 0.0
                    collisions = collisions + 1
                else
                    grounded = false
                    vx = vx + v_sweep.nx * 2.4 * dt
                    vz = vz + v_sweep.nz * 2.4 * dt
                end
            else
                fy = fy + dy * math.max(0.0, v_sweep.fraction - (SKIN / math.max(math.abs(dy), 1e-4)))
                vy = 0.0
                grounded = false
                collisions = collisions + 1
            end
        else
            fy = fy + dy
            grounded = false
        end
    end

    local overlap = physics.resolve_capsule_overlap(fx, capsule_center_y(fy, cap_h), fz, radius, half_seg)
    if overlap.penetrated then
        fx = fx + overlap.push_x
        fy = fy + overlap.push_y
        fz = fz + overlap.push_z
        collisions = collisions + 1
        if math.abs(overlap.push_x) > 0.002 or math.abs(overlap.push_z) > 0.002 then
            touching_wall = true
        end
    end

    if fy < -15.0 then
        fy = 2.0
        vy = 0.0
        grounded = false
    end

    local landing_dip = p.landing_dip * math.exp(-dt * 12.0)
    if grounded and not was_grounded and prev_vy < -3.0 then
        landing_dip = clamp(math.abs(prev_vy) * 0.014, 0.02, 0.14)
        events.emit("player.landed", { entity = entity_id, impact_vy = prev_vy, y = fy })
    end

    local horiz_speed = math.sqrt(vx * vx + vz * vz)
    local step_phase = p.step_phase
    local bob_x = p.bob_x
    local bob_y = p.bob_y
    if grounded and horiz_speed > 0.35 then
        local freq = crouched and 6.5 or (p.sprint_held and 11.5 or 9.2)
        step_phase = step_phase + dt * freq
        local amp = clamp(horiz_speed / WALK_SPEED, 0.0, 1.35)
        local target_by = math.sin(step_phase * 2.0) * 0.028 * amp
        local target_bx = math.cos(step_phase) * 0.015 * amp
        bob_y = bob_y + (target_by - bob_y) * (1.0 - math.exp(-dt * 18.0))
        bob_x = bob_x + (target_bx - bob_x) * (1.0 - math.exp(-dt * 18.0))
    else
        bob_y = bob_y * math.exp(-dt * 12.0)
        bob_x = bob_x * math.exp(-dt * 12.0)
    end

    if touching_wall then
        events.emit("player.wall_collision", { entity = entity_id, x = fx, y = fy, z = fz })
    end

    ecs.set_player(entity_id, {
        feet_x = fx,
        feet_y = fy,
        feet_z = fz,
        vx = vx,
        vy = vy,
        vz = vz,
        eye_height = eye_h,
        capsule_height = cap_h,
        jump_pressed = false,
        grounded = grounded,
        touching_wall = touching_wall,
        crouched = crouched,
        coyote_timer = coyote,
        jump_buffer_timer = jump_buf,
        step_phase = step_phase,
        bob_x = bob_x,
        bob_y = bob_y,
        landing_dip = landing_dip,
        horizontal_speed = horiz_speed,
        collision_count = collisions,
    })
end

return M
