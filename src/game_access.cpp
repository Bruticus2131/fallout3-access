// Bridge between Fallout3Access modules and the running game.
//
// We use FOSE 1.7's documented globals (`g_thePlayer`, `g_interfaceManager`,
// `g_MenuVisibilityArray`) at known addresses but read fields directly off
// the structs — never invoke FOSE member functions that resolve via
// DEFINE_MEMBER_FN, because those would require linking the FOSE static lib.

#include "f3a/game_access.h"
#include "f3a/fose_runtime.h"
#include "f3a/config.h"
#include "f3a/logger.h"
#include "f3a/polling_loop.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace f3a::game {

namespace rt = fose_rt;

// The game's own UI text (tile labels, item names) is stored in the
// localization's ANSI code page — Polish FO3 uses Windows-1250, English 1252.
// Our internal pipeline is UTF-8, so convert game strings on the way out.
// Without this, Polish letters (ą ć ę ł ń ó ś ź ż) read as garbage because
// the bytes get reinterpreted as UTF-8.
static UINT ResolveGameCodepage()
{
    int cp = config::Get().game_text_codepage;
    if (cp > 0) return (UINT)cp;
    // auto: Polish localization -> 1250; otherwise the system ANSI page.
    if (config::Get().language == "pl") return 1250;
    return CP_ACP;
}

static std::string GameStrToUtf8(const char* s)
{
    if (!s || !*s) return {};
    UINT cp = ResolveGameCodepage();
    int wlen = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return s;                 // conversion failed; pass through
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(cp, 0, s, -1, w.data(), wlen);
    int alen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    std::string a(alen > 0 ? alen - 1 : 0, '\0');
    if (alen > 1) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                            a.data(), alen, nullptr, nullptr);
    }
    return a;
}

// ---- Player ---------------------------------------------------------------

bool IsPlayerValid()
{
    return rt::Player() != nullptr;
}

Vec3 GetPlayerPosition()
{
    auto* p = rt::Player();
    if (!p) return {0, 0, 0};
    return { p->posX, p->posY, p->posZ };
}

float GetPlayerYaw()
{
    auto* p = rt::Player();
    if (!p) return 0.0f;
    // Engine stores yaw in radians, +Z up, 0 = north.
    float deg = p->rotZ * 57.2957795f;
    while (deg < 0)    deg += 360.0f;
    while (deg >= 360) deg -= 360.0f;
    return deg;
}

ActorStatus GetPlayerStatus()
{
    // ActorValueOwner virtuals are Fn_00..Fn_0A in FOSE — unmapped. Until we
    // identify the GetCurrent slot reliably for 1.7.0.3 we fill what we can
    // from plain field reads.
    ActorStatus s{};
    s.in_combat = false;            // TODO: needs Actor::IsInCombat
    s.current_location = GetCurrentLocationName();
    return s;
}

std::string GetCurrentLocationName()
{
    auto* p = rt::Player();
    if (!p) return {};
    TESObjectCELL* cell = p->parentCell;
    if (!cell) return {};
    // TESObjectCELL embeds TESFullName at offset 0x018 (see GameForms.h);
    // its `name` field is a `String { char* m_data; ... }`.
    const char* raw = cell->fullName.name.m_data;
    if (!raw || !*raw) return {};
    return raw;
}

// ---- Quest target ---------------------------------------------------------

QuestTarget GetCurrentQuestTarget()
{
    QuestTarget t{};
    auto* p = rt::Player();
    if (!p || !p->questObjective) return t;
    // BGSQuestObjective holds an array of targets; the first valid one is
    // what the compass arrow points at. Field layout TODO — for now flag
    // the target as invalid so callers say "no active target".
    return t;
}

// ---- World scan -----------------------------------------------------------

std::vector<WorldEntity> ScanNearby(int, int, bool, bool)
{
    // Walking the parent cell's object list requires GetFirstRef-style
    // iteration over BSTArray<TESObjectREFR*>. Deferred to a follow-up.
    return {};
}

Bearing ComputeBearing(const Vec3& from, float from_yaw_deg, const Vec3& to)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float dist = std::sqrt(dx*dx + dy*dy);
    float bearing = std::atan2(dx, dy) * 57.2957795f;
    float rel = bearing - from_yaw_deg;
    while (rel > 180.0f)   rel -= 360.0f;
    while (rel < -180.0f)  rel += 360.0f;
    return { dist, rel };
}

// ---- Active UI ------------------------------------------------------------

