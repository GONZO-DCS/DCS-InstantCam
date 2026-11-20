-- DCS Instant Cam: loads the camera into DCS (flat screen and VR)
-- and writes the mission date and nearby town names for photo captions.
local INSTDIR = [[%INSTDIR%]]

local ok, err = pcall(function()
    package.cpath = package.cpath .. ";" .. INSTDIR .. "\\?.dll"
    require("instantcam")
end)
if not ok and log and log.write then log.write("InstantCam", log.ERROR, tostring(err)) end

local lfs = require("lfs")

local function dataDir()
    return lfs.writedir() .. "..\\InstantCam\\"
end

local function writeMissionInfo()
    local m = DCS.getCurrentMission and DCS.getCurrentMission()
    if not m or not m.mission then return end
    local theatre = tostring(m.mission.theatre or "")
    local d = m.mission.date or {}
    local dir = dataDir()
    local f = io.open(dir .. "mission.txt", "w")
    if f then
        f:write(string.format("%s|%d|%d|%d|%d\n", theatre, d.Year or 0, d.Month or 0, d.Day or 0,
            m.mission.start_time or 0))
        f:close()
    end
    -- airfields of the current map (DCS metres), for "Near <airfield>" captions
    local okT, Terrain = pcall(require, "terrain")
    if okT and type(Terrain) == "table" and Terrain.GetTerrainConfig then
        local okA, ads = pcall(Terrain.GetTerrainConfig, "Airdromes")
        if okA and type(ads) == "table" then
            local out = io.open(dir .. "airfields.txt", "w")
            if out then
                for id, a in pairs(ads) do
                    if type(a) == "table" and type(a.reference_point) == "table" and a.abandoned ~= true then
                        local name = a.display_name
                        if type(name) ~= "string" and type(a.names) == "table" then name = a.names.en or a.names["en"] end
                        if type(name) ~= "string" then name = a.name end
                        if type(name) ~= "string" then name = a.code or a.id or tostring(id) end
                        name = string.gsub(tostring(name), "|", "/")
                        out:write(string.format("%s|%.1f|%.1f\n", name, a.reference_point.x or 0, a.reference_point.y or 0))
                    end
                end
                out:close()
            end
        end
    end
    -- towns of the current map (older maps with readable towns.lua)
    local folder = theatre
    if folder == "SinaiMap" then folder = "Sinai" end
    local root = lfs.currentdir()
    if string.sub(root, -1) ~= "\\" and string.sub(root, -1) ~= "/" then root = root .. "\\" end
    for _, sub in ipairs({ "\\Map\\towns.lua", "\\map\\towns.lua" }) do
        local chunk = loadfile(root .. "Mods\\terrains\\" .. folder .. sub)
        if chunk then
            local env = { _ = function(s) return s end }
            setfenv(chunk, env)
            if pcall(chunk) and type(env.towns) == "table" then
                local out = io.open(dir .. "towns.txt", "w")
                if out then
                    for k, v in pairs(env.towns) do
                        if type(v) == "table" and v.latitude and v.longitude then
                            local name = string.gsub(tostring(v.display_name or k), "|", "/")
                            out:write(string.format("%s|%.5f|%.5f\n", name, v.latitude, v.longitude))
                        end
                    end
                    out:close()
                end
                return
            end
        end
    end
    os.remove(dir .. "towns.txt")
end

DCS.setUserCallbacks({
    onMissionLoadEnd = function() pcall(writeMissionInfo) end,
    onSimulationStart = function() pcall(writeMissionInfo) end,
})
