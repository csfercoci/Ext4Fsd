# JBD2 / Journaling — Detailed Reference

## kjournald (async commit)

- `ext4/ext4_jbd2.c`: `Ext2KjournaldThread`, `Ext2StartKjournald`, `Ext2StopKjournald`, `Ext2JournalForceCommit`
- Thread per VCB, started after `Ext2RecoverJournal` in mount path, stopped in `Ext2DestroyVcb`
- `EXT2_COMMIT_INTERVAL_SECONDS = 10` — safety-net timeout; actual commit happens on every `ext4_journal_stop` via `KeSetEvent(&Vcb->KjournaldWake)`
- Commit-on-stop pattern: `ext4_journal_stop` merges handle into `Vcb->PendingJournalHandle`, then signals kjournald. If multiple stops fire before kjournald processes, handles are batched (deduplicated buffers, merged revokes).
- Pending handle overflow (`MAX_HANDLE_BUFFERS=256`/`MAX_HANDLE_REVOKES=256`): forces synchronous `journal_commit_sync` of the old pending, new handle becomes pending.
- Force-commit: `Ext2JournalForceCommit(Vcb)` wakes kjournald and waits for completion. Called from `flush.c` (`Ext2FlushFile`) after `CcFlushCache` — ensures fsync/FlushFileBuffers returns with metadata on disk.
- No `Ext2JournalFlushPending` in `Ext2FlushVcb` — kjournald owns all deferred commit.

## SyncReaper (periodic metadata flush)

- `memory.c`: `Ext2SyncReaperThread` — wakes every `EXT2_SYNC_INTERVAL_SECONDS = 30` (safety net)
- Walks mounted VCBs, non-blocking acquire on `Vcb->MainResource`, calls `Ext2FlushVcb` (bh writeback + range flush, NO `CcFlushCache`)
- Does NOT commit journal — kjournald handles that

## DASD volume read path

- `read.c`: `Ext2FlushVolume` REMOVED from direct/DASD volume read. `fsutil`/Explorer trigger DASD reads during probing; synchronously flushing the entire volume while holding `MainResource` exclusive deadlocks the caller. Only sets `CCB_VOLUME_DASD_PURGE` flag.

## CcFlushCache locations

- `CcFlushCache(&Vcb->SectionObject, NULL, 0, ...)` is called ONLY in safe contexts (no `MainResource` contention):
  - `shutdown.c` — after `Ext2FlushVolume` at shutdown
  - `fsctl.c` — after `Ext2FlushVolume` at FSCTL_LOCK_VOLUME
  - `fsctl.c` — after `Ext2FlushVolume` at dismount
  - `create.c` — after `Ext2FlushVolume` at close/dismount
  - `pnp.c` — after `Ext2FlushVolume` at PnP query-remove
  - `devctl.c` — after `Ext2FlushVolume` at read-only transition
- NOT in `Ext2FlushVcb` (would deadlock when called from SyncReaper under `MainResource`)
- NOT in DASD read path (would deadlock when called from `fsutil`/Explorer under `MainResource`)

## Journal commit barriers

- `journal_commit_sync` uses 2x `Ext2FlushDiskCache(Vcb)` (`IOCTL_DISK_FLUSH_CACHE`) barriers:
  1. After descriptor+data+revoke writes (before commit block)
  2. After commit block (before journal superblock update)
- Home writes have no explicit barrier (redundant with checkpoint)
- `Ext2WriteSync` — synchronous non-cached write with 30s timeout + cancel + infinite wait fallback

## Wrapping pattern for metadata-mutating top-level ops

```c
BOOLEAN OwnsTxn = Ext2JournalNestedStart(IrpContext, Vcb, /*blocks*/32);
__try { ... metadata ops ... } __finally { ... }
if (OwnsTxn) Ext2JournalStop(IrpContext, Vcb);
```
- `IrpContext->Handle` carries the active JBD2 `handle_t *`. NULL → fallback to direct `mark_buffer_dirty` (legacy/no-journal/ext2 path).
- `NestedStart` returns FALSE if a handle already exists → caller must NOT Stop.
- `Ext2DirtyMetadata(IrpContext, Vcb, bh)` is the chokepoint inside `Ext2Save{Group,Inode,Block,Buffer}` etc. — routes through journal when `Handle != NULL`.
- Per-handle limits: `MAX_HANDLE_BUFFERS=256`, `MAX_HANDLE_REVOKES=256`. Watch dmesg for `"Ext4Fsd: handle buffer overflow"` / `"handle revoke overflow"`.
- Block frees emit revoke records via `Ext2JournalRevokeBlock` (called in `Ext2FreeBlock`).
- Orphan list: `Ext2OrphanAdd/Del` in `Ext2DeleteFile`. Mount-time `Ext2ProcessOrphanList` finishes interrupted deletes. NEXT_ORPHAN aliased on `i_dtime`.
- Truncate-orphan (nlink>0 mid-truncate) NOT yet emitted → standalone `Ext2TruncateFile` crash still leaks blocks.

Top-level wrapped ops: `Ext2CreateFile` (64), `Ext2DeleteFile` (32), `Ext2TruncateFile` (32), `Ext2SetFileInformation` (32), `Ext2WriteFile` expand path (32), `Ext2WriteSymlink/TruncateSymlink/SetReparsePoint/DeleteReparsePoint` (32). Inner `Ext2CreateInode` budget 16 (nested).

`Ext2SaveSuper` is left unwrapped (idempotent shutdown/volinfo paths).
