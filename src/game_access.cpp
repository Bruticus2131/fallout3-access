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
#include <atomic>
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

float GetPlayerPitch()
{
    // Look pitch in degrees, read-only. Writing rotX doesn't drive the FP camera
    // (the engine does, from the mouse), but READING it reflects the current
    // look pitch — usable as feedback to aim via mouse-Y. Convention TBD by log;
    // returned raw (degrees) so the caller can calibrate sign/zero.
    auto* p = rt::Player();
    if (!p) return 0.0f;
    return p->rotX * 57.2957795f;
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
    t.refr = refr;          // target ref → cross-worldspace check / cell-aware teleport
    Vec3 pos = { refr->posX, refr->posY, refr->posZ };
    if (pos.x != 0.0f || pos.y != 0.0f || pos.z != 0.0f) {
        t.position = pos;   // have a real bearing
    }
    return t;
}

namespace {
// objective + 0x14 -> Target* -> + 0x0C -> TESObjectREFR* -> world pos. Also
// hands back the target reference (for cell-aware MoveTo teleport) and its refID.
bool ObjectiveMarkerPos(UInt8* obj, Vec3& out,
                        const void** out_refr = nullptr,
                        uint32_t* out_id = nullptr)
{
    if (out_refr) *out_refr = nullptr;
    if (out_id)   *out_id   = 0;
    if (IsBadReadPtr(obj + 0x14, 4)) return false;
    UInt8* target = *reinterpret_cast<UInt8**>(obj + 0x14);
    if (IsBadReadPtr(target, 0x10)) return false;
    auto* refr = *reinterpret_cast<TESObjectREFR**>(target + 0x0C);
    if (IsBadReadPtr(refr, 0x38)) return false;
    if (!refr->baseForm) return false;
    Vec3 p = { refr->posX, refr->posY, refr->posZ };
    if (p.x == 0.0f && p.y == 0.0f && p.z == 0.0f) return false;
    out = p;
    if (out_refr) *out_refr = refr;
    if (out_id)   *out_id   = refr->refID;
    return true;
}
} // namespace

const void* GetTrackedQuestPtr()
{
    auto* p = rt::Player();
    if (!p || !p->questObjective) return nullptr;
    auto* obj = reinterpret_cast<UInt8*>(p->questObjective);
    if (IsBadReadPtr(obj, 0x14)) return nullptr;
    // BGSQuestObjective+0x10 = TESQuest* (after displayText String @0x08, before
    // the targets tList @0x14). Verified from an F11 objective dump.
    UInt8* q = *reinterpret_cast<UInt8**>(obj + 0x10);
    if (IsBadReadPtr(q, 0x38)) return nullptr;
    return q;
}

bool AdvanceTrackedQuestStage()
{
    // Skip an inaccessible objective by advancing the tracked quest to its next
    // defined stage (runs that stage's result script, so the quest progresses
    // properly — not just a flag flip). For forced no-VATS / unreachable scenes
    // like the vault BB-gun range. MUST run on the main thread (runs scripts).
    auto* p = reinterpret_cast<UInt8*>(rt::Player());
    if (!p || IsBadReadPtr(p + 0x614, 4)) return false;
    UInt8* q = *reinterpret_cast<UInt8**>(p + 0x614);   // tracked TESQuest*
    if (!q || IsBadReadPtr(q, 0x64)) return false;

    UInt8 cur = *(q + 0x60);   // current stage (GetCurrentStage = movzx [this+0x60])

    // Smallest defined stage greater than the current one (stages tList @ +0x44,
    // each node->item -> StageInfo{ UInt8 stage; ... }).
    struct Node { UInt8* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(q + 0x44);
    int next = -1;
    for (int i = 0; node && i < 512; ++i, node = node->next) {
        if (IsBadReadPtr(node, 8)) break;
        UInt8* si = node->item;
        if (!si || IsBadReadPtr(si, 1)) continue;
        int s = *si;
        if (s > cur && (next < 0 || s < next)) next = s;
    }
    if (next < 0) { F3A_INFO("AdvanceStage: no stage > %u", cur); return false; }

    // Guard the SetStage address by its prologue (53 56 57 8B F9) so a wrong
    // no-gore address no-ops instead of crashing.
    auto* fn = reinterpret_cast<UInt8*>(rt::g_addrs->questSetStage);
    static const UInt8 kSig[] = { 0x53, 0x56, 0x57, 0x8B, 0xF9 };
    if (IsBadReadPtr(fn, sizeof(kSig)) ||
        std::memcmp(fn, kSig, sizeof(kSig)) != 0) {
        F3A_WARN("AdvanceStage: SetStage prologue mismatch; skipping.");
        return false;
    }
    typedef bool(__thiscall* SetStageFn)(void* thisptr, UInt32 stage);
    bool ok = reinterpret_cast<SetStageFn>(fn)(q, (UInt32)next);
    F3A_INFO("AdvanceStage: %u -> %d ok=%d", cur, next, (int)ok);
    return ok;
}

std::string GetTrackedQuestName()
{
    auto* q = reinterpret_cast<const UInt8*>(GetTrackedQuestPtr());
    if (!q) return {};
    const char* nm = *reinterpret_cast<char* const*>(q + 0x34);  // fullName.name
    if (!nm || IsBadReadPtr(nm, 1) || !*nm) return {};
    return GameStrToUtf8(nm);
}

std::vector<WorldEntity> GetActiveQuests()
{
    // List EVERY quest the player currently has, not just the tracked one.
    // (DLC auto-starts quests and steals the tracked slot, spamming the compass
    // and hiding the quest the player cares about — so the scanner must show
    // them all.) Walk DataHandler::questList (@ +0xD4), keep quests that are
    //   * running (q+0x3C != 0), AND
    //   * have a live objective — one flagged DISPLAYED (obj+0x20 bit0) and
    //     NOT COMPLETED (bit1). That flag pair is exactly what the Pip-Boy
    //     journal shows, and it cleanly separates the live journal from the
    //     ~190 dormant/system quest defs.
    std::vector<WorldEntity> out;
    if (!rt::g_addrs) return out;
    auto* gp = reinterpret_cast<UInt8**>(rt::g_addrs->dataHandler);
    if (IsBadReadPtr(gp, 4)) return out;
    UInt8* dh = *gp;
    if (IsBadReadPtr(dh, 0xD8)) return out;

    // The tracked objective (PlayerCharacter+0x618) — so we can float the
    // currently-tracked quest to the front of the list.
    auto* p = rt::Player();
    void* trackedObj = (p && p->questObjective) ? (void*)p->questObjective : nullptr;

    struct Node { UInt8* item; Node* next; };
    Node* node = reinterpret_cast<Node*>(dh + 0xD4);
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (IsBadReadPtr(node, 8)) break;
        UInt8* q = node->item;
        node = node->next;
        if (!q || IsBadReadPtr(q, 0x54)) continue;
        if (*reinterpret_cast<UInt8*>(q + 0x3C) == 0) continue;   // not running
        const char* qnm = *reinterpret_cast<char**>(q + 0x34);    // fullName
        if (IsBadReadPtr(qnm, 1) || !*qnm) continue;

        // Find a live objective (displayed & not completed). Prefer the tracked
        // one if this is the tracked quest; else the last live objective.
        Node* on = reinterpret_cast<Node*>(q + 0x4C);             // objectives @ +0x4C
        UInt8* live = nullptr;
        bool isTracked = false;
        for (int s2 = 0; on && s2 < 64; ++s2) {
            if (IsBadReadPtr(on, 8)) break;
            UInt8* obj = on->item;
            on = on->next;
            if (!obj || IsBadReadPtr(obj, 0x24)) continue;
            UInt32 flags = *reinterpret_cast<UInt32*>(obj + 0x20);
            if ((flags & 1) && !(flags & 2)) {                    // displayed, not done
                live = obj;
                if (obj == trackedObj) isTracked = true;
            }
        }
        if (!live) continue;   // running but nothing live in the journal → skip

        WorldEntity e;
        e.kind = WorldEntity::Kind::Quest;
        std::string name = GameStrToUtf8(qnm);
        const char* otxt = *reinterpret_cast<char**>(live + 0x08);   // objective text
        if (!IsBadReadPtr(otxt, 1) && *otxt) { name += ": "; name += GameStrToUtf8(otxt); }
        e.name = std::move(name);

        Vec3 pos{}; const void* qrefr = nullptr; uint32_t qid = 0;
        if (ObjectiveMarkerPos(live, pos, &qrefr, &qid)) {
            e.position = pos; e.has_marker = true;
            e.refr = qrefr;    // objective target ref → cell-aware MoveTo teleport
            e.form_id = qid;
        } else {
            e.has_marker = false; e.form_id = 0;
        }
        if (isTracked) out.insert(out.begin(), std::move(e));   // tracked quest first
        else           out.push_back(std::move(e));
    }
    return out;
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

// Read a ref's lock by walking its extraDataList manually (memory reads only —
// no native call, so it's safe from the poll thread). Returns -1 = no lock;
// otherwise the lock level (0 Very Easy .. 100 Very Hard, 255 = requires key).
// Layout: TESObjectREFR+0x40 = BaseExtraList; +0x04 = list head; BSExtraData has
// type@+0x04, next@+0x08; ExtraLock (type 0x2A) has Data*@+0x0C → lockLevel@0.
int ReadLockLevel(const TESObjectREFR* r, int* out_flags)
{
    if (out_flags) *out_flags = -1;
    if (!Readable(r, 0x44)) return -1;
    const UInt8* base = reinterpret_cast<const UInt8*>(r);
    const UInt8* node = *reinterpret_cast<const UInt8* const*>(base + 0x44);
    for (int i = 0; node && i < 128; ++i) {
        if (!Readable(node, 0x0C)) break;
        if (node[0x04] == 0x2A) {                       // kExtraData_Lock
            const UInt8* data = *reinterpret_cast<const UInt8* const*>(node + 0x0C);
            if (Readable(data, 0x0C)) {
                if (out_flags) *out_flags = data[0x08];  // ExtraLock::Data::flags
                return (int)*reinterpret_cast<const UInt32*>(data);   // lockLevel
            }
            return -1;
        }
        node = *reinterpret_cast<const UInt8* const*>(node + 0x08);   // next
    }
    return -1;
}

// A load door teleports to a linked door in another cell; tell the player WHERE
// it leads (the destination cell's name — e.g. "Megatona", "Składzik Moiry")
// instead of a bare "Drzwi". Walks ExtraTeleport (kExtraData_Teleport 0x2B) on
// the ref's extra list: node+0x0C = Data*, Data+0x00 = linkedDoor (a
// TESObjectREFR* in the destination cell). The name is that cell's fullName, or
// its worldspace name for exterior doors. Empty if not a load door / unreadable.
// Memory reads only (no native call) so it's safe from the poll thread.
std::string DoorDestinationName(const TESObjectREFR* r)
{
    if (!Readable(r, 0x48)) return {};
    const UInt8* node = *reinterpret_cast<const UInt8* const*>(
                            reinterpret_cast<const UInt8*>(r) + 0x44);
    for (int i = 0; node && i < 128; ++i) {
        if (!Readable(node, 0x10)) break;
        if (node[0x04] == 0x2B) {                        // kExtraData_Teleport
            const UInt8* data = *reinterpret_cast<const UInt8* const*>(node + 0x0C);
            if (!Readable(data, 0x04)) return {};
            auto* linked = *reinterpret_cast<TESObjectREFR* const*>(data);  // linkedDoor
            if (!Readable(linked, 0x40)) return {};
            TESObjectCELL* cell = linked->parentCell;
            if (!Readable(cell, 0x24)) return {};
            const char* nm = SafeCStr(cell->fullName.name.m_data);
            if (nm && *nm) return GameStrToUtf8(nm);
            // Exterior destination: the cell has no name → use the worldspace.
            if (Readable(cell, 0xC4) && cell->worldSpace) {
                TESWorldSpace* ws = cell->worldSpace;
                if (Readable(ws, 0x24)) {
                    const char* wn = SafeCStr(ws->fullName.name.m_data);
                    if (wn && *wn) return GameStrToUtf8(wn);
                }
            }
            return {};
        }
        node = *reinterpret_cast<const UInt8* const*>(node + 0x08);   // next
    }
    return {};
}

// Is a container currently empty? Its live contents = the base default item list
// (TESContainer.formCountList) adjusted by the ref's ExtraContainerChanges deltas
// (what's been looted / added since it spawned). Sum both — base counts are
// positive, a change's countDelta can be negative — and total 0 means empty.
// Returns: 1 = has items, 0 = empty, -1 = couldn't tell (label stays neutral;
// we NEVER claim "empty" on an unreliable read, so real loot is never hidden).
//
// TESObjectCONT isn't laid out in the FOSE headers. BaseFormName puts its
// TESFullName at +0x30, and TESContainer (size 0x0C) sits directly before it, so
// formCountList (a tList head: item@0, next@+0x04) is at +0x28. That +0x28 is an
// INFERENCE — every read is guarded, and the F3A_INFO line lets us confirm it
// against a known-full vs known-empty container in-game. All memory reads (no
// native call), so it's safe from the poll thread.
int ContainerItemState(TESForm* base, const TESObjectREFR* r)
{
    long total  = 0;
    bool baseOk = false;

    if (Readable(base, 0x30)) {
        const UInt8* node = reinterpret_cast<const UInt8*>(base) + 0x28;   // list head
        baseOk = true;
        for (int i = 0; node && i < 256; ++i) {
            if (!Readable(node, 0x08)) { baseOk = false; break; }
            const UInt8* fc = *reinterpret_cast<const UInt8* const*>(node); // FormCount*
            if (fc) {
                if (!Readable(fc, 0x08)) { baseOk = false; break; }
                SInt32 cnt = *reinterpret_cast<const SInt32*>(fc);          // count @0
                if (cnt > 0) total += cnt;
            }
            node = *reinterpret_cast<const UInt8* const*>(node + 0x04);     // next
        }
    }
    if (!baseOk) return -1;   // base unreadable → don't risk a false "empty"

    // ExtraContainerChanges (type 0x15): Data*@+0x0C; Data->objList@+0x00 is a
    // tList<EntryData>*; each EntryData has countDelta @ +0x04.
    if (Readable(r, 0x48)) {
        const UInt8* xn = *reinterpret_cast<const UInt8* const*>(
                              reinterpret_cast<const UInt8*>(r) + 0x44);
        for (int i = 0; xn && i < 128; ++i) {
            if (!Readable(xn, 0x10)) break;
            if (xn[0x04] == 0x15) {                        // kExtraData_ContainerChanges
                const UInt8* data = *reinterpret_cast<const UInt8* const*>(xn + 0x0C);
                if (Readable(data, 0x04)) {
                    const UInt8* list = *reinterpret_cast<const UInt8* const*>(data);
                    for (int j = 0; Readable(list, 0x08) && j < 512; ++j) {
                        const UInt8* ed = *reinterpret_cast<const UInt8* const*>(list);
                        if (ed && Readable(ed, 0x08))
                            total += *reinterpret_cast<const SInt32*>(ed + 0x04);
                        list = *reinterpret_cast<const UInt8* const*>(list + 0x04);
                    }
                }
                break;
            }
            xn = *reinterpret_cast<const UInt8* const*>(xn + 0x08);
        }
    }

    return total > 0 ? 1 : 0;
}

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

        // Load doors: append/replace with WHERE the door leads so the player
        // hears "Drzwi do: Składzik Moiry", not a bare "Drzwi". A named local
        // door (e.g. "Drzwi Krypty 101") keeps its name plus the destination.
        if (kind == WorldEntity::Kind::Door) {
            std::string dest = DoorDestinationName(r);
            if (!dest.empty()) {
                label = (nm && label != dest) ? label + ", do: " + dest
                                              : "Drzwi do: " + dest;
            }
        }

        // Containers: tell the player if it's empty so they don't bother opening
        // it. Only tag "pusty" when we're sure — a -1 (unknown) keeps the plain
        // name so real loot is never hidden behind a false "empty".
        if (kind == WorldEntity::Kind::Container) {
            int st = ContainerItemState(base, r);
            F3A_INFO("Cont: '%s' state=%d (1=full 0=empty -1=?)", label.c_str(), st);
            if (st == 0) label += ", pusty";
        }

        WorldEntity e;
        e.kind     = kind;
        e.name     = std::move(label);
        e.position = { r->posX, r->posY, r->posZ };
        e.form_id  = r->refID;
        e.refr     = r;            // for crosshair-forced activation
        if (kind == WorldEntity::Kind::Door ||
            kind == WorldEntity::Kind::Container) {
            int flags = -1;
            int lvl = ReadLockLevel(r, &flags);
            if (lvl >= 0) {
                e.lock_level = lvl;
                e.locked = (flags & 0x01) != 0;   // guess: bit 0 = currently locked
                // TEMP diagnostic to nail the "locked" bit: compare a known-locked
                // vs a known-unlocked door in the log.
                F3A_INFO("Lock: '%s' level=%d flags=0x%02X -> locked=%d",
                         e.name.c_str(), lvl, flags, (int)e.locked);
            }
        }
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

std::vector<WorldEntity> GetShootingTargets(int radius)
{
    // Targets of a "shoot the targets" objective (e.g. the vault BB-gun range)
    // are unnamed refs the scanner skips, but the quest marks ONE of them. The
    // others are placements of the SAME base object — so we read the marked
    // ref's base form and return every ref in the cell sharing it. That gives
    // the full set (e.g. all three targets), nearest first, so the aim key can
    // cycle them. Empty if there's no marked target with a live ref.
    std::vector<WorldEntity> out;
    auto* p = rt::Player();
    if (!p || !p->questObjective || !p->parentCell) return out;
    auto* obj = reinterpret_cast<UInt8*>(p->questObjective);
    if (IsBadReadPtr(obj + 0x14, 4)) return out;
    UInt8* target = *reinterpret_cast<UInt8**>(obj + 0x14);
    if (IsBadReadPtr(target, 0x10)) return out;
    auto* mref = *reinterpret_cast<TESObjectREFR**>(target + 0x0C);
    if (!Readable(mref, 0x40)) return out;
    TESForm* mbase = mref->baseForm;
    if (!Readable(mbase, 0x08)) return out;

    Vec3 pp = GetPlayerPosition();
    const float r2 = (float)radius * (float)radius;
    TESObjectCELL* cell = p->parentCell;
    if (!Readable(cell, 0xB4)) return out;
    struct Node { TESObjectREFR* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&cell->objectList);
    for (int safety = 0; node && safety < 16384; ++safety) {
        if (!Readable(node, 8)) break;
        TESObjectREFR* r = node->item;
        node = node->next;
        if (!r || !Readable(r, 0x40)) continue;
        if (r->flags & 0x820) continue;            // disabled / deleted
        if (r->baseForm != mbase) continue;        // not the same target object
        float dx = r->posX - pp.x, dy = r->posY - pp.y;
        if (dx * dx + dy * dy > r2) continue;
        WorldEntity e;
        e.kind       = WorldEntity::Kind::Item;
        e.name       = "Cel";
        e.position   = { r->posX, r->posY, r->posZ };
        e.has_marker = true;
        e.form_id    = r->refID;
        e.refr       = r;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [&pp](const WorldEntity& a, const WorldEntity& b) {
                  float da = (a.position.x-pp.x)*(a.position.x-pp.x) +
                             (a.position.y-pp.y)*(a.position.y-pp.y);
                  float db = (b.position.x-pp.x)*(b.position.x-pp.x) +
                             (b.position.y-pp.y)*(b.position.y-pp.y);
                  return da < db;
              });
    return out;
}

std::vector<std::string> DebugNearbyRefs(int radius)
{
    // Diagnostic: list EVERY reference in the player's cell within `radius`,
    // unfiltered by type or name — including the statics/activators the normal
    // scanner skips. Used to identify "what is there to shoot" when VATS and the
    // scanner find nothing. Sorted nearest-first.
    std::vector<std::string> lines;
    auto* p = rt::Player();
    if (!p || !p->parentCell) { lines.push_back("brak komorki"); return lines; }
    Vec3 pp = GetPlayerPosition();
    const float r2 = (float)radius * (float)radius;
    TESObjectCELL* cell = p->parentCell;
    if (!Readable(cell, 0xB4)) { lines.push_back("komorka nieczytelna"); return lines; }

    struct Row { float dist; std::string text; };
    std::vector<Row> rows;
    struct Node { TESObjectREFR* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&cell->objectList);
    int total = 0;
    for (int safety = 0; node && safety < 16384; ++safety) {
        if (!Readable(node, 8)) break;
        TESObjectREFR* r = node->item;
        node = node->next;
        if (!r || !Readable(r, 0x68)) continue;
        ++total;
        TESForm* base = r->baseForm;
        if (!Readable(base, 0x08)) continue;
        float dx = r->posX - pp.x, dy = r->posY - pp.y, dz = r->posZ - pp.z;
        float d2 = dx * dx + dy * dy;
        if (d2 > r2) continue;
        const char* nm = BaseFormName(base);
        std::string name = (nm && !IsBadReadPtr(nm, 1)) ? GameStrToUtf8(nm)
                                                        : std::string();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "0x%08X typ=%u dist=%.0f dz=%.0f flags=0x%X '%.60s'",
                      r->refID, base->typeID, std::sqrt(d2), dz,
                      r->flags, name.c_str());
        rows.push_back({ std::sqrt(d2), buf });
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b){ return a.dist < b.dist; });
    char hdr[96];
    std::snprintf(hdr, sizeof(hdr), "%d refs w komorce, %d w promieniu %d",
                  total, (int)rows.size(), radius);
    lines.push_back(hdr);
    for (size_t i = 0; i < rows.size() && i < 150; ++i)
        lines.push_back(rows[i].text);
    return lines;
}

