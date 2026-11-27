local M = {}

local WEAPONS = {
    [0] = {
        name = "USP",
        mag_size = 12,
        reserve_max = 48,
        fire_interval = 0.145,
        reload_duration = 1.25,
        full_auto = false,
        pellet_count = 1,
        base_damage = 34.0,
        hip_spread = 0.0110,
        ads_spread = 0.0012,
        move_spread = 0.0065,
        heat_add = 0.30,
        heat_decay = 4.2,
        recoil_kick = 0.78,
        recoil_pitch = 0.022,
        recoil_yaw = 0.0045,
        ads_fov = 0.88,
        scoped = false,
    },
    [1] = {
        name = "MP5",
        mag_size = 30,
        reserve_max = 120,
        fire_interval = 0.080,
        reload_duration = 1.65,
        full_auto = true,
        pellet_count = 1,
        base_damage = 26.0,
        hip_spread = 0.0155,
        ads_spread = 0.0042,
        move_spread = 0.0048,
        heat_add = 0.14,
        heat_decay = 4.6,
        recoil_kick = 0.42,
        recoil_pitch = 0.014,
        recoil_yaw = 0.0052,
        ads_fov = 0.82,
        scoped = false,
    },
    [2] = {
        name = "AK47",
        mag_size = 30,
        reserve_max = 90,
        fire_interval = 0.100,
        reload_duration = 1.85,
        full_auto = true,
        pellet_count = 1,
        base_damage = 38.0,
        hip_spread = 0.0140,
        ads_spread = 0.0022,
        move_spread = 0.0095,
        heat_add = 0.20,
        heat_decay = 3.5,
        recoil_kick = 0.82,
        recoil_pitch = 0.029,
        recoil_yaw = 0.0085,
        ads_fov = 0.78,
        scoped = false,
    },
    [3] = {
        name = "SCOUT",
        mag_size = 10,
        reserve_max = 40,
        fire_interval = 0.920,
        reload_duration = 2.05,
        full_auto = false,
        pellet_count = 1,
        base_damage = 78.0,
        hip_spread = 0.0320,
        ads_spread = 0.0003,
        move_spread = 0.0110,
        heat_add = 0.45,
        heat_decay = 2.4,
        recoil_kick = 1.35,
        recoil_pitch = 0.056,
        recoil_yaw = 0.0040,
        ads_fov = 0.40,
        scoped = true,
    },
    [4] = {
        name = "SHOTGUN",
        mag_size = 8,
        reserve_max = 32,
        fire_interval = 0.740,
        reload_duration = 2.10,
        full_auto = false,
        pellet_count = 8,
        base_damage = 22.0,
        hip_spread = 0.0520,
        ads_spread = 0.0340,
        move_spread = 0.0060,
        heat_add = 0.42,
        heat_decay = 3.2,
        recoil_kick = 1.40,
        recoil_pitch = 0.052,
        recoil_yaw = 0.0075,
        ads_fov = 0.92,
        scoped = false,
    },
    [5] = {
        name = "DUALS",
        mag_size = 14,
        reserve_max = 56,
        fire_interval = 0.115,
        reload_duration = 1.75,
        full_auto = false,
        pellet_count = 1,
        base_damage = 40.0,
        hip_spread = 0.0165,
        ads_spread = 0.0060,
        move_spread = 0.0055,
        heat_add = 0.22,
        heat_decay = 4.0,
        recoil_kick = 0.72,
        recoil_pitch = 0.020,
        recoil_yaw = 0.0065,
        ads_fov = 0.96,
        scoped = false,
    },
}

local ammo_store = {}
for wid, spec in pairs(WEAPONS) do
    ammo_store[wid] = { mag = spec.mag_size, res = spec.reserve_max }
end

local function next_lcg(state)
    local next_state = (state * 1103515245 + 12345) % 2147483648
    local u = (next_state % 16777216) / 16777216.0
    return next_state, math.max(0.0001, math.min(0.9999, u))
end

local function get_spec(wid)
    return WEAPONS[wid] or WEAPONS[0]
