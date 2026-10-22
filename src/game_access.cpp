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
#include "f3a/strings.h"
#include "fose/GameOSDepend.h"
#include "f3a/polling_loop.h"
#include "f3a/questobjs.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace f3a::game {

namespace rt = fose_rt;

// The game's own UI text (tile labels, item names) is stored in the
// localization's ANSI code page — Polish FO3 uses Windows-1250, English 1252.
// Our internal pipeline is UTF-8, so convert game strings on the way out.
// Without this, Polish letters (ą ć ę ł ń ó ś ź ż) read as garbage because
// the bytes get reinterpreted as UTF-8.
static UINT ResolveGameCodepage()
{
    // The INI's, or the GAME's language's (the mod may speak another one):
    // Polish 1250, Russian 1251, Western 1252, otherwise the system's.
    return (UINT)config::GameTextCodepage();
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
    // fmod, not a subtract-loop: an infinite or huge value never leaves such
    // a loop, and this runs on every poll.
    if (!std::isfinite(deg)) return 0.0f;
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    if (!(deg < 360.0f)) deg = 0.0f;
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
    return GameStrToUtf8(raw);   // the game's code page, not UTF-8
}

// ---- Quest target ---------------------------------------------------------

UInt8* TrackedObjective();   // below, after the quest helpers

QuestTarget GetCurrentQuestTarget()
{
    QuestTarget t{};
    // The tracked quest's objective (see TrackedObjective). BGSQuestObjective
    // .displayText (String @0x08) is the objective line, e.g. "Find the Galaxy
    // News Radio building".
    UInt8* obj = TrackedObjective();
    if (!obj) return t;
    const char* raw = *reinterpret_cast<const char* const*>(obj + 0x08);
    if (!raw || IsBadReadPtr(raw, 1) || !*raw) return t;
    t.name     = GameStrToUtf8(raw);
    t.position = { 0.0f, 0.0f, 0.0f };
    t.valid    = true;

    // Target marker (decoded from live dumps): objective + 0x14 -> Target*,
    // Target + 0x0C -> TESObjectREFR* -> world position at posX/Y/Z (0x2C).
    // The compass arrow points to this reference. Guarded; if anything looks
    // off we leave position {0,0,0} and the caller reads text only.
    if (IsBadReadPtr(obj + 0x14, 4)) return t;
    UInt8* target = *reinterpret_cast<UInt8**>(obj + 0x14);
    if (IsBadReadPtr(target, 0x10)) return t;
    auto* refr = *reinterpret_cast<TESObjectREFR**>(target + 0x0C);
    if (IsBadReadPtr(refr, 0x38)) return t;
    if (refr->baseForm == nullptr) return t;
    t.refr = refr;          // target ref → cross-worldspace check / cell-aware teleport
    t.refid = refr->refID;
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
    if (!p) return nullptr;
    // PlayerCharacter::quest (+0x614) is the quest the player tracks; the
    // Pip-Boy's quest list sets it at once (0x00770580). The objective (+0x618)
    // follows only later, so going by the objective's quest named the OLD quest
    // right after a new one was picked: "nieśledzone" after Enter.
    if (auto* tq = reinterpret_cast<UInt8*>(p->quest))
        if (!IsBadReadPtr(tq, 0x38)) return tq;
    if (!p->questObjective) return nullptr;
    auto* obj = reinterpret_cast<UInt8*>(p->questObjective);
    if (IsBadReadPtr(obj, 0x14)) return nullptr;
    // BGSQuestObjective+0x10 = TESQuest* (after displayText String @0x08, before
    // the targets tList @0x14). Verified from an F11 objective dump.
    UInt8* q = *reinterpret_cast<UInt8**>(obj + 0x10);
    if (IsBadReadPtr(q, 0x38)) return nullptr;
    return q;
}

namespace {
bool ObjectiveLive(const UInt8* o)
{
    if (!o || IsBadReadPtr(o, 0x24)) return false;
    UInt32 f = *reinterpret_cast<const UInt32*>(o + 0x20);
    return (f & 1) && !(f & 2);                        // displayed, not done
}
} // namespace

// The objective the mod follows: the player's own (+0x618) when it belongs
// to the tracked quest (+0x614) and is still live; else the tracked quest's
// live objective (displayed, not done), one with a map marker first, the
// newest of them. +0x618 lags the Pip-Boy and can even be another quest's
// ("Pomóż Moirze..." while the main quest was tracked), so trusting it alone
// left the scanner without a quest and could aim the marker at the wrong one.
UInt8* TrackedObjective()
{
    auto* q = reinterpret_cast<UInt8*>(const_cast<void*>(GetTrackedQuestPtr()));
    if (!q || IsBadReadPtr(q, 0x54)) return nullptr;
    auto* p = rt::Player();
    if (p && p->questObjective) {
        auto* o = reinterpret_cast<UInt8*>(p->questObjective);
        if (!IsBadReadPtr(o, 0x24) && *reinterpret_cast<UInt8**>(o + 0x10) == q &&
            ObjectiveLive(o))
            return o;
    }
    struct Node { UInt8* item; Node* next; };
    Node* on = reinterpret_cast<Node*>(q + 0x4C);
    UInt8* best = nullptr;
    bool bestMarked = false;
    for (int s = 0; on && s < 64; ++s) {
        if (IsBadReadPtr(on, 8)) break;
        UInt8* obj = on->item;
        on = on->next;
        if (!ObjectiveLive(obj)) continue;
        Vec3 where{};
        bool marked = ObjectiveMarkerPos(obj, where);
        if (marked) { best = obj; bestMarked = true; }
        else if (!bestMarked) best = obj;
    }
    return best;
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

// TESQuest+0x3C is a FLAGS byte (FOSE calls it "running"): bit 0 running,
// bit 1 completed, bit 5 shown in the Pip-Boy. The values a save shows: 0x25
// the main quest under way, 0x26 the finished prologue quests, 0x11/0x15 quests
// running in the background that nobody has given the player yet (the
// Wasteland Survival Guide until Moira is asked about her book). "!= 0" counted
// all of them as running - finished ones included.
static bool QuestRunning(const UInt8* q) { return (q[0x3C] & 0x01) != 0; }

// Has the player got this quest - is any of its objectives on the list (done
// or not)? A quest running in the background has none shown.
static bool QuestKnown(const UInt8* q)
{
    struct Node { UInt8* item; Node* next; };
    const Node* on = reinterpret_cast<const Node*>(q + 0x4C);
    for (int s2 = 0; on && s2 < 64; ++s2) {
        if (IsBadReadPtr(on, 8)) break;
        const UInt8* obj = on->item;
        on = on->next;
        if (obj && !IsBadReadPtr(obj, 0x24) && (*reinterpret_cast<const UInt32*>(obj + 0x20) & 1))
            return true;
    }
    return false;
}

std::vector<std::string> RunningQuestNames()
{
    std::vector<std::string> out;
    UInt8* dh = rt::g_addrs ? rt::DataHandlerBase() : nullptr;
    if (IsBadReadPtr(dh, 0xD8)) return out;
    struct Node { UInt8* item; Node* next; };
    Node* node = reinterpret_cast<Node*>(dh + 0xD4);
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (IsBadReadPtr(node, 8)) break;
        UInt8* q = node->item;
        node = node->next;
        if (!q || IsBadReadPtr(q, 0x54) || !QuestRunning(q) || !QuestKnown(q)) continue;
        const char* nm = *reinterpret_cast<char**>(q + 0x34);
        if (IsBadReadPtr(nm, 1) || !*nm) continue;
        out.push_back(GameStrToUtf8(nm));
    }
    return out;
}

std::vector<std::string> CompletedQuestNames()
{
    std::vector<std::string> out;
    UInt8* dh = rt::g_addrs ? rt::DataHandlerBase() : nullptr;
    if (IsBadReadPtr(dh, 0xD8)) return out;
    struct Node { UInt8* item; Node* next; };
    Node* node = reinterpret_cast<Node*>(dh + 0xD4);
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (IsBadReadPtr(node, 8)) break;
        UInt8* q = node->item;
        node = node->next;
        if (!q || IsBadReadPtr(q, 0x54) || !(q[0x3C] & 0x02) || !QuestKnown(q)) continue;
        const char* nm = *reinterpret_cast<char**>(q + 0x34);
        if (IsBadReadPtr(nm, 1) || !*nm) continue;
        out.push_back(GameStrToUtf8(nm));
    }
    return out;
}

std::vector<WorldEntity> GetActiveQuests(bool trackedOnly)
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

    // The tracked quest (PlayerCharacter+0x614) and the objective followed in
    // it - the tracked quest goes first, or alone with trackedOnly.
    const void* trackedQuest = GetTrackedQuestPtr();
    UInt8* trackedObj = TrackedObjective();

    struct Node { UInt8* item; Node* next; };
    Node* node = reinterpret_cast<Node*>(dh + 0xD4);
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (IsBadReadPtr(node, 8)) break;
        UInt8* q = node->item;
        node = node->next;
        if (!q || IsBadReadPtr(q, 0x54)) continue;
        if (!QuestRunning(q)) continue;                           // not running
        const char* qnm = *reinterpret_cast<char**>(q + 0x34);    // fullName
        if (IsBadReadPtr(qnm, 1) || !*qnm) continue;

        // Find a live objective (displayed & not completed). Prefer the tracked
        // one if this is the tracked quest; else the last live objective.
        const bool isTracked = q == trackedQuest;
        if (trackedOnly && !isTracked) continue;
        UInt8* live = nullptr;
        if (isTracked) {
            live = trackedObj;
        } else {
            Node* on = reinterpret_cast<Node*>(q + 0x4C);         // objectives @ +0x4C
            for (int s2 = 0; on && s2 < 64; ++s2) {
                if (IsBadReadPtr(on, 8)) break;
                UInt8* obj = on->item;
                on = on->next;
                if (ObjectiveLive(obj)) live = obj;               // the last live one
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
    // A container carries its TESContainer (0x0C) between TESBoundAnimObject
    // and the name, so its TESFullName sits at 0x3C, not 0x30. Reading 0x30
    // landed on the item list and every container came out as "Pojemnik".
    case kFormType_Container:
        off = 0x3C;
        break;
    // TESBoundAnimObject children — TESFullName directly after TESBoundObject.
    case kFormType_Activator:
    case kFormType_TalkingActivator:
    case kFormType_Terminal:
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
    // BGSNote: TESModel (0x30) comes first, so its name is at 0x48 too
    // (FOSE GameForms.h). Without it every note and holotape lying about was
    // unnamed and the scanner skipped it - Dad's last holotape included.
    case kFormType_Note:
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

bool IsFixtureType(UInt32 t)
{
    return t == kFormType_Activator || t == kFormType_TalkingActivator ||
           t == kFormType_Terminal  || t == kFormType_Furniture ||
           t == kFormType_Light;
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
    if (!Readable(r, 0x48)) return -1;
    const UInt8* base = reinterpret_cast<const UInt8*>(r);
    const UInt8* node = *reinterpret_cast<const UInt8* const*>(base + 0x44);
    for (int i = 0; node && i < 128; ++i) {
        if (!Readable(node, 0x10)) break;
        if (node[0x04] == 0x2A) {                       // kExtraData_Lock
            const UInt8* data = *reinterpret_cast<const UInt8* const*>(node + 0x0C);
            if (Readable(data, 0x0C)) {
                // flags bit 0 = locked: what the game's own GetLocked tests
                // (0x50F380 -> [lock+8] & 1).
                if (out_flags) *out_flags = data[0x08];
                // The level is ONE byte. The three after it are copied
                // unchanged from the plugin's record, where they are padding
                // full of leftovers — read as a whole word they turned a
                // "very hard" lock (100) into 4639076 and the walk announced
                // "needs a key".
                return (int)data[0x00];
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
// Walk a ref's extra-data list for one type. Returns the node, nullptr when
// the whole list was walked without it, or `bad` when the list could not be
// read to the end — so "no such data" is never confused with "unreadable".
const UInt8* const kExtraUnreadable = reinterpret_cast<const UInt8*>(1);
const UInt8* FindExtra(const TESObjectREFR* r, UInt8 type)
{
    if (!Readable(r, 0x48)) return kExtraUnreadable;
    const UInt8* node = *reinterpret_cast<const UInt8* const*>(
                            reinterpret_cast<const UInt8*>(r) + 0x44);
    for (int i = 0; node; ++i) {
        if (i >= 128 || !Readable(node, 0x10)) return kExtraUnreadable;
        if (node[0x04] == type) return node;
        node = *reinterpret_cast<const UInt8* const*>(node + 0x08);   // next
    }
    return nullptr;
}

// Door open state, read the way the game's GetOpenState does it (FO3
// 0x0044DF30 → ExtraDataList flag test 0x0040BAF0): bit 0x04 of the
// ExtraAction (type 0x0E) flags byte at +0x0C; a door with no ExtraAction has
// flags 1, i.e. shut. 1 = open, 0 = closed, -1 = couldn't read. (The swing
// animation part of GetOpenState reads the 3D and is left out.)
int ReadDoorOpenState(const TESObjectREFR* r)
{
    const UInt8* x = FindExtra(r, 0x0E);
    if (x == kExtraUnreadable) return -1;
    if (!x) return 0;
    return (x[0x0C] & 0x04) ? 1 : 0;
}

// 1 = a load door (ExtraTeleport, type 0x2B), 0 = ordinary, -1 = unknown.
int ReadIsLoadDoor(const TESObjectREFR* r)
{
    const UInt8* x = FindExtra(r, 0x2B);
    if (x == kExtraUnreadable) return -1;
    return x ? 1 : 0;
}

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

// What ContainerItemState saw, for the log line that lets us check it in play.
struct ContainerTally {
    int  forms    = 0;      // distinct item forms with a positive count
    bool levelled = false;  // base list holds a leveled list (rolled at runtime)
    bool changes  = false;  // the ref has ExtraContainerChanges
    int  entries  = 0;      // ...and how many entries it lists
};

// Is a container currently empty? Counted the way FOSE's own GetNumItems does
// it: per item form, the base default count (TESContainer.formCountList) plus
// the ref's ExtraContainerChanges countDelta (what's been looted or added since
// it spawned). Leveled lists in the base list are not items — what they rolled
// shows up as positive deltas in the changes instead. Per form, so a looted
// stimpak can't cancel out a different item still inside.
// Returns: 1 = has items, 0 = empty, -1 = couldn't tell. The scanner HIDES
// empty containers, so anything doubtful must come back -1, never 0.
//
// TESObjectCONT isn't laid out in the FOSE headers. It is TESBoundAnimObject
// (0x30), then TESContainer (vtable @0x30, formCountList head @0x34 — item@0,
// next@+0x04), then TESFullName @0x3C, as in New Vegas. The TESContainer
// vtable must point into the game image or the layout guess is wrong. All
// memory reads (no native call), so it's safe from the poll thread.
// `contOff` is where the base form keeps its TESContainer: +0x30 in a
// container (TESObjectCONT), +0x64 in an actor base (TESActorBase, FOSE).
int ContainerItemStateAt(TESForm* base, UInt32 contOff, const TESObjectREFR* r,
                         ContainerTally* out)
{
    ContainerTally t;
    if (!Readable(base, contOff + 0x18)) return -1;
    const UInt8* b = reinterpret_cast<const UInt8*>(base);
    UInt32 vt = *reinterpret_cast<const UInt32*>(b + contOff);
    if (!(vt >= 0x00400000 && vt < 0x01200000)) return -1;

    struct Count { const void* form; long n; };
    std::vector<Count> counts;
    auto add = [&](const void* form, long n) {
        for (auto& c : counts) if (c.form == form) { c.n += n; return; }
        counts.push_back({ form, n });
    };

    const UInt8* node = b + contOff + 0x04;                             // list head
    for (int i = 0; node; ++i) {
        if (i >= 256 || !Readable(node, 0x08)) return -1;
        const UInt8* fc = *reinterpret_cast<const UInt8* const*>(node); // FormCount*
        if (fc) {
            if (!Readable(fc, 0x08)) return -1;
            SInt32 cnt = *reinterpret_cast<const SInt32*>(fc);          // count @0
            auto* f = *reinterpret_cast<TESForm* const*>(fc + 0x04);     // form @4
            if (!Readable(f, 0x08)) return -1;
            if (f->typeID == kFormType_LeveledItem) t.levelled = true;
            else if (cnt > 0) add(f, cnt);
        }
        node = *reinterpret_cast<const UInt8* const*>(node + 0x04);     // next
    }

    // ExtraContainerChanges (type 0x15): Data*@+0x0C; Data->objList@+0x00 is a
    // tList<EntryData>*; each EntryData has countDelta @+0x04, form @+0x08.
    if (!Readable(r, 0x48)) return -1;
    const UInt8* xn = *reinterpret_cast<const UInt8* const*>(
                          reinterpret_cast<const UInt8*>(r) + 0x44);
    for (int i = 0; xn && i < 128; ++i) {
        if (!Readable(xn, 0x10)) return -1;
        if (xn[0x04] == 0x15) {                            // kExtraData_ContainerChanges
            const UInt8* data = *reinterpret_cast<const UInt8* const*>(xn + 0x0C);
            if (!Readable(data, 0x04)) return -1;
            t.changes = true;
            const UInt8* list = *reinterpret_cast<const UInt8* const*>(data);
            for (int j = 0; list; ++j) {
                if (j >= 512 || !Readable(list, 0x08)) return -1;
                const UInt8* ed = *reinterpret_cast<const UInt8* const*>(list);
                if (ed) {
                    if (!Readable(ed, 0x0C)) return -1;
                    ++t.entries;
                    add(*reinterpret_cast<const void* const*>(ed + 0x08),
                        *reinterpret_cast<const SInt32*>(ed + 0x04));
                }
                list = *reinterpret_cast<const UInt8* const*>(list + 0x04);
            }
            break;
        }
        xn = *reinterpret_cast<const UInt8* const*>(xn + 0x08);
    }

    for (const auto& c : counts) if (c.n > 0) ++t.forms;
    if (out) *out = t;
    if (t.forms > 0) return 1;
    // A leveled list with no changes yet may simply not have been rolled —
    // nor with a changes record that lists nothing: "empty" is said only when
    // the game's own record of the contents says so. (The scanner hides
    // empty containers, and a hidden container that held something is the
    // one mistake it must never make.)
    if (t.levelled && (!t.changes || t.entries == 0)) return -1;
    return 0;
}

int ContainerItemState(TESForm* base, const TESObjectREFR* r, ContainerTally* out)
{
    return ContainerItemStateAt(base, 0x30, r, out);
}

// The ref's ExtraContainerChanges entry list (tList<EntryData> head: item @0,
// next @4; EntryData = extendData @0, countDelta @4, form @8), or null.
const UInt8* ContainerChangesList(const TESObjectREFR* r)
{
    if (!Readable(r, 0x48)) return nullptr;
    const UInt8* xn = *reinterpret_cast<const UInt8* const*>(
                          reinterpret_cast<const UInt8*>(r) + 0x44);
    for (int i = 0; xn && i < 128; ++i) {
        if (!Readable(xn, 0x10)) return nullptr;
        if (xn[0x04] == 0x15) {                            // kExtraData_ContainerChanges
            const UInt8* data = *reinterpret_cast<const UInt8* const*>(xn + 0x0C);
            if (!Readable(data, 0x04)) return nullptr;
            return *reinterpret_cast<const UInt8* const*>(data);
        }
        xn = *reinterpret_cast<const UInt8* const*>(xn + 0x08);
    }
    return nullptr;
}

// The extra data of `type` in one ExtraDataList (vtable @0, first node @4;
// a node: type byte @4, next @8), or null.
const UInt8* FindExtra(const UInt8* xlist, UInt8 type)
{
    if (!Readable(xlist, 0x08)) return nullptr;
    const UInt8* xn = *reinterpret_cast<const UInt8* const*>(xlist + 0x04);
    for (int i = 0; xn && i < 64; ++i) {
        if (!Readable(xn, 0x10)) return nullptr;
        if (xn[0x04] == type) return xn;
        xn = *reinterpret_cast<const UInt8* const*>(xn + 0x08);
    }
    return nullptr;
}

// How many of the form with this id a ref carries: its base container's count
// plus the changes record's delta, as FOSE's GetItemCount counts it.
// `contOff` = where the base keeps its TESContainer (+0x64 for actors).
int ItemCountIn(const TESObjectREFR* r, UInt32 contOff, uint32_t formId)
{
    if (!Readable(r, 0x48) || !Readable(r->baseForm, contOff + 0x0C)) return -1;
    long n = 0;
    const UInt8* node = reinterpret_cast<const UInt8*>(r->baseForm) + contOff + 0x04;
    for (int i = 0; node && i < 256; ++i) {
        if (!Readable(node, 0x08)) break;
        const UInt8* fc = *reinterpret_cast<const UInt8* const*>(node);   // FormCount*
        if (fc && Readable(fc, 0x08)) {
            auto* f = *reinterpret_cast<TESForm* const*>(fc + 0x04);
            if (Readable(f, 0x10) && f->refID == formId)
                n += *reinterpret_cast<const SInt32*>(fc);
        }
        node = *reinterpret_cast<const UInt8* const*>(node + 0x04);
    }
    for (const UInt8* list = ContainerChangesList(r); list; ) {
        if (!Readable(list, 0x08)) break;
        const UInt8* ed = *reinterpret_cast<const UInt8* const*>(list);
        if (ed && Readable(ed, 0x0C)) {
            auto* f = *reinterpret_cast<TESForm* const*>(ed + 0x08);
            if (Readable(f, 0x10) && f->refID == formId)
                n += *reinterpret_cast<const SInt32*>(ed + 0x04);
        }
        list = *reinterpret_cast<const UInt8* const*>(list + 0x04);
    }
    return n < 0 ? 0 : static_cast<int>(n);
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
        if (!(dx * dx + dy * dy <= r2)) continue;   // NaN fails too

        const char* nm = BaseFormName(base);
        std::string label;
        if (nm) {
            label = GameStrToUtf8(nm);
        } else if (kind == WorldEntity::Kind::Door) {
            label = strings::T("Drzwi", "Door");       // unnamed load doors = metro entrances
        } else if (kind == WorldEntity::Kind::Container) {
            label = strings::T("Pojemnik", "Container");
        } else {
            continue;              // unnamed item/actor/etc. — skip noise
        }

        // Load doors: append/replace with WHERE the door leads so the player
        // hears "Drzwi do: Składzik Moiry", not a bare "Drzwi". A named local
        // door (e.g. "Drzwi Krypty 101") keeps its name plus the destination.
        if (kind == WorldEntity::Kind::Door) {
            std::string dest = DoorDestinationName(r);
            if (!dest.empty()) {
                label = (nm && label != dest) ? label + strings::T(", do: ", ", to ") + dest
                                              : strings::T("Drzwi do: ", "Door to ") + dest;
            }
        }

        // Containers: a blind player can't see that a crate is empty, so the
        // scanner leaves empty ones out (or tags them "pusty" if the INI turns
        // hiding off). Only a sure 0 counts — a -1 (unknown) stays listed under
        // its plain name so real loot is never hidden.
        int cont_state = -1;
        if (kind == WorldEntity::Kind::Container) {
            ContainerTally tally;
            int st = ContainerItemState(base, r, &tally);
            cont_state = st;
            {
                // Logged once per container and state: the scanner now
                // rescans on every category step.
                static std::mutex s_mx;
                static std::vector<std::pair<uint32_t, int>> s_seen;
                std::lock_guard<std::mutex> lk(s_mx);
                bool seen = false;
                for (const auto& q : s_seen) if (q.first == r->refID && q.second == st) seen = true;
                if (!seen && s_seen.size() < 8192) {
                    s_seen.push_back({ r->refID, st });
                    F3A_INFO("Cont: '%s' state=%d (1=full 0=empty -1=?) forms=%d lvl=%d "
                             "changes=%d entries=%d", label.c_str(), st, tally.forms,
                             (int)tally.levelled, (int)tally.changes, tally.entries);
                }
            }
            if (st == 0) {
                if (config::Get().hide_empty_containers) continue;
                label += strings::T(", pusty", ", empty");
            }
        }

        WorldEntity e;
        e.kind     = kind;
        e.name     = std::move(label);
        e.position = { r->posX, r->posY, r->posZ };
        e.form_id  = r->refID;
        e.refr     = r;            // for crosshair-forced activation
        e.form_type = (uint8_t)base->typeID;
        e.base_id  = base->refID;
        if (kind == WorldEntity::Kind::Container) e.items = cont_state;
        if (kind == WorldEntity::Kind::Door) {
            e.load_door  = ReadIsLoadDoor(r);
            e.open_state = ReadDoorOpenState(r);
        }
        if (kind == WorldEntity::Kind::Door ||
            kind == WorldEntity::Kind::Container) {
            int flags = -1;
            int lvl = ReadLockLevel(r, &flags);
            if (lvl >= 0) {
                e.lock_level = lvl;
                // Bit 0 = locked, as the game's GetLocked reads it (0x50F380).
                e.locked = (flags & 0x01) != 0;
                // Logged once per reference: auto-walk now checks every route
                // for locked doors, and the scan behind that runs often.
                static std::mutex s_logged_mutex;
                static std::vector<uint32_t> s_logged;
                std::lock_guard<std::mutex> lk(s_logged_mutex);
                if (s_logged.size() < 4096 &&
                    std::find(s_logged.begin(), s_logged.end(), r->refID) == s_logged.end()) {
                    s_logged.push_back(r->refID);
                    F3A_INFO("Lock: '%s' level=%d flags=0x%02X -> locked=%d",
                             e.name.c_str(), lvl, flags, (int)e.locked);
                }
            }
        }
        out.push_back(std::move(e));
    }
}

} // namespace

// Is this reference an actor that is fighting the PLAYER?
//
// "Nearest enemy" used to mean "nearest actor", which in a vault full of
// neighbours is almost never the enemy. The engine already knows the answer:
// its own GetCombatTarget script command (0x00529960) resolves the actor and
// then calls a VIRTUAL method at vtable +0x420, reading the form id at +0x0C
// off whatever comes back. So that slot is Actor::GetCombatTarget(), and an
// actor whose combat target is the player is hostile to the player — the same
// test the FNV accessibility mod uses, by a different route (it reads a raw
// field, which would be a guess on this build; a virtual slot is not).
//
// A virtual call into the engine, so: every pointer checked before use, and an
// INI switch to turn the whole thing off if it ever misbehaves.
namespace {
std::atomic<DWORD>    g_main_tid{ 0 };
std::mutex            g_hostile_mx;
std::vector<uint32_t> g_hostile_ids;
} // namespace

void NoteMainThread() { g_main_tid.store(GetCurrentThreadId()); }
bool OnMainThread()
{
    DWORD t = g_main_tid.load();
    return t != 0 && t == GetCurrentThreadId();
}

bool IsFixture(const WorldEntity& e) { return IsFixtureType(e.form_type); }

bool IsHostileToPlayer(const void* refr)
{
    // A virtual call into the actor: the game's thread only. Elsewhere the
    // answer is "not known to be hostile".
    if (!OnMainThread()) return false;
    if (!refr || !config::Get().hostile_from_combat) return false;
    auto* r = reinterpret_cast<TESObjectREFR*>(const_cast<void*>(refr));
    if (IsBadReadPtr(r, 0x70)) return false;
    auto* bf = r->baseForm;
    if (!bf || IsBadReadPtr(bf, 0x10)) return false;
    if (KindOf(bf->typeID) != WorldEntity::Kind::Actor) return false;

    void** vt = *reinterpret_cast<void***>(r);
    if (!vt || IsBadReadPtr(vt, 0x424)) return false;
    void* fn = vt[0x420 / 4];
    if (!fn || IsBadReadPtr(fn, 1)) return false;

    using GetTargetFn = void* (__thiscall*)(void* self);
    void* tgt = reinterpret_cast<GetTargetFn>(fn)(r);
    if (!tgt || IsBadReadPtr(tgt, 0x10)) return false;
    return tgt == (void*)rt::Player();
}

// The player's cell and, outdoors, the loaded cells around it (two squares
// each way): exteriors are split into a grid of cells and the player's
// parentCell holds only its own square, so a metro door one square over is
// invisible from it. Walks the worldspace cell map (objectList non-empty =
// loaded). Uses only FOSE-defined layouts; heavily guarded.
namespace {
std::vector<TESObjectCELL*> NearbyCells(TESObjectCELL* pcell)
{
    std::vector<TESObjectCELL*> cells{ pcell };
    auto* ws = reinterpret_cast<UInt8*>(pcell->worldSpace);
    auto* pcoords = reinterpret_cast<UInt8*>(pcell->coords);   // cell+0x44
    if (!Readable(ws, 0x34) || !Readable(pcoords, 8)) return cells;
    int px = *reinterpret_cast<int*>(pcoords + 0);
    int py = *reinterpret_cast<int*>(pcoords + 4);
    // TESWorldSpace::cellMap @ +0x30 (NiTPointerMap<TESObjectCELL>).
    UInt8* map = *reinterpret_cast<UInt8**>(ws + 0x30);
    if (!Readable(map, 0x10)) return cells;
    UInt32 nb = *reinterpret_cast<UInt32*>(map + 0x04);     // m_numBuckets
    UInt8** buckets = *reinterpret_cast<UInt8***>(map + 0x08);
    if (!(nb > 0 && nb < 1000000) || !Readable(buckets, nb * sizeof(void*))) return cells;
    for (UInt32 b = 0; b < nb && cells.size() <= 64; ++b) {
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
            if (std::abs(cx - px) > 2 || std::abs(cy - py) > 2) continue;
            cells.push_back(cell);
        }
    }
    return cells;
}
} // namespace

std::vector<WorldEntity> ScanNearby(int radius, int max_results,
                                    bool actors_only, bool hostiles_only)
{
    std::vector<WorldEntity> out;
    auto* p = rt::Player();
    if (!p || !p->parentCell) return out;
    Vec3 pp = GetPlayerPosition();
    const float r2 = (float)radius * (float)radius;

    // The player's cell and the loaded ones around it (NearbyCells).
    for (TESObjectCELL* cell : NearbyCells(p->parentCell))
        ScanCellRefs(cell, pp, r2, p, actors_only, out);

    // Mark the hostiles, and drop everything else when only those were asked
    // for. Done here rather than inside the cell walk so the virtual call is
    // made once per candidate, on a list that is already distance-filtered.
    // The hostility test is a virtual call and this runs on the poll thread, so
    // the answer comes from the set the main thread keeps (RefreshHostiles).
    // It used to be a lambda that was never called — `hostile` was always
    // false and "hostiles only" returned every actor, friends included.
    auto MarkHostiles = [&]() {
        for (auto& e : out)
            if (e.kind == WorldEntity::Kind::Actor) {
                e.hostile = IsKnownHostile(e.form_id);
                e.dead    = IsKnownDead(e.form_id);
                // A body: anything left on it? (Its worn clothes count too.)
                if (e.dead && e.refr) {
                    auto* r = reinterpret_cast<const TESObjectREFR*>(e.refr);
                    if (Readable(r, 0x40))
                        e.items = ContainerItemStateAt(r->baseForm, 0x64, r, nullptr);
                }
            }
        if (!hostiles_only) return;
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](const WorldEntity& e) { return !e.hostile; }),
                  out.end());
    };

    MarkHostiles();

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

std::vector<WorldEntity> GetQuestObjects(int radius, int max_results)
{
    std::vector<WorldEntity> out;
    auto* p = rt::Player();
    if (!p || !p->parentCell || !rt::g_addrs) return out;

    // Where the quests stand: DataHandler::questList (+0xD4), plain reads -
    // running (+0x3C), stage (+0x60), name (+0x34), objectives (+0x4C:
    // objectiveId +0x04, flags +0x20: 1 displayed, 2 completed).
    struct QState {
        bool running = false;
        int stage = 0;
        std::string name;
        std::vector<std::pair<UInt32, UInt32>> objectives;   // id, flags
    };
    std::unordered_map<uint32_t, QState> quests;
    const UInt8* tracked = reinterpret_cast<const UInt8*>(GetTrackedQuestPtr());
    if (!tracked) return out;                       // nothing tracked: nothing to show
    UInt8* dh = rt::DataHandlerBase();
    if (!Readable(dh, 0xDC)) return out;
    struct Node { UInt8* item; Node* next; };
    Node* node = reinterpret_cast<Node*>(dh + 0xD4);
    for (int safety = 0; node && safety < 8192; ++safety) {
        if (!Readable(node, 8)) break;
        UInt8* q = node->item;
        node = node->next;
        if (!q || !Readable(q, 0x64)) continue;
        QState s;
        s.running = QuestRunning(q);
        // Only quests the player has: running, and on the Pip-Boy's list. One
        // running in the background (not given yet) must not name its things
        // after a quest the Pip-Boy does not show.
        if (!s.running || !QuestKnown(q)) continue;
        // Only the quest the player tracks: the category is for the quest being
        // worked on, not every quest's buttons and spots at once.
        if (q != tracked) continue;
        s.stage = q[0x60];
        const char* nm = *reinterpret_cast<char**>(q + 0x34);
        if (Readable(nm, 1) && *nm) s.name = GameStrToUtf8(nm);
        Node* on = reinterpret_cast<Node*>(q + 0x4C);
        for (int s2 = 0; on && s2 < 64; ++s2) {
            if (!Readable(on, 8)) break;
            UInt8* obj = on->item;
            on = on->next;
            if (!obj || !Readable(obj, 0x24)) continue;
            s.objectives.push_back({ *reinterpret_cast<UInt32*>(obj + 0x04),
                                     *reinterpret_cast<UInt32*>(obj + 0x20) });
        }
        quests[reinterpret_cast<TESForm*>(q)->refID] = std::move(s);
    }
    if (quests.empty()) return out;

    auto relevant = [&](const questobjs::Entry& e, const QState** qs) {
        auto it = quests.find(e.quest);
        if (it == quests.end()) return false;       // not running
        *qs = &it->second;
        if (e.objective >= 0) {
            for (const auto& o : it->second.objectives)
                if (o.first == (UInt32)e.objective) return (o.second & 1) && !(o.second & 2);
            return false;
        }
        return e.stage == 0 || it->second.stage < e.stage;
    };

    Vec3 pp = GetPlayerPosition();
    const float r2 = (float)radius * (float)radius;
    std::vector<Vec3> sitters;                      // living actors, for seats
    std::vector<questobjs::Entry> ents;
    std::vector<bool> isSeat;
    for (TESObjectCELL* cell : NearbyCells(p->parentCell)) {
        if (!Readable(cell, 0xB4)) continue;
        struct RNode { TESObjectREFR* item; RNode* next; };
        auto* rn = reinterpret_cast<RNode*>(&cell->objectList);
        for (int safety = 0; rn && safety < 8192; ++safety) {
            if (!Readable(rn, 8)) break;
            TESObjectREFR* r = rn->item;
            rn = rn->next;
            if (!r || r == (void*)p || !Readable(r, 0x40)) continue;
            if (r->flags & 0x820) continue;         // disabled / deleted
            TESForm* base = r->baseForm;
            if (!Readable(base, 0x08)) continue;
            float dx = r->posX - pp.x, dy = r->posY - pp.y;
            WorldEntity::Kind kind = KindOf(base->typeID);
            if (kind == WorldEntity::Kind::Actor) {
                if (!IsKnownDead(r->refID) && dx * dx + dy * dy <= r2 * 4.0f)
                    sitters.push_back({ r->posX, r->posY, r->posZ });
                continue;
            }
            if (!(dx * dx + dy * dy <= r2)) continue;   // NaN fails too
            if (!questobjs::Lookup(r->refID, &ents)) continue;
            const questobjs::Entry* use = nullptr;
            const QState* qs = nullptr;
            for (const auto& e : ents)
                if (relevant(e, &qs)) { use = &e; break; }
            if (!use || !qs) continue;
            std::string label;
            if (const char* nm = BaseFormName(base)) label = GameStrToUtf8(nm);
            if (label.empty()) {
                label = (use->kind == questobjs::kTrigger || use->kind == questobjs::kMarker)
                            ? strings::T("Miejsce zadania", "Quest spot")
                            : use->kind == questobjs::kSeat ? strings::T("Miejsce do siedzenia", "Seat")
                                                            : strings::T("Obiekt zadania", "Quest object");
            }
            if (use->kind == questobjs::kSeat) label += strings::T(", wolne miejsce", ", free seat");
            if (!qs->name.empty()) label += ", " + qs->name;
            WorldEntity e;
            e.kind = kind;
            e.name = std::move(label);
            e.position = { r->posX, r->posY, r->posZ };
            e.form_id = r->refID;
            e.refr = r;
            e.form_type = (uint8_t)base->typeID;
            if (kind == WorldEntity::Kind::Door) {
                e.load_door  = ReadIsLoadDoor(r);
                e.open_state = ReadDoorOpenState(r);
            }
            if (kind == WorldEntity::Kind::Door || kind == WorldEntity::Kind::Container) {
                int flags = -1;
                int lvl = ReadLockLevel(r, &flags);
                if (lvl >= 0) { e.lock_level = lvl; e.locked = (flags & 0x01) != 0; }
            }
            out.push_back(std::move(e));
            isSeat.push_back(use->kind == questobjs::kSeat);
        }
    }
    // A seat someone is sitting at is not free: each actor takes the seat
    // nearest to them, if one is within reach of sitting at it.
    std::vector<bool> taken(out.size(), false);
    for (const auto& a : sitters) {
        int best = -1;
        float bd = 90.0f * 90.0f;
        for (size_t i = 0; i < out.size(); ++i) {
            if (!isSeat[i]) continue;
            float dx = out[i].position.x - a.x, dy = out[i].position.y - a.y;
            float dz = out[i].position.z - a.z;
            float d2 = dx * dx + dy * dy;
            if (d2 < bd && std::fabs(dz) < 120.0f) { bd = d2; best = (int)i; }
        }
        if (best >= 0) taken[best] = true;
    }
    std::vector<WorldEntity> kept;
    for (size_t i = 0; i < out.size(); ++i)
        if (!taken[i]) kept.push_back(std::move(out[i]));
    std::sort(kept.begin(), kept.end(), [&pp](const WorldEntity& a, const WorldEntity& b) {
        float da = (a.position.x - pp.x) * (a.position.x - pp.x) +
                   (a.position.y - pp.y) * (a.position.y - pp.y);
        float db = (b.position.x - pp.x) * (b.position.x - pp.x) +
                   (b.position.y - pp.y) * (b.position.y - pp.y);
        return da < db;
    });
    if ((int)kept.size() > max_results) kept.resize(max_results);
    return kept;
}

namespace {
std::vector<uint32_t> g_dead_ids;      // under g_hostile_mx
std::vector<HostileRef> g_hostile_refs;   // under g_hostile_mx

// Is this actor dead? The game's GetDead (0x0050F840) asks two virtuals:
// vtable+0x100 "is an actor" and vtable+0x22C IsDead(1). 1 dead, 0 alive,
// -1 unknown. Main thread only (virtual calls).
int ActorDeadState(const void* refr)
{
    auto* r = reinterpret_cast<TESObjectREFR*>(const_cast<void*>(refr));
    if (!Readable(r, 0x40)) return -1;
    void** vt = *reinterpret_cast<void***>(r);
    if (!Readable(vt, 0x230)) return -1;
    void* isActor = vt[0x100 / 4];
    void* isDead  = vt[0x22C / 4];
    if (!Readable(isActor, 1) || !Readable(isDead, 1)) return -1;
    using IsActorFn = bool (__thiscall*)(void*);
    using IsDeadFn  = bool (__thiscall*)(void*, bool);
    if (!reinterpret_cast<IsActorFn>(isActor)(r)) return -1;
    return reinterpret_cast<IsDeadFn>(isDead)(r, true) ? 1 : 0;
}

// The actors the game marks as enemies on the compass (the red marks).
// PlayerCharacter+0x900 is a list of nodes {entry*, next*}, entry = {Actor*,
// bool hostile}: read exactly so by the game's own lookup 0x007747A0, which
// its bullet correction (0x007794B0) uses to tell an enemy. Plain data.
void CompassHostiles(std::vector<const void*>* out)
{
    auto* pl = reinterpret_cast<UInt8*>(rt::Player());
    if (!Readable(pl, 0x904)) return;
    UInt8* node = *reinterpret_cast<UInt8**>(pl + 0x900);
    for (int guard = 0; node && guard < 512; ++guard) {
        if (!Readable(node, 8)) break;
        UInt8* entry = *reinterpret_cast<UInt8**>(node);
        if (!entry) break;                       // the game stops here too
        if (Readable(entry, 5) && entry[4] != 0)
            out->push_back(*reinterpret_cast<void**>(entry));
        node = *reinterpret_cast<UInt8**>(node + 4);
    }
}
} // namespace

void RefreshHostiles()
{
    if (!OnMainThread()) return;
    // An enemy is an actor fighting the player OR one the game itself counts
    // as hostile (the compass). The first test alone missed the radroaches
    // biting Butch's mother — they were fighting HER — so the aim key fell
    // back to the scanner's pick, which was the mother, and the shots hit her.
    std::vector<const void*> compass;
    CompassHostiles(&compass);
    std::vector<uint32_t> ids, dead;
    std::vector<HostileRef> refs;
    // Wider than anything the scanner lists (2400 units by default), and with
    // room for a crowd: a body left out here would be listed among the living.
    for (const auto& e : ScanNearby(8000, 256, /*actors_only*/true, false)) {
        if (!e.refr) continue;
        int d = ActorDeadState(e.refr);
        if (d == 1) { dead.push_back(e.form_id); continue; }
        bool onCompass = std::find(compass.begin(), compass.end(), e.refr) != compass.end();
        if (onCompass || IsHostileToPlayer(e.refr)) {
            ids.push_back(e.form_id);
            refs.push_back({ e.refr, e.form_id, e.name });
        }
    }
    std::lock_guard<std::mutex> lk(g_hostile_mx);
    g_hostile_ids.swap(ids);
    g_dead_ids.swap(dead);
    g_hostile_refs.swap(refs);
}

void ForgetHostiles()
{
    std::lock_guard<std::mutex> lk(g_hostile_mx);
    g_hostile_ids.clear();
    g_dead_ids.clear();
    g_hostile_refs.clear();
}

std::vector<HostileRef> GetHostileRefs()
{
    std::lock_guard<std::mutex> lk(g_hostile_mx);
    return g_hostile_refs;
}

int RefDeadState(const void* refr) { return ActorDeadState(refr); }

bool IsKnownHostile(uint32_t refid)
{
    if (!refid) return false;
    std::lock_guard<std::mutex> lk(g_hostile_mx);
    for (uint32_t id : g_hostile_ids) if (id == refid) return true;
    return false;
}

bool IsKnownDead(uint32_t refid)
{
    if (!refid) return false;
    std::lock_guard<std::mutex> lk(g_hostile_mx);
    for (uint32_t id : g_dead_ids) if (id == refid) return true;
    return false;
}

float GetGameHour()
{
    if (!OnMainThread()) return -1.0f;
    const UInt8 want[] = { 0x51, 0x8B, 0x0D, 0x14, 0xDD, 0x06, 0x01 };   // LookupFormByID
    auto* p = reinterpret_cast<const UInt8*>(0x00455190);
    if (!Readable(p, sizeof(want)) || std::memcmp(p, want, sizeof(want)) != 0) return -1.0f;
    using LookupFn = TESForm* (__cdecl*)(UInt32);
    TESForm* f = reinterpret_cast<LookupFn>(0x00455190)(0x00000038);   // GameHour
    if (!Readable(f, 0x28) || f->typeID != kFormType_Global) return -1.0f;
    const float h = *reinterpret_cast<const float*>(reinterpret_cast<const UInt8*>(f) + 0x24);
    return (std::isfinite(h) && h >= 0.0f && h <= 24.0f) ? h : -1.0f;
}

int GetQuestStage(uint32_t questId, bool* running)
{
    if (running) *running = false;
    if (!questId || !OnMainThread()) return -1;
    const UInt8 want[] = { 0x51, 0x8B, 0x0D, 0x14, 0xDD, 0x06, 0x01 };   // LookupFormByID
    auto* p = reinterpret_cast<const UInt8*>(0x00455190);
    if (!Readable(p, sizeof(want)) || std::memcmp(p, want, sizeof(want)) != 0) return -1;
    using LookupFn = TESForm* (__cdecl*)(UInt32);
    TESForm* f = reinterpret_cast<LookupFn>(0x00455190)(questId);
    if (!Readable(f, 0x64) || f->typeID != 0x47) return -1;          // kFormType_Quest
    auto* q = reinterpret_cast<const UInt8*>(f);
    if (running) *running = QuestRunning(q);
    return q[0x60];
}

std::vector<std::string> LoadedPlugins()
{
    // DataHandler::modList (+0x19C, FOSE GameData.h): tList (8 bytes), then
    // loadedModCount (+0x1A4) and ModInfo* loadedMods[0xFF] (+0x1A8), slot =
    // load-order byte; a ModInfo's file name is at +0x20 (char[0x104]).
    std::vector<std::string> out;
    if (!rt::g_addrs) return out;
    UInt8* dh = rt::DataHandlerBase();
    if (!Readable(dh, 0x1A8 + 0xFF * 4)) return out;
    UInt32 count = *reinterpret_cast<UInt32*>(dh + 0x1A4);
    if (!(count <= 0xFF)) return out;
    auto** mods = reinterpret_cast<UInt8**>(dh + 0x1A8);
    for (UInt32 i = 0; i < count; ++i) {
        UInt8* m = mods[i];
        if (!Readable(m, 0x124)) return {};
        const char* name = reinterpret_cast<const char*>(m + 0x20);
        size_t len = strnlen(name, 0x104);
        // "Must be within": a plugin file name, ending inside its field.
        if (len < 5 || len >= 0x104) return {};
        std::string ext(name + len - 4, 4);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".esm" && ext != ".esp") return {};
        out.emplace_back(name, len);
    }
    return out;
}