std::optional<std::string> GetActiveMenuSelectionText()
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->activeTile) return std::nullopt;
    Tile* at = ifm->activeTile;

    // Tile.values is a BSSimpleArray<Value*>. Each Value carries the trait
    // ID (0xFC4 = string), the float number for numeric traits, and a
    // separate `str` slot for string traits. The user-facing label for a
    // ListBox row sits in kTileValue_string of the row's text-tile child;
    // for controls like buttons it's the tile's own _string trait.
    auto pick_string = [](Tile* t) -> const char* {
        if (!t) return nullptr;
        for (UInt32 i = 0; i < t->values.size; ++i) {
            Tile::Value* v = t->values.data[i];
            if (!v) continue;
            if (v->id == kTileValue_string && v->str && *v->str) {
                return v->str;
            }
        }
        return nullptr;
    };

    // Walk children: row tiles (item_hotrect) carry the label on a child
    // text tile rather than the row itself. Depth-limited DFS — Bethesda
    // tile trees rarely exceed 3-4 levels under a list row.
    auto search_children = [&](Tile* root, int depth, auto& self) -> const char* {
        if (!root || depth > 4) return nullptr;
        struct Node { Tile::ChildNode* item; Node* next; };
        auto* node = reinterpret_cast<Node*>(&root->childList);
        for (int safety = 0; node && safety < 4096; ++safety) {
            Tile::ChildNode* cn = node->item;
            if (cn && cn->child) {
                if (const char* s = pick_string(cn->child)) return s;
                if (const char* s = self(cn->child, depth + 1, self)) return s;
            }
            node = node->next;
        }
        return nullptr;
    };

    // Active tile first, then its children, then crawl up parents (and
    // their children) — this mirrors how the engine resolves a label
    // when the focused element is itself a wrapper.
    for (Tile* t = at; t; t = t->parent) {
        if (const char* s = pick_string(t)) return GameStrToUtf8(s);
        if (const char* s = search_children(t, 0, search_children)) {
            return GameStrToUtf8(s);
        }
    }
    // No real label resolved. Caller should stay silent rather than read
    // the internal tile identifier — that's just noise to the user.
    return std::nullopt;
}

// ---- Keyboard selection ---------------------------------------------------

namespace {

// Read a string trait if present, else nullptr.
const char* TileStringTrait(const Tile* t)
{
    if (!t) return nullptr;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (!v) continue;
        if (v->id == kTileValue_string && v->str && *v->str) return v->str;
    }
    return nullptr;
}

// Find first string label on the tile or one of its children (depth-limited).
const char* TileLabelDeep(Tile* t, int depth)
{
    if (!t || depth > 4) return nullptr;
    if (const char* s = TileStringTrait(t)) return s;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            if (const char* s = TileLabelDeep(cn->child, depth + 1)) return s;
        }
        node = node->next;
    }
    return nullptr;
}

// Read a numeric trait by id (0 if absent).
float TileNum(const Tile* t, UInt32 id)
{
    if (!t) return 0.0f;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == id) return v->num;
    }
    return 0.0f;
}

bool TileVisible(const Tile* t) { return TileNum(t, kTileValue_visible) != 0.0f; }

// Absolute Y of a tile = sum of `y` traits up the parent chain. F3 stores
// each tile's y relative to its parent, so the screen position needs the
// chain summed. Used to match the floating selection highlight to the row
// it currently sits on.
float TileAbsY(Tile* t)
{
    float y = 0.0f;
    for (Tile* p = t; p; p = p->parent) y += TileNum(p, kTileValue_y);
    return y;
}

// Does this tile's name contain `needle` (case-sensitive substring)?
bool TileNameHas(const Tile* t, const char* needle)
{
    const char* n = (t && t->name.m_data) ? t->name.m_data : nullptr;
    return n && std::strstr(n, needle) != nullptr;
}

// --- Highlight-box selection model -----------------------------------------
//
// Bethesda menus render keyboard/controller selection as a floating
// "lb_highlight_box" tile whose Y is moved onto the selected row. The
// selected row is the nearest *selectable* tile (one with both a label and a
// non-zero `target` trait — i.e. clickable) sharing the highlight's listbox.
// This covers list rows (lb_item_hotrect, str via ListItemText) AND plain
// buttons like the Yes/No of a confirmation box. (The mouse path uses
// activeTile instead; this covers keyboard nav, which never updates it.)

// A tile is a selectable "row/button": it carries a user-facing label (on
// itself or a child) AND is either clickable (target != 0) or is a list-row
// template by name. The name fallback matters: e.g. save-list rows
// (`lb_saveload_template_item`) have no target trait at all, yet they're the
// rows the highlight lands on. Static text (confirm_question etc.) has no
// target and no row-ish name, so it stays excluded.
bool TileSelectable(Tile* t, const char*& out_label)
{
    if (!t) return false;
    bool rowish = TileNum(t, kTileValue_target) != 0.0f ||
                  TileNameHas(t, "template_item") ||
                  TileNameHas(t, "hotrect");
    if (!rowish) return false;
    const char* s = TileLabelDeep(t, 0);
    if (!s) return false;
    out_label = s;
    return true;
}