uint32_t GetCrosshairRefID(uint32_t* out_type)
{
    if (out_type) *out_type = 0;
    auto* ifm = rt::IFM();
    if (!ifm) return 0;
    UInt8* b = reinterpret_cast<UInt8*>(ifm);
    if (IsBadReadPtr(b + 0xFC, 4)) return 0;
    auto* refr = *reinterpret_cast<TESObjectREFR**>(b + 0xFC);
    if (!refr || IsBadReadPtr(refr, 0x40)) return 0;
    if (out_type) {
        TESForm* bf = refr->baseForm;
        if (Readable(bf, 0x08)) *out_type = bf->typeID;
    }
    return refr->refID;
}

// What the camera is pointing at: name + distance + whether it's an actor. The
// engine keeps the picked reference at InterfaceManager+0xFC, so this is the
// same source the HUD activation prompt uses — but it also resolves ENEMIES and
// anything else that has no "activate" verb, which the prompt tile alone misses.
// (The FNV mod reads its HUD's own crosshair-ref field for this.) Returns false
// when nothing is picked.
// The engine Menu object for a menu type (not its root tile) — needed to read a
// menu's own fields and to call its HandleClick. Menus hang off menuRoot as
// TileMenu children, each carrying its Menu*.
Menu* FindMenuByType(UInt32 type)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return nullptr;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m && !IsBadReadPtr(m, 0x20) && m->typeID == type) return m;
        }
        node = node->next;
    }
    return nullptr;
}

// The tile helpers are file-local and defined further down; declare them here so
// the map-marker code below can use them.
namespace {
Tile*       FindVisibleMenuTile(UInt32 menuType);
Tile*       FindChildByName(Tile* t, const char* needle, int depth);
float       TileNum(const Tile* t, UInt32 id);
float       TileAbsX(Tile* t);
float       TileAbsY(Tile* t);
bool        TileVisible(const Tile* t);
bool        TileNameHas(const Tile* t, const char* needle);
const char* TileStringTrait(const Tile* t);
void        CollectAllVisibleText(Tile* t, int depth, std::string& out);
}

