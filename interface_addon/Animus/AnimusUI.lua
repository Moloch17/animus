-- Animus: the window and the minimap button. Draws Animus.state (Animus.lua) and sends its requests. Three tabs:
-- Followers (bots played by the follow models: summon one to four, dismiss, who is with you), Record (recordings of
-- your own play; the bar itself is AnimusRecord.lua) and Settings (dungeon party fill, Prepare, god mode).

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

local TABS = { "Followers", "Record", "Settings" }
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

-- Followers panel ------------------------------------------------------------------------------------------------

local followPanel = CreateFrame("Frame", nil, frame)
followPanel:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -62)
followPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 40)
InTab(1, followPanel)

local followHeader = followPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
followHeader:SetPoint("TOPLEFT", followPanel, "TOPLEFT", 0, 0)
followHeader:SetText("Followers")

local followHelp = followPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
followHelp:SetPoint("TOPLEFT", followHeader, "BOTTOMLEFT", 0, -6)
followHelp:SetWidth(WIDTH - 60)
followHelp:SetJustifyH("LEFT")
followHelp:SetText(Color(GREY, "Bots of your level and faction, each played by its class's follow model as the forge "
    .. "trained it: they see through a camera, keep a map in mind and follow you through dungeons and loading screens, "
    .. "moving as a player would. They join your party (which you lead) and are never saved: dismissing them, logging "
    .. "out or leaving the dungeon they filled sends them away."))

local summonLabel = followPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
summonLabel:SetPoint("TOPLEFT", followHelp, "BOTTOMLEFT", 0, -12)
summonLabel:SetText("Summon")

local summonButtons = {}
for n = 1, 4 do
    local button = CreateFrame("Button", "AnimusSummonButton" .. n, followPanel, "UIPanelButtonTemplate")
    button:SetWidth(40)
    button:SetHeight(22)
    if n == 1 then
        button:SetPoint("LEFT", summonLabel, "RIGHT", 10, 0)
    else
        button:SetPoint("LEFT", summonButtons[n - 1], "RIGHT", 4, 0)
    end
    button:SetText(tostring(n))
    button:SetScript("OnClick", function() A.Follow(n) end)
    summonButtons[n] = button
end

local dismissButton = CreateFrame("Button", "AnimusDismissButton", followPanel, "UIPanelButtonTemplate")
dismissButton:SetWidth(90)
dismissButton:SetHeight(22)
dismissButton:SetPoint("LEFT", summonButtons[4], "RIGHT", 14, 0)
dismissButton:SetText("Dismiss")
dismissButton:SetScript("OnClick", A.Dismiss)

local followList = followPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
followList:SetPoint("TOPLEFT", summonLabel, "BOTTOMLEFT", 0, -14)
followList:SetWidth(WIDTH - 60)
followList:SetJustifyH("LEFT")

local followNote = followPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
followNote:SetPoint("BOTTOMLEFT", followPanel, "BOTTOMLEFT", 0, 0)
followNote:SetWidth(WIDTH - 60)
followNote:SetJustifyH("LEFT")
followNote:SetText(Color(GREY, "Typed: .animus follow <1-4>, .animus dismiss. Followers can only be summoned on a "
    .. "map the realm baked a camera scene for (the dungeons in Animus.Vision.Maps)."))

local function RefreshFollowers(s)
    local ready = s.connected and s.enabled
    local have = #s.followers
    local room = (s.maxFollowers or 4) - have
    for n = 1, 4 do
        SetEnabled(summonButtons[n], ready and s.maySummon and have == 0 and n <= (s.maxFollowers or 4))
    end
    SetEnabled(dismissButton, ready and have > 0)
    if not s.connected then
        followList:SetText("")
    elseif not s.maySummon then
        followList:SetText(Color(RED, "Followers are not for your account on this realm (Animus.Follow.Security)."))
    elseif have == 0 then
        followList:SetText(Color(GREY, "No followers with you. Summon one to four; they come at your level."))
    else
        followList:SetText(Color(WHITE, table.concat(s.followers, "\n")))
    end
end

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
    OnAccept = function(self, data)
        -- The level and spec the dialog was shown for, not whatever the boxes hold now.
        if data then
            A.Prepare(data.level, data.spec)
        end
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
    local level = levelBox:GetNumber()
    if chosenSpec and level >= A.state.levels.min and level <= A.state.levels.max then
        StaticPopup_Show("ANIMUS_PREPARE", UnitName("player"), level, chosenSpec.name,
            { level = level, spec = chosenSpec.index })
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
    -- An older module (protocol below 3) has no recordings or settings: those tabs are hidden.
    local modern = not s.connected or (s.protocol or 0) >= 3
    for i = 2, #TABS do
        if modern then
            tabButtons[i]:Show()
        else
            tabButtons[i]:Hide()
        end
    end
    if not modern and currentTab ~= 1 then
        ShowTab(1)
        return
    end
    if s.absent then
        status:SetText(Color(RED, "mod-animus is not installed on this realm."))
    elseif not s.connected then
        status:SetText(Color(GREY, s.waitingSince and "Asking the realm..." or "Not connected. Reopen the window."))
    elseif not s.enabled then
        status:SetText(Color(RED, "Animus is disabled on this realm."))
    else
        status:SetText(Color(GREY, format("Followers play the %s models.", s.stage or "?")))
    end

    RefreshFollowers(s)
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
        A.RequestFollowers()
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
    local have = #A.state.followers
    GameTooltip:AddLine(have > 0 and format("%d follower%s with you", have, have == 1 and "" or "s")
        or "No followers", 1, 1, 1)
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
