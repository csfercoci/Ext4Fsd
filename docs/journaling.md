# JBD2 / Journaling — Detailed Reference

## kjournald (async commit)

- `ext4/ext4_jbd2.c`: `Ext2KjournaldThread`, `Ext2StartKjournald`, `Ext2StopKjournald`, `Ext2JournalForceCommit`
- Thread per VCB, started after `Ext2RecoverJournal` in mount path, stopped in `Ext2DestroyVcb`
- Wakes on:
  - **Batch timer** — `EXT2_BATCH_DELAY_MS = 5000` after the first `ext4_journal_stop` that creates/extends the pending handle. Burst stops within the window merge into one pending handle → one commit.
  - **Force-commit** — `Ext2JournalForceCommit` cancels the batch timer and signals `KjournaldWake` immediately (fsync / FlushFileBuffers).
  - **Safety-net timeout** — `EXT2_COMMIT_INTERVAL_SECONDS = 10` on the kjournald wait.
- Commit path: `ext4_journal_stop` merges the handle into a VCB-level pending FIFO (`PendingJournalHandle` / `PendingJournalTail`) under `j_checkpoint_mutex`. Handles are batched (deduplicated buffers, merged revokes). Overflow (tail full) queues a new FIFO entry and wakes kjournald immediately.
- Pending handle limits: `MAX_HANDLE_BUFFERS=256` / `MAX_HANDLE_REVOKES=256`.
- Double-stop guard: if the same `eh` is already on the pending FIFO, stop is a no-op (avoids double-free).
- Force-commit: `Ext2JournalForceCommit(Vcb)` increments `JournalForceWaiters`; the `EXT2_FSYNC_BATCH_MS` (15ms) delay runs only when waiters > 1 so solo fsync is not taxed. Then snapshots `JournalCommittedSeq`, wakes kjournald, waits on `KjournaldDone` until the sequence advances and the pending FIFO is empty (up to ~30s). Returns `JournalCommitError` via `Ext2WinntError`, or `STATUS_IO_TIMEOUT`. Called from `Ext2FlushFile` (per-file) and once at end of `Ext2FlushFiles` (bulk). Must **not** commit inline under caller locks (ABBA with `FcbLock` / `MainResource`).
- `Ext2FlushFiles` flushes each file **without** per-file force-commit, then one `Ext2JournalForceCommit` for the batch; clears ordered ranges / `FCB_FILE_MODIFIED` only after that barrier succeeds.
- Shared helper `Ext2FlushFcbOrderedData` (used by fsync and kjournald ordered flush): ranged flush when ranges exist, else full-file; restores ranges on failure. Does not clear `FCB_FILE_MODIFIED`.
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

- `journal_commit_sync` uses 3× `Ext2DiskFlushBuffers(Vcb)` (`IRP_MJ_FLUSH_BUFFERS` to the target device):
  1. After descriptor+data+revoke writes (before commit block) — PREFLUSH equivalent
  2. After commit block (before home writes) — FUA equivalent
  3. After home writes (before on-disk log tail advance / reclaim)
- `__wait_on_buffer` flushes the buffer's volume-stream range via `CcFlushCache` and surfaces failure through `Write_EIO`.
- `submit_bh` consumes one bh reference; every journal-block submit takes an extra `get_bh` so `journal_wait_for_writes` can `brelse` without underflow.
- Journal commit result: `Vcb->JournalCommitError` (0 or negative errno); sequence still bumps on failure so ForceCommit waiters wake.

## data=ordered + dirty ranges

- Before metadata commit, kjournald flushes modified file data (`Ext2FlushDirtyData` → `Ext2FlushFcbOrderedData`) so data hits disk before the metadata that points at it.
- Per-FCB ordered dirty ranges (`OrderedDirtyRanges[]` / `Ext2MarkOrderedDirtyRange`) track which byte ranges need flush; avoids full-cache flushes on every commit.
- `FCB_FILE_MODIFIED` must be set **before** `Ext2JournalStop` on expand paths so a concurrent commit does not skip the file. Clear the flag only after a successful force-commit (or cleanup/purge), never after data flush alone.

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
- Orphan list: `Ext2OrphanAdd/Del` on delete (`nlink==0`) and on standalone truncate when `nlink>0` and size shrinks (`Ext2TruncateFile`). Mount-time `Ext2ProcessOrphanList` finishes both: delete orphans truncate-to-0 + free inode; truncate orphans free blocks past `i_size` and keep the inode. NEXT_ORPHAN aliased on `i_dtime`.
- Failed delete truncate keeps the inode on the orphan list (does not FreeInode).
- Descriptor packing: tags per descriptor capped by `(blocksize - header) / journal_tag_bytes`; multi-descriptor commits use interleaved `[desc][data...]` layout for recovery.
- Failed `journal_commit_sync` requeues the pending handle (does not free it). After a durable commit record, **never** roll back `j_head`/`j_free`/`tid` — abort the journal instead so the recovery log cannot be overwritten.
- Bitmap alloc/free: undo in-core bit flips if `Ext2DirtyMetadata` fails before free-count publish.

Top-level wrapped ops: `Ext2CreateFile` (64), `Ext2DeleteFile` (32), `Ext2TruncateFile` (32), `Ext2SetFileInformation` (32), `Ext2WriteFile` expand path (32), `Ext2WriteSymlink/TruncateSymlink/SetReparsePoint/DeleteReparsePoint` (32). Inner `Ext2CreateInode` budget 16 (nested).

`Ext2SaveSuper` is left unwrapped (idempotent shutdown/volinfo paths).

## JBD2 port gaps (still stubs)

- `jbd2_journal_cancel_revoke` — stub (`-ENOENT`); real path in `revoke.c` is `#if 0`
- `__jbd2_journal_remove_checkpoint` — stub; `checkpoint.c` not ported
- `__jbd2_log_wait_for_space` — stub (no wait when journal is full)
- `jbd2_log_wait_commit` — no-op (this architecture commits synchronously inside `journal_commit_sync`)
