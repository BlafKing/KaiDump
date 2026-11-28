# Dumping a game with its update from any title: research notes

This document records everything KaiDump has tried so far to dump a game's
**update** (and DLC updates) while kaiHEN was started through a *different*
game, what each attempt returned on the console, and what we currently
believe would be required to make it work.

Status: **not solved**. Today KaiDump falls back to the base version (with a
warning) whenever the update cannot be read, and the only working route for
updates is starting the game you want to dump with R held (Switch 2 titles
only).

---

## 1. The goal

From any title that kaiHEN was launched through, dump:

| Content | What "complete" means |
|---|---|
| Game | ExeFS **of the update**, and the RomFS with the update **applied** (base + patch merged) |
| Switch 2 Editions | Both programs (`game_switch1/`, `game_switch2/`), each with its update |
| DLC | Each add-on's RomFS, **with its DLC update (DataPatch) applied** |

---

## 2. Environment, as far as it matters here

From the kaiHEN source (`ouncebrew/kaihen`, commit `9aa3716`) and libounce:

- **HEN holds privileged sessions** handed over by the bootstrap: an
  already-initialized `fsp-srv` root (`fs_root_`), FS's own SM session
  (`sm_session_`), the `fsp-ldr` root (inside `GameArm`), and handles to the
  FS process and PM.
- **Homebrew's `fsp-srv` is a clone of HEN's FS root** (`GetFilesystemRoot`,
  command 16), installed by libounce as the `fsp-srv` override. It keeps the
  identity the root was initialized with; it is *not* KaiDump's own identity.
  Sending `SetCurrentProcess` on it is forbidden (it would change the identity
  for every holder of the root).
- **Homebrew's `fsp-ldr` is a clone of HEN's loader root**
  (`GetLoaderRoot`, command 23). This is what makes ExeFS dumping work
  (`fsldrOpenCodeFileSystem`). Same rule: no `SetCurrentProcess`.
- **Service lookups go through HEN's SM proxy**, which forwards them on FS's
  SM session.
- **Game arming** (holding R while starting a game) lets a *real* game
  process start through PM with its code replaced by the homebrew loader.
  It only accepts `0x04…` (Switch 2) application IDs
  (`isApplicationProgram()` in `hen/source/game_arm.cpp`).
- KaiDump's own process runs under the program ID of the game kaiHEN was
  launched through (for example Minecraft, `04008A90274EC000`).

---

## 3. What works today

| Case | Result | How |
|---|---|---|
| Base game ExeFS + RomFS | Works | `fsldrOpenCodeFileSystem` / data storage of the base Program NCA |
| Update ExeFS (Stardew, Hollow Knight, Splatoon Raiders) | Works | Registered LR location → update Program NCA, then `fsldrOpenCodeFileSystem`; license prepared first (see §5) |
| Update ExeFS (Donkey Kong Bananza, Hogwarts Legacy) from another title | **Fails** 2002-1004 | Update license cannot be prepared (see §5) |
| Switch 2 program of a Switch 2 Edition (Stardew, TotK program 1) | Works | It is a complete Program NCA in the update, not a patch; opened by path |
| Game RomFS **with update**, kaiHEN launched through **that game** | Works (Switch 2 only) | `OpenDataStorageByCurrentProcess` on a fresh `fsp-srv` session after `SetCurrentProcess` with KaiDump's PID |
| Game RomFS **with update**, launched through **another** game | **Fails** → base-version fallback | §4 |
| Base without RomFS (Hogwarts Legacy: 135 MB stub, all data in the 24.6 GB update) | **Fails**, no fallback possible | §4 + §5 |
| DLC (AddOnContent) | Works | Data storage of the add-on |
| DLC updates (DataPatch, type 132) | **Skipped** | Same problem as game updates |

---

## 4. Merged RomFS (base + update): every method tried

An update's Program NCA RomFS is a patch (BKTR / indirect storage): it only
contains the changed data and references the rest from the base RomFS. FS
only returns a readable, merged RomFS when it builds the storage itself with
both NCAs.

### 4.1 Open the update's Program NCA data storage on its own
- **Call:** open data storage by content path (type 129 content), then
  `romfsMountFromStorage`.
