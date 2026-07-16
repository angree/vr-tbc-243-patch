-- Hand-built 2.4.3 panel.  No modern UI APIs and no external libraries.

VRConfigNS = VRConfigNS or {}
local ns = VRConfigNS

local PANEL_W, PANEL_H = 560, 520
local PAD = 18
local GROUP_PAGES = {
    ["World"] = 1,
    ["Floating game screen"] = 1,
    ["Crosshair"] = 1,
    ["Nameplates"] = 2,
    ["Highlight glow"] = 2,
    ["Aim assist"] = 2,
}

local panel
local popup

local function formatValue(setting, value)
    if setting.decimals > 0 then
        return string.format("%.2f", value)
    end
    return string.format("%d", value)
end

local function setSliderText(slider, setting, value)
    local text = _G[slider:GetName() .. "Text"]
    if text then
        text:SetText(setting.label .. ": " .. formatValue(setting, value))
    end
end

local function setControlsEnabled(enabled)
    local i
    if not panel then return end
    -- Guard Enable()/Disable() in case a widget type lacks them, so disabling
    -- controls can never throw during the test window.
    local function apply(w)
        if not w then return end
        if enabled then
            if w.Enable then w:Enable() end
        else
            if w.Disable then w:Disable() end
        end
    end
    for i = 1, #panel.sliders do apply(panel.sliders[i]) end
    for i = 1, #panel.checkboxes do apply(panel.checkboxes[i]) end
    apply(panel.prevButton)
    apply(panel.nextButton)
    apply(panel.applyButton)
    apply(panel.resetButton)
end

local function refreshWidgets()
    local i, widget, setting, value
    if not panel then return end

    panel.updating = true
    for i, widget in ipairs(panel.sliders) do
        setting = widget.setting
        value = ns.NormalizeSlider(setting, panel.values[setting.cvar])
        panel.values[setting.cvar] = value
        widget:SetValue(value)
        setSliderText(widget, setting, value)
    end
    for i, widget in ipairs(panel.checkboxes) do
        widget:SetChecked(panel.values[widget.setting.cvar] ~= 0)
    end
    panel.updating = false
end

local function showPage(page)
    local i, widget
    if not panel then return end

    if page < 1 then page = 1 end
    if page > 2 then page = 2 end
    panel.currentPage = page

    for i = 1, 2 do
        for _, widget in ipairs(panel.pageWidgets[i]) do
            if i == page then widget:Show() else widget:Hide() end
        end
    end
    panel.pageText:SetText("Page " .. page .. " / 2")
    if page == 1 then panel.prevButton:Disable() else panel.prevButton:Enable() end
    if page == 2 then panel.nextButton:Disable() else panel.nextButton:Enable() end
end

local function updatePopupText()
    if popup and popup.text then
        popup.text:SetText("Keep VR settings? Reverting in " .. popup.remaining .. "...")
    end
end

local function finishPending(keep)
    local snapshot, pending
    if not popup or not popup.pending then return end

    snapshot = popup.snapshot
    pending = popup.pendingValues
    popup:SetScript("OnUpdate", nil)
    popup:Hide()
    popup.pending = false
    popup.remaining = nil
    popup.snapshot = nil
    popup.pendingValues = nil

    if keep then
        ns.CommitValues(pending)
        panel.values = ns.CopyValues(pending)
    else
        ns.PushDangerous(snapshot)
        panel.values = ns.CopyValues(snapshot)
        refreshWidgets()
    end
    setControlsEnabled(true)
    showPage(panel.currentPage or 1)
end

local function beginApply()
    local candidate
    if not panel or not popup or popup.pending then return end

    -- VRConfigDB is the last confirmed, live configuration.
    popup.snapshot = ns.CopyValues(VRConfigDB)
    candidate = ns.CopyValues(panel.values)
    popup.pendingValues = candidate
    popup.pending = true
    popup.remaining = 20

    ns.PushDangerous(candidate)
    setControlsEnabled(false)
    updatePopupText()
    popup:Show()
    popup.elapsed = 0
    popup:SetScript("OnUpdate", function(self, elapsed)
        self.elapsed = self.elapsed + (elapsed or 0)
        while self.elapsed >= 1 do
            self.elapsed = self.elapsed - 1
            self.remaining = self.remaining - 1
            if self.remaining <= 0 then
                finishPending(false)
                return
            end
            updatePopupText()
        end
    end)
end

