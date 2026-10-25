-- Animus: the window and the minimap button. Draws Animus.state (Animus.lua) and sends its requests. Three tabs:
-- Companion (one per character: a create panel until there is one, then the companion with summon, dismiss, rename
-- and a new race and class), Record (recordings of your own play; the bar itself is AnimusRecord.lua) and Settings
-- (dungeon party fill, Prepare, god mode).

local A = Animus

local WIDTH, HEIGHT = 400, 460

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

-- Tabs -----------------------------------------------------------------------------------------------------------

local TABS = { "Companion", "Record", "Settings" }
local tabButtons = {}
local tabPanels = {}       -- tab index -> { panel, ... } shown with it
local currentTab = 1

local function ShowTab(index)
    currentTab = index
    PanelTemplates_SetTab(frame, index)
    for i, panels in pairs(tabPanels) do
        for _, panel in ipairs(panels) do
            if i == index then
                panel:Show()
            else
                panel:Hide()
            end
        end
    end
    if A.RefreshWindow then
        A.RefreshWindow()
    end
end

for i, name in ipairs(TABS) do
    local tab = CreateFrame("Button", "AnimusFrameTab" .. i, frame, "CharacterFrameTabButtonTemplate")
    tab:SetID(i)
    tab:SetText(name)
    if i == 1 then
        tab:SetPoint("TOPLEFT", frame, "BOTTOMLEFT", 14, 10)
    else
        tab:SetPoint("LEFT", tabButtons[i - 1], "RIGHT", -16, 0)
    end
    tab:SetScript("OnClick", function(self)
        PlaySound("igCharacterInfoTab")
        ShowTab(self:GetID())
    end)
    PanelTemplates_TabResize(tab, 0)
    tabButtons[i] = tab
    tabPanels[i] = {}
end
PanelTemplates_SetNumTabs(frame, #TABS)

local function InTab(index, panel)
    tinsert(tabPanels[index], panel)
    if index ~= currentTab then
        panel:Hide()
    end
end

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
InTab(1, createPanel)

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
InTab(1, companionPanel)

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

-- How it played: recorded for training (Animus.Capture), shown while it is out.
local rateGoodButton = CreateFrame("Button", "AnimusRateGoodButton", companionPanel, "UIPanelButtonTemplate")
rateGoodButton:SetWidth(60)
rateGoodButton:SetHeight(24)
rateGoodButton:SetPoint("LEFT", dismissButton, "RIGHT", 8, 0)
rateGoodButton:SetText("Good")
rateGoodButton:SetScript("OnClick", function() A.Rate("+") end)

local rateBadButton = CreateFrame("Button", "AnimusRateBadButton", companionPanel, "UIPanelButtonTemplate")
rateBadButton:SetWidth(60)
rateBadButton:SetHeight(24)
rateBadButton:SetPoint("LEFT", rateGoodButton, "RIGHT", 4, 0)
rateBadButton:SetText("Bad")
rateBadButton:SetScript("OnClick", function() A.Rate("-") end)

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

local function SetEnabled(button, enabled)
    if enabled then
        button:Enable()
    else
        button:Disable()
    end
end

-- Record tab --------------------------------------------------------------------------------------------------------

local recordPanel = CreateFrame("Frame", nil, frame)
recordPanel:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -62)
recordPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 40)
InTab(2, recordPanel)

local recordHeader = recordPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
recordHeader:SetPoint("TOPLEFT", recordPanel, "TOPLEFT", 0, 0)
recordHeader:SetText("Record your play")

local recordHelp = recordPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
recordHelp:SetPoint("TOPLEFT", recordHeader, "BOTTOMLEFT", 0, -6)
recordHelp:SetWidth(WIDTH - 60)
recordHelp:SetJustifyH("LEFT")
recordHelp:SetText(Color(GREY, "A recording saves your movement, actions, combat, your group's chat and " ..
    "everything around you to the realm's recordings folder from the moment you press Record until you stop. It " ..
    "is identified as yours. Type notes into it whenever you like (the Note box, /animus note, or a key binding); " ..
    "pulls, rests, doors and bosses are worked out afterwards from the recording itself. When you stop, keep it " ..
    "or discard it and give it a title."))

