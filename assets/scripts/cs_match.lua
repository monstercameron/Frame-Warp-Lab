local M = {}

local bots_spawned_for_dust2 = false
local match_time = 0.0

local function snap_to_ground(x, y, z)
    local hit = physics.raycast(x, y + 1.8, z, 0.0, -1.0, 0.0, 6.0)
    if hit.hit and hit.ny >= 0.60 then
        return hit.y + 0.01
    end
    return y
end

local function yaw_between(ax, az, bx, bz)
    return math.atan(bx - ax, bz - az)
end

local function angle_lerp(cur, target, alpha)
    local diff = target - cur
    while diff > 3.14159265 do diff = diff - 6.28318530 end
    while diff < -3.14159265 do diff = diff + 6.28318530 end
    return cur + diff * alpha
end

local function spawn_bots_in_world(is_dust2)
    local bots = ecs.find_by_tag("cs_bot")
    local wps = ecs.get_waypoints()
    local wp_count = #wps

    if not is_dust2 then
        for i = 1, #bots do
            ecs.set_bot(bots[i], { active = false })
        end
        bots_spawned_for_dust2 = false
        return
    end

    local player_ent = ecs.find("player")
    local pl = player_ent ~= 0 and ecs.get_player(player_ent) or nil
    local px = pl ~= nil and pl.feet_x or 0.0
    local py = pl ~= nil and pl.feet_y or 3.6
    local pz = pl ~= nil and pl.feet_z or 36.0

    local ct_offsets = {
        { 5.5,  0.0, -1.5 },
        { 9.0,  0.0,  1.6 },
        { 14.0, 0.0, -0.8 },
        { 22.0, 0.6,  1.2 },
    }
    local t_offsets = {
        { 12.5, 0.0,  0.4 },
        { 18.5, 0.0, -1.2 },
        { 26.5, 1.2,  0.8 },
        { 32.0, 2.4, -0.6 },
    }

    for i = 1, #bots do
        local b = ecs.get_bot(bots[i])
        if b ~= nil then
            local sx, sy, sz
            local wp_idx = 1
            if b.team == 0 then
                local slot = (b.slot_index % 4) + 1
                if wp_count >= 8 and slot >= 3 then
                    wp_idx = ((slot * 3) % wp_count) + 1
                    local wp = wps[wp_idx]
                    sx, sy, sz = wp.x, wp.y - 1.60, wp.z
                else
                    local o = ct_offsets[slot]
                    sx, sy, sz = px + o[1], py + o[2], pz + o[3]
                end
            else
                local slot = (b.slot_index % 4) + 1
                if wp_count >= 8 and slot >= 3 then
                    wp_idx = ((slot * 4 + 2) % wp_count) + 1
                    local wp = wps[wp_idx]
                    sx, sy, sz = wp.x, wp.y - 1.60, wp.z
                else
                    local o = t_offsets[slot]
                    sx, sy, sz = px + o[1], py + o[2], pz + o[3]
                end
            end
            sy = snap_to_ground(sx, sy, sz)
            local face_yaw = yaw_between(sx, sz, px, pz)
            ecs.set_bot(bots[i], {
                active = true,
                alive = true,
                hp = 100,
                max_hp = 100,
                x = sx,
                y = sy,
                z = sz,
                vx = 0.0,
                vy = 0.0,
                vz = 0.0,
                yaw = face_yaw,
                aim_pitch = 0.0,
                anim_clip = "Idle",
                anim_time = b.slot_index * 0.37,
                anim_speed = 1.0,
                anim_loop = true,
                hit_flash = 0.0,
                respawn_timer = 0.0,
                shoot_cooldown = 0.8 + b.slot_index * 0.25,
                waypoint_index = wp_idx,
            })
        end
    end
    bots_spawned_for_dust2 = true
end

