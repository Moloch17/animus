-- Animus: the window and the minimap button. Draws Animus.state (Animus.lua) and sends its requests.

local A = Animus

local WIDTH, HEIGHT = 400, 470
local ROWS = 4

local GREEN, RED, GREY, WHITE = "|cff40ff40", "|cffff6060", "|cff909090", "|cffffffff"

local function Color(color, text)
    return color .. text .. "|r"
end

-- ---------------------------------------------------------------------------------------------------------------
-- The window

local frame = CreateFrame("Frame", "AnimusFrame", UIParent)
frame:SetWidth(WIDTH)
frame:SetHeight(HEIGHT)
frame:SetPoint("CENTER")
frame:SetFrameStrata("DIALOG")
frame:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
frame:SetMovable(true)
frame:EnableMouse(true)
frame:SetClampedToScreen(true)
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", frame.StartMoving)
frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
frame:Hide()
tinsert(UISpecialFrames, "AnimusFrame")

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", frame, "TOP", 0, -18)
title:SetText("Animus")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -6, -7)

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
status:SetPoint("TOP", title, "BOTTOM", 0, -4)
status:SetWidth(WIDTH - 50)
status:SetJustifyH("CENTER")

-- Companions -----------------------------------------------------------------------------------------------------

local partyHeader = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
partyHeader:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -62)

local rows = {}
for i = 1, ROWS do
    local row = CreateFrame("Frame", nil, frame)
    row:SetWidth(WIDTH - 44)
    row:SetHeight(34)
    row:SetPoint("TOPLEFT", partyHeader, "BOTTOMLEFT", 0, -6 - (i - 1) * 36)

    row.name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    row.name:SetPoint("TOPLEFT", row, "TOPLEFT", 4, -2)
    row.name:SetJustifyH("LEFT")

    row.detail = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    row.detail:SetPoint("TOPLEFT", row.name, "BOTTOMLEFT", 0, -2)
    row.detail:SetWidth(WIDTH - 52)
    row.detail:SetJustifyH("LEFT")

    row:EnableMouse(true)
    row:SetScript("OnEnter", function(self)
        if not self.companion then
            return
        end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine(self.companion.name)
        GameTooltip:AddLine(format("Level %d %s (%s)", self.companion.level, A.Display("classes", self.companion.class),
            self.companion.spec), 1, 1, 1)
        GameTooltip:AddLine(format("Model %s: %s", self.companion.modelName or "?", self.companion.model),
            1, 1, 1, true)
        if self.companion.parked then
            GameTooltip:AddLine("Waiting for you to land.", 1, 0.8, 0.4)
        end
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function() GameTooltip:Hide() end)
    rows[i] = row
end

local dismiss = CreateFrame("Button", "AnimusDismissButton", frame, "UIPanelButtonTemplate")
dismiss:SetWidth(110)
dismiss:SetHeight(22)
dismiss:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -22, -58)
dismiss:SetText("Dismiss all")
dismiss:SetScript("OnClick", A.Dismiss)

-- Summon ---------------------------------------------------------------------------------------------------------

local summonHeader = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
summonHeader:SetPoint("TOPLEFT", partyHeader, "BOTTOMLEFT", 0, -6 - ROWS * 36 - 8)
summonHeader:SetText("Summon a companion")

local chosen = { race = nil, class = nil, wants = "dps" }

local raceLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
raceLabel:SetPoint("TOPLEFT", summonHeader, "BOTTOMLEFT", 0, -10)
raceLabel:SetText("Race")

local raceDrop = CreateFrame("Frame", "AnimusRaceDropDown", frame, "UIDropDownMenuTemplate")
raceDrop:SetPoint("TOPLEFT", raceLabel, "BOTTOMLEFT", -16, -2)
UIDropDownMenu_SetWidth(raceDrop, 140)

local classLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
classLabel:SetPoint("LEFT", raceLabel, "LEFT", 180, 0)
classLabel:SetText("Class")

local classDrop = CreateFrame("Frame", "AnimusClassDropDown", frame, "UIDropDownMenuTemplate")
classDrop:SetPoint("TOPLEFT", classLabel, "BOTTOMLEFT", -16, -2)
UIDropDownMenu_SetWidth(classDrop, 140)

local wantsLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
wantsLabel:SetPoint("TOPLEFT", raceDrop, "BOTTOMLEFT", 16, -8)
wantsLabel:SetText("Ask it to")

