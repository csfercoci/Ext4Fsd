# Architecture

WDM kernel filesystem driver for ext2/ext3/ext4. Pure C (no C++, no GNU extensions).

- `Ext4Fsd/` — driver source root
  - top-level `*.c` — IRP handlers (create, read, write, close, flush, fileinfo, fsctl, devctl, cleanup)
  - `init.c` — `DriverEntry` / `DriverUnload`; registry defaults (CodePage → **utf8** when unset/invalid)
  - `linux.c` — Linux kernel API emulation (kmalloc, kmem_cache, buffer_head, ll_rw_block)
  - `memory.c` — FCB/VCB/MCB/inode lifecycle; mount path (`Ext2MountVolume`); bhReaper; MCB child hash
  - `misc.c` — name encoding; surrogate-aware UTF-8 codec when volume codepage is `utf8`
  - `block.c` — sync block I/O via IRP; `Ext2DiskFlushBuffers` / `Ext2FlushDiskCache` barriers
  - `ext3/` — common metadata ops (inode/group/bitmap save/load, indirect blocks, htree, journal recovery wrappers in `recover.c`); **inode cache** (LRU, generation invalidation)
  - `ext4/` — extents, xattr, ext4_jbd2 bridge, checksums
  - `jbd2/` — JBD2 journaling (journal/transaction/revoke/recovery); some APIs still stubbed
  - `include/linux/` — Linux header emulation (resolves `#include <linux/*.h>`)
  - `jbd2_compat.h` — Linux→NT API stubs (must include AFTER struct defs that they reference)
- `Ext2Mgr/` Win32 GUI · `Ext2Srv/` user-mode helper · `Setup/` NSIS · `package/` packaging

Project includes: `.\include;.\include\asm`. Preprocessor defines `__KERNEL__`.

## Codepage / names

- Global default CodePage is **utf8** when registry key is missing or invalid (`init.c`).
- Per-volume settings without an explicit CodePage inherit the global table (`devctl.c`).
- UTF-8 path uses a surrogate-aware codec in `misc.c` (not single-`wchar_t` NLS round-trip).
- Encoded names longer than `EXT2_NAME_LEN` are rejected before directory mutation.

## Caches / lookups

- **Inode cache** (`Vcb->InodeCache*`) — read-only accelerator for decoded inodes (dir enum hot path). Never source of truth for writes. Invalidate on save/clear/free/extent-init; generation bump closes read-vs-write races. `InodeCacheLock` is a leaf lock (`s_gd_lock` → `InodeCacheLock` order).
- **MCB child hash** — O(1) directory-child lookup when a directory has 16+ children.
- **Ordered dirty ranges** per FCB — track modified byte ranges for data=ordered flush without full-cache flushes.

## JBD2 / Journaling

**Async-commit JBD2 is functional.** A dedicated `kjournald` thread per mounted volume commits the deferred journal handle (5s batch delay, 10s safety net, immediate on fsync).

Deferred handles store `buffer_head` pointers; each must be `get_bh()`-pinned when added and `put_bh()`-released after commit, or the bhReaper frees them mid-commit (use-after-free). See `docs/journaling.md` for full details (kjournald, SyncReaper, CcFlushCache rules, commit barriers, dirty ranges, wrapping pattern, orphan list, stub gaps).
