# Architecture

WDM kernel filesystem driver for ext2/ext3/ext4. Pure C (no C++, no GNU extensions).

- `Ext4Fsd/` — driver source root
  - top-level `*.c` — IRP handlers (create, read, write, close, flush, fileinfo, fsctl, devctl, cleanup)
  - `init.c` — `DriverEntry` / `DriverUnload`
  - `linux.c` — Linux kernel API emulation (kmalloc, kmem_cache, buffer_head, ll_rw_block)
  - `memory.c` — FCB/VCB/MCB/inode lifecycle; mount path (`Ext2MountVolume`); bhReaper
  - `block.c` — sync block I/O via IRP
  - `ext3/` — common metadata ops (inode/group/bitmap save/load, indirect blocks, htree, journal recovery wrappers in `recover.c`)
  - `ext4/` — extents, xattr, ext4_jbd2 bridge, checksums
  - `jbd2/` — JBD2 journaling (journal/transaction/revoke/recovery)
  - `include/linux/` — Linux header emulation (resolves `#include <linux/*.h>`)
  - `jbd2_compat.h` — Linux→NT API stubs (must include AFTER struct defs that they reference)
- `Ext2Mgr/` Win32 GUI · `Ext2Srv/` user-mode helper · `Setup/` NSIS · `package/` packaging

Project includes: `.\include;.\include\asm`. Preprocessor defines `__KERNEL__`.

## JBD2 / Journaling

**Async-commit JBD2 is functional.** A dedicated `kjournald` thread per mounted volume commits the deferred journal handle.

Deferred handles store `buffer_head` pointers; each must be `get_bh()`-pinned when added and `put_bh()`-released after commit, or the bhReaper frees them mid-commit (use-after-free). See `docs/journaling.md` for full details (kjournald, SyncReaper, CcFlushCache rules, commit barriers, wrapping pattern, orphan list).
