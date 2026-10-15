# FO3 1.7.0.3 engine-pathfinding RE (port of the FNV AutoWalk pieces)

Read-only static analysis of Fallout3.exe (ImageBase 0x400000) with capstone. Scratch tools: `relib.py`, `funcs_region.py`, `find_stride.py`, `scan_mover_vcall.py` in this folder. The FO3 code is structurally a 1:1 match of the FNV code. The ActorMover layout, its vtable order and the handler and solution sizes (0x88/0xA0/0xE4/0x20/0x44) all match.

## 1. ActorMover::Update = 0x007E0D40 (`__thiscall(ActorMover*, float dt)`, `ret 4`)
- ActorMover vtable 0xE1E764, slot 5 (+0x14) = 0x7E0D40. PlayerMover vtable 0xE1EE04, slot 5 (+0x14) = 0x7E8950. The slot index is the same; PlayerMover overrides it.
- How the vtables were matched:
  - Slot 3 0x7DDB30 = SetMovementFlags: `mov byte[ecx+71h],1; mov [ecx+38h],eax`.
  - Slot 4 0x7DDB40 = ClearMovementFlags, shared by both vtables.
  - Slot 8: ActorMover 0x7DDAD0 vs PlayerMover 0x7E8DC0 (`mov eax,[ecx+94h]`).
  - Slot 9 0x7DDAF0 = GetAndClearOverrideMovement (+0x77 / +0x10..18), shared by both vtables.
  - This matches the FNV slot table exactly.
- The body of 0x7E0D40 matches FNV 0x9DC120:
  ```
  7E0D6B cmp [esi+72h],bl / mov [esi+78h],1 / call 7DF780(1)        ; bNeedsClear -> reset
  7E0D83 actor->vfunc[22Ch](0) -> handler->vfunc[70h] (Stop), exit
  7E0DCA ebp=[process+28h]; call 766D80; cmp ebp,eax                ; process-level gate
  7E0E46 cmp [esi+74h],bl                                           ; async result pending?
  7E0E8C call [[esi+30h]+10h](&msg)                                 ; pop ActorPathingMessageQueue
  7E0F4A call 8E9600 (mover+20h = msg.solution) ; 7E0F59 call 7E0AE0 ; CreatePathHandler
  7E0FA4 call [[esi+24h]+10h](dt) ; cmp eax,5 -> [esi+6Ch]=6        ; handler tick
  7E0FBB call 7DDEE0                                                ; PostUpdate / UpdateMovementDirection
  ```
- PostUpdate (UpdateMovementDirection) is 0x7DDEE0, the equivalent of FNV 0x9DDC00.
  - It branches on +0x71. Branch A uses actor vfunc 0x1E4 and +0x38.
  - Branch B uses the handler's vfunc +0x48 and +0x4C.
  - It writes +0x10/14/18, +0x3C and +0x77.
- PlayerMover::Update (0x7E8950) calls nothing in the mover or pathing region. The player's mover never runs the path state machine by itself, the same as in FNV.

## 2. SetPathfindingGoal / SubmitPathRequest
- **Actor-level SetPathfindingGoal = 0x006F8AB0**, `__thiscall(Actor*, TESObjectREFR* target, float radius, void* avoidNodes)`, returns bool in `al`, `ret 0Ch`. This is the FNV 0x8B36F0 equivalent.
  ```
  6F8AB0 mov eax,[esp+0Ch]; test eax,eax; jne                       ; avoid==NULL ->
  6F8ABB mov ecx,[esi+60h]; call [[ecx]+250h](actor)                ; process default avoid nodes
  6F8AD3 mov ecx,[esi+184h]; push eax; push radius; push target; call 7E0140
  ```
  - It is non-virtual (no data refs). It has 4 callers in AI code, for example 0x74B2FE: `push 0; push radius; push [esi+40h]; mov ecx,edi; call 6F8AB0; test al,al`.
  - It does **not** null-check actor+0x184 (the mover), or actor+0x60 (the process) when `avoid` is NULL.
- ActorMover-level SetPathfindingGoal = 0x7E0140, `__thiscall(mover, target, radius, avoid)`, `ret 0Ch`.
  - It takes `cell=[target+3Ch]`, `ws=0x4E8880(target)` and `pos=target->vfunc[1F4h]()`.
  - It then calls 0x7E0080.
- Variant for a raw position, so no ghost REFR is needed:
  - Actor thunk 0x6F8A70 = `__thiscall(Actor*, NiPoint3* pos, TESObjectCELL* cell, TESWorldSpace* ws, float radius, void* avoid)`, `ret 14h`.
  - It calls mover 0x7E0080 with the same arguments (`ret 14h`). That function builds a stack PathingRequest (ctor 0x600EB0, vtable 0xE024B8) and calls `0x7DDC90(actor, &req, pos, cell, ws, radius, avoid)`.
  - 0x7DDC90 is cdecl. It stores the goal at req+0x34 and the radius at req+0x74; a radius ≤ 0 uses the default from [0xF69410] = 25.0. The avoid nodes go to req+0x94.
  - Finally 0x7E0080 calls `0x7DFB70(&req)`.