- **Result:** Stardew: mounts, but every file that comes from the base fails
  with **EIO** (`SmallFont.hu-HU.xnb`). Hollow Knight: mount fails
  (`0x1559`).
- **Conclusion:** a patch RomFS cannot be read without its base. Removed.

### 4.2 `fsOpenFileSystemWithPatch(application_id, Rom)`
- **Result:** **2002-6001** (InvalidArgument).
- **Conclusion:** as far as we know FS only accepts this call for the
  manual/HTML type, so it cannot return a RomFS.

### 4.3 `fsOpenDataStorageByProgramId` (fsp-srv command 201) on the FS root clone
- **Setup:** registered LR location of the program pointed at the update's
  Program NCA (same as for ExeFS).
- **Result:** **2002-6605** = `ProgramInfoNotFound`.
- **Explanation (Atmosphère `fssrv`, as reference):** this command looks the
  program up with `GetProgramInfoByProgramId` in FS's **program registry**.
  The registry only holds programs that are running: PM adds them with
  `fsp-pr` `RegisterProgram` at launch and removes them at exit. The caller's
  privilege does not matter, so HEN's own root would get the same 6605.

### 4.4 Native/hybrid route (registered location + program index map)
- **Setup:** registered LR location → selected Program NCA,
  `SetProgramIndexMapInfo` (fsp-srv 810), then `OpenFileSystemWithPatch`,
  `OpenFileSystemWithId`, or data storage by path.
- **Result:** works only for standalone programs (the Switch 2 program of a
  Switch 2 Edition), not for patches.

### 4.5 Register the program in FS's program registry from homebrew (`fsp-pr`)
- **Plan:** `RegisterProgram` under an unused process ID with an empty access
  control, `OpenDataStorageByProgramId`, then `UnregisterProgram`.
- **Result:** the `fsp-pr` session could not be obtained: **2021-0003**
  (`sm::ResultOutOfSessions`), both through `smGetServiceOriginal` and the
  normal lookup. (An earlier note in the chat called this "not allowed"; that
  was a misreading.)
- **Explanation:** FS allows **one** `fsp-pr` session in total, held by PM.
  Atmosphère's `RegisterProgram` also requires the caller to be an **initial
  program** (`IsInitialProgram`), which neither KaiDump nor HEN is.
- Removed from KaiDump.

### 4.6 Program index map + `OpenDataStorageWithProgramIndex` (fsp-srv 205)
- **Plan:** briefly map KaiDump's own program into the target application's
  program index map (KaiDump at index 15, the game at index 0), register the
  update in the LR, then ask for "program index 0".
- **Result:** the map write succeeds, the open fails:
  **2002-6606** on the FS root clone, **2002-6400** (PermissionDenied range)
  on a fresh `fsp-srv` session with `SetCurrentProcess`. KaiDump's
  `GetProgramIndexForAccessLog` also always returns 2002-6400, so FS does not
  know KaiDump's process under the program ID it reports.
- Removed from KaiDump.

### 4.7 Run inside the game (game arming, "hold R")
- **Result:** works. A real PM launch registers the program (with its
  update) in the registry, and `OpenDataStorageByCurrentProcess` returns the
  merged RomFS; the update key is also available.
- **Limits:** only Switch 2 (`0x04…`) titles can be armed, only the title you
  launched can be dumped, and it is awkward for users.

---

## 5. Update keys (licenses)

Some update NCAs use a rights ID of their own. Before FS can decrypt them, the
console's license for that rights ID has to be prepared.

What KaiDump does (in order) when an open fails with 2002-1004
(`NcaExternalKeyNotFound`):
1. Prepare the license for the content's rights ID (ES prepare, cmd 503).
2. If that fails, create a license context for the rights title (contexts
   2001 / 2 / 17 / 3 / 102), evaluate it, and prepare again.
3. If that fails, try the context of the application itself.

Observed:

| Title | Base license | Update license | Update ExeFS |
|---|---|---|---|
| Stardew, Hollow Knight | prepared | not needed separately once the base license is prepared | opens |
| Splatoon Raiders (launched through itself) | — | available | opens |
| Donkey Kong Bananza, Hogwarts Legacy (from another title) | prepared | **fails**: prepare after context `0x00000691`, application context `0x00032691` | **2002-1004** |