local recordState = recordPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
recordState:SetPoint("TOPLEFT", recordHelp, "BOTTOMLEFT", 0, -14)
recordState:SetWidth(WIDTH - 60)
recordState:SetJustifyH("LEFT")

local recordToggle = CreateFrame("Button", "AnimusRecordToggleButton", recordPanel, "UIPanelButtonTemplate")
recordToggle:SetWidth(130)
recordToggle:SetHeight(24)
recordToggle:SetPoint("TOPLEFT", recordState, "BOTTOMLEFT", 0, -12)
recordToggle:SetText("Record")
recordToggle:SetScript("OnClick", function() A.ToggleRecording() end)

local recordBarButton = CreateFrame("Button", "AnimusRecordBarButton", recordPanel, "UIPanelButtonTemplate")
recordBarButton:SetWidth(130)
recordBarButton:SetHeight(24)
recordBarButton:SetPoint("LEFT", recordToggle, "RIGHT", 8, 0)
recordBarButton:SetText("Recorder bar")
recordBarButton:SetScript("OnClick", function() A.ToggleRecordBar() end)

local recordKeys = recordPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
recordKeys:SetPoint("TOPLEFT", recordToggle, "BOTTOMLEFT", 0, -14)
recordKeys:SetWidth(WIDTH - 60)
recordKeys:SetJustifyH("LEFT")
recordKeys:SetText(Color(GREY, "Key bindings (Esc > Key Bindings > Animus): a note, record/stop. Slash: /animus rec " ..
    "shows the bar, /animus note <text> notes."))

-- Settings tab ------------------------------------------------------------------------------------------------------

local settingsPanel = CreateFrame("Frame", nil, frame)
settingsPanel:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -62)
settingsPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 40)
InTab(3, settingsPanel)

local fillHeader = settingsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
fillHeader:SetPoint("TOPLEFT", settingsPanel, "TOPLEFT", 0, 0)
fillHeader:SetText("Dungeon party")

local fillCheck = CreateFrame("CheckButton", "AnimusPartyFillCheck", settingsPanel, "UICheckButtonTemplate")
fillCheck:SetPoint("TOPLEFT", fillHeader, "BOTTOMLEFT", -4, -4)
fillCheck:SetWidth(26)
fillCheck:SetHeight(26)
AnimusPartyFillCheckText:SetText("Fill my dungeon party with bots")
fillCheck:SetScript("OnClick", function(self)
    A.PartyFill(self:GetChecked() and true or false)
end)

local fillRealmCheck = CreateFrame("CheckButton", "AnimusPartyFillRealmCheck", settingsPanel, "UICheckButtonTemplate")
fillRealmCheck:SetPoint("TOPLEFT", fillCheck, "BOTTOMLEFT", 0, 0)
fillRealmCheck:SetWidth(26)
fillRealmCheck:SetHeight(26)
AnimusPartyFillRealmCheckText:SetText("Realm: fill dungeon parties (game master, until the config is reloaded)")
fillRealmCheck:SetScript("OnClick", function(self)
    A.PartyFillRealm(self:GetChecked() and true or false)
end)

local fillNote = settingsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
fillNote:SetPoint("TOPLEFT", fillRealmCheck, "BOTTOMLEFT", 4, -2)
fillNote:SetWidth(WIDTH - 60)
fillNote:SetJustifyH("LEFT")

local prepareHeader = settingsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
prepareHeader:SetPoint("TOPLEFT", fillNote, "BOTTOMLEFT", 0, -14)
prepareHeader:SetText("Prepare this character")

local prepareHelp = settingsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
prepareHelp:SetPoint("TOPLEFT", prepareHeader, "BOTTOMLEFT", 0, -4)
prepareHelp:SetWidth(WIDTH - 60)
prepareHelp:SetJustifyH("LEFT")
prepareHelp:SetText(Color(GREY, "Sets this character to the level you type, with gear, talents and every spell " ..
    "for it, built the way a companion is. Everything you wear and carry is mailed back to you first (bags too); " ..
    "keyring, bank, money and professions stay. Not while recording."))

