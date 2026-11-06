--[[--
Launches the Upsilon calculator from KOReader.

Upsilon needs the screen and the touch panel to itself, so while it runs we
release KOReader's (exclusively grabbed) input devices, then take them back
and repaint everything once it exits.

@module koplugin.Upsilon
--]]

local Device = require("device")
local Dispatcher = require("dispatcher")
local InfoMessage = require("ui/widget/infomessage")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local _ = require("gettext")
local T = require("ffi/util").template

local UPSILON_DIR = "/mnt/us/upsilon"

local Upsilon = WidgetContainer:extend{
    name = "upsilon",
    is_doc_only = false,
}

function Upsilon:onDispatcherRegisterActions()
    Dispatcher:registerAction("upsilon_launch", {
        category = "none",
        event = "LaunchUpsilon",
        title = _("Upsilon calculator"),
        general = true,
    })
end

function Upsilon:init()
    self:onDispatcherRegisterActions()
    self.ui.menu:registerToMainMenu(self)
end

function Upsilon:addToMainMenu(menu_items)
    menu_items.upsilon = {
        text = _("Upsilon calculator"),
        sorting_hint = "more_tools",
        sub_item_table = {
            {
                text = _("Launch"),
                callback = function()
                    self:onLaunchUpsilon()
                end,
            },
            {
                text = _("Launch with debug log"),
                callback = function()
                    self:onLaunchUpsilon(true)
                end,
            },
        },
    }
end

function Upsilon:onLaunchUpsilon(debug)
    if not Device:isKindle() then
        UIManager:show(InfoMessage:new{ text = _("Upsilon for e-readers only runs on Kindle.") })
        return true
    end
    if lfs.attributes(UPSILON_DIR .. "/bin/upsilon", "mode") ~= "file" then
        UIManager:show(InfoMessage:new{
            text = T(_("Upsilon is not installed.\n\nCopy the \"upsilon\" folder from the package to the root of the Kindle, so that %1 exists."),
                UPSILON_DIR .. "/bin/upsilon"),
        })
        return true
    end
    -- Let the menu close before handing over the screen
    UIManager:scheduleIn(0.5, function() self:run(debug == true) end)
    return true
end

function Upsilon:run(debug)
    local input = Device.input
    -- KOReader grabs its input devices: give them up so Upsilon receives
    -- the touches. Keep the fake events pipe and rotation (gyro) devices.
    local released = {}
    local rotation_fds = input.rotation_fds or {}
    for path, fd in pairs(input.opened_devices or {}) do
        if path ~= "fake_events" and not rotation_fds[fd] then
            table.insert(released, path)
        end
    end
    for _i, path in ipairs(released) do
        input:close(path)
    end

    logger.info("Upsilon: starting, released input devices:", table.concat(released, ", "))
    local launcher = UPSILON_DIR .. "/upsilon.sh"
    local a, _reason, c = os.execute(string.format("sh '%s' --koreader%s", launcher, debug and " --debug" or ""))
    -- Lua 5.1 returns the raw wait status, Lua 5.2 style (true|nil, "exit", code)
    local code = c
    if code == nil and type(a) == "number" then
        code = a >= 256 and math.floor(a / 256) or a
    end
    logger.info("Upsilon: exited with", code)

    for _i, path in ipairs(released) do
        local ok, err = pcall(input.open, input, path)
        if not ok then
            logger.warn("Upsilon: could not reopen input device", path, err)
        end
    end
    UIManager:setDirty("all", "full")
    if code == 75 then
        UIManager:show(InfoMessage:new{
            text = _("Upsilon could not use the touchscreen: KOReader was still busy with it (for example extracting book covers). Please try again in a moment."),
        })
    end
end

return Upsilon
