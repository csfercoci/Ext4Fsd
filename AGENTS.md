# Ext4Fsd — Windows Kernel Driver for ext2/ext3/ext4

WDM kernel filesystem driver. **Pure C — no C++, no GNU extensions.**
Toolchain: Visual Studio 2026 (v18) + WDK 10.0.26100.0.

## Build / Install

**Spectre mitigation MUST be disabled** — project lacks Spectre libraries.

```
build.cmd              # Debug x64 (default)
build.cmd release      # Release x64
install.cmd            # Debug x64 (default; matches build.cmd)
install.cmd release    # Release x64 — exits non-zero if live replace needs reboot
```

Manual msbuild (pass SolutionDir=repo root so OutDir is Ext4Fsd\{Config}\x64\):
```
cmd /c "call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && msbuild Ext4Fsd\Ext4Fsd.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SpectreMitigation=false /p:SolutionDir=%CD%\ /m"
```

**Output filename gotcha**: vcxproj `TargetName=Ext2Fsd`. Binary is `Ext4Fsd\{Debug|Release}\x64\Ext2Fsd.sys` (NOT `Ext4Fsd.sys`). Service name also `Ext2Fsd`. Installs to `%SystemRoot%\System32\drivers\Ext2Fsd.sys`.

## Verify a change

- **Unit (host):** `tests/build_test.bat` — userland tests against driver headers (17/17). VS 2026 dev cmd prompt. Header-level only; does NOT exercise the kernel driver.
- **Kernel (VM):** see `docs/testing.md` — VM setup, `vmrun` usage, install-after-reboot rule, stress + persist workflow. `ASSERT()` is compiled out in Release (`DBG=0`) — never rely on it for runtime safety.

## Detailed docs (read the relevant one before working)

- `agent_docs/architecture.md` — source tree, codepage/UTF-8, caches, JBD2 overview
- `agent_docs/conventions.md` — code style, endianness/type gotchas, tooling noise to ignore
- `docs/journaling.md` — kjournald batch/force-commit, barriers, dirty ranges, orphans, stubs
- `docs/testing.md` — VM test harness and workflows

## Notable recent behavior (committed)

- Default CodePage **utf8** when registry unset; invalid registry falls back to **default** (system ANSI); volume inherits global; surrogate-aware UTF-8 codec
- Inode cache, MCB child hash, per-FCB ordered dirty ranges, rename preallocate-before-delete
- Journal: multi-descriptor tags for small blocks; no head rollback after durable commit (abort instead); real commit ordering + disk flush barriers; `JournalCommittedSeq` fsync wait
- Orphan on truncate fail / truncate-orphan nlink>0 (FCB syncs i_size on async orphan); bitmaps journaled with undo on DirtyMetadata fail; failed commit requeues handle
- Perf: 15ms fsync batch only when concurrent forcers; bulk flush one commit; shared ordered-range flush; dir enum validates name_len