local wantButtons = {}
local wantHelp = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
wantHelp:SetPoint("TOPLEFT", wantsLabel, "BOTTOMLEFT", 0, -30)
wantHelp:SetWidth(WIDTH - 60)
wantHelp:SetJustifyH("LEFT")

local function RefreshWants()
    for word, button in pairs(wantButtons) do
        button:SetChecked(word == chosen.wants)
    end
    wantHelp:SetText(Color(GREY, A.WANT_HELP[chosen.wants] or ""))
end

local function WantButton(word, index)
    local button = CreateFrame("CheckButton", "AnimusWant" .. word, frame, "UIRadioButtonTemplate")
    button:SetPoint("TOPLEFT", wantsLabel, "BOTTOMLEFT", (index - 1) * 110, -4)
    _G[button:GetName() .. "Text"]:SetText(A.Display("wants", word))
    button:SetScript("OnClick", function()
        chosen.wants = word
        RefreshWants()
    end)
    wantButtons[word] = button
    return button
end

local summon = CreateFrame("Button", "AnimusSummonButton", frame, "UIPanelButtonTemplate")
summon:SetWidth(120)
summon:SetHeight(24)
summon:SetPoint("BOTTOM", frame, "BOTTOM", 0, 44)
summon:SetText("Summon")
summon:SetScript("OnClick", function()
    A.Summon(chosen.race, chosen.class, chosen.wants)
end)

local message = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
message:SetPoint("BOTTOM", frame, "BOTTOM", 0, 22)
message:SetWidth(WIDTH - 50)
message:SetJustifyH("CENTER")

-- Dropdown contents -----------------------------------------------------------------------------------------------

local function ClassesOf(race)
    return (race and A.state.races[race]) or {}
end

local function Allowed(race, class)
    for _, word in ipairs(ClassesOf(race)) do
        if word == class then
            return true
        end
    end
    return false
end

local function SetClass(class)
    chosen.class = class
    UIDropDownMenu_SetSelectedValue(classDrop, class)
    UIDropDownMenu_SetText(classDrop, class and A.Display("classes", class) or "")
end

local function SetRace(race)
    chosen.race = race
    UIDropDownMenu_SetSelectedValue(raceDrop, race)
    UIDropDownMenu_SetText(raceDrop, race and A.Display("races", race) or "")
    if not Allowed(race, chosen.class) then
        SetClass(ClassesOf(race)[1])
    end
end

UIDropDownMenu_Initialize(raceDrop, function(self, level)
    for _, race in ipairs(A.state.raceOrder) do
        local info = UIDropDownMenu_CreateInfo()
        info.text = A.Display("races", race)
        info.value = race
        info.checked = race == chosen.race
        info.func = function() SetRace(race) end
        UIDropDownMenu_AddButton(info, level)
    end
end)

UIDropDownMenu_Initialize(classDrop, function(self, level)
    for _, class in ipairs(ClassesOf(chosen.race)) do
        local info = UIDropDownMenu_CreateInfo()
        info.text = A.Display("classes", class)
        info.value = class
        info.checked = class == chosen.class
        info.func = function() SetClass(class) end
        UIDropDownMenu_AddButton(info, level)
    end
end)

-- The player's own race, in the module's words, as the first choice.
local function OwnRace()
    local _, file = UnitRace("player")
    file = strlower(file or "")
    if file == "scourge" then
        file = "undead"
    end
    return file
end

-- Drawing the state -----------------------------------------------------------------------------------------------

