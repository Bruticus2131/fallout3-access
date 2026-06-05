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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // BGSQuestObjective.displayText (String @0x08) is the current objective
    // line, e.g. "Find the Galaxy News Radio building". The target MARKER
    // position lives in a targets array we haven't decoded yet, so for now
    // we report the text only (no bearing). position stays {0,0,0}.
    const char* raw = p->questObjective->displayText.m_data;
    if (!raw || !*raw) return t;
    t.name     = GameStrToUtf8(raw);
    t.position = { 0.0f, 0.0f, 0.0f };
    t.valid    = true;

    // Target marker (decoded from live dumps): objective + 0x14 -> Target*,
    // Target + 0x0C -> TESObjectREFR* -> world position at posX/Y/Z (0x2C).
    // The compass arrow points to this reference. Guarded; if anything looks
    // off we leave position {0,0,0} and the caller reads text only.
    auto* obj = reinterpret_cast<UInt8*>(p->questObjective);
    if (IsBadReadPtr(obj + 0x14, 4)) return t;
    UInt8* target = *reinterpret_cast<UInt8**>(obj + 0x14);
    if (IsBadReadPtr(target, 0x10)) return t;
    auto* refr = *reinterpret_cast<TESObjectREFR**>(target + 0x0C);
    if (IsBadReadPtr(refr, 0x38)) return t;
    if (refr->baseForm == nullptr) return t;
    Vec3 pos = { refr->posX, refr->posY, refr->posZ };
    if (pos.x != 0.0f || pos.y != 0.0f || pos.z != 0.0f) {
        t.position = pos;   // have a real bearing
    }
    return t;
}

// ---- World scan -----------------------------------------------------------

namespace {

// Defensive read of a possibly-bogus char*: a wrong fullName offset must not
// crash the scanner thread.
const char* SafeCStr(const char* s)
{
    if (!s) return nullptr;
    if (IsBadReadPtr(s, 4)) return nullptr;
    return s;
}

// Display name of a reference's BASE form. FOSE finds TESFullName via game
// RTTI; we can't link that, so use the per-type member offsets documented in
// FOSE's GameForms.h class layouts instead. Types not in the table (statics,
// grass...) return null and get skipped by the scanner.
const char* BaseFormName(TESForm* base)
{
    if (!base) return nullptr;
    UInt32 off;
    switch (base->typeID) {
    // TESBoundAnimObject children — TESFullName directly after TESBoundObject.
    case kFormType_Activator:
    case kFormType_TalkingActivator:
    case kFormType_Terminal:
    case kFormType_Container:
    case kFormType_Door:
    case kFormType_Light:
    case kFormType_Furniture:
    // Bound objects with the same layout prefix.
    case kFormType_Armor:
    case kFormType_Book:
    case kFormType_Clothing:
    case kFormType_Misc:
    case kFormType_Weapon:
    case kFormType_Ammo:
    case kFormType_Key:
        off = 0x30;
        break;
    // TESActorBase embeds TESFullName at 0xD0.
    case kFormType_NPC:
    case kFormType_Creature:
        off = 0xD0;
        break;
    // AlchemyItem (MagicItem mixin) — 0x48.
    case kFormType_AlchemyItem:
        off = 0x48;
        break;
    default:
        return nullptr;
    }
    auto* fn = reinterpret_cast<TESFullName*>(
        reinterpret_cast<UInt8*>(base) + off);
    const char* s = SafeCStr(fn->name.m_data);
    if (!s || !*s) return nullptr;
    if (fn->name.m_dataLen == 0 || fn->name.m_dataLen > 200) return nullptr;
    return s;
}

WorldEntity::Kind KindOf(UInt32 typeID)
{
    switch (typeID) {
    case kFormType_NPC:
    case kFormType_Creature:  return WorldEntity::Kind::Actor;
    case kFormType_Container: return WorldEntity::Kind::Container;
    case kFormType_Door:      return WorldEntity::Kind::Door;
    case kFormType_Note:      return WorldEntity::Kind::Note;
    default:                  return WorldEntity::Kind::Item;
    }
}

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

// Scan one cell's reference list into `out`.
void ScanCellRefs(TESObjectCELL* cell, const Vec3& pp, float r2,
                  const void* selfRef, bool actors_only,
                  std::vector<WorldEntity>& out)
{
    if (!Readable(cell, 0xB4)) return;
    struct Node { TESObjectREFR* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&cell->objectList);
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (!Readable(node, 8)) break;
        TESObjectREFR* r = node->item;
        node = node->next;
        if (!r || r == selfRef || !Readable(r, 0x40)) continue;
        if (r->flags & 0x820) continue;          // disabled / deleted

        TESForm* base = r->baseForm;
        if (!Readable(base, 0x08)) continue;

        WorldEntity::Kind kind = KindOf(base->typeID);
        if (actors_only && kind != WorldEntity::Kind::Actor) continue;

        float dx = r->posX - pp.x, dy = r->posY - pp.y;
        if (dx * dx + dy * dy > r2) continue;

        const char* nm = BaseFormName(base);
        std::string label;
        if (nm) {
            label = GameStrToUtf8(nm);
        } else if (kind == WorldEntity::Kind::Door) {
            label = "Drzwi";       // unnamed load doors = metro entrances
        } else if (kind == WorldEntity::Kind::Container) {
            label = "Pojemnik";
        } else {
            continue;              // unnamed item/actor/etc. — skip noise
        }

        WorldEntity e;
        e.kind     = kind;
        e.name     = std::move(label);
        e.position = { r->posX, r->posY, r->posZ };
        e.form_id  = r->refID;
        out.push_back(std::move(e));
    }
}

} // namespace