// ---- World map markers -----------------------------------------------------
//
// The map screen keeps the markers it draws in an inline tList<TESObjectREFR*>
// inside its Menu object. The FNV mod reads that list at a hard-coded offset,
// but FO3's MapMenu layout is its own — so instead of guessing, PROBE the menu:
// treat each 4-byte slot as a tList head and accept the one whose entries are
// readable references carrying an ExtraMapMarker. That validates on CONTENT, so
// a wrong guess can't silently produce garbage. The winning offset is cached.
namespace {

constexpr UInt32 kExtraMapMarkerType = 0x2C;      // kExtraData_MapMarker

// The ExtraMapMarker attached to a reference, or null.
const UInt8* MapMarkerExtra(TESObjectREFR* refr)
{
    if (!refr || IsBadReadPtr(refr, 0x54)) return nullptr;
    // ExtraDataList at refr+0x40: m_vtbl, then m_data = first BSExtraData.
    auto* xd = *reinterpret_cast<UInt8**>(reinterpret_cast<UInt8*>(refr) + 0x40 + 4);
    for (int i = 0; xd && i < 64; ++i) {
        if (IsBadReadPtr(xd, 0x10)) return nullptr;
        UInt8 type = *(xd + 4);                       // BSExtraData::type
        if (type == kExtraMapMarkerType) return xd;
        xd = *reinterpret_cast<UInt8**>(xd + 8);      // ::next
    }
    return nullptr;
}

// MarkerData begins with a TESFullName (vtable, then the String) — so the name
// is a char* at +0x04. Flags/type follow it; we read them defensively and let
// the caller treat them as advisory.
bool ReadMarkerData(const UInt8* extra, std::string* name, uint16_t* type,
                    uint16_t* flags)
{
    if (!extra) return false;
    auto* md = *reinterpret_cast<UInt8* const*>(extra + 0x0C);   // MarkerData*
    if (!md || IsBadReadPtr(md, 0x10)) return false;
    const char* s = *reinterpret_cast<const char* const*>(md + 0x04);
    if (!s || IsBadReadPtr((void*)s, 1) || !*s) return false;
    // Sanity: map names are short printable text, never binary.
    for (const char* p = s; *p; ++p) {
        if (p - s > 96) return false;
        if ((unsigned char)*p < 0x20) return false;
    }
    if (name)  *name  = GameStrToUtf8(s);
    if (flags) *flags = *reinterpret_cast<const uint16_t*>(md + 0x08);
    if (type)  *type  = *reinterpret_cast<const uint16_t*>(md + 0x0A);
    return true;
}

struct RefNode { TESObjectREFR* item; RefNode* next; };

// How many leading entries of this candidate list are real map markers?
int CountMarkerEntries(const UInt8* head)
{
    if (IsBadReadPtr((void*)head, 8)) return 0;
    int good = 0;
    auto* node = reinterpret_cast<const RefNode*>(head);
    for (int i = 0; node && i < 512; ++i) {
        if (node->item) {
            if (!MapMarkerExtra(node->item)) return good;   // a non-marker → not the list
            ++good;
        }
        if (IsBadReadPtr((void*)&node->next, 4)) break;
        node = node->next;
        if (node && IsBadReadPtr((void*)node, 8)) break;
    }
    return good;
}

std::vector<MapMarker> g_marker_cache;

// The world map draws one tile per location, NAMED after the location, under
// MM_WorldMap_ParentImage — e.g. [Megatona] at x=1133 y=1367. A `user0` trait of
// 1 marks the ones the player may travel to (confirmed against a live dump:
// visited Megaton/Vault 101 had it, merely-heard-of Arefu/Super-Duper Mart did
// not). Reading these gives us both a reliable discovered flag and a click
// target for real fast travel. The quest arrow and the route icons are not
// locations, so they're skipped.
struct MapTile { std::string name; float x, y; bool can_travel; Tile* tile; };

void CollectWorldMapTiles(Tile* t, int depth, std::vector<MapTile>& out)
{
    if (!t || depth > 12) return;
    if (TileNameHas(t, "MM_WorldMap_ParentImage")) {
        struct Node { Tile::ChildNode* item; Node* next; };
        auto* node = reinterpret_cast<Node*>(&t->childList);
        for (int i = 0; node && i < 4096; ++i) {
            Tile::ChildNode* cn = node->item;
            if (cn && cn->child) {
                Tile* c = cn->child;
                const char* nm = c->name.m_data;
                if (nm && *nm &&
                    !TileNameHas(c, "QuestMarker") &&
                    !TileNameHas(c, "WaypointIcon") &&
                    !TileNameHas(c, "PathLine")) {
                    out.push_back({ GameStrToUtf8(nm), TileAbsX(c), TileAbsY(c),
                                    TileNum(c, kTileValue_user0) != 0.0f, c });
                }
            }
            node = node->next;
        }
        return;                      // found the map layer; no need to go deeper
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectWorldMapTiles(cn->child, depth + 1, out);
        node = node->next;
    }
}

} // namespace

std::vector<MapMarker> GetMapMarkers()
{
    std::vector<MapMarker> out;
    Menu* menu = FindMenuByType(kMenuType_Map);
    if (!menu) return out;

    static int s_offset = -1;         // cached list offset within MapMenu
    const UInt8* base = reinterpret_cast<const UInt8*>(menu);

    if (s_offset < 0) {
        int bestOff = -1, bestCount = 2;      // need >2 entries to be convincing
        for (int off = 0x18; off <= 0x300; off += 4) {
            int n = CountMarkerEntries(base + off);
            if (n > bestCount) { bestCount = n; bestOff = off; }
        }
        if (bestOff < 0) {
            // The list is empty for the first frames after the map opens, so this
            // is normal at first — log it once per menu instance, not per tick.
            static const Menu* s_warned = nullptr;
            if (s_warned != menu) {
                s_warned = menu;
                F3A_INFO("MapMarkers: no marker list yet in MapMenu %p", menu);
            }
            return out;
        }
        s_offset = bestOff;   // FO3 1.7.0.3 lands on +0xC8 (FNV's mod uses 0xD8)
        F3A_INFO("MapMarkers: list at MapMenu+0x%X (%d entries)", bestOff, bestCount);
    }

    auto* p = rt::Player();
    auto* node = reinterpret_cast<const RefNode*>(base + s_offset);
    for (int i = 0; node && i < 512; ++i) {
        TESObjectREFR* refr = node->item;
        if (refr) {
            if (const UInt8* extra = MapMarkerExtra(refr)) {
                MapMarker m;
                if (ReadMarkerData(extra, &m.name, &m.type, &m.flags)) {
                    m.refr    = refr;
                    m.form_id = refr->refID;
                    m.pos     = { refr->posX, refr->posY, refr->posZ };
                    // MarkerData flags: bit0 = shown on map, bit1 = can fast travel.
                    m.discovered = (m.flags & 0x02) != 0 || (m.flags & 0x01) != 0;
                    if (p) {
                        float dx = m.pos.x - p->posX, dy = m.pos.y - p->posY;
                        m.dist = std::sqrt(dx * dx + dy * dy);
                    }
                    if (!m.name.empty()) out.push_back(std::move(m));
                }
            }
        }
        node = node->next;
        if (node && IsBadReadPtr((void*)node, 8)) break;
    }

    // Match each marker to its on-screen tile by name. The tile is the better
    // source for "can I travel there": it's what the game itself draws, whereas
    // the MarkerData flag bits are inferred (and read Arefu as discovered when
    // it wasn't).
    std::vector<MapTile> tiles;
    if (Tile* mt = FindVisibleMenuTile(kMenuType_Map))
        CollectWorldMapTiles(mt, 0, tiles);
    for (auto& m : out) {
        for (const auto& t : tiles) {
            if (t.name != m.name) continue;
            m.has_tile   = true;
            m.screen_x   = t.x;
            m.screen_y   = t.y;
            m.discovered = t.can_travel;
            m.tile       = t.tile;
            break;
        }
    }

    std::sort(out.begin(), out.end(),
              [](const MapMarker& a, const MapMarker& b) { return a.dist < b.dist; });
    if (!out.empty()) g_marker_cache = out;      // keep for planning during gameplay
    return out;
}

const std::vector<MapMarker>& CachedMapMarkers() { return g_marker_cache; }

// The world map's cursor position on screen — feedback for driving the mouse
// onto a marker, exactly like the hacking grid's cursor.
bool GetMapCursor(float* x, float* y)
{
    Tile* m = FindVisibleMenuTile(kMenuType_Map);
    if (!m) return false;
    Tile* c = FindChildByName(m, "MM_WorldMapCursor", 10);
    if (!c || !TileVisible(c)) return false;
    if (x) *x = TileAbsX(c) + TileNum(c, kTileValue_width) * 0.5f;
    if (y) *y = TileAbsY(c) + TileNum(c, kTileValue_height) * 0.5f;
    return true;
}

// The objectives listed for the quest selected in the Pip-Boy's Quests tab.
// Selecting a quest only speaks its NAME through the generic list reader; the
// objectives sit in a separate list beside it, and that is the part that
// actually tells you what to do.
std::string GetQuestObjectivesText()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Map);
    if (!m) return {};
    Tile* list = FindChildByName(m, "MM_QuestObjectivesList", 8);
    if (!list) return {};
    std::string out;
    CollectAllVisibleText(list, 0, out);
    const char* kWs = " \t\n\r";
    size_t b = out.find_first_not_of(kWs);
    if (b == std::string::npos) return {};
    return out.substr(b, out.find_last_not_of(kWs) - b + 1);
}

// The "Ile?" quantity prompt: the amount currently chosen and the stack size.
//
// The amount is read from QM_AmountChosen — literally the number on screen. The
// maximum is the slider's user0 trait; that is the trait the FNV accessibility
// mod reads for the same prompt, and it replaces an earlier guess of ours that
// produced nonsense ("7 z 5"). The amount is clamped to it, since a stale text
// tile must never read as more than the stack holds.
bool GetQuantityState(int* amount, int* maximum)
{
    Tile* m = FindVisibleMenuTile(kMenuType_Quantity);
    if (!m) return false;

    int mx = 0;
    if (Tile* meter = FindChildByName(m, "QM_AmountMeter", 6))
        mx = (int)TileNum(meter, kTileValue_user0);

    int amt = -1;
    if (Tile* chosen = FindChildByName(m, "QM_AmountChosen", 6)) {
        if (const char* t = TileStringTrait(chosen)) {
            int v = std::atoi(t);
            if (v > 0) amt = v;
        }
    }
    if (amt < 0) return false;
    if (mx > 0 && amt > mx) amt = mx;

    if (amount)  *amount  = amt;
    if (maximum) *maximum = mx;
    return true;
}

// Screen position of a named tile's centre inside a menu — for driving the game
// cursor onto it and clicking like a player would.
bool GetMenuTileCenter(uint32_t menuType, const char* tileName, float* x, float* y)
{
    Tile* m = FindVisibleMenuTile(menuType);
    if (!m) return false;
    Tile* t = FindChildByName(m, tileName, 8);
    if (!t) return false;
    if (x) *x = TileAbsX(t) + TileNum(t, kTileValue_width) * 0.5f;
    if (y) *y = TileAbsY(t) + TileNum(t, kTileValue_height) * 0.5f;
    return true;
}

// Where the game's own menu cursor is (InterfaceManager::cursor) — the feedback
// signal for steering the mouse onto a tile.
bool GetMenuCursorPos(float* x, float* y)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->cursor) return false;
    Tile* c = ifm->cursor;
    if (IsBadReadPtr(c, 0x40)) return false;
    if (x) *x = TileAbsX(c);
    if (y) *y = TileAbsY(c);
    return true;
}

bool GetRefPosition(const void* refr, uint32_t expectedRefID, Vec3* out)
{
    if (!refr || !out) return false;
    auto* r = reinterpret_cast<TESObjectREFR*>(const_cast<void*>(refr));
    if (IsBadReadPtr(r, 0x40)) return false;
    if (expectedRefID && r->refID != expectedRefID) return false;   // pointer reused
    out->x = r->posX; out->y = r->posY; out->z = r->posZ;
    return true;
}