local function makeCheckbox(parent, setting)
    local checkbox = CreateFrame("CheckButton", nil, parent, "UICheckButtonTemplate")
    checkbox:SetWidth(20)
    checkbox:SetHeight(20)
    checkbox.setting = setting

    local label = parent:CreateFontString(nil, "ARTWORK", "GameFontHighlight")
    label:SetPoint("LEFT", checkbox, "RIGHT", 4, 1)
    label:SetText(setting.label)
    checkbox.label = label

    checkbox:SetScript("OnClick", function(self)
        if panel and not panel.updating then
            local v = self:GetChecked() and 1 or 0
            panel.values[self.setting.cvar] = v
            if self.setting.live ~= false then
                ns.PushOne(self.setting.cvar, v)
                ns.CommitOne(self.setting.cvar, v)
            end
        end
    end)
    return checkbox
end

local function buildPopup()
    if popup or type(CreateFrame) ~= "function" or not UIParent then return end

    popup = CreateFrame("Frame", "VRConfigConfirmFrame", UIParent)
    popup:SetWidth(340)
    popup:SetHeight(92)
    -- Sit ABOVE the panel (never over the sliders) and on the top-most strata, so
    -- the "Keep" button is always clickable and no slider steals the click.
    popup:ClearAllPoints()
    if panel then
        popup:SetPoint("BOTTOM", panel, "TOP", 0, 12)
    else
        popup:SetPoint("TOP", UIParent, "TOP", 0, -40)
    end
    popup:SetFrameStrata("FULLSCREEN_DIALOG")
    popup:SetToplevel(true)
    popup:EnableMouse(true)
    popup:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 8, right = 8, top = 8, bottom = 8 },
    })
    popup:Hide()

    popup.text = popup:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    popup.text:SetPoint("TOP", popup, "TOP", 0, -19)

    local keep = CreateFrame("Button", nil, popup, "UIPanelButtonTemplate")
    keep:SetWidth(90)
    keep:SetHeight(22)
    keep:SetPoint("BOTTOM", popup, "BOTTOM", 0, 14)
    keep:SetText("Keep")
    keep:SetScript("OnClick", function()
        finishPending(true)
    end)
end

