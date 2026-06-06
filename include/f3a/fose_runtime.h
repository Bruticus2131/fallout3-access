#pragma once

// Direct access to Fallout 3 GOTY 1.7.0.3 (FALLOUT_VERSION_1_7) game pointers
// and arrays — the addresses are reproduced from FOSE's GameObjects.cpp and
// GameInterface.cpp so we don't have to compile those translation units.
//
// We only ever READ from these locations and only access public struct fields
// in the FOSE types. We never call FOSE member functions that resolve through
// DEFINE_MEMBER_FN (those would require linking the FOSE static lib).

#include "fose/GameObjects.h"
#include "fose/GameInterface.h"
#include "fose/GameTiles.h"
#include "fose/GameMenus.h"

#include <unordered_set>

namespace f3a::fose_rt {

// ---- Per-runtime address tables --------------------------------------------
//
// Fallout 3's globals live at hardcoded addresses that differ between the two
// final builds: the standard 1.7.0.3 (FALLOUT_VERSION_1_7 = 0x01070030) and
// the "no gore" edition (FALLOUT_VERSION_1_7ng = 0x01070031). Rather than bake
// one set at compile time, we keep BOTH and pick the right table at load from
// fose->runtimeVersion (see SelectRuntime). All values reproduced from FOSE's
// GameObjects.cpp / GameInterface.cpp / GameSettings.cpp.
//
// Older patches (1.0–1.6) are intentionally unsupported: the plugin's
// FOSEPlugin_Query rejects any runtime that isn't one of these two, and modern
// installs (Steam/GOG) all ship 1.7.0.3 anyway.
struct RuntimeAddrs {
    UInt32 thePlayer;        // PlayerCharacter**
    UInt32 interfaceManager; // InterfaceManager**
    UInt32 tileMenuArray;    // NiTArray<TileMenu*>*
    UInt32 menuVisibility;   // bool[0x3C]
    UInt32 iniPrefColl;      // IniSettingCollection** (FalloutPrefs.ini)
    UInt32 iniSettingColl;   // IniSettingCollection** (Fallout.ini)
    UInt32 gameSettingColl;  // GameSettingCollection**
    UInt32 dataHandler;      // DataHandler** (has questList)
    UInt32 refrActivate;     // TESObjectREFR::Activate (__thiscall) — code addr
};

inline constexpr RuntimeAddrs kAddrs_1_7 = {   // 0x01070030 standard
    0x0107A104, 0x01075B24, 0x0106A7BC, 0x011793DB,
    0x01179578, 0x0116D6F4, 0x010701A8, 0x0106CDCC,
    0x004EE000,
};
inline constexpr RuntimeAddrs kAddrs_1_7ng = { // 0x01070031 no-gore
    0x01077104, 0x01072B24, 0x010677BC, 0x011763DB,
    0x01176578, 0x0116A6F4, 0x0106D1A8, 0x01069DCC,
    0x004EE000,   // best guess; a prologue-bytes check guards the call
};

// Active table; defaults to standard 1.7.0.3 until SelectRuntime() runs.
inline const RuntimeAddrs* g_addrs = &kAddrs_1_7;

// Called once at plugin load with fose->runtimeVersion to pick the table.
inline void SelectRuntime(UInt32 runtimeVersion)
{
    g_addrs = (runtimeVersion == 0x01070031) ? &kAddrs_1_7ng : &kAddrs_1_7;
}

inline PlayerCharacter* Player()
{
    auto pp = reinterpret_cast<PlayerCharacter**>(g_addrs->thePlayer);
    return pp ? *pp : nullptr;
}

inline InterfaceManager* IFM()
{
    auto pp = reinterpret_cast<InterfaceManager**>(g_addrs->interfaceManager);
    return pp ? *pp : nullptr;
}

// Legacy path — keeping it as a fallback but the main path is the menuRoot
// walker below. The bool array reads as garbage on the GOTY runtime we
// checked, with many high-numbered slots flagged true at boot.
inline bool IsMenuVisibleByArray(UInt32 menuType)
{
    if (menuType >= kMenuType_Message) menuType -= kMenuType_Message;
    if (menuType >= 0x3C) return false;
    auto arr = reinterpret_cast<bool*>(g_addrs->menuVisibility);
    return arr[menuType];
}

// Read a numeric trait off a tile (0 if absent).
inline float TileTraitNum(const Tile* t, UInt32 id)
{
    if (!t) return 0.0f;
    for (UInt32 i = 0; i < t->values.size; ++i) {
        Tile::Value* v = t->values.data[i];
        if (v && v->id == id) return v->num;
    }
    return 0.0f;
}

inline bool TileIsVisible(const Tile* t)
{
    return TileTraitNum(t, kTileValue_visible) != 0.0f;
}

// Walk InterfaceManager.menuRoot.childList and collect the typeID of every
// TileMenu currently attached to the UI tree. The childList layout is
//   childList: tList<Tile::ChildNode>
//     m_listHead: { item: Tile::ChildNode*, next: _Node* }
//   ChildNode    : { next, prev, child: Tile* }
// — first sentinel item points to the first real ChildNode; we walk the
// linked-list nodes by raw pointer to avoid pulling in tList's template
// machinery.
//
// `only_visible`: in GAMEPLAY the engine keeps every menu permanently
// mounted in menuRoot (Repair, Inventory, Stats... live there from the
// moment a game loads), so mere presence no longer means "open". Open menus
// carry visible=1 on their TileMenu root; mounted-but-closed ones don't.
// At the MAIN MENU phase visibility flags are unreliable (the menu UI hangs
// under a vis=0 MapMenu), so callers pass only_visible=false there.
inline void CollectActiveMenuTypes(std::unordered_set<UInt32>& out_types,
                                   bool only_visible = false)
{
    out_types.clear();
    InterfaceManager* ifm = IFM();
    if (!ifm || !ifm->menuRoot) return;

    struct ListNode { Tile::ChildNode* item; ListNode* next; };
    auto* node = reinterpret_cast<ListNode*>(&ifm->menuRoot->childList);

    // Walk via tList node->next (FOSE's Iterator semantics): only terminate
    // when the list node itself is null, not when an item slot is null.
    // Breaking on null item misses every sibling past the first hole.
    for (int safety = 0; node && safety < 4096; ++safety) {
        Tile::ChildNode* cn = node->item;
        if (cn && cn->child) {
            if (!only_visible || TileIsVisible(cn->child)) {
                auto* tm = reinterpret_cast<TileMenu*>(cn->child);
                Menu* m = tm->menu;
                if (m && m->typeID) out_types.insert(m->typeID);
            }
        }
        node = node->next;
    }
}

// Convenience: is this single menu type currently in the active set?
inline bool IsMenuVisible(UInt32 menuType)
{
    std::unordered_set<UInt32> active;
    CollectActiveMenuTypes(active);
    return active.count(menuType) > 0;
}

// All menu type IDs we route into the dispatcher. Order matches priority.
inline constexpr UInt32 kAllMenus[] = {
    kMenuType_Dialog, kMenuType_VATS, kMenuType_Loading,
    kMenuType_Message, kMenuType_Inventory, kMenuType_Stats, kMenuType_Container,
    kMenuType_LockPick, kMenuType_Map, kMenuType_Quantity, kMenuType_SleepWait,
    kMenuType_Barter, kMenuType_Repair, kMenuType_RepairServices,
    kMenuType_Hacking, kMenuType_Computers, kMenuType_LevelUp,
    kMenuType_Book, kMenuType_Start, kMenuType_RaceSex, kMenuType_TextEdit,
    kMenuType_Tutorial, kMenuType_SPECIALBook,
};

inline const char* TileName(const Tile* t)
{
    if (!t) return "";
    return t->name.m_data ? t->name.m_data : "";
}

} // namespace f3a::fose_rt
