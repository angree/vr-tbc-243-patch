-- VRConfig core for World of Warcraft 2.4.3.
-- The addon owns the UI values and only writes the special vr* CVars.

VRConfigNS = VRConfigNS or {}
local ns = VRConfigNS
local addonName = "VRConfig"

ns.addonName = addonName

-- Slider definitions are also the order used when values are sent to the DLL.
ns.sliderSettings = {
    -- live = false: applied only on Apply (with the 20s revert), because a bad
    -- value here can move/resize the screen so you can no longer see/click.
    -- Everything else (incl. world scale) applies live on slider drag.
    { cvar = "vrWorldScale",       min = 0.10, max = 2.00, step = 0.05, default = 0.20, label = "World scale (miniature/giant)", decimals = 2, group = "World" },
    { cvar = "vrScreenSize",       min = 0.30, max = 3.00, step = 0.05, default = 1.20, label = "Screen size", decimals = 2, group = "Floating game screen", live = false },
    { cvar = "vrScreenDistance",   min = -300, max = 0,    step = 5,    default = -100, label = "Screen distance", decimals = 0, group = "Floating game screen", live = false },
    { cvar = "vrScreenHeight",     min = -100, max = 100,  step = 1,    default = -20,  label = "Screen height", decimals = 0, group = "Floating game screen", live = false },
    { cvar = "vrNameplateMode",    min = 0,    max = 2,    step = 1,    default = 2,    label = "Nameplate type (0 Original, 1 3D, 2 NewPlate)", decimals = 0, group = "Nameplates" },
    { cvar = "vrNameplateDepth",   min = 0.00, max = 1.00, step = 0.05, default = 1.00, label = "Nameplate 3D depth", decimals = 2, group = "Nameplates" },
    { cvar = "vrNameplateZForce",  min = -0.05, max = 1.00, step = 0.05, default = -0.05, label = "Occlusion test depth (-0.05 = off)", decimals = 2, group = "Nameplates" },
    { cvar = "vrCrosshairSize",    min = 1,    max = 20,   step = 1,    default = 6,    label = "Crosshair size", decimals = 0, group = "Crosshair" },
    { cvar = "vrCrosshairOffset",  min = -20,  max = 20,   step = 1,    default = 2,    label = "Crosshair H-offset (fuse both eyes)", decimals = 0, group = "Crosshair" },
    { cvar = "vrCrosshairYoffset", min = -20,  max = 20,   step = 1,    default = 0,    label = "Crosshair V-offset", decimals = 0, group = "Crosshair" },
    { cvar = "vrMouseoverBright",  min = 1,    max = 5,    step = 1,    default = 2,    label = "Gaze highlight brightness", decimals = 0, group = "Highlight glow" },
    { cvar = "vrTargetBright",     min = 1,    max = 5,    step = 1,    default = 2,    label = "Target highlight brightness", decimals = 0, group = "Highlight glow" },
    { cvar = "vrAimRings",         min = 0,    max = 4,    step = 1,    default = 2,    label = "Aim assist rings", decimals = 0, group = "Aim assist" },
    { cvar = "vrAimSpreadDeg",     min = 0.0,  max = 6.0,  step = 0.5,  default = 2.0,  label = "Aim assist spread (deg)", decimals = 2, group = "Aim assist" },
    { cvar = "vrAimSamples",       min = 3,    max = 16,   step = 1,    default = 6,    label = "Aim assist samples", decimals = 0, group = "Aim assist" },
}

ns.checkboxSettings = {
    { cvar = "vrCrosshair", default = 1, label = "Show crosshair", group = "Crosshair" },
    { cvar = "vrAimAssist", default = 1, label = "Aim assist on", group = "Aim assist" },
}

-- Round slider values to their declared step, keeping SavedVariables tidy.
function ns.NormalizeSlider(setting, value)
    if type(value) ~= "number" then
        value = setting.default
    end
    if value < setting.min then value = setting.min end
    if value > setting.max then value = setting.max end

    value = setting.min + math.floor(((value - setting.min) / setting.step) + 0.5) * setting.step
    if setting.decimals > 0 then
        value = tonumber(string.format("%.2f", value))
    else
        value = math.floor(value + 0.5)
    end
    return value
end

function ns.CopyValues(source)
    local values = {}
    local i, setting, value

    for i, setting in ipairs(ns.sliderSettings) do
        value = source and source[setting.cvar]
        values[setting.cvar] = ns.NormalizeSlider(setting, value)
    end

    for i, setting in ipairs(ns.checkboxSettings) do
        value = source and source[setting.cvar]
        if type(value) ~= "number" and type(value) ~= "boolean" then
            value = setting.default
        end
        if value == true then
            values[setting.cvar] = 1
        elseif value == false then
            values[setting.cvar] = 0
        else
            values[setting.cvar] = (value ~= 0) and 1 or 0
        end
    end

    return values
