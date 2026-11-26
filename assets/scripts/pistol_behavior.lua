local M = {}

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
    events.emit("weapon.reload_started", { entity = entity_id })
    return true
end

function M.on_init(entity_id)
    events.on("weapon.ads_changed", function(payload)
        ecs.set_weapon(entity_id, {
            ads_requested = payload.active == true,
        })
    end)

    events.on("weapon.reload_requested", function()
        local w = ecs.get_weapon(entity_id)
        if w ~= nil then
            begin_reload(entity_id, w)
        end
    end)

    events.on("weapon.fire_requested", function()
        local w = ecs.get_weapon(entity_id)
        if w == nil or w.reloading or w.fire_cooldown > 0.0 then
            return
        end
        if w.ammo_in_mag <= 0 then
            begin_reload(entity_id, w)
            return
        end

        local shots = w.shots_fired + 1
        local dir_sign = ((shots % 2) == 1) and 1.0 or -1.0
        local ads = w.ads_alpha
        local next_kick = math.min(w.recoil_kick + 0.85, 1.25)
        local next_pitch = math.min(w.recoil_pitch + (0.024 - 0.008 * ads), 0.075)
        local next_yaw = w.recoil_yaw + dir_sign * (0.0045 * (1.0 - 0.45 * ads))
        local next_bloom = math.min(w.crosshair_bloom + 0.72, 1.35)
        local spread = (1.0 - ads) * (0.0035 + next_bloom * 0.005)

        ecs.set_weapon(entity_id, {
            ammo_in_mag = w.ammo_in_mag - 1,
            shots_fired = shots,
            fire_cooldown = w.fire_interval,
            slide_blowback = 1.0,
            trigger_pull = 1.0,
            hammer_cock = 1.0,
            recoil_kick = next_kick,
            recoil_pitch = next_pitch,
            recoil_yaw = next_yaw,
            recoil_roll = dir_sign * 0.85,
            crosshair_bloom = next_bloom,
            muzzle_flash_timer = 0.048,
            muzzle_flash_spin = shots * 1.17,
        })

        events.emit("shell.eject_requested", {
            entity = entity_id,
            shot = shots,
        })
        events.emit("ballistics.raycast_requested", {
            entity = entity_id,
            shot = shots,
            spread = spread,
        })
        events.emit("weapon.fired", {
            entity = entity_id,
            ammo_in_mag = w.ammo_in_mag - 1,
            shots_fired = shots,
        })
    end)

    events.on("range.reset", function()
        local w = ecs.get_weapon(entity_id)
        if w == nil then
            return
        end
        ecs.set_weapon(entity_id, {
            ammo_in_mag = w.mag_size,
            ammo_reserve = 48,
            shots_fired = 0,
            fire_cooldown = 0.0,
            ads_alpha = 0.0,
            reloading = false,
            reload_timer = 0.0,
            slide_blowback = 0.0,
            trigger_pull = 0.0,
            hammer_cock = 0.0,
            recoil_kick = 0.0,
            recoil_pitch = 0.0,
            recoil_yaw = 0.0,
            recoil_roll = 0.0,
            crosshair_bloom = 0.0,
            muzzle_flash_timer = 0.0,
        })
    end)
end

function M.on_update(entity_id, dt)
    local w = ecs.get_weapon(entity_id)
    if w == nil then
        return
    end

    local ads_target = (w.ads_requested and not w.reloading) and 1.0 or 0.0
    local ads_alpha = w.ads_alpha + (ads_target - w.ads_alpha) * (1.0 - math.exp(-dt * 18.0))
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
                ammo_reserve = 48
            end
            reloading = false
            reload_timer = 0.0
            slide_blowback = 0.0
            completed_reload = true
        end
    end

    local slide_target = (ammo_in_mag == 0 and not reloading) and 0.92 or 0.0
    slide_blowback = slide_blowback + (slide_target - slide_blowback) * (1.0 - math.exp(-dt * 28.0))

    ecs.set_weapon(entity_id, {
        ads_alpha = ads_alpha,
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
        crosshair_bloom = w.crosshair_bloom * math.exp(-dt * 9.0),
    })

    if completed_reload then
        events.emit("weapon.reload_completed", {
            entity = entity_id,
            ammo_in_mag = ammo_in_mag,
            ammo_reserve = ammo_reserve,
        })
    end
end

return M