end

local function compute_spread_radians(spec, ads_alpha, move_speed, spread_heat)
    local base = spec.hip_spread * (1.0 - ads_alpha) + spec.ads_spread * ads_alpha
    local move_factor = math.min(move_speed, 5.5) * (spec.move_spread * (1.0 - 0.55 * ads_alpha))
    local heat_factor = spread_heat * (0.0280 * (1.0 - 0.45 * ads_alpha))
    return base + move_factor + heat_factor
end

local function begin_reload(entity_id, w)
    if w.reloading then
        return false
    end
    if w.ammo_in_mag >= w.mag_size or w.ammo_reserve <= 0 then
        return false
    end
    ecs.set_weapon(entity_id, {
        reloading = true,
        reload_timer = w.reload_duration,
    })
    events.emit("weapon.reload_started", { entity = entity_id, weapon_id = w.weapon_id })
    return true
end

local function equip_weapon(entity_id, new_wid)
    local w = ecs.get_weapon(entity_id)
    if w == nil then
        return
    end
    local wid = math.floor(new_wid) % 6
    if wid < 0 then wid = wid + 6 end
    if ammo_store[w.weapon_id] ~= nil then
        ammo_store[w.weapon_id].mag = w.ammo_in_mag
        ammo_store[w.weapon_id].res = w.ammo_reserve
    end
    local spec = get_spec(wid)
    local saved = ammo_store[wid] or { mag = spec.mag_size, res = spec.reserve_max }
    ammo_store[wid] = saved

    ecs.set_weapon(entity_id, {
        weapon_id = wid,
        ammo_in_mag = saved.mag,
        mag_size = spec.mag_size,
        ammo_reserve = saved.res,
        fire_interval = spec.fire_interval,
        reload_duration = spec.reload_duration,
        full_auto = spec.full_auto,
        pellet_count = spec.pellet_count,
        base_damage = spec.base_damage,
        ads_fov_rad = spec.ads_fov,
        scope_alpha = 0.0,
        fire_cooldown = 0.16,
        reloading = false,
        reload_timer = 0.0,
        ads_alpha = 0.0,
        recoil_kick = 0.25,
        recoil_pitch = 0.0,
        recoil_yaw = 0.0,
        recoil_roll = 0.0,
        dual_left_kick = 0.0,
        dual_right_kick = 0.0,
        spread_heat = 0.0,
        current_spread_rad = spec.hip_spread,
    })
    events.emit("weapon.switched", { entity = entity_id, weapon_id = wid, name = spec.name })
end