// A form by id, read from the game's own form table as plain data (any
// thread): NiTPointerMap<TESForm> {vtable, numBuckets +4, buckets +8},
// entries {next, key, data}, bucket = id % numBuckets. The table's address is
// taken from LookupFormByID itself (0x00455190: push ecx; mov ecx, [table]),
// checked byte for byte first.
TESForm* FormById(uint32_t id)
{
    const auto* fn = reinterpret_cast<const UInt8*>(0x00455190);
    if (!id || !Readable(fn, 7) || fn[0] != 0x51 || fn[1] != 0x8B || fn[2] != 0x0D) return nullptr;
    auto** tableRef = *reinterpret_cast<UInt8** const*>(fn + 3);
    if (!Readable(tableRef, 4)) return nullptr;
    UInt8* map = *tableRef;
    if (!Readable(map, 0x10)) return nullptr;
    UInt32 nb = *reinterpret_cast<UInt32*>(map + 4);
    auto** buckets = *reinterpret_cast<UInt8***>(map + 8);
    if (!(nb > 0 && nb < 0x1000000) || !Readable(buckets + (id % nb), 4)) return nullptr;
    UInt8* e = buckets[id % nb];
    for (int guard = 0; e && guard < 4096; ++guard) {
        if (!Readable(e, 0x0C)) return nullptr;
        if (*reinterpret_cast<UInt32*>(e + 4) == id) {
            auto* f = *reinterpret_cast<TESForm**>(e + 8);
            return Readable(f, 0x18) && f->refID == id ? f : nullptr;
        }
        e = *reinterpret_cast<UInt8**>(e);
    }
    return nullptr;
}

std::string SpaceDisplayName(uint32_t formId)
{
    TESForm* f = FormById(formId);
    // kFormType_Cell 0x39, kFormType_WorldSpace 0x41; both carry TESFullName
    // at +0x18, its String at +0x1C (char* data, UInt16 length +0x20).
    if (!f || (f->typeID != 0x39 && f->typeID != 0x41)) return {};
    auto* b = reinterpret_cast<UInt8*>(f);
    if (!Readable(b, 0x24)) return {};
    const char* s = *reinterpret_cast<const char**>(b + 0x1C);
    UInt16 len = *reinterpret_cast<UInt16*>(b + 0x20);
    if (!s || len == 0 || len > 200 || !Readable(s, len)) return {};
    return GameStrToUtf8(std::string(s, len).c_str());
}

bool ReadQuestState(uint32_t questId, int* stage, bool* running)
{
    if (!questId || !rt::g_addrs) return false;
    static std::mutex s_mx;
    static std::unordered_map<uint32_t, UInt8*> s_found;
    auto valid = [questId](UInt8* q) {
        auto* f = reinterpret_cast<TESForm*>(q);
        return Readable(q, 0x64) && f->typeID == 0x47 && f->refID == questId;   // kFormType_Quest
    };
    UInt8* q = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        auto it = s_found.find(questId);
        if (it != s_found.end()) q = it->second;
    }
    if (!q || !valid(q)) {
        q = nullptr;
        UInt8* dh = rt::DataHandlerBase();
        if (!Readable(dh, 0xDC)) return false;
        struct Node { UInt8* item; Node* next; };
        Node* node = reinterpret_cast<Node*>(dh + 0xD4);           // questList
        for (int safety = 0; node && safety < 8192; ++safety) {
            if (!Readable(node, 8)) break;
            UInt8* item = node->item;
            node = node->next;
            if (item && valid(item)) { q = item; break; }
        }
        if (!q) return false;
        std::lock_guard<std::mutex> lk(s_mx);
        s_found[questId] = q;
    }
    if (stage) *stage = q[0x60];
    if (running) *running = QuestRunning(q);
    return true;
}

bool RefIsActor(const void* refr)
{
    auto* r = reinterpret_cast<const TESObjectREFR*>(refr);
    if (!Readable(r, 0x40)) return false;
    const TESForm* b = r->baseForm;
    return Readable(b, 0x08) && KindOf(b->typeID) == WorldEntity::Kind::Actor;
}

bool RefIsCreature(const void* refr)
{
    auto* f = reinterpret_cast<const TESForm*>(refr);
    return Readable(f, 0x10) && f->typeID == 0x3C;     // ACRE, a placed creature
}

std::string RefActorName(uint32_t refid, bool* isActor)
{
    if (isActor) *isActor = false;
    if (!refid || !OnMainThread()) return {};
    using LookupFn = TESForm* (__cdecl*)(UInt32);
    static const bool ok = [] {
        // LookupFormByID; prologue checked before the first call (see
        // GetSpecialDescription, which verified the same bytes).
        const UInt8 want[] = { 0x51, 0x8B, 0x0D, 0x14, 0xDD, 0x06, 0x01 };
        auto* p = reinterpret_cast<const UInt8*>(0x00455190);
        return Readable(p, sizeof(want)) && std::memcmp(p, want, sizeof(want)) == 0;
    }();
    if (!ok) return {};
    TESForm* f = reinterpret_cast<LookupFn>(0x00455190)(refid);
    // Only a placed actor (ACHR 0x3B / ACRE 0x3C) has the reference layout.
    if (!Readable(f, 0x40) || (f->typeID != 0x3B && f->typeID != 0x3C) || !RefIsActor(f))
        return {};
    if (isActor) *isActor = true;
    const char* nm = BaseFormName(reinterpret_cast<TESObjectREFR*>(f)->baseForm);
    return nm ? GameStrToUtf8(nm) : std::string();
}

// ---- The engine's bullet correction (FO3 1.7.0.3) --------------------------
//
// Read from Fallout3.exe (2026-10-15). The shot-direction code 0x00773570
// (called from the fire sites) bends a ranged shot toward the target at
// PlayerCharacter+0x8E4 by at most fAutoAimMaxDegrees (fAutoAimMaxDegrees3rd-
// Person when +0x5A9 is set; the VATS and melee settings are other branches,
// left alone). That target is chosen by 0x007794B0 from the high-process
// actors: alive, with 3D, flagged hostile in the player's compass-target list
// (+0x900), nearer than fAutoAimMaxDistance, inside a screen window of
// ±fAutoAimScreenPercentage*0.005 round the crosshair, and reached by a ray.
// Each Setting is { vtable, float value @+4, const char* name @+8 }; the
// names are checked before anything is written, so another build stays off.
namespace {
struct AutoAimSetting { UInt32 addr; const char* name; };
const AutoAimSetting kAutoAim[] = {
    { 0x00F5FC84, "fAutoAimMaxDegrees" },
    { 0x00F5FC90, "fAutoAimMaxDegrees3rdPerson" },
    { 0x00F5FC78, "fAutoAimScreenPercentage" },
    { 0x00F5FC6C, "fAutoAimMaxDistance" },
};
constexpr int kAutoAimCount = sizeof(kAutoAim) / sizeof(kAutoAim[0]);
float g_autoaim_saved[kAutoAimCount] = {};
bool  g_autoaim_applied  = false;
int   g_autoaim_verified = -1;          // -1 unknown, 0 no, 1 yes

bool AutoAimSettingsOk()
{
    if (g_autoaim_verified < 0) {
        bool ok = true;
        for (const auto& st : kAutoAim) {
            auto* p = reinterpret_cast<const UInt8*>(st.addr);
            if (!Readable(p, 12)) { ok = false; break; }
            const char* nm = *reinterpret_cast<const char* const*>(p + 8);
            if (!nm || !Readable(nm, 32) || std::strcmp(nm, st.name) != 0) { ok = false; break; }
        }
        g_autoaim_verified = ok ? 1 : 0;
        F3A_INFO("EngineAim: game settings %s", ok ? "verified"
                 : "do not match this build - engine bullet correction stays off");
    }
    return g_autoaim_verified == 1;
}

float* AutoAimValue(int i) { return reinterpret_cast<float*>(kAutoAim[i].addr + 4); }
} // namespace

void SetEngineAutoAim(bool on, float degrees)
{
    if (!OnMainThread() || on == g_autoaim_applied || !AutoAimSettingsOk()) return;
    if (on) {
        for (int i = 0; i < kAutoAimCount; ++i) g_autoaim_saved[i] = *AutoAimValue(i);
        // Cone: the degrees asked for. Window: wide enough for an actor that
        // far off the crosshair to be picked at all (about 4x, by the 0.005
        // scale above). Range: as far as the scanner looks for enemies.
        const float want[kAutoAimCount] = { degrees, degrees, degrees * 4.0f, 5000.0f };
        for (int i = 0; i < kAutoAimCount; ++i) {
            float v = g_autoaim_saved[i];
            if (std::isfinite(want[i]) && want[i] > v) v = want[i];   // never narrower
            *AutoAimValue(i) = v;
        }
        g_autoaim_applied = true;
        F3A_INFO("EngineAim: on - cone %.1f (3rd person %.1f), window %.1f, range %.0f "
                 "(game values %.1f, %.1f, %.1f, %.0f)",
                 *AutoAimValue(0), *AutoAimValue(1), *AutoAimValue(2), *AutoAimValue(3),
                 g_autoaim_saved[0], g_autoaim_saved[1], g_autoaim_saved[2],
                 g_autoaim_saved[3]);
    } else {
        for (int i = 0; i < kAutoAimCount; ++i) *AutoAimValue(i) = g_autoaim_saved[i];
        g_autoaim_applied = false;
        F3A_INFO("EngineAim: off - game values restored");
    }
}

// SPECIALBookMenu, FO3 1.7.0.3 (vtable 0x00E04834). The same fields the FNV
// mod reads, at the same offsets — read from its HandleClick 0x00676430 and
// its page-to-attribute routine 0x00675BE0: +0x48 the page (0 cover, 1-7 =
// actor values 5-11, 8 the summary page), +0x4C the attribute picked on the
// summary page (1-7; index_up/index_down step it), +0x58 the points there
// are to spend (compared with the sum of the seven by the increase button).
Menu* FindMenuByType(UInt32 type);   // defined below
bool GetSpecialBookState(int* page, int* indexStat, int* maxPoints)
{
    if (!OnMainThread()) return false;
    Menu* m = FindMenuByType(kMenuType_SPECIALBook);
    if (!m || !Readable(m, 0x5C)) return false;
    if (*reinterpret_cast<const UInt32*>(m) != 0x00E04834) return false;
    auto* b = reinterpret_cast<const UInt8*>(m);
    if (page)      *page      = *reinterpret_cast<const SInt32*>(b + 0x48);
    if (indexStat) *indexStat = *reinterpret_cast<const SInt32*>(b + 0x4C);
    if (maxPoints) *maxPoints = *reinterpret_cast<const SInt32*>(b + 0x58);
    return true;
}

// The SPECIAL attributes' AVIF forms are 0x3E8 (Strength) .. 0x3EE (Luck) in
// Fallout3.esm. ActorValueInfo = TESForm (0x18) + TESFullName (0x18) +
// TESDescription (0x24, vtable 0x00DBCBE8, Get = slot 4 = 0x00453960) — the
// FNV mod reads the same component the same way. Get loads the text from the
// plugin file into a shared buffer, so it is copied at once.
std::string GetSpecialDescription(int i)
{
    if (!OnMainThread() || i < 0 || i > 6) return {};
    constexpr UInt32 kLookupFormByID = 0x00455190;   // FOSE GameAPI.cpp, 1.7.0.3
    static const unsigned char kLookupSig[] = { 0x51, 0x8B, 0x0D, 0x14, 0xDD, 0x06, 0x01 };
    static const unsigned char kGetSig[]    = { 0xA1, 0xCC, 0xCD, 0x06, 0x01 };
    const void* lookup = reinterpret_cast<const void*>(kLookupFormByID);
    const void* get    = reinterpret_cast<const void*>(0x00453960);
    if (!Readable(lookup, sizeof(kLookupSig)) ||
        std::memcmp(lookup, kLookupSig, sizeof(kLookupSig)) != 0 ||
        !Readable(get, sizeof(kGetSig)) ||
        std::memcmp(get, kGetSig, sizeof(kGetSig)) != 0)
        return {};
    using LookupFn = TESForm* (__cdecl*)(UInt32);
    TESForm* f = reinterpret_cast<LookupFn>(kLookupFormByID)(0x3E8u + (UInt32)i);
    if (!f || !Readable(f, 0x2C)) return {};
    auto* desc = reinterpret_cast<UInt8*>(f) + 0x24;
    if (*reinterpret_cast<const UInt32*>(desc) != 0x00DBCBE8) return {};
    using GetFn = const char* (__thiscall*)(void*, void*, UInt32);
    const char* text = reinterpret_cast<GetFn>(0x00453960)(desc, f, 0x43534544u);  // 'DESC'
    if (!text || !Readable(text, 1) || !*text) return {};
    std::string out = GameStrToUtf8(text);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\r' || out.back() == '\n'))
        out.pop_back();
    return out;
}