local function Refresh(s)
    local ready = s.connected and s.enabled
    if s.absent then
        status:SetText(Color(RED, "mod-animus is not installed on this realm."))
    elseif not s.connected then
        status:SetText(Color(GREY, s.waitingSince and "Asking the realm..." or "Not connected. /animus reconnect"))
    elseif not s.enabled then
        status:SetText(Color(RED, "Animus is disabled on this realm."))
    else
        status:SetText(Color(GREY, format("Companions play the %s models.", s.stage or "?")))
    end

    partyHeader:SetText(format("Companions (%d/%d)", #s.companions, s.max))
    for i, row in ipairs(rows) do
        local companion = s.companions[i]
        row.companion = companion
        if companion then
            local loaded = companion.model == "loaded"
            row.name:SetText(companion.name .. (companion.parked and Color(GREY, "  (waiting for you to land)") or ""))
            row.detail:SetText(format("Level %d %s (%s)  %s", companion.level, A.Display("classes", companion.class),
                companion.spec, loaded and Color(GREEN, "model loaded") or Color(RED, "model missing: follows only")))
        else
            row.name:SetText(Color(GREY, i == #s.companions + 1 and "Empty" or ""))
            row.detail:SetText("")
        end
    end

    if #s.raceOrder > 0 and not s.races[chosen.race] then
        SetRace(s.races[OwnRace()] and OwnRace() or s.raceOrder[1])
    end

    -- The wants the realm offers, in its order; built once the first hello arrives.
    if #s.wants > 0 and not next(wantButtons) then
        for index, word in ipairs(s.wants) do
            WantButton(word, index)
        end
        if not wantButtons[chosen.wants] then
            chosen.wants = s.wants[1]
        end
        RefreshWants()
    end

    local full = #s.companions >= s.max
    if ready and not full and chosen.race and chosen.class then
        summon:Enable()
    else
        summon:Disable()
    end
    if #s.companions > 0 then
        dismiss:Enable()
    else
        dismiss:Disable()
    end

    if s.message then
        message:SetText(Color(s.messageOk and GREEN or RED, s.message))
    else
        message:SetText("")
    end
end

A.OnChange(function(s)
    if frame:IsShown() then
        Refresh(s)
    end
end)
frame:SetScript("OnShow", function()
    Refresh(A.state)
    if not A.state.connected and not A.state.waitingSince then
        A.Hello()
    else
        A.List()
    end
end)

function A.ShowWindow()
    frame:Show()
end

function A.ToggleWindow()
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end

-- ---------------------------------------------------------------------------------------------------------------
-- The minimap button

local minimap = CreateFrame("Button", "AnimusMinimapButton", Minimap)
minimap:SetWidth(31)
minimap:SetHeight(31)
minimap:SetFrameStrata("MEDIUM")
minimap:SetFrameLevel(8)
minimap:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
minimap:RegisterForClicks("LeftButtonUp", "RightButtonUp")
minimap:RegisterForDrag("LeftButton")

local icon = minimap:CreateTexture(nil, "BACKGROUND")
icon:SetWidth(20)
icon:SetHeight(20)
icon:SetPoint("TOPLEFT", minimap, "TOPLEFT", 7, -5)
icon:SetTexture("Interface\\Icons\\Ability_Hunter_BeastCall")

local border = minimap:CreateTexture(nil, "OVERLAY")
border:SetWidth(53)
border:SetHeight(53)
border:SetPoint("TOPLEFT", minimap, "TOPLEFT")
border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

local function PlaceMinimapButton()
    local angle = math.rad(AnimusDB and AnimusDB.minimapAngle or 220)
    minimap:SetPoint("CENTER", Minimap, "CENTER", 80 * math.cos(angle), 80 * math.sin(angle))
end

local function DragMinimapButton()
    local mx, my = Minimap:GetCenter()
    local cx, cy = GetCursorPosition()
    local scale = Minimap:GetEffectiveScale()
    AnimusDB.minimapAngle = math.deg(math.atan2(cy / scale - my, cx / scale - mx))
    PlaceMinimapButton()
end

minimap:SetScript("OnDragStart", function(self)
    self:SetScript("OnUpdate", DragMinimapButton)
end)
minimap:SetScript("OnDragStop", function(self)
    self:SetScript("OnUpdate", nil)
end)
minimap:SetScript("OnClick", function(self, button)
    if button == "RightButton" then
        A.Dismiss()
    else
        A.ToggleWindow()
    end
end)
minimap:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:AddLine("Animus")
    GameTooltip:AddLine(format("Companions: %d/%d", #A.state.companions, A.state.max), 1, 1, 1)
    GameTooltip:AddLine("Left click: companions. Right click: dismiss all. Drag to move.", 0.7, 0.7, 0.7)
    GameTooltip:Show()
end)
minimap:SetScript("OnLeave", function() GameTooltip:Hide() end)

function A.UpdateMinimapButton()
    if AnimusDB and AnimusDB.minimapHidden then
        minimap:Hide()
    else
        PlaceMinimapButton()
        minimap:Show()
    end
end

local loader = CreateFrame("Frame")
loader:RegisterEvent("PLAYER_LOGIN")
loader:SetScript("OnEvent", function()
    A.UpdateMinimapButton()
end)