function M.on_init(entity_id)
    events.on("range.reset", function()
        bots_spawned_for_dust2 = false
        match_time = 0.0
    end)

    events.on("bot.damaged", function(payload)
        local target_id = payload.entity
        if target_id == nil then return end
        local b = ecs.get_bot(target_id)
        if b == nil or not b.alive then return end
        local dmg = math.floor(payload.damage or 34)
        if payload.headshot then
            dmg = math.floor(dmg * 2.6)
        end
        local new_hp = math.max(0, b.hp - dmg)
        if new_hp <= 0 then
            ecs.set_bot(target_id, {
                hp = 0,
                alive = false,
                hit_flash = 1.0,
                anim_clip = "Death",
                anim_time = 0.0,
                anim_speed = 1.1,
                anim_loop = false,
                respawn_timer = 6.5,
            })
            ecs.record_bot_kill(b.team)
            events.emit("bot.killed", {
                entity = target_id,
                team = b.team,
                headshot = payload.headshot == true,
                damage = dmg,
                hit_dir_x = payload.hit_dir_x or 1.0,
                hit_dir_y = payload.hit_dir_y or 0.15,
                hit_dir_z = payload.hit_dir_z or 0.0,
            })
        else
            ecs.set_bot(target_id, {
                hp = new_hp,
                hit_flash = 1.0,
                anim_clip = "HitReact",
                anim_time = 0.0,
                anim_speed = 1.35,
                anim_loop = false,
            })
        end
    end)

    events.on("bot.test_kill", function()
        local player_ent = ecs.find("player")
        local pl = player_ent ~= 0 and ecs.get_player(player_ent) or nil
        local px = pl ~= nil and pl.feet_x or 0.0
        local pz = pl ~= nil and pl.feet_z or 36.0
        local bots = ecs.find_by_tag("cs_bot")
        local best_id = nil
        local best_d = 9999.0
        local best_dx, best_dz = 1.0, 0.0
        for i = 1, #bots do
            local b = ecs.get_bot(bots[i])
            if b ~= nil and b.active and b.alive then
                local dx = b.x - px
                local dz = b.z - pz
                local d = math.sqrt(dx * dx + dz * dz)
                if d < best_d then
                    best_d = d
                    best_id = bots[i]
                    best_dx = dx / math.max(d, 0.01)
                    best_dz = dz / math.max(d, 0.01)
                end
            end
        end
        if best_id ~= nil then
            events.emit("bot.damaged", {
                entity = best_id,
                damage = 140,
                headshot = true,
                hit_dir_x = best_dx,
                hit_dir_y = 0.18,
                hit_dir_z = best_dz,
            })
        end
    end)
end