uint32_t EngineAutoAimTargetId()
{
    if (!OnMainThread()) return 0;
    auto* p = reinterpret_cast<const UInt8*>(rt::Player());
    if (!p || !Readable(p + 0x8E4, 4)) return 0;
    auto* r = *reinterpret_cast<TESObjectREFR* const*>(p + 0x8E4);
    if (!r || !Readable(r, 0x10)) return 0;
    return r->refID;
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
    UInt8* obj = TrackedObjective();
    if (!p || !obj || !p->parentCell) return out;
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
        if (!(dx * dx + dy * dy <= r2)) continue;   // NaN fails too
        WorldEntity e;
        e.kind       = WorldEntity::Kind::Item;
        e.name       = strings::T("Cel", "Target");
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
    if (!p || !p->parentCell) { lines.push_back("no cell"); return lines; }
    Vec3 pp = GetPlayerPosition();
    const float r2 = (float)radius * (float)radius;
    TESObjectCELL* cell = p->parentCell;
    if (!Readable(cell, 0xB4)) { lines.push_back("cell unreadable"); return lines; }

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
        if (!(d2 <= r2)) continue;
        const char* nm = BaseFormName(base);
        std::string name = (nm && !IsBadReadPtr(nm, 1)) ? GameStrToUtf8(nm)
                                                        : std::string();
        // World position as well as range: with the player's own position in
        // the log, that is enough to work out on paper WHAT SITS IN THE LINE OF
        // FIRE. Distance alone cannot answer that, and "the shot keeps missing
        // although the aim is provably correct" is exactly the question left.
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "0x%08X typ=%u dist=%.0f dz=%.0f at=(%.0f,%.0f,%.0f) "
                      "flags=0x%X '%.60s'",
                      r->refID, base->typeID, std::sqrt(d2), dz,
                      r->posX, r->posY, r->posZ,
                      r->flags, name.c_str());
        rows.push_back({ std::sqrt(d2), buf });
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b){ return a.dist < b.dist; });
    char hdr[96];
    std::snprintf(hdr, sizeof(hdr), "%d refs in the cell, %d within %d",
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

std::vector<WorldEntity> GetNearbyLocations(float radius, int max_results)
{
    std::vector<WorldEntity> out;
    auto* p = rt::Player();
    if (!p || !p->parentCell) return out;
    auto* ws = reinterpret_cast<UInt8*>(p->parentCell->worldSpace);
    if (!Readable(ws, 0x38)) return out;                        // indoors
    // TESWorldSpace+0x34: the persistent cell (FOSE), where map markers live.
    auto* pcell = *reinterpret_cast<TESObjectCELL**>(ws + 0x34);
    if (!Readable(pcell, 0xB4)) return out;
    Vec3 pp = GetPlayerPosition();
    const float r2 = radius * radius;
    static int s_logged = 0;
    struct Node { TESObjectREFR* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&pcell->objectList);
    for (int safety = 0; node && safety < 65536; ++safety) {
        if (!Readable(node, 8)) break;
        TESObjectREFR* r = node->item;
        node = node->next;
        if (!r || !Readable(r, 0x54) || (r->flags & 0x820)) continue;
        const UInt8* x = MapMarkerExtra(r);
        if (!x) continue;
        std::string name;
        uint16_t type = 0, unused = 0;
        if (!ReadMarkerData(x, &name, &type, &unused)) continue;
        auto* md = *reinterpret_cast<const UInt8* const*>(x + 0x0C);
        if (!Readable(md, 0x10)) continue;
        // MarkerData = TESFullName (0x0C) then the flags: 1 on the map,
        // 2 can travel there (= discovered), 4 hidden — the FNV layout and
        // the bits of the XMRK FNAM record. Logged for the first few so a
        // live log can confirm the reading.
        UInt8 flags = md[0x0C];
        if (s_logged < 12) {
            ++s_logged;
            F3A_INFO("Locations: '%s' marker bytes +08=%04X +0C=%02X +0D=%02X +0E=%04X",
                     name.c_str(), *reinterpret_cast<const uint16_t*>(md + 0x08), md[0x0C],
                     md[0x0D], *reinterpret_cast<const uint16_t*>(md + 0x0E));
        }
        if (flags & 0x04) continue;                              // hidden
        float dx = r->posX - pp.x, dy = r->posY - pp.y;
        if (!(dx * dx + dy * dy <= r2)) continue;
        WorldEntity e;
        e.kind     = WorldEntity::Kind::Location;
        e.position = { r->posX, r->posY, r->posZ };
        e.form_id  = r->refID;
        e.refr     = r;
        e.discovered = (flags & 0x02) != 0;
        if (flags & 0x02)      e.name = name;                     // been there
        else if (flags & 0x01) e.name = name + strings::T(", nieodkryte", ", undiscovered");    // heard of it
        else                   e.name = strings::T("Nieodkryte miejsce", "Undiscovered place");
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [&pp](const WorldEntity& a, const WorldEntity& b) {
        float da = (a.position.x - pp.x) * (a.position.x - pp.x) +
                   (a.position.y - pp.y) * (a.position.y - pp.y);
        float db = (b.position.x - pp.x) * (b.position.x - pp.x) +
                   (b.position.y - pp.y) * (b.position.y - pp.y);
        return da < db;
    });
    if ((int)out.size() > max_results) out.resize(max_results);
    return out;
}

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
void ClearMapMarkerCache() { g_marker_cache.clear(); }

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

namespace {
// Every distinct visible string inside a tile subtree, as separate lines.
// Duplicates are dropped: menus repeat the same text on a parent and its child
// all the time, and hearing it twice is worse than not hearing it at all.
void CollectTextLines(Tile* t, int depth, std::vector<std::string>& out)
{
    if (!t || depth > 12 || out.size() > 64) return;
    if (TileVisible(t)) {
        if (const char* raw = TileStringTrait(t)) {
            std::string line = GameStrToUtf8(raw);
            const char* ws = " \t\n\r";
            size_t b = line.find_first_not_of(ws);
            if (b != std::string::npos) {
                line = line.substr(b, line.find_last_not_of(ws) - b + 1);
                if (line.size() > 1) {
                    bool dup = false;
                    for (const auto& e : out) if (e == line) { dup = true; break; }
                    if (!dup) out.push_back(line);
                }
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectTextLines(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

std::vector<std::string> GetMenuTextLines(uint32_t menuType)
{
    std::vector<std::string> out;
    if (Tile* m = FindVisibleMenuTile(menuType)) CollectTextLines(m, 0, out);
    return out;
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
    // The list keeps a pool of rows: every objective it ever showed stays in
    // it, hidden (28 rows, one of them shown, in a dump at Tranquility Lane),
    // and their text tiles still say visible. Only the shown rows are this
    // quest's, in their order on screen; a dimmed one is done, as the screen
    // greys it out.
    struct Row { float y; std::string text; bool done; };
    std::vector<Row> rows;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&list->childList);
    for (int i = 0; node && i < 512; ++i) {
        if (!Readable(node, sizeof(Node))) break;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child || !TileVisible(cn->child)) continue;
        Tile* txt = FindChildByName(cn->child, "ListItemText", 1);
        const char* str = txt ? TileStringTrait(txt) : TileStringTrait(cn->child);
        std::string t = str ? GameStrToUtf8(str) : std::string();
        const char* kWs = " \t\n\r";
        size_t b = t.find_first_not_of(kWs);
        if (b == std::string::npos) continue;
        t = t.substr(b, t.find_last_not_of(kWs) - b + 1);
        const float a = txt ? TileNum(txt, kTileValue_alpha) : 255.0f;
        rows.push_back({ TileNum(cn->child, kTileValue_y), t, a > 0.0f && a <= 128.0f });
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.y < y.y; });
    std::string out;
    for (const auto& r : rows) {
        if (!out.empty()) out += ' ';
        out += r.text;
        if (r.done) out += strings::T(" (wykonane)", " (done)");
    }
    return out;
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
    std::string nm = GetCrosshairRefName(&out->refid, &type, &out->known);   // handles the name fallbacks
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

std::atomic<uint32_t> g_pumps{ 0 };      // messages the game has dispatched

LRESULT WINAPI DispatchMessageA_Hook(const MSG* msg)
{
    // Runs on the main (message-pump) thread — safe to call game code here.
    g_pumps.fetch_add(1);
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

namespace {
// The game plays its films (the opening, the "years later" cards, the ending
// slides) through Bink. Its two imports BinkOpen/BinkClose say which film and
// when; the open BINK structure says how far it has got (frame number and
// frame rate at +0x0C / +0x14 / +0x18, Bink 1.x), so descriptions follow the
// picture even when playback stalls.
using BinkOpen_t  = void* (__stdcall*)(const char*, UInt32);
using BinkClose_t = void  (__stdcall*)(void*);
BinkOpen_t  g_realBinkOpen  = nullptr;
BinkClose_t g_realBinkClose = nullptr;
std::mutex  g_bink_mx;
void*       g_bink = nullptr;           // the film playing, under g_bink_mx
std::string g_bink_name;
DWORD       g_bink_open_at = 0;
uint32_t    g_bink_seq = 0;

void* __stdcall BinkOpen_Hook(const char* name, UInt32 flags)
{
    void* h = g_realBinkOpen ? g_realBinkOpen(name, flags) : nullptr;
    std::string base = name ? name : "";
    size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    for (auto& c : base) c = (char)std::tolower((unsigned char)c);
    if (h) {
        std::lock_guard<std::mutex> lk(g_bink_mx);
        g_bink = h;
        g_bink_name = base;
        g_bink_open_at = GetTickCount();
        ++g_bink_seq;
    }
    F3A_INFO("Movie: '%s' opened (%p)", name ? name : "?", h);
    return h;
}

void __stdcall BinkClose_Hook(void* h)
{
    {
        std::lock_guard<std::mutex> lk(g_bink_mx);
        if (h && h == g_bink) {
            F3A_INFO("Movie: '%s' closed after %.1f s", g_bink_name.c_str(),
                     (GetTickCount() - g_bink_open_at) / 1000.0);
            g_bink = nullptr;
        }
        if (g_realBinkClose) g_realBinkClose(h);
    }
}
} // namespace

void InstallMovieHooks()
{
    void* o = HookIAT("binkw32.dll", "_BinkOpen@8", reinterpret_cast<void*>(&BinkOpen_Hook));
    if (o) g_realBinkOpen = reinterpret_cast<BinkOpen_t>(o);
    void* c = HookIAT("binkw32.dll", "_BinkClose@4", reinterpret_cast<void*>(&BinkClose_Hook));
    if (c) g_realBinkClose = reinterpret_cast<BinkClose_t>(c);
    F3A_INFO("Movie hooks (BinkOpen/BinkClose IAT) %s.", (o && c) ? "installed" : "FAILED");
}

bool CurrentMovie(std::string* name, double* seconds, uint32_t* seq)
{
    std::lock_guard<std::mutex> lk(g_bink_mx);
    if (!g_bink) return false;
    if (name) *name = g_bink_name;
    if (seq) *seq = g_bink_seq;
    double t = (GetTickCount() - g_bink_open_at) / 1000.0;
    auto* b = reinterpret_cast<const UInt32*>(g_bink);
    if (Readable(b, 0x1C)) {
        UInt32 w = b[0], h = b[1], frames = b[2], fn = b[3], rate = b[5], div = b[6];
        if (w > 0 && w <= 8192 && h > 0 && h <= 8192 && rate > 0 && div > 0 &&
            fn >= 1 && fn <= frames)
            t = (fn - 1) * (double)div / (double)rate;
    }
    if (seconds) *seconds = t;
    return true;
}

void InstallMainThreadHook()
{
    void* prev = HookIAT("user32.dll", "DispatchMessageA",
                         reinterpret_cast<void*>(&DispatchMessageA_Hook));
    g_realDispatchMessageA = reinterpret_cast<DispatchMessageA_t>(prev);
    F3A_INFO("Main-thread hook (DispatchMessageA IAT) %s.",
             prev ? "installed" : "FAILED");
}

uint32_t MainThreadPumps() { return g_pumps.load(); }

void QueueActivate(const void* refr, uint32_t expectedRefID)
{
    g_pending_id.store(expectedRefID);
    g_pending_refr.store(refr);
}

int DoorOpenState(const void* refr, uint32_t expectedRefID)
{
    auto* r = reinterpret_cast<const TESObjectREFR*>(refr);
    if (!Readable(r, 0x48) || r->refID != expectedRefID) return -1;
    return ReadDoorOpenState(r);
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

std::string GetCrosshairRefName(uint32_t* out_form_id, uint32_t* out_type_id,
                                bool* out_known)
{
    if (out_form_id) *out_form_id = 0;
    if (out_type_id) *out_type_id = 0;
    if (out_known) *out_known = false;
    auto* ifm = rt::IFM();
    if (!ifm) return {};
    UInt8* base = reinterpret_cast<UInt8*>(ifm);
    if (IsBadReadPtr(base + 0xFC, 4)) return {};
    auto* refr = *reinterpret_cast<TESObjectREFR**>(base + 0xFC);  // crosshairRef
    if (!refr || IsBadReadPtr(refr, 0x40)) return {};
    if (out_form_id) *out_form_id = refr->refID;

    TESForm* bf = refr->baseForm;
    if (!Readable(bf, 0x08)) return std::string(strings::T("obiekt", "object"));
    if (out_type_id) *out_type_id = bf->typeID;
    const char* nm = BaseFormName(bf);
    if (out_known) *out_known = true;
    if (nm) return GameStrToUtf8(nm);
    switch (KindOf(bf->typeID)) {
        case WorldEntity::Kind::Door:      return strings::T("drzwi", "door");
        case WorldEntity::Kind::Container: return strings::T("pojemnik", "container");
        case WorldEntity::Kind::Actor:     return strings::T("postać", "character");
        default:
            if (out_known) *out_known = false;
            return strings::T("obiekt", "object");
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

float GetPlayerAVMax(uint32_t avCode)
{
    auto* p = rt::Player();
    if (!p) return -1.0f;
    UInt8* avOwner = reinterpret_cast<UInt8*>(p) + 0x9C;
    if (IsBadReadPtr(avOwner, 4)) return -1.0f;
    void** vtbl = *reinterpret_cast<void***>(avOwner);
    if (IsBadReadPtr(vtbl, 0x24) || IsBadReadPtr(vtbl[8], 1)) return -1.0f;
    typedef float (__thiscall* GetAVFn)(void* thisptr, UInt32 code);
    float v = reinterpret_cast<GetAVFn>(vtbl[8])(avOwner, avCode);
    return std::isfinite(v) ? v : -1.0f;
}

// The same call as GetPlayerAV, but for any actor. Used to notice that a shot
// landed: the engine has no event we can hook here, so the evidence is the
// target's own health going down.
float GetRefAV(const void* refr, uint32_t avCode)
{
    auto* r = reinterpret_cast<TESObjectREFR*>(const_cast<void*>(refr));
    if (!r || IsBadReadPtr(r, 0xA0)) return -1.0f;
    // Only an ACTOR has actor values. Everything else — a door, a quest marker,
    // a crate — has something else entirely at +0x9C, and calling through it
    // would be the same mistake that crashed this game on the skeleton path:
    // readable memory is not proof of the expected structure.
    TESForm* bf = r->baseForm;
    if (!Readable(bf, 0x08)) return -1.0f;
    if (KindOf(bf->typeID) != WorldEntity::Kind::Actor) return -1.0f;
    UInt8* avOwner = reinterpret_cast<UInt8*>(r) + 0x9C;
    if (IsBadReadPtr(avOwner, 4)) return -1.0f;
    void** vtbl = *reinterpret_cast<void***>(avOwner);
    if (!vtbl || IsBadReadPtr(vtbl, 0x10) || IsBadReadPtr(vtbl[3], 1)) return -1.0f;
    typedef float (__thiscall* GetAVFn)(void* thisptr, UInt32 code);
    float v = reinterpret_cast<GetAVFn>(vtbl[3])(avOwner, avCode);
    return std::isfinite(v) ? v : -1.0f;
}

int GetPlayerItemCount(uint32_t formId)
{
    return ItemCountIn(reinterpret_cast<const TESObjectREFR*>(rt::Player()), 0x64, formId);
}

// The equipped weapon is the inventory entry of a weapon one of whose extra
// data lists carries Worn (0x16) or WornLeft (0x17) - how FOSE's
// GetEquippedObject finds it. TESObjectWEAP (FOSE GameForms.h): name @0x30,
// TESHealthForm @0x94 (health @0x98), BGSAmmoForm @0xA4 (TESAmmo* @0xA8). The
// carried weapon's own condition is its ExtraHealth (0x25, float @0x0C).
bool GetEquippedWeapon(EquippedWeaponInfo* out)
{
    auto* p = reinterpret_cast<const TESObjectREFR*>(rt::Player());
    if (!out || !p) return false;
    for (const UInt8* list = ContainerChangesList(p); list; ) {
        if (!Readable(list, 0x08)) break;
        const UInt8* ed = *reinterpret_cast<const UInt8* const*>(list);
        list = *reinterpret_cast<const UInt8* const*>(list + 0x04);
        if (!ed || !Readable(ed, 0x0C)) continue;
        auto* w = *reinterpret_cast<TESForm* const*>(ed + 0x08);
        if (!Readable(w, 0xB0) || w->typeID != kFormType_Weapon) continue;
        for (const UInt8* xl = *reinterpret_cast<const UInt8* const*>(ed); xl; ) {
            if (!Readable(xl, 0x08)) break;
            const UInt8* x = *reinterpret_cast<const UInt8* const*>(xl);   // ExtraDataList*
            xl = *reinterpret_cast<const UInt8* const*>(xl + 0x04);
            if (!x || !(FindExtra(x, 0x16) || FindExtra(x, 0x17))) continue;
            const char* nm = BaseFormName(w);
            out->name = nm ? GameStrToUtf8(nm) : std::string();
            const UInt32 baseHealth = *reinterpret_cast<const UInt32*>(
                                          reinterpret_cast<const UInt8*>(w) + 0x98);
            if (baseHealth > 0 && baseHealth < 100000) {
                float h = static_cast<float>(baseHealth);
                if (const UInt8* xh = FindExtra(x, 0x25))
                    if (Readable(xh, 0x10)) h = *reinterpret_cast<const float*>(xh + 0x0C);
                if (std::isfinite(h)) {
                    int pct = static_cast<int>(h / baseHealth * 100.0f + 0.5f);
                    out->condition = pct < 0 ? 0 : pct > 100 ? 100 : pct;
                }
            }
            auto* ammo = *reinterpret_cast<TESForm* const*>(
                             reinterpret_cast<const UInt8*>(w) + 0xA8);
            if (Readable(ammo, 0x40) && ammo->typeID == kFormType_Ammo) {
                const char* an = BaseFormName(ammo);
                out->ammo = an ? GameStrToUtf8(an) : std::string();
                out->ammo_count = GetPlayerItemCount(ammo->refID);
            }
            return !out->name.empty();
        }
    }
    return false;
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

// ---- Native aim rotation (the FNV accessibility mod's recipe, ported) ------
//
// Writing rotZ/rotX, or driving the SetAngle script command, does NOT aim the
// weapon. The engine recomputes the view every frame from its own internal
// pitch/yaw state and from the camera's rotation globals, and overwrites what
// we wrote before it ever reaches the crosshair. That is why "center on the
// target, hold aim, shoot" kept missing: the announcement was right and the
// gun was pointing somewhere else.
//
// The FNV mod drives all three layers in the same frame. Fallout 3 has the same
// three, found here by disassembly:
//
//   SetPitchWithClamp  0x00765440  __thiscall(Actor*, float), ret 4
//       Clamps to +-1.5533430 rad (89 degrees — the constant at 0x00E17CA4)
//       and writes the pitch through the engine's own accessor.
//   AddDeltaZRot       0x00765400  __thiscall(Actor*, float), ret 4
//       Reads the current yaw through vtable+0x2BC, adds the delta, writes it
//       back through vtable+0x2C4. Yaw is set as a DELTA, not absolutely.
//   Camera rotation globals   pitch = 0x0107BA14, yaw = 0x0107BA1C
//       What the renderer actually reads. Three independent places in the
//       executable fill them with exactly this pair, which is how they were
//       identified:
//           call [vtable+0x2BC] ; fstp [0x0107BA1C]   <- yaw
//           call 0x00765430     ; fstp [0x0107BA14]   <- pitch
//       (0x00765430 is `fld [ecx+0x20]; ret` — GetRotationX.) Without these the
//       camera lags behind the weapon and the crosshair sits on empty space.
//
// Everything here CALLS game code, so it is main-thread only.
namespace {

constexpr uintptr_t kSetPitchWithClamp = 0x00765440;
constexpr uintptr_t kAddDeltaZRot      = 0x00765400;
constexpr uintptr_t kCameraPitchGlobal = 0x0107BA14;
constexpr uintptr_t kCameraYawGlobal   = 0x0107BA1C;
constexpr unsigned  kGetRotationZSlot  = 0x2BC / 4;   // vtable index
constexpr float     kAimPi    = 3.14159265f;
constexpr float     kAimTwoPi = 6.28318530f;
constexpr float     kEyeDefault = 100.0f;  // starting guess, replaced by measurement
float g_eye_height = kEyeDefault;

// Prologue guards, checked once. A build that does not match keeps the old
// behaviour instead of jumping into the middle of something else.
int g_aim_native_ok = -1;   // -1 = untested

// Camera probe state — see the probe in AimPlayerAtRad.
int   g_cam_probe_tick  = 0;
bool  g_cam_probe_skip  = false;
bool  g_cam_probe_armed = false;
float g_cam_probe_yaw   = 0.0f;
float g_cam_probe_pitch = 0.0f;

// What we asked for last time, so the next frame can see what survived.
float g_aim_last_yaw   = 0.0f;
float g_aim_last_pitch = 0.0f;
bool  g_aim_last_valid = false;

bool AimNativeReady()
{
    if (g_aim_native_ok >= 0) return g_aim_native_ok != 0;
    g_aim_native_ok = 0;
    // The camera globals written below are .data addresses of the standard
    // 1.7.0.3 build; on the no-gore build they sit elsewhere, so never write.
    if (rt::g_addrs != &rt::kAddrs_1_7) {
        F3A_INFO("Aim: native rotation only on the standard 1.7.0.3 build.");
        return false;
    }
    const uint8_t pitchSig[] = { 0x51, 0xD9, 0x05 };          // push ecx; fld [imm32]
    const uint8_t yawSig[]   = { 0x56, 0x57, 0x8B, 0xF1 };    // push esi; push edi; mov esi,ecx
    auto* pp = reinterpret_cast<const uint8_t*>(kSetPitchWithClamp);
    auto* yp = reinterpret_cast<const uint8_t*>(kAddDeltaZRot);
    if (IsBadReadPtr(pp, sizeof(pitchSig)) || IsBadReadPtr(yp, sizeof(yawSig))) {
        F3A_INFO("Aim: native rotation functions unreadable — falling back.");
        return false;
    }
    if (std::memcmp(pp, pitchSig, sizeof(pitchSig)) != 0 ||
        std::memcmp(yp, yawSig,   sizeof(yawSig))   != 0) {
        F3A_INFO("Aim: native rotation prologues differ — falling back.");
        return false;
    }
    if (IsBadWritePtr(reinterpret_cast<void*>(kCameraPitchGlobal), 4) ||
        IsBadWritePtr(reinterpret_cast<void*>(kCameraYawGlobal), 4)) {
        F3A_INFO("Aim: camera rotation globals unwritable — falling back.");
        return false;
    }
    g_aim_native_ok = 1;
    F3A_INFO("Aim: native rotation ready (pitch 0x%08X, yaw 0x%08X, camera "
             "0x%08X/0x%08X).", (unsigned)kSetPitchWithClamp,
             (unsigned)kAddDeltaZRot, (unsigned)kCameraPitchGlobal,
             (unsigned)kCameraYawGlobal);
    return true;
}

} // namespace

const void* BaseFormOf(const void* refr)
{
    if (!refr || IsBadReadPtr(refr, 0x20)) return nullptr;
    return reinterpret_cast<const TESObjectREFR*>(refr)->baseForm;
}

uint32_t BaseFormIdOf(const void* refr)
{
    auto* base = reinterpret_cast<const TESForm*>(BaseFormOf(refr));
    if (!base || IsBadReadPtr(base, 0x10)) return 0;
    return base->refID;
}

float EyeHeight()            { return g_eye_height; }
void  SetEyeHeight(float u)  { g_eye_height = u; }

// ---- The engine's own view raycast ----------------------------------------
//
// Everything above this point had to guess where a shot comes from and where an
// object's middle is, because nothing in the mod could SEE. The engine can. The
// pick that decides what is under the crosshair is a Havok raycast, and it is
// callable: ButcherPete FOSE (c6-dev, MIT) implements GetCrosshairRefEx for
// Fallout 3 with exactly this call, the way JIP does for New Vegas — which is
// what the FNV accessibility mod leans on. The addresses below come from that
// source and were checked against this executable.
//
//   0x00574540  __thiscall TESObjectREFR* Pick(void* viewCaster,
//                   NiPoint3* from, NiPoint3* dir, float maxDist,
//                   float* outDist, bool* outHit)
//   InterfaceManager + 0x13C   the viewCaster the pick runs on
//   0x0107BA78                 the first-person camera node (null = no 3D yet)
//   player + 0x988             kCamera1stPos — the camera's WORLD position,
//                              which is the real answer to "where does the shot
//                              leave from", a number we had been inventing
//   player + 0x5A9             bThirdPerson
//   player + 0x90C             camera3rdPos, added when the camera orbits
namespace {

constexpr uintptr_t kViewPickFn        = 0x00574540;
constexpr uintptr_t kCamera1stNodePtr  = 0x0107BA78;
constexpr uint32_t  kIfmViewCaster     = 0x13C;
constexpr uint32_t  kPlayerCamera1stPos= 0x988;
constexpr uint32_t  kPlayerThirdPerson = 0x5A9;
constexpr uint32_t  kPlayerCamera3rdPos= 0x90C;

int g_ray_ok = -1;   // -1 = untested

bool RayCastReady()
{
    if (g_ray_ok >= 0) return g_ray_ok != 0;
    g_ray_ok = 0;
    const uint8_t sig[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF };
    auto* p = reinterpret_cast<const uint8_t*>(kViewPickFn);
    if (IsBadReadPtr(p, sizeof(sig)) || std::memcmp(p, sig, sizeof(sig)) != 0) {
        F3A_INFO("Ray: the view pick at 0x%08X does not match — raycasts off.",
                 (unsigned)kViewPickFn);
        return false;
    }
    g_ray_ok = 1;
    F3A_INFO("Ray: engine view pick available at 0x%08X.", (unsigned)kViewPickFn);
    return true;
}

} // namespace

bool GetCameraPos(Vec3* out)
{
    auto* p = rt::Player();
    if (!p || !out) return false;
    auto* b = reinterpret_cast<uint8_t*>(p);
    if (IsBadReadPtr(b + kPlayerCamera1stPos, 12)) return false;

    const float* c = reinterpret_cast<const float*>(b + kPlayerCamera1stPos);
    Vec3 pos{ c[0], c[1], c[2] };

    if (!IsBadReadPtr(b + kPlayerThirdPerson, 1) && b[kPlayerThirdPerson] &&
        !IsBadReadPtr(b + kPlayerCamera3rdPos, 12)) {
        const float* c3 = reinterpret_cast<const float*>(b + kPlayerCamera3rdPos);
        pos.x += c3[0]; pos.y += c3[1]; pos.z += c3[2];
    }

    // Sanity: the camera belongs to the player. Before the 3D exists it can be
    // stale or zero, and silently aiming from the wrong place is worse than
    // admitting we do not know.
    if (!(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)))
        return false;
    float dx = pos.x - p->posX, dy = pos.y - p->posY, dz = pos.z - p->posZ;
    if (!(dx * dx + dy * dy + dz * dz <= 400.0f * 400.0f)) return false;
    // A hard-coded .data global: only on the build it was read from.
    if (rt::g_addrs != &rt::kAddrs_1_7 ||
        IsBadReadPtr(reinterpret_cast<void*>(kCamera1stNodePtr), 4)) return false;
    if (!*reinterpret_cast<void**>(kCamera1stNodePtr) && !IsThirdPerson())
        return false;

    *out = pos;
    return true;
}

bool RayCastView(const Vec3& from, const Vec3& dir, float maxDist,
                 uint32_t* outRefId, float* outDist, const void** outRef)
{
    if (outRefId) *outRefId = 0;
    if (outDist)  *outDist  = 0.0f;
    if (outRef)   *outRef   = nullptr;
    if (!RayCastReady()) return false;
    auto* ifm = rt::IFM();
    if (!ifm) return false;
    auto* caster = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(ifm) + kIfmViewCaster);
    if (!caster || IsBadReadPtr(caster, 4)) return false;

    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f) return false;
    float f[3] = { from.x, from.y, from.z };
    float d[3] = { dir.x / len, dir.y / len, dir.z / len };
    float hitDist = 0.0f;
    bool  hit = false;

    using PickFn = TESObjectREFR* (__thiscall*)(void*, float*, float*, float,
                                                float*, bool*);
    TESObjectREFR* ref = reinterpret_cast<PickFn>(kViewPickFn)(
        caster, f, d, maxDist, &hitDist, &hit);

    // NO conversion. The comment that used to stand here said the pick reports
    // Havok units and multiplied by seven — and the log says otherwise: a
    // target measured at 97 units away came back as 704, and another at 97 as
    // 638. Divide those by seven and you get 100 and 91, which is the true
    // range give or take the difference between an object's surface and its
    // origin. The multiplication was the error, and it poisoned every
    // comparison the solver made between "how far the ray went" and "how far
    // the target is".
    if (outDist) *outDist = hitDist;
    if (!ref || IsBadReadPtr(ref, 0x20)) return hit;
    if (outRefId) *outRefId = ref->refID;
    if (outRef)   *outRef   = ref;
    return true;
}

namespace {

} // namespace

// Find a point on `origin`'s object that the view can actually reach.
//
// A reference's origin is wherever its model happens to be anchored — for the
// vault's rifle targets that is a foot to one side and half a metre below the
// part you shoot at. Rather than ask a sighted player to measure that for every
// kind of object in the game, ASK THE ENGINE: cast its own view ray at the
// origin and, if that does not land on the thing, at points around it until one
// does. The rays are invisible and cost nothing the player can feel, and the
// answer is exact.
//
// Returns false when nothing on the object can be reached — which is itself
// worth knowing, because it means something is in the way.
// What the player is actually looking at: one ray from the camera, straight
// down the view. This is the honest answer to "is my shot going to hit it",
// and it replaces the InterfaceManager crosshair field for that purpose.
//
// That field had to go. A log showed it naming the SAME reference at the SAME
// distance (170 units) through a 21-degree swing of the aim - it does not track
// the view, so every conclusion drawn from it, including a cheerful "target
// obscured" the instant the aim key went down, was noise. The New Vegas mod
// does not use its equivalent either: it calls a raycast (JIP's
// GetCrosshairRefEx) for exactly this check.
bool RayPickAhead(uint32_t* outRefId, float* outDist)
{
    if (outRefId) *outRefId = 0;
    if (outDist)  *outDist  = 0.0f;
    auto* p = rt::Player();
    if (!p) return false;

    Vec3 eye{ p->posX, p->posY, p->posZ + 100.0f };
    GetCameraPos(&eye);                 // the real muzzle height when available

    // Camera direction: yaw 0 = +Y, and a POSITIVE rotX looks down.
    float yaw = GetPlayerYaw() * 0.01745329f;
    float pit = GetPlayerPitch() * 0.01745329f;
    float cp = std::cos(pit);
    Vec3 dir{ std::sin(yaw) * cp, std::cos(yaw) * cp, -std::sin(pit) };

    uint32_t id = 0;
    float dist = 0.0f;
    if (!RayCastView(eye, dir, 4096.0f, &id, &dist)) return false;
    if (outRefId) *outRefId = id;
    if (outDist)  *outDist  = dist;
    return true;
}

bool SolveAimPoint(const Vec3& origin, uint32_t wantRefId, Vec3* out)
{
    if (!out) return false;
    Vec3 eye;
    if (!GetCameraPos(&eye)) return false;

    // Two standards, tried in that order.
    //
    // `exact` means the ray came back holding the target itself. `loose` also
    // accepts anything stopping at the same range, on the grounds that a target
    // is built from several references and a bullet cannot tell them apart.
    //
    // Loose alone is not enough, and the log shows exactly why: aiming at this
    // target's origin, the ray stops on a wall SIX UNITS in front of it. Six is
    // well within any sane "same object" tolerance, so the loose test was happy
    // — while every shot went into the wall. The target's origin is buried
    // behind the surface it is mounted on; the part you are meant to hit sticks
    // out somewhere else entirely. So: look for a real hit on the target first,
    // everywhere, and settle for a near miss only if there is none.
    auto probe = [&](const Vec3& c, bool exact) -> bool {
        Vec3 d{ c.x - eye.x, c.y - eye.y, c.z - eye.z };
        float want = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (want < 1.0f) return false;
        uint32_t id = 0; float dist = 0.0f;
        if (!RayCastView(eye, d, want + 400.0f, &id, &dist)) return false;
        if (wantRefId && id == wantRefId) return true;
        if (exact) return false;
        return std::fabs(dist - want) <= 60.0f;
    };

    // One line about the very first ray, so the next log says what the engine
    // saw rather than only that we did not like it.
    {
        Vec3 d{ origin.x - eye.x, origin.y - eye.y, origin.z - eye.z };
        float want = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        uint32_t id = 0; float dist = 0.0f;
        bool any = RayCastView(eye, d, want + 400.0f, &id, &dist);
        F3A_INFO("AimSolve: ray from (%.0f,%.0f,%.0f) at the origin -> hit=%d "
                 "ref=%08X dist=%.0f (wanted %.0f)",
                 eye.x, eye.y, eye.z, any ? 1 : 0, id, dist, want);
    }

    if (probe(origin, /*exact*/true)) { *out = origin; return true; }

    // Build a frame facing the target so the search sweeps ACROSS the view
    // rather than along it.
    Vec3 f{ origin.x - eye.x, origin.y - eye.y, origin.z - eye.z };
    float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (fl < 1.0f) return false;
    f.x /= fl; f.y /= fl; f.z /= fl;
    Vec3 right{ f.y, -f.x, 0.0f };
    float rl = std::sqrt(right.x * right.x + right.y * right.y);
    if (rl < 1e-4f) { right = { 1.0f, 0.0f, 0.0f }; rl = 1.0f; }
    right.x /= rl; right.y /= rl;
    Vec3 up{ right.y * f.z - right.z * f.y,
             right.z * f.x - right.x * f.z,
             right.x * f.y - right.y * f.x };

    static const float kRadius[] = { 20.0f, 40.0f, 60.0f, 80.0f, 100.0f, 130.0f };
    constexpr int kSpokes = 16;
    for (int pass = 0; pass < 2; ++pass) {
        const bool exact = (pass == 0);
        for (float r : kRadius) {
            for (int i = 0; i < kSpokes; ++i) {
                float a = i * (6.28318530f / kSpokes);
                float cu = std::cos(a) * r, cr = std::sin(a) * r;
                Vec3 c{ origin.x + up.x * cu + right.x * cr,
                        origin.y + up.y * cu + right.y * cr,
                        origin.z + up.z * cu + right.z * cr };
                if (probe(c, exact)) {
                    F3A_INFO("AimSolve: found %s at radius %.0f, angle %.0f deg",
                             exact ? "the target itself" : "the same range",
                             r, a * 57.2957795f);
                    *out = c;
                    return true;
                }
            }
        }
        if (!wantRefId) break;   // no reference to be exact about
    }
    return false;
}

void RayMapAround(const Vec3& centre)
{
    Vec3 eye;
    if (!GetCameraPos(&eye)) { F3A_INFO("RayMap: no camera yet."); return; }

    Vec3 f{ centre.x - eye.x, centre.y - eye.y, centre.z - eye.z };
    float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (fl < 1.0f) return;
    f.x /= fl; f.y /= fl; f.z /= fl;
    Vec3 right{ f.y, -f.x, 0.0f };
    float rl = std::sqrt(right.x * right.x + right.y * right.y);
    if (rl < 1e-4f) { right = { 1.0f, 0.0f, 0.0f }; rl = 1.0f; }
    right.x /= rl; right.y /= rl;
    Vec3 up{ right.y * f.z - right.z * f.y,
             right.z * f.x - right.x * f.z,
             right.x * f.y - right.y * f.x };

    struct Row { uint32_t id; int n; float minD, maxD, minU, maxU, minR, maxR; };
    Row rows[24];
    int nrows = 0;

    // A window a little wider than a practice target, sampled finely enough
    // that nothing the size of a dinner plate can hide between two rays.
    constexpr float kSpan = 150.0f, kStep = 20.0f;
    int probes = 0;
    for (float u = -kSpan; u <= kSpan; u += kStep) {
        for (float r = -kSpan; r <= kSpan; r += kStep) {
            Vec3 c{ centre.x + up.x * u + right.x * r,
                    centre.y + up.y * u + right.y * r,
                    centre.z + up.z * u + right.z * r };
            Vec3 d{ c.x - eye.x, c.y - eye.y, c.z - eye.z };
            uint32_t id = 0; float dist = 0.0f;
            ++probes;
            if (!RayCastView(eye, d, fl + 600.0f, &id, &dist)) continue;
            int k = -1;
            for (int i = 0; i < nrows; ++i) if (rows[i].id == id) { k = i; break; }
            if (k < 0) {
                if (nrows >= 24) continue;
                k = nrows++;
                rows[k] = { id, 0, dist, dist, u, u, r, r };
            }
            Row& q = rows[k];
            ++q.n;
            if (dist < q.minD) q.minD = dist;
            if (dist > q.maxD) q.maxD = dist;
            if (u < q.minU) q.minU = u;   if (u > q.maxU) q.maxU = u;
            if (r < q.minR) q.minR = r;   if (r > q.maxR) q.maxR = r;
        }
    }

    F3A_INFO("RayMap: %d rays around (%.0f,%.0f,%.0f) from (%.0f,%.0f,%.0f), "
             "range %.0f — %d distinct references:",
             probes, centre.x, centre.y, centre.z, eye.x, eye.y, eye.z, fl, nrows);
    for (int i = 0; i < nrows; ++i) {
        const Row& q = rows[i];
        F3A_INFO("RayMap:   %08X hits=%-4d dist %.0f..%.0f  up %+.0f..%+.0f  "
                 "side %+.0f..%+.0f", q.id, q.n, q.minD, q.maxD,
                 q.minU, q.maxU, q.minR, q.maxR);
    }
}

std::vector<RayHit> RayScanAhead(float yawSpanDeg, float pitchSpanDeg,
                                 float stepDeg, float maxDist)
{
    std::vector<RayHit> out;
    Vec3 eye;
    if (!GetCameraPos(&eye)) return out;
    auto* p = rt::Player();
    if (!p) return out;

    const float D2R = 0.01745329f;
    float baseYaw   = p->rotZ;
    float basePitch = p->rotX;
    if (stepDeg < 0.5f) stepDeg = 0.5f;

    for (float dp = -pitchSpanDeg; dp <= pitchSpanDeg; dp += stepDeg) {
        for (float dy = -yawSpanDeg; dy <= yawSpanDeg; dy += stepDeg) {
            float yaw   = baseYaw   + dy * D2R;
            float pitch = basePitch + dp * D2R;
            float cp = std::cos(pitch);
            // Engine convention: yaw 0 looks along +Y, positive pitch looks down.
            Vec3 dir{ std::sin(yaw) * cp, std::cos(yaw) * cp, -std::sin(pitch) };
            uint32_t id = 0; float dist = 0.0f; const void* refp = nullptr;
            if (!RayCastView(eye, dir, maxDist, &id, &dist, &refp)) continue;
            if (!id) continue;

            RayHit* slot = nullptr;
            for (auto& h : out) if (h.refid == id) { slot = &h; break; }
            if (!slot) {
                if (out.size() >= 16) continue;
                out.push_back(RayHit{});
                slot = &out.back();
                slot->refid = id;
                slot->dist  = dist;
                slot->yaw   = dy;
                slot->pitch = dp;
                auto* ref = reinterpret_cast<const TESObjectREFR*>(refp);
                const char* nm = nullptr;
                if (ref && Readable(ref->baseForm, 8))
                    nm = BaseFormName(ref->baseForm);
                slot->name = nm ? GameStrToUtf8(nm) : std::string();
            }
            ++slot->samples;
            if (dist < slot->dist) {         // keep the closest sample's aim
                slot->dist  = dist;
                slot->yaw   = dy;
                slot->pitch = dp;
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const RayHit& a, const RayHit& b) { return a.dist < b.dist; });
    return out;
}

bool AimPlayerAtRad(float pitchRad, float yawRad)
{
    if (!(std::isfinite(pitchRad) && std::isfinite(yawRad))) return false;
    if (!AimNativeReady()) return false;
    auto* player = rt::Player();
    if (!player) return false;

    using PitchFn = void  (__thiscall*)(void*, float);
    using YawFn   = void  (__thiscall*)(void*, float);
    using GetZFn  = float (__thiscall*)(void*, int);

    reinterpret_cast<PitchFn>(kSetPitchWithClamp)(player, pitchRad);

    // Yaw goes in as a delta from wherever the engine currently thinks we face.
    // GetRotationZ takes one stack argument it ignores but still cleans up, so
    // the parameter must be passed or the call corrupts the caller's stack.
    void** vt = *reinterpret_cast<void***>(player);
    if (vt && !IsBadReadPtr(vt + kGetRotationZSlot, sizeof(void*))) {
        auto getZ = reinterpret_cast<GetZFn>(vt[kGetRotationZSlot]);
        if (getZ) {
            float cur = getZ(player, 0);
            if (std::isfinite(cur)) {
                float d = std::remainder(yawRad - cur, kAimTwoPi);
                reinterpret_cast<YawFn>(kAddDeltaZRot)(player, d);
            }
        }
    }

    // THE PUBLIC ROTATION FIELDS. This is the layer that was missing, and it
    // is the one the WEAPON reads.
    //
    // The comment above lists three places the aim has to land, and we were
    // writing two: the engine's internal aim state (the calls above) and the
    // camera globals (below). The fields the FNV mod labels "used by weapon hit
    // raycasts" were left alone, because an early experiment wrote ONLY them,
    // saw the first-person camera ignore it, and concluded they were useless.
    // They are not useless — they are insufficient on their own. Written
    // together with the other two, in this order, they are what points the gun
    // where the camera is already looking. Without them the mod announces the
    // target, the view is on it, and the shot goes somewhere else — which is
    // exactly what a magazine emptied into an officer looked like.
    player->rotX = pitchRad;
    player->rotZ = yawRad;

    // Camera probe.
    //
    // Every value we have been reading back is a value we wrote ourselves,
    // which proves nothing about where the camera really points — and the one
    // independent signal, the reference under the crosshair, never moved while
    // the aim swung through 24 degrees. So once in a while, DON'T write these
    // two, and let the engine fill them. Whatever is in them on the next frame
    // is the engine's own opinion of where the view is looking, and comparing
    // that with the rotation we set says whether our writes reach the camera at
    // all. One frame in two hundred: invisible in play, decisive in the log.
    if (g_cam_probe_skip) {
        g_cam_probe_skip  = false;
        g_cam_probe_armed = true;
        g_cam_probe_yaw   = yawRad;
        g_cam_probe_pitch = pitchRad;
    } else {
        if (g_cam_probe_armed) {
            g_cam_probe_armed = false;
            float engYaw   = *reinterpret_cast<float*>(kCameraYawGlobal);
            float engPitch = *reinterpret_cast<float*>(kCameraPitchGlobal);
            const float R2D = 57.2957795f;
            float dy = engYaw - g_cam_probe_yaw;
            dy = std::isfinite(dy) ? std::remainder(dy, kAimTwoPi) : 0.0f;
            F3A_INFO("CamProbe: engine says yaw=%.1f pitch=%.1f, we set body "
                     "yaw=%.1f pitch=%.1f (difference %+.1f / %+.1f deg)",
                     engYaw * R2D, engPitch * R2D,
                     g_cam_probe_yaw * R2D, g_cam_probe_pitch * R2D,
                     dy * R2D, (engPitch - g_cam_probe_pitch) * R2D);
        }
        if (++g_cam_probe_tick >= 200) { g_cam_probe_tick = 0; g_cam_probe_skip = true; }
        *reinterpret_cast<float*>(kCameraPitchGlobal) = pitchRad;
        *reinterpret_cast<float*>(kCameraYawGlobal)   = yawRad;
    }

    // Whether the engine KEEPS this has to be measured a frame later, not here
    // — reading it back in the same breath only proves the write landed. That
    // check lives in poll::AimTick.
    g_aim_last_yaw   = yawRad;
    g_aim_last_pitch = pitchRad;
    g_aim_last_valid = true;
    return true;
}

bool GetEffectiveYawRad(float* out)
{
    auto* p = rt::Player();
    if (!p) return false;
    void** vt = *reinterpret_cast<void***>(p);
    if (!vt || IsBadReadPtr(vt + kGetRotationZSlot, sizeof(void*))) return false;
    using GetZFn = float (__thiscall*)(void*, int);
    auto fn = reinterpret_cast<GetZFn>(vt[kGetRotationZSlot]);
    if (!fn) return false;
    if (out) *out = fn(p, 0);
    return true;
}

bool GetCameraAngles(float* outPitch, float* outYaw)
{
    if (!AimNativeReady()) return false;
    if (outPitch) *outPitch = *reinterpret_cast<float*>(kCameraPitchGlobal);
    if (outYaw)   *outYaw   = *reinterpret_cast<float*>(kCameraYawGlobal);
    return true;
}

bool AimDriftSinceLast(float* outYaw, float* outPitch)
{
    if (!g_aim_last_valid) return false;
    auto* p = rt::Player();
    if (!p) return false;
    float dy = p->rotZ - g_aim_last_yaw;
    if (!std::isfinite(dy)) return false;
    dy = std::remainder(dy, kAimTwoPi);
    if (outYaw)   *outYaw   = dy;
    if (outPitch) *outPitch = p->rotX - g_aim_last_pitch;
    return true;
}

bool AimPlayerAtPoint(const Vec3& target, float up, float dzBias)
{
    auto* p = rt::Player();
    if (!p) return false;
    // Aim from where the shot really comes from. The engine knows: the camera's
    // own world position. Everything before this estimated it as the player's
    // origin plus a fixed number, which is wrong for a crouching adult and
    // hopeless for the ten-year-old of the prologue — and a wrong origin tilts
    // every shot no matter how exactly the target is known.
    Vec3 eye{ p->posX, p->posY, p->posZ + g_eye_height };
    GetCameraPos(&eye);
    float dx = target.x - eye.x;
    float dy = target.y - eye.y;
    // Two heights here are guesses: where the shot leaves the player (the eye,
    // which is NOT the reference origin, and which for the ten-year-old of the
    // prologue is nowhere near an adult's) and where the target's aim point
    // sits above ITS origin. Geometrically those are one number — both shift
    // dz — so the aim tracker searches this single correction in UNITS rather
    // than in degrees. Units are the right currency: the correct value is the
    // same at every range, whereas an angle that works at two metres is wrong
    // at ten, which is why the earlier angle sweep could not converge.
    float dz = (target.z + up + dzBias) - eye.z;
    float horiz = std::sqrt(dx * dx + dy * dy);
    if (horiz < 1.0f) horiz = 1.0f;
    float yaw   = std::atan2(dx, dy);       // engine convention: 0 = +Y (north)
    float pitch = -std::atan2(dz, horiz);   // negative = looking up
    return AimPlayerAtRad(pitch, yaw);
}

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

// ---- VATS numbers ----------------------------------------------------------
//
// Reading VATS from tiles gives the target, the limb and the hit chance, but
// misses the thing that actually decides a fight: how many action points are
// left, and therefore how many shots you can queue. The FNV accessibility mod
// reads those straight out of the VATSMenu object, and the same fields exist
// here — FNV's menu layout is an evolution of FO3's.
//
// Offsets are FNV's, so they are treated as UNVERIFIED on this build: every
// value is sanity-checked before it is used, and the raw numbers go to the log
// once per VATS session so a wrong guess shows up as data rather than as a
// confidently spoken lie.
//   +0x0CC  UInt16  queued action count
//   +0x0E0  float   action points
//   +0x0E4  float   max action points
//   +0x0F0  float   ammo in the clip
//   +0x0F4  float   reserve ammo
//   +0x100  ptr     current body-part data (hit chance at +0x28)
bool GetVatsInfo(VatsInfo* out)
{
    if (!out) return false;
    *out = VatsInfo{};
    Menu* menu = FindMenuByType(kMenuType_VATS);
    if (!menu || IsBadReadPtr(menu, 0x108)) return false;
    auto* base = reinterpret_cast<const uint8_t*>(menu);

    auto plausible = [](float v, float hi) {
        return v >= 0.0f && v <= hi && v == v;      // v == v rejects NaN
    };

    float ap     = *reinterpret_cast<const float*>(base + 0x0E0);
    float maxAp  = *reinterpret_cast<const float*>(base + 0x0E4);
    float clip   = *reinterpret_cast<const float*>(base + 0x0F0);
    float reserve= *reinterpret_cast<const float*>(base + 0x0F4);
    uint16_t queued = *reinterpret_cast<const uint16_t*>(base + 0x0CC);

    if (plausible(maxAp, 2000.0f) && maxAp > 0.0f && plausible(ap, maxAp + 1.0f)) {
        out->ap = (int)(ap + 0.5f);
        out->max_ap = (int)(maxAp + 0.5f);
        out->has_ap = true;
    }
    if (plausible(clip, 9999.0f) && plausible(reserve, 99999.0f)) {
        out->clip_ammo = (int)clip;
        out->reserve_ammo = (int)reserve;
        out->has_ammo = true;
    }
    if (queued <= 64) out->queued = queued;

    // Hit chance from the body-part record, when it looks like a percentage.
    auto* bp = *reinterpret_cast<const uint8_t* const*>(base + 0x100);
    if (bp && !IsBadReadPtr((void*)bp, 0x30)) {
        float chance = *reinterpret_cast<const float*>(bp + 0x28);
        if (plausible(chance, 100.0f)) { out->hit_chance = (int)(chance + 0.5f); out->has_chance = true; }
    }

    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        F3A_INFO("VATS raw: ap=%.1f/%.1f clip=%.1f reserve=%.1f queued=%u bp=%p "
                 "(accepted ap=%d ammo=%d chance=%d)",
                 ap, maxAp, clip, reserve, (unsigned)queued, (void*)bp,
                 (int)out->has_ap, (int)out->has_ammo, (int)out->has_chance);
    }
    return true;
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

// Stop the walk/run animation. This is NOT the same thing as playing the idle
// one, and the difference is the whole bug.
//
// The movement animations (Forward = 3, FastForward = 7) run in SEQUENCE SLOT 1.
// The idle runs in slot 0. Asking for the idle therefore does not touch the walk
// cycle at all — it keeps looping on a player who is standing perfectly still.
// The game hangs its own FOOTSTEP SOUNDS off that cycle, so to a player who
// cannot see the screen this is indistinguishable from the walk never stopping:
// the mod announces "Zatrzymano" and the footsteps carry on. Measured, not
// guessed — the post-stop probe reported the player moving 0 units while the
// walking was still audible. The FNV accessibility mod hit this same wall and
// says so in its own comment; this is that fix on Fallout 3's addresses.
//
// AnimData::DeactivateSequence(int slot, float blendOut) at 0x0045DB50, found by
// matching New Vegas's 0x00496080 structurally rather than by guesswork: the
// same slot special cases (4 / 0x14 / 0x15), the same recursion into slots
// 7/0/1, the same member layout — sequence array at this+0xE0 over 8 slots, and
// the slot's group id at this+0x9C, which is the very array PlayActorAnimGroup
// above already writes through. Blend 0 = stop now, which is what silences the
// footsteps on the spot instead of fading them out.
namespace {
bool DeactivateAddrOk()
{
    // mov eax,[esp+4]; push esi; push edi
    static const unsigned char kSig[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57 };
    static int cached = -1;
    if (cached < 0) {
        auto* q = reinterpret_cast<const unsigned char*>(0x0045DB50);
        bool ok = !IsBadReadPtr((void*)q, sizeof(kSig));
        for (size_t i = 0; ok && i < sizeof(kSig); ++i) ok = (q[i] == kSig[i]);
        cached = ok ? 1 : 0;
        if (!ok) F3A_INFO("Anim: DeactivateSequence signature mismatch at "
                          "0x0045DB50 " "— not stopping movement animations.");
    }
    return cached == 1;
}
constexpr int kSequenceMovement = 1;   // the slot Forward/FastForward play in
} // namespace

bool StopActorMovementAnim(void* actor)
{
    if (!actor || !DeactivateAddrOk()) return false;
    void* animData = ActorAnimData(actor);
    // The function writes as far as this+0x100, so demand that much.
    if (!animData || IsBadReadPtr(animData, 0x104)) return false;
    using DeactivateFn = void (__thiscall*)(void* self, int slot, float blendOut);
    reinterpret_cast<DeactivateFn>(0x0045DB50)(animData, kSequenceMovement, 0.0f);
    return true;
}

// ---- Aiming at a reference's real body ------------------------------------
//
// Pointing the crosshair at a thing means pointing it at the thing's BODY, not
// at the origin the engine records for it. Those differ badly: a target's origin
// often sits at its feet, inside a table, or at the base of a post. Every
// attempt to paper over that with fixed height offsets and pitch fudges produced
// something that worked for one object and missed the next.
//
// The reliable source is the object's collision shape, which is what a bullet
// interacts with anyway. The FNV accessibility mod resolves its aim point the
// same way, and this is a clean-room implementation of that idea for FO3.
namespace {

// Gamebryo layout, unchanged between FO3 and FNV:
constexpr UInt32 kNiAV_Name            = 0x08;   // NiObjectNET::m_name
constexpr UInt32 kNiAV_CollisionObject = 0x1C;
constexpr UInt32 kNiNode_WorldBound    = 0x20;   // -> {x, y, z, radius}
constexpr UInt32 kNiAV_WorldTranslate  = 0x8C;   // m_worldTransform.translate
constexpr UInt32 kNiNode_Children      = 0x9C;   // NiTArray: data @+4, count @+0xA
constexpr UInt32 kNiAV_GetAsNiNode     = 3;      // vtable slot
// Havok stores collision in its own units; the game divides by ~1/7 to draw it.
constexpr float  kHavokToGame          = 6.999125f;

// The reference's 3D model. FOSE has no field for it, but the engine fetches it
// through a virtual: SetAngle's worker does `mov edx,[vtable+0x1D0]; call edx`
// and then writes the rotation matrix into the result at +0x34.
void* RefNiNode(TESObjectREFR* refr)
{
    if (!refr || IsBadReadPtr(refr, 0x40)) return nullptr;
    void** vtable = *reinterpret_cast<void***>(refr);
    if (!vtable || IsBadReadPtr(vtable, 0x1D4)) return nullptr;
    using GetNiNodeFn = void* (__thiscall*)(void* self, int flag);
    auto fn = reinterpret_cast<GetNiNodeFn>(vtable[0x1D0 / 4]);
    if (!fn) return nullptr;
    void* node = fn(refr, 0);
    if (!node || IsBadReadPtr(node, 0xA0)) return nullptr;
    return node;
}

// Havok's shape diagonal comes back in SSE vectors, so both must be 16-aligned.
__declspec(align(16)) struct HkVec4 { float x, y, z, w; };
__declspec(align(16)) struct HkDiagonal { HkVec4 min; HkVec4 max; };

// The collision shape's centre for one node, if it carries a rigid body.
bool NodeCollisionCenter(void* node, float& cx, float& cy, float& cz)
{
    auto* n = reinterpret_cast<UInt8*>(node);
    UInt8* collObj = *reinterpret_cast<UInt8**>(n + kNiAV_CollisionObject);
    if (!collObj || IsBadReadPtr(collObj, 0x14)) return false;
    UInt8* worldObj = *reinterpret_cast<UInt8**>(collObj + 0x10);
    if (!worldObj || IsBadReadPtr(worldObj, 0x2C)) return false;
    UInt8* hkWorldObj = *reinterpret_cast<UInt8**>(worldObj + 0x08);
    if (!hkWorldObj || IsBadReadPtr(hkWorldObj, 0x2C)) return false;
    // collisionType 1 = rigid body. Phantoms (2) have no shape diagonal at all,
    // and asking them for one calls into the wrong slot.
    if (*(hkWorldObj + 0x28) != 1) return false;

    UInt32* vtable = *reinterpret_cast<UInt32**>(worldObj);
    if (!vtable || IsBadReadPtr(vtable, 0xFC)) return false;
    using GetDiagFn = void (__thiscall*)(UInt8*, HkDiagonal*);
    auto getDiag = reinterpret_cast<GetDiagFn>(vtable[0xF8 / 4]);
    if (!getDiag) return false;

    HkDiagonal d{};
    getDiag(worldObj, &d);
    cx = (d.min.x + d.max.x) * 0.5f * kHavokToGame;
    cy = (d.min.y + d.max.y) * 0.5f * kHavokToGame;
    cz = (d.min.z + d.max.z) * 0.5f * kHavokToGame;
    return true;
}

} // namespace

bool GetCollisionAABBCenter(const void* refr, float* outX, float* outY, float* outZ)
{
    void* root = RefNiNode(reinterpret_cast<TESObjectREFR*>(const_cast<void*>(refr)));
    if (!root) return false;

    // Breadth of the model tree, iteratively — a recursive walk over game memory
    // is a crash waiting for a malformed node.
    void* stack[64];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        void* node = stack[--top];
        if (!node || IsBadReadPtr(node, 0xA0)) continue;

        float cx, cy, cz;
        if (NodeCollisionCenter(node, cx, cy, cz)) {
            if (outX) *outX = cx;
            if (outY) *outY = cy;
            if (outZ) *outZ = cz;
            return true;
        }

        // Only real NiNodes have children; the cast is the engine's own test.
        UInt32* vt = *reinterpret_cast<UInt32**>(node);
        if (!vt || IsBadReadPtr(vt, (kNiAV_GetAsNiNode + 1) * 4)) continue;
        using AsNodeFn = void* (__thiscall*)(void*);
        auto asNode = reinterpret_cast<AsNodeFn>(vt[kNiAV_GetAsNiNode]);
        if (!asNode) continue;
        void* asNiNode = asNode(node);
        if (!asNiNode) continue;

        UInt8* arr = reinterpret_cast<UInt8*>(asNiNode) + kNiNode_Children;
        if (IsBadReadPtr(arr, 0x10)) continue;
        void** children = *reinterpret_cast<void***>(arr + 0x04);
        UInt16 count    = *reinterpret_cast<UInt16*>(arr + 0x0A);
        if (!children || count > 512 || IsBadReadPtr(children, count * 4)) continue;
        for (UInt16 i = 0; i < count && top < 64; ++i)
            if (children[i]) stack[top++] = children[i];
    }
    return false;
}

// Where the RENDERING camera is, which is where shots come from. In first person
// that is near the player's eyes but not at their origin, and the difference is
// exactly what makes a pitch computed from the feet miss.
bool GetCameraWorldPos(float* x, float* y, float* z)
{
    auto* ifm = rt::IFM();
    if (!ifm) return false;
    // InterfaceManager::sceneGraph004 (+0x04) owns the main camera.
    UInt8* sg = *reinterpret_cast<UInt8**>(reinterpret_cast<UInt8*>(ifm) + 0x04);
    if (!sg || IsBadReadPtr(sg, 0xB0)) return false;
    UInt8* cam = *reinterpret_cast<UInt8**>(sg + 0xAC);
    if (!cam || IsBadReadPtr(cam, kNiAV_WorldTranslate + 12)) return false;
    float* t = reinterpret_cast<float*>(cam + kNiAV_WorldTranslate);

    // Sanity: the camera lives on the player. If this offset means something
    // else in this build, the result lands far away — then we say so and the
    // caller falls back to the player's eye position instead of aiming at junk.
    auto* p = rt::Player();
    if (!p) return false;
    if (!(std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2])))
        return false;
    {
        float dx = t[0] - p->posX, dy = t[1] - p->posY, dz = t[2] - p->posZ;
        if (!(dx * dx + dy * dy + dz * dz <= 600.0f * 600.0f)) return false;
    }
    if (x) *x = t[0];
    if (y) *y = t[1];
    if (z) *z = t[2];
    return true;
}

// The object's own dimensions, from the BASE FORM — TESBoundObject::bounds at
// +0x24: six signed shorts, min x/y/z then max x/y/z, loaded straight from the
// game's data files. This is plain data: no model, no virtual calls, nothing
// that can crash. It gives the real distance from an object's origin to its
// middle, which is exactly what a fixed "+100 units" was pretending to know.
bool BaseFormCenterOffset(TESForm* base, float* ox, float* oy, float* oz)
{
    if (!base || IsBadReadPtr(base, 0x30)) return false;
    auto* b = reinterpret_cast<const int16_t*>(
        reinterpret_cast<const UInt8*>(base) + 0x24);
    int mnx = b[0], mny = b[1], mnz = b[2];
    int mxx = b[3], mxy = b[4], mxz = b[5];
    // Reject nonsense: a form that isn't a bound object has something else here.
    if (mxx < mnx || mxy < mny || mxz < mnz) return false;
    int spanX = mxx - mnx, spanY = mxy - mny, spanZ = mxz - mnz;
    if (spanX == 0 && spanY == 0 && spanZ == 0) return false;
    if (spanX > 20000 || spanY > 20000 || spanZ > 20000) return false;
    if (ox) *ox = (mnx + mxx) * 0.5f;
    if (oy) *oy = (mny + mxy) * 0.5f;
    if (oz) *oz = (mnz + mxz) * 0.5f;
    return true;
}

// The aim point from the target's own SKELETON — the upper torso, the neck, the
// spine, in that order. This is how the New Vegas accessibility mod resolves it,
// and reading its source is what settled the question here: it does NOT aim at
// a model's middle. The difference matters because a base form's centre is
// derived from the object's bounding box, which for a creature low to the ground
// sits near its feet, and a shot sent there meets the floor or whatever is
// standing between — which is precisely what a log of a missed pistol shot
// showed (crosshair on an object at 170 units, target at 258).
//
// Unlike the collision-shape route this is plain data: node names and world
// positions, no Havok virtuals, nothing that can call into the wrong slot. The
// offsets come from the same Gamebryo layout, so each result is still checked
// against the reference's own position before it is believed.
namespace {
constexpr const char* kBoneCandidates[] = {
    "Bip01 Spine2",   // upper torso — the best aim point on a humanoid
    "Bip01 Neck",
    "Bip01 Spine1",
    "Bip01 Spine",
    "Bip01 Pelvis",
    "Bip01 Head",
    "Bip01",          // root — last resort for skinned creatures
};

// A coordinate we are willing to act on. NaN is the dangerous case, not a big
// number: every comparison against NaN is false, so a plain "reject if too far"
// test PASSES it. That is how a bone position of (0,0,0) on a reference whose
// own origin read as 869108023296 got through and took the game down.
bool SanePos(float x, float y, float z)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const float kWorldLimit = 4.0e6f;      // far beyond any real cell coordinate
    return std::fabs(x) < kWorldLimit && std::fabs(y) < kWorldLimit &&
           std::fabs(z) < kWorldLimit;
}

bool FindBoneWorldPos(TESObjectREFR* r, Vec3* out, const char** boneName)
{
    // Nothing is read off the model until the REFERENCE itself looks real. If
    // its own position is nonsense then the pointer is not a reference at all,
    // and walking its 3D is walking arbitrary memory.
    if (!r || !SanePos(r->posX, r->posY, r->posZ)) return false;
    void* root = RefNiNode(r);
    if (!root) return false;

    for (const char* want : kBoneCandidates) {
        void* stack[96];
        int   top = 0;
        stack[top++] = root;
        while (top > 0) {
            UInt8* node = reinterpret_cast<UInt8*>(stack[--top]);
            if (!node || IsBadReadPtr(node, kNiAV_WorldTranslate + 12)) continue;

            const char* nm = *reinterpret_cast<const char**>(node + 0x08);
            if (nm && !IsBadReadPtr((void*)nm, 1) && _stricmp(nm, want) == 0) {
                float* wt = reinterpret_cast<float*>(node + kNiAV_WorldTranslate);
                // Believe it only if it is a real coordinate AND actually ON
                // this reference. Written as an explicit "must be within" test,
                // never as "reject if beyond": the second form lets NaN pass.
                if (!SanePos(wt[0], wt[1], wt[2])) break;
                float dx = wt[0] - r->posX, dy = wt[1] - r->posY,
                      dz = wt[2] - r->posZ;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (!(std::isfinite(d2) && d2 <= 400.0f * 400.0f)) break;
                *out = { wt[0], wt[1], wt[2] };
                if (boneName) *boneName = want;
                return true;
            }

            // Only an NiNode has children; the vtable slot says whether it is.
            void** vt = *reinterpret_cast<void***>(node);
            if (!vt || IsBadReadPtr(vt, (kNiAV_GetAsNiNode + 1) * 4)) continue;
            using AsNode = void* (__thiscall*)(void*);
            void* asNode = reinterpret_cast<AsNode>(vt[kNiAV_GetAsNiNode])(node);
            if (!asNode || IsBadReadPtr(asNode, kNiNode_Children + 0xC)) continue;
            UInt8* arr = reinterpret_cast<UInt8*>(asNode) + kNiNode_Children;
            void** kids = *reinterpret_cast<void***>(arr + 0x4);
            UInt16  n   = *reinterpret_cast<UInt16*>(arr + 0xA);
            if (!kids || n > 512 || IsBadReadPtr(kids, (size_t)n * 4)) continue;
            for (UInt16 i = 0; i < n && top < 96; ++i)
                if (kids[i]) stack[top++] = kids[i];
        }
    }
    return false;
}
} // namespace

// Objects whose HITTABLE part is not the middle of their bounds. Measured from
// the model's own collision hull — the shape a bullet actually registers on —
// in model space (x, y, z from the origin, before the reference's rotation).
namespace {
struct HitCentre { UInt32 baseId; float x, y, z; };
const HitCentre kHitCentres[] = {
    // CG02Target, the vault's BB-gun practice target (all three on the range
    // share it). Its bounds span the whole stand, z -50..67, so their middle
    // (z +8.5) is the bare post: every shot aimed there missed. The part that
    // takes the hit is a disc ~26 units across at the top — the collision hull
    // of clutter\questitems\cg02targetpractice01.nif, centred here.
    { 0x000304DB, -2.4f, 42.5f, 54.7f },
};
} // namespace

bool GetAimPointFor(const void* refr, Vec3* out, const char** how)
{
    if (how) *how = "none";
    auto* r = reinterpret_cast<TESObjectREFR*>(const_cast<void*>(refr));
    if (!r || IsBadReadPtr(r, 0x40) || !out) return false;
    // IsBadReadPtr only proves the memory is readable, not that a reference
    // lives there. A log caught this function being handed something whose
    // position read as (0,0,869108023296) — readable, and not a reference.
    if (!SanePos(r->posX, r->posY, r->posZ)) {
        F3A_INFO("AimPoint: refusing %p — its position is not a real coordinate "
                 "(%.0f,%.0f,%.0f)", (void*)r, r->posX, r->posY, r->posZ);
        return false;
    }

    // Bones first: this is the point a sighted player's crosshair ends up on.
    const char* bone = nullptr;
    if (config::Get().aim_bones && OnMainThread() && FindBoneWorldPos(r, out, &bone)) {
        if (how) *how = bone;
        F3A_INFO("AimPoint: bone '%s' at (%.0f,%.0f,%.0f), origin (%.0f,%.0f,%.0f)",
                 bone, out->x, out->y, out->z, r->posX, r->posY, r->posZ);
        return true;
    }

    // The collision centre would be ideal, but reaching it requires calling into
    // the model, and BOTH attempts at that crashed the game on this build: first
    // the Havok virtuals, then the 3D getter itself (the log stopped exactly at
    // the call). Those offsets came from New Vegas. Opt-in only, and off by
    // default, until they can be verified against FO3 rather than assumed.
    if (config::Get().aim_collision) {
        float cx, cy, cz;
        F3A_INFO("AimPoint: trying collision centre (opt-in)");
        if (GetCollisionAABBCenter(refr, &cx, &cy, &cz)) {
            *out = { cx, cy, cz };
            if (how) *how = "collision";
            return true;
        }
    }

    // A known hittable part first (see kHitCentres), rotated into the
    // reference's facing like the bounds centre below.
    if (r->baseForm && !IsBadReadPtr(r->baseForm, 0x10)) {
        UInt32 bid = r->baseForm->refID;
        for (const auto& h : kHitCentres) {
            if (h.baseId != bid) continue;
            float c = std::cos(r->rotZ), sn = std::sin(r->rotZ);
            out->x = r->posX + (h.x * c - h.y * sn);
            out->y = r->posY + (h.x * sn + h.y * c);
            out->z = r->posZ + h.z;
            if (how) *how = "hit-centre";
            return true;
        }
    }

    // Default: origin plus the base form's own centre offset, rotated into the
    // reference's facing so a long object leans the right way.
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    if (BaseFormCenterOffset(r->baseForm, &ox, &oy, &oz)) {
        float c = std::cos(r->rotZ), sn = std::sin(r->rotZ);
        out->x = r->posX + (ox * c - oy * sn);
        out->y = r->posY + (ox * sn + oy * c);
        out->z = r->posZ + oz;
        if (how) *how = "bounds";
        return true;
    }

    // The model's own world bound: centre and radius of the sphere the
    // renderer uses for the loaded 3D. It is model-derived, so unlike a fixed
    // height it is right for a dog, a child and a Super Mutant alike — the FNV
    // mod uses exactly this as its own last fallback. Thirty per cent of the
    // radius above the centre lands on the upper body rather than the middle
    // of the bounding sphere, which for a standing figure is the waist.
    // Behind the same switch as the bone walk, and for the same reason: it
    // reaches the model through a virtual call, which is only safe on the
    // game's own thread.
    if (void* root = (config::Get().aim_bones && OnMainThread()) ? RefNiNode(r) : nullptr) {
        if (!IsBadReadPtr(root, 0x24)) {
            auto* wb = *reinterpret_cast<float**>(reinterpret_cast<UInt8*>(root) + 0x20);
            if (wb && !IsBadReadPtr(wb, 16)) {
                float cx = wb[0], cy = wb[1], cz = wb[2], rad = wb[3];
                // "Must be within", never "reject if beyond": NaN passes the
                // second form, and a NaN aim point crashed this game twice.
                if (SanePos(cx, cy, cz) && std::isfinite(rad) &&
                    rad >= 0.0f && rad <= 2000.0f) {
                    *out = { cx, cy, cz + rad * 0.3f };
                    if (how) *how = "model-bound";
                    return true;
                }
            }
        }
    }

    // Last resort: the origin, lifted to roughly chest height. A guess, and
    // reported as one.
    *out = { r->posX, r->posY, r->posZ + 100.0f };
    if (how) *how = "origin+100";
    return true;
}

bool AimAtReference(const void* refr, std::string* detail)
{
    F3A_INFO("AimAtReference: start refr=%p", refr);
    Vec3 target{};
    const char* how = "none";
    if (!GetAimPointFor(refr, &target, &how)) return false;
    F3A_INFO("AimAtReference: aim point %s (%.0f,%.0f,%.0f)",
             how, target.x, target.y, target.z);

    // Aim FROM the camera when we can read it; the player's origin is at their
    // feet, and a pitch measured from there is wrong by the player's height.
    float sx, sy, sz;
    const char* src = "camera";
    F3A_INFO("AimAtReference: reading camera");
    if (!GetCameraWorldPos(&sx, &sy, &sz)) {
        auto* p = rt::Player();
        if (!p) return false;
        sx = p->posX; sy = p->posY; sz = p->posZ + 100.0f;
        src = "eyes";
    }

    float dx = target.x - sx, dy = target.y - sy, dz = target.z - sz;
    float horiz = std::sqrt(dx * dx + dy * dy);
    const float R2D = 57.2957795f;
    float yawDeg   = std::atan2(dx, dy) * R2D;                         // 0 = north
    float pitchUp  = std::atan2(dz, horiz > 1.0f ? horiz : 1.0f) * R2D; // + = above

    // Heading through the engine's own turning routine, elevation through
    // SetAngle (negative looks up). No bias terms: if this misses, the aim point
    // or the source position is wrong, and that is what should be fixed.
    FacePointNative(target);
    SetPlayerAngleDeg('X', -pitchUp);

    F3A_INFO("AimAtReference: point=%s src=%s target=(%.0f,%.0f,%.0f) "
             "yaw=%.1f pitch=%.1f dist=%.0f",
             how, src, target.x, target.y, target.z, yawDeg, pitchUp, horiz);
    if (detail) *detail = how;
    return true;
}

// Is the player holding the game's AIM control right now?
//
// Reading the CONTROL rather than a fixed key means this follows whatever the
// player bound it to — right mouse button by default, but remappable, and the
// mod has no business assuming. FOSE exposes the bind tables; we then ask
// Windows whether that physical key or mouse button is down, which is what the
// FNV accessibility mod does for the same purpose.
// OSInputGlobals for THIS build (FOSE's ladder: 1.7 = 0x011795C4, no-gore
// 0x011765C4). The old hard-coded 0x01176524 was the 1.0.15 address, so every
// key-binding read — hint tokens, the Use key, the aim control — read some
// other object.
OSInputGlobals* InputGlobals()
{
    auto* pp = reinterpret_cast<OSInputGlobals**>(rt::g_addrs->inputGlobals);
    if (!pp || IsBadReadPtr(pp, 4)) return nullptr;
    return *pp;
}

// Is the key bound to this control (a DirectInput scancode) held? Codes from
// 0x80 up are E0-prefixed (the arrows, Insert/Delete, Home/End, PgUp/PgDn);
// plain MAPVK_VSC_TO_VK does not translate those, so a control bound to an
// arrow key read as never held.
bool BoundKeyHeld(OSInputGlobals* globs, UInt32 control)
{
    UInt8 kb = globs->keyBinds[control];
    if (kb == 0xFF || kb == 0) return false;
    UINT vk = kb >= 0x80 ? MapVirtualKeyA(0xE000u | (kb & 0x7Fu), MAPVK_VSC_TO_VK_EX)
                         : MapVirtualKeyA(kb, MAPVK_VSC_TO_VK);
    return vk && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
}

bool IsAimControlHeld()
{
    constexpr UInt32 kControl_Aim = 6;
    auto* globs = InputGlobals();
    if (!globs || IsBadReadPtr(globs, 0x1BC0)) return false;
    if (kControl_Aim >= OSInputGlobals::kMaxControlBinds) return false;

    if (BoundKeyHeld(globs, kControl_Aim)) return true;
    UInt8 mb = globs->mouseBinds[kControl_Aim];
    if (mb != 0xFF) {
        static const int kMouseVK[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                                        VK_XBUTTON1, VK_XBUTTON2 };
        if (mb < (UInt8)(sizeof(kMouseVK) / sizeof(kMouseVK[0])) &&
            (GetAsyncKeyState(kMouseVK[mb]) & 0x8000)) return true;
    }
    return false;
}

bool IsControlKeyHeld(uint32_t control)
{
    auto* globs = InputGlobals();
    if (!globs || IsBadReadPtr(globs, 0x1BC0)) return false;
    if (control >= OSInputGlobals::kMaxControlBinds) return false;
    return BoundKeyHeld(globs, control);
}

// Does the player have a weapon out? Aim assistance has no business running
// while the gun is holstered.
bool IsWeaponOut()
{
    auto* p = rt::Player();
    if (!p) return false;
    // Actor::isWeaponOut, a byte on the actor; guarded like every raw read.
    const UInt8* b = reinterpret_cast<const UInt8*>(p) + 0x19C;
    if (IsBadReadPtr((void*)b, 1)) return false;
    return *b != 0;
}

// ---- VATS limbs -----------------------------------------------------------
//
// VATS draws ONE TILE PER BODY PART (eight of them on a humanoid), each with its
// own screen position, its name and its hit chance — "Korpus 95%", "Prawe nogi
// 95%". There is no working button to cycle them: the menu's BodyPart button is
// invisible on this build, which is why clicking it did nothing. Selecting a
// limb means pointing at ITS tile, and queueing a shot means clicking there.
namespace {
// Per-limb body-part id carried on the limb_percent tile (Korpus=3, Prawe
// nogi=1, Lewe nogi=2, Glowa=4 in the dump). Identical across the duplicate
// tiles of one body part, so it identifies a limb independently of its name.
constexpr UInt32 kTrait_VatsBodyPartId = 0x27D4;

// The SELECTED body part. In the dump this is 1 on exactly one limb and 0 on
// the other three, and that same limb is independently singled out two other
// ways: it is drawn at a different alpha (119.9 against 255) and it is the only
// one showing its name label. Three signals agreeing is why this is the marker
// and not the name label, which turned out not to be set at all in play.
constexpr UInt32 kTrait_VatsLimbSelected = 0x27D6;

int g_limb_tiles_seen = 0;   // all limb_percent tiles, before the visible filter

void CollectVatsLimbs(Tile* t, int depth, std::vector<VatsLimb>& out)
{
    if (!t || depth > 10 || out.size() >= 32) return;
    // VATS draws ONE limb_percent tile per skeleton node, so a creature with
    // several legs mapped to the same body part gets several identical tiles —
    // which is why cycling read "Prawe nogi" three times before moving on. A
    // dump settled it: of eight tiles, exactly four carry visible=1, and those
    // four are exactly the four distinct body parts. So the visible one is the
    // representative tile and the rest are duplicates.
    // Every limb tile is counted, visible or not, so the log can answer the
    // question the visibility filter raises: are the other parts simply not
    // drawn yet, or are they there and hidden? A target that offered exactly
    // ONE part called "Butch" — the target's own name, not a body part — is
    // either a menu that had not finished drawing or a filter that is too
    // strict, and only the counts tell those apart.
    if (TileNameHas(t, "limb_percent")) ++g_limb_tiles_seen;
    if (TileNameHas(t, "limb_percent") && TileVisible(t)) {
        VatsLimb l;
        l.tile = t;
        l.x = TileAbsX(t);
        l.y = TileAbsY(t);
        l.w = TileNum(t, kTileValue_width);
        l.h = TileNum(t, kTileValue_height);
        l.part_id = (int)TileNum(t, kTrait_VatsBodyPartId);
        l.selected = TileNum(t, kTrait_VatsLimbSelected) != 0.0f;
        if (Tile* n = FindChildByName(t, "limb_name", 3)) {
            // Fallback only: the selected limb also shows its name label, but
            // that label was not set during play, so it cannot be the primary
            // test — it is kept as corroboration if the flag ever goes missing.
            if (!l.selected) l.selected = TileVisible(n);
            if (const char* raw = TileStringTrait(n)) {
                l.name = GameStrToUtf8(raw);
                // The game pads these with tabs and spaces.
                while (!l.name.empty() &&
                       (l.name.back() == ' ' || l.name.back() == '	'))
                    l.name.pop_back();
            }
        }
        if (Tile* c = FindChildByName(t, "chance_to_hit", 3))
            if (const char* raw = TileStringTrait(c)) l.chance = GameStrToUtf8(raw);
        // Same body part twice (several visible nodes) — keep the first, but
        // let a later tile contribute the selection flag.
        //
        // Matched on the NAME as well as the id: a human target came back with
        // sixteen tiles carrying only six distinct names, so the id alone does
        // not collapse them. The name is also what the player hears, and two
        // entries that read identically are not a choice.
        if (!l.name.empty()) {
            bool dup = false;
            for (auto& e : out) {
                bool same = (l.part_id != 0 && e.part_id == l.part_id) ||
                            e.name == l.name;
                if (!same) continue;
                if (l.selected) { e.selected = true; e.tile = l.tile;
                                  e.x = l.x; e.y = l.y; }
                dup = true;
                break;
            }
            if (!dup) out.push_back(l);
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectVatsLimbs(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

std::vector<VatsLimb> GetVatsLimbs()
{
    std::vector<VatsLimb> out;
    g_limb_tiles_seen = 0;
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return out;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            auto* tm = reinterpret_cast<TileMenu*>(cn->child);
            Menu* m = tm->menu;
            if (m && m->typeID == kMenuType_VATS) {
                CollectVatsLimbs(cn->child, 0, out);
                break;
            }
        }
        node = node->next;
    }
    // A STABLE ORDER. The tile tree is walked fresh every frame and comes back
    // in a different order each time — the log caught the same four parts as
    // [Korpus, Prawe nogi, Lewe nogi, Głowa] on one frame and
    // [Prawe nogi, Lewe nogi, Głowa, Korpus] on the next. An index into a list
    // that reshuffles itself is meaningless, which is why stepping with W and S
    // jumped about and kept landing back on the head. Sorted by the engine's own
    // part id, then by name, so position N means the same part from one frame to
    // the next.
    std::sort(out.begin(), out.end(),
              [](const VatsLimb& a, const VatsLimb& b) {
                  if (a.part_id != b.part_id) return a.part_id < b.part_id;
                  return a.name < b.name;
              });

    static int  last_n = -1;
    static int  last_sel = -1;
    int sel = -1;
    for (size_t i = 0; i < out.size(); ++i) if (out[i].selected) { sel = (int)i; break; }
    if ((int)out.size() != last_n || sel != last_sel) {
        // The whole list, not just the pick. Which parts the engine is even
        // offering is the thing we cannot see from the outside, and "W and S
        // do not read body parts" turned out to be a question about this list
        // rather than about the keys.
        std::string all;
        for (size_t i = 0; i < out.size(); ++i) {
            if (i) all += ", ";
            all += out[i].name;
        }
        last_n = (int)out.size();
        last_sel = sel;
        F3A_INFO("GetVatsLimbs: %d of %d tile(s) usable, selected=%d (%s) | %s",
                 last_n, g_limb_tiles_seen, sel,
                 sel >= 0 ? out[sel].name.c_str() : "none", all.c_str());
    }
    return out;
}

// How many shots are queued, counted from the list VATS draws on screen.
// The menu-struct offset we had for this came from the New Vegas mod and does
// not hold here — the log shows it reading maxAP as 0 and the body-part pointer
// as null, so nothing read through those offsets can be trusted. The list, by
// contrast, is what the player sees.
int GetVatsQueuedCount()
{
    Tile* vats = FindVisibleMenuTile(kMenuType_VATS);
    if (!vats) return -1;
    Tile* list = FindChildByName(vats, "queued_actions", 6);
    if (!list) return -1;
    int n = 0;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&list->childList);
    for (int i = 0; node && i < 256; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            const char* nm = cn->child->name.m_data;
            // The listbox's own furniture is always there; the rest are entries.
            if (nm && std::strcmp(nm, "lb_scrollbar") != 0 &&
                      std::strcmp(nm, "lb_highlight_box") != 0)
                ++n;
        }
        node = node->next;
    }
    return n;
}

// Where on screen the SELECTED limb's marker sits. Queueing a shot in VATS is
// a positional click — the game only accepts it when the cursor is actually on
// a body-part marker — so this is the spot the cursor has to reach.
bool GetVatsSelectedLimbPos(float* x, float* y)
{
    auto limbs = GetVatsLimbs();
    for (const auto& l : limbs) {
        if (!l.selected) continue;
        if (x) *x = l.x;
        if (y) *y = l.y;
        return true;
    }
    return false;
}

bool GetVatsPick(VatsPick* out)
{
    if (!out) return false;
    *out = VatsPick{};
    out->queued = GetVatsQueuedCount();
    auto limbs = GetVatsLimbs();
    out->count = (int)limbs.size();
    for (size_t i = 0; i < limbs.size() && i < 8; ++i) {
        std::snprintf(out->names[i], sizeof(out->names[i]), "%s", limbs[i].name.c_str());
        std::snprintf(out->chances[i], sizeof(out->chances[i]), "%s", limbs[i].chance.c_str());
    }
    for (size_t i = 0; i < limbs.size(); ++i) {
        if (!limbs[i].selected) continue;
        out->index = (int)i + 1;
        std::snprintf(out->name, sizeof(out->name), "%s", limbs[i].name.c_str());
        std::snprintf(out->chance, sizeof(out->chance), "%s", limbs[i].chance.c_str());
        break;
    }
    return out->count > 0;
}

// Click a limb's tile through the menu's own handler — the same path that works
// for the target arrows. This both selects the limb and queues a shot at it,
// which is what clicking it with a mouse does.
bool ClickVatsLimb(const void* tile)
{
    Menu* menu = FindMenuByType(kMenuType_VATS);
    auto* t = reinterpret_cast<Tile*>(const_cast<void*>(tile));
    if (!menu || !t || IsBadReadPtr(t, 0x40)) return false;
    UInt32 id = (UInt32)TileNum(t, kTileValue_id);
    F3A_INFO("ClickVatsLimb: id=%u vis=%d", id, (int)TileVisible(t));
    menu->HandleClick(id, t);
    return true;
}

// ---- Control placeholders in the game's own text --------------------------
//
// Tutorial hints arrive with the game's control markers still in them, e.g.
//   "Naciśnij i przytrzymaj &-sUActnMenumode;, aby uruchomić światło Pip-Boya."
// The engine swaps those for a button icon on a gamepad; with a keyboard it
// leaves them as they are, so a screen reader reads out "su act n menumode".
// We replace each one with the key the player ACTUALLY has bound, read from the
// game's own control table — so remapping a key changes the hint too, and no
// list of keys is hard-coded here.
namespace {

struct ControlName { const char* name; UInt32 index; };

// Bethesda's control order, as used by the bind tables.
constexpr ControlName kControlNames[] = {
    { "forward",     0 },  { "back",        1 },
    { "left",        2 },  { "right",       3 },
    { "attack",      4 },  { "activate",    5 },  { "use", 5 },
    { "block",       6 },  { "aim",         6 },
    { "readyweapon", 7 },  { "sneak",       8 },  { "crouch", 8 },
    { "run",         9 },  { "alwaysrun",  10 },
    { "automove",   11 },  { "jump",       12 },
    { "togglepov",  13 },  { "menumode",   14 },  { "pipboy", 14 },
    { "rest",       15 },  { "vats",       16 },
    { "quicksave",  25 },  { "quickload",  26 },  { "grab", 27 },
};

std::string LowerAscii(std::string v)
{
    for (char& c : v) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return v;
}

// "sUActnMenumode" -> "menumode": strip the engine's prefix letters, keeping
// only the action itself.
std::string StripControlPrefix(const std::string& token)
{
    std::string low = LowerAscii(token);
    static const char* kPrefixes[] = { "suactn", "suact", "sact", "sn", "su" };
    for (const char* pre : kPrefixes) {
        size_t n = std::strlen(pre);
        if (low.size() > n && low.compare(0, n, pre) == 0) return low.substr(n);
    }
    return low;
}

// The readable name of a DirectInput scancode, from Windows — so it comes out
// in the user's own language and matches their keyboard layout.
// The scancode the PLAYER has bound to a control, read from the game's own
// bind table. Index 5 is Activate/Use.
//
// This exists because the Use key was hard-coded to E. E is only the DEFAULT:
// a player who has remapped it — and a blind player is quite likely to have —
// got a keypress sent to a key that does nothing, with no way to tell from the
// outside why activating silently failed. 0 means "bound to nothing" (or to a
// mouse button), and the caller falls back to the default.
UInt8 BoundScancode(UInt32 controlIndex)
{
    auto* globs = InputGlobals();
    if (!globs || IsBadReadPtr(globs, 0x1BC0)) return 0;
    if (controlIndex >= OSInputGlobals::kMaxControlBinds) return 0;
    UInt8 k = globs->keyBinds[controlIndex];
    return k == 0xFF ? 0 : k;          // 0xFF = bound to nothing
}

std::string KeyNameForScancode(UInt8 dik)
{
    if (dik == 0 || dik == 0xFF) return {};
    char buf[64] = {};
    // DirectInput marks extended keys (arrows, Home, Ins, right Ctrl...) with
    // bit 7; GetKeyNameText wants the low 7 bits plus its own extended flag.
    LONG lparam = ((LONG)(dik & 0x7F) << 16) | ((dik & 0x80) ? (1L << 24) : 0);
    if (GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0 && buf[0]) return buf;
    return {};
}

} // namespace

std::string ExpandControlTokens(const std::string& text)
{
    if (text.find('&') == std::string::npos) return text;

    auto* globs = InputGlobals();
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = text.find('&', pos);
        if (start == std::string::npos) { out += text.substr(pos); break; }
        size_t end = text.find(';', start);
        if (end == std::string::npos) { out += text.substr(pos); break; }

        // Only a real marker: short, letters/digits/underscore/minus. A plain
        // "&" followed somewhere later by a ";" is text, not a token.
        bool token = end - start <= 40;
        for (size_t k = start + 1; token && k < end; ++k) {
            char ch = text[k];
            token = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        }
        if (!token) {
            out += text.substr(pos, start - pos + 1);
            pos = start + 1;
            continue;
        }

        out += text.substr(pos, start - pos);
        // Both "&-name;" and "&name;" appear in the game's text.
        size_t nameAt = start + 1;
        if (nameAt < text.size() && text[nameAt] == '-') ++nameAt;
        std::string action = StripControlPrefix(text.substr(nameAt, end - nameAt));

        std::string replacement;
        for (const auto& cn : kControlNames) {
            if (action != cn.name) continue;
            if (globs && !IsBadReadPtr(globs, 0x1BC0) &&
                cn.index < OSInputGlobals::kMaxControlBinds) {
                replacement = KeyNameForScancode(globs->keyBinds[cn.index]);
                if (replacement.empty()) {
                    UInt8 mb = globs->mouseBinds[cn.index];
                    static const char* kMouse[][2] = {
                        { "lewy przycisk myszy",     "left mouse button" },
                        { "prawy przycisk myszy",    "right mouse button" },
                        { "środkowy przycisk myszy", "middle mouse button" } };
                    if (mb < 3) replacement = strings::T(kMouse[mb][0], kMouse[mb][1]);
                }
            }
            break;
        }
        // Unknown marker, or a control bound to nothing: drop it rather than
        // read the raw token aloud.
        out += replacement;
        pos = end + 1;
    }
    return out;
}

// The player's own Activate/Use key, or 0 when it is on a mouse button or
// unbound. Defined here, after BoundScancode, which it uses.
uint8_t GetUseKeyScancode() { return BoundScancode(5); }


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
namespace { std::atomic<DWORD> g_teleport_at{ 0 }; }
constexpr DWORD kWorldSettleMs = 2500;

bool WorldSettling()
{
    const DWORD t = g_teleport_at.load();
    return t != 0 && GetTickCount() - t < kWorldSettleMs;
}

void TeleportPlayerToRef(const void* targetRefr, uint32_t expectedRefID)
{
    auto* p = rt::Player();
    if (!p || !targetRefr) {
        F3A_INFO("Teleport: aborted (player=%p target=%p)", (void*)p, targetRefr);
        return;
    }
    // A scanner or walk target can be unloaded (and its memory reused) by the
    // time the key is pressed: MoveTo on that is a crash or a jump to junk.
    {
        auto* t = reinterpret_cast<const TESObjectREFR*>(targetRefr);
        if (IsBadReadPtr(t, 0x40) || (expectedRefID && t->refID != expectedRefID) ||
            !SanePos(t->posX, t->posY, t->posZ)) {
            F3A_INFO("Teleport: target %p is gone or reused (want %08X) - aborted",
                     targetRefr, expectedRefID);
            return;
        }
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
    // From here the world is in motion: settle before touching it again.
    g_teleport_at.store(GetTickCount() | 1u);
    ForgetHostiles();
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
std::string GetRefPlaceName(const void* refr)
{
    if (!refr || IsBadReadPtr((void*)refr, 0x40)) return {};
    auto* r = reinterpret_cast<const TESObjectREFR*>(refr);
    TESObjectCELL* c = r->parentCell;
    if (!c || IsBadReadPtr(c, 0xC4)) return {};
    TESWorldSpace* ws = c->worldSpace;
    if (!ws) {
        const char* cn = c->fullName.name.m_data;
        if (cn && !IsBadReadPtr((void*)cn, 1) && *cn) return GameStrToUtf8(cn);
        return {};
    }
    if (IsBadReadPtr(ws, 0x24)) return {};
    const char* wn = ws->fullName.name.m_data;
    return (wn && !IsBadReadPtr((void*)wn, 1) && *wn) ? GameStrToUtf8(wn) : std::string();
}

std::string GetRefSpaceLabel(const void* refr)
{
    if (!refr || IsBadReadPtr((void*)refr, 0x40)) return "(bad ref)";
    auto* r = reinterpret_cast<const TESObjectREFR*>(refr);
    TESObjectCELL* c = r->parentCell;
    if (!c || IsBadReadPtr(c, 0xC4)) return "(no cell)";
    const char* cn = c->fullName.name.m_data;
    std::string out = (cn && !IsBadReadPtr((void*)cn, 1) && *cn)
                          ? GameStrToUtf8(cn) : "(unnamed)";
    TESWorldSpace* ws = c->worldSpace;
    char tail[64];
    if (ws && !IsBadReadPtr(ws, 0x24)) {
        const char* wn = ws->fullName.name.m_data;
        std::string wsn = (wn && !IsBadReadPtr((void*)wn, 1) && *wn)
                              ? GameStrToUtf8(wn) : "(unnamed worldspace)";
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
// Is this reference inside? Needed to tell a wasteland route from a route
// through a building: outdoors the way on is the nearest exterior door, but in
// a place like Vault 101 every step of the way is another interior, and a rule
// that refuses those has nowhere left to go.
bool RefIsInInterior(const void* refr)
{
    TESObjectCELL* c = RefCellGuarded(refr);
    if (!c || IsBadReadPtr(c, 0xC4)) return false;
    return c->worldSpace == nullptr;
}

namespace {
uint32_t CellSpaceId(TESObjectCELL* c)
{
    if (!c || IsBadReadPtr(c, 0xC4)) return 0;
    TESWorldSpace* ws = c->worldSpace;
    if (!ws) return c->refID;                        // an interior: the cell itself
    if (IsBadReadPtr(ws, 0x10)) return 0;
    return ws->refID;
}
} // namespace

uint32_t PlayerSpaceId()
{
    return CellSpaceId(reinterpret_cast<TESObjectCELL*>(const_cast<void*>(GetPlayerCell())));
}

uint32_t RefSpaceId(const void* refr)
{
    return CellSpaceId(RefCellGuarded(refr));
}

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
// Is the player inside? An interior cell has no worldspace, and that single
// fact decides whether fast travel exists at all: in Vault 101 there is no
// world map and no marker to travel to, so advising it there is advice that
// cannot be followed.
bool PlayerIsInInterior()
{
    auto* c = reinterpret_cast<const TESObjectCELL*>(GetPlayerCell());
    if (!c || IsBadReadPtr((void*)c, 0xC4)) return false;
    return c->worldSpace == nullptr;
}

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
    rel = std::isfinite(rel) ? std::remainder(rel, 360.0f) : 0.0f;   // -180..180
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
// A row the menu has HIDDEN — its visible trait present and 0. The Pip-Boy's
// inventory keeps every item of every category in one list and hides the
// ones outside the current tab, at the very same y as the shown ones; matched
// by height alone, the highlight on Med-X read out as "Kombinezon Krypty 101"
// sitting hidden underneath it. Only an explicit 0 counts: rows in other menus
// that carry no visible trait at all must stay selectable.
bool TileHiddenExplicitly(const Tile* t)
{
    if (!t) return false;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == kTileValue_visible) return v->num == 0.0f;
    }
    return false;
}

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

// Is `part` already in `out` as a complete comma-separated element? Used
// instead of a substring test so a short value (a key binding like "V") is not
// swallowed by a longer word that happens to contain it ("VATS").
bool ContainsWholePart(const std::string& out, const std::string& part)
{
    for (size_t pos = 0; (pos = out.find(part, pos)) != std::string::npos; ) {
        size_t end = pos + part.size();
        bool startOk = (pos == 0) || (pos >= 2 && out.compare(pos - 2, 2, ", ") == 0);
        bool endOk   = (end == out.size()) || out.compare(end, 2, ", ") == 0;
        if (startOk && endOk) return true;
        pos = end;
    }
    return false;
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
        // Dedup: row tiles often repeat the same string on a child. Compare
        // WHOLE parts, not substrings — a plain `find` dropped the control
        // binding "V" because it occurs inside the action name "VATS", so that
        // row read as just "VATS" and rebinding it to any other key "fixed" it.
        if (!part.empty() && !ContainsWholePart(out, part)) {
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
    if (TileSelectable(root, label) && !TileHiddenExplicitly(root)) {
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

// The row the game itself treats as selected: keyboard navigation moves the
// interface's ACTIVE tile, which carries mouseover=1 (only one tile in the
// whole interface has it at a time). Hidden rows never count.
Tile* FindMouseoverRow(Tile* root, int depth)
{
    if (!root || depth > 14) return nullptr;
    const char* label = nullptr;
    if (TileNum(root, kTileValue_mouseover) != 0.0f &&
        TileSelectable(root, label) && !TileHiddenExplicitly(root))
        return root;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&root->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child)
            if (Tile* r = FindMouseoverRow(cn->child, depth + 1)) return r;
        node = node->next;
    }
    return nullptr;
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
        // The game's own mark first: the row with mouseover - but only when
        // it sits where the highlight is. Matching by height alone read the
        // wrong row whenever rows of another tab sat at the same height; the
        // mouseover alone stuck to the row under the hidden mouse pointer
        // while the arrows moved the highlight (after switching the item
        // category the list shifts under the pointer): up/down read nothing.
        if (Tile* mo = FindMouseoverRow(listbox, 0)) {
            float d = TileAbsY(mo) - target_y;
            if (d < 0) d = -d;
            if (d < 28.0f) { tile = mo; dy = -1.0f; }
        }
        if (!tile) FindRowAtY(listbox, target_y, 0, dy, tile);
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
    // Pip-Boy: IM_Template_ItemMarker; container: CM_list_template_ItemMarker
    // (a lower-case t - the old search for "Template_" never found it, so no
    // worn item in a container screen was ever said to be worn). Barter's
    // marker also shows an item put up for the deal: told apart by its
    // picture, square_filled = equipped (see DescribeRowParts).
    Tile* mk = FindChildByName(row, "emplate_ItemMarker", 3);
    if (!mk || !TileVisible(mk)) return false;
    const char* f = TileStrTrait(mk, kTileValue_filename);
    return !(mt == kMenuType_Barter && f && !std::strstr(f, "filled"));
}

void DescribeRowParts(Tile* t, std::string& label, std::string& value)
{
    label.clear();
    value.clear();
    if (!t) return;
    // A barter row: its name, its price, and whether it is worn or put up for
    // the deal - the square beside it says which (barter_menu.xml
    // BM_list_template: _Filename_1 square_filled = equipped, _Filename_2
    // square = selected for the transaction).
    // Told by the menu the row is in, not by its name: the name test alone
    // never matched in play ("Pancerz skórzany, 46" - a bare number nobody
    // could place). The price is the ItemValue text, or else the row's one
    // all-digit text.
    if (TileNameHas(t, "BM_list_template") || OwningMenuType(t) == kMenuType_Barter) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            std::string kids;
            struct Node { Tile::ChildNode* item; Node* next; };
            auto* node = reinterpret_cast<Node*>(&t->childList);
            for (int i = 0; node && i < 16; ++i) {
                if (!Readable(node, sizeof(Node))) break;
                Tile::ChildNode* cn = node->item;
                node = node->next;
                if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
                const char* kn = cn->child->name.m_data;
                kids += std::string(kids.empty() ? "" : ", ") + (kn ? kn : "?");
            }
            F3A_INFO("Barter row tile '%s', children: %s",
                     t->name.m_data ? t->name.m_data : "?", kids.c_str());
        }
        Tile* nameT  = FindChildByName(t, "ListItemText", 0);
        Tile* priceT = FindChildByName(t, "ItemValue", 0);
        Tile* mark   = FindChildByName(t, "ItemMarker", 0);
        const char* n = nameT ? TileStringTrait(nameT) : TileStringTrait(t);
        label = n ? GameStrToUtf8(n) : std::string();
        if (label.empty()) return;
        const char* pr = priceT ? TileStringTrait(priceT) : nullptr;
        if (!pr || !*pr) {
            struct Node { Tile::ChildNode* item; Node* next; };
            auto* node = reinterpret_cast<Node*>(&t->childList);
            for (int i = 0; node && i < 16 && !pr; ++i) {
                if (!Readable(node, sizeof(Node))) break;
                Tile::ChildNode* cn = node->item;
                node = node->next;
                if (!cn || !Readable(cn, 0x08) || !cn->child || cn->child == nameT) continue;
                const char* s = TileStringTrait(cn->child);
                if (!s || !*s) continue;
                bool digits = true;
                for (const char* c = s; *c; ++c) if ((*c < '0' || *c > '9') && *c != ' ') digits = false;
                if (digits) pr = s;
            }
        }
        if (pr && *pr)
            label += strings::T(", cena ", ", price ") + GameStrToUtf8(pr);
        if (mark && TileVisible(mark)) {
            const char* f = TileStrTrait(mark, kTileValue_filename);
            if (f && std::strstr(f, "filled")) label += strings::T(", założone", ", equipped");
            else                               label += strings::T(", w transakcji", ", in the deal");
        }
        return;
    }
    CollectRowText(t, 0, label);
    if (!label.empty() && RowIsEquipped(t)) label += strings::T(", założone", ", equipped");

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

    // A dialogue topic already talked through is drawn dim: the row's
    // _line_alpha, which dialog_menu.xml copies to its ListItemText's alpha.
    if (TileNameHas(best, "DM_Topic")) {
        Tile* txt = FindChildByName(best, "ListItemText", 0);
        const float a = txt ? TileNum(txt, kTileValue_alpha) : 255.0f;
        if (a > 0.0f && a <= 128.0f) sel.label += strings::T(", już powiedziane", ", already said");
    }

    // Its place among the rows of the same kind (same tile name) beside it;
    // the highlight box and the scrollbar share the parent and are skipped.
    // Only from a listindex the row really has and no other row shares - a
    // list without one would read "1 of 5" on every row.
    bool hasIndex = false;
    for (UInt32 i = 0; i < best->values.size; ++i) {
        Tile::Value* v = best->values.data[i];
        if (v && v->id == kTileValue_listindex) { hasIndex = true; break; }
    }
    const float li = TileNum(best, kTileValue_listindex);
    if (hasIndex && li >= 0.0f && best->parent && best->name.m_data) {
        int sameIndex = 0;
        struct Node { Tile::ChildNode* item; Node* next; };
        auto* node = reinterpret_cast<Node*>(&best->parent->childList);
        for (int i = 0; node && i < 512; ++i, node = node->next) {
            Tile* c = node->item ? node->item->child : nullptr;
            if (!c || !c->name.m_data || std::strcmp(c->name.m_data, best->name.m_data) != 0)
                continue;
            const float ci = TileNum(c, kTileValue_listindex);
            if (TileHiddenExplicitly(c) || ci < 0.0f) continue;
            ++sel.count;
            if (ci == li) ++sameIndex;
        }
        sel.index = static_cast<int>(li);
        if (sameIndex != 1 || sel.index >= sel.count) { sel.index = -1; sel.count = 0; }
    }

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

std::string SelectionPosition(const MenuSelection& sel)
{
    if (sel.index < 0 || sel.count < 2) return {};
    char pos[32];
    std::snprintf(pos, sizeof(pos), strings::T(", %d z %d", ", %d of %d"),
                  sel.index + 1, sel.count);
    return pos;
}

std::optional<std::string> GetKeyboardSelectionText(bool withPosition)
{
    auto sel = GetKeyboardSelection();
    if (!sel) return std::nullopt;
    std::string t = sel->value.empty() ? sel->label
                  : sel->label.empty() ? sel->value
                  : sel->label + ", " + sel->value;
    if (withPosition) t += SelectionPosition(*sel);
    return t;
}

std::mutex& ColumnReadLock()
{
    static std::mutex m;
    return m;
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
                // Barter and the container screen: their Exit buttons
                // (BM_ButtonB id 14, CM_ExitButton id 11 - the menus' own
                // XML) are not called *back*/exit_button. Pressed as a mouse
                // click on them is, with the screen's reader held off.
                if ((m->typeID == kMenuType_Barter || m->typeID == kMenuType_Container) &&
                    TileVisible(cn->child)) {
                    Tile* btn = FindChildByName(cn->child,
                        m->typeID == kMenuType_Barter ? "BM_ButtonB" : "CM_ExitButton", 6);
                    if (btn && TileVisible(btn)) {
                        UInt32 id = (UInt32)TileNum(btn, kTileValue_id);
                        F3A_INFO("ClickMenuBack: exit '%s' (id=%u) in menu 0x%X",
                                 btn->name.m_data ? btn->name.m_data : "?", id, m->typeID);
                        std::lock_guard<std::mutex> lk(ColumnReadLock());
                        m->HandleClick(id, btn);
                        return true;
                    }
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
                    F3A_INFO("ClickMenuButton(menu 0x%X, '%s') id=%u vis=%.0f "
                             "menu=%p vtable=%p",
                             menuType, btnName, id,
                             TileNum(btn, kTileValue_visible), (void*)m,
                             *reinterpret_cast<void**>(m));
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


namespace {

// Rows in a list tile: its children minus the furniture every list carries.
int CountListRows(Tile* list, Tile** firstRow)
{
    if (firstRow) *firstRow = nullptr;
    if (!list) return 0;
    int n = 0;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&list->childList);
    for (int safety = 0; node && safety < 512; ++safety) {
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !cn->child) continue;
        const char* nm = fose_rt::TileName(cn->child);
        if (!nm || !*nm) continue;
        if (std::strcmp(nm, "lb_scrollbar") == 0 ||
            std::strcmp(nm, "lb_highlight_box") == 0) continue;
        // Rows the category filter has taken out stay in the list, hidden and
        // with list index -1; counted, an emptied column still read as full.
        if (TileHiddenExplicitly(cn->child) ||
            TileNum(cn->child, kTileValue_listindex) < 0.0f) continue;
        if (n == 0 && firstRow) *firstRow = cn->child;
        ++n;
    }
    return n;
}

// The two-column menus: the container screen and barter. FO3 builds both the
// same way (FOSE ListBox<T>, 0x30 each): your list, the other side's list, a
// pointer to the focused one, and a HandleSpecialKeyInput (vtable slot 14)
// that answers 13 / 14 by moving the focus to yours / theirs and 3 / 4 by
// clicking the focused column's category arrows. Disassembly of FO3 1.7.0.3:
// ContainerMenu 0x0063ADE0 (lists +0x84 / +0xB4, focus +0xE4), BarterMenu
// 0x006337C0 (lists +0x94 / +0xC4, focus +0xF4, its own menu-on-top check
// 0x41D). Their row tiles carry id 15 (container) and 18 (barter); their tile
// names are the same but for the CM_ / BM_ prefix.
struct ColumnMenu {
    UInt32      type, vtbl, special_key, mine, theirs, focus;
    float       row_id;
    const char* prefix;
};
const ColumnMenu kColumnMenus[] = {
    { kMenuType_Container, 0x00E04E54, 0x0063ADE0, 0x84, 0xB4, 0xE4, 15.0f, "CM_" },
    { kMenuType_Barter,    0x00E04A04, 0x006337C0, 0x94, 0xC4, 0xF4, 18.0f, "BM_" },
};

// The open two-column menu and which one it is - checked against the vtable
// and the slot that was disassembled, so another build is left alone.
UInt8* FindColumnMenu(const ColumnMenu** which, Tile** tile = nullptr)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return nullptr;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        if (!Readable(node, sizeof(Node))) return nullptr;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
        auto* tm = reinterpret_cast<TileMenu*>(cn->child);
        if (!Readable(tm, sizeof(TileMenu))) continue;
        Menu* m = tm->menu;
        if (!m || !Readable(m, 0xF8)) continue;
        for (const auto& d : kColumnMenus) {
            if (m->typeID != d.type) continue;
            auto* raw = reinterpret_cast<UInt8*>(m);
            UInt32 vt = *reinterpret_cast<UInt32*>(raw);
            if (vt != d.vtbl || !Readable(reinterpret_cast<void*>(vt), 15 * 4) ||
                reinterpret_cast<UInt32*>(vt)[14] != d.special_key)
                return nullptr;
            if (which) *which = &d;
            if (tile) *tile = cn->child;
            return raw;
        }
    }
    return nullptr;
}

Tile* ColumnTile(const char* suffix, int depth)
{
    const ColumnMenu* d = nullptr;
    Tile* menuTile = nullptr;
    if (!FindColumnMenu(&d, &menuTile) || !menuTile) return nullptr;
    std::string name = std::string(d->prefix) + suffix;
    return FindChildByName(menuTile, name.c_str(), depth);
}

std::string ColumnText(const char* suffix)
{
    Tile* t = ColumnTile(suffix, 4);
    const char* s = t ? TileStringTrait(t) : nullptr;
    return s ? GameStrToUtf8(s) : std::string();
}

} // namespace

bool GetContainerSides(ContainerSide* mine, ContainerSide* theirs)
{
    Tile* myList  = ColumnTile("Items_InventoryList", 4);
    Tile* thList  = ColumnTile("Container_InventoryList", 4);
    if (!myList && !thList) return false;
    if (mine) {
        mine->title = ColumnText("ItemsTitle");
        mine->rows  = CountListRows(myList, nullptr);
    }
    if (theirs) {
        theirs->title = ColumnText("ContainerTitle");
        theirs->rows  = CountListRows(thList, nullptr);
    }
    return true;
}

bool GetBarterState(BarterInfo* out)
{
    const ColumnMenu* d = nullptr;
    if (!out || !FindColumnMenu(&d) || d->type != kMenuType_Barter) return false;
    *out = BarterInfo{};
    out->vendor      = ColumnText("ContainerTitle");
    out->player_caps = ColumnText("Items_CapsLabel");
    out->vendor_caps = ColumnText("Container_CapsLabel");
    // The deal so far: BM_CapsFlow is shown while one stands, the amount is
    // its text, the arrow's picture says which way the caps go.
    Tile* flow = ColumnTile("CapsFlow", 3);
    if (flow && TileVisible(flow)) {
        out->amount = ColumnText("TransactionAmount");
        Tile* icon = ColumnTile("CapsFlowIcon", 4);
        const char* f = icon ? TileStrTrait(icon, kTileValue_filename) : nullptr;
        if (f && std::strstr(f, "sell")) out->direction = 1;
        else if (f && std::strstr(f, "buy")) out->direction = -1;
    }
    out->valid = true;
    return true;
}

namespace {
int FocusSideOf(const UInt8* menu, const ColumnMenu& d)
{
    const UInt8* cur = *reinterpret_cast<const UInt8* const*>(menu + d.focus);
    if (cur == menu + d.theirs) return 1;
    if (cur == menu + d.mine) return 0;
    return -1;
}
} // namespace

int ContainerFocusSide()
{
    const ColumnMenu* d = nullptr;
    const UInt8* m = FindColumnMenu(&d);
    return m ? FocusSideOf(m, *d) : -1;
}

Tile* SelectedRowOf(UInt8* list, float rowId);   // defined below

std::optional<MenuSelection> GetContainerSelection(int* out_side)
{
    if (out_side) *out_side = -1;
    const ColumnMenu* d = nullptr;
    UInt8* m = FindColumnMenu(&d);
    if (!m) return std::nullopt;
    // The focused column's ListBox, and ITS selected row: ListBox+0x10 is the
    // selected tile (the game's own column switch reads it there — 0x00632250
    // takes its list index, trait 0xFAC, from [list+0x10]); +0x0C is the
    // list's tile. Read from the menu, not guessed from where a highlight box
    // happens to sit — with two columns, two highlight boxes match two rows
    // equally well, and the wrong one kept winning.
    int side = FocusSideOf(m, *d);
    if (side < 0) return std::nullopt;
    UInt8* list = m + (side == 1 ? d->theirs : d->mine);
    Tile* row = SelectedRowOf(list, d->row_id);
    if (!row) return std::nullopt;
    Tile* box = *reinterpret_cast<Tile* const*>(list + 0x0C);
    MenuSelection sel;
    DescribeRowParts(row, sel.label, sel.value);
    if (sel.label.empty() && sel.value.empty()) return std::nullopt;
    sel.container = box;
    if (out_side) *out_side = side;
    return sel;
}

// The row a column has selected.
//
// The game marks it with mouseover=1 (the interface's active tile) — the
// in-game dump shows exactly one such row, the one the arrows put the
// selection on. ListBox+0x10 looked like the "selected" pointer and is read
// that way by the game's own column switch, but the list's select routine
// clears it again straight after selecting (0x00633278: vtable[0](0)), so
// after a keyboard step it is null and the row names went unread. Order of
// trust: the mouseover row, then +0x10, then the row whose list index is the
// list's saved index (+0x24). Always a visible row inside this list's tile.
Tile* SelectedRowOf(UInt8* list, float rowId)
{
    if (!Readable(list, 0x28)) return nullptr;
    Tile* box = *reinterpret_cast<Tile**>(list + 0x0C);
    if (!Readable(box, sizeof(Tile))) return nullptr;
    if (Tile* mo = FindMouseoverRow(box, 0)) return mo;

    Tile* row = *reinterpret_cast<Tile**>(list + 0x10);
    if (Readable(row, sizeof(Tile)) && !TileHiddenExplicitly(row)) {
        int depth = 0;
        for (Tile* p = row; p && depth < 12; p = p->parent, ++depth) {
            if (!Readable(p, sizeof(Tile))) break;
            if (p == box) return row;
        }
    }

    float saved = *reinterpret_cast<float*>(list + 0x24);
    if (!std::isfinite(saved) || saved < 0.0f) return nullptr;
    int want = (int)(saved + 0.5f);
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&box->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        if (!Readable(node, sizeof(Node))) break;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
        Tile* c = cn->child;
        if (TileNum(c, kTileValue_id) == rowId && !TileHiddenExplicitly(c) &&
            (int)TileNum(c, kTileValue_listindex) == want)
            return c;
    }
    return nullptr;
}

// Visible rows of a column (rows filtered out by the category have visible=0
// and list index -1).
int CountVisibleRows(Tile* box, float rowId)
{
    int n = 0;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&box->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        if (!Readable(node, sizeof(Node))) break;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
        Tile* c = cn->child;
        if (TileNum(c, kTileValue_id) == rowId && !TileHiddenExplicitly(c) &&
            TileNum(c, kTileValue_listindex) >= 0.0f)
            ++n;
    }
    return n;
}

int ContainerSelectedIndex(bool container_side)
{
    const ColumnMenu* d = nullptr;
    UInt8* m = FindColumnMenu(&d);
    if (!m) return -1;
    UInt8* list = m + (container_side ? d->theirs : d->mine);
    Tile* row = SelectedRowOf(list, d->row_id);
    return row ? (int)TileNum(row, kTileValue_listindex) : -1;
}

int StepContainerSelection(bool container_side, int delta)
{
    const ColumnMenu* d = nullptr;
    UInt8* m = FindColumnMenu(&d);
    if (!m) return -1;
    const int want = container_side ? 1 : 0;
    if (FocusSideOf(m, *d) != want) {
        // The game's own Up/Down roam YOUR column, and the moment they put the
        // mouseover on one of your rows the menu switches the focus back to
        // it. Take the focus back first, the game's own way.
        using SpecialKey = bool (__thiscall*)(void*, UInt32, float);
        reinterpret_cast<SpecialKey>(d->special_key)(m, container_side ? 14u : 13u, 1.0f);
        int now = FocusSideOf(m, *d);
        if (now < 0) return -1;
        if (now != want) return 0;
    }
    UInt8* list = m + (container_side ? d->theirs : d->mine);
    if (!Readable(list, 0x28)) return -1;
    Tile* box = *reinterpret_cast<Tile**>(list + 0x0C);
    if (!Readable(box, sizeof(Tile))) return -1;
    int n = CountVisibleRows(box, d->row_id);
    if (n <= 0) return 0;
    Tile* row = SelectedRowOf(list, d->row_id);
    int cur = row ? (int)TileNum(row, kTileValue_listindex) : -1;
    if (cur < 0 || cur >= n) {
        float saved = *reinterpret_cast<float*>(list + 0x24);
        cur = std::isfinite(saved) ? (int)saved : 0;
        if (cur < 0) cur = 0;
        if (cur >= n) cur = n - 1;
    }
    int nw = cur + delta;
    if (nw < 0) nw = 0;
    if (nw > n - 1) nw = n - 1;
    if (nw == cur && row) return 2;            // already at that end
    // Exactly what the game does when it hands the focus to a column: store
    // the row index the list should select (+0x24, a float) and run the
    // list's select routine (0x00633140), which picks that row, makes it the
    // interface's active tile and tells the menu — so the item panel, and the
    // game's own Enter, follow it.
    *reinterpret_cast<float*>(list + 0x24) = (float)nw;
    using SelectFn = void (__thiscall*)(void*, UInt32);
    reinterpret_cast<SelectFn>(0x00633140)(list, 1);
    F3A_INFO("%s: step %+d in the %s column: row %d -> %d of %d",
             d->type == kMenuType_Barter ? "Barter" : "Container",
             delta, container_side ? "other" : "player", cur, nw, n);
    return 1;
}

int SwitchContainerSide(bool container_side)
{
    const ColumnMenu* d = nullptr;
    UInt8* m = FindColumnMenu(&d);
    if (!m) return -1;
    using SpecialKey = bool (__thiscall*)(void*, UInt32, float);
    auto fn = reinterpret_cast<SpecialKey>(d->special_key);
    bool handled = fn(m, container_side ? 14u : 13u, 1.0f);
    int side = FocusSideOf(m, *d);
    F3A_INFO("%s: side key %u -> handled=%d, focus now %d (1=other 0=yours)",
             d->type == kMenuType_Barter ? "Barter" : "Container",
             container_side ? 14u : 13u, (int)handled, side);
    if (side < 0) return -1;                  // can't tell: say nothing confident
    return side == (container_side ? 1 : 0) ? 1 : 0;
}

// The category filter either side of a column's title.
//
// Both columns carry one: a dump of an open locker shows
// CM_Items_LeftFilterArrow (id 0) and CM_Items_RightFilterArrow (id 2) beside
// 'SPRZĘT', and CM_Container_LeftFilterArrow (id 5) / RightFilterArrow (id 7)
// beside the container's name (barter: BM_..., ids 2/4 and 7/9). They are
// ordinary buttons with ordinary ids, and the engine offered them to nothing
// but the mouse.
//
// Safe for HandleClick: an arrow neither closes the menu nor moves an item,
// the two things that crash this path.
bool ClickContainerFilter(bool container_side, bool next)
{
    const ColumnMenu* d = nullptr;
    if (!FindColumnMenu(&d)) return false;
    std::string nm = std::string(d->prefix) +
        (container_side ? (next ? "Container_RightFilterArrow" : "Container_LeftFilterArrow")
                        : (next ? "Items_RightFilterArrow" : "Items_LeftFilterArrow"));
    return ClickMenuButton(d->type, nm.c_str(), 4);
}

bool ClickVatsButton(const char* btnName)
{
    // e.g. "BodyPart_button" (cycle limb), "left_arrow"/"right_arrow" (switch
    // target). The tile reader then announces the new selection.
    return ClickMenuButton(kMenuType_VATS, btnName, 6);
}

bool ClickVatsBodyPart() { return ClickVatsButton("BodyPart_button"); }

// Queue a shot at the limb currently selected. This is the menu's own "Wybierz"
// button (id 7). The limb TILES are not buttons at all — a dump shows them with
// id 0 and no target trait, which is why clicking them queued nothing.
// "Zatwierdz" (accept_button, id 4) is a different thing entirely: it FIRES the
// queue and leaves VATS.
bool ClickVatsSelect() { return ClickVatsButton("Select_button"); }

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
void SetTileFloat(Tile* tile, UInt32 traitId, float value, int propagate = 0)
{
    if (!tile) return;
    const unsigned char* code = reinterpret_cast<const unsigned char*>(0x00BEE190);
    if (IsBadReadPtr((void*)code, 5) ||
        code[0] != 0x8B || code[1] != 0x44 || code[2] != 0x24 || code[3] != 0x04) {
        F3A_INFO("SetTileFloat: prologue guard rejected 0x00BEE190");
        return;
    }
    using PFN = void (__thiscall*)(Tile*, UInt32, float, int);
    reinterpret_cast<PFN>(0x00BEE190)(tile, traitId, value, propagate);
}

// THE MENU CURSOR, as the engine itself keeps it.
//
// InterfaceManager::cursor (+0x28) is a Tile, and a tile's position lives in
// its own x/y traits — the same traits we already read off the VATS body-part
// markers, so the two are in one coordinate space and directly comparable.
//
// This is what the VATS work has been missing. Fallout 3 picks the body part
// from the CURSOR (proven in play: the selection sits on whichever marker the
// cursor rests on, W and S do nothing, and the queue click lands on that same
// part). Until now the only way to move it was to shove the real mouse with
// SendInput, which moves the SYSTEM cursor — out of the window, onto whatever
// else is on screen, and into the mess the player reported. Moving the engine's
// own tile asks the game to put its cursor somewhere, and the operating system
// never hears about it.
//
// Writing goes through the engine's own trait setter, which re-evaluates
// whatever depends on the value, so the menu sees the move as a real one.
// Main thread only: it calls into the game.
bool SetMenuCursorPos(float x, float y)
{
    auto* ifm = rt::IFM();
    if (!ifm || IsBadReadPtr(ifm, 0x2C)) return false;
    Tile* cur = ifm->cursor;
    if (!cur || IsBadReadPtr(cur, 0x40)) return false;
    // Must be within, never "reject if beyond": NaN passes the second form.
    if (!(std::isfinite(x) && std::isfinite(y) &&
          std::fabs(x) <= 20000.0f && std::fabs(y) <= 20000.0f)) return false;
    // The traits are relative to the parent, and the values we are handed are
    // absolute, so take off whatever the parents contribute.
    float baseX = 0.0f, baseY = 0.0f;
    for (Tile* pt = cur->parent; pt; pt = pt->parent) {
        baseX += TileNum(pt, kTileValue_x);
        baseY += TileNum(pt, kTileValue_y);
    }
    SetTileFloat(cur, kTileValue_x, x - baseX);
    SetTileFloat(cur, kTileValue_y, y - baseY);
    return true;
}


// The game's cursor, as the engine itself moves it (FO3 1.7.0.3, the cursor
// update at 0x0061FA8D..0x0061FC66):
//   * each frame the new position = the cursor tile's NiNode local translate
//     (node = [cursorTile+0x2C], x @+0x58, z @+0x60) plus the mouse delta;
//   * if that differs from the stored position (IFM+0x2C x, +0x34 z) the
//     engine stores it, sets "cursor moved" (IFM+0xED) and recomputes the
//     screen point: IFM+0x38 = W/2 + x*W/Fw, IFM+0x40 = H/2 - z*H/Fh, where
//     W,H = the screen size (ints at 0x00F4CC4C / 0x00F4CC50) and
//     Fw = W/H*960 (or 1280 when not wider than tall), Fh = 960 (or H/W*1280).
// So the UI is 960 units tall, and a tile at (x, y) — origin top-left — sits at
// node (x - Fw/2, Fh/2 - y). Writing the node translate is a mouse move as far
// as the engine can tell.
bool MoveEngineCursorTo(float x, float y)
{
    if (!(std::isfinite(x) && std::isfinite(y))) return false;
    auto* ifm = rt::IFM();
    if (!Readable(ifm, 0xF0)) return false;
    UInt8* raw = reinterpret_cast<UInt8*>(ifm);
    Tile* cur = ifm->cursor;
    if (!Readable(cur, 0x30)) return false;
    UInt8* node = *reinterpret_cast<UInt8**>(reinterpret_cast<UInt8*>(cur) + 0x2C);
    if (!Readable(node, 0x64)) return false;
    const int* pw = reinterpret_cast<const int*>(0x00F4CC4C);
    const int* ph = reinterpret_cast<const int*>(0x00F4CC50);
    if (!Readable(pw, 4) || !Readable(ph, 4)) return false;
    int W = *pw, H = *ph;
    if (!(W >= 320 && W <= 16384 && H >= 200 && H <= 16384)) return false;
    float Fw = (H < W) ? (float)W / (float)H * 960.0f : 1280.0f;
    float Fh = (W < H) ? (float)H / (float)W * 1280.0f : 960.0f;

    // Self-check before touching anything: the engine's stored screen point
    // must be what this formula gives for its stored position. If another
    // build moved any of these, they disagree and we write nothing.
    float px = *reinterpret_cast<float*>(raw + 0x2C);
    float pz = *reinterpret_cast<float*>(raw + 0x34);
    float sx = *reinterpret_cast<float*>(raw + 0x38);
    float sy = *reinterpret_cast<float*>(raw + 0x40);
    float ex = W * 0.5f + px * (float)W / Fw;
    float ey = H * 0.5f - pz * (float)H / Fh;
    if (!(std::fabs(ex - sx) <= 2.0f && std::fabs(ey - sy) <= 2.0f)) {
        F3A_WARN("MoveEngineCursorTo: layout check failed (pos %.1f,%.1f screen "
                 "%.1f,%.1f expected %.1f,%.1f, %dx%d) — not moving",
                 px, pz, sx, sy, ex, ey, W, H);
        return false;
    }
    float nx = x - Fw * 0.5f, nz = Fh * 0.5f - y;
    if (!(std::fabs(nx) <= Fw * 0.5f && std::fabs(nz) <= Fh * 0.5f)) return false;
    *reinterpret_cast<float*>(node + 0x58) = nx;
    *reinterpret_cast<float*>(node + 0x60) = nz;
    return true;
}

bool ClickMapMarkerTile(const void* tile)
{
    Menu* menu = FindMenuByType(kMenuType_Map);
    Tile* menuTile = FindVisibleMenuTile(kMenuType_Map);
    auto* marker = reinterpret_cast<Tile*>(const_cast<void*>(tile));
    if (!menu || !menuTile || !marker || IsBadReadPtr(marker, 0x40)) return false;
    // The tile was collected earlier; the map may have been rebuilt since
    // (Pip-Boy closed and opened, world/local map switched) and the pointer
    // freed. Only click it if it is still part of the map that is open now.
    {
        bool inside = false;
        int depth = 0;
        for (Tile* t = marker; t && depth < 16; t = t->parent, ++depth) {
            if (IsBadReadPtr(t, 0x40)) break;
            if (t == menuTile) { inside = true; break; }
        }
        if (!inside) {
            F3A_INFO("ClickMapMarkerTile: marker tile no longer in the open map");
            return false;
        }
    }

    // FO3's MapMenu::HandleClick (0x00667730), case 0x16 - a world-map
    // marker - is what asks "Travel to <place>?". It goes ahead only when
    //   * the open tab is the world map ([menu+0x70] == 0x1D),
    //   * the click was not a drag: the map tile's x/y ([menu+0x74]) still
    //     equal where the click started ([menu+0x110]/[menu+0x114]),
    //   * the action button ([menu+0x54], MM_ButtonA) is a live target.
    // The FNV mod's id 0x1A lands on the switch's empty default in FO3 - the
    // marker was "selected", nothing happened - and the second step, a click on
    // MM_ButtonA (id 11), is no case at all here.
    auto* mb = reinterpret_cast<UInt8*>(menu);
    if (IsBadReadPtr(mb, 0x118) || *reinterpret_cast<int*>(mb + 0x70) != kMapTabWorld) {
        F3A_INFO("ClickMapMarkerTile: not on the world map tab");
        return false;
    }
    if (Tile* buttonA = *reinterpret_cast<Tile**>(mb + 0x54))
        if (!IsBadReadPtr(buttonA, 0x40)) SetTileFloat(buttonA, kTileValue_target, 1.0f);
    if (Tile* buttonA = FindChildByName(menuTile, "MM_ButtonA", 8))
        SetTileFloat(buttonA, kTileValue_target, 1.0f);
    if (Tile* mapTile = *reinterpret_cast<Tile**>(mb + 0x74)) {
        if (!IsBadReadPtr(mapTile, 0x40)) {
            *reinterpret_cast<float*>(mb + 0x110) = TileNum(mapTile, kTileValue_x);
            *reinterpret_cast<float*>(mb + 0x114) = TileNum(mapTile, kTileValue_y);
        }
    }
    constexpr UInt32 kTileID_WorldMarker = 0x16;
    F3A_INFO("ClickMapMarkerTile: %p vis=%d", marker, (int)TileVisible(marker));
    menu->HandleClick(kTileID_WorldMarker, marker);
    return true;
}

// Click the keyboard-highlighted row of a Data-page list, with the list's own
// selection ([menu+listOff]+0x10, the tile MapMenu::HandleClick reads it by)
// set to it first.
// The list's own item for the highlighted row. A Data-page ListBox keeps its
// rows as {Tile* tile, T* object} items in a tList at [list+4] and finds the
// clicked one by tile (0x00632D70 compares with [list+0x10]); the row found
// on screen is that tile or lies inside it.
Tile* DataListItemTile(UInt8* list, Tile* row, const void** object)
{
    struct Item { Tile* tile; void* object; };
    struct Node { Item* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(list + 4);
    for (int i = 0; node && i < 1024; ++i) {
        if (IsBadReadPtr(node, sizeof(Node))) break;
        Item* it = node->item;
        if (it && !IsBadReadPtr(it, sizeof(Item)) && it->tile) {
            int d = 0;
            for (Tile* p = row; p && d < 6; p = p->parent, ++d) {
                if (p == it->tile) {
                    if (object) *object = it->object;
                    return it->tile;
                }
            }
        }
        node = node->next;
    }
    return nullptr;
}

bool ClickDataListRow(const char* listName, UInt32 listOff, UInt32 clickId,
                      const void** object = nullptr)
{
    Tile* row = FindKeyboardSelectedTile(nullptr);
    if (!row) return false;
    bool inList = false;
    for (Tile* p = row; p && !inList; p = p->parent) inList = TileNameHas(p, listName);
    if (!inList) return false;
    Menu* menu = FindMenuByType(kMenuType_Map);
    auto* mb = reinterpret_cast<UInt8*>(menu);
    if (!mb || IsBadReadPtr(mb, listOff + 0x14)) return false;
    UInt8* list = mb + listOff;
    const void* obj = nullptr;
    Tile* item = DataListItemTile(list, row, &obj);
    F3A_INFO("ClickDataListRow: %s row %p -> item %p object %p, click 0x%X",
             listName, (void*)row, (void*)item, obj, clickId);
    if (!item) return false;
    *reinterpret_cast<Tile**>(list + 0x10) = item;
    if (object) *object = obj;
    menu->HandleClick(clickId, item);
    return true;
}

int TrackSelectedQuest()
{
    const void* quest = nullptr;
    if (!ClickDataListRow("MM_QuestsList", 0x120, 0x13, &quest) || !quest) return 0;
    auto* q = reinterpret_cast<const UInt8*>(quest);
    // The click's own test: a quest with the completed flag (+0x3C bit 1)
    // is not tracked.
    if (!IsBadReadPtr(q, 0x40) && (q[0x3C] & 0x02)) return -1;
    auto* p = reinterpret_cast<UInt8*>(rt::Player());
    const void* now = (p && !IsBadReadPtr(p + 0x614, 4))
                    ? *reinterpret_cast<void* const*>(p + 0x614) : nullptr;
    F3A_INFO("TrackSelectedQuest: quest %p, the player tracks %p", quest, now);
    return now == quest ? 1 : -2;
}
bool OpenSelectedNote() { return ClickDataListRow("MM_NotesList", 0x150, 0x14); }

namespace { Tile* FindChildExact(Tile* t, const char* name, int depth); }

std::string GetDataPageText()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Map);
    if (!m) return {};
    // By its whole name: "MM_DataText" is also the start of its parent's,
    // MM_DataTextRect, which comes first and holds no text - so not one note
    // was ever read.
    Tile* t = FindChildExact(m, "MM_DataText", 8);
    if (!t || !TileVisible(t)) return {};
    for (UInt32 i = 0; i < t->values.size; ++i) {          // faded out = not shown
        Tile::Value* v = t->values.data[i];
        if (v && v->id == kTileValue_alpha && v->num <= 0.0f) return {};
    }
    const char* s = TileStringTrait(t);
    return s && *s ? GameStrToUtf8(s) : std::string();
}

namespace {
// Book text is HTML (<p>, <br>, <font ...>): the tags go, a paragraph or a
// line break becomes a pause, entities become their letters.
std::string StripBookHtml(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '<') {
            size_t e = in.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = in.substr(i + 1, e - i - 1);
            for (auto& ch : tag) ch = (char)std::tolower((unsigned char)ch);
            if (tag.rfind("br", 0) == 0 || tag.rfind("p", 0) == 0 || tag.rfind("/p", 0) == 0 ||
                tag.rfind("div", 0) == 0 || tag.rfind("/div", 0) == 0)
                out += '\n';
            i = e;
            continue;
        }
        if (c == '&') {
            static const struct { const char* ent; char ch; } kEnt[] = {
                { "&nbsp;", ' ' }, { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
                { "&quot;", '"' }, { "&apos;", '\'' } };
            bool done = false;
            for (const auto& e : kEnt) {
                size_t n = std::strlen(e.ent);
                if (in.compare(i, n, e.ent) == 0) { out += e.ch; i += n - 1; done = true; break; }
            }
            if (done) continue;
        }
        out += c;
    }
    // Lines trimmed, runs of blank lines folded into one break.
    std::string clean;
    size_t pos = 0;
    bool blank = false;
    while (pos <= out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(pos, e - pos);
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        line = a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
        if (line.empty()) { blank = !clean.empty(); }
        else {
            if (!clean.empty()) clean += blank ? "\n\n" : "\n";
            clean += line;
            blank = false;
        }
        pos = e + 1;
    }
    return clean;
}
} // namespace