- **SubmitPathRequest = 0x007DFB70**, `__thiscall(ActorMover*, PathingRequest*)`, returns bool, `ret 4`. This is FNV 0x9DBB10 with 0x9DBA50 and 0x9DB090 inlined into it.
  ```
  7DFBAF mov [esi+6Ch],1 ; call 7DF4D0(0)          ; state=submitted, clear old req/sol (not handler)
  7DFD41 call 7DF780(1) ; mov al,1 -> return        ; "trivially close": returns TRUE, state 6, NO request
  7DFD8F jmp [eax*4+7E0048]                          ; type switch 0..8, new PathingRequest subclass
  7DFEBB lea ecx,[esi+1Ch]; call 8E9650              ; mover+0x1C = request
  7DFF5C new 0x1C; call 7434E0 -> [esi+30h]          ; ActorPathingMessageQueue if none
  7DFFF8 call 44F4B0; call 607E70                    ; PathingTaskManager::SubmitTask
  7E0010 fail: call 7DF400; [esi+6Ch]=7 ; ok: 7E0025 mov [esi+74h],1 ; return 1
  ```
- PathingTaskManager:
  - The singleton accessor 0x44F4B0 returns 0x106DB84.
  - SubmitTask = 0x607E70, `__thiscall(mgr, queueSP, requestSP, bool allowAsync)`, `ret 0Ch`.
  - It runs async when `[0x1074A84]` is set, which is `bBackgroundPathing:Pathfinding` (setting object 0x1074A80, default 1), and allowAsync is true.
  - Otherwise it runs synchronously: `new 0x44` PathingSolution, `0x6062F0`, pathfinder `0x5FB8C0(req, sol)`, then it posts a message to the queue (status 0 = ok, 1 = fail/partial).
  - SubmitPathRequest passes `allowAsync = !(player+0x5B4 > 0)` (via 0x76C8E0).

## 3. Field offsets (identical to FNV)
ActorMover (ctor 0x7DF2A0, 0x88 bytes). PlayerMover ctor 0x7E88F0: allocated 0xA0 at 0x76DBA6 and stored at player+0x184 at 0x76DBD1.

| Offset | Field |
|---|---|
| +0x10/14/18 | move direction (output) |
| **+0x1C** | **PathingRequest\*** (refcounted, set at 0x7DFEC6) |
| **+0x20** | **PathingSolution\*** (refcounted, set at 0x7E0EC4/0x7E0F4A) |
| **+0x24** | **path handler\*** (raw; set at 0x7E0C2B/0x7E0CCA, deleted in 0x7DF400) |
| +0x28 | Actor\* |
| +0x30 | ActorPathingMessageQueue\* |
| +0x34 | move flags (init 0x101) |
| +0x38 | override flags |
| +0x3C | output flags |
| +0x40/+0x42 | start/end waypoint index (0xFFFF) |
| +0x44 | PathingLocation (0x28 bytes) |
| +0x6C | pathingState: 1 = submitted, 3 = has path, 6 = idle/done, 7 = failed |
| +0x70 | bIsPathing |
| +0x71 | override-dir |
| +0x72 | deferred clear |
| +0x74 | **request in flight** |
| +0x75 | repath |
| +0x76 | disable handler tick (checked only in 0x7E0D10) |
| +0x77 | has-direction |
| +0x78 | stop |

- **CreatePathHandler = 0x7E0AE0**, `__thiscall(mover, float dt)`, `ret 4`. Its **only call site is 0x7E0F59** (`E8 82 FB FF FF`): FO3 merged FNV's two call sites into one.
  - Waypoint count > 0 and `GetProcessLevel(0x7650B0)==0` gives a DetailedActorPathHandler: `new 0xE4`, ctor 0x7E5400, vtable 0xE1EA8C.
  - Otherwise it builds a straight-line handler: `new 0x20`, ctor 0x7E9180, vtable 0xE1EE54.
  - It deletes the old handler after `new->vfunc[58h](old)`.
- **Handler layout** (base ctor 0x7E0FE0, vtable 0xE1E7EC):
  ```
  mov [esi],0E1E7ECh; mov [esi+4],actor; mov [esi+8],mover; mov [esi+0Ch],sol (AddRef); [esi+10h]=request (AddRef)
  ```
  So the **solution is at handler+0x0C** and the request at +0x10.
  - Detailed handler: +0x60 movement flags (slot +0x48 = 0x7E55B0); direction at +0x1C/20/24 with valid flag +0xE2 (slot +0x4C = 0x7E11D0); state at +0xC8.
  - Stop (slot +0x70, 0x7E2FA0) **releases handler+0x0C and +0x10 and sets +0xC8=5**.

## 4. PathingSolution waypoints (stride 0x3C, xyz at +0x08 confirmed)
- 0x604E00 (GetTotalPathLength) reads `[ecx+28h]` (count) and `[ecx+24h]` (data), then `add esi,44h` and `[esi-3Ch]`/`[esi]`.
  - So it reads pos(i) at data+i\*0x3C+0x08 and pos(i+1) at +0x44. The loop step is `add esi,3Ch`.
