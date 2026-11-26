local M = {}

function M.on_init(entity_id)
    events.on("target.hit", function(payload)
        if payload.entity ~= entity_id then
            return
        end
        local t = ecs.get_target(entity_id)
        if t == nil then
            return
        end

        local local_h = (payload.hit_y or 0.0) - t.base_y
        local is_headshot = (not t.knocked_down) and (local_h >= t.head_height_threshold)
        local pts = is_headshot and t.headshot_points or t.body_points
        local impulse_mag = is_headshot and t.headshot_impulse or t.body_impulse

        hud.hit_marker(is_headshot)
        hud.add_score(pts, is_headshot)

        ecs.set_target(entity_id, {
            hit_flash = 1.0,
            knocked_down = true,
            reset_timer = t.reset_delay,
        })

        physics.set_target_upright(entity_id, false)
        physics.apply_impulse(
            entity_id,
            (payload.dir_x or 0.0) * impulse_mag,
            (payload.dir_y or 0.0) * impulse_mag * 0.25,
            (payload.dir_z or 1.0) * impulse_mag,
            payload.hit_x or t.base_x,
            payload.hit_y or (t.base_y + 0.72),
            payload.hit_z or t.base_z
        )

        events.emit("target.knocked_down", {
            entity = entity_id,
            slot_index = t.slot_index,
            headshot = is_headshot,
            points = pts,
        })
    end)

    events.on("target.force_reset", function(payload)
        if payload.entity ~= nil and payload.entity ~= entity_id then
            return
        end
        local t = ecs.get_target(entity_id)
        if t == nil then
            return
        end
        ecs.set_target(entity_id, {
            knocked_down = false,
            reset_timer = 0.0,
            hit_flash = 0.0,
        })
        physics.set_target_upright(entity_id, true)
        events.emit("target.reset", { entity = entity_id, slot_index = t.slot_index })
    end)

    events.on("range.reset", function()
        local t = ecs.get_target(entity_id)
        if t == nil then
            return
        end
        ecs.set_target(entity_id, {
            knocked_down = false,
            reset_timer = 0.0,
            hit_flash = 0.0,
        })
        physics.reset_target_pose(entity_id)
    end)
end

function M.on_update(entity_id, dt)
    local t = ecs.get_target(entity_id)
    if t == nil then
        return
    end
    local changed = false
    local hit_flash = t.hit_flash
    local knocked_down = t.knocked_down
    local reset_timer = t.reset_timer
    local popped_up = false

    if hit_flash > 0.0 then
        hit_flash = math.max(0.0, hit_flash - dt * 4.5)
        changed = true
    end
    if knocked_down then
        reset_timer = reset_timer - dt
        changed = true
        if reset_timer <= 0.0 then
            knocked_down = false
            reset_timer = 0.0
            popped_up = true
        end
    end

    if changed then
        ecs.set_target(entity_id, {
            hit_flash = hit_flash,
            knocked_down = knocked_down,
            reset_timer = reset_timer,
        })
    end

    if popped_up then
        physics.set_target_upright(entity_id, true)
        events.emit("target.reset", {
            entity = entity_id,
            slot_index = t.slot_index,
        })
    end
end

return M