std::string GetBookText()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Book);
    if (!m) return {};
    const char* s = TileStrTrait(m, kTileValue_user2);
    if (!s || !*s) {
        // The page tiles copy the same text; the scroll's, for a paper.
        for (const char* nm : { "book_scroll_text", "book_page_1_text" }) {
            if (Tile* t = FindChildByName(m, nm, 6)) {
                s = TileStringTrait(t);
                if (s && *s) break;
            }
        }
    }
    if (!s || !*s) return {};
    return StripBookHtml(GameStrToUtf8(s));
}

std::string GetDataNoteKind()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Map);
    if (!m) return {};
    Tile* audio = FindChildByName(m, "MM_DataAudioInfo", 8);
    if (audio && TileVisible(audio)) {
        std::string s = strings::T("Nagranie", "Audio");
        if (Tile* left = FindChildByName(audio, "MM_AudioTimeRemaining", 3)) {
            // "--:--:-" before it plays: no time to tell.
            const char* t = TileStringTrait(left);
            if (t && std::strpbrk(t, "0123456789")) s += ", " + GameStrToUtf8(t);
        }
        return s;
    }
    Tile* image = FindChildByName(m, "MM_DataImage", 8);
    if (image && TileVisible(image)) return strings::T("Obraz", "Image");
    return {};
}

