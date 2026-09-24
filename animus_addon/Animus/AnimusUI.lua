-- Animus: the window and the minimap button. Draws Animus.state (Animus.lua) and sends its requests. One companion
-- per character: a create panel until there is one, then the companion with summon, dismiss, rename and a new race
-- and class.

local A = Animus

local WIDTH, HEIGHT = 400, 430

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

local message = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
message:SetPoint("BOTTOM", frame, "BOTTOM", 0, 22)
message:SetWidth(WIDTH - 50)
message:SetJustifyH("CENTER")

-- Race and class pickers, shared by the create panel and the change panel ---------------------------------------

local chosen = { race = nil, class = nil }

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

local raceDrop, classDrop
local pickersChanged = nil      -- the panel's refresh, called when a pick changes

local function SetClass(class)
    chosen.class = class
    UIDropDownMenu_SetSelectedValue(classDrop, class)
    UIDropDownMenu_SetText(classDrop, class and A.Display("classes", class) or "")
    if pickersChanged then
        pickersChanged()
    end
end

local function SetRace(race)
    chosen.race = race
    UIDropDownMenu_SetSelectedValue(raceDrop, race)
    UIDropDownMenu_SetText(raceDrop, race and A.Display("races", race) or "")
    if not Allowed(race, chosen.class) then
        SetClass(ClassesOf(race)[1])
    elseif pickersChanged then
        pickersChanged()
    end
end

local raceLabel, classLabel

local function BuildPickers()
    raceDrop = CreateFrame("Frame", "AnimusRaceDropDown", frame, "UIDropDownMenuTemplate")
    UIDropDownMenu_SetWidth(raceDrop, 140)
    classDrop = CreateFrame("Frame", "AnimusClassDropDown", frame, "UIDropDownMenuTemplate")
    classDrop:SetPoint("LEFT", raceDrop, "RIGHT", 0, 0)
    UIDropDownMenu_SetWidth(classDrop, 140)

    raceLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    raceLabel:SetPoint("BOTTOMLEFT", raceDrop, "TOPLEFT", 16, 2)
    raceLabel:SetText("Race")
    classLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    classLabel:SetPoint("BOTTOMLEFT", classDrop, "TOPLEFT", 16, 2)
    classLabel:SetText("Class")

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
end

-- The player's own race, in the module's words, as the first choice.
local function OwnRace()
    local _, file = UnitRace("player")
    file = strlower(file or "")
    if file == "scourge" then
        file = "undead"
    end
    return file
end

-- The pickers move between the two panels; both share one race and class choice.
local pickerHost = nil
local function HostPickers(panel, anchor, yOffset)
    if not raceDrop then
        BuildPickers()
    end
    if pickerHost == panel then
        return
    end
    pickerHost = panel
    raceDrop:ClearAllPoints()
    raceDrop:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", -16, yOffset - 16)
    for _, widget in ipairs({ raceDrop, classDrop, raceLabel, classLabel }) do
        widget:SetParent(panel)
    end
end

-- Create panel ------------------------------------------------------------------------------------------------------

local createPanel = CreateFrame("Frame", nil, frame)
createPanel:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -62)
createPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 40)

local createHeader = createPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
createHeader:SetPoint("TOPLEFT", createPanel, "TOPLEFT", 0, 0)
createHeader:SetText("Create your companion")

local createHelp = createPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
createHelp:SetPoint("TOPLEFT", createHeader, "BOTTOMLEFT", 0, -6)
createHelp:SetWidth(WIDTH - 60)
createHelp:SetJustifyH("LEFT")
createHelp:SetText(Color(GREY, "A character of its own, on an account made for it, that only you can summon. It " ..
    "joins you at your level and levels up with you. You can rename it later, or give it a new race and class, " ..
    "which makes it a new character with the same name."))

local nameLabel = createPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
nameLabel:SetPoint("TOPLEFT", createHelp, "BOTTOMLEFT", 0, -12)
nameLabel:SetText("Name")