std::vector<WorldEntity> ScanNearby(int radius, int max_results,
                                    bool actors_only, bool /*hostiles_only*/)
{
    std::vector<WorldEntity> out;
    auto* p = rt::Player();
    if (!p || !p->parentCell) return out;
    Vec3 pp = GetPlayerPosition();
    const float r2 = (float)radius * (float)radius;

    TESObjectCELL* pcell = p->parentCell;
    ScanCellRefs(pcell, pp, r2, p, actors_only, out);

    // Exteriors are split into a grid of cells; the player's parentCell holds
    // only its own square, so a metro door one square over is invisible from
    // it. Walk the worldspace cell map and also scan the loaded cells around
    // the player (objectList non-empty = loaded). Uses only FOSE-defined
    // layouts; heavily guarded.
    auto* ws = reinterpret_cast<UInt8*>(pcell->worldSpace);
    auto* pcoords = reinterpret_cast<UInt8*>(pcell->coords);   // cell+0x44
    if (Readable(ws, 0x34) && Readable(pcoords, 8)) {
        int px = *reinterpret_cast<int*>(pcoords + 0);
        int py = *reinterpret_cast<int*>(pcoords + 4);
        // TESWorldSpace::cellMap @ +0x30 (NiTPointerMap<TESObjectCELL>).
        UInt8* map = *reinterpret_cast<UInt8**>(ws + 0x30);
        if (Readable(map, 0x10)) {
            UInt32 nb = *reinterpret_cast<UInt32*>(map + 0x04);     // m_numBuckets
            UInt8** buckets = *reinterpret_cast<UInt8***>(map + 0x08);
            int scannedCells = 0;
            if (nb > 0 && nb < 1000000 && Readable(buckets, nb * sizeof(void*))) {
                for (UInt32 b = 0; b < nb && scannedCells < 64; ++b) {
                    UInt8* entry = buckets[b];
                    for (int guard = 0; entry && guard < 8192; ++guard) {
                        if (!Readable(entry, 0x0C)) break;
                        // Entry { next@0, key@4, data@8 }
                        auto* cell = *reinterpret_cast<TESObjectCELL**>(entry + 8);
                        entry = *reinterpret_cast<UInt8**>(entry + 0);
                        if (cell == pcell || !Readable(cell, 0xB4)) continue;
                        auto* cc = reinterpret_cast<UInt8*>(cell->coords);
                        if (!Readable(cc, 8)) continue;
                        int cx = *reinterpret_cast<int*>(cc + 0);
                        int cy = *reinterpret_cast<int*>(cc + 4);
                        if (std::abs(cx - px) > 2 || std::abs(cy - py) > 2)
                            continue;
                        ScanCellRefs(cell, pp, r2, p, actors_only, out);
                        ++scannedCells;
                    }
                }
            }
            F3A_DEBUG("ScanNearby: grid cells scanned=%d", scannedCells);
        }
    }

    std::sort(out.begin(), out.end(),
              [&pp](const WorldEntity& a, const WorldEntity& b) {
                  float da = (a.position.x - pp.x) * (a.position.x - pp.x) +
                             (a.position.y - pp.y) * (a.position.y - pp.y);
                  float db = (b.position.x - pp.x) * (b.position.x - pp.x) +
                             (b.position.y - pp.y) * (b.position.y - pp.y);
                  return da < db;
              });
    if ((int)out.size() > max_results) out.resize(max_results);
    return out;
}