function M.on_update(entity_id, dt)
    local is_d2 = ecs.is_dust2()
    if not is_d2 then
        if bots_spawned_for_dust2 then
            spawn_bots_in_world(false)
        end
        return
    end
    if not bots_spawned_for_dust2 then
        spawn_bots_in_world(true)
        return
    end

    dt = math.min(dt, 0.033)
    match_time = match_time + dt

    local player_ent = ecs.find("player")
    local pl = player_ent ~= 0 and ecs.get_player(player_ent) or nil
    local wps = ecs.get_waypoints()
    local wp_count = #wps
    local bots = ecs.find_by_tag("cs_bot")

    for i = 1, #bots do
        local bid = bots[i]
        local b = ecs.get_bot(bid)
        if b ~= nil and b.active then
            local hit_flash = math.max(0.0, b.hit_flash - dt * 4.5)
            local anim_time = b.anim_time + dt * b.anim_speed
            local anim_clip = b.anim_clip
            local anim_loop = b.anim_loop

            if not b.alive then
                local rtimer = b.respawn_timer - dt
                if rtimer <= 0.0 then
                    local rx, ry, rz = b.x, b.y, b.z
                    if wp_count >= 2 then
                        local wpi = ((b.waypoint_index + b.slot_index) % wp_count) + 1
                        local wp = wps[wpi]
                        rx, ry, rz = wp.x, snap_to_ground(wp.x, wp.y - 1.60, wp.z), wp.z
                    end
                    ecs.set_bot(bid, {
                        alive = true,
                        hp = 100,
                        x = rx,
                        y = ry,
                        z = rz,
                        hit_flash = 0.0,
                        anim_clip = "Idle",
                        anim_time = 0.0,
                        anim_speed = 1.0,
                        anim_loop = true,
                        respawn_timer = 0.0,
                    })
                else
                    ecs.set_bot(bid, {
                        anim_time = anim_time,
                        hit_flash = hit_flash,
                        respawn_timer = rtimer,
                    })
                end
            else
                local bx, by, bz = b.x, b.y, b.z
                local yaw = b.yaw
                local aim_pitch = b.aim_pitch
                local wp_idx = b.waypoint_index
                local shoot_cd = math.max(0.0, b.shoot_cooldown - dt)

                local best_tx, best_ty, best_tz = nil, nil, nil
                local best_dist = 26.0
                if b.team == 1 and pl ~= nil then
                    local dx = pl.feet_x - bx
                    local dz = pl.feet_z - bz
                    local d = math.sqrt(dx * dx + dz * dz)
                    if d < best_dist then
                        best_dist = d
                        best_tx, best_ty, best_tz = pl.feet_x, pl.feet_y + 1.4, pl.feet_z
                    end
                end
                for j = 1, #bots do
                    if j ~= i then
                        local ob = ecs.get_bot(bots[j])
                        if ob ~= nil and ob.active and ob.alive and ob.team ~= b.team then
                            local dx = ob.x - bx
                            local dz = ob.z - bz
                            local d = math.sqrt(dx * dx + dz * dz)
                            if d < best_dist then
                                best_dist = d
                                best_tx, best_ty, best_tz = ob.x, ob.y + 1.35, ob.z
                            end
                        end
                    end
                end

                local has_los = false
                if best_tx ~= nil and best_dist > 1.2 then
                    local ox, oy, oz = bx, by + 1.42, bz
                    local dx, dy, dz = best_tx - ox, best_ty - oy, best_tz - oz
                    local dlen = math.sqrt(dx * dx + dy * dy + dz * dz)
                    if dlen > 0.5 then
                        local los = physics.raycast(ox, oy, oz, dx / dlen, dy / dlen, dz / dlen, dlen - 0.35)
                        has_los = not los.hit
                    end
                end

                local speed = 0.0
                local desired_clip = "Idle"
                if anim_clip == "HitReact" and anim_time < 0.32 then
                    desired_clip = "HitReact"
                    anim_loop = false
                elseif has_los then
                    local target_yaw = yaw_between(bx, bz, best_tx, best_tz)
                    yaw = angle_lerp(yaw, target_yaw, 1.0 - math.exp(-dt * 10.0))
                    local dy = (best_ty - (by + 1.42))
                    aim_pitch = math.max(-0.4, math.min(0.4, math.atan(dy, math.max(best_dist, 1.0))))
                    local strafe_sign = math.sin(match_time * 1.4 + b.slot_index * 1.9) >= 0.0 and 1.0 or -1.0
                    local sx = math.cos(yaw) * strafe_sign * 1.85 * dt
                    local sz = -math.sin(yaw) * strafe_sign * 1.85 * dt
                    local sw = physics.sweep_capsule(bx, by + 0.88, bz, sx, 0.0, sz, 0.30, 0.56)
                    if not sw.hit then
                        bx = bx + sx
                        bz = bz + sz
                        by = snap_to_ground(bx, by, bz)
                        speed = 1.85
                    end
                    if shoot_cd <= 0.25 then
                        desired_clip = speed > 0.5 and "Walk_Shoot" or "Idle_Shoot"
                    else
                        desired_clip = speed > 0.5 and "Walk" or "Duck"
                    end
                    if shoot_cd <= 0.0 then
                        shoot_cd = 0.75 + ((b.slot_index * 17) % 10) * 0.08
                    end
                else
                    if wp_count >= 1 then
                        if wp_idx < 1 or wp_idx > wp_count then wp_idx = 1 end
                        local wp = wps[wp_idx]
                        local dx = wp.x - bx
                        local dz = wp.z - bz
                        local dist = math.sqrt(dx * dx + dz * dz)
                        if dist < 1.8 then
                            wp_idx = (wp_idx % wp_count) + 1
                        else
                            local target_yaw = yaw_between(bx, bz, wp.x, wp.z)
                            yaw = angle_lerp(yaw, target_yaw, 1.0 - math.exp(-dt * 7.5))
                            local step = 2.65 * dt
                            local mx = (dx / dist) * step
                            local mz = (dz / dist) * step
                            local sw = physics.sweep_capsule(bx, by + 0.88, bz, mx, 0.0, mz, 0.30, 0.56)
                            if not sw.hit then
                                bx = bx + mx
                                bz = bz + mz
                                by = snap_to_ground(bx, by, bz)
                                speed = 2.65
                                desired_clip = "Run_Gun"
                            else
                                wp_idx = (wp_idx % wp_count) + 1
                                desired_clip = "Idle"
                            end
                        end
                    end
                end

                if desired_clip ~= anim_clip then
                    anim_clip = desired_clip
                    anim_time = 0.0
                    anim_loop = (desired_clip ~= "HitReact")
                end

                ecs.set_bot(bid, {
                    x = bx,
                    y = by,
                    z = bz,
                    yaw = yaw,
                    aim_pitch = aim_pitch,
                    anim_clip = anim_clip,
                    anim_time = anim_time,
                    anim_speed = 1.0,
                    anim_loop = anim_loop,
                    hit_flash = hit_flash,
                    shoot_cooldown = shoot_cd,
                    waypoint_index = wp_idx,
                })
            end
        end
    end
end

return M
