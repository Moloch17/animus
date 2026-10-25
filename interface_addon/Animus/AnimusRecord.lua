-- Animus: the recorder bar. A small movable bar with Record/Stop, the clock and the note count, and a note box;
-- Stop asks whether to keep or discard the recording and for a title. Keybindings (Bindings.xml): a note, and the
-- record toggle. Draws Animus.state.recording (Animus.lua) and sends its requests.

local A = Animus

BINDING_HEADER_ANIMUS = "Animus"
BINDING_NAME_ANIMUS_NOTE = "Note into the recording"
BINDING_NAME_ANIMUS_RECORD_TOGGLE = "Record / stop recording"

local GREEN, RED, GREY, WHITE = "|cff40ff40", "|cffff6060", "|cff909090", "|cffffffff"

local function Color(color, text)
    return color .. text .. "|r"
end

local function Clock(seconds)
    return format("%d:%02d", floor(seconds / 60), seconds % 60)
end

-- ---------------------------------------------------------------------------------------------------------------
-- The bar

local BAR_WIDTH, BAR_HEIGHT = 330, 58

local bar = CreateFrame("Frame", "AnimusRecordBar", UIParent)
bar:SetWidth(BAR_WIDTH)
bar:SetHeight(BAR_HEIGHT)
bar:SetPoint("TOP", UIParent, "TOP", 0, -120)
bar:SetFrameStrata("MEDIUM")
bar:SetBackdrop({
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
})
bar:SetBackdropColor(0, 0, 0, 0.75)
bar:SetMovable(true)
bar:EnableMouse(true)
bar:SetClampedToScreen(true)
bar:RegisterForDrag("LeftButton")
bar:SetScript("OnDragStart", bar.StartMoving)
bar:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    local point, _, relative, x, y = self:GetPoint()
    AnimusDB = AnimusDB or {}
    AnimusDB.recorderPos = { point = point, relative = relative, x = x, y = y }
end)
bar:Hide()

local light = bar:CreateTexture(nil, "ARTWORK")
light:SetWidth(14)
light:SetHeight(14)
light:SetPoint("TOPLEFT", bar, "TOPLEFT", 10, -9)
light:SetTexture("Interface\\COMMON\\Indicator-Red")

local label = bar:CreateFontString(nil, "OVERLAY", "GameFontNormal")
label:SetPoint("LEFT", light, "RIGHT", 4, 0)
label:SetJustifyH("LEFT")

local recordButton = CreateFrame("Button", "AnimusRecordButton", bar, "UIPanelButtonTemplate")
recordButton:SetWidth(70)
recordButton:SetHeight(20)
recordButton:SetPoint("TOPRIGHT", bar, "TOPRIGHT", -36, -6)
recordButton:SetText("Record")

local closeButton = CreateFrame("Button", nil, bar, "UIPanelCloseButton")
closeButton:SetWidth(24)
closeButton:SetHeight(24)
closeButton:SetPoint("TOPRIGHT", bar, "TOPRIGHT", -6, -4)
closeButton:SetScript("OnClick", function() bar:Hide() end)

local noteBox = CreateFrame("EditBox", "AnimusNoteBox", bar, "InputBoxTemplate")
noteBox:SetWidth(BAR_WIDTH - 90)
noteBox:SetHeight(20)
noteBox:SetPoint("BOTTOMLEFT", bar, "BOTTOMLEFT", 14, 8)
noteBox:SetAutoFocus(false)
noteBox:SetMaxLetters(200)

local noteButton = CreateFrame("Button", "AnimusNoteButton", bar, "UIPanelButtonTemplate")
noteButton:SetWidth(56)
noteButton:SetHeight(20)
noteButton:SetPoint("LEFT", noteBox, "RIGHT", 6, 0)
noteButton:SetText("Note")

local function SendNote()
    local text = strtrim(noteBox:GetText())
    if text == "" then
        return
    end
    A.Note(text)
    noteBox:SetText("")
    noteBox:ClearFocus()
