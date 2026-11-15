-- DCS Instant Cam - telemetry for photo captions
-- Sends your own aircraft's type, position, altitude, heading and speed to the
-- Instant Cam layer on 127.0.0.1 twice a second. Chains any existing Export.lua hooks.
-- If a server disables "Allow player export", captions just show the date and time.

local IC_PORT = 47811

package.path  = package.path  .. ";.\\LuaSocket\\?.lua"
package.cpath = package.cpath .. ";.\\LuaSocket\\?.dll"

local ok_socket, socket = pcall(require, "socket")
if not ok_socket then return end

local ic = { udp = nil, nextSend = 0 }

local function ic_start()
    ic.udp = socket.udp()
    if ic.udp then ic.udp:settimeout(0) end
end

local function ic_frame()
    if not ic.udp then return end
    local t = LoGetModelTime and LoGetModelTime() or 0
    if t < ic.nextSend then return end
    ic.nextSend = t + 0.5
    local me = LoGetSelfData and LoGetSelfData()
    if not me or not me.LatLongAlt then return end
    local start = LoGetMissionStartTime and LoGetMissionStartTime() or -1
    local mtime = (start >= 0) and (start + t) or -1
    local ias = LoGetIndicatedAirSpeed and LoGetIndicatedAirSpeed() or 0
    local pilot = LoGetPilotName and LoGetPilotName() or ""
    pilot = string.gsub(tostring(pilot or ""), "|", "/")
    local msg = string.format("IC1|%s|%.6f|%.6f|%.1f|%.1f|%.1f|%.0f|%s",
        tostring(me.Name or ""), me.LatLongAlt.Lat or 0, me.LatLongAlt.Long or 0,
        me.LatLongAlt.Alt or 0, math.deg(me.Heading or 0), ias or 0, mtime, pilot)
    ic.udp:sendto(msg, "127.0.0.1", IC_PORT)
end

local function ic_stop()
    if ic.udp then ic.udp:close() end
    ic.udp = nil
end

do
    local prevStart, prevAfter, prevStop = LuaExportStart, LuaExportAfterNextFrame, LuaExportStop
    LuaExportStart = function()
        pcall(ic_start)
        if prevStart then prevStart() end
    end
    LuaExportAfterNextFrame = function()
        pcall(ic_frame)
        if prevAfter then prevAfter() end
    end
    LuaExportStop = function()
        pcall(ic_stop)
        if prevStop then prevStop() end
    end
end