bool GetCrosshairTarget(CrosshairTarget* out)
{
    if (!out) return false;
    *out = CrosshairTarget{};
    uint32_t type = 0;
    std::string nm = GetCrosshairRefName(&out->refid, &type);   // handles the name fallbacks
    if (!out->refid) return false;
    out->name     = std::move(nm);
    out->is_actor = KindOf(type) == WorldEntity::Kind::Actor;

    auto* ifm = rt::IFM();
    auto* p   = rt::Player();
    if (ifm && p) {
        UInt8* b = reinterpret_cast<UInt8*>(ifm);
        if (!IsBadReadPtr(b + 0xFC, 4)) {
            auto* refr = *reinterpret_cast<TESObjectREFR**>(b + 0xFC);
            if (refr && !IsBadReadPtr(refr, 0x40) && refr->refID == out->refid) {
                float dx = refr->posX - p->posX, dy = refr->posY - p->posY,
                      dz = refr->posZ - p->posZ;
                out->dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
    }
    return true;
}

uint32_t GetCrosshairActorID()
{
    // refID of the crosshair ref IF it's an actor (NPC/Creature), else 0 — for
    // the line-of-sight aim cue. (crosshairRef only covers refs within the
    // engine's pick range, so this fires for close/medium targets.)
    auto* ifm = rt::IFM();
    if (!ifm) return 0;
    UInt8* b = reinterpret_cast<UInt8*>(ifm);
    if (IsBadReadPtr(b + 0xFC, 4)) return 0;
    auto* refr = *reinterpret_cast<TESObjectREFR**>(b + 0xFC);
    if (!refr || IsBadReadPtr(refr, 0x40)) return 0;
    TESForm* bf = refr->baseForm;
    if (!Readable(bf, 0x08)) return 0;
    if (KindOf(bf->typeID) != WorldEntity::Kind::Actor) return 0;
    return refr->refID;
}

// ---- Native activation on the main thread --------------------------------
namespace {
typedef bool (__thiscall* ActivateFn)(void* thisRef, void* activator,
                                      UInt32 a2, UInt32 a3, UInt32 a4);

// Call TESObjectREFR::Activate(this=refr, activator=player, 0,0,1). Guarded by a
// prologue-bytes check so a wrong address (e.g. an unverified runtime) can't
// crash the game — it simply does nothing.
bool ActivateRefNative(const void* refr, uint32_t expectedRefID)
{
    UInt32 addr = rt::g_addrs->refrActivate;
    if (!addr || IsBadReadPtr(reinterpret_cast<void*>(addr), 6)) return false;
    const UInt8 prologue[6] = { 0x81, 0xEC, 0x14, 0x01, 0x00, 0x00 }; // sub esp,0x114
    if (std::memcmp(reinterpret_cast<void*>(addr), prologue, 6) != 0) return false;
    if (!refr || IsBadReadPtr(refr, 0x40)) return false;
    if (reinterpret_cast<const TESObjectREFR*>(refr)->refID != expectedRefID)
        return false;                                  // stale / reused pointer
    auto* player = rt::Player();
    if (!player) return false;
    reinterpret_cast<ActivateFn>(addr)(const_cast<void*>(refr), player, 0, 0, 1);
    return true;
}

// Pending activation, set from the poll thread, drained on the main thread.
std::atomic<const void*> g_pending_refr{ nullptr };
std::atomic<uint32_t>    g_pending_id{ 0 };

// Per-frame callback run on the MAIN thread (so it can call game code safely):
// the poll loop registers its main-thread work here.
std::atomic<void(*)()>   g_mainThreadCb{ nullptr };

typedef LRESULT (WINAPI* DispatchMessageA_t)(const MSG*);
DispatchMessageA_t g_realDispatchMessageA = nullptr;

LRESULT WINAPI DispatchMessageA_Hook(const MSG* msg)
{
    // Runs on the main (message-pump) thread — safe to call game code here.
    const void* refr = g_pending_refr.exchange(nullptr);
    if (refr) ActivateRefNative(refr, g_pending_id.load());
    if (auto cb = g_mainThreadCb.load()) cb();
    return g_realDispatchMessageA ? g_realDispatchMessageA(msg)
                                  : ::DispatchMessageA(msg);
}

// Swap an IAT entry (pointer write only — no code patching, low risk).
void* HookIAT(const char* dll, const char* func, void* newFn)
{
    auto* mod = reinterpret_cast<UInt8*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(mod + dos->e_lfanew);
    UInt32 rva = nt->OptionalHeader
                   .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return nullptr;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(mod + rva);
    for (; desc->Name; ++desc) {
        const char* name = reinterpret_cast<const char*>(mod + desc->Name);
        if (_stricmp(name, dll) != 0) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(mod + desc->FirstThunk);
        auto* orig  = reinterpret_cast<IMAGE_THUNK_DATA*>(
            mod + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                            : desc->FirstThunk));
        for (; thunk->u1.Function; ++thunk, ++orig) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                mod + orig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), func) != 0)
                continue;
            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD old = 0;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            void* prev = *slot;
            *slot = newFn;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}
} // namespace

void InstallMainThreadHook()
{
    void* prev = HookIAT("user32.dll", "DispatchMessageA",
                         reinterpret_cast<void*>(&DispatchMessageA_Hook));
    g_realDispatchMessageA = reinterpret_cast<DispatchMessageA_t>(prev);
    F3A_INFO("Main-thread hook (DispatchMessageA IAT) %s.",
             prev ? "installed" : "FAILED");
}

void QueueActivate(const void* refr, uint32_t expectedRefID)
{
    g_pending_id.store(expectedRefID);
    g_pending_refr.store(refr);
}

void SetMainThreadCallback(void (*cb)())
{
    g_mainThreadCb.store(cb);
}

bool ForceCrosshairRef(const void* refr, uint32_t expectedRefID)
{
    if (!refr || IsBadReadPtr(refr, 0x40)) return false;
    auto* r = reinterpret_cast<const TESObjectREFR*>(refr);
    if (r->refID != expectedRefID) return false;   // stale / reused pointer
    auto* ifm = rt::IFM();
    if (!ifm) return false;
    UInt8* b = reinterpret_cast<UInt8*>(ifm);
    if (IsBadReadPtr(b + 0xFC, 4)) return false;
    *reinterpret_cast<const void**>(b + 0xFC) = refr;   // crosshairRef
    return true;
}