- 0x605D50 (InterpolatePositionAlongPath): `eax=[edi+28h]-1; ecx=eax<<4-eax; lea eax,[edx+ecx*4]` (=idx\*0x3C), then reads `[eax+8]`, `[eax+0Ch]`, `[eax+10h]`.
- The +0x20 array destructor 0x6060B0 steps `add esi,3Ch`; the +0x08 array destructor 0x605F80 steps `add esi,30h`.
- Element +0x04 is a **PathingLocation**. Its RTTI is confirmed: vtable 0xDC4DC0, ctor 0x5FC3F0, 0x28 bytes, xyz at +4/+8/+0xC (init FLT_MAX), cell +0x18, ws +0x1C, triangle u16 +0x24. That gives waypoint xyz at **+0x08/+0x0C/+0x10**, cell at +0x1C and ws at +0x20, as in FNV.
- Second array: +0x08 (data +0x0C, count +0x10, stride 0x30, positions also at +0x08). Used by 0x604D50 and indexed by [sol+0x1C] at 0x7E0F0F.
- Validity check: 0x7DDC70.
- Refcount is at sol+4 (InterlockedIncrement/Decrement imports 0xD9B068/0xD9B064). Release goes through vtable[1] (0x987C70, which calls vtable[0](1); size 0x44).

## 5. Clear/reset/stop
| Function | Address | What it does |
|---|---|---|
| ClearMovementFlags | 0x7DDB40 (slot 4) | `+0x71=0, +0x38=0` |
| SetMovementFlags | 0x7DDB30 (slot 3) | sets +0x71 and +0x38 |
| ResetPath (FNV 0x9DD1C0) | 0x7DF780 `(mover, bool stopHandler)` | Sets state 6 (unless 7/8). Releases +0x1C/+0x20. Calls handler->Stop. If +0x74 is set, it **swaps in a fresh message queue** (dropping stale results) and clears +0x74. |
| FullReset | 0x7DF400 `(mover)` | Sets state 6, releases request and solution, **deletes the handler** and sets +0x24=0, swaps the queue if a request is pending, then tail-calls slot 4. |
| Pre-submit clear (FNV 0x9DB950) | 0x7DF4D0 | Leaves the handler alone. |
| StopMoving, deferred | 0x7DF740 (Actor thunk 0x6F8CE0) | Sets +0x72; the next Update resets. |
| StopMoving, immediate | 0x7E0320 (Actor thunk 0x6F8CF0) | |

Both Actor thunks dereference actor+0x184 without a null check.

## 6. Assessment
**Feasible.** The FO3 code is a near-verbatim match of the FNV pipeline, and every FNV address has an FO3 counterpart. Recommended path-only use, which reads waypoints and lets the mod's existing ActorMove steering follow them:
1. Run everything on the game thread, inside the existing PlayerMover::Update hook.
2. Call FullReset(0x7DF400) on the player's mover.
3. Call `0x6F8AB0(player, ref, radius, nullptr)`. If it returns true with +0x74==0 and +0x6C==6, the target was "already reached".
4. While +0x74 is 1, call ActorMover::Update(0x7E0D40) on the mover each frame, with a timeout. In async mode the result takes a few frames.
5. Capture the waypoints by hooking 0x7E0F59: `mover+0x20` is valid on entry. Copy the points (count at +0x28, data at +0x24, i\*0x3C+8) or AddRef the solution.
   - Without a hook: read `[mover+0x24]+0x0C` right after the Update in which +0x74 dropped to 0.
6. Call FullReset again so no NPC handler keeps ticking.

Crash and behaviour risks:
- **Wrong thread.**
- **Null pointers:** check the mover (player+0x184) and the process (player+0x60) first.
- **Bad target refs:** the target needs a parentCell or worldspace.
- **Solution lifetime:** +0x20 is released by the reset functions, and handler+0x0C is released by Stop on the next tick. Copy the points or AddRef immediately.
- **Requests left pending across save/load:** mover slots 10 to 14 serialize and re-submit (0x7DF860 re-submits +0x1C). Call FullReset on load and before every new goal.
- **Hand-zeroing +0x24:** don't. Use FullReset instead to avoid leaks or dangling pointers.
- **Handler tick:** if a handler is left alive, Update ticks it. FO3's IsActorInActiveMovement (0x6FAE90; call sites in pathing 0x7E80BB, 0x7E152C, 0x7DF5FD) only blocks the player when 0x774B50 is true, which is a special-anim check or InterfaceManager+0x4BC in {2,3}. So it may **pass** for the player and rotate them, unlike FNV's reported behaviour.
- **Override vector:** 0x704FD0 (Actor::UpdateMovement) skips the player at 0x705051 (`cmp esi,[107A104h]; je`), so the mover's override vector is never applied. Steering stays ours.
- **Untested:** whether the six process gates in Update pass for the player in FO3 (FNV found they did) and whether +0x28>0 for typical player paths. Both need an in-game check.
