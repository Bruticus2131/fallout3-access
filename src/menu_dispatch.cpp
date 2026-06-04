#include "f3a/menu_dispatch.h"
#include "f3a/logger.h"
#include "f3a/tolk_bridge.h"
#include "f3a/strings.h"
#include "f3a/config.h"

#include <unordered_map>
#include <vector>

namespace f3a::menu {
namespace {

struct Slot {
    OpenHandler  on_open;
    CloseHandler on_close;
    TickHandler  on_tick;
};

std::unordered_map<uint32_t, Slot> g_slots;
Id g_active = Id::None;

} // namespace

void Init()
{
    g_slots.clear();
    g_active = Id::None;
}

void Shutdown()
{
    g_slots.clear();
    g_active = Id::None;
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
    default:
        return false;
    }
}

void OnMenuOpen(Id id)
{
    g_active = id;
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
    if (g_active == id) g_active = Id::None;
}

void OnTick(float dt)
{
    if (g_active == Id::None) return;
    auto it = g_slots.find((uint32_t)g_active);
    if (it != g_slots.end() && it->second.on_tick) it->second.on_tick(dt);
}

Id ActiveMenu() { return g_active; }

std::string_view DisplayName(Id id)
{
    switch (id) {
    case Id::Message:        return "Wiadomość";
    case Id::Inventory:      return "Ekwipunek";
    case Id::Stats:          return "Statystyki";
    case Id::Loading:        return "Wczytywanie";
    case Id::Container:      return "Kontener";
    case Id::Dialog:         return "Dialog";
    case Id::SleepWait:      return "Sen i czekanie";
    case Id::Start:          return "Menu główne";
    case Id::LockPick:       return "Wytrych";
    case Id::Quantity:       return "Ilość";
    case Id::Map:            return "Mapa";
    case Id::Book:           return "Notatka";
    case Id::LevelUp:        return "Awans poziomu";
    case Id::Repair:         return "Naprawa";
    case Id::Race:           return "Wybór postaci";
    case Id::TextEdit:       return "Edycja tekstu";
    case Id::Barter:         return "Handel";
    case Id::Surgery:        return "Operacja";
    case Id::HackingShort:   return "Hackowanie";
    case Id::VATS:           return "V.A.T.S.";
    case Id::Computers:      return "Terminal";
    case Id::RepairServices: return "Usługi naprawcze";
    case Id::Tutorial:       return "Samouczek";
    case Id::SpecialBookMenu:return "Księga S.P.E.C.I.A.L.";
    case Id::PipBoy:         return "Pip-Boy";
    default:                 return {};
    }
}

} // namespace f3a::menu