local function addPageWidget(page, widget)
    panel.pageWidgets[page][#panel.pageWidgets[page] + 1] = widget
end

local function buildPanel()
    local y, currentGroup, currentPage, i, setting, slider, checkbox
    if panel or type(CreateFrame) ~= "function" or not UIParent then return end

    panel = CreateFrame("Frame", "VRConfigFrame", UIParent)
    panel:SetWidth(PANEL_W)
    panel:SetHeight(PANEL_H)
    panel:SetPoint("CENTER")
    panel:SetFrameStrata("DIALOG")
    panel:EnableMouse(true)
    panel:SetMovable(true)
    panel:RegisterForDrag("LeftButton")
    panel:SetScript("OnDragStart", function(self) self:StartMoving() end)
    panel:SetScript("OnDragStop", function(self) self:StopMovingOrSizing() end)
    panel:SetClampedToScreen(true)
    panel:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 8, right = 8, top = 8, bottom = 8 },
    })
    panel:Hide()

    local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
    title:SetPoint("TOP", panel, "TOP", 0, -14)
    title:SetText("VR Config")

    local close = CreateFrame("Button", nil, panel, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", panel, "TOPRIGHT", -4, -4)
    close:SetScript("OnClick", function() panel:Hide() end)

    panel.sliders = {}
    panel.checkboxes = {}
    panel.pageWidgets = { {}, {} }
    panel.values = ns.CopyValues(VRConfigDB)
    panel.currentPage = 1
    y = { [1] = -72, [2] = -72 }

    for i, setting in ipairs(ns.sliderSettings) do
        currentPage = GROUP_PAGES[setting.group]
        if setting.group ~= currentGroup then
            currentGroup = setting.group
            local heading = panel:CreateFontString(nil, "ARTWORK", "GameFontNormal")
            heading:SetPoint("TOPLEFT", panel, "TOPLEFT", PAD, y[currentPage])
            heading:SetText(currentGroup)
            heading:SetTextColor(1, 0.82, 0)
            addPageWidget(currentPage, heading)
            y[currentPage] = y[currentPage] - 19
        end

        slider = CreateFrame("Slider", "VRConfigSlider" .. i, panel, "OptionsSliderTemplate")
        slider:SetWidth(PANEL_W - (PAD * 2) - 8)
        slider:SetHeight(16)
        slider:SetPoint("TOPLEFT", panel, "TOPLEFT", PAD, y[currentPage])
        slider:SetMinMaxValues(setting.min, setting.max)
        slider:SetValueStep(setting.step)
        slider.setting = setting

        local low = _G[slider:GetName() .. "Low"]
        local high = _G[slider:GetName() .. "High"]
        if low then low:SetText(formatValue(setting, setting.min)) end
        if high then high:SetText(formatValue(setting, setting.max)) end

        slider:SetScript("OnValueChanged", function(self, value)
            if not panel or panel.updating then return end
            value = ns.NormalizeSlider(self.setting, value)
            panel.values[self.setting.cvar] = value
            setSliderText(self, self.setting, value)
            -- Non-dangerous settings apply immediately as you drag; the screen /
            -- world-scale ones (live == false) wait for Apply + the 20s revert.
            if self.setting.live ~= false then
                ns.PushOne(self.setting.cvar, value)
                ns.CommitOne(self.setting.cvar, value)
            end
        end)
        panel.sliders[#panel.sliders + 1] = slider
        addPageWidget(currentPage, slider)
        y[currentPage] = y[currentPage] - 36

        -- The two boolean settings sit with their related slider group.
        local nextSetting = ns.sliderSettings[i + 1]
        local groupEnds = not nextSetting or nextSetting.group ~= currentGroup
        if groupEnds then
            local c
            for c, setting in ipairs(ns.checkboxSettings) do
                if setting.group == currentGroup then
                    checkbox = makeCheckbox(panel, setting)
                    checkbox:SetPoint("TOPLEFT", panel, "TOPLEFT", PAD, y[currentPage])
                    panel.checkboxes[#panel.checkboxes + 1] = checkbox
                    addPageWidget(currentPage, checkbox)
                    addPageWidget(currentPage, checkbox.label)
                    y[currentPage] = y[currentPage] - 24
                end
            end
        end
    end

    panel.prevButton = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    panel.prevButton:SetWidth(80)
    panel.prevButton:SetHeight(22)
    panel.prevButton:SetPoint("BOTTOMLEFT", panel, "BOTTOMLEFT", 18, 14)
    panel.prevButton:SetText("< Prev")
    panel.prevButton:SetScript("OnClick", function() showPage(panel.currentPage - 1) end)

    panel.nextButton = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    panel.nextButton:SetWidth(80)
    panel.nextButton:SetHeight(22)
    panel.nextButton:SetPoint("LEFT", panel.prevButton, "RIGHT", 8, 0)
    panel.nextButton:SetText("Next >")
    panel.nextButton:SetScript("OnClick", function() showPage(panel.currentPage + 1) end)

    panel.pageText = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    panel.pageText:SetPoint("LEFT", panel.nextButton, "RIGHT", 12, 0)

    panel.applyButton = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    panel.applyButton:SetWidth(100)
    panel.applyButton:SetHeight(22)
    panel.applyButton:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -18, 14)
    panel.applyButton:SetText("Apply")
    panel.applyButton:SetScript("OnClick", beginApply)

    panel.resetButton = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    panel.resetButton:SetWidth(150)
    panel.resetButton:SetHeight(22)
    panel.resetButton:SetPoint("BOTTOMRIGHT", panel.applyButton, "BOTTOMLEFT", -10, 0)
    panel.resetButton:SetText("Reset to defaults")
    panel.resetButton:SetScript("OnClick", function()
        if popup and popup.pending then return end
        panel.values = ns.DefaultValues()
        refreshWidgets()
        beginApply()
    end)

    local footer = panel:CreateFontString(nil, "ARTWORK", "GameFontDisableSmall")
    footer:SetPoint("BOTTOM", panel, "BOTTOM", 0, 42)
    footer:SetText("Changes are tested for 20 seconds before they are saved.")

    buildPopup()
    refreshWidgets()
    showPage(1)
end

function ns.TogglePanel()
    if not panel then buildPanel() end
    if not panel then return end

    if panel:IsShown() then
        panel:Hide()
    else
        -- Do not overwrite a still-pending test configuration.
        if not popup or not popup.pending then
            panel.values = ns.CopyValues(VRConfigDB)
            refreshWidgets()
        end
        showPage(panel.currentPage or 1)
        panel:Show()
    end
end
