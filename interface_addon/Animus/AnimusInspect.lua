-- Animus: companions in the standard frames. A "Dismiss companion" entry in the unit menu (right click on a party
-- or target frame), and an inspect window that edits a companion: its talent tab learns on left click and
-- unlearns on right click, its character pane takes items dragged from your bags, and a Pet tab shows its hunter
-- pet's tree, edited the same way. Everything goes through the requests in Animus.lua.

local A = Animus

-- Add a handler after whatever the frame already runs on the script (HookScript needs one to hook).
local function AddScript(frame, script, handler)
    if frame:GetScript(script) then
        frame:HookScript(script, handler)
    else
        frame:SetScript(script, handler)
    end
end

local function InspectedCompanion()
    local unit = InspectFrame and InspectFrame.unit
    local name = unit and UnitName(unit)
    if name and A.IsCompanion(name) then
        return name, unit
    end
    return nil
end

-- ---------------------------------------------------------------------------------------------------------------
-- The unit menu

UnitPopupButtons["ANIMUS_DISMISS"] = { text = "Dismiss companion", dist = 0 }
for _, which in ipairs({ "PARTY", "RAID_PLAYER" }) do
    local menu = UnitPopupMenus[which]
    if menu then
        -- Before CANCEL, which every menu ends with.
        table.insert(menu, #menu, "ANIMUS_DISMISS")
    end
end

hooksecurefunc("UnitPopup_HideButtons", function()
    local dropdown = UIDROPDOWNMENU_INIT_MENU
    local menu = dropdown and UnitPopupMenus[dropdown.which]
    -- Only the top level: a submenu's list is another one, and dropdown.which still names the top menu.
    if not menu or UIDROPDOWNMENU_MENU_LEVEL ~= 1 then
        return
    end
    for index, value in ipairs(menu) do
        if value == "ANIMUS_DISMISS" then
            UnitPopupShown[UIDROPDOWNMENU_MENU_LEVEL][index] = A.IsCompanion(dropdown.name) and 1 or 0
        end
    end
end)

hooksecurefunc("UnitPopup_OnClick", function(self)
    if self.value == "ANIMUS_DISMISS" then
        local dropdown = UIDROPDOWNMENU_INIT_MENU
        if dropdown and A.IsCompanion(dropdown.name) then
            A.Dismiss()
        end
    end
end)

-- ---------------------------------------------------------------------------------------------------------------
-- What is on the cursor: PickupContainerItem is the only place the bag and slot of a picked-up item are known.

local picked = nil
hooksecurefunc("PickupContainerItem", function(bag, slot)
    local link = GetContainerItemLink(bag, slot)
    if link then
        picked = { bag = bag, slot = slot, link = link }
    end
end)

-- The item on the cursor, if it came from a bag: bag, slot.
local function CursorBagItem()
    local kind, _, link = GetCursorInfo()
    if kind ~= "item" or not picked or picked.link ~= link then
        return nil
    end
    return picked.bag, picked.slot
end

-- ---------------------------------------------------------------------------------------------------------------
-- The inspect window (Blizzard_InspectUI loads on demand)

local PAPERDOLL_SLOTS = {
    "HeadSlot", "NeckSlot", "ShoulderSlot", "BackSlot", "ChestSlot", "ShirtSlot", "TabardSlot", "WristSlot",
    "HandsSlot", "WaistSlot", "LegsSlot", "FeetSlot", "Finger0Slot", "Finger1Slot", "Trinket0Slot", "Trinket1Slot",
    "MainHandSlot", "SecondaryHandSlot", "RangedSlot",
}

local petFrame = nil
local petButtons = {}
local PET_ROWS, PET_COLS = 6, 4
local CELL = 63

local function RefreshInspect()
    local name, unit = InspectedCompanion()
    if not name then
        return
    end
    NotifyInspect(unit)
    if InspectPaperDollItemSlotButton_Update then
        for _, slot in ipairs(PAPERDOLL_SLOTS) do
            local button = _G["Inspect" .. slot]
            if button then
                InspectPaperDollItemSlotButton_Update(button)
            end
        end
    end
end

-- Talents ----------------------------------------------------------------------------------------------------------

local talentHint = nil

local function TalentTab()
    if PanelTemplates_GetSelectedTab then
        return PanelTemplates_GetSelectedTab(InspectTalentFrame) or 1
    end
    return InspectTalentFrame.selectedTab or 1
end

local function OnTalentClick(self, button)
    local name = InspectedCompanion()
    if not name then
        return
    end
    local index = self.id or self:GetID()
    local link = GetTalentLink(TalentTab(), index, true, false, InspectTalentFrame.talentGroup)
    local talentId = link and tonumber(link:match("talent:(%d+)"))
    if not talentId then
        A.Print("Could not tell which talent that is.")
        return
    end
    A.Talent(name, button ~= "RightButton", talentId)
end

local function DecorateTalents()
    if not talentHint then
        talentHint = InspectTalentFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        talentHint:SetPoint("BOTTOM", InspectTalentFrame, "BOTTOM", 0, 92)
        talentHint:SetWidth(300)
        talentHint:SetJustifyH("CENTER")
        for i = 1, (MAX_NUM_TALENTS or 40) do
            local button = _G["InspectTalentFrameTalent" .. i]
            if button then
                button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
                AddScript(button, "OnClick", OnTalentClick)
            end
        end
    end
    local name = InspectedCompanion()
    if name then
        local unspent = GetUnspentTalentPoints(true, false, InspectTalentFrame.talentGroup) or 0
        talentHint:SetText(format("|cffffffff%d|r unspent. Left click learns a rank, right click unlearns one.",
            unspent))
        talentHint:Show()
    else
        talentHint:Hide()
    end
end

-- Character pane ---------------------------------------------------------------------------------------------------

local function OnSlotDrop(self)
    local name = InspectedCompanion()
    if not name or not CursorHasItem() then
        return
    end
    local bag, slot = CursorBagItem()
    if not bag then
        A.Print("Drag the item from your bags; that is the only place a companion takes gear from.")
        return
    end
    A.Equip(name, bag, slot, self:GetID())
    ClearCursor()
end

local function DecoratePaperDoll()
    for _, slot in ipairs(PAPERDOLL_SLOTS) do
        local button = _G["Inspect" .. slot]
        if button then
            AddScript(button, "OnClick", OnSlotDrop)
            AddScript(button, "OnReceiveDrag", OnSlotDrop)
        end
    end
end

-- The Pet tab ------------------------------------------------------------------------------------------------------

local function PetTalentTooltip(self)
    local talent = self.talent
    if not talent then
        return
    end
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    local spell = talent.spells[math.max(1, math.min(talent.rank, #talent.spells))]
    if spell then
        GameTooltip:SetHyperlink("spell:" .. spell)
    end
    GameTooltip:AddLine(format("Rank %d/%d", talent.rank, talent.maxRank), 1, 1, 1)
    if talent.rank < talent.maxRank and talent.rank > 0 and talent.spells[talent.rank + 1] then
        local nextName = GetSpellInfo(talent.spells[talent.rank + 1])
        GameTooltip:AddLine("Next rank: " .. (nextName or "?"), 0.7, 0.7, 0.7)
    end
    if talent.row > 0 then
        GameTooltip:AddLine(format("Requires %d points in the rows above.", talent.row * 3), 0.7, 0.7, 0.7)
    end
    if talent.dependsOn ~= 0 then
        local pet = A.state.pets[InspectedCompanion() or ""]
        local needed = pet and pet.byId[talent.dependsOn]
        local neededName = needed and GetSpellInfo(needed.spells[1])
        GameTooltip:AddLine(format("Requires %d rank%s of %s.", talent.dependsOnRank,
            talent.dependsOnRank == 1 and "" or "s", neededName or "another talent"), 0.7, 0.7, 0.7)
    end
    GameTooltip:AddLine("Left click learns a rank, right click unlearns one.", 0.5, 0.8, 1)
    GameTooltip:Show()
end

local function OnPetTalentClick(self, button)
    local name = InspectedCompanion()
    if name and self.talent then
        A.PetTalent(name, button ~= "RightButton", self.talent.id)
    end
end

local function BuildPetFrame()
    petFrame = CreateFrame("Frame", "AnimusInspectPetFrame", InspectFrame)
    petFrame:SetAllPoints(InspectFrame)
    petFrame:Hide()

    local pieces = {
        { "TopLeft", "TOPLEFT", 256, 256, 0, 0 }, { "TopRight", "TOPLEFT", 128, 256, 256, 0 },
        { "BottomLeft", "TOPLEFT", 256, 256, 0, -256 }, { "BottomRight", "TOPLEFT", 128, 256, 256, -256 },
    }
    for _, piece in ipairs(pieces) do
        local texture = petFrame:CreateTexture(nil, "BACKGROUND")
        texture:SetTexture("Interface\\TalentFrame\\UI-TalentFrame-" .. piece[1])
        texture:SetWidth(piece[3])
        texture:SetHeight(piece[4])
        texture:SetPoint(piece[2], petFrame, piece[2], piece[5], piece[6])
    end

    petFrame.title = petFrame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    petFrame.title:SetPoint("TOP", petFrame, "TOP", 0, -18)
    petFrame.points = petFrame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    petFrame.points:SetPoint("TOP", petFrame.title, "BOTTOM", 0, -4)
    petFrame.note = petFrame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    petFrame.note:SetPoint("CENTER", petFrame, "CENTER", 0, 40)
    petFrame.note:SetWidth(300)

    for row = 0, PET_ROWS - 1 do
        for col = 0, PET_COLS - 1 do
            local button = CreateFrame("Button", format("AnimusInspectPetTalent%d_%d", row, col), petFrame,
                "ItemButtonTemplate")
            button:SetPoint("TOPLEFT", petFrame, "TOPLEFT", 68 + col * CELL, -96 - row * CELL)
            button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
            button:SetScript("OnClick", OnPetTalentClick)
            button:SetScript("OnEnter", PetTalentTooltip)
            button:SetScript("OnLeave", function() GameTooltip:Hide() end)
            button.rankText = _G[button:GetName() .. "Count"]
            button.icon = _G[button:GetName() .. "IconTexture"]
            button:Hide()
            petButtons[row * PET_COLS + col] = button
        end
    end

    local hint = petFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    hint:SetPoint("BOTTOM", petFrame, "BOTTOM", 0, 92)
    hint:SetText("Left click learns a rank, right click unlearns one.")
end

local function RefreshPetFrame()
    if not petFrame or not petFrame:IsShown() then
        return
    end
    for _, button in pairs(petButtons) do
        button.talent = nil
        button:Hide()
    end

    local name = InspectedCompanion()
    local pet = name and A.state.pets[name]
    if not pet or #pet.talents == 0 then
        petFrame.title:SetText(name and (name .. "'s pet") or "Pet")
        petFrame.points:SetText("")
        petFrame.note:SetText(A.state.message and not A.state.messageOk and A.state.message
            or "No pet with talents is out.")
        return
    end

    petFrame.title:SetText(format("%s (level %d)", pet.name, pet.level))
    petFrame.points:SetText(format("|cffffffff%d|r unspent talent points", pet.free))
    petFrame.note:SetText("")
    pet.byId = {}
    for _, talent in ipairs(pet.talents) do
        pet.byId[talent.id] = talent
    end
    for _, talent in ipairs(pet.talents) do
        local button = petButtons[talent.row * PET_COLS + talent.col]
        if button and talent.spells[1] then
            local _, _, icon = GetSpellInfo(talent.spells[1])
            button.talent = talent
            button.icon:SetTexture(icon)
            button.icon:SetDesaturated(talent.rank == 0)
            button.rankText:SetText(format("%d/%d", talent.rank, talent.maxRank))
            button.rankText:Show()
            button:Show()
        end
    end
end

local function ShowPetTab(show)
    local tab = _G["InspectFrameTab4"]
    if not tab then
        return
    end
    if show then
        tab:Show()
    else
        tab:Hide()
        if petFrame and petFrame:IsShown() then
            InspectSwitchTabs(1)
        end
    end
end

local function OnInspectShown()
    local name = InspectedCompanion()
    ShowPetTab(name ~= nil)
    if name then
        A.RequestPet(name)
    end
end

local function SetUpInspect()
    DecoratePaperDoll()
    hooksecurefunc("TalentFrame_Update", function(frame)
        if frame == InspectTalentFrame then
            DecorateTalents()
        end
    end)

    BuildPetFrame()
    table.insert(INSPECTFRAME_SUBFRAMES, "AnimusInspectPetFrame")
    local tab = CreateFrame("Button", "InspectFrameTab4", InspectFrame, "CharacterFrameTabButtonTemplate")
    tab:SetID(4)
    tab:SetText("Pet")
    tab:SetPoint("LEFT", InspectFrameTab3, "RIGHT", -16, 0)
    tab:SetScript("OnClick", function(self)
        InspectSwitchTabs(self:GetID())
    end)
    PanelTemplates_SetNumTabs(InspectFrame, 4)
    PanelTemplates_TabResize(tab, 0)
    tab:Hide()

    petFrame:SetScript("OnShow", RefreshPetFrame)
    AddScript(InspectFrame, "OnShow", OnInspectShown)
    if InspectFrame_UpdateTabs then
        hooksecurefunc("InspectFrame_UpdateTabs", OnInspectShown)
    end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Wiring

A.OnChange(function(s)
    RefreshPetFrame()
    local request = s.lastRequest
    if s.message and s.messageOk and (request == "talent" or request == "pettalent" or request == "equip") then
        RefreshInspect()
    end
end)

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:SetScript("OnEvent", function(self, event, addon)
    if addon == "Blizzard_InspectUI" then
        SetUpInspect()
    end
end)
if IsAddOnLoaded("Blizzard_InspectUI") then
    SetUpInspect()
end