local levelLabel = settingsPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
levelLabel:SetPoint("TOPLEFT", prepareHelp, "BOTTOMLEFT", 0, -10)
levelLabel:SetText("Level")

local levelBox = CreateFrame("EditBox", "AnimusLevelBox", settingsPanel, "InputBoxTemplate")
levelBox:SetPoint("TOPLEFT", levelLabel, "BOTTOMLEFT", 6, -4)
levelBox:SetWidth(50)
levelBox:SetHeight(20)
levelBox:SetMaxLetters(2)
levelBox:SetNumeric(true)
levelBox:SetAutoFocus(false)

local specLabel = settingsPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
specLabel:SetPoint("BOTTOMLEFT", levelLabel, "BOTTOMLEFT", 80, 0)
specLabel:SetText("Spec")

local chosenSpec = nil
local specDrop = CreateFrame("Frame", "AnimusSpecDropDown", settingsPanel, "UIDropDownMenuTemplate")
specDrop:SetPoint("TOPLEFT", specLabel, "BOTTOMLEFT", -16, 0)
UIDropDownMenu_SetWidth(specDrop, 130)

local function SetSpec(spec)
    chosenSpec = spec
    UIDropDownMenu_SetSelectedValue(specDrop, spec and spec.index or nil)
    UIDropDownMenu_SetText(specDrop, spec and spec.name or "")
    if A.RefreshWindow then
        A.RefreshWindow()
    end
end

UIDropDownMenu_Initialize(specDrop, function(self, level)
    for _, spec in ipairs(A.state.specs) do
        local info = UIDropDownMenu_CreateInfo()
        info.text = spec.name
        info.value = spec.index
        info.checked = chosenSpec and chosenSpec.index == spec.index
        info.func = function() SetSpec(spec) end
        UIDropDownMenu_AddButton(info, level)
    end
end)

StaticPopupDialogs["ANIMUS_PREPARE"] = {
    text = "Prepare %s as a level %d %s?\n\nEverything you wear and carry is mailed to you (bags included), "
        .. "talents are reset and your level is changed. This cannot be undone.",
    button1 = "Prepare",
    button2 = CANCEL,
    OnAccept = function()
        A.Prepare(levelBox:GetNumber(), chosenSpec and chosenSpec.index)
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
    preferredIndex = 3,
}

local prepareButton = CreateFrame("Button", "AnimusPrepareButton", settingsPanel, "UIPanelButtonTemplate")
prepareButton:SetWidth(120)
prepareButton:SetHeight(24)
prepareButton:SetPoint("TOPLEFT", levelBox, "BOTTOMLEFT", -6, -10)
prepareButton:SetText("Prepare")
prepareButton:SetScript("OnClick", function()
    if chosenSpec then
        StaticPopup_Show("ANIMUS_PREPARE", UnitName("player"), levelBox:GetNumber(), chosenSpec.name)
    end
end)

local godCheck = CreateFrame("CheckButton", "AnimusGodCheck", settingsPanel, "UICheckButtonTemplate")
godCheck:SetPoint("TOPLEFT", prepareButton, "BOTTOMLEFT", -4, -10)
godCheck:SetWidth(26)
godCheck:SetHeight(26)
AnimusGodCheckText:SetText("God mode (flagged in the recording; health and damage are no longer human-like)")
AnimusGodCheckText:SetWidth(WIDTH - 100)
AnimusGodCheckText:SetJustifyH("LEFT")
godCheck:SetScript("OnClick", function(self)
    A.God(self:GetChecked() and true or false)
end)

local function RefreshRecord(s)
    local r = s.recording
    if not s.captureAvailable then
        recordState:SetText(Color(RED, "Recording is not available on this realm (Animus.Capture.Enable and Dir)."))
    elseif r.on then
        local seconds = A.RecordingSeconds()
        recordState:SetText(Color(RED, "Recording") .. Color(WHITE, format("  %d:%02d  ", floor(seconds / 60),
            seconds % 60)) .. Color(GREY, format("%d note%s%s\n%s", r.notes, r.notes == 1 and "" or "s",
            r.god and ", god mode" or "", r.id or "")))
    else
        recordState:SetText(Color(GREY, "Not recording."))
    end
    recordToggle:SetText(r.on and "Stop" or "Record")
    SetEnabled(recordToggle, s.connected and s.captureAvailable)