end
noteButton:SetScript("OnClick", SendNote)
noteBox:SetScript("OnEnterPressed", SendNote)
noteBox:SetScript("OnEscapePressed", function(self)
    self:ClearFocus()
end)

-- A note kept: the bar flashes the number instead of printing to chat (chat is for the recording's own lines).
local flashUntil = nil
local flashText = nil
function A.OnNoteKept(seq)
    flashUntil = GetTime() + 1.5
    flashText = Color(GREEN, format("Note %d kept", seq))
end

-- ---------------------------------------------------------------------------------------------------------------
-- The stop dialog: keep or discard, with an optional title

StaticPopupDialogs["ANIMUS_STOP"] = {
    text = "Stop recording?\nGive it a title if you like, then keep it or discard it.",
    button1 = "Keep",
    button2 = "Discard",
    button3 = "Cancel",
    hasEditBox = true,
    maxLetters = 200,
    OnAccept = function(self)
        A.StopRecording("keep", self.editBox:GetText())
    end,
    OnCancel = function(self, data, reason)
        if reason == "clicked" then
            A.StopRecording("discard", self.editBox:GetText())
        end
    end,
    OnAlt = function() end,
    EditBoxOnEnterPressed = function(self)
        local dialog = self:GetParent()
        A.StopRecording("keep", self:GetText())
        dialog:Hide()
    end,
    EditBoxOnEscapePressed = function(self)
        self:GetParent():Hide()
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
    preferredIndex = 3,
}

function A.ToggleRecording()
    if not A.state.captureAvailable then
        A.SetMessage(false, "Recording is not available on this realm.")
        return
    end
    if A.IsRecording() then
        StaticPopup_Show("ANIMUS_STOP")
    else
        A.StartRecording()
    end
end
recordButton:SetScript("OnClick", A.ToggleRecording)

function A.ToggleRecordBar()
    if bar:IsShown() then
        bar:Hide()
    else
        bar:Show()
    end
end

function A.ShowRecordBar()
    bar:Show()
end

function A.FocusNote()
    bar:Show()
    noteBox:SetFocus()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Drawing

local blink = 0
local function Draw()
    local r = A.state.recording
    if r.on then
        local seconds = A.RecordingSeconds()
        local on = floor(GetTime() * 2) % 2 == 0
        light:SetAlpha(on and 1 or 0.35)
        label:SetText(Color(RED, "REC") .. "  " .. Color(WHITE, Clock(seconds)) .. "  "
            .. Color(GREY, format("%d note%s", r.notes, r.notes == 1 and "" or "s")) .. (r.god and Color(GREY, "  god") or ""))
        recordButton:SetText("Stop")
        recordButton:Enable()
        noteBox:Enable()
        noteButton:Enable()
    else
        light:SetAlpha(0.2)
        label:SetText(Color(GREY, A.state.captureAvailable and "Not recording" or "Recording unavailable here"))
        recordButton:SetText("Record")
        if A.state.captureAvailable and A.state.connected then
            recordButton:Enable()
        else
            recordButton:Disable()
        end
        noteBox:Disable()
        noteButton:Disable()
    end
    if flashUntil and GetTime() < flashUntil then
        label:SetText(flashText)
    end
end

bar:SetScript("OnUpdate", function(self, elapsed)
    blink = blink + elapsed
    if blink >= 0.25 then
        blink = 0
        Draw()
    end
end)
bar:SetScript("OnShow", function()
    if AnimusDB and AnimusDB.recorderPos then
        local p = AnimusDB.recorderPos
        bar:ClearAllPoints()
        bar:SetPoint(p.point, UIParent, p.relative, p.x, p.y)
    end
    Draw()
    if A.state.connected then
        A.RequestRecord()
    end
end)
A.OnChange(function()
    if bar:IsShown() then
        Draw()
    end
end)

-- The bar comes up by itself while a recording runs (a reload or a relog in the middle of one), once the realm says
-- so.
A.OnChange(function(s)
    if s.recording.on and not bar:IsShown() then
        bar:Show()
    end
end)