const void* GetPlayerCell()
{
    auto* p = rt::Player();
    return p ? (const void*)p->parentCell : nullptr;
}

void SetPlayerYawTo(const Vec3& target)
{
    auto* p = rt::Player();
    if (!p) return;
    float dx = target.x - p->posX;
    float dy = target.y - p->posY;
    // Engine yaw: radians, 0 = north (+Y), increasing clockwise — the same
    // convention ComputeBearing assumes (atan2(dx, dy)).
    p->rotZ = std::atan2(dx, dy);
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

// Which top-level menu does this tile belong to? (typeID of the TileMenu
// that is a direct child of menuRoot on this tile's parent chain.)
UInt32 OwningMenuType(Tile* t)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot || !t) return 0;
    Tile* prev = t;
    for (Tile* p = t->parent; p; p = p->parent) {
        if (p == ifm->menuRoot) {
            auto* tm = reinterpret_cast<TileMenu*>(prev);
            Menu* m = tm->menu;
            return m ? m->typeID : 0;
        }
        prev = p;
    }
    return 0;
}

// The Polish localization labels some keys confusingly in the key-binding
// list — most famously Caps Lock is "KAPSLE" (= bottle caps!). Normalize
// such names, but ONLY for rows inside StartMenu (settings/controls); in
// Barter "Kapsle" legitimately means the currency.
std::string NormalizeKeyNamePart(const std::string& part)
{
    if (_stricmp(part.c_str(), "KAPSLE") == 0) return "Caps Lock";
    return part;
}

std::string NormalizeKeyNames(const std::string& label)
{
    std::string out;
    size_t start = 0;
    while (start <= label.size()) {
        size_t end = label.find(", ", start);
        std::string part = (end == std::string::npos)
                               ? label.substr(start)
                               : label.substr(start, end - start);
        if (!out.empty()) out += ", ";
        out += NormalizeKeyNamePart(part);
        if (end == std::string::npos) break;
        start = end + 2;
    }
    return out;
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

    // In settings/controls (StartMenu) normalize confusing key names
    // ("KAPSLE" → "Caps Lock").
    if (OwningMenuType(listbox) == kMenuType_Start) {
        sel.label = NormalizeKeyNames(sel.label);
        sel.value = NormalizeKeyNames(sel.value);
    }
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

namespace {

// Find the top-level TileMenu of the given engine type, but only if it is
// actually VISIBLE (in gameplay all Pip-Boy pages stay mounted; the active
// page is the one with visible=1 on its menu tile).
Tile* FindVisibleMenuTile(UInt32 menuType)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return nullptr;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child && rt::TileIsVisible(cn->child)) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m && m->typeID == menuType) return cn->child;
        }
        node = node->next;
    }
    return nullptr;
}

