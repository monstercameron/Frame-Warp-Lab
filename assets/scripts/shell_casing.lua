local M = {}

local next_slot = 0
local casing_entities = {}
local registered_listener = false

function M.on_init(entity_id)
    local c = ecs.get_casing(entity_id)
    if c ~= nil then
        casing_entities[c.slot_index + 1] = entity_id
    end
    if registered_listener then
        return
    end
    registered_listener = true

    events.on("shell.eject_spawn", function(payload)
        local count = #casing_entities
        if count == 0 then
            return
        end
        local slot = (next_slot % count) + 1
        next_slot = next_slot + 1
        local cid = casing_entities[slot]
        if cid == nil then
            return
        end
        ecs.set_casing(cid, {
            active = true,
            age = 0.0,
        })
        ecs.set_visible(cid, true)
        local shot = payload.shot or next_slot
        local jitter = math.sin(shot * 3.7) * 0.25
        physics.launch_casing(
            cid,
            payload.px or 0.0,
            payload.py or 1.4,
            payload.pz or 2.0,
            (payload.vx or 2.4) + jitter,
            (payload.vy or 1.8) + math.abs(jitter) * 0.5,
            (payload.vz or -0.4),
            18.0 + jitter * 12.0,
            11.0,
            -24.0
        )
    end)

    events.on("range.reset", function()
        next_slot = 0
        for _, cid in ipairs(casing_entities) do
            ecs.set_casing(cid, { active = false, age = 0.0 })
            ecs.set_visible(cid, false)
            physics.deactivate_casing(cid)
        end
    end)
end

function M.on_update(entity_id, dt)
    local c = ecs.get_casing(entity_id)
    if c == nil or not c.active then
        return
    end
    local next_age = c.age + dt
    if next_age > c.lifetime then
        ecs.set_casing(entity_id, {
            active = false,
            age = 0.0,
        })
        ecs.set_visible(entity_id, false)
        physics.deactivate_casing(entity_id)
    else
        ecs.set_casing(entity_id, {
            age = next_age,
        })
    end
end

return M