// Collect ALL distinct visible strings inside a row (depth-limited DFS),
// skipping the toggle-value subtree (read separately as the row's value).
// Save-list rows carry several text parts ('AUTO', 'Vault 101'); reading
// only the first string drops information.
void CollectRowText(Tile* t, int depth, std::string& out)
{
    if (!t || depth > 3) return;
    if (TileNameHas(t, "toggle_value")) return;
    if (const char* s = TileStringTrait(t)) {
        std::string part = GameStrToUtf8(s);
        // Dedup: row tiles often repeat the same string on a child.
        if (!part.empty() && out.find(part) == std::string::npos) {
            if (!out.empty()) out += ", ";
            out += part;
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectRowText(cn->child, depth + 1, out);
        node = node->next;
    }
}

// Gather the static text of a panel: visible direct children of the listbox
// container that carry their own string but are not clickable and not rows —
// e.g. confirm_question ('Load this game?'), warnings, headers.
std::string CollectPanelStaticText(Tile* container)
{
    std::string out;
    if (!container) return out;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&container->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            Tile* c = cn->child;
            if (TileVisible(c) &&
                TileNum(c, kTileValue_target) == 0.0f &&
                !TileNameHas(c, "highlight") &&
                !TileNameHas(c, "template_item") &&
                !TileNameHas(c, "hotrect") &&
                !TileNameHas(c, "scrollbar")) {
                if (const char* s = TileStringTrait(c)) {
                    std::string part = GameStrToUtf8(s);
                    if (!part.empty() &&
                        out.find(part) == std::string::npos) {
                        if (!out.empty()) out += " ";
                        out += part;
                    }
                }
            }
        }
        node = node->next;
    }
    return out;
}

// Within a listbox subtree, find the selectable row TILE whose absolute Y is
// closest to target_y. Updates best_dy / best_tile in place.
void FindRowAtY(Tile* root, float target_y, int depth,
                float& best_dy, Tile*& best_tile)
{
    if (!root || depth > 14) return;
    const char* label = nullptr;
    if (TileSelectable(root, label)) {
        float dy = TileAbsY(root) - target_y;
        if (dy < 0) dy = -dy;
        if (dy < best_dy) { best_dy = dy; best_tile = root; }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&root->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            FindRowAtY(cn->child, target_y, depth + 1, best_dy, best_tile);
        }
        node = node->next;
    }
}

// Recurse over the whole menu tree. For every VISIBLE highlight box (one
// whose visible trait is set and whose y is non-negative — parked highlights
// read visible=0 / y=-1), find the best matching selectable row in its parent
// listbox, and keep the globally tightest match. With several menus mounted
// at once (backdrops), the highlight that sits exactly on its row wins over
// a loosely-parked one.
void ScanHighlights(Tile* root, int depth,
                    float& best_dy, Tile*& best_tile, Tile*& best_listbox)
{
    if (!root || depth > 16) return;
    if (TileNameHas(root, "highlight") && TileVisible(root) &&
        TileNum(root, kTileValue_y) >= 0.0f) {
        Tile* listbox = root->parent ? root->parent : root;
        float target_y = TileAbsY(root);
        float dy = 28.0f;               // tolerance ~ half a row height
        Tile* tile = nullptr;
        FindRowAtY(listbox, target_y, 0, dy, tile);
        if (tile && dy < best_dy) {
            best_dy = dy;
            best_tile = tile;
            best_listbox = listbox;
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&root->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            ScanHighlights(cn->child, depth + 1,
                           best_dy, best_tile, best_listbox);
        }
        node = node->next;
    }
}

// Find a descendant tile whose name contains `needle` (shallow search).
Tile* FindChildByName(Tile* t, const char* needle, int depth)
{
    if (!t || depth < 0) return nullptr;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            if (TileNameHas(cn->child, needle)) return cn->child;
            if (Tile* r = FindChildByName(cn->child, needle, depth - 1)) {
                return r;
            }
        }
        node = node->next;
    }
    return nullptr;
}

// Split a focused row into label + value. Two settings-row templates exist:
//   * `lb_meter_template_item`  — numeric slider; current value in user0
//     trait (0x1004). E.g. str='Muzyka' user0=25 → "Muzyka", "25".
//   * `lb_toggle_template_item` — stepper/toggle; current choice is TEXT in
//     the child tile `lb_toggle_value`. E.g. row str='Poziom trudności',
//     child lb_toggle_value str='Bardzo łatwy' → label + that text. Covers
//     On/Off toggles too (value 'Wł.'/'Wył.').
void DescribeRowParts(Tile* t, std::string& label, std::string& value)
{
    label.clear();
    value.clear();
    if (!t) return;
    CollectRowText(t, 0, label);

    // Walk the row tile and a couple of ancestors looking for a known
    // template, so we catch the value whether the focused tile is the row
    // itself or a wrapper/hotrect over it.
    for (Tile* p = t; p; p = p->parent) {
        if (TileNameHas(p, "meter")) {
            float v = TileNum(p, kTileValue_user0);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            value = buf;
            break;
        }
        if (TileNameHas(p, "toggle")) {
            if (Tile* vt = FindChildByName(p, "toggle_value", 2)) {
                if (const char* vs = TileLabelDeep(vt, 0)) {
                    value = GameStrToUtf8(vs);
                }
            }
            break;
        }
    }
}

} // namespace