std::string GetCrosshairRefName(uint32_t* out_form_id, uint32_t* out_type_id)
{
    if (out_form_id) *out_form_id = 0;
    if (out_type_id) *out_type_id = 0;
    auto* ifm = rt::IFM();
    if (!ifm) return {};
    UInt8* base = reinterpret_cast<UInt8*>(ifm);
    if (IsBadReadPtr(base + 0xFC, 4)) return {};
    auto* refr = *reinterpret_cast<TESObjectREFR**>(base + 0xFC);  // crosshairRef
    if (!refr || IsBadReadPtr(refr, 0x40)) return {};
    if (out_form_id) *out_form_id = refr->refID;

    TESForm* bf = refr->baseForm;
    if (!Readable(bf, 0x08)) return std::string("obiekt");
    if (out_type_id) *out_type_id = bf->typeID;
    const char* nm = BaseFormName(bf);
    if (nm) return GameStrToUtf8(nm);
    switch (KindOf(bf->typeID)) {
        case WorldEntity::Kind::Door:      return "drzwi";
        case WorldEntity::Kind::Container: return "pojemnik";
        case WorldEntity::Kind::Actor:     return "postać";
        default:                           return "obiekt";
    }
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

namespace {
// Locate a Setting struct by name across both INI collections (returns the
// Setting*, whose float value is at +0x04), or nullptr.
// IniSettingCollection: vtbl@0, iniPath[0x100]@4, +0x104, +0x108,
// tList<Setting> settings @ +0x10C. Setting: vtbl@0, Info data@0x04 (float f),
// char* name @0x08. Names may carry a ":Section" suffix, so match the prefix.
UInt8* FindIniSetting(const char* name)
{
    if (!name || !*name) return nullptr;
    const size_t nlen = std::strlen(name);
    const UInt32 colls[2] = { rt::g_addrs->iniSettingColl,
                              rt::g_addrs->iniPrefColl };
    for (UInt32 caddr : colls) {
        if (!caddr) continue;
        auto** pp = reinterpret_cast<UInt8**>(caddr);
        if (IsBadReadPtr(pp, 4)) continue;
        UInt8* coll = *pp;
        if (IsBadReadPtr(coll, 0x114)) continue;
        struct Node { UInt8* item; Node* next; };
        Node* node = reinterpret_cast<Node*>(coll + 0x10C);
        for (int s = 0; node && s < 40000; ++s) {
            if (IsBadReadPtr(node, 8)) break;
            UInt8* set = node->item;
            node = node->next;
            if (!set || IsBadReadPtr(set, 0x0C)) continue;
            char* nm = *reinterpret_cast<char**>(set + 0x08);
            if (!nm || IsBadReadPtr(nm, nlen + 1)) continue;
            if (_strnicmp(nm, name, nlen) == 0 &&
                (nm[nlen] == '\0' || nm[nlen] == ':'))
                return set;
        }
    }
    return nullptr;
}
} // namespace

float GetPlayerAV(uint32_t avCode)
{
    // GetActorValue = actor->avOwner(@+0x9C)->vtable[3](avCode), returning a
    // float (RE'd from the GetActorValue command handler). Reading the vtable
    // at runtime makes this version-independent. A read-only call — safe enough
    // off the main thread (no state change / menu).
    auto* p = rt::Player();
    if (!p) return -1.0f;
    UInt8* avOwner = reinterpret_cast<UInt8*>(p) + 0x9C;
    if (IsBadReadPtr(avOwner, 4)) return -1.0f;
    void** vtbl = *reinterpret_cast<void***>(avOwner);
    if (IsBadReadPtr(vtbl, 0x10) || IsBadReadPtr(vtbl[3], 1)) return -1.0f;
    typedef float (__thiscall* GetAVFn)(void* thisptr, UInt32 code);
    return reinterpret_cast<GetAVFn>(vtbl[3])(avOwner, avCode);
}

bool IsThirdPerson()
{
    auto* p = rt::Player();
    if (!p) return false;
    UInt8* base = reinterpret_cast<UInt8*>(p);
    if (IsBadReadPtr(base + 0x5A8, 1)) return false;
    return *(base + 0x5A8) != 0;   // PlayerCharacter::bThirdPerson
}

// PlayerCharacter::disabledControlFlags @ +0x5DC — non-zero during scripted
// scenes (char creation, cutscenes, forced dialogue) where the player has no
// free control. Used to suppress gameplay cues (the LOS blip) in those states.
bool ArePlayerControlsDisabled()
{
    auto* p = rt::Player();
    if (!p) return false;
    UInt8* base = reinterpret_cast<UInt8*>(p);
    if (IsBadReadPtr(base + 0x5DC, 4)) return false;
    return *reinterpret_cast<UInt32*>(base + 0x5DC) != 0;
}

void SetPlayerLook(float yawRad, float pitchRad)
{
    auto* p = rt::Player();
    if (!p) return;
    p->rotZ = yawRad;     // heading (same field SetPlayerYawTo writes)
    p->rotX = pitchRad;   // look pitch (SkyrimAccessMod's SetLooking equiv.)
}

void AimLookAtBiased(const Vec3& target, float pitchBiasRad)
{
    auto* p = rt::Player();
    if (!p) return;
    float dx = target.x - p->posX;
    float dy = target.y - p->posY;
    float horiz = std::sqrt(dx * dx + dy * dy);
    float yaw = std::atan2(dx, dy);          // 0 = +Y north, clockwise
    // The player origin is at the feet; the eye/weapon sits ~100 units above.
    // Pitch is the angle from the eye to the target. In the Gamebryo look
    // convention positive rotX pitches the view DOWN, so a target below eye
    // level (dz < 0) needs a positive pitch. The caller can add a bias to sweep
    // the vertical aim when the exact eye height / ref origin is uncertain.
    const float kEyeHeight = 100.0f;
    float dz = target.z - (p->posZ + kEyeHeight);
    float pitch = std::atan2(-dz, horiz > 1.0f ? horiz : 1.0f) + pitchBiasRad;
    SetPlayerLook(yaw, pitch);
}

void AimLookAt(const Vec3& target) { AimLookAtBiased(target, 0.0f); }

// ---- Native engine SetAngle (the friend's lead) ---------------------------
//
// `Player.SetAngle X/Z` runs 0x00522B50 SetAngleComponent(ref, axisChar, deg).
// Unlike our raw rotX/rotZ field write (which the first-person camera ignored),
// it: (a) writes rot[XYZ] at +0x20/+0x24/+0x28 via the per-axis setter, (b) that
// setter tail-calls vtable+0x48 (post-rotation update), and (c) 0x522B50 then
// runs the full 3D node-transform update. Those follow-ups are exactly what our
// naked write skipped — the suspected reason the camera never moved. MUST run on
// the MAIN thread (touches the render node). axisAscii = 'X' (pitch) or 'Z'
// (yaw, 0 = north/+Y). Per GECK wiki SetAngle X is INVERTED: negative = look up.
void SetPlayerAngleDeg(int axisAscii, float degrees)
{
    auto* p = rt::Player();
    if (!p) return;
    // Guard the hardcoded address: confirm the prologue (sub esp,0x30; push ebp
    // = 83 EC 30 55) before calling, in case of an unexpected build.
    const unsigned char* code = reinterpret_cast<const unsigned char*>(0x00522B50);
    if (IsBadReadPtr((void*)code, 4) ||
        code[0] != 0x83 || code[1] != 0xEC || code[2] != 0x30 || code[3] != 0x55)
        return;
    using PFN = char (__cdecl*)(void* ref, int axis, float deg);
    reinterpret_cast<PFN>(0x00522B50)(p, axisAscii, degrees);
}

// ---- Walk animation --------------------------------------------------------
//
// Driving the player with Actor::Move moves the body but plays no animation, and
// the game hangs its FOOTSTEP SOUNDS off the walk animation — so an autowalk was
// both visually frozen and silent. These are the calls the game's own `PlayGroup`
// script command makes, read straight out of its handler at 0x00532690:
//
//   seq      = 0x6FCB20(actor, animGroup, animData_or_null, 0, 0)   ret 0x10
//   animData = actor->vtable[0x1E4](actor)
//              0x461690(animData, seq, flags, blend, blend)          ret 0x10
//   active   = *(void**)(animData + 0xE4)
//              0x6FF040(actor, 0xE, active)                          ret 8
//
// The argument counts come from each function's `ret N`, and they match the FNV
// accessibility mod's equivalents one for one, which is a good independent check.
// Anim groups are the engine's own ids: 0 = Idle, 3 = Forward (walk),
// 7 = FastForward (run).
namespace {

// Every address is prologue-checked, so a different build silently does nothing
// instead of jumping into the middle of some other function.
bool AnimAddrsOk()
{
    auto ok = [](uintptr_t a, const unsigned char* sig, int n) {
        auto* p = reinterpret_cast<const unsigned char*>(a);
        if (IsBadReadPtr((void*)p, n)) return false;
        for (int i = 0; i < n; ++i) if (p[i] != sig[i]) return false;
        return true;
    };
    static const unsigned char kSlot[]  = { 0x83, 0xEC, 0x08 };              // sub esp,8
    static const unsigned char kPlay[]  = { 0x53, 0x8B, 0x5C, 0x24, 0x08 };  // push ebx; mov ebx,[esp+8]
    static const unsigned char kState[] = { 0x53, 0x55, 0x56 };              // push ebx; push ebp; push esi
    static int cached = -1;
    if (cached < 0)
        cached = (ok(0x006FCB20, kSlot, 3) && ok(0x00461690, kPlay, 5) &&
                  ok(0x006FF040, kState, 3)) ? 1 : 0;
    return cached == 1;
}

void* ActorAnimData(void* actor)
{
    void** vtable = *reinterpret_cast<void***>(actor);
    if (!vtable || IsBadReadPtr(vtable, 0x1E8)) return nullptr;
    using GetAnimData = void* (__thiscall*)(void*);
    auto fn = reinterpret_cast<GetAnimData>(vtable[0x1E4 / 4]);
    return fn ? fn(actor) : nullptr;
}

} // namespace

bool PlayActorAnimGroup(void* actor, uint32_t animGroup)
{
    if (!actor || !AnimAddrsOk()) return false;
    void* animData = ActorAnimData(actor);
    if (!animData || IsBadReadPtr(animData, 0xE8)) return false;

    using SeqSlotFn = int  (__thiscall*)(void* self, uint32_t group, void* animData,
                                         uint32_t a3, uint32_t a4);
    using PlaySeqFn = void (__thiscall*)(void* self, int seq, uint32_t flags,
                                         uint32_t blendIn, uint32_t blendOut);
    using AnimStateFn = void (__thiscall*)(void* self, uint32_t state, void* sequence);

    int seq = reinterpret_cast<SeqSlotFn>(0x006FCB20)(actor, animGroup, nullptr, 0, 0);
    if (seq < 0) return false;

    // blend 0xFFFFFFFF = the engine's default blend, same value PlayGroup passes.
    reinterpret_cast<PlaySeqFn>(0x00461690)(animData, seq, 1, 0xFFFFFFFF, 0xFFFFFFFF);

    void* active = *reinterpret_cast<void**>(reinterpret_cast<char*>(animData) + 0xE4);
    reinterpret_cast<AnimStateFn>(0x006FF040)(actor, 0xE, active);
    return true;
}

// ---- Native "face this point" ---------------------------------------------
//
// Command Extender's `FaceObject` command turns out to be a thin wrapper: it
// extracts the target ref and calls a GAME function with the target's position —
//     push 0; push [ref+0x34]; push [ref+0x30]; push [ref+0x2C]; call 0x703430
// so `0x00703430` is `__thiscall bool FaceLocation(this=actor, x, y, z, flag)`
// (ret 0x10 = four stack args). Unlike our SetAngle path this goes through the
// actor's own turning code (it reads the AI process at this+0x60), which is what
// the engine itself uses to point an actor at something.
// MUST run on the MAIN THREAD. Returns false if the guard rejected the address.
bool FacePointNative(const Vec3& p)
{
    auto* who = rt::Player();
    if (!who) return false;
    // Guard: `push esi; mov esi,ecx; call rel32` (56 8B F1 E8).
    const unsigned char* code = reinterpret_cast<const unsigned char*>(0x00703430);
    if (IsBadReadPtr((void*)code, 4) ||
        code[0] != 0x56 || code[1] != 0x8B || code[2] != 0xF1 || code[3] != 0xE8) {
        F3A_INFO("FacePoint: prologue guard REJECTED 0x703430");
        return false;
    }
    // What heading the turn should end up at, so the log can be read as
    // "did the previous request actually land?". The engine turns the actor over
    // several frames, so the yaw read immediately after the call is still the OLD
    // one — comparing want/have across consecutive calls is what shows progress.
    float want = std::atan2(p.x - who->posX, p.y - who->posY) * 57.2957795f;
    if (want < 0.0f) want += 360.0f;

    using PFN = char (__thiscall*)(void* self, float x, float y, float z, int flag);
    char rv = reinterpret_cast<PFN>(0x00703430)(who, p.x, p.y, p.z, 0);
    F3A_INFO("FacePoint: ret=%d want_yaw=%.1f have_yaw=%.1f pitch=%.1f "
             "target=(%.0f,%.0f,%.0f)",
             (int)rv, want, GetPlayerYaw(), GetPlayerPitch(), p.x, p.y, p.z);
    return rv != 0;
}

// ---- Native teleport (`Player.MoveTo`) ------------------------------------
//
// `Ref.MoveTo target` runs 0x00528730 MoveTo(whoToMove, targetRef, x, y, z) —
// the engine's reference-mover, which handles cell loading / collision (its
// prologue even loads the player singleton and special-cases moving the player).
// Used as a last resort when navigation can't reach a target: teleport the
// player to the selected object's reference. MUST run on the MAIN thread.
void TeleportPlayerToRef(const void* targetRefr)
{
    auto* p = rt::Player();
    if (!p || !targetRefr) {
        F3A_INFO("Teleport: aborted (player=%p target=%p)", (void*)p, targetRefr);
        return;
    }
    // Guard the address across builds: `mov ecx,[imm32]` (8B 0D ..) then
    // `sub esp,0x48` (83 EC 48). The imm32 (player ptr) differs by runtime, so
    // only the opcode bytes are checked.
    const unsigned char* code = reinterpret_cast<const unsigned char*>(0x00528730);
    if (IsBadReadPtr((void*)code, 9) ||
        code[0] != 0x8B || code[1] != 0x0D ||
        code[6] != 0x83 || code[7] != 0xEC) {
        F3A_INFO("Teleport: prologue guard REJECTED 0x528730 (%02X %02X .. %02X %02X)",
                 code[0], code[1], code[6], code[7]);
        return;
    }
    // Log the before/after position: that's the only way to tell "MoveTo was
    // never called" apart from "MoveTo ran and did nothing" (the case a map
    // marker would hit if the engine refuses to move onto a disabled ref).
    auto* tr = reinterpret_cast<const TESObjectREFR*>(targetRefr);
    float bx = p->posX, by = p->posY, bz = p->posZ;
    using PFN = char (__cdecl*)(void* who, void* target, float x, float y, float z);
    char rv = reinterpret_cast<PFN>(0x00528730)(p, const_cast<void*>(targetRefr),
                                                0.0f, 0.0f, 0.0f);
    F3A_INFO("Teleport: ret=%d player (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) "
             "target refID=%08X at (%.0f,%.0f,%.0f) cell=%p",
             (int)rv, bx, by, bz, p->posX, p->posY, p->posZ,
             (tr && !IsBadReadPtr((void*)tr, 0x40)) ? tr->refID : 0,
             tr ? tr->posX : 0.0f, tr ? tr->posY : 0.0f, tr ? tr->posZ : 0.0f,
             tr ? (void*)tr->parentCell : nullptr);
}

// Does a target reference share the player's coordinate frame? On-foot guidance
// (autowalk / beacon) steers toward the target's RAW world position (posX/Y/Z),
// which is only meaningful when the target lives in the same worldspace as the
// player. A quest marker in ANOTHER worldspace — e.g. the Super-Duper Mart out in
// the Wasteland while you're standing in Megaton (its own worldspace) — has
// coordinates in a different frame, so walking "toward" them heads off in an
// arbitrary direction (straight at Moriarty's door, in the report that surfaced
// this). Native teleport (MoveTo) is cross-cell and works regardless; this guard
// lets callers refuse the walk and point the player at the teleport instead.
//   Same frame  <=>  same parentCell, OR both exterior cells of the SAME
//                    worldspace (the exterior grid shares one coordinate system).
//   Different   <=>  different worldspace, interior-vs-exterior, or two
//                    different interiors.
// Memory reads only — safe from the poll thread. Fails safe: an unreadable or
// cell-less target returns false (treated as "different"), so we never send the
// walker off toward garbage coordinates.
bool TargetSharesPlayerSpace(const void* targetRefr)
{
    auto* p = rt::Player();
    if (!p || !targetRefr) return false;
    auto* tr = reinterpret_cast<const TESObjectREFR*>(targetRefr);
    if (IsBadReadPtr((void*)tr, 0x40)) return false;
    TESObjectCELL* pc = p->parentCell;
    TESObjectCELL* tc = tr->parentCell;
    if (!pc || !tc) return false;
    if (pc == tc) return true;                       // same cell
    if (IsBadReadPtr(pc, 0xC4) || IsBadReadPtr(tc, 0xC4)) return false;
    TESWorldSpace* pw = pc->worldSpace;              // NULL for an interior cell
    TESWorldSpace* tw = tc->worldSpace;
    return pw != nullptr && pw == tw;                // same exterior worldspace
}

// Human-readable label for a reference's location — its cell name and, for an
// exterior, the worldspace name plus the worldspace pointer. Diagnostic aid for
// the cross-worldspace autowalk (so a dump shows WHERE the target ref lives vs
// the player). "(...)" placeholders on any bad read.
std::string GetRefSpaceLabel(const void* refr)
{
    if (!refr || IsBadReadPtr((void*)refr, 0x40)) return "(zly ref)";
    auto* r = reinterpret_cast<const TESObjectREFR*>(refr);
    TESObjectCELL* c = r->parentCell;
    if (!c || IsBadReadPtr(c, 0xC4)) return "(brak komorki)";
    const char* cn = c->fullName.name.m_data;
    std::string out = (cn && !IsBadReadPtr((void*)cn, 1) && *cn)
                          ? GameStrToUtf8(cn) : "(bez nazwy)";
    TESWorldSpace* ws = c->worldSpace;
    char tail[64];
    if (ws && !IsBadReadPtr(ws, 0x24)) {
        const char* wn = ws->fullName.name.m_data;
        std::string wsn = (wn && !IsBadReadPtr((void*)wn, 1) && *wn)
                              ? GameStrToUtf8(wn) : "(ws bez nazwy)";
        std::snprintf(tail, sizeof(tail), " ws=%p", (void*)ws);
        out += " / " + wsn + tail;
    } else {
        out += " / wnetrze";
    }
    return out;
}

namespace {
// Two cells share one coordinate frame: the same cell, or two exterior cells of
// the same worldspace. (Interiors have worldSpace==NULL and never match across
// cells.) Used to decide whether a load door bridges us toward the target.
bool CellsShareSpace(TESObjectCELL* a, TESObjectCELL* b)
{
    if (!a || !b) return false;
    if (a == b) return true;
    if (IsBadReadPtr(a, 0xC4) || IsBadReadPtr(b, 0xC4)) return false;
    TESWorldSpace* wa = a->worldSpace;
    TESWorldSpace* wb = b->worldSpace;
    return wa != nullptr && wa == wb;
}
TESObjectCELL* RefCellGuarded(const void* r)
{
    if (!r || IsBadReadPtr((void*)r, 0x40)) return nullptr;
    return reinterpret_cast<const TESObjectREFR*>(r)->parentCell;
}
// The reference a LOAD door teleports to — its linked door in the destination
// cell — or null when this ref isn't a load door. Walks ExtraTeleport
// (kExtraData_Teleport 0x2B): node+0x0C = Data*, Data+0x00 = linkedDoor. Memory
// reads only (safe off the main thread).
const TESObjectREFR* DoorLinkedRef(const void* doorRefr)
{
    const auto* r = reinterpret_cast<const TESObjectREFR*>(doorRefr);
    if (!r || IsBadReadPtr((void*)r, 0x48)) return nullptr;
    const UInt8* node = *reinterpret_cast<const UInt8* const*>(
                            reinterpret_cast<const UInt8*>(r) + 0x44);
    for (int i = 0; node && i < 128; ++i) {
        if (IsBadReadPtr((void*)node, 0x10)) break;
        if (node[0x04] == 0x2B) {                        // kExtraData_Teleport
            const UInt8* data = *reinterpret_cast<const UInt8* const*>(node + 0x0C);
            if (IsBadReadPtr((void*)data, 4)) return nullptr;
            auto* linked = *reinterpret_cast<TESObjectREFR* const*>(data);  // linkedDoor
            if (IsBadReadPtr(linked, 0x40)) return nullptr;
            return linked;
        }
        node = *reinterpret_cast<const UInt8* const*>(node + 0x08);   // next
    }
    return nullptr;
}
} // namespace

// A load door is one that teleports the player to another cell when activated.
bool IsLoadDoor(const void* doorRefr)
{
    return DoorLinkedRef(doorRefr) != nullptr;
}

// Would walking through this load door put us in the SAME coordinate frame as
// the target reference? (I.e. does its far side share the target's cell/world-
// space.) Lets cross-worldspace autowalk prefer the door that actually leads
// toward the goal. False for non-load doors and on any unreadable pointer.
bool DoorLeadsToTargetSpace(const void* doorRefr, const void* targetRefr)
{
    const TESObjectREFR* linked = DoorLinkedRef(doorRefr);
    if (!linked) return false;
    return CellsShareSpace(linked->parentCell, RefCellGuarded(targetRefr));
}

// Does this load door lead back into the given cell's coordinate frame? Used to
// avoid immediately re-entering the door we just came through (its far side is
// in the cell we just left), which would otherwise loop the walker.
bool DoorLeadsToCellSpace(const void* doorRefr, const void* cellPtr)
{
    const TESObjectREFR* linked = DoorLinkedRef(doorRefr);
    if (!linked) return false;
    return CellsShareSpace(linked->parentCell,
                           reinterpret_cast<TESObjectCELL*>(const_cast<void*>(cellPtr)));
}

// Does this load door lead OUT to an exterior worldspace (as opposed to into
// another interior)? Cross-worldspace autowalk uses this to head toward the open
// world — where a distant target building lives — instead of wandering into
// random interiors (Craterside, houses...) that aren't the goal.
bool DoorLeadsToExterior(const void* doorRefr)
{
    const TESObjectREFR* linked = DoorLinkedRef(doorRefr);
    if (!linked) return false;
    TESObjectCELL* c = linked->parentCell;
    if (!c || IsBadReadPtr(c, 0xC4)) return false;
    return c->worldSpace != nullptr;                 // NULL worldSpace == interior
}

bool SetIniSettingFloat(const char* name, float value)
{
    UInt8* set = FindIniSetting(name);
    if (!set) return false;
    *reinterpret_cast<float*>(set + 0x04) = value;
    return true;
}

bool GetIniSettingFloat(const char* name, float& out)
{
    UInt8* set = FindIniSetting(name);
    if (!set) return false;
    out = *reinterpret_cast<float*>(set + 0x04);
    return true;
}

bool SetIniSettingInt(const char* name, uint32_t value)
{
    UInt8* set = FindIniSetting(name);
    if (!set) return false;
    *reinterpret_cast<uint32_t*>(set + 0x04) = value;   // bool/int share the union
    return true;
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

// Read a STRING trait by id (nullptr if absent or empty). Menus keep some state
// in the `user0..userN` traits as strings — e.g. the hacking grid stores the
// word currently under the cursor there.
const char* TileStrTrait(const Tile* t, UInt32 id)
{
    if (!t) return nullptr;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == id && v->str && *v->str) return v->str;
    }
    return nullptr;
}

