-- Animus: the client half of mod-animus. Talks to the module over addon whispers the player sends to
-- themselves (prefix "Animus", tab-separated words); the module answers the same way. See README.md for
-- every message. This file is the protocol and the state; AnimusUI.lua draws it.

Animus = {}
local A = Animus

A.PREFIX = "Animus"
A.PROTOCOL = 1
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
    wants = { tank = "Tank", heal = "Healer", dps = "Damage" },
}

-- What a `wants` word asks of the build, for the summon panel.
A.WANT_HELP = {
    tank = "A build that can hold a pull.",
    heal = "A build that can keep somebody up.",
    dps = "No demand: whatever the class does when nothing else is asked of it.",
}

A.state = {
    connected = false,      -- the module answered a hello
    absent = false,         -- the realm echoed our request back: no module there
    enabled = true,         -- Animus.Enable on the realm
    stage = nil,            -- the stage whose models companions play
    protocol = nil,
    races = {},             -- race word -> ordered list of class words
    raceOrder = {},
    wants = {},             -- ordered list of want words
    companions = {},        -- { name, class, spec, level, parked, modelName, model }
    max = 4,
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
    SendAddonMessage(A.PREFIX, message, "WHISPER", UnitName("player"))
end

function A.Hello()
    A.state.absent = false
    A.Send("hello")
end

function A.List()
    A.Send("list")
end

function A.Summon(race, class, wants)
    if not (race and class and wants) then
        A.SetMessage(false, "A summon names a race, a class and what to ask for.")
        return
    end
    A.Send("summon", race, class, wants)
end

function A.Dismiss()
    A.Send("dismiss")
end

function A.SetMessage(ok, text)
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
    s.wants = {}
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

function handlers.WANTS(wants)
    A.state.wants = { strsplit(",", wants or "") }
end

function handlers.PARTY(count, max)
    A.state.companions = {}
    A.state.max = tonumber(max) or A.state.max
    A.state.expected = tonumber(count) or 0
end

function handlers.MEMBER(name, class, spec, level, parked, modelName, model)
    table.insert(A.state.companions, {
        name = name, class = class, spec = spec, level = tonumber(level) or 0, parked = parked == "1",
        modelName = modelName, model = model or "",
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
SlashCmdList.ANIMUS = function(text)
    local words = { strsplit(" ", strtrim(text or "")) }
    local command = strlower(words[1] or "")
    if command == "" or command == "show" then
        A.ToggleWindow()
    elseif command == "summon" then
        A.Summon(strlower(words[2] or ""), strlower(words[3] or ""), strlower(words[4] or "dps"))
    elseif command == "list" then
        A.List()
        A.ShowWindow()
    elseif command == "dismiss" then
        A.Dismiss()
    elseif command == "hello" or command == "reconnect" then
        A.Hello()
    elseif command == "minimap" then
        AnimusDB.minimapHidden = not AnimusDB.minimapHidden
        A.UpdateMinimapButton()
    else
        A.Print("/animus -- open the window")
        A.Print("/animus summon <race> <class> [tank|heal|dps] -- a companion joins your party")
        A.Print("/animus list, /animus dismiss, /animus reconnect, /animus minimap")
    end
end