local function fire_once(entity_id, w)
    if w == nil or w.reloading or w.fire_cooldown > 0.0 then
        return
    end
    if w.ammo_in_mag <= 0 then
        begin_reload(entity_id, w)
        return
    end

    local spec = get_spec(w.weapon_id)
    local shots = w.shots_fired + 1
    local dir_sign = ((shots % 2) == 1) and 1.0 or -1.0
    local ads = w.ads_alpha
    local spread_rad = compute_spread_radians(spec, ads, w.move_speed, w.spread_heat)

    local rng = w.spread_rng_state
    local first_offset = 0.0
    local pellets = math.max(1, spec.pellet_count)
    local dual_side = w.dual_hand_toggle
    local next_dual_side = (dual_side == 0) and 1 or 0
    local left_kick = w.dual_left_kick
    local right_kick = w.dual_right_kick
    if w.weapon_id == 5 then
        if dual_side == 0 then
            right_kick = math.min(right_kick + 1.0, 1.35)
        else
            left_kick = math.min(left_kick + 1.0, 1.35)
        end
    end

    for p = 1, pellets do
        local u1, u2
        rng, u1 = next_lcg(rng)
        rng, u2 = next_lcg(rng)
        local gauss_r = math.min(1.0, math.sqrt(-2.0 * math.log(u1)) * 0.44)
        local theta = 6.28318530718 * u2
        local offset_r = gauss_r * spread_rad
        if p == 1 then
            first_offset = offset_r
        end
        local spread_x = math.cos(theta) * offset_r
        local spread_y = math.sin(theta) * offset_r
        events.emit("ballistics.raycast_requested", {
            entity = entity_id,
            shot = shots,
            pellet = p,
            weapon_id = w.weapon_id,
            damage = spec.base_damage,
            spread_x = spread_x,
            spread_y = spread_y,
            spread_rad = spread_rad,
        })
    end

    local next_heat = math.min(1.0, w.spread_heat + spec.heat_add * (1.0 - 0.25 * ads))
    local next_kick = math.min(w.recoil_kick + spec.recoil_kick, 1.45)
    local next_pitch = math.min(w.recoil_pitch + spec.recoil_pitch * (1.0 - 0.35 * ads), 0.11)
    local next_yaw = w.recoil_yaw + dir_sign * (spec.recoil_yaw * (1.0 - 0.45 * ads))
    local next_bloom = math.min(w.crosshair_bloom + 0.65, 1.45)
    local next_spread = compute_spread_radians(spec, ads, w.move_speed, next_heat)

    local new_mag = w.ammo_in_mag - 1
    if ammo_store[w.weapon_id] ~= nil then
        ammo_store[w.weapon_id].mag = new_mag
    end

    ecs.set_weapon(entity_id, {
        ammo_in_mag = new_mag,
        shots_fired = shots,
        fire_cooldown = spec.fire_interval,
        slide_blowback = 1.0,
        trigger_pull = 1.0,
        hammer_cock = 1.0,
        recoil_kick = next_kick,
        recoil_pitch = next_pitch,
        recoil_yaw = next_yaw,
        recoil_roll = dir_sign * 0.85,
        dual_left_kick = left_kick,
        dual_right_kick = right_kick,
        dual_hand_toggle = next_dual_side,
        crosshair_bloom = next_bloom,
        muzzle_flash_timer = 0.048,
        muzzle_flash_spin = shots * 1.17,
        spread_heat = next_heat,
        current_spread_rad = next_spread,
        last_shot_offset_rad = first_offset,
        spread_rng_state = rng,
    })

    events.emit("shell.eject_requested", {
        entity = entity_id,
        shot = shots,
        weapon_id = w.weapon_id,
        dual_side = dual_side,
    })
    events.emit("weapon.fired", {
        entity = entity_id,
        weapon_id = w.weapon_id,
        ammo_in_mag = new_mag,
        shots_fired = shots,
        spread_rad = spread_rad,
        offset_rad = first_offset,
    })
end

function M.on_init(entity_id)
    events.on("weapon.ads_changed", function(payload)
        ecs.set_weapon(entity_id, {
            ads_requested = payload.active == true,
        })
    end)

    events.on("weapon.switch_requested", function(payload)
        local wid = payload.weapon_id
        if wid ~= nil then
            equip_weapon(entity_id, wid)
        end
    end)

    events.on("weapon.cycle_requested", function(payload)
        local w = ecs.get_weapon(entity_id)
        if w ~= nil then
            local delta = payload.delta or 1
            equip_weapon(entity_id, w.weapon_id + delta)
        end
    end)

    events.on("weapon.reload_requested", function()
        local w = ecs.get_weapon(entity_id)
        if w ~= nil then
            begin_reload(entity_id, w)
        end
    end)

    events.on("weapon.fire_requested", function()
        local w = ecs.get_weapon(entity_id)
        fire_once(entity_id, w)
    end)

    events.on("range.reset", function()
        for wid, spec in pairs(WEAPONS) do
            ammo_store[wid] = { mag = spec.mag_size, res = spec.reserve_max }
        end
        local w = ecs.get_weapon(entity_id)
        local cur_wid = w ~= nil and w.weapon_id or 0
        equip_weapon(entity_id, cur_wid)
        ecs.set_weapon(entity_id, {
            shots_fired = 0,
            fire_cooldown = 0.0,
            recoil_kick = 0.0,
            crosshair_bloom = 0.0,
            muzzle_flash_timer = 0.0,
            last_shot_offset_rad = 0.0,
            spread_rng_state = 1234567,
        })
    end)