// Visibility for tiles that omit the trait: absent `visible` means SHOWN here
// (TileVisible treats absent as hidden, which is right for the highlight boxes
// but wrong for list rows that never declare it).
bool TileShown(const Tile* t)
{
    if (!t) return false;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == kTileValue_visible) return v->num != 0.0f;
    }
    return true;
}

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
float TileAbsX(Tile* t)
{
    float x = 0.0f;
    for (Tile* p = t; p; p = p->parent) x += TileNum(p, kTileValue_x);
    return x;
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
UInt32 OwningMenuType(Tile* t);   // defined below

// Is this row an EQUIPPED item? The item lists draw a small marker tile inside
// the row when the item is worn/wielded — FO3 uses the same `IM_Template_
// ItemMarker` name the FNV accessibility mod reads. Restricted to menus where
// "equipped" is meaningful: the Pip-Boy data lists reuse the same tile templates
// for unrelated things and would otherwise report quests as worn.
bool RowIsEquipped(Tile* row)
{
    if (!row) return false;
    UInt32 mt = OwningMenuType(row);
    if (mt != kMenuType_Inventory && mt != kMenuType_Container &&
        mt != kMenuType_Barter)
        return false;

    // The row carries an `IM_Template_ItemMarker` tile whose VISIBILITY is the
    // equipped flag. Confirmed against a live inventory dump: the worn Pip-Boy
    // and its glove had `vis=1`, while the spare Pip-Boy, caps and Mentats
    // carried the same tile with no visible flag. Alpha is NOT the signal —
    // every marker sits at 255, so testing alpha reported every item as worn.
    Tile* mk = FindChildByName(row, "Template_ItemMarker", 3);
    return mk && TileVisible(mk);
}

void DescribeRowParts(Tile* t, std::string& label, std::string& value)
{
    label.clear();
    value.clear();
    if (!t) return;
    CollectRowText(t, 0, label);
    if (!label.empty() && RowIsEquipped(t)) label += ", założone";

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

// The row the keyboard highlight currently sits on, plus the listbox holding it.
// Split out of GetKeyboardSelection so the row can also be CLICKED, not just
// read — pressing a quest in the Pip-Boy is what makes it the tracked quest.
Tile* FindKeyboardSelectedTile(Tile** out_listbox)
{
    if (out_listbox) *out_listbox = nullptr;
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return nullptr;

    // In gameplay every menu stays mounted in menuRoot; scan only the
    // VISIBLE ones or we'd read rows of half-built / closed menus (the
    // "krzaczki" garbage during loading). At the main menu the visibility
    // flags are unreliable, so scan everything there (works today).
    const bool only_visible = poll::IsGameplayActive();

    float best_dy = 28.0f;
    Tile* best = nullptr;
    Tile* listbox = nullptr;
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
    if (out_listbox) *out_listbox = listbox;
    return best;
}

std::optional<MenuSelection> GetKeyboardSelection()
{
    Tile* listbox = nullptr;
    Tile* best = FindKeyboardSelectedTile(&listbox);
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

// Press the row the keyboard highlight is on, through the engine's own click
// path — the same thing a mouse click on that row does. `listNameSubstr` guards
// which list it may fire in (a substring matched against the row's ancestors),
// so a key bound for the quest list can't activate a row in some other menu.
//
// Used for "make this the tracked quest": selecting a quest in the Pip-Boy only
// moves the highlight; the game starts tracking it when the row is CLICKED.
// Is the keyboard highlight currently inside a list whose name contains this?
// Pure memory reads, so it is safe from the poll thread — callers use it to
// decide WHICH action a key should take before requesting anything.
bool IsKeyboardSelectionIn(const char* listNameSubstr)
{
    Tile* row = FindKeyboardSelectedTile(nullptr);
    if (!row || !listNameSubstr) return false;
    for (Tile* p = row; p; p = p->parent)
        if (TileNameHas(p, listNameSubstr)) return true;
    return false;
}

bool ClickSelectedRowIn(const char* listNameSubstr)
{
    Tile* listbox = nullptr;
    Tile* row = FindKeyboardSelectedTile(&listbox);
    if (!row) return false;

    if (listNameSubstr && *listNameSubstr) {
        bool inList = false;
        for (Tile* p = row; p && !inList; p = p->parent)
            if (TileNameHas(p, listNameSubstr)) inList = true;
        if (!inList) return false;
    }

    // Find the Menu that owns this row so we can go through its HandleClick.
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return false;
    Tile* prev = row;
    Menu* owner = nullptr;
    for (Tile* p = row->parent; p; p = p->parent) {
        if (p == ifm->menuRoot) {
            auto* tm = reinterpret_cast<TileMenu*>(prev);
            owner = tm->menu;
            break;
        }
        prev = p;
    }
    if (!owner) return false;

    UInt32 id = (UInt32)TileNum(row, kTileValue_id);
    F3A_INFO("ClickSelectedRow: menu 0x%X id=%u list='%s'",
             owner->typeID, id,
             (listbox && listbox->name.m_data) ? listbox->name.m_data : "?");
    owner->HandleClick(id, row);
    return true;
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
    // Names that act as "leave this menu": the StartMenu's *back_button, and
    // exit buttons on full-screen menus like the SPECIAL book (exit_menu).
    static const char* kBackNames[] = { "back_button", "exit_menu",
                                        "exit_button" };
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m) {
                // FO3 terminals (ComputersMenu) and the hacking screen have no
                // back_button tile — sighted players leave them with the Tab
                // control. Inject Tab so Backspace closes them too.
                if ((m->typeID == kMenuType_Computers ||
                     m->typeID == kMenuType_Hacking) && TileVisible(cn->child)) {
                    // FO3 terminals have no back_button tile and don't respond
                    // to injected keys (keyboard is DirectInput; SendInput is
                    // ignored — only mouse gets through). Call the engine's own
                    // terminal log-off routine 0x636EC0 — exactly what "Back" at
                    // the root directory runs: it sets the close trait 0x1772 on
                    // the terminal tile (from the global active-terminal state at
                    // 0x1075FF4) and runs the fade/finalize (0xBFC470). It takes
                    // no args and self-guards when no terminal is active. Guard
                    // the address by its prologue (cmp [0x1075FF4],0). MAIN THREAD.
                    const unsigned char* code =
                        reinterpret_cast<const unsigned char*>(0x00636EC0);
                    if (!IsBadReadPtr((void*)code, 6) &&
                        code[0] == 0x83 && code[1] == 0x3D && code[2] == 0xF4 &&
                        code[3] == 0x5F && code[4] == 0x07 && code[5] == 0x01) {
                        reinterpret_cast<void(__cdecl*)(void)>(0x00636EC0)();
                        F3A_INFO("ClickMenuBack: terminal log-off via 0x636EC0 (menu 0x%X)",
                                 m->typeID);
                    }
                    return true;
                }
                for (const char* nm : kBackNames) {
                    Tile* btn = FindChildByName(cn->child, nm, 6);
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
        }
        node = node->next;
    }
    return false;
}

// Click a named button inside a menu through the menu's own HandleClick — the
// engine's normal "the user pressed this" path. MAIN THREAD only (it runs game
// code, opens dialogs, etc.).
//
// DO NOT use this for a button that CLOSES its menu or moves items — the
// quantity prompt's Ok crashed the game every time, in both the tile and
// no-tile form, because the player's own key press is already tearing that menu
// down. Read such menus and let the player press their own keys instead.
bool ClickMenuButton(uint32_t menuType, const char* btnName, int depth)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return false;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m && m->typeID == menuType) {
                Tile* btn = FindChildByName(cn->child, btnName, depth);
                if (btn) {
                    UInt32 id = (UInt32)TileNum(btn, kTileValue_id);
                    F3A_INFO("ClickMenuButton(menu 0x%X, '%s') id=%u",
                             menuType, btnName, id);
                    m->HandleClick(id, btn);
                    return true;
                }
                F3A_INFO("ClickMenuButton(menu 0x%X): '%s' not found",
                         menuType, btnName);
                return false;
            }
        }
        node = node->next;
    }
    return false;
}


bool ClickVatsButton(const char* btnName)
{
    // e.g. "BodyPart_button" (cycle limb), "left_arrow"/"right_arrow" (switch
    // target). The tile reader then announces the new selection.
    return ClickMenuButton(kMenuType_VATS, btnName, 6);
}

bool ClickVatsBodyPart() { return ClickVatsButton("BodyPart_button"); }

// The map's action button ("Podróżować do <miejsce>" once a marker is selected).
// Clicking a marker only SELECTS it — this is the press that actually travels,
// and it goes through the game's own path, so all its rules apply.
bool ClickMapTravelButton() { return ClickMenuButton(kMenuType_Map, "MM_ButtonA", 8); }

// Select a world-map location by clicking its marker tile through the engine.
// Driving the mouse cursor there instead was fragile: it depends on the map's
// scroll/zoom, on the cursor tile being visible, and silently did nothing when
// the local-map tab was up. The world map names each marker tile after the
// location, so we can hand the tile straight to HandleClick.
// Write a numeric trait on a tile through the engine's own setter, so the menu
// re-evaluates whatever depends on it (0x00BEE190, __thiscall, ret 0xC).
void SetTileFloat(Tile* tile, UInt32 traitId, float value)
{
    if (!tile) return;
    const unsigned char* code = reinterpret_cast<const unsigned char*>(0x00BEE190);
    if (IsBadReadPtr((void*)code, 5) ||
        code[0] != 0x8B || code[1] != 0x44 || code[2] != 0x24 || code[3] != 0x04) {
        F3A_INFO("SetTileFloat: prologue guard rejected 0x00BEE190");
        return;
    }
    using PFN = void (__thiscall*)(Tile*, UInt32, float, int);
    reinterpret_cast<PFN>(0x00BEE190)(tile, traitId, value, 0);
}

bool ClickMapMarkerTile(const void* tile)
{
    Menu* menu = FindMenuByType(kMenuType_Map);
    Tile* menuTile = FindVisibleMenuTile(kMenuType_Map);
    auto* marker = reinterpret_cast<Tile*>(const_cast<void*>(tile));
    if (!menu || !menuTile || !marker || IsBadReadPtr(marker, 0x40)) return false;

    // The engine accepts a marker click only while the map's action button is a
    // live target — the FNV mod sets exactly this before clicking, and without it
    // the click merely selects the marker instead of travelling.
    if (Tile* buttonA = FindChildByName(menuTile, "MM_ButtonA", 8))
        SetTileFloat(buttonA, kTileValue_target, 1.0f);

    // Marker clicks use a FIXED id (0x1A), not the tile's own id.
    constexpr UInt32 kTileID_LocationMarker = 0x1A;
    F3A_INFO("ClickMapMarkerTile: %p vis=%d", marker, (int)TileVisible(marker));
    menu->HandleClick(kTileID_LocationMarker, marker);
    return true;
}