std::optional<MenuSelection> GetKeyboardSelection()
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return std::nullopt;

    // In gameplay every menu stays mounted in menuRoot; scan only the
    // VISIBLE ones or we'd read rows of half-built / closed menus (the
    // "krzaczki" garbage during loading). At the main menu the visibility
    // flags are unreliable, so scan everything there (works today).
    const bool only_visible = poll::IsGameplayActive();

    float best_dy = 28.0f;
    Tile* best = nullptr;
    Tile* listbox = nullptr;
    {
        struct Node { Tile::ChildNode* item; Node* next; };
        auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
        for (int safety = 0; node && safety < 4096; ++safety) {
            Tile::ChildNode* cn = node->item;
            if (cn && cn->child &&
                (!only_visible || rt::TileIsVisible(cn->child))) {
                ScanHighlights(cn->child, 1, best_dy, best, listbox);
            }
            node = node->next;
        }
    }

    if (!best) return std::nullopt;
    MenuSelection sel;
    DescribeRowParts(best, sel.label, sel.value);
    if (sel.label.empty() && sel.value.empty()) return std::nullopt;
    sel.container = listbox;
    sel.context   = CollectPanelStaticText(listbox);
    return sel;
}

std::optional<std::string> GetKeyboardSelectionText()
{
    auto sel = GetKeyboardSelection();
    if (!sel) return std::nullopt;
    if (sel->value.empty()) return sel->label;
    if (sel->label.empty()) return sel->value;
    return sel->label + ", " + sel->value;
}

bool ClickMenuBack()
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return false;

    // Walk top-level menus; in each, look for a VISIBLE *back_button tile
    // (StartMenu's is `main_back_button`, str='Wstecz', id=5). Then press it
    // via the owning Menu's virtual HandleClick — a vtable call on the live
    // object, so no hardcoded address is involved and the game's own panel
    // logic decides what "back" means at the current level.
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m) {
                Tile* btn = FindChildByName(cn->child, "back_button", 6);
                if (btn && TileVisible(btn)) {
                    UInt32 id = (UInt32)TileNum(btn, kTileValue_id);
                    F3A_INFO("ClickMenuBack: '%s' (id=%u) in menu 0x%X",
                             btn->name.m_data ? btn->name.m_data : "?",
                             id, m->typeID);
                    m->HandleClick(id, btn);
                    return true;
                }
            }
        }
        node = node->next;
    }
    return false;
}

// ---- Pip-Boy --------------------------------------------------------------

PipBoyTab GetActivePipBoyTab()
{
    // Distinguish by which of the Pip-Boy sub-menus is visible:
    //   Stats     → kMenuType_Stats     (0x3EB)
    //   Items     → kMenuType_Inventory (0x3EA)
    //   Data/Map  → kMenuType_Map       (0x3FF) or the Quest/Notes/Radio
    //               sub-tab inside the StatsMenu container
    if (rt::IsMenuVisible(kMenuType_Inventory)) return PipBoyTab::Items;
    if (rt::IsMenuVisible(kMenuType_Map))       return PipBoyTab::Data;
    return PipBoyTab::Stats;
}

std::string GetActivePipBoyTabName()
{
    switch (GetActivePipBoyTab()) {
    case PipBoyTab::Stats: return "Statystyki";
    case PipBoyTab::Items: return "Przedmioty";
    case PipBoyTab::Data:  return "Dane";
    }
    return {};
}

std::string GetActivePipBoySubTabName() { return {}; }
std::optional<InventoryItem> GetSelectedInventoryItem() { return std::nullopt; }

// ---- Dialog / Barter / Lockpick / VATS — TODO ----------------------------

std::string GetCurrentDialogSpeaker() { return {}; }
std::string GetCurrentDialogLine()    { return {}; }
std::vector<DialogOption> GetDialogOptions() { return {}; }
int GetHighlightedDialogOption() { return -1; }

BarterState GetBarterState() { return {}; }
LockpickState GetLockpickState() { return {}; }
std::vector<VatsTarget> GetVatsTargets() { return {}; }
int GetVatsSelectedIndex() { return -1; }
int GetVatsQueueLength()   { return 0; }
int GetVatsQueueCapacity() { return 0; }
std::optional<std::string> PollNewSubtitle() { return std::nullopt; }

} // namespace f3a::game