local nameBox = CreateFrame("EditBox", "AnimusNameBox", createPanel, "InputBoxTemplate")
nameBox:SetPoint("TOPLEFT", nameLabel, "BOTTOMLEFT", 6, -4)
nameBox:SetWidth(160)
nameBox:SetHeight(20)
nameBox:SetMaxLetters(12)
nameBox:SetAutoFocus(false)

local createButton = CreateFrame("Button", "AnimusCreateButton", createPanel, "UIPanelButtonTemplate")
createButton:SetWidth(120)
createButton:SetHeight(24)
createButton:SetPoint("BOTTOM", createPanel, "BOTTOM", 0, 0)
createButton:SetText("Create bot")
createButton:SetScript("OnClick", function()
    A.Create(strtrim(nameBox:GetText()), chosen.race, chosen.class)
end)

-- Companion panel ---------------------------------------------------------------------------------------------------

local companionPanel = CreateFrame("Frame", nil, frame)
companionPanel:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -62)
companionPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 40)

local companionName = companionPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
companionName:SetPoint("TOPLEFT", companionPanel, "TOPLEFT", 0, 0)

local companionDetail = companionPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
companionDetail:SetPoint("TOPLEFT", companionName, "BOTTOMLEFT", 0, -4)
companionDetail:SetWidth(WIDTH - 60)
companionDetail:SetJustifyH("LEFT")

local summonButton = CreateFrame("Button", "AnimusSummonButton", companionPanel, "UIPanelButtonTemplate")
summonButton:SetWidth(110)
summonButton:SetHeight(24)
summonButton:SetPoint("TOPLEFT", companionDetail, "BOTTOMLEFT", 0, -10)
summonButton:SetText("Summon")
summonButton:SetScript("OnClick", A.Summon)

local dismissButton = CreateFrame("Button", "AnimusDismissButton", companionPanel, "UIPanelButtonTemplate")
dismissButton:SetWidth(110)
dismissButton:SetHeight(24)
dismissButton:SetPoint("LEFT", summonButton, "RIGHT", 8, 0)
dismissButton:SetText("Dismiss")
dismissButton:SetScript("OnClick", A.Dismiss)

local renameLabel = companionPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
renameLabel:SetPoint("TOPLEFT", summonButton, "BOTTOMLEFT", 0, -16)
renameLabel:SetText("Rename")

local renameBox = CreateFrame("EditBox", "AnimusRenameBox", companionPanel, "InputBoxTemplate")
renameBox:SetPoint("TOPLEFT", renameLabel, "BOTTOMLEFT", 6, -4)
renameBox:SetWidth(160)
renameBox:SetHeight(20)
renameBox:SetMaxLetters(12)
renameBox:SetAutoFocus(false)

local renameButton = CreateFrame("Button", "AnimusRenameButton", companionPanel, "UIPanelButtonTemplate")
renameButton:SetWidth(90)
renameButton:SetHeight(22)
renameButton:SetPoint("LEFT", renameBox, "RIGHT", 8, 0)
renameButton:SetText("Rename")
renameButton:SetScript("OnClick", function()
    A.Rename(strtrim(renameBox:GetText()))
    renameBox:SetText("")
end)

local rerollLabel = companionPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
rerollLabel:SetPoint("TOPLEFT", renameBox, "BOTTOMLEFT", -6, -14)
rerollLabel:SetText("New race and class")

local rerollHelp = companionPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
rerollHelp:SetPoint("TOPLEFT", rerollLabel, "BOTTOMLEFT", 0, -4)
rerollHelp:SetWidth(WIDTH - 60)
rerollHelp:SetJustifyH("LEFT")
rerollHelp:SetText(Color(GREY, "Resets the companion: a new character of the same name, with default talents " ..
    "and gear. Everything you changed on it is lost."))

