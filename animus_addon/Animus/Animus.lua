-- Animus: the client half of mod-animus. Talks to the module over addon whispers the player sends to
-- themselves (prefix "Animus", tab-separated words); the module answers the same way. See README.md for
-- every message. This file is the protocol and the state; AnimusUI.lua draws it.

Animus = {}
local A = Animus

A.PREFIX = "Animus"
A.PROTOCOL = 2
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

function handlers.HELLO(protocol, enabled, stage)
    local s = A.state
    s.connected = true
    s.absent = false
    s.protocol = tonumber(protocol)
    s.enabled = enabled == "1"
    s.stage = stage
    s.races = {}
    s.raceOrder = {}
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
SlashCmdList.ANIMUS = function()
    A.ToggleWindow()
end