int MapMenuTab()
{
    Menu* menu = FindMenuByType(kMenuType_Map);
    auto* mb = reinterpret_cast<UInt8*>(menu);
    if (!mb || IsBadReadPtr(mb, 0x74)) return -1;
    int tab = *reinterpret_cast<int*>(mb + 0x70);
    return (tab >= kMapTabLocal && tab <= kMapTabRadio) ? tab : -1;
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

namespace {
// The visible Start menu (main menu or pause menu) and its "Restore Defaults"
// button — which only the SETTINGS pages have (gameplay, display, audio,
// controls); the pause list itself does not.
bool FindStartDefaults(Menu** outMenu, Tile** outBtn, UInt32* outId)
{
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
                    if (outMenu) *outMenu = m;
                    if (outBtn) *outBtn = btn;
                    if (outId) *outId = id;
                    return true;
                }
            }
        }
        node = node->next;
    }
    return false;
}
} // namespace

bool SettingsPageOpen()
{
    return FindStartDefaults(nullptr, nullptr, nullptr);
}

bool ClickRestoreDefaults()
{
    // Click the "Restore Defaults" button in the settings/controls page
    // (StartMenu). Like ClickMenuBack, we press it through the owning menu's
    // HandleClick (a game call → main thread only). The game then pops its own
    // Yes/No confirm, which our message-box reader already handles.
    Menu* m = nullptr;
    Tile* btn = nullptr;
    UInt32 id = 0;
    if (!FindStartDefaults(&m, &btn, &id)) return false;
    F3A_INFO("ClickRestoreDefaults: '%s' (id=%u)", btn->name.m_data ? btn->name.m_data : "?", id);
    m->HandleClick(id, btn);
    return true;
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
    case PipBoyTab::Stats: return strings::T("Statystyki", "Stats");
    case PipBoyTab::Items: return strings::T("Przedmioty", "Items");
    case PipBoyTab::Data:  return strings::T("Dane", "Data");
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

std::string GetItemCard()
{
    Tile* inv = FindVisibleMenuTile(kMenuType_Inventory);
    Tile* box = inv ? FindChildByName(inv, "IM_ItemInfoRect", 6) : nullptr;
    if (!box || !TileVisible(box)) return {};
    // The card's panels, top row then bottom row as on screen. Each is shown
    // (visible = 1) only for items it applies to: damage for weapons, damage
    // resistance for apparel, condition for both, effects for what has them.
    struct Panel { const char* name; const char* pl; const char* en; };
    static const Panel kPanels[] = {
        { "DPSInfo",          "obrażenia",              "damage" },
        { "DamageResistInfo", "odporność na obrażenia", "damage resistance" },
        { "WeightInfo",       "waga",                   "weight" },
        { "ValueInfo",        "wartość",                "value" },
        { "ConditionInfo",    "stan",                   "condition" },
        { "AmmoInfo",         "amunicja",               "ammo" },
        { "EffectsInfo",      "efekty",                 "effects" },
    };
    auto text = [](Tile* panel, const char* part) -> std::string {
        Tile* t = FindChildByName(panel, part, 0);
        const char* s = t ? TileStringTrait(t) : nullptr;
        std::string r = s ? GameStrToUtf8(s) : std::string();
        size_t b = r.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        size_t e = r.find_last_not_of(" \t\r\n");
        return r.substr(b, e - b + 1);
    };
    std::string out;
    for (const auto& p : kPanels) {
        Tile* panel = FindChildByName(box, p.name, 0);
        if (!panel || !TileVisible(panel)) continue;
        std::string value = text(panel, "CI_ValueText");
        std::string part;
        if (std::strcmp(p.name, "ConditionInfo") == 0) {
            // A bar with no number: the filled image's width out of the
            // background's.
            Tile* meter = FindChildByName(panel, "ConditionMeter", 0);
            Tile* img = meter ? FindChildByName(meter, "MeterImage", 0) : nullptr;
            Tile* bg  = meter ? FindChildByName(meter, "MeterBackground", 0) : nullptr;
            float full = bg ? TileNum(bg, kTileValue_width) : 0.0f;
            if (value.empty() && img && full > 0.0f) {
                int pct = static_cast<int>(TileNum(img, kTileValue_width) / full * 100.0f + 0.5f);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                char buf[32];
                std::snprintf(buf, sizeof(buf), strings::T("%d procent", "%d percent"), pct);
                value = buf;
            }
            if (!value.empty()) part = std::string(strings::T(p.pl, p.en)) + " " + value;
        } else if (std::strcmp(p.name, "AmmoInfo") == 0) {
            // Its title is the ammo's own name, the value how many you have.
            std::string ammo = text(panel, "CI_TitleText");
            if (!ammo.empty())
                part = std::string(strings::T(p.pl, p.en)) + ": " + ammo +
                       (value.empty() ? std::string() : ", " + value);
        } else if (std::strcmp(p.name, "EffectsInfo") == 0) {
            if (!value.empty()) part = std::string(strings::T(p.pl, p.en)) + ": " + value;
        } else if (!value.empty()) {
            part = std::string(strings::T(p.pl, p.en)) + " " + value;
        }
        if (part.empty()) continue;
        if (!out.empty()) out += ", ";
        out += part;
    }
    return out;
}

std::string GetSelectedItemInfo()
{
    // The Pip-Boy's own card first: only its shown panels, in words.
    if (std::string card = GetItemCard(); !card.empty()) return card;
    // On the Stats page, the description / the status view.
    if (std::string detail = GetStatsDetail(); !detail.empty()) return detail;
    // On a repair screen, the before / after comparison.
    if (std::string detail = GetRepairDetail(); !detail.empty()) return detail;
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
    // The caret is a DEL (0x7F) the field puts where the cursor is, there one
    // moment and gone the next - "b", "b", "b" on every blink while it was
    // kept. It and any other control character are not part of the name.
    r.erase(std::remove_if(r.begin(), r.end(), [](char c) {
                const unsigned char u = static_cast<unsigned char>(c);
                return u == 0x7F || u < 0x20;
            }), r.end());
    while (!r.empty() && (r.back() == '|' || r.back() == ' ')) r.pop_back();
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
// Text of a RaceSex tile, cleaned for speech: no trailing spaces, colon or the
// "more" arrow ("Modyfikuj >", "Domyślne:").
std::string RsmText(const char* raw)
{
    std::string t = raw ? GameStrToUtf8(raw) : std::string();
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == ':' ||
                          t.back() == '>'))
        t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    return t;
}

