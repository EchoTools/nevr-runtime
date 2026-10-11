--[[nevr
{
  "name": "low_gravity",
  "version": "1.0.0",
  "api": 1,
  "description": "Halves gravity and adds one to every test.add result",
  "overrides": ["physics.gravity"],
  "hooks": ["test.add"]
}
]]
--!strict

local ok, err = nevr.override("physics.gravity", -4.9)
if not ok then
  nevr.log("warn", "gravity not applied: " .. tostring(err))
end

nevr.hook("test.add", {
  pre = function(h)
    h:set("a", h:get("a") * 2)
  end,
  post = function(h)
    h:set("result", h:get("result") + 1)
  end,
})