namespace {
// ASCII case-insensitive substring test (the game strings are code page 1250,
// so we only match ASCII-only needles to avoid encoding mismatches on ą/ś/...).
bool AsciiIContains(const char* hay, const char* needle)
{
    if (!hay || !needle) return false;
    size_t nl = std::strlen(needle);
    for (const char* p = hay; *p; ++p)
        if (_strnicmp(p, needle, nl) == 0) return true;
    return false;
}

// Recursively find a VISIBLE, clickable tile whose label looks like a "restore
// defaults" button. ASCII fragments: "domy" (Domyślne), "przyw" (Przywróć),
// "default". Returns the tile (and its click id via out_id) or nullptr.
Tile* FindDefaultsButton(Tile* t, int depth, UInt32& out_id)
{
    if (!t || depth > 10) return nullptr;
    if (TileVisible(t) && TileNum(t, kTileValue_target) != 0.0f) {
        if (const char* s = TileLabelDeep(t, 0)) {
            if (AsciiIContains(s, "domy") || AsciiIContains(s, "przyw") ||
                AsciiIContains(s, "default")) {
                out_id = (UInt32)TileNum(t, kTileValue_id);
                return t;
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            UInt32 id = 0;
            if (Tile* r = FindDefaultsButton(cn->child, depth + 1, id)) {
                out_id = id; return r;
            }
        }
        node = node->next;
    }
    return nullptr;
}
} // namespace

bool ClickRestoreDefaults()
{
    // Click the "Restore Defaults" button in the settings/controls page
    // (StartMenu). Like ClickMenuBack, we press it through the owning menu's
    // HandleClick (a game call → main thread only). The game then pops its own
    // Yes/No confirm, which our message-box reader already handles.
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return false;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child && rt::TileIsVisible(cn->child)) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m && m->typeID == kMenuType_Start) {
                UInt32 id = 0;
                if (Tile* btn = FindDefaultsButton(cn->child, 0, id)) {
                    F3A_INFO("ClickRestoreDefaults: '%s' (id=%u)",
                             btn->name.m_data ? btn->name.m_data : "?", id);
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

std::string GetSelectedItemInfo()
{
    // The selected item's stat card is an "*ItemInfoRect" box (Pip-Boy
    // inventory IM_ItemInfoRect) holding CI_TitleText+CI_ValueText sub-panels:
    // weight, value, damage/DR, condition, effects, ammo. Find the visible one
    // under any open menu and read its pairs. FO3's container UI has no such
    // card (only item names), so this is empty there.
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return {};
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int s = 0; node && s < 4096; ++s) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            Tile* box = FindChildByName(cn->child, "ItemInfoRect", 8);
            if (box && TileVisible(box)) {
                std::string out;
                CollectInfoPairs(box, out);
                if (!out.empty()) return out;
            }
        }
        node = node->next;
    }
    return {};
}

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

// The text-entry popup (character name at birth, etc.) is a TextEditMenu with a
// `textedit_prompt` tile (the question) and a `textedit_text` tile (what's typed
// so far, with a trailing '|' caret glyph). Read either for accessible name
// entry. Returns nullopt if the TextEdit menu isn't open.
std::optional<std::string> GetTextEditPrompt()
{
    Tile* m = FindVisibleMenuTile(kMenuType_TextEdit);
    if (!m) return std::nullopt;
    if (Tile* p = FindChildByName(m, "textedit_prompt", 6))
        if (const char* s = TileStringTrait(p)) return GameStrToUtf8(s);
    return std::nullopt;
}

std::optional<std::string> GetTextEditText()
{
    Tile* m = FindVisibleMenuTile(kMenuType_TextEdit);
    if (!m) return std::nullopt;
    Tile* t = FindChildByName(m, "textedit_text", 6);
    if (!t) return std::nullopt;
    const char* s = TileStringTrait(t);
    std::string r = s ? GameStrToUtf8(s) : std::string();
    // Strip the trailing caret glyph the field appends (and any blink/space) so
    // a blinking caret doesn't read as a change and the text comes out clean.
    while (!r.empty() && (r.back() == '|' || r.back() == ' ' ||
                          r.back() == '\t')) r.pop_back();
    return r;
}

// The terminal/computer screen body. A FO3 terminal is the `ComputersMenu`: the
// opened log/report/note body is shown in a `computers_display_zone_text` tile
// (NOT `TM_text` — that's a different menu family). The selectable command list
// (computers_file_template_text) is read by the generic focus poller; this reads
// the displayed PAGE so logs/messages can be heard. nullopt if no page is shown.
std::optional<std::string> GetTerminalText()
{
    // Don't rely on the menu dispatch recognising the terminal as "open": search
    // every mounted top-level menu for the body tile and read it. Gate on the
    // body tile's own visibility so a closed-but-mounted terminal's stale text
    // isn't read.
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return std::nullopt;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            Tile* body = FindChildByName(cn->child, "computers_display_zone_text", 12);
            if (!body) body = FindChildByName(cn->child, "TM_text", 12);  // other terminal families
            if (body && rt::TileIsVisible(body)) {
                if (const char* s = TileStringTrait(body)) {
                    std::string r = GameStrToUtf8(s);
                    // Trim outer whitespace but keep internal line breaks.
                    while (!r.empty() && (r.back() == ' ' || r.back() == '\t' ||
                                          r.back() == '\n' || r.back() == '\r'))
                        r.pop_back();
                    size_t b = r.find_first_not_of(" \t\n\r");
                    if (b != std::string::npos) r = r.substr(b);
                    if (!r.empty()) return r;
                }
            }
        }
        node = node->next;
    }
    // A LOCKED / hackable terminal opens the HackingMenu instead of TM_text.
    // Read the whole screen (welcome banner, "TERMINAL ZABLOKOWANY", status) so
    // the player at least knows the state. (Full word-guessing accessibility is
    // a separate feature.)
    if (Tile* hk = FindVisibleMenuTile(kMenuType_Hacking)) {
        std::string out;
        CollectAllVisibleText(hk, 0, out);
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                                out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        if (!out.empty()) return out;
    }
    return std::nullopt;
}

bool IsTerminalOpen() { return FindVisibleMenuTile(kMenuType_Computers) != nullptr; }

namespace {
// Terminal "chrome" = everything the screen shows EXCEPT the opened entry body
// (handled by GetTerminalText) and the selectable command list (read by the
// focus poller): the boot/welcome banner, the ROBCO headers, the login lines,
// command-result echoes. We read these live, as they appear, like F4 Access.
bool TermChromeExcluded(const Tile* t)
{
    const char* n = (t && t->name.m_data) ? t->name.m_data : "";
    return std::strstr(n, "file_template") ||   // the command list (focus poller)
           std::strstr(n, "display_zone")  ||   // the opened entry body
           std::strstr(n, "result_prompt") ||   // just "> "
           std::strstr(n, "cursor")        ||
           std::strstr(n, "pwdisplay");         // "*****"
}
void CollectTermChromeRec(Tile* t, int depth, std::vector<std::string>& out)
{
    if (!t || depth > 12) return;
    if (!TermChromeExcluded(t) && TileVisible(t)) {
        if (const char* s = TileStringTrait(t)) {
            std::string e = GameStrToUtf8(s);
            size_t b = e.find_first_not_of(" \t\n\r");
            if (b != std::string::npos) {
                size_t en = e.find_last_not_of(" \t\n\r");
                e = e.substr(b, en - b + 1);
                if (!e.empty()) out.push_back(e);
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectTermChromeRec(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

std::vector<std::string> CollectTerminalChrome()
{
    std::vector<std::string> out;
    if (Tile* menu = FindVisibleMenuTile(kMenuType_Computers))
        CollectTermChromeRec(menu, 0, out);
    return out;
}

namespace {
// True if the tile currently carries the mouseover/highlight trait. In the
// RaceSex menu this MOVES with keyboard navigation (that's why an option could
// be highlighted with no mouse), so it's the live cursor — unlike the
// selection_indicator, which marks only the committed choice.
bool TileMouseover(const Tile* t)
{
    if (!t) return false;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == kTileValue_mouseover && v->num != 0.0f) return true;
    }
    return false;
}
// The committed selection: an RSM_list_item whose RSM_selection_indicator child
// is visible. Fallback when nothing is highlighted.
bool RsmItemCommitted(Tile* item)
{
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&item->childList);
    for (int s = 0; node && s < 64; ++s) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child && TileNameHas(cn->child, "selection_indicator") &&
            TileVisible(cn->child))
            return true;
        node = node->next;
    }
    return false;
}
void FindRaceSexFocus(Tile* t, int depth, std::string& out, bool wantHover)
{
    if (!t || depth > 12 || !out.empty()) return;
    if (TileVisible(t)) {
        bool match = false;
        if (wantHover) {
            // The moving cursor: list options AND the WSTECZ/DALEJ buttons at the
            // bottom (RSM_back_button/RSM_next_button) — they're not list items.
            match = TileMouseover(t) &&
                    (TileNameHas(t, "RSM_list_item") ||
                     TileNameHas(t, "RSM_back_button") ||
                     TileNameHas(t, "RSM_next_button"));
        } else {
            match = TileNameHas(t, "RSM_list_item") && RsmItemCommitted(t);
        }
        if (match) {
            if (const char* s = TileStringTrait(t)) { out = GameStrToUtf8(s); return; }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int s = 0; node && s < 4096 && out.empty(); ++s) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) FindRaceSexFocus(cn->child, depth + 1, out, wantHover);
        node = node->next;
    }
}
} // namespace

// Character creation (RaceSexMenu): the focused category/option (Płeć, Rasa,
// Włosy, the race list, …) is the RSM_list_item whose RSM_selection_indicator
// child is visible. Returns its text so navigation can be heard. nullopt if the
// menu isn't open / nothing focused. (Face fine-tuning sliders are a TODO.)
std::optional<std::string> GetRaceSexSelection()
{
    // Search the whole menu tree (don't depend on the RaceSexMenu being detected
    // as a dispatched "open" menu — like the terminal, it isn't reliably).
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return std::nullopt;
    Tile* root = reinterpret_cast<Tile*>(ifm->menuRoot);
    std::string out;
    FindRaceSexFocus(root, 0, out, /*wantHover=*/true);    // the moving cursor
    if (out.empty())
        FindRaceSexFocus(root, 0, out, /*wantHover=*/false); // committed selection
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    if (out.empty()) return std::nullopt;
    return out;
}

namespace {
// Gather the lock's sweet-spot band: several concentric LPM_SweetSpot tiles,
// all centred on the same x; the NARROWEST is the tightest "will open" zone.
void CollectSweetSpot(Tile* t, int depth, float& center, float& minw, int& n)
{
    if (!t || depth > 12) return;
    if (TileNameHas(t, "LPM_SweetSpot")) {
        float x = TileNum(t, kTileValue_x);
        float w = TileNum(t, kTileValue_width);
        if (w > 1.0f) { if (w < minw) { minw = w; center = x + w * 0.5f; } ++n; }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int s = 0; node && s < 4096; ++s) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectSweetSpot(cn->child, depth + 1, center, minw, n);
        node = node->next;
    }
}
} // namespace

// Lockpick cue: the pin position is LPM_DetectArrow.x; the sweet spot is the
// LPM_SweetSpot band's centre, tolerance = its narrowest half-width. Fills
// *offset = |pin - centre| and *tol = tolerance. Returns false if the lockpick
// menu isn't open. Lets us beep the player onto the sweet spot by ear.
bool GetLockpickCue(float* offset, float* tol)
{
    Tile* m = FindVisibleMenuTile(kMenuType_LockPick);
    if (!m) return false;
    Tile* arrow = FindChildByName(m, "LPM_DetectArrow", 10);
    if (!arrow) return false;
    float pin = TileNum(arrow, kTileValue_x);
    float center = 0.0f, minw = 1e9f; int n = 0;
    CollectSweetSpot(m, 0, center, minw, n);
    if (n == 0) return false;
    if (offset) *offset = std::fabs(pin - center);
    if (tol)    *tol    = (minw > 2.0f ? minw : 2.0f) * 0.5f;
    return true;
}