// A slider's value as the menu shows it: its RSM_slider_value text, or —
// where that is blank, as on the face-shape sliders — the number in user1
// (the FNV mod's order; user5 is not a string on FO3).
std::string RsmSliderValue(Tile* slider)
{
    if (Tile* v = FindChildByName(slider, "RSM_slider_value", 1)) {
        std::string t = RsmText(TileStringTrait(v));
        if (!t.empty()) return t;
    }
    float n = TileNum(slider, kTileValue_user1);
    if (!std::isfinite(n)) return {};
    char buf[32];
    if (n == (float)(int)n) std::snprintf(buf, sizeof(buf), "%d", (int)n);
    else {
        std::snprintf(buf, sizeof(buf), "%.1f", n);
        for (char* q = buf; *q; ++q) if (*q == '.') *q = ',';
    }
    return buf;
}
} // namespace

// Character creation (RaceSexMenu). The FNV mod's reading, on FO3's tiles (a
// menu dump of the face page settled the layout): every item of every page
// is a direct child of RSM_Background, and the ones of other pages are
// hidden. A page's header is the RSM_list_item that is NOT a target (so it
// cannot be focused) and whose user0 is the page number held in the menu
// root's user0. The item the keyboard is on carries mouseover — the FNV mod
// uses activeTileAlt, which FO3's interface manager does not have.
bool GetRaceSexState(RaceSexState* out)
{
    if (!out) return false;
    *out = RaceSexState{};
    Tile* root = FindVisibleMenuTile(kMenuType_RaceSex);
    if (!root) return false;
    Tile* bg = FindChildByName(root, "RSM_Background", 3);
    if (!bg) return false;
    out->page = (int)TileNum(root, kTileValue_user0);

    std::string anyHeader;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&bg->childList);
    for (int safety = 0; node && safety < 1024; ++safety) {
        if (!Readable(node, sizeof(Node))) break;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
        Tile* t = cn->child;
        if (!TileVisible(t)) continue;
        const bool item   = TileNameHas(t, "RSM_list_item");
        const bool slider = TileNameHas(t, "RSM_slider_option");
        const bool button = TileNameHas(t, "RSM_back_button") ||
                            TileNameHas(t, "RSM_next_button");
        if (item && TileNum(t, kTileValue_target) == 0.0f) {
            std::string h = RsmText(TileStringTrait(t));
            if ((int)TileNum(t, kTileValue_user0) == out->page && out->header.empty())
                out->header = h;
            else if (anyHeader.empty())
                anyHeader = h;
            continue;
        }
        if (out->kind != 0 || !(item || slider || button) || !TileMouseover(t)) continue;
        out->focus = t;
        out->kind  = slider ? 2 : item ? 1 : 3;
        out->label = RsmText(TileStringTrait(t));
        if (slider) out->value = RsmSliderValue(t);
        if (item)   out->selected = RsmItemCommitted(t);
    }
    if (out->header.empty()) out->header = anyHeader;
    return true;
}