end

function M.on_update(entity_id, dt)
    local w = ecs.get_weapon(entity_id)
    if w == nil then
        return
    end

    local spec = get_spec(w.weapon_id)
    local ads_target = (w.ads_requested and not w.reloading) and 1.0 or 0.0
    local ads_alpha = w.ads_alpha + (ads_target - w.ads_alpha) * (1.0 - math.exp(-dt * 16.0))
    local scope_target = (spec.scoped and ads_alpha > 0.72 and not w.reloading) and 1.0 or 0.0
    local scope_alpha = w.scope_alpha + (scope_target - w.scope_alpha) * (1.0 - math.exp(-dt * 22.0))
    local fire_cooldown = math.max(0.0, w.fire_cooldown - dt)
    local muzzle_flash_timer = math.max(0.0, w.muzzle_flash_timer - dt)

    local reloading = w.reloading
    local reload_timer = w.reload_timer
    local ammo_in_mag = w.ammo_in_mag
    local ammo_reserve = w.ammo_reserve
    local slide_blowback = w.slide_blowback
    local completed_reload = false

    if reloading then
        reload_timer = reload_timer - dt
        if reload_timer <= 0.0 then
            local needed = w.mag_size - ammo_in_mag
            local loaded = math.min(needed, ammo_reserve)
            ammo_in_mag = ammo_in_mag + loaded
            ammo_reserve = ammo_reserve - loaded
            if ammo_reserve < w.mag_size then
                ammo_reserve = spec.reserve_max
            end
            if ammo_store[w.weapon_id] ~= nil then
                ammo_store[w.weapon_id].mag = ammo_in_mag
                ammo_store[w.weapon_id].res = ammo_reserve
            end
            reloading = false
            reload_timer = 0.0
            slide_blowback = 0.0
            completed_reload = true
        end
    end

    local slide_target = (ammo_in_mag == 0 and not reloading) and 0.92 or 0.0
    slide_blowback = slide_blowback + (slide_target - slide_blowback) * (1.0 - math.exp(-dt * 28.0))
    local next_heat = w.spread_heat * math.exp(-dt * spec.heat_decay)
    if next_heat < 0.001 then
        next_heat = 0.0
    end
    local next_spread = compute_spread_radians(spec, ads_alpha, w.move_speed, next_heat)

    ecs.set_weapon(entity_id, {
        ads_alpha = ads_alpha,
        scope_alpha = scope_alpha,
        fire_cooldown = fire_cooldown,
        muzzle_flash_timer = muzzle_flash_timer,
        reloading = reloading,
        reload_timer = reload_timer,
        ammo_in_mag = ammo_in_mag,
        ammo_reserve = ammo_reserve,
        slide_blowback = slide_blowback,
        trigger_pull = w.trigger_pull * math.exp(-dt * 24.0),
        hammer_cock = w.hammer_cock * math.exp(-dt * 18.0),
        recoil_kick = w.recoil_kick * math.exp(-dt * 15.0),
        recoil_pitch = w.recoil_pitch * math.exp(-dt * 11.0),
        recoil_yaw = w.recoil_yaw * math.exp(-dt * 11.0),
        recoil_roll = w.recoil_roll * math.exp(-dt * 14.0),
        dual_left_kick = w.dual_left_kick * math.exp(-dt * 16.0),
        dual_right_kick = w.dual_right_kick * math.exp(-dt * 16.0),
        crosshair_bloom = w.crosshair_bloom * math.exp(-dt * 9.0),
        spread_heat = next_heat,
        current_spread_rad = next_spread,
    })

    if completed_reload then
        events.emit("weapon.reload_completed", {
            entity = entity_id,
            weapon_id = w.weapon_id,
            ammo_in_mag = ammo_in_mag,
            ammo_reserve = ammo_reserve,
        })
    end

    if spec.full_auto and w.firing_held and not reloading and fire_cooldown <= 0.0 then
        local fresh = ecs.get_weapon(entity_id)
        fire_once(entity_id, fresh)
    end
end

return M
