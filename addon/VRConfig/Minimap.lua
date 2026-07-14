-- Draggable minimap button for World of Warcraft 2.4.3.

VRConfigNS = VRConfigNS or {}
local ns = VRConfigNS

local BUTTON_RADIUS = 80
local BUTTON_SIZE = 32

local button

local function angleToOffset(angleDeg)
    local rad = math.rad(angleDeg)
    return math.cos(rad) * BUTTON_RADIUS, math.sin(rad) * BUTTON_RADIUS
end

local function offsetToAngle(x, y)
    -- The two-argument angle helper is unreliable on 2.4.3 Lua; emulate it with atan.
    if x == 0 and y == 0 then return 0 end

    local angle
    if x == 0 then
        angle = (y > 0) and 90 or 270
    else
        angle = math.deg(math.atan(y / x))
        if x < 0 then angle = angle + 180 end
        if angle < 0 then angle = angle + 360 end
    end
    return angle
end

local function applyPosition()
    if not button or type(VRConfigDB) ~= "table" or type(VRConfigDB.minimap) ~= "table" then return end

    local x, y = angleToOffset(VRConfigDB.minimap.angle or 220)
    button:ClearAllPoints()
    button:SetPoint("CENTER", Minimap, "CENTER", x, y)
end

local function updateDrag()
    if not button or type(VRConfigDB) ~= "table" or type(VRConfigDB.minimap) ~= "table" then return end

    local mx, my = Minimap:GetCenter()
    local px, py = GetCursorPosition()
    local scale = Minimap:GetEffectiveScale()
    local x = px / scale - mx
    local y = py / scale - my
    VRConfigDB.minimap.angle = offsetToAngle(x, y)
    applyPosition()
end

local function showTooltip()
    if not button or not GameTooltip then return end

    GameTooltip:SetOwner(button, "ANCHOR_LEFT")
    GameTooltip:AddLine("VR Config")
    GameTooltip:AddLine("Left-click: open settings", 1, 1, 1)
    GameTooltip:AddLine("Right-click: 3D screenshot", 1, 1, 1)
    GameTooltip:AddLine("Drag: move", 1, 1, 1)
    GameTooltip:Show()
end

local function hideTooltip()
    if GameTooltip then GameTooltip:Hide() end
end

-- 3D screenshot with a clean frame: hide the whole UI, wait a moment so a UI-less
-- frame renders, tell the DLL to dump both eyes, then restore the UI. No C_Timer on
-- 2.4.3, so an OnUpdate frame drives the phases.
local shotTimer = (type(CreateFrame) == "function") and CreateFrame("Frame") or nil
if shotTimer then shotTimer:Hide() end

function ns.TakeScreenshot()
    if not shotTimer or shotTimer.busy then return end
    shotTimer.busy = true
    shotTimer.t = 0
    shotTimer.phase = 1
    if UIParent then UIParent:Hide() end
    shotTimer:Show()
    shotTimer:SetScript("OnUpdate", function(self, elapsed)
        self.t = self.t + (elapsed or 0)
        if self.phase == 1 and self.t >= 0.25 then
            -- UI is hidden now; arm the eye dump
            if type(SetCVar) == "function" then pcall(SetCVar, "vrDumpEyes", "1") end
            self.phase = 2
        elseif self.phase == 2 and self.t >= 1.0 then
            if UIParent then UIParent:Show() end
            if DEFAULT_CHAT_FRAME then
                DEFAULT_CHAT_FRAME:AddMessage("|cff55ddff[VR]|r 3D screenshot saved (no UI).")
            end
            self:SetScript("OnUpdate", nil)
            self:Hide()
            self.busy = false
        end
    end)
end

local function createButton()
    if button then return button end
    if type(CreateFrame) ~= "function" or not Minimap then return end

    button = CreateFrame("Button", "VRConfigMinimapButton", Minimap)
    button:SetFrameStrata("MEDIUM")
    button:SetFrameLevel(8)
    button:SetWidth(BUTTON_SIZE)
    button:SetHeight(BUTTON_SIZE)
    button:SetMovable(true)
    button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    button:RegisterForDrag("LeftButton")

    local icon = button:CreateTexture(nil, "BACKGROUND")
    -- Custom logo (BLP2 RAW1 with mipmaps + 8-bit alpha, the format 2.4.3 renders).
    icon:SetTexture("Interface\\AddOns\\VRConfig\\VR_LOGO")
    icon:SetWidth(20)
    icon:SetHeight(20)
    icon:SetPoint("CENTER", button, "CENTER", 0, 1)

    local ring = button:CreateTexture(nil, "OVERLAY")
    ring:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    ring:SetWidth(54)
    ring:SetHeight(54)
    ring:SetPoint("TOPLEFT", button, "TOPLEFT", 0, 0)

    -- No hover-highlight texture: the built-in "UI-Minimap-ZoomButton-Highlight"
    -- is a SQUARE glow meant for square zoom buttons and looks like a box around
    -- our round icon. Hover feedback is the tooltip instead.

    button:SetScript("OnClick", function(self, mouseButton)
        if mouseButton == "LeftButton" then
            if ns.TogglePanel then ns.TogglePanel() end
        elseif mouseButton == "RightButton" then
            ns.TakeScreenshot()
        end
    end)
    button:SetScript("OnDragStart", function(self)
        self:LockHighlight()
        self:SetScript("OnUpdate", updateDrag)
    end)
    button:SetScript("OnDragStop", function(self)
        self:SetScript("OnUpdate", nil)
        self:UnlockHighlight()
    end)
    button:SetScript("OnEnter", showTooltip)
    button:SetScript("OnLeave", hideTooltip)

    return button
end

function ns.InitMinimap()
    if type(VRConfigDB) ~= "table" or type(VRConfigDB.minimap) ~= "table" then return end
    if not createButton() then return end

    applyPosition()
    button:Show()
end