namespace {
// A tile's text for speech: game codepage to UTF-8, line breaks as spaces,
// no leading or trailing blanks.
std::string LumText(Tile* t)
{
    const char* raw = t ? TileStringTrait(t) : nullptr;
    std::string s = raw ? GameStrToUtf8(raw) : std::string();
    for (char& ch : s) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}
} // namespace

// LevelUpMenu, from a dump of it (FO3 1.7.0.3, Polish build): LUM_Headline_Title,
// LUM_PointCounter and LUM_SelectionText carry the headline, the points left
// and the selected entry's description; LUM_SkillList holds one row per skill
// (ListItemText + LUM_Template_ItemValue) and LUM_PerkList one LUM_Perk per perk
// (ListItemText + LUM_Template_ItemMarker, shown on the picked one); only the
// page in use is visible. The row the keyboard is on carries mouseover — the
// FNV mod reads the same three texts and the selected row.
bool GetLevelUpState(LevelUpState* out)
{
    if (!out) return false;
    *out = LevelUpState{};
    Tile* root = FindVisibleMenuTile(kMenuType_LevelUp);
    if (!root) return false;
    out->headline    = LumText(FindChildByName(root, "LUM_Headline_Title", 6));
    out->counter     = LumText(FindChildByName(root, "LUM_PointCounter", 6));
    out->description = LumText(FindChildByName(root, "LUM_SelectionText", 6));
    Tile* skills = FindChildByName(root, "LUM_SkillList", 5);
    Tile* perks  = FindChildByName(root, "LUM_PerkList", 5);
    out->perks = perks && TileVisible(perks) && !(skills && TileVisible(skills));
    Tile* list = out->perks ? perks : skills;

    struct Node { Tile::ChildNode* item; Node* next; };
    if (list && TileVisible(list)) {
        auto* node = reinterpret_cast<Node*>(&list->childList);
        for (int safety = 0; node && safety < 1024; ++safety) {
            if (!Readable(node, sizeof(Node))) break;
            Tile::ChildNode* cn = node->item;
            node = node->next;
            if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
            Tile* row = cn->child;
            if (TileNameHas(row, "lb_highlight_box") || TileNameHas(row, "lb_scrollbar")) continue;
            if (!TileVisible(row) || !TileMouseover(row)) continue;
            Tile* text = FindChildByName(row, "ListItemText", 1);
            out->focus = row;
            out->label = LumText(text);
            if (out->label.empty()) out->label = LumText(row);
            out->value = LumText(FindChildByName(row, "LUM_Template_ItemValue", 1));
            Tile* marker = FindChildByName(row, "LUM_Template_ItemMarker", 1);
            out->chosen = marker && TileVisible(marker);
            out->dimmed = text && TileNum(text, kTileValue_alpha) < 250.0f;
            return true;
        }
    }
    // Not on a row: perhaps on one of the buttons (Wstecz, Od nowa, Kontynuuj).
    if (Tile* buttons = FindChildByName(root, "LUM_ButtonRect", 5)) {
        auto* node = reinterpret_cast<Node*>(&buttons->childList);
        for (int safety = 0; node && safety < 64; ++safety) {
            if (!Readable(node, sizeof(Node))) break;
            Tile::ChildNode* cn = node->item;
            node = node->next;
            if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
            Tile* b = cn->child;
            if (!TileVisible(b) || !TileMouseover(b)) continue;
            Tile* text = FindChildByName(b, "button_text", 1);
            out->focus  = b;
            out->button = true;
            out->label  = LumText(text);
            if (out->label.empty()) out->label = LumText(b);
            out->dimmed = text && TileNum(text, kTileValue_alpha) < 250.0f;
            break;
        }
    }
    return true;
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

// LockPickMenu (vtable 0x00E0658C) moves the pin in 0x0065AEF0, each frame of
// the picking state ([menu+0x28] == 2; 3 is the lock turning): it adds the
// mouse's x movement, scaled to UI units, to the "_x" trait of the pin tile
// [menu+0x68] and clamps it to [0, width of [menu+0x64]]. The trait id is
// registered by the menu's constructor into 0x01076AE0. Writing the trait the
// same way is the same as moving the mouse.
bool NudgeLockpick(float delta, bool* atEdge)
{
    if (atEdge) *atEdge = false;
    if (!std::isfinite(delta)) return false;
    Menu* menu = FindMenuByType(kMenuType_LockPick);
    auto* mb = reinterpret_cast<UInt8*>(menu);
    if (!mb || IsBadReadPtr(mb, 0x80)) return false;
    if (*reinterpret_cast<UInt32*>(mb + 0x28) != 2) return false;
    const auto* pTrait = reinterpret_cast<const UInt32*>(0x01076AE0);
    if (IsBadReadPtr(pTrait, 4)) return false;
    const UInt32 traitX = *pTrait;
    if (traitX < 10000 || traitX > 0xFFFF) return false;     // not a named trait
    Tile* pin = *reinterpret_cast<Tile**>(mb + 0x68);
    Tile* bar = *reinterpret_cast<Tile**>(mb + 0x64);
    if (!pin || !bar || IsBadReadPtr(pin, 0x40) || IsBadReadPtr(bar, 0x40)) return false;
    const float w = TileNum(bar, kTileValue_width);
    if (!(w > 0.0f)) return false;
    const float x = TileNum(pin, traitX);
    float nx = x + delta;
    if (nx <= 0.0f) { nx = 0.0f; if (atEdge) *atEdge = x > 0.0f; }
    if (nx >= w)    { nx = w;    if (atEdge) *atEdge = x < w; }
    if (nx != x) SetTileFloat(pin, traitX, nx, 1);
    return true;
}

namespace {
struct HackRow { unsigned long addr; std::string chars; Tile* row; int prefix; };
struct HackWord { std::string text; float tx; float ty; float cw; bool bracket = false; };

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
            HackWord w{ run, 0.0f, 0.0f, 0.0f };
            if (mid < loc.size()) {
                Loc& l = loc[mid];
                w.tx = TileAbsX(l.row) + l.ci * l.cw + l.cw * 0.5f;
                w.ty = TileAbsY(l.row) + TileNum(l.row, kTileValue_height) * 0.5f;
                w.cw = l.cw;
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

    // Bracket pairs: an opening ( [ { < and its own closing mate later on the
    // SAME line, nothing but symbols between them. The click goes on the
    // opening one - that is where the game highlights the pair.
    for (auto& r : rows) {
        int fullLen = r.prefix + (int)r.chars.size();
        float cw = fullLen > 0 ? TileNum(r.row, kTileValue_width) / fullLen : 0.0f;
        for (size_t i = 0; i < r.chars.size(); ++i) {
            const char o = r.chars[i];
            const char c = o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : o == '<' ? '>' : 0;
            if (!c) continue;
            for (size_t j = i + 1; j < r.chars.size(); ++j) {
                const char ch = r.chars[j];
                if (ch >= 'A' && ch <= 'Z') break;
                if (ch != c) continue;
                HackWord b{ r.chars.substr(i, j - i + 1), 0.0f, 0.0f, cw, true };
                b.tx = TileAbsX(r.row) + (r.prefix + (int)i) * cw + cw * 0.5f;
                b.ty = TileAbsY(r.row) + TileNum(r.row, kTileValue_height) * 0.5f;
                out.push_back(b);
                break;
            }
        }
    }
    // Words first, brackets after: the order the module's list and
    // GetHackWordTarget's numbering share.
    std::stable_partition(out.begin(), out.end(), [](const HackWord& w) { return !w.bracket; });
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

std::string GetHackingSelection();   // below, with the click

// Read the hacking (terminal password) minigame: candidate words + attempts.
HackingInfo GetHackingInfo()
{
    HackingInfo h;
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m) return h;
    std::vector<HackWord> words;
    BuildHackWords(m, words);
    for (auto& w : words) (w.bracket ? h.brackets : h.words).push_back(w.text);
    h.attempts = CountTilesNamed(m, 0, "hacking_guess_block");
    h.highlighted = HighlightedWordOf(m);
    if (h.highlighted.empty()) {
        std::string sel = GetHackingSelection();
        if (LooksLikeGridWord(sel.c_str())) h.highlighted = sel;
    }
    // Locked out: the grid is gone and the lock-out message is showing.
    Tile* lk = FindChildByName(m, "hacking_locked_message_text_1", 10);
    h.locked = lk && TileShown(lk) && TileStringTrait(lk) != nullptr;
    h.active = !h.words.empty() || h.locked;
    return h;
}

std::string GetHackingHighlightedWord()
{
    std::string w = HighlightedWordOf(FindVisibleMenuTile(kMenuType_Hacking));
    if (w.empty()) {
        std::string sel = GetHackingSelection();
        if (LooksLikeGridWord(sel.c_str())) w = sel;
    }
    return w;
}

// HackingMenu::HandleClick (vtable 0x00E05354 slot 3, 0x00643780), while the
// grid takes guesses (stage [menu+0x1C4] == 2), enters the text of the
// highlight object at menu+0xE0 - the word, bracket pair or symbol under the
// game's cursor, worked out each frame from the tile under it
// (InterfaceManager::activeTile) and the cursor's x. It does so only for a tile
// of the grid (parent == [menu+0x114], hacking_password_file_rect) and reads
// that tile's row for a bracket pair - so the click goes to the tile under the
// cursor, as the mouse's does.
std::string GetHackingSelection()
{
    Menu* menu = FindMenuByType(kMenuType_Hacking);
    auto* mb = reinterpret_cast<const UInt8*>(menu);
    if (!mb || IsBadReadPtr(mb, 0xE4)) return {};
    const char* s = *reinterpret_cast<const char* const*>(mb + 0xE0);
    std::string out;
    for (int i = 0; s && i < 48 && !IsBadReadPtr(s + i, 1) && s[i]; ++i) out += s[i];
    return out;
}

bool ClickHackingSelection()
{
    Menu* menu = FindMenuByType(kMenuType_Hacking);
    auto* mb = reinterpret_cast<UInt8*>(menu);
    if (!mb || IsBadReadPtr(mb, 0x1C8)) return false;
    const UInt32 stage = *reinterpret_cast<UInt32*>(mb + 0x1C4);
    Tile* grid = *reinterpret_cast<Tile**>(mb + 0x114);
    auto* ifm = rt::IFM();
    Tile* hit = ifm ? ifm->activeTile : nullptr;
    if (stage != 2 || !grid || !hit || IsBadReadPtr(hit, 0x40) || hit->parent != grid) {
        F3A_INFO("ClickHackingSelection: not on the grid (stage %u, tile %p, grid %p)",
                 stage, (void*)hit, (void*)grid);
        return false;
    }
    const UInt32 id = static_cast<UInt32>(TileNum(hit, kTileValue_id));
    F3A_INFO("ClickHackingSelection: '%s' (tile %p, id %u)",
             GetHackingSelection().c_str(), (void*)hit, id);
    menu->HandleClick(id, hit);
    return true;
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

// The level-up banner on the XP meter (HUDMainMenu → XPMeter → XPLevelUp,
// "NASTĘPNY POZIOM" in the Polish build). Hidden (visible=0) except while the
// game is showing a level-up. Empty when not showing.
std::string GetHudLevelUpText()
{
    Tile* hud = FindVisibleMenuTile(kMenuType_HUDMain);
    if (!hud) return {};
    Tile* meter = FindChildByName(hud, "XPMeter", 3);
    if (!meter) return {};
    Tile* lvl = FindChildByName(meter, "XPLevelUp", 2);
    if (!lvl || !TileVisible(lvl) || TileNum(lvl, kTileValue_alpha) <= 0.0f)
        return {};
    const char* s = TileStringTrait(lvl);
    return s && *s ? GameStrToUtf8(s) : std::string();
}

namespace {
// The HUD menu's tile whether or not it is the visible/active menu.
Tile* FindMenuTileAny(UInt32 menuType)
{
    auto* ifm = rt::IFM();
    if (!ifm || !ifm->menuRoot) return nullptr;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&ifm->menuRoot->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        if (!Readable(node, sizeof(Node))) return nullptr;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
        auto* tm = reinterpret_cast<TileMenu*>(cn->child);
        if (!Readable(tm, sizeof(TileMenu))) continue;
        Menu* m = tm->menu;
        if (m && Readable(m, 0x24) && m->typeID == menuType) return cn->child;
    }
    return nullptr;
}

// Text of a tile's direct children. `letters` = the children each hold one
// character of an animated headline. Those are joined in SCREEN order — by
// line, then left to right — not in the order of the child list: the list
// holds them back to front, and read in list order the quest "Kroki dziecka"
// came out as "AKCEIZD IKORK". A space goes wherever a blank tile, a new line
// or a gap wider than a letter separates them.
// A tile faded out: its alpha trait is there and 0 (the quest reminder's
// letters stay in the tree after a popup, visible flag and alpha at 0).
bool TileFadedOut(const Tile* t)
{
    if (!t) return false;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == kTileValue_alpha) return v->num <= 0.0f;
    }
    return false;
}

// The children's strings - only those on screen: an old popup's text stays in
// the tiles, hidden, and was read again after every menu ("Dodano zadanie..."
// four times for one quest).
std::string JoinChildStrings(Tile* parent, const char* sep, bool letters)
{
    std::string out;
    struct Glyph { float x, y, w, h; std::string s; int order; int line; };
    std::vector<Glyph> glyphs;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&parent->childList);
    for (int i = 0; node && i < 256; ++i) {
        if (!Readable(node, sizeof(Node))) break;
        Tile::ChildNode* cn = node->item;
        node = node->next;
        if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
        if (TileHiddenExplicitly(cn->child) || TileFadedOut(cn->child)) continue;
        const char* s = TileStringTrait(cn->child);
        if (!s || !*s) continue;
        std::string part = GameStrToUtf8(s);
        if (letters) {
            auto num = [&](UInt32 id) {
                float v = TileNum(cn->child, id);
                return std::isfinite(v) ? v : 0.0f;
            };
            glyphs.push_back({ num(kTileValue_x), num(kTileValue_y),
                               num(kTileValue_width), num(kTileValue_height),
                               part, (int)glyphs.size(), 0 });
        } else {
            if (!out.empty() && sep) out += sep;
            out += part;
        }
    }
    if (letters && !glyphs.empty()) {
        // Evidence for the next time: if the letters carry no position at all,
        // the order below is still the list's.
        static bool s_logged_flat = false;
        bool flat = true;
        for (const auto& g : glyphs) if (g.x != 0.0f || g.y != 0.0f) { flat = false; break; }
        if (flat && !s_logged_flat) {
            s_logged_flat = true;
            F3A_INFO("QuestAdded: %u letter tiles carry no position - list order kept",
                     (unsigned)glyphs.size());
        }
        // Lines first: a step down of more than half a letter starts a new one.
        std::stable_sort(glyphs.begin(), glyphs.end(),
                         [](const Glyph& a, const Glyph& b) { return a.y < b.y; });
        int line = 0;
        float lineY = glyphs[0].y;
        for (auto& g : glyphs) {
            float step = g.h > 16.0f ? g.h * 0.5f : 8.0f;
            if (g.y - lineY > step) { ++line; lineY = g.y; }
            g.line = line;
        }
        std::stable_sort(glyphs.begin(), glyphs.end(),
                         [](const Glyph& a, const Glyph& b) {
                             if (a.line != b.line) return a.line < b.line;
                             if (a.x != b.x) return a.x < b.x;
                             return a.order < b.order;
                         });
        float prevRight = -1.0f;
        int   prevLine  = -1;
        for (const auto& g : glyphs) {
            bool blank = g.s.find_first_not_of(" \t\r\n") == std::string::npos;
            bool gap = blank ||
                       (prevLine >= 0 && g.line != prevLine) ||
                       (prevRight >= 0.0f && g.x > prevRight + 4.0f);
            if (gap && !out.empty() && out.back() != ' ') out += ' ';
            if (!blank) out += g.s;
            prevRight = g.x + (g.w > 0.0f ? g.w : 0.0f);
            prevLine  = g.line;
        }
    }
    // Trim.
    size_t b = out.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = out.find_last_not_of(" \t\r\n");
    return out.substr(b, e - b + 1);
}
} // namespace

bool GetQuestReminder(std::string* headline, std::string* stages)
{
    if (headline) headline->clear();
    if (stages) stages->clear();
    Tile* hud = FindMenuTileAny(kMenuType_HUDMain);
    if (!hud) return false;
    Tile* rem = FindChildByName(hud, "QuestReminder", 3);
    if (!rem) return false;
    // Hidden between popups, but the old strings stay in the tiles — reading
    // them then would repeat the last popup after a load. Empty = nothing up.
    if (!TileVisible(rem)) return true;
    if (headline) {
        if (Tile* qa = FindChildByName(rem, "QuestAdded", 1)) {
            // One letter per child while animating — decide by the children:
            // mostly single characters means letters.
            int kids = 0, singles = 0;
            struct Node { Tile::ChildNode* item; Node* next; };
            auto* node = reinterpret_cast<Node*>(&qa->childList);
            for (int i = 0; node && i < 256; ++i) {
                if (!Readable(node, sizeof(Node))) break;
                Tile::ChildNode* cn = node->item;
                node = node->next;
                if (!cn || !Readable(cn, 0x08) || !cn->child) continue;
                const char* s = TileStringTrait(cn->child);
                if (!s || !*s) continue;
                ++kids;
                if (std::strlen(s) == 1) ++singles;
            }
            bool letters = kids > 0 && singles * 2 >= kids;
            *headline = JoinChildStrings(qa, " ", letters);
        }
    }
    if (stages) {     // one objective per line
        if (Tile* qs = FindChildByName(rem, "QuestStages", 1))
            *stages = JoinChildStrings(qs, "\n", false);
    }
    return true;
}

