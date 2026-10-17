#include "f3a/menu_dispatch.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <atomic>

namespace f3a::menu {
namespace {

struct Slot {
    OpenHandler  on_open;
    CloseHandler on_close;
    TickHandler  on_tick;
};

std::unordered_map<uint32_t, Slot> g_slots;

// Open menus, in the order the engine opened them; the last one is the active
// one. A single "active" field was wrong: the VATS tutorial pops up a second
// AFTER VATS, and when it closed it cleared the field while VATS was still on
// screen — so VATS stopped getting ticks and its keys went dead.
//
// The stack itself is the poll thread's (it opens, closes and ticks menus).
// The main thread asks too — "is VATS up?", "is the quantity prompt the
// active menu?" — and walking a vector while the other thread reallocates it
// is a crash, so those answers are published as atomics: the active id, and
// one bit per open menu id (all FO3 menu ids lie in 1000..1127).
std::vector<Id> g_stack;
std::atomic<Id> g_active{ Id::None };
std::atomic<uint32_t> g_open_bits[4];

void Recompute()
{
    uint32_t bits[4] = {};
    for (Id open : g_stack) {
        uint32_t n = (uint32_t)open - 1000u;
        if (n < 128u) bits[n >> 5] |= 1u << (n & 31u);
    }
    for (int k = 0; k < 4; ++k) g_open_bits[k].store(bits[k]);
    g_active.store(g_stack.empty() ? Id::None : g_stack.back());
}

} // namespace

void Init()
{
    g_slots.clear();
    g_stack.clear();
    Recompute();
}

void Shutdown()
{
    g_slots.clear();
    g_stack.clear();
    Recompute();
}

void RegisterMenu(Id id, OpenHandler on_open,
                  CloseHandler on_close, TickHandler on_tick)
{
    auto& slot = g_slots[(uint32_t)id];
    if (on_open)  slot.on_open  = std::move(on_open);
    if (on_close) slot.on_close = std::move(on_close);
    if (on_tick)  slot.on_tick  = std::move(on_tick);
}

// Menus whose opening is announced by their dedicated module (with a more
// specific message). We skip the generic "X opened" announcement for these
// so the user doesn't hear "Mapa otwarta" immediately followed by "Dane".
static bool SuppressOpenAnnouncement(Id id)
{
    switch (id) {
    case Id::Map:
    case Id::Stats:
    case Id::Inventory:
    case Id::PipBoy:
        return true;
    case Id::Start:
        // Start is both the main menu AND the in-game pause menu; the
        // announcement is context-dependent, so polling_loop speaks it
        // (with the right "Menu główne" vs "Pauza" wording) instead.
        return true;
    case Id::Message:
    case Id::Book:
    case Id::LevelUp:
        // The message module reads the actual popup text (and the LevelUp
        // line); announcing "Wiadomość otwarte" on top would be noise.
        return true;
    default:
        return false;
    }
}

void OnMenuOpen(Id id)
{
    auto at = std::find(g_stack.begin(), g_stack.end(), id);
    if (at != g_stack.end()) g_stack.erase(at);   // re-open = move to the top
    g_stack.push_back(id);
    Recompute();
    F3A_DEBUG("Menu open: %u", (unsigned)id);

    if (config::Get().speak_on_menu_open && !SuppressOpenAnnouncement(id)) {
        auto name = DisplayName(id);
        if (!name.empty()) {
            std::string label(name);
            tolk::Speak(
                strings::RenderArgs(strings::Key::MenuOpened, label.c_str()),
                tolk::Priority::Ui, true);
        }
    }

    auto it = g_slots.find((uint32_t)id);
    if (it != g_slots.end() && it->second.on_open) it->second.on_open();
}

void OnMenuClose(Id id)
{
    F3A_DEBUG("Menu close: %u", (unsigned)id);
    auto it = g_slots.find((uint32_t)id);
    if (it != g_slots.end() && it->second.on_close) it->second.on_close();
    auto at = std::find(g_stack.begin(), g_stack.end(), id);
    if (at != g_stack.end()) g_stack.erase(at);
    Recompute();   // whatever is still open underneath takes over again
}

void OnTick(float dt)
{
    // Give the tick to the topmost menu that actually HANDLES one, not simply
    // to the topmost menu. The game drops a tutorial hint over VATS a second
    // after it opens and takes it away a second later; nothing handles that
    // hint, so dispatching to it only meant VATS went deaf for the time it was
    // up — which is exactly the pause before the body-part keys start working.
    for (auto it = g_stack.rbegin(); it != g_stack.rend(); ++it) {
        auto slot = g_slots.find((uint32_t)*it);
        if (slot != g_slots.end() && slot->second.on_tick) {
            slot->second.on_tick(dt);
            return;
        }
    }
}

Id ActiveMenu() { return g_active.load(); }

bool IsOpen(Id id)
{
    uint32_t n = (uint32_t)id - 1000u;
    if (n >= 128u) return false;
    return (g_open_bits[n >> 5].load() >> (n & 31u)) & 1u;
}

std::string_view DisplayName(Id id)
{
    switch (id) {
    case Id::Message:        return strings::T("Wiadomość", "Message");
    case Id::Inventory:      return strings::T("Ekwipunek", "Inventory");
    case Id::Stats:          return strings::T("Statystyki", "Stats");
    case Id::Loading:        return strings::T("Wczytywanie", "Loading");
    case Id::Container:      return strings::T("Kontener", "Container");
    case Id::Dialog:         return strings::T("Dialog", "Dialogue");
    case Id::SleepWait:      return strings::T("Sen i czekanie", "Sleep and wait");
    case Id::Start:          return strings::T("Menu główne", "Main menu");
    case Id::LockPick:       return strings::T("Wytrych", "Lockpick");
    case Id::Quantity:       return strings::T("Ilość", "Quantity");
    case Id::Map:            return strings::T("Mapa", "Map");
    case Id::Book:           return strings::T("Notatka", "Note");
    case Id::LevelUp:        return strings::T("Awans poziomu", "Level up");
    case Id::Repair:         return strings::T("Naprawa", "Repair");
    case Id::Race:           return strings::T("Wybór postaci", "Character creation");
    case Id::TextEdit:       return strings::T("Edycja tekstu", "Text edit");
    case Id::Barter:         return strings::T("Handel", "Barter");
    case Id::Surgery:        return strings::T("Operacja", "Surgery");
    case Id::HackingShort:   return strings::T("Hackowanie", "Hacking");
    case Id::VATS:           return "V.A.T.S.";
    case Id::Computers:      return "Terminal";
    case Id::RepairServices: return strings::T("Usługi naprawcze", "Repair services");
    case Id::Tutorial:       return strings::T("Samouczek", "Tutorial");
    case Id::SpecialBookMenu:return strings::T("Księga S.P.E.C.I.A.L.", "S.P.E.C.I.A.L. book");
    case Id::PipBoy:         return "Pip-Boy";
    default:                 return {};
    }
}

} // namespace f3a::menu