So even with a merged-RomFS route, these titles also need their **update
license made available** while the game is not running. Inside the running
game (armed) it already is.

---

## 6. What would be required

### 6.1 Merged RomFS from any title
FS will only build the merged storage for a program in its registry. Options:

1. **A HEN command that registers the target program for the duration of the
   open** (registry entry under a spare process ID → open the data storage →
   unregister → return only the storage). Needs HEN to have a way to perform
   `fsp-pr` `RegisterProgram` despite the single session held by PM and the
   initial-program check. Whether that is possible is for the kaiHEN
   developer to judge.
2. **Any other HEN-side way to get FS to compose base + patch** for an
   installed program that is not running, returning a read-only storage.
3. **Arming Switch 1 titles.** Would make "hold R" possible for Switch 1
   games, but still requires launching each game; it does not meet the
   "from any game" goal.
4. **Applying the patch in software** (reading base and patch and resolving
   the indirect table ourselves, as on Switch 1 with raw NCAs). FS does not
   hand out the raw patch data in a usable form from homebrew, so this is not
   considered feasible.

Whatever the route, homebrew should get **only a read-only storage** back:
no shared session, no `SetCurrentProcess`, nothing left registered. That is
the shape the kaiHEN maintainer asked for in PR #8.

### 6.2 Update keys
The update's license must be prepared or loaded while the game is not
running (Donkey Kong Bananza, Hogwarts Legacy). The command in 6.1 should
cover this, or a separate HEN-side step must.

### 6.3 DLC updates
A DLC update (DataPatch, type 132) is a patch on its DLC exactly like a game
update. With 6.1 and 6.2 in place, the same route (by data ID / add-on
program ID) should give the merged DLC RomFS, and KaiDump can stop skipping
them.

### 6.4 In KaiDump, once a route exists
- Try the new route before the base-version fallback.
- Drop the "hold R while starting this game" hints and most fallback
  warnings.
- `game_switch1/` and Switch 1 NUPs get the updated version.
- Offer DLC updates in the content list.

---

## 7. Where things stand

- An issue was filed on the kaiHEN repository asking for a narrow command
  that returns a read-only data storage of an installed program with its
  update applied, including the key problem.
- Next step on our side: a small diagnostic HEN command in our fork that
  tries, from HEN's own sessions and in order, to open `fsp-pr` through FS's
  SM session, `RegisterProgram` the target, `OpenDataStorageByProgramId`, and
  `UnregisterProgram`, and logs each result. One on-device run with Hollow
  Knight / Stardew then tells us whether 6.1 option 1 is possible.
- Caveat: our reading of FS behaviour is based on Atmosphère's `fssrv`
  reimplementation, much of which is unimplemented for these calls. Real FS
  on HOS 20 / Switch 2 may differ.

---

## 8. Error codes seen

| Code | Raw | Meaning | Where |
|---|---|---|---|
| 2002-1001 | `0x7d202` | PartitionNotFound | Base Program NCA without RomFS (Hogwarts Legacy) |
| 2002-1004 | `0x7d802` | NcaExternalKeyNotFound | Update license not prepared |
| 2002-6001 | `0x2ee202` | InvalidArgument | `OpenFileSystemWithPatch` with Rom type |
| 2002-6400 | `0x320002` | PermissionDenied (range) | `OpenDataStorageWithProgramIndex` on own session; `GetProgramIndexForAccessLog` |
| 2002-6605 | `0x339a02` | ProgramInfoNotFound | `OpenDataStorageByProgramId`, program not registered |
| 2002-6606 | `0x339c02` | NotFound range (exact meaning unknown) | `OpenDataStorageWithProgramIndex` on the FS root clone |
| 2021-0003 | `0x615` | sm OutOfSessions | Looking up `fsp-pr` |
| — | `0x1559` | RomFS mount failed | Standalone patch RomFS (Hollow Knight) |
| — | EIO | Read error | Reading base-backed files from a standalone patch RomFS (Stardew) |
