--[[--
Launches the Upsilon calculator from KOReader.

KOReader keeps its input devices open with an exclusive grab (EVIOCGRAB),
so nobody else receives touches. While Upsilon runs, we lift those grabs
(leaving the devices open, so KOReader's own bookkeeping is untouched) and
put them back once it exits.

@module koplugin.Upsilon
--]]

local Device = require("device")
local Dispatcher = require("dispatcher")
local InfoMessage = require("ui/widget/infomessage")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local ffi = require("ffi")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local _ = require("gettext")
local T = require("ffi/util").template

require("ffi/posix_h") -- read, ioctl
pcall(ffi.cdef, "ssize_t readlink(const char *, char *, size_t);")
local C = ffi.C

local UPSILON_DIR = "/mnt/us/upsilon"
local EVIOCGRAB = 0x40044590 -- _IOW('E', 0x90, int)
local MAX_FD = 1024

local Upsilon = WidgetContainer:extend{
    name = "upsilon",
    is_doc_only = false,
    -- Overridable for tests
    input_device_pattern = "^/dev/input/event%d+$",
}

--- Lists our own file descriptors that point to input event devices.
function Upsilon:inputDeviceFds()
    local found = {}
    local buffer = ffi.new("char[256]")
    for fd = 0, MAX_FD - 1 do
        local n = C.readlink("/proc/self/fd/" .. fd, buffer, 255)
        if n > 0 then
            local target = ffi.string(buffer, n)
            if target:match(self.input_device_pattern) then
                table.insert(found, { fd = fd, path = target })
            end
        end
    end
    return found
end

--- Lifts KOReader's grabs. Returns the descriptors that were grabbed.
function Upsilon:releaseGrabs()
    local released = {}
    for _i, dev in ipairs(self:inputDeviceFds()) do
        -- Ungrabbing fails (EINVAL) when this descriptor did not hold the grab
        if C.ioctl(dev.fd, EVIOCGRAB, ffi.cast("int", 0)) == 0 then
            table.insert(released, dev)
            logger.info("Upsilon: released grab on fd", dev.fd, dev.path)
        else
            logger.dbg("Upsilon: fd", dev.fd, dev.path, "was not grabbed")
        end
    end
    return released
end

function Upsilon:restoreGrabs(released)
    local buffer = ffi.new("char[1024]")
    for _i, dev in ipairs(released) do
        if C.ioctl(dev.fd, EVIOCGRAB, ffi.cast("int", 1)) ~= 0 then
            logger.warn("Upsilon: could not grab fd", dev.fd, dev.path, "again:", ffi.string(C.strerror(ffi.errno())))
        end
        -- Drop touches that reached us between Upsilon's exit and the grab
        -- (e.g. lifting the finger from Upsilon's exit key). The descriptor
        -- is non-blocking; stop at a sane limit just in case.
        for _j = 1, 64 do
            if C.read(dev.fd, buffer, 1024) <= 0 then
                break
            end
        end
    end
end

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
    local released = self:releaseGrabs()
    logger.info("Upsilon: starting, released", #released, "input grab(s)")

    local launcher = UPSILON_DIR .. "/upsilon.sh"
    local a, _reason, c = os.execute(string.format("sh '%s' --koreader%s", launcher, debug and " --debug" or ""))
    -- Lua 5.1 returns the raw wait status, Lua 5.2 style (true|nil, "exit", code)
    local code = c
    if code == nil and type(a) == "number" then
        code = a >= 256 and math.floor(a / 256) or a
    end
    logger.info("Upsilon: exited with", code)

    self:restoreGrabs(released)
    UIManager:setDirty("all", "full")
    if code == 75 then
        UIManager:show(InfoMessage:new{
            text = _("Upsilon could not use the touchscreen: another program kept it. Restarting KOReader should help; please send upsilon/upsilon.log if it keeps happening."),
        })
    end
end

return Upsilon