StaticPopupDialogs["ANIMUS_REROLL"] = {
    text = "Make %s a new %s %s? The current character, its talents and its gear are lost.",
    button1 = "Reset",
    button2 = CANCEL,
    OnAccept = function()
        A.Reroll(chosen.race, chosen.class)
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

local rerollButton = CreateFrame("Button", "AnimusRerollButton", companionPanel, "UIPanelButtonTemplate")
rerollButton:SetWidth(150)
rerollButton:SetHeight(24)
rerollButton:SetPoint("BOTTOM", companionPanel, "BOTTOM", 0, 0)
rerollButton:SetText("Change race and class")
rerollButton:SetScript("OnClick", function()
    local companion = A.state.companion
    if companion and chosen.race and chosen.class then
        StaticPopup_Show("ANIMUS_REROLL", companion.name, A.Display("races", chosen.race),
            A.Display("classes", chosen.class))
    end
end)

-- Drawing the state -----------------------------------------------------------------------------------------------

local function SetEnabled(button, enabled)
    if enabled then
        button:Enable()
    else
        button:Disable()
    end
end

local function Refresh(s)
    local ready = s.connected and s.enabled
    if s.absent then
        status:SetText(Color(RED, "mod-animus is not installed on this realm."))
    elseif not s.connected then
        status:SetText(Color(GREY, s.waitingSince and "Asking the realm..." or "Not connected. Reopen the window."))
    elseif not s.enabled then
        status:SetText(Color(RED, "Animus is disabled on this realm."))
    else
        status:SetText(Color(GREY, format("Companions play the %s models.", s.stage or "?")))
    end

    if #s.raceOrder > 0 and not s.races[chosen.race] then
        SetRace(s.races[OwnRace()] and OwnRace() or s.raceOrder[1])
    end

    local companion = s.companion
    if companion then
        createPanel:Hide()
        companionPanel:Show()
        HostPickers(companionPanel, rerollHelp, -8)

        local state
        if companion.loading then
            state = Color(GREY, "on the way")
        elseif companion.out then
            state = Color(GREEN, "with you") .. (companion.parked and Color(GREY, " (waiting for you to land)") or "")
        else
            state = Color(GREY, "waiting to be summoned")
        end
        companionName:SetText(companion.name)
        local model = ""
        if companion.out and not companion.loading then
            model = companion.model == "loaded" and Color(GREEN, "\nModel loaded.")
                or Color(RED, "\nModel missing: it only follows you.")
        end
        companionDetail:SetText(format("Level %d %s %s%s, %s.%s", companion.level, A.Display("races", companion.race),
            A.Display("classes", companion.class), companion.spec ~= "" and " (" .. companion.spec .. ")" or "",
            state, model))

        SetEnabled(summonButton, ready and not companion.out)
        SetEnabled(dismissButton, ready and companion.out and not companion.loading)
        SetEnabled(renameButton, ready and not companion.loading)
        SetEnabled(rerollButton, ready and not companion.loading and chosen.race ~= nil and chosen.class ~= nil)
    else
        companionPanel:Hide()
        createPanel:Show()
        HostPickers(createPanel, nameBox, -10)
        SetEnabled(createButton, ready and chosen.race ~= nil and chosen.class ~= nil
            and strtrim(nameBox:GetText()) ~= "")
    end

    if s.message then
        message:SetText(Color(s.messageOk and GREEN or RED, s.message))
    else
        message:SetText("")
    end
end

pickersChanged = function()
    if frame:IsShown() then
        Refresh(A.state)
    end
end
nameBox:SetScript("OnTextChanged", pickersChanged)

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
minimap:RegisterForClicks("LeftButtonUp")
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
minimap:SetScript("OnClick", function()
    A.ToggleWindow()
end)
minimap:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:AddLine("Animus")
    local companion = A.state.companion
    GameTooltip:AddLine(companion and format("%s, level %d, %s", companion.name, companion.level,
        companion.out and "with you" or "waiting") or "No companion yet", 1, 1, 1)
    GameTooltip:AddLine("Click: companion. Drag to move.", 0.7, 0.7, 0.7)
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