// The game marks the ACTIVE tab button of a tab strip by dimming it to
// alpha == 32 (all Pip-Boy sub-tab strips follow this: Status/SPECIAL/...,
// KND/RAD/SKT, the Data page's Local Map/.../Radio). Collect the labels of
// such buttons in the page's subtree.
void CollectActiveTabButtons(Tile* t, int depth, std::string& out)
{
    if (!t || depth > 7) return;
    if (TileNum(t, kTileValue_target) != 0.0f) {
        float a = TileNum(t, kTileValue_alpha);
        if (a >= 24.0f && a <= 48.0f) {
            if (const char* s = TileStringTrait(t)) {
                std::string part = GameStrToUtf8(s);
                if (!part.empty() && out.find(part) == std::string::npos) {
                    if (!out.empty()) out += ", ";
                    out += part;
                }
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectActiveTabButtons(cn->child, depth + 1, out);
        node = node->next;
    }
}

// Read "Title Value" pairs out of a Pip-Boy info box (CI_TitleText +
// CI_ValueText children, one pair per sub-box) — e.g. "PW 156/200".
void CollectInfoPairs(Tile* box, std::string& out)
{
    if (!box) return;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&box->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            Tile* c = cn->child;
            Tile* tt = TileNameHas(c, "TitleText") ? c
                       : FindChildByName(c, "TitleText", 1);
            Tile* vt = TileNameHas(c, "ValueText") ? c
                       : FindChildByName(c, "ValueText", 1);
            const char* ts = tt ? TileStringTrait(tt) : nullptr;
            const char* vs = vt ? TileStringTrait(vt) : nullptr;
            if (ts && vs) {
                if (!out.empty()) out += ", ";
                out += GameStrToUtf8(ts);
                out += " ";
                out += GameStrToUtf8(vs);
            }
        }
        node = node->next;
    }
}

// Collect every visible, non-empty string in a menu subtree, in tree order,
// de-duplicated. Used to read out a whole message box / notification (title,
// body text, and button labels like OK / Boy / Girl) when it opens.
void CollectAllVisibleText(Tile* t, int depth, std::string& out)
{
    if (!t || depth > 8) return;
    if (TileVisible(t)) {
        if (const char* s = TileStringTrait(t)) {
            std::string part = GameStrToUtf8(s);
            if (!part.empty() && out.find(part) == std::string::npos) {
                if (!out.empty()) out += ". ";
                out += part;
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectAllVisibleText(cn->child, depth + 1, out);
        node = node->next;
    }
}

} // namespace

PipBoyTab GetActivePipBoyTab()
{
    // The active page is the VISIBLE one — all three stay mounted.
    if (FindVisibleMenuTile(kMenuType_Inventory)) return PipBoyTab::Items;
    if (FindVisibleMenuTile(kMenuType_Map))       return PipBoyTab::Data;
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

std::string GetActivePipBoySubTabName()
{
    static const UInt32 kPages[] = {
        kMenuType_Stats, kMenuType_Inventory, kMenuType_Map,
    };
    for (UInt32 mt : kPages) {
        if (Tile* page = FindVisibleMenuTile(mt)) {
            std::string out;
            CollectActiveTabButtons(page, 0, out);
            return out;
        }
    }
    return {};
}

std::string GetPipBoyVitals()
{
    Tile* stats = FindVisibleMenuTile(kMenuType_Stats);
    if (!stats) return {};
    std::string out;
    CollectInfoPairs(FindChildByName(stats, "lvl_info", 4), out);
    CollectInfoPairs(FindChildByName(stats, "hp_info", 4), out);
    return out;
}

std::optional<InventoryItem> GetSelectedInventoryItem() { return std::nullopt; }

std::string GetActiveMessageText()
{
    // The plain notification / yes-no / gender-choice popups are Message
    // menus. Read the whole thing: title, body, and button labels.
    Tile* m = FindVisibleMenuTile(kMenuType_Message);
    if (!m) return {};
    std::string out;
    CollectAllVisibleText(m, 0, out);
    return out;
}

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
