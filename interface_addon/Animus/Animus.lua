-- Animus: the client half of mod-animus. Talks to the module over addon whispers the player sends to
-- themselves (prefix "Animus", tab-separated words); the module answers the same way. See README.md for
-- every message. This file is the protocol and the state; AnimusUI.lua draws it.

Animus = {}
local A = Animus

A.PREFIX = "Animus"
A.PROTOCOL = 3
A.TIMEOUT = 5           -- seconds without an answer before the realm is declared silent

-- The words the module accepts, shown the way the game spells them.
A.NAMES = {
    races = {
        human = "Human", dwarf = "Dwarf", nightelf = "Night Elf", gnome = "Gnome", draenei = "Draenei",
        orc = "Orc", undead = "Undead", tauren = "Tauren", troll = "Troll", bloodelf = "Blood Elf",
    },
    classes = {
        warrior = "Warrior", paladin = "Paladin", hunter = "Hunter", rogue = "Rogue", priest = "Priest",
        deathknight = "Death Knight", shaman = "Shaman", mage = "Mage", warlock = "Warlock", druid = "Druid",
    },
}

A.state = {
    connected = false,      -- the module answered a hello
    absent = false,         -- the realm echoed our request back: no module there
    enabled = true,         -- Animus.Enable on the realm
    stage = nil,            -- the stage whose models companions play
    protocol = nil,
    races = {},             -- race word -> ordered list of class words
    raceOrder = {},
    companion = nil,        -- { name, race, class, level, spec, out, loading, parked, modelName, model } or nil
    pets = {},              -- companion name -> { name, level, free, talents = { {id, row, col, maxRank, rank,
                            --   spells, dependsOn, dependsOnRank} } }
    lastRequest = nil,      -- the first word of the request the last answer was for
    message = nil,          -- the last OK/ERR text
    messageOk = true,
    waitingSince = nil,     -- GetTime() of the request still unanswered
    -- Protocol 3: recordings, settings, Prepare.
    captureAvailable = false,   -- the realm can record (Animus.Capture.Enable and Dir)
    isGM = false,
    prepareAllowed = false,     -- Prepare (and god) are for this account (Animus.Prepare.Security)
    recording = { on = false, id = nil, startedAt = nil, notes = 0, god = false },
    settings = { partyFill = true, partyFillRealm = true, partyFillOverridden = false, partyFillConfig = true },
    specs = {},             -- { { index = 0, name = "arms" }, ... } of the player's class
    levels = { min = 1, max = 80 },
}

A.listeners = {}

-- Anything drawing the state registers here and is called after every change.
function A.OnChange(callback)
    table.insert(A.listeners, callback)
end

local function Changed()
    for _, callback in ipairs(A.listeners) do
        callback(A.state)
    end
end

function A.Display(kind, word)
    return A.NAMES[kind][word] or word
end

