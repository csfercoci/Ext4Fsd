# Ext4Fsd — Windows Kernel Driver for ext2/ext3/ext4

## Build / Install

**Toolchain**: Visual Studio 2026 (v18) + WDK 10.0.26100.0.
**Spectre mitigation must be disabled** — project lacks Spectre-libraries.

```
build.cmd              # Debug x64 (default)
build.cmd release      # Release x64
install.cmd [release]  # Stops driver, copies .sys, sc create+start (auto-elevates)
```

Manual msbuild:
```
cmd /c "call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && msbuild Ext4Fsd\Ext4Fsd.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SpectreMitigation=false /m"
```

**Output filename gotcha**: vcxproj `TargetName=Ext2Fsd`. Binary is `Ext4Fsd\Ext4Fsd\{Debug|Release}\x64\Ext2Fsd.sys` (NOT `Ext4Fsd.sys`). Service name also `Ext2Fsd`. Installs to `%SystemRoot%\System32\drivers\Ext2Fsd.sys`.

## User-mode tests

`tests/build_test.bat` builds + runs userland unit tests against driver headers (sb / dir / extents / image — currently 17/17). Requires VS 2026 dev cmd prompt. Does NOT exercise the kernel driver — only header-level logic.

## Architecture

WDM kernel filesystem driver. Pure C (no C++, no GNU extensions).

- `Ext4Fsd/` driver source root
  - top-level `*.c` — IRP handlers (create, read, write, close, flush, fileinfo, fsctl, devctl, cleanup)
  - `init.c` — `DriverEntry` / `DriverUnload`
  - `linux.c` — Linux kernel API emulation (kmalloc, kmem_cache, buffer_head, ll_rw_block)
  - `memory.c` — FCB/VCB/MCB/inode lifecycle; mount path (`Ext2MountVolume`)
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

See `docs/journaling.md` for full details (kjournald, SyncReaper, CcFlushCache rules, commit barriers, wrapping pattern, orphan list).

## Coding conventions / gotchas

- MSVC only. C23 features OK for tests, kernel keeps to C99-ish.
- Hungarian + `IN/OUT` annotations; `__try/__finally` for cleanup.
- `ASSERT()` compiled out in Release (`DBG=0`) — never use for runtime safety.
- `kfree` and `kmem_cache_destroy` macros are NULL-safe.
- Superblock fields are `__le32` — wrap with `cpu_to_le32` / `le32_to_cpu` (e.g. `s_last_orphan`).
- `Vcb->SuperBlock` → `PEXT2_SUPER_BLOCK` (`struct ext3_super_block`). Driver also exposes `&Vcb->sb` (Linux `super_block`); use `EXT4_SB(&Vcb->sb)->s_journal` to test for journal.
- In-memory `inode->i_dtime` is a native `__u32`; on disk it's `__le32`. Driver code treats it as native (`Ext2SaveInode` does the swap).

## Tooling noise to ignore

- The Edit/Read tool LSP wires through clangd (`.clangd`) which lacks a real `compile_commands.json`. **Editing `ext2fs.h`, `linux/*.h`, `memory.c`, `fileinfo.c` etc. produces dozens of bogus "file not found" / "unknown type" diagnostics.** Treat only `msbuild` errors as truth. Don't waste time chasing LSP errors.
- `.gitignore` is minimal (`.vs/ Debug/ Release/ *.vcxproj.user Ext2Fsd-setup.exe */cab/`). Root accumulates `*.log`, `t.obj`, `test_std.obj`, `UpgradeLog.htm`, `devenv_*.log` — do NOT add these to commits.

## Testing

See `docs/testing.md` for VM setup, vmrun usage, persist test workflow, and safe scheduled-task pattern.
