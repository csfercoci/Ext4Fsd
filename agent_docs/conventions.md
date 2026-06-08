# Conventions & gotchas

## Code conventions
- MSVC only. C23 features OK for tests, kernel keeps to C99-ish.
- Hungarian + `IN/OUT` annotations; `__try/__finally` for cleanup.
- `kfree` and `kmem_cache_destroy` macros are NULL-safe.

## Endianness / type gotchas
- Superblock fields are `__le32` — wrap with `cpu_to_le32` / `le32_to_cpu` (e.g. `s_last_orphan`).
- `Vcb->SuperBlock` → `PEXT2_SUPER_BLOCK` (`struct ext3_super_block`). Driver also exposes `&Vcb->sb` (Linux `super_block`); use `EXT4_SB(&Vcb->sb)->s_journal` to test for journal.
- In-memory `inode->i_dtime` is a native `__u32`; on disk it's `__le32`. Driver code treats it as native (`Ext2SaveInode` does the swap).

## Tooling noise to ignore
- The Edit/Read tool LSP wires through clangd (`.clangd`) which lacks a real `compile_commands.json`. **Editing `ext2fs.h`, `linux/*.h`, `memory.c`, `fileinfo.c` etc. produces dozens of bogus "file not found" / "unknown type" diagnostics.** Treat only `msbuild` errors as truth. Don't waste time chasing LSP errors.
- `.gitignore` is minimal (`.vs/ Debug/ Release/ *.vcxproj.user Ext2Fsd-setup.exe */cab/`). Root accumulates `*.log`, `t.obj`, `test_std.obj`, `UpgradeLog.htm`, `devenv_*.log` — do NOT add these to commits.