function A.Print(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cff9bd4ffAnimus:|r " .. text)
end

-- ---------------------------------------------------------------------------------------------------------------
-- Requests

function A.Send(...)
    local message = strjoin("\t", ...)
    A.state.waitingSince = GetTime()
    A.state.lastRequest = (...)
    SendAddonMessage(A.PREFIX, message, "WHISPER", UnitName("player"))
end

function A.Hello()
    A.state.absent = false
    A.Send("hello")
end

function A.List()
    A.Send("list")
end

-- The one companion: created with a name, race and class; summoned and dismissed; renamed; or given a new race and
-- class (which makes a new character of the same name).
function A.Create(name, race, class)
    if not (name and name ~= "" and race and class) then
        A.SetMessage(false, "A companion needs a name, a race and a class.")
        return
    end
    A.Send("create", name, race, class)
end

function A.Summon()
    A.Send("summon")
end

function A.Dismiss()
    A.Send("dismiss")
end

-- Your verdict on how your companion played ("+" or "-"), recorded for training when the realm records play.
function A.Rate(sign)
    A.Send("rate", sign)
end

function A.Rename(name)
    if not name or name == "" then
        A.SetMessage(false, "Enter the new name first.")
        return
    end
    A.Send("rename", name)
end

function A.Reroll(race, class)
    if not (race and class) then
        A.SetMessage(false, "Pick a race and a class first.")
        return
    end
    A.Send("reroll", race, class)
end

-- ---------------------------------------------------------------------------------------------------------------
-- Recordings (protocol 3): your own play, recorded on the realm while you say so; notes into it.

function A.RequestRecord()
    A.Send("record")
end

function A.StartRecording()
    A.Send("record", "on")
end

-- verdict "keep" or "discard" (or nil), an optional title.
function A.StopRecording(verdict, title)
    if title and title ~= "" then
        A.Send("record", "off", verdict or "", title)
    else
        A.Send("record", "off", verdict or "")
    end
end

-- Free text into the running recording; tabs, newlines and pipes cannot travel, so they become spaces.
function A.Note(text)
    text = strtrim((text or ""):gsub("[\t\r\n|]", " "))
    if text == "" then
        A.SetMessage(false, "A note needs some text.")
        return
    end
    if #text > 200 then
        text = text:sub(1, 200)
    end
    A.Send("note", floor(GetTime() * 1000), text)
end

function A.IsRecording()
    return A.state.recording.on
end

function A.RecordingSeconds()
    local r = A.state.recording
    if not r.on or not r.startedAt then
        return 0
    end
    return floor(GetTime() - r.startedAt)
end

-- Settings: your own dungeon party fill, and (a game master's) the realm's.
function A.RequestSettings()
    A.Send("settings")
end

function A.PartyFill(on)
    A.Send("partyfill", on and "on" or "off")
end

function A.PartyFillRealm(on)
    A.Send("partyfill", "realm", on and "on" or "off")
end

-- Prepare: the specs and the levels your class takes, then the character set up at a level you typed.
function A.RequestSpecs()
    A.Send("specs")
end

function A.Prepare(level, spec)
    level = tonumber(level)
    if not level or not spec then
        A.SetMessage(false, "Type a level and pick a spec first.")
        return
    end
    A.Send("prepare", floor(level), spec)
end

function A.God(on)
    A.Send("god", on and "on" or "off")
end

-- One rank of the companion's talent learned (learn true) or unlearned, by talent id; the same of its pet.
function A.Talent(name, learn, talentId)
    A.Send("talent", name, learn and "learn" or "unlearn", talentId)
end

function A.PetTalent(name, learn, talentId)
    A.Send("pettalent", name, learn and "learn" or "unlearn", talentId)
end

function A.RequestPet(name)
    A.Send("pet", name)
end

-- The item in the owner's bag `bag` slot `slot` (client numbering) goes on the companion's inventory slot.
function A.Equip(name, bag, slot, invSlot)
    A.Send("equip", name, bag, slot, invSlot)
end

-- Whether `name` is the owner's companion, out in the world.
function A.IsCompanion(name)
    local companion = A.state.companion
    return name ~= nil and companion ~= nil and companion.out and companion.name == name
end

function A.SetMessage(ok, text)
    if text == "" then
        text = nil
    end
    A.state.message = text
    A.state.messageOk = ok
    if text then
        A.Print((ok and "" or "|cffff6060") .. text .. (ok and "" or "|r"))
    end
    Changed()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Replies

local handlers = {}

function handlers.HELLO(protocol, enabled, stage, capture, gm, prepare)
    local s = A.state
    s.connected = true
    s.absent = false
    s.protocol = tonumber(protocol)
    s.enabled = enabled == "1"
    s.stage = stage
    s.captureAvailable = capture == "1"
    s.isGM = gm == "1"
    s.prepareAllowed = prepare == "1"
    s.races = {}
    s.raceOrder = {}
    s.specs = {}
    s.specsAsked = false
    if s.protocol ~= A.PROTOCOL then
        A.SetMessage(false, format("The realm speaks Animus protocol %s, this addon %d: update one of them.",
            tostring(protocol), A.PROTOCOL))
    elseif not s.enabled then
        A.SetMessage(false, "Animus is disabled on this realm.")
    end
end

function handlers.RACE(race, classes)
    local s = A.state
    if not s.races[race] then
        table.insert(s.raceOrder, race)
    end
    s.races[race] = { strsplit(",", classes or "") }
end

function handlers.COMPANION(exists, name, race, class, level, spec, out, loading, parked, modelName, model)
    if exists ~= "1" then
        A.state.companion = nil
        return
    end
    A.state.companion = {
        name = name, race = race, class = class, level = tonumber(level) or 0, spec = spec or "",
        out = out == "1", loading = loading == "1", parked = parked == "1", modelName = modelName or "",
        model = model or "",
    }
end

function handlers.PET(name, petName, level, free, count)
    A.state.pets[name] = { name = petName, level = tonumber(level) or 0, free = tonumber(free) or 0,
        expected = tonumber(count) or 0, talents = {} }
end

function handlers.PETTALENT(name, id, row, col, maxRank, rank, spells, dependsOn, dependsOnRank)
    local pet = A.state.pets[name]
    if not pet then
        return
    end
    local list = {}
    for spell in string.gmatch(spells or "", "%d+") do
        table.insert(list, tonumber(spell))
    end
    table.insert(pet.talents, {
        id = tonumber(id), row = tonumber(row) or 0, col = tonumber(col) or 0, maxRank = tonumber(maxRank) or 0,
        rank = tonumber(rank) or 0, spells = list, dependsOn = tonumber(dependsOn) or 0,
        dependsOnRank = tonumber(dependsOnRank) or 0,
    })
end

function handlers.RECORD(on, id, seconds, notes, god, available)
    local r = A.state.recording
    r.on = on == "1"
    r.id = r.on and id or nil
    r.startedAt = r.on and (GetTime() - (tonumber(seconds) or 0)) or nil
    r.notes = tonumber(notes) or 0
    r.god = god == "1"
    if available ~= nil then
        A.state.captureAvailable = available == "1"
    end
end

function handlers.NOTE(seq, count)
    A.state.recording.notes = tonumber(count) or A.state.recording.notes
    if A.OnNoteKept then
        A.OnNoteKept(tonumber(seq) or 0)
    end
end

function handlers.SETTINGS(mine, realm, overridden, config)
    local s = A.state.settings
    s.partyFill = mine == "1"
    s.partyFillRealm = realm == "1"
    s.partyFillOverridden = overridden == "1"
    s.partyFillConfig = config == "1"
end

function handlers.SPEC(index, name)
    local n = tonumber(index)
    if not n then
        return
    end
    for i, spec in ipairs(A.state.specs) do
        if spec.index == n then
            table.remove(A.state.specs, i)
            break
        end
    end
    table.insert(A.state.specs, { index = n, name = name or tostring(n) })
    table.sort(A.state.specs, function(a, b) return a.index < b.index end)
end

function handlers.LEVELS(min, max)
    A.state.levels.min = tonumber(min) or 1
    A.state.levels.max = tonumber(max) or 80
end

function handlers.OK(text)
    A.SetMessage(true, text)
end

function handlers.ERR(text)
    A.SetMessage(false, text)
end

-- A request of ours, delivered back to us: the realm has no module to answer it.
local function Echoed(request)
    local s = A.state
    s.connected = false
    s.absent = true
    s.waitingSince = nil
    A.SetMessage(false, "The realm passed the request on instead of answering it: mod-animus is not installed there.")
end

local function OnAddonMessage(prefix, message, channel, sender)
    if prefix ~= A.PREFIX or sender ~= UnitName("player") then
        return
    end
    local words = { strsplit("\t", message) }
    local verb = table.remove(words, 1)
    if not verb then
        return
    end
    A.state.waitingSince = nil
    local handler = handlers[verb]
    if handler then
        handler(unpack(words))
    elseif verb == strlower(verb) then
        Echoed(verb)
        return
    end
    Changed()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Events and the answer timeout

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("CHAT_MSG_ADDON")
events:SetScript("OnEvent", function(self, event, ...)
    if event == "CHAT_MSG_ADDON" then
        OnAddonMessage(...)
    elseif event == "ADDON_LOADED" and (...) == "Animus" then
        AnimusDB = AnimusDB or {}
        if AnimusDB.minimapAngle == nil then
            AnimusDB.minimapAngle = 220
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        A.Hello()
    end
end)
events:SetScript("OnUpdate", function(self, elapsed)
    local since = A.state.waitingSince
    if since and GetTime() - since > A.TIMEOUT then
        A.state.waitingSince = nil
        A.state.connected = false
        A.SetMessage(false,
            "No answer from the realm. Is mod-animus installed, and AddonChannel on in worldserver.conf?")
    end
end)

-- ---------------------------------------------------------------------------------------------------------------
-- /animus

SLASH_ANIMUS1 = "/animus"
SlashCmdList.ANIMUS = function(input)
    local word, rest = strsplit(" ", strtrim(input or ""), 2)
    word = strlower(word or "")
    if word == "rec" or word == "record" then
        if A.ToggleRecordBar then
            A.ToggleRecordBar()
        end
    elseif word == "note" then
        if rest and rest ~= "" then
            A.Note(rest)
        elseif A.FocusNote then
            A.FocusNote()
        end
    else
        A.ToggleWindow()
    end
end