end

local function RefreshSettings(s)
    local ready = s.connected and s.enabled
    local fill = s.settings
    fillCheck:SetChecked(fill.partyFill)
    SetEnabled(fillCheck, ready)
    if s.isGM then
        fillRealmCheck:Show()
        fillRealmCheck:SetChecked(fill.partyFillRealm)
        SetEnabled(fillRealmCheck, ready)
        fillNote:SetText(Color(GREY, fill.partyFillOverridden
            and format("Realm override in force (config says %s).", fill.partyFillConfig and "on" or "off")
            or format("Realm follows the config (%s).", fill.partyFillConfig and "on" or "off")))
    else
        fillRealmCheck:Hide()
        fillNote:SetText(Color(GREY, fill.partyFillRealm and "Dungeon parties are filled on this realm."
            or "Dungeon party fill is off on this realm."))
    end

    local prep = s.prepareAllowed
    for _, widget in ipairs({ prepareHeader, prepareHelp, levelLabel, levelBox, specLabel, specDrop, prepareButton,
        godCheck }) do
        if prep then
            widget:Show()
        else
            widget:Hide()
        end
    end
    if prep then
        if levelBox:GetText() == "" then
            levelBox:SetText(tostring(UnitLevel("player")))
        end
        if #s.specs > 0 and not chosenSpec then
            SetSpec(s.specs[1])
        end
        local level = levelBox:GetNumber()
        local levelOk = level >= s.levels.min and level <= s.levels.max
        levelLabel:SetText(format("Level (%d-%d)", s.levels.min, s.levels.max))
        SetEnabled(prepareButton, ready and levelOk and chosenSpec ~= nil and not s.recording.on)
        godCheck:SetChecked(s.recording.god)
        SetEnabled(godCheck, ready)
    end
end

-- Drawing the state -----------------------------------------------------------------------------------------------

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
        SetEnabled(rateGoodButton, ready and companion.out and not companion.loading)
        SetEnabled(rateBadButton, ready and companion.out and not companion.loading)
        SetEnabled(renameButton, ready and not companion.loading)
        SetEnabled(rerollButton, ready and not companion.loading and chosen.race ~= nil and chosen.class ~= nil)
    else
        companionPanel:Hide()
        createPanel:Show()
        HostPickers(createPanel, nameBox, -10)
        SetEnabled(createButton, ready and chosen.race ~= nil and chosen.class ~= nil
            and strtrim(nameBox:GetText()) ~= "")
    end

    RefreshRecord(s)
    RefreshSettings(s)

    if s.message then
        message:SetText(Color(s.messageOk and GREEN or RED, s.message))
    else
        message:SetText("")
    end
end

function A.RefreshWindow()
    if frame:IsShown() then
        Refresh(A.state)
    end
end
levelBox:SetScript("OnTextChanged", function() A.RefreshWindow() end)

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
    ShowTab(currentTab)
    if not A.state.connected and not A.state.waitingSince then
        A.Hello()
    else
        A.List()
        A.RequestRecord()
        A.RequestSettings()
        if A.state.prepareAllowed and #A.state.specs == 0 then
            A.RequestSpecs()
        end
    end
end)

-- The specs come once HELLO has said Prepare is allowed.
A.OnChange(function(s)
    if s.connected and s.prepareAllowed and #s.specs == 0 and not s.specsAsked then
        s.specsAsked = true
        A.RequestSpecs()
    end
end)

-- The record tab's clock, ticking while the window is open on it.
local tick = 0
frame:SetScript("OnUpdate", function(self, elapsed)
    tick = tick + elapsed
    if tick >= 1 and currentTab == 2 and A.state.recording.on then
        tick = 0
        RefreshRecord(A.state)
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
    if A.state.recording.on then
        local seconds = A.RecordingSeconds()
        GameTooltip:AddLine(format("Recording %d:%02d, %d notes", floor(seconds / 60), seconds % 60,
            A.state.recording.notes), 1, 0.4, 0.4)
    end
    GameTooltip:AddLine("Click: window. Drag to move.", 0.7, 0.7, 0.7)
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
