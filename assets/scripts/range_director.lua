local M = {}

local streak = 0

function M.on_init(entity_id)
    events.on("target.knocked_down", function()
        streak = streak + 1
        local targets = ecs.find_by_tag("target")
        local up_count = 0
        for _, tid in ipairs(targets) do
            local t = ecs.get_target(tid)
            if t ~= nil and not t.knocked_down then
                up_count = up_count + 1
            end
        end
        if #targets > 0 and up_count == 0 then
            hud.add_score(500, true)
            events.emit("range.rack_cleared", {
                director = entity_id,
                streak = streak,
                bonus = 500,
            })
            for _, tid in ipairs(targets) do
                ecs.set_target(tid, { reset_timer = 0.85 })
            end
        end
    end)

    events.on("range.reset", function()
        streak = 0
        hud.reset_score()
    end)
end

function M.on_update(entity_id, dt)
end

return M