namespace {
struct HackRow { unsigned long addr; std::string chars; Tile* row; int prefix; };
struct HackWord { std::string text; float tx; float ty; };   // UI pos of middle letter

void CollectHackRows(Tile* t, int depth, std::vector<HackRow>& rows)
{
    if (!t || depth > 14) return;
    if (TileNameHas(t, "hacking_password_file_row")) {
        if (const char* s = TileStringTrait(t)) {
            std::string str = s;                  // "0xADDR <chars>"
            size_t sp = str.find(' ');
            if (sp != std::string::npos && sp + 1 < str.size())
                rows.push_back({ std::strtoul(str.c_str(), nullptr, 16),
                                 str.substr(sp + 1), t, (int)(sp + 1) });
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectHackRows(cn->child, depth + 1, rows);
        node = node->next;
    }
}
int CountTilesNamed(Tile* t, int depth, const char* needle)
{
    if (!t || depth > 14) return 0;
    int n = TileNameHas(t, needle) ? 1 : 0;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) n += CountTilesNamed(cn->child, depth + 1, needle);
        node = node->next;
    }
    return n;
}
// Candidate words + the grid tile to "click" to guess each. Words are maximal
// A-Z runs in the address-ordered char stream (they span rows); a word's click
// target is the row tile where its first letter lives.
void BuildHackWords(Tile* m, std::vector<HackWord>& out)
{
    std::vector<HackRow> rows;
    CollectHackRows(m, 0, rows);
    std::sort(rows.begin(), rows.end(),
              [](const HackRow& a, const HackRow& b) { return a.addr < b.addr; });
    struct Loc { Tile* row; int ci; float cw; };   // per content char
    std::string stream;
    std::vector<Loc> loc;
    for (auto& r : rows) {
        int fullLen = r.prefix + (int)r.chars.size();
        float cw = fullLen > 0 ? TileNum(r.row, kTileValue_width) / fullLen : 0.0f;
        for (size_t k = 0; k < r.chars.size(); ++k) {
            stream += r.chars[k];
            loc.push_back({ r.row, r.prefix + (int)k, cw });
        }
    }
    std::string run;
    auto flush = [&](size_t end) {
        if (run.size() >= 4) {
            size_t s = end - run.size(), mid = s + run.size() / 2;
            HackWord w{ run, 0.0f, 0.0f };
            if (mid < loc.size()) {
                Loc& l = loc[mid];
                w.tx = TileAbsX(l.row) + l.ci * l.cw + l.cw * 0.5f;
                w.ty = TileAbsY(l.row) + TileNum(l.row, kTileValue_height) * 0.5f;
            }
            out.push_back(w);
        }
        run.clear();
    };
    for (size_t i = 0; i < stream.size(); ++i) {
        char ch = stream[i];
        if (ch >= 'A' && ch <= 'Z') run += ch; else flush(i);
    }
    flush(stream.size());
}
} // namespace

namespace {
// Is this an all-caps candidate word (what the grid's user trait holds)?
bool LooksLikeGridWord(const char* s)
{
    if (!s) return false;
    int n = 0;
    for (const char* p = s; *p; ++p, ++n)
        if (*p < 'A' || *p > 'Z') return false;
    return n >= 2;
}
// The word under the cursor, from `hacking_password_file_rect`'s user trait.
std::string HighlightedWordOf(Tile* m)
{
    Tile* grid = m ? FindChildByName(m, "hacking_password_file_rect", 10) : nullptr;
    if (!grid) return {};
    // FNV's mod reads user0; the trait INDEX isn't guaranteed to match in FO3,
    // so probe user0..user9 and take the first all-caps value. Log the hit once
    // so a wrong guess here is visible in the log rather than silent.
    for (UInt32 i = 0; i < 10; ++i) {
        const char* s = TileStrTrait(grid, kTileValue_user0 + i);
        if (LooksLikeGridWord(s)) {
            static UInt32 s_logged = 0xFFFFFFFF;
            if (s_logged != i) {
                s_logged = i;
                F3A_INFO("Hacking: highlighted word comes from the user%u trait.", i);
            }
            return s;
        }
    }
    return {};
}
} // namespace

// Read the hacking (terminal password) minigame: candidate words + attempts.
HackingInfo GetHackingInfo()
{
    HackingInfo h;
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m) return h;
    std::vector<HackWord> words;
    BuildHackWords(m, words);
    for (auto& w : words) h.words.push_back(w.text);
    h.attempts = CountTilesNamed(m, 0, "hacking_guess_block");
    h.highlighted = HighlightedWordOf(m);
    // Locked out: the grid is gone and the lock-out message is showing.
    Tile* lk = FindChildByName(m, "hacking_locked_message_text_1", 10);
    h.locked = lk && TileShown(lk) && TileStringTrait(lk) != nullptr;
    h.active = !h.words.empty() || h.locked;
    return h;
}

std::string GetHackingHighlightedWord()
{
    return HighlightedWordOf(FindVisibleMenuTile(kMenuType_Hacking));
}

namespace {
void CollectHackTraits(Tile* t, int depth, std::vector<std::string>& out)
{
    if (!t || depth > 14 || out.size() > 200) return;
    const char* nm = (t->name.m_data ? t->name.m_data : "?");
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        // The plain `string` trait is already dumped by the tile tree; what we
        // want here are the extra string-carrying traits (user0.., floats aside).
        if (!v || v->id == kTileValue_string || !v->str || !*v->str) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s trait 0x%04X = '%s'", nm, v->id, v->str);
        out.push_back(buf);
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectHackTraits(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

std::vector<std::string> DebugHackingTraits()
{
    std::vector<std::string> out;
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m) return out;
    CollectHackTraits(m, 0, out);
    return out;
}

namespace {
void CollectHackLog(Tile* t, int depth, std::string& out)
{
    if (!t || depth > 14) return;
    if (TileNameHas(t, "hacking_password_log_entry")) {
        if (const char* s = TileStringTrait(t)) {
            std::string e = GameStrToUtf8(s);
            // skip the ">" prompt and empty rows
            if (e.size() > 1 || (e.size() == 1 && e[0] != '>')) {
                if (!out.empty()) out += " ";
                out += e;
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectHackLog(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

namespace {
struct HackLogEntry { int index; std::string text; };
void CollectHackLogEntries(Tile* t, int depth, std::vector<HackLogEntry>& out)
{
    if (!t || depth > 14) return;
    if (TileNameHas(t, "hacking_password_log_entry") && TileShown(t)) {
        if (const char* s = TileStringTrait(t)) {
            std::string e = GameStrToUtf8(s);
            if (e.size() > 1 || (e.size() == 1 && e[0] != '>'))
                out.push_back({ static_cast<int>(TileNum(t, kTileValue_listindex)), e });
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectHackLogEntries(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

// The attempt log split into entries, oldest first. The tile tree hands them
// back in arbitrary order, so sort by `listindex` — the row order on screen.
std::vector<std::string> GetHackingLogEntries()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m) return {};
    std::vector<HackLogEntry> es;
    CollectHackLogEntries(m, 0, es);
    std::stable_sort(es.begin(), es.end(),
                     [](const HackLogEntry& a, const HackLogEntry& b) { return a.index < b.index; });
    std::vector<std::string> out;
    out.reserve(es.size());
    for (auto& e : es) out.push_back(e.text);
    return out;
}

// The hacking attempt log joined (guesses + "Likeness=N" / "Entry denied" /
// "Access granted"). Empty if the menu isn't open.
std::string GetHackingLog()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m) return {};
    std::string out;
    CollectHackLog(m, 0, out);
    return out;
}

// The HUD corner-notification text (the "Messages" tile: quest updates, items
// received, XP, discovered locations…). Empty when nothing is showing. Read live
// so a blind player hears the passive feedback a sighted player sees.
std::string GetHudMessage()
{
    Tile* hud = FindVisibleMenuTile(kMenuType_HUDMain);
    if (!hud) return {};
    Tile* msgs = FindChildByName(hud, "Messages", 4);
    if (!msgs) return {};
    std::string out;
    CollectAllVisibleText(msgs, 0, out);
    size_t b = out.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return {};
    size_t e = out.find_last_not_of(" \t\n\r");
    return out.substr(b, e - b + 1);
}

// The crosshair ACTIVATE prompt (the HUD `Info` tile): the verb + target shown
// when you look at something you can interact with — "Rozmawiaj", "Weź",
// "Okradnij", "Otwórz", "Użyj"… Reading it aloud lets a blind player know the
// action BEFORE pressing Use (e.g. hearing "Okradnij" avoids a karma-losing
// theft). Empty when nothing is targeted (the tile hides / shows a placeholder).
std::string GetActivatePrompt()
{
    Tile* hud = FindVisibleMenuTile(kMenuType_HUDMain);
    if (!hud) return {};
    Tile* info = FindChildByName(hud, "Info", 4);
    if (!info) return {};
    // The prompt lives in `button_text` inside a `justify_center_hotrect`; the
    // hotrect is visible only while a prompt is actually showing.
    Tile* hot = FindChildByName(info, "justify_center_hotrect", 4);
    if (!hot || !TileVisible(hot)) return {};
    Tile* bt = FindChildByName(hot, "button_text", 3);
    const char* s = bt ? TileStringTrait(bt) : nullptr;
    if (!s) return {};
    std::string r = GameStrToUtf8(s);
    size_t b = r.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return {};
    size_t e = r.find_last_not_of(" \t\n\r");
    r = r.substr(b, e - b + 1);
    if (r == "Button Text" || r.empty()) return {};   // untranslated placeholder
    return r;
}

namespace {
void CollectHackTextLines(Tile* t, int depth, std::vector<std::string>& out)
{
    if (!t || depth > 14) return;
    // The readable prose of the screen: the header (hacking_intro_text) and the
    // attempt log — NOT the garbage grid rows. Skip the ">" prompt / blanks.
    if (TileNameHas(t, "hacking_intro_text") ||
        TileNameHas(t, "hacking_password_log_entry")) {
        if (TileVisible(t)) {
            if (const char* s = TileStringTrait(t)) {
                std::string e = GameStrToUtf8(s);
                if (e.size() > 1 || (e.size() == 1 && e[0] != '>')) out.push_back(e);
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectHackTextLines(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

// All readable text lines of the hacking screen (header + log), for browsing and
// live reading of text as it appears. Excludes the garbage memory-dump grid.
std::vector<std::string> GetHackingTextLines()
{
    std::vector<std::string> out;
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (m) CollectHackTextLines(m, 0, out);
    return out;
}

// UI screen position of the idx-th candidate word's middle letter (for the
// mouse-driven guess). Returns false if not open / index invalid.
bool GetHackWordTarget(int idx, float* x, float* y)
{
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m || idx < 0) return false;
    std::vector<HackWord> words;
    BuildHackWords(m, words);
    if (idx >= (int)words.size()) return false;
    if (x) *x = words[idx].tx;
    if (y) *y = words[idx].ty;
    return true;
}

// The hacking cursor's current UI position (feedback for driving the mouse).
bool GetHackingCursor(float* x, float* y)
{
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m) return false;
    Tile* c = FindChildByName(m, "hacking_cursor", 10);
    if (!c) return false;
    if (x) *x = TileAbsX(c) + TileNum(c, kTileValue_width) * 0.5f;
    if (y) *y = TileAbsY(c) + TileNum(c, kTileValue_height) * 0.5f;
    return true;
}

// True while the pause menu (Start) is up — hacking keys must stop then.
bool IsPauseMenuOpen() { return FindVisibleMenuTile(kMenuType_Start) != nullptr; }

// Read the current VATS selection straight from the VATS menu tiles (the game
// renders the target name, highlighted body part and hit chance as UI tiles —
// see an F11 VATS dump). Returns e.g. "Radrakan, Korpus, 92%" for the currently
// highlighted limb, or nullopt if VATS isn't open / has no target. The full
// engine VATS structs aren't needed.
std::optional<std::string> GetVatsSelectionText()
{
    Tile* vats = FindVisibleMenuTile(kMenuType_VATS);
    if (!vats) return std::nullopt;

    auto clean = [](std::string s) {
        while (!s.empty() && (s.back() == '\t' || s.back() == ' ' ||
                              s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        return s;
    };

    std::string enemy, limb, chance;
    if (Tile* eh = FindChildByName(vats, "EnemyHealth", 6))
        if (const char* s = TileLabelDeep(eh, 0)) enemy = clean(GameStrToUtf8(s));
    if (Tile* ln = FindChildByName(vats, "limb_name", 8))
        if (const char* s = TileStringTrait(ln)) limb = clean(GameStrToUtf8(s));
    if (Tile* ct = FindChildByName(vats, "chance_to_hit", 8))
        if (const char* s = TileStringTrait(ct)) chance = clean(GameStrToUtf8(s));

    std::string res;
    auto add = [&](const std::string& p) {
        if (!p.empty()) { if (!res.empty()) res += ", "; res += p; }
    };
    add(enemy); add(limb); add(chance);
    if (res.empty()) return std::nullopt;
    return res;
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