// Everything the HUD is currently SAYING, as separate lines.
//
// The game puts tutorials, hints, "you cannot fast travel while overencumbered",
// perk and level notices and similar prose straight onto the HUD, and a sighted
// player just reads them. There is no single tile for that — it comes and goes
// across the HUD tree — so collect every visible string and let the caller
// announce the ones that are NEW.
//
// The filter is the important part: the HUD is also full of text that changes
// constantly (health, action points, ammo counts, the compass, the clock). Those
// are excluded by tile name, otherwise the mod would talk over the game forever.
// Tiles we already read through their own pollers (Messages, Info) are skipped
// too, so nothing is announced twice.
namespace {
bool HudTileIsNoise(Tile* t)
{
    static const char* kNoise[] = {
        "health", "Health", "hp", "ap_", "Ap", "ammo", "Ammo", "compass",
        "Compass", "clock", "Clock", "meter", "Meter", "bar", "Bar",
        "Messages", "Info", "crippled", "Crippled", "xp", "XP", "rad", "Rad",
        "sneak", "Sneak", "reticle", "Reticle", "enemyhealth", "EnemyHealth",
        // The bottom panels: health with the compass, and action points with
        // the ammo counter and weapon condition. Their labels ("AMUNICJA",
        // "KND") were read at every load, and the ammo count ("11/24") after
        // every shot and reload. Ammo and AP are numbers to ask for, not news.
        "HitPoints", "ActionPoints",
        // Quest popups and objectives: read by their own reader, which also
        // works under other menus (poll::PollQuestReminder).
        "QuestReminder",
        // Loot Menu Updated's overlay. It lives on the HUD and is full of
        // prose — item names, "Take", the carry weight — which this collector
        // happily read out at every loading screen. GetLootMenuInfo reads it
        // properly, so here it is noise.
        "JLM", "lm_box", "lm_button_text", "lm_PCShortcutLabel",
    };
    for (Tile* p = t; p; p = p->parent)
        for (const char* n : kNoise)
            if (TileNameHas(p, n)) return true;
    return false;
}

void CollectHudProse(Tile* t, int depth, std::vector<std::string>& out)
{
    if (!t || depth > 10 || out.size() > 40) return;
    if (TileVisible(t) && !HudTileIsNoise(t)) {
        if (const char* s = TileStringTrait(t)) {
            std::string line = GameStrToUtf8(s);
            size_t b = line.find_first_not_of(" \t\n\r");
            if (b != std::string::npos) {
                size_t e = line.find_last_not_of(" \t\n\r");
                line = line.substr(b, e - b + 1);
                // Prose only: skip bare numbers, single characters and the
                // untranslated placeholders the game leaves in unused tiles.
                bool digits_only = !line.empty();
                for (char c : line) if (c < '0' || c > '9') { digits_only = false; break; }
                if (line.size() > 2 && !digits_only && line != "Button Text")
                    out.push_back(line);
            }
        }
    }
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int i = 0; node && i < 4096; ++i) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) CollectHudProse(cn->child, depth + 1, out);
        node = node->next;
    }
}
} // namespace

std::vector<std::string> GetHudProse()
{
    std::vector<std::string> out;
    Tile* hud = FindVisibleMenuTile(kMenuType_HUDMain);
    if (hud) CollectHudProse(hud, 0, out);
    return out;
}

// ---- Loot Menu Updated (F3LootMenu.dll) ------------------------------------
//
// The overlay keeps everything we need in tile traits it registers itself:
// _JLMVisible, _JLMTitle, _JLMIndex, _JLMOffset, _JLMTotal, _JLMWeight,
// _JLMStealing, and per-entry _JLMEquipped. Those names only become numeric
// trait ids through the engine's own table, so resolve them once and cache.
namespace {

// 0x00BEA9E0 = TraitNameToID on 1.7 (the FOSE source's own address ladder).
// Rather than trust that, ASK IT SOMETHING WE ALREADY KNOW: a built-in trait
// whose id is fixed. If "visible" doesn't come back as kTileValue_visible, this
// is not the function we think it is and we stay out of it entirely.
UInt32 TraitId(const char* name)
{
    using PFN = UInt32 (__cdecl*)(const char*);
    static int  state = 0;          // 0 = untested, 1 = usable, -1 = rejected
    if (state == 0) {
        const void* fn = reinterpret_cast<const void*>(0x00BEA9E0);
        static const unsigned char kSig[] = { 0x51,0x8B,0x4C,0x24,0x08,0x8D,0x04,0x24 };
        if (IsBadReadPtr(fn, sizeof(kSig))) {
            state = -1;
            F3A_INFO("TraitId: 0x00BEA9E0 unreadable — loot menu support off");
        } else if (std::memcmp(fn, kSig, sizeof(kSig)) != 0) {
            state = -1;
            F3A_INFO("TraitId: 0x00BEA9E0 is not TraitNameToID on this build — loot menu support off");
        } else if (reinterpret_cast<PFN>(0x00BEA9E0)("visible") != kTileValue_visible) {
            state = -1;
            F3A_INFO("TraitId: self-test failed — loot menu support off");
        } else {
            state = 1;
        }
    }
    if (state < 0) return 0;
    return reinterpret_cast<PFN>(0x00BEA9E0)(name);
}

struct JlmTraits {
    UInt32 visible = 0, title = 0, index = 0, offset = 0,
           total = 0, items = 0, weight = 0, stealing = 0, equipped = 0;
    bool ready = false;
};
JlmTraits g_jlm;

bool InitJlmTraits()
{
    if (g_jlm.ready) return g_jlm.visible != 0;
    g_jlm.ready    = true;
    g_jlm.visible  = TraitId("_JLMVisible");
    if (!g_jlm.visible) return false;
    g_jlm.title    = TraitId("_JLMTitle");
    g_jlm.index    = TraitId("_JLMIndex");
    g_jlm.offset   = TraitId("_JLMOffset");
    g_jlm.total    = TraitId("_JLMTotal");
    g_jlm.items    = TraitId("_JLMItems");
    g_jlm.weight   = TraitId("_JLMWeight");
    g_jlm.stealing = TraitId("_JLMStealing");
    g_jlm.equipped = TraitId("_JLMEquipped");
    F3A_INFO("Loot menu traits: visible=%u title=%u index=%u total=%u",
             g_jlm.visible, g_jlm.title, g_jlm.index, g_jlm.total);
    return true;
}

// Exact name match. FindChildByName matches on a SUBSTRING, which would hand
// back "ItemMarker" when asked for "Item1".
Tile* FindChildExact(Tile* t, const char* name, int depth)
{
    if (!t || depth < 0) return nullptr;
    struct Node { Tile::ChildNode* item; Node* next; };
    auto* node = reinterpret_cast<Node*>(&t->childList);
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            const char* n = cn->child->name.m_data;
            if (n && std::strcmp(n, name) == 0) return cn->child;
            if (Tile* r = FindChildExact(cn->child, name, depth - 1)) return r;
        }
        node = node->next;
    }
    return nullptr;
}

void CopyTrimmed(char* dst, size_t cap, const char* src)
{
    dst[0] = '\0';
    if (!src || !*src) return;
    std::string v = GameStrToUtf8(src);
    size_t b = v.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return;
    size_t e = v.find_last_not_of(" \t\n\r");
    v = v.substr(b, e - b + 1);
    std::snprintf(dst, cap, "%s", v.c_str());
}

} // namespace

bool GetLootMenuInfo(LootMenuInfo* out)
{
    if (!out) return false;
    *out = LootMenuInfo{};
    if (!InitJlmTraits()) return false;

    Tile* hud = FindVisibleMenuTile(kMenuType_HUDMain);
    if (!hud) return false;
    Tile* jlm = FindChildExact(hud, "JLM", 6);
    if (!jlm) return false;

    if (TileNum(jlm, g_jlm.visible) == 0.0f) return false;
    CopyTrimmed(out->title, sizeof(out->title), TileStrTrait(jlm, g_jlm.title));
    // Visible with no container name means the overlay is holding stale state
    // and has nothing to show — same check the New Vegas mod makes.
    if (!out->title[0]) return false;

    out->visible  = true;
    out->index    = (int)TileNum(jlm, g_jlm.index);
    out->offset   = (int)TileNum(jlm, g_jlm.offset);
    out->total    = (int)TileNum(jlm, g_jlm.total);
    out->stealing = TileNum(jlm, g_jlm.stealing) != 0.0f;
    CopyTrimmed(out->weight, sizeof(out->weight), TileStrTrait(jlm, g_jlm.weight));

    // The highlighted row is the tile named after the in-window index; the
    // absolute position is that plus the scroll offset.
    if (out->index >= 0) {
        char want[16];
        std::snprintf(want, sizeof(want), "Item%d", out->index);
        if (Tile* it = FindChildExact(jlm, want, 4)) {
            const char* s = nullptr;
            if (Tile* txt = FindChildExact(it, "lm_button_text", 3))
                s = TileStrTrait(txt, kTileValue_string);
            if (!s) s = TileStrTrait(it, kTileValue_string);
            CopyTrimmed(out->item, sizeof(out->item), s);
            out->equipped = TileNum(it, g_jlm.equipped) != 0.0f;
        }
    }
    return true;
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
bool GetHackWordTarget(int idx, float* x, float* y, float* charWidth)
{
    Tile* m = FindVisibleMenuTile(kMenuType_Hacking);
    if (!m || idx < 0) return false;
    std::vector<HackWord> words;
    BuildHackWords(m, words);
    if (idx >= (int)words.size()) return false;
    if (x) *x = words[idx].tx;
    if (y) *y = words[idx].ty;
    if (charWidth) *charWidth = words[idx].cw;
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

    // The TARGET only. This used to append the limb as well, by taking the
    // first limb_name in the tree — but VATS draws one such tile per body part
    // and the first is simply whichever comes first in the tree, not the one
    // being aimed at. The selected limb comes from GetVatsPick, which reads the
    // game's own marker for it.
    std::string enemy;
    if (Tile* eh = FindChildByName(vats, "EnemyHealth", 6))
        if (const char* s = TileLabelDeep(eh, 0)) enemy = clean(GameStrToUtf8(s));
    if (enemy.empty()) return std::nullopt;
    return enemy;
}

// ---- Dialog / Barter / Lockpick / VATS — TODO ----------------------------

namespace {
// A tile's text as UTF-8, without the whitespace around it; "" for none.
std::string TrimmedText(Tile* t)
{
    const char* s = t ? TileStringTrait(t) : nullptr;
    std::string r = s ? GameStrToUtf8(s) : std::string();
    size_t b = r.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = r.find_last_not_of(" \t\r\n");
    return r.substr(b, e - b + 1);
}

std::string DialogTileText(const char* name)
{
    Tile* m = FindVisibleMenuTile(kMenuType_Dialog);
    return TrimmedText(m ? FindChildExact(m, name, 4) : nullptr);
}
} // namespace

std::string GetCurrentDialogSpeaker() { return DialogTileText("DM_SpeakerNameLabel"); }
std::string GetCurrentDialogLine()    { return DialogTileText("DM_SpeakerText"); }

LockpickState GetLockpickState() { return {}; }
std::vector<VatsTarget> GetVatsTargets() { return {}; }
int GetVatsSelectedIndex() { return -1; }
// FO3's stats_menu has the same tiles as New Vegas's (checked against a dump):
// the menu tile's user0 is the tab (0 Status, 1 S.P.E.C.I.A.L., 2 Skills,
// 3 Perks, 4 General), stats_status_container's user0 the Status view (0 CND,
// 1 RAD, 2 EFF).
std::string GetStatsDetail()
{
    Tile* m = FindVisibleMenuTile(kMenuType_Stats);
    if (!m) return {};
    auto text = [](Tile* t) { return TrimmedText(t); };
    const int tab = static_cast<int>(TileNum(m, kTileValue_user0));
    if (tab != 0) {
        Tile* d = FindChildExact(m, "stats_description", 6);
        return d && TileVisible(d) ? text(d) : std::string();
    }
    Tile* sc = FindChildExact(m, "stats_status_container", 3);
    if (!sc) return {};
    const int view = static_cast<int>(TileNum(sc, kTileValue_user0));
    std::string out;
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!out.empty()) out += ", ";
        out += part;
    };
    if (view == 0) {
        // Each limb: a meter (the filled image's width out of its background's)
        // and a CRIPPLED label shown when it is.
        static const struct { const char* tile; const char* pl; const char* en; } kLimbs[] = {
            { "head",     "głowa",      "head" },
            { "torso",    "tułów",      "torso" },
            { "leftarm",  "lewa ręka",  "left arm" },
            { "rightarm", "prawa ręka", "right arm" },
            { "leftleg",  "lewa noga",  "left leg" },
            { "rightleg", "prawa noga", "right leg" },
        };
        Tile* body = FindChildExact(sc, "stats_player_head", 1);
        if (!body) return {};
        for (const auto& l : kLimbs) {
            const std::string base = std::string("stats_player_") + l.tile;
            Tile* crip = FindChildExact(body, (base + "_crippled").c_str(), 2);
            Tile* meter = FindChildExact(body, (base + "_pct_meter").c_str(), 3);
            Tile* img = meter ? FindChildExact(meter, "MeterImage", 0) : nullptr;
            Tile* bg  = meter ? FindChildExact(meter, "MeterBackground", 0) : nullptr;
            const float full = bg ? TileNum(bg, kTileValue_width) : 0.0f;
            std::string part = strings::T(l.pl, l.en);
            if (crip && TileVisible(crip)) {
                part += strings::T(" okaleczona", " crippled");
            } else if (img && full > 0.0f) {
                int pct = static_cast<int>(TileNum(img, kTileValue_width) / full * 100.0f + 0.5f);
                part += " " + std::to_string(pct < 0 ? 0 : pct > 100 ? 100 : pct) + "%";
            } else {
                continue;
            }
            add(part);
        }
    } else if (view == 1) {
        Tile* rc = FindChildExact(sc, "stats_radiation_container", 1);
        std::string rads = rc ? text(FindChildExact(rc, "stats_player_rads", 2)) : std::string();
        if (rads.empty()) rads = std::to_string(static_cast<int>(GetPlayerAV(54) + 0.5f));
        add(strings::T("promieniowanie ", "rads ") + rads);
        std::string resist = rc ? text(FindChildExact(rc, "stats_rad_resist_level", 2)) : std::string();
        if (!resist.empty()) add(strings::T("odporność na promieniowanie ", "rad resistance ") + resist);
    } else if (view == 2) {
        Tile* none = FindChildExact(sc, "stats_no_effects", 1);
        if (none && TileVisible(none)) return strings::T("Brak efektów.", "No effects.");
        Tile* list = FindChildExact(sc, "stats_effects_container", 1);
        struct Node { Tile::ChildNode* item; Node* next; };
        auto* node = list ? reinterpret_cast<Node*>(&list->childList) : nullptr;
        for (int i = 0; node && i < 256; ++i) {
            // Read off the poll thread while the game may rebuild the list:
            // every node checked, as the other tile walks here do.
            if (!Readable(node, sizeof(Node))) break;
            Tile::ChildNode* cn = node->item;
            node = node->next;
            Tile* row = (cn && Readable(cn, 0x08)) ? cn->child : nullptr;
            if (!row || !TileNameHas(row, "stats_effects_template_item") || !TileVisible(row) ||
                TileNum(row, kTileValue_listindex) < 0.0f)
                continue;
            std::string nm = text(FindChildExact(row, "stats_effect_name", 0));
            std::string fx = text(FindChildExact(row, "stats_effect_string", 0));
            if (nm.empty() && fx.empty()) continue;
            if (!out.empty()) out += ". ";
            out += nm.empty() ? fx : fx.empty() ? nm : nm + ": " + fx;
        }
    }
    return out;
}

int GetVatsQueueLength()   { return 0; }
int GetVatsQueueCapacity() { return 0; }
std::optional<std::string> PollNewSubtitle() { return std::nullopt; }

bool GetSleepWaitInfo(SleepWaitInfo* out)
{
    Tile* m = FindVisibleMenuTile(kMenuType_SleepWait);
    if (!m || !out) return false;
    auto text = [m](const char* name) -> std::string {
        Tile* t = FindChildByName(m, name, 5);
        const char* s = t ? TileStringTrait(t) : nullptr;
        return s ? GameStrToUtf8(s) : std::string();
    };
    out->prompt = text("SWM_HowManyText");
    out->hours  = text("SWM_HoursChosen");
    out->now    = text("SWM_CurrentTime");
    return !(out->prompt.empty() && out->hours.empty() && out->now.empty());
}

namespace {
// The open repair screen and its tile prefix.
Tile* RepairMenuTile(const char** prefix)
{
    if (Tile* m = FindVisibleMenuTile(kMenuType_RepairServices)) { *prefix = "RSM_"; return m; }
    if (Tile* m = FindVisibleMenuTile(kMenuType_Repair))         { *prefix = "RM_";  return m; }
    return nullptr;
}

// A card_info panel shown on screen: "TITLE value"; a condition card has a
// meter instead of a value, read as a percentage of its width.
std::string CardText(Tile* card)
{
    if (!card || !TileVisible(card)) return {};
    std::string title = TrimmedText(FindChildByName(card, "CI_TitleText", 0));
    std::string value = TrimmedText(FindChildByName(card, "CI_ValueText", 0));
    if (value.empty()) {
        Tile* meter = FindChildByName(card, "Meter", 0);
        Tile* img = meter ? FindChildByName(meter, "MeterImage", 0) : nullptr;
        Tile* bg  = meter ? FindChildByName(meter, "MeterBackground", 0) : nullptr;
        const float full = bg ? TileNum(bg, kTileValue_width) : 0.0f;
        if (img && full > 0.0f) {
            int pct = static_cast<int>(TileNum(img, kTileValue_width) / full * 100.0f + 0.5f);
            value = std::to_string(pct < 0 ? 0 : pct > 100 ? 100 : pct) + "%";
        }
    }
    if (title.empty() && value.empty()) return {};
    return title.empty() ? value : value.empty() ? title : title + " " + value;
}
} // namespace

std::string GetRepairDetail()
{
    const char* pre = nullptr;
    Tile* m = RepairMenuTile(&pre);
    if (!m) return {};
    auto tile = [&](const char* suffix) {
        return FindChildByName(m, (std::string(pre) + suffix).c_str(), 6);
    };
    auto shown = [&](const char* suffix) {
        Tile* t = tile(suffix);
        return t && TileVisible(t) ? TrimmedText(t) : std::string();
    };
    std::string out;
    auto add = [&](const std::string& part, const char* sep = ", ") {
        if (part.empty()) return;
        if (!out.empty()) out += sep;
        out += part;
    };
    std::string now, after;
    for (const char* card : { "BrokenItemHealth", "BrokenItemStat" }) {
        std::string c = CardText(tile(card));
        if (!c.empty()) now += (now.empty() ? "" : ", ") + c;
    }
    for (const char* card : { "FixedItemHealth", "FixedItemStat" }) {
        std::string c = CardText(tile(card));
        if (!c.empty()) after += (after.empty() ? "" : ", ") + c;
    }
    if (!now.empty())   add(strings::T("teraz: ", "now: ") + now);
    if (!after.empty()) add(strings::T("po naprawie: ", "after repair: ") + after, "; ");
    std::string gain = shown("HealthImprovementText");
    std::string sgain = shown("StatImprovementText");
    if (!gain.empty() || !sgain.empty())
        add(strings::T("zysk ", "gain ") + gain + (gain.empty() || sgain.empty() ? "" : ", ") + sgain);
    add(shown("RepairCostText"), ". ");
    add(shown("ErrorText"), ". ");
    add(shown("ChooseItemText"), ". ");
    return out;
}

std::string GetRepairHeader()
{
    const char* pre = nullptr;
    Tile* m = RepairMenuTile(&pre);
    if (!m) return {};
    std::string out;
    if (std::strcmp(pre, "RM_") == 0) {
        out = CardText(FindChildByName(m, "RM_Headline_PlayerSkillInfo", 6));
    } else {
        out = TrimmedText(FindChildByName(m, "RSM_VendorSkillLabel", 6));
        std::string caps = TrimmedText(FindChildByName(m, "RSM_CapsLabel", 6));
        if (!caps.empty()) out += (out.empty() ? "" : ", ") + caps;
    }
    return out;
}

// inventory_menu.xml: IM_HotKeyWheel (HotKeys.xml prefab) is shown while the
// player assigns or picks a hotkey; HK_Item_0..7 each have an HK_SelectionBox
// visible on the selected one, IM_HotKeyText copies the wheel's _SelectedText.
bool GetHotkeyWheel(int* slot, std::string* text)
{
    Tile* inv = FindVisibleMenuTile(kMenuType_Inventory);
    Tile* wheel = inv ? FindChildByName(inv, "IM_HotKeyWheel", 4) : nullptr;
    if (!wheel || !TileVisible(wheel)) return false;
    int sel = -1;
    for (int i = 0; i < 8 && sel < 0; ++i) {
        char nm[16];
        std::snprintf(nm, sizeof(nm), "HK_Item_%d", i);
        Tile* item = FindChildExact(wheel, nm, 0);
        Tile* box = item ? FindChildByName(item, "HK_SelectionBox", 0) : nullptr;
        if (box && TileVisible(box)) sel = i;
    }
    if (slot) *slot = sel;
    if (text) *text = TrimmedText(FindChildByName(wheel, "IM_HotKeyText", 0));
    return true;
}

// ---- The console (~) -------------------------------------------------------

namespace {
struct ConsoleAddrs {
    UInt8** mgr    = nullptr;   // &ConsoleManager*
    UInt32  shown  = 0;         // int: how many lines the view shows
    void**  text   = nullptr;   // &DebugText* - the engine's on-screen text lines
    UInt32  lookup = 0;         // DebugText: the text drawn at (x, y) in a font
};
ConsoleAddrs   g_con;
std::once_flag g_con_once;

// The bytes at `at` match `sig` (-1 = any byte).
template <size_t N>
bool CodeIs(UInt32 at, const int (&sig)[N])
{
    auto* p = reinterpret_cast<const UInt8*>(at);
    if (!at || IsBadReadPtr(p, N)) return false;
    for (size_t i = 0; i < N; ++i)
        if (sig[i] >= 0 && p[i] != static_cast<UInt8>(sig[i])) return false;
    return true;
}
UInt32 CallTarget(UInt32 at) { return at + 5 + *reinterpret_cast<const SInt32*>(at + 1); }

// Everything is found from FOSE's GetSingleton address and checked byte by
// byte, so a wrong edition reads nothing instead of crashing.
void FindConsole()
{
    static const int kGet[] = { 0xA1, -1, -1, -1, -1, 0x85, 0xC0, 0x75 };   // mov eax, [g]; test; jne
    const UInt32 get = rt::g_addrs->consoleSingleton;
    if (!CodeIs(get, kGet)) {
        F3A_WARN("Console: no GetSingleton at %08X; the console is not read.", get);
        return;
    }
    const UInt32 global = *reinterpret_cast<const UInt32*>(get + 1);
    g_con.mgr = reinterpret_cast<UInt8**>(global);
    g_con.shown = global + 0x14;     // the static beside it the scrolling clamps by
    // The key handler comes right after GetSingleton. Where it fetches the line
    // being typed: fild [edi+0x34] ... push 1; call DebugText::GetSingleton;
    // add esp, 4; mov ecx, eax; call DebugText::Lookup(out, x, y, font).
    static const int kKeyHandler[] = { 0x6A, 0xFF, 0x68, -1, -1, -1, -1, 0x64, 0xA1, 0, 0, 0, 0,
                                       0x50, 0xB8, 0x68, 0x21, 0, 0 };
    static const int kFetch[] = { 0xDB, 0x47, 0x34, 0x56, 0x83, 0xEC, 0x08, 0x8D, 0x8C, 0x24, -1, -1, -1, -1,
                                  0xD9, 0x5C, 0x24, 0x04, 0xDB, 0x47, 0x30, 0xD9, 0x1C, 0x24, 0x51, 0x56,
                                  0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x04, 0x8B, 0xC8, 0xE8 };
    static const int kTextGet[] = { 0xA1, -1, -1, -1, -1, 0x85, 0xC0, 0x75 };
    static const int kLookup[]  = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C, 0x56, 0x8B, 0xD9 };
    const UInt32 keys = get + 0x50;
    if (CodeIs(keys, kKeyHandler)) {
        for (UInt32 at = keys; at < keys + 0x200; ++at) {
            if (!CodeIs(at, kFetch)) continue;
            const UInt32 single = CallTarget(at + 26);
            const UInt32 look   = CallTarget(at + 36);
            if (CodeIs(single + 0x25, kTextGet) && CodeIs(look, kLookup)) {
                g_con.text   = *reinterpret_cast<void***>(single + 0x26);
                g_con.lookup = look;
            }
            break;
        }
    }
    F3A_INFO("Console: manager at %08X, typed line %s (%08X)", global,
             g_con.lookup ? "readable" : "NOT readable", g_con.lookup);
}

UInt8* ConsoleMgr()
{
    std::call_once(g_con_once, FindConsole);
    if (!g_con.mgr || IsBadReadPtr(g_con.mgr, 4)) return nullptr;
    UInt8* m = *g_con.mgr;
    if (!m || IsBadReadPtr(m, 0x40)) return nullptr;
    return m;
}

std::string ConsoleNodeText(const UInt8* node)
{
    const char* s = *reinterpret_cast<const char* const*>(node + 0x08);
    // One probe up to the terminator (or 0x800), not one per character: the
    // dump snapshot reads 20 lines this way every half second on the game's
    // thread.
    if (!s || IsBadStringPtrA(s, 0x800)) return {};
    return GameStrToUtf8(std::string(s, strnlen(s, 0x800)).c_str());
}

const char kEmptyConsole = 0;   // a cursor for "the list was empty"
} // namespace

bool ConsoleOpen()
{
    UInt8* m = ConsoleMgr();
    return m && *reinterpret_cast<const signed char*>(m + 0x38) > 0;
}

bool GetConsoleInput(std::string* text, int* caret)
{
    if (!OnMainThread() || !ConsoleOpen() || !g_con.lookup || !g_con.text ||
        IsBadReadPtr(g_con.text, 4) || !*g_con.text)
        return false;
    UInt8* m = ConsoleMgr();
    static char buf[0x800];
    buf[0] = 0;
    typedef void(__thiscall* LookupFn)(void*, char*, float, float, int);
    reinterpret_cast<LookupFn>(g_con.lookup)(*g_con.text, buf,
        static_cast<float>(*reinterpret_cast<const int*>(m + 0x30)),
        static_cast<float>(*reinterpret_cast<const int*>(m + 0x34)), 1);
    buf[sizeof(buf) - 1] = 0;
    std::string t(buf);
    size_t bar = t.find('|');   // the engine's own caret, the first '|'
    if (bar == std::string::npos) bar = t.size();
    else t.erase(bar, 1);
    *text  = t;
    *caret = static_cast<int>(bar);
    return true;
}

std::vector<std::string> ConsoleNewLines(const void** cursor)
{
    std::vector<std::string> out;
    if (!OnMainThread()) return out;
    UInt8* m = ConsoleMgr();
    if (!m) { *cursor = nullptr; return out; }
    UInt8* last = *reinterpret_cast<UInt8**>(m + 0x08);
    const void* now = last ? static_cast<const void*>(last) : &kEmptyConsole;
    if (!*cursor) { *cursor = now; return out; }
    if (*cursor == now) return out;
    const void* was = *cursor;
    // Back from the newest to the one seen last time.
    std::vector<const UInt8*> fresh;
    bool found = false;
    for (const UInt8* n = last; n && fresh.size() < 400; ) {
        if (n == was) { found = true; break; }
        if (IsBadReadPtr(n, 0x10)) break;
        fresh.push_back(n);
        n = *reinterpret_cast<const UInt8* const*>(n + 0x04);
    }
    *cursor = now;
    // That one gone (the console cleared, or more lines than it keeps): the
    // newest few only.
    // (The list was empty last time: every line is new, none is cut.)
    if (!found && was != &kEmptyConsole && fresh.size() > 50) fresh.resize(50);
    for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
        std::string t = ConsoleNodeText(*it);
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

bool ConsoleView(int* count, int* bottom, int* shown)
{
    UInt8* m = ConsoleMgr();
    if (!m) return false;
    *count  = *reinterpret_cast<const int*>(m + 0x0C);
    *bottom = *reinterpret_cast<const int*>(m + 0x24);
    int v = 0;
    if (g_con.shown && !IsBadReadPtr(reinterpret_cast<const void*>(g_con.shown), 4))
        v = *reinterpret_cast<const int*>(g_con.shown);
    *shown = (v >= 1 && v <= 200) ? v : 10;
    return true;
}

std::vector<std::string> ConsoleLines(int from, int to)
{
    std::vector<std::string> out;
    UInt8* m = ConsoleMgr();
    if (!m || to <= from) return out;
    int i = 0;
    for (const UInt8* n = *reinterpret_cast<UInt8**>(m + 0x04); n && i < to && i < 2000; ++i) {
        if (IsBadReadPtr(n, 0x10)) break;
        if (i >= from) out.push_back(ConsoleNodeText(n));
        n = *reinterpret_cast<const UInt8* const*>(n);
    }
    return out;
}

std::string GameTextToUtf8(const std::string& raw) { return GameStrToUtf8(raw.c_str()); }

// ---- A game setting as loaded ----------------------------------------------

namespace {
// A setting the exe defines is a static {vtable, value, name}; the game puts
// the loaded value (Fallout3.esm, then any plugin over it) in it - the XP
// sum reads iXPBumpBase only from there, and the game's 550 for level 3 is
// Fallout3.esm's 150, not the exe's 100. Found by its name: the name's text,
// then the dword pointing at it. Remembered once found.
const UInt32* FindExeSetting(const char* name)
{
    static std::mutex mx;
    static std::unordered_map<std::string, const UInt32*> found;
    {
        std::lock_guard<std::mutex> lk(mx);
        auto it = found.find(name);
        if (it != found.end()) return it->second;
    }
    const UInt32* result = nullptr;
    auto* base = reinterpret_cast<const UInt8*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (base && !IsBadReadPtr(base, 0x400) && dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (!IsBadReadPtr(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE) {
            const UInt32 lo = reinterpret_cast<UInt32>(base);
            const UInt32 hi = lo + nt->OptionalHeader.SizeOfImage;
            const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            const int nsec = nt->FileHeader.NumberOfSections;
            std::string pat(1, '\0');
            pat += name;
            pat += '\0';
            UInt32 text = 0;
            for (int i = 0; i < nsec && !text; ++i) {
                if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ)) continue;
                const char* p = reinterpret_cast<const char*>(base + sec[i].VirtualAddress);
                const size_t n = sec[i].Misc.VirtualSize;
                if (n < pat.size() || IsBadReadPtr(p, n)) continue;
                const char* hit = std::search(p, p + n, pat.begin(), pat.end());
                if (hit != p + n) text = reinterpret_cast<UInt32>(hit + 1);
            }
            for (int i = 0; text && i < nsec && !result; ++i) {
                if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                auto* w = reinterpret_cast<const UInt32*>(base + sec[i].VirtualAddress);
                const size_t n = sec[i].Misc.VirtualSize / 4;
                if (IsBadReadPtr(w, n * 4)) continue;
                for (size_t k = 2; k < n; ++k) {
                    if (w[k] != text) continue;
                    if (w[k - 2] < lo || w[k - 2] >= hi) continue;   // the vtable is the exe's
                    result = w + k - 2;
                    break;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(mx);
    found[name] = result;
    return result;
}
} // namespace

std::string ExeGameSettingString(const char* name)
{
    const UInt32* gs = FindExeSetting(name);
    if (!gs) return {};
    const char* val = reinterpret_cast<const char*>(gs[1]);
    if (!val || IsBadReadPtr(val, 1)) return {};
    size_t len = 0;
    while (len < 64 && !IsBadReadPtr(val + len, 1) && val[len]) ++len;
    return std::string(val, len);
}

int ExeGameSettingInt(const char* name, int fallback)
{
    const UInt32* gs = FindExeSetting(name);
    return gs ? static_cast<int>(gs[1]) : fallback;
}

int GetPlayerLevel()
{
    auto* p = rt::Player();
    if (!p || !Readable(p, 0x20)) return 0;
    auto* base = reinterpret_cast<const UInt8*>(p->baseForm);
    if (!base || !Readable(base + 0x30, 0x10)) return 0;
    const int level = *reinterpret_cast<const UInt16*>(base + 0x3C);
    return (level >= 1 && level <= 255) ? level : 0;
}

int XPForLevel(int level)
{
    if (level <= 1) return 0;
    const int first = ExeGameSettingInt("iXPBase", 200);
    const int bump  = ExeGameSettingInt("iXPBumpBase", 150);
    // As the game adds it up: each step takes `bump` more than the last.
    int total = first, step = first;
    for (int n = level - 1; n > 1; --n) {
        step += bump;
        total += step;
    }
    return total;
}

} // namespace f3a::game