end

function ns.DefaultValues()
    return ns.CopyValues(nil)
end

-- Live-apply a single setting immediately (used by non-dangerous sliders/checkboxes
-- as you drag them) and remember it, so it survives and persists.
function ns.PushOne(cvar, value)
    if type(SetCVar) == "function" then
        pcall(SetCVar, cvar, tostring(value))
    end
end

function ns.CommitOne(cvar, value)
    if not ns.initialized then ns.InitializeDB() end
    VRConfigDB[cvar] = value
    VRConfigDB.userConfigured = true
end

-- Push only the DANGEROUS (live == false) settings. Live settings are already applied
-- the moment the user drags them — re-pushing everything from Apply/auto-revert used
-- to snap live settings (like the nameplate mode) back to stale values.
function ns.PushDangerous(values)
    local setCVar = SetCVar
    local i, setting
    if type(setCVar) ~= "function" then return end
    values = ns.CopyValues(values)
    for i, setting in ipairs(ns.sliderSettings) do
        if setting.live == false then
            pcall(setCVar, setting.cvar, tostring(values[setting.cvar]))
        end
    end
    for i, setting in ipairs(ns.checkboxSettings) do
        if setting.live == false then
            pcall(setCVar, setting.cvar, tostring(values[setting.cvar]))
        end
    end
end

-- Send every setting in one pass.  The injected DLL consumes the vr* names.
function ns.PushValues(values)
    local setCVar = SetCVar
    local i, setting

    if type(setCVar) ~= "function" then return end
    values = ns.CopyValues(values)

    for i, setting in ipairs(ns.sliderSettings) do
        pcall(setCVar, setting.cvar, tostring(values[setting.cvar]))
    end
    for i, setting in ipairs(ns.checkboxSettings) do
        pcall(setCVar, setting.cvar, tostring(values[setting.cvar]))
    end
end

function ns.InitializeDB()
    if type(VRConfigDB) ~= "table" then
        VRConfigDB = {}
    end

    if type(VRConfigDB.minimap) ~= "table" then
        VRConfigDB.minimap = { angle = 220 }
    elseif type(VRConfigDB.minimap.angle) ~= "number" then
        VRConfigDB.minimap.angle = 220
    end

    -- Only take over the DLL config once the user has actually saved settings via
    -- the panel (Apply -> Keep). Until then, the DLL keeps reading its own
    -- vr_config.cfg, so we never clobber a working setup on login.
    if type(VRConfigDB.userConfigured) ~= "boolean" then
        VRConfigDB.userConfigured = false
    end

    local values = ns.CopyValues(VRConfigDB)
    local key, value
    for key, value in pairs(values) do
        VRConfigDB[key] = value
    end
    ns.initialized = true
end

-- (userConfigured is set true in CommitValues, the moment the user keeps settings)

function ns.CommitValues(values)
    if not ns.initialized then ns.InitializeDB() end
    values = ns.CopyValues(values)

    local key, value
    for key, value in pairs(values) do
        VRConfigDB[key] = value
    end
    -- From now on the addon owns the config and will push it on every login.
    VRConfigDB.userConfigured = true
end

-- Slash aliases are registered once at load time, as required by the TBC UI.
SLASH_VRCONFIG1 = "/vr"
SLASH_VRCONFIG2 = "/vrconfig"
if type(SlashCmdList) == "table" then
    SlashCmdList["VRCONFIG"] = function()
        if ns.TogglePanel then ns.TogglePanel() end
    end
end

local eventFrame
if type(CreateFrame) == "function" then
    eventFrame = CreateFrame("Frame")
    eventFrame:RegisterEvent("ADDON_LOADED")
    eventFrame:RegisterEvent("PLAYER_LOGIN")
    eventFrame:SetScript("OnEvent", function(self, event, arg1)
        if event == "ADDON_LOADED" and arg1 == addonName then
            ns.InitializeDB()
            self:UnregisterEvent("ADDON_LOADED")
        elseif event == "PLAYER_LOGIN" then
            if not ns.initialized then ns.InitializeDB() end
            -- Do NOT push on login unless the user has committed settings at least
            -- once; otherwise we would overwrite (and freeze) the DLL's cfg values.
            if VRConfigDB.userConfigured then
                ns.PushValues(VRConfigDB)
            end
            if ns.InitMinimap then ns.InitMinimap() end
            self:UnregisterEvent("PLAYER_LOGIN")
        end
    end)
end
