#include "ext2fs.h"
#include "linux\ext4.h"
#include <linux/jbd2.h>

#define EXT4_NOJOURNAL_MAX_REF_COUNT ((unsigned long) 4096)

static inline int ext4_handle_valid(handle_t *handle)
{
    if ((ULONG_PTR)handle < EXT4_NOJOURNAL_MAX_REF_COUNT)
        return 0;
    return 1;
}

#define MAX_HANDLE_BUFFERS   256

#define MAX_HANDLE_REVOKES   256

#define EXT2_COMMIT_INTERVAL_SECONDS  10
/* Linux jbd2 default commit interval is 5s.  A short delay here makes every
 * sustained write/delete workload commit (and data=ordered flush) dozens of
 * times per second, which degrades the volume to write-through speed.  Crash
 * durability within this window is still provided by fsync/FlushFileBuffers
 * (Ext2JournalForceCommit) and the flush/dismount funnel. */
#define EXT2_BATCH_DELAY_MS           5000

struct ext4_handle {
    handle_t            h;
    int                 nbuffers;
    struct buffer_head  *buffers[MAX_HANDLE_BUFFERS];
    journal_t           *journal;
    struct super_block  *sb;
    int                 nrevoked;
    __u64               revoked[MAX_HANDLE_REVOKES];
};

static handle_t no_journal;

static struct ext4_handle *to_eh(handle_t *handle)
{
    return (struct ext4_handle *)((char *)handle - FIELD_OFFSET(struct ext4_handle, h));
}

static int journal_wait_for_writes(struct buffer_head **bufs, int nbufs)
{
    int i;
    int err = 0;

    for (i = 0; i < nbufs; i++) {
        wait_on_buffer(bufs[i]);
        if (buffer_write_io_error(bufs[i])) {
            clear_buffer_write_io_error(bufs[i]);
            err = -EIO;
        }
        brelse(bufs[i]);
    }

    return err;
}

/*
 * Minimal NT-native journal commit: write dirty buffers to journal,
 * wait for I/O, then write to home locations.
 *
 * Journal layout per transaction (standard JBD2 format):
 *   [descriptor] [data block 0] [data block 1] ... [commit block]
 *
 * Descriptor block contains block tags mapping data blocks to home locations.
 * Data blocks contain copies of modified metadata buffers.
 * Commit block marks the transaction as complete.
 *
 * After commit block is on disk, the data is crash-safe.
 */
static int journal_commit_sync(struct ext4_handle *eh)
{
    journal_t                  *journal = eh->journal;
    int                        blocksize = journal->j_blocksize;
    unsigned long              head, first, last;
    unsigned long              transaction_start;
    int                        i, err = 0;
    unsigned long long         desc_phys, commit_phys;
    unsigned long long         data_phys[MAX_HANDLE_BUFFERS];
    unsigned long long         revoke_phys[8];
    struct buffer_head         *jbh;
    struct buffer_head         *wbufs[MAX_HANDLE_BUFFERS + 16]; /* desc + data + revoke + commit */
    int                        nwbufs = 0;
    int                        nblocks;
    int                        nrev_blocks = 0;
    int                        revoke_entry_sz;
    int                        revoke_per_block;
    struct buffer_head         *home_bufs[MAX_HANDLE_BUFFERS];
    int                        nhome = 0;

    if (eh->nbuffers == 0 && eh->nrevoked == 0)
        return 0;

    revoke_entry_sz = jbd2_has_feature_64bit(journal) ? 8 : 4;
    revoke_per_block = (blocksize - sizeof(jbd2_journal_revoke_header_t)) / revoke_entry_sz;
    if (eh->nrevoked > 0) {
        nrev_blocks = (eh->nrevoked + revoke_per_block - 1) / revoke_per_block;
        if (nrev_blocks > (int)(sizeof(revoke_phys)/sizeof(revoke_phys[0]))) {
            return -ENOSPC;
        }
    }

    mutex_lock(&journal->j_checkpoint_mutex);

    head = journal->j_head;
    transaction_start = head;
    first = journal->j_first;
    last = journal->j_last;
    nblocks = 1 + eh->nbuffers + nrev_blocks + 1; /* descriptor + data + revoke + commit */
    if (nblocks > (int)(journal->j_free)) {
        mutex_unlock(&journal->j_checkpoint_mutex);
        return -ENOSPC;
    }

    /* --- Phase 0: Pre-compute all logical → physical mappings --- */

    /* descriptor block bmap */
    {
        unsigned long logical_block = head;

        head++;
        if (head == last)
            head = first;

        err = jbd2_journal_bmap(journal, logical_block, &desc_phys);
        if (err)
            goto out_release;
    }

    for (i = 0; i < eh->nbuffers; i++) {
        unsigned long logical_block = head;

        head++;
        if (head == last)
            head = first;

        err = jbd2_journal_bmap(journal, logical_block, &data_phys[i]);
        if (err)
            goto out_release;
    }

    /* revoke descriptor block bmap(s) */
    for (i = 0; i < nrev_blocks; i++) {
        unsigned long logical_block = head;

        head++;
        if (head == last)
            head = first;

        err = jbd2_journal_bmap(journal, logical_block, &revoke_phys[i]);
        if (err)
            goto out_release;
    }

    /* commit block bmap */
    {
        unsigned long logical_block = head;

        head++;
        if (head == last)
            head = first;

        err = jbd2_journal_bmap(journal, logical_block, &commit_phys);
        if (err)
            goto out_release;
    }

    /* --- Phase 1: Write descriptor block (standard JBD2: descriptor first) --- */

    {
        journal_header_t *header;
        char *tag_ptr;

        jbh = sb_getblk_zero(eh->sb, (sector_t)desc_phys);
        if (!jbh) {
            err = -ENOMEM;
            goto out_release;
        }

        header = (journal_header_t *)jbh->b_data;
        header->h_magic = cpu_to_be32(JBD2_MAGIC_NUMBER);
        header->h_blocktype = cpu_to_be32(JBD2_DESCRIPTOR_BLOCK);
        header->h_sequence = cpu_to_be32(journal->j_transaction_sequence);

        tag_ptr = jbh->b_data + sizeof(journal_header_t);

        for (i = 0; i < eh->nbuffers; i++) {
            struct buffer_head *bh = eh->buffers[i];
            journal_block_tag_t *tag = (journal_block_tag_t *)tag_ptr;

            tag->t_blocknr = cpu_to_be32((__u32)(bh->b_blocknr & 0xFFFFFFFF));
            tag->t_flags = cpu_to_be16((i == eh->nbuffers - 1) ? JBD2_FLAG_LAST_TAG : 0);
            tag->t_blocknr_high = cpu_to_be32((__u32)(bh->b_blocknr >> 32));
            tag->t_checksum = 0;

            tag_ptr += sizeof(journal_block_tag_t);
        }

        set_buffer_dirty(jbh);
        mark_buffer_dirty(jbh);
        set_buffer_uptodate(jbh);
        err = submit_bh(WRITE, jbh);
        wbufs[nwbufs++] = jbh;
        if (err)
            goto out_wait_journal;
    }

    /* --- Phase 2: Write data copies to journal (standard JBD2: after descriptor) --- */

    for (i = 0; i < eh->nbuffers; i++) {
        struct buffer_head *bh = eh->buffers[i];

        jbh = sb_getblk_zero(eh->sb, (sector_t)data_phys[i]);
        if (!jbh) {
            err = -ENOMEM;
            goto out_release;
        }

        memcpy(jbh->b_data, bh->b_data, blocksize);
        set_buffer_dirty(jbh);
        mark_buffer_dirty(jbh);
        set_buffer_uptodate(jbh);
        err = submit_bh(WRITE, jbh);
        wbufs[nwbufs++] = jbh;
        if (err)
            goto out_wait_journal;

        home_bufs[nhome++] = bh;
    }

    /* --- Phase 2.5: Write revoke descriptor block(s) --- */

    if (nrev_blocks > 0) {
        int rev_idx = 0;
        int b;

        for (b = 0; b < nrev_blocks; b++) {
            jbd2_journal_revoke_header_t *rhdr;
            char *p;
            int n_this;
            int k;

            jbh = sb_getblk_zero(eh->sb, (sector_t)revoke_phys[b]);
            if (!jbh) {
                err = -ENOMEM;
                goto out_wait_journal;
            }

            rhdr = (jbd2_journal_revoke_header_t *)jbh->b_data;
            rhdr->r_header.h_magic = cpu_to_be32(JBD2_MAGIC_NUMBER);
            rhdr->r_header.h_blocktype = cpu_to_be32(JBD2_REVOKE_BLOCK);
            rhdr->r_header.h_sequence = cpu_to_be32(journal->j_transaction_sequence);

            p = jbh->b_data + sizeof(jbd2_journal_revoke_header_t);
            n_this = eh->nrevoked - rev_idx;
            if (n_this > revoke_per_block)
                n_this = revoke_per_block;

            for (k = 0; k < n_this; k++) {
                if (revoke_entry_sz == 8) {
                    *((__be64 *)p) = cpu_to_be64(eh->revoked[rev_idx + k]);
                    p += 8;
                } else {
                    *((__be32 *)p) = cpu_to_be32((__u32)eh->revoked[rev_idx + k]);
                    p += 4;
                }
            }
            rev_idx += n_this;

            rhdr->r_count = cpu_to_be32(sizeof(jbd2_journal_revoke_header_t) +
                                        n_this * revoke_entry_sz);

            set_buffer_dirty(jbh);
            mark_buffer_dirty(jbh);
            set_buffer_uptodate(jbh);
            err = submit_bh(WRITE, jbh);
            wbufs[nwbufs++] = jbh;
            if (err)
                goto out_wait_journal;
        }
    }

    err = journal_wait_for_writes(wbufs, nwbufs);
    nwbufs = 0;
    if (err)
        goto out_release;

    /* Make committed transaction discoverable before commit/home writes. */
    err = jbd2_journal_update_sb_log_tail(journal,
                                          journal->j_transaction_sequence,
                                          transaction_start,
                                          0);
    if (err)
        goto out_release;

    /* --- Phase 3: Write commit block --- */

    {
        struct commit_header *commit;
        LARGE_INTEGER li;

        jbh = sb_getblk_zero(eh->sb, (sector_t)commit_phys);
        if (!jbh) {
            err = -ENOMEM;
            goto out_release;
        }

        commit = (struct commit_header *)jbh->b_data;
        commit->h_magic = cpu_to_be32(JBD2_MAGIC_NUMBER);
        commit->h_blocktype = cpu_to_be32(JBD2_COMMIT_BLOCK);
        commit->h_sequence = cpu_to_be32(journal->j_transaction_sequence);
        commit->h_chksum_type = 0;
        commit->h_chksum_size = 0;
        RtlZeroMemory(commit->h_padding, sizeof(commit->h_padding));
        RtlZeroMemory(commit->h_chksum, sizeof(commit->h_chksum));

        KeQuerySystemTimePrecise(&li);
        commit->h_commit_sec = cpu_to_be64((__u64)(li.QuadPart / 10000000));
        commit->h_commit_nsec = cpu_to_be32((__u32)((li.QuadPart % 10000000) * 100));

        set_buffer_dirty(jbh);
        mark_buffer_dirty(jbh);
        set_buffer_uptodate(jbh);
        err = submit_bh(WRITE, jbh);
        wbufs[nwbufs++] = jbh;
        if (err)
            goto out_wait_journal;
    }

    /* --- Phase 4: Wait for commit I/O --- */

    err = journal_wait_for_writes(wbufs, nwbufs);
    nwbufs = 0;
    if (err)
        goto out_release;

    /* --- Phase 5: Update journal head in memory --- */

    journal->j_head = head;
    journal->j_free -= nblocks;
    journal->j_transaction_sequence++;

    /* --- Phase 6: Write originals to home locations --- */

    for (i = 0; i < nhome; i++) {
        err = sync_dirty_buffer(home_bufs[i]);
        if (err)
            goto out_release;
    }

    err = jbd2_journal_update_sb_log_tail(journal,
                                          journal->j_transaction_sequence,
                                          journal->j_head,
                                          0);
    if (!err) {
        /* All home writes flushed → entire committed range is checkpointed.
         * Reclaim journal log space: advance tail to head, restore j_free. */
        unsigned long freed = journal->j_head - journal->j_tail;
        if (journal->j_head < journal->j_tail)
            freed += journal->j_last - journal->j_first;
        journal->j_free += freed;
        journal->j_tail = journal->j_head;
        journal->j_tail_sequence = journal->j_transaction_sequence;
        journal->j_flags |= JBD2_FLUSHED;
    }

    goto out_release;

out_wait_journal:
    if (nwbufs)
        journal_wait_for_writes(wbufs, nwbufs);

out_release:
    mutex_unlock(&journal->j_checkpoint_mutex);
    return err;
}

/*
 * Drop the buffer_head references this handle pinned in
 * __ext4_handle_dirty_metadata (one get_bh per tracked buffer).
 *
 * Call AFTER the handle's buffers have been committed (or when discarding
 * an uncommitted handle), and NEVER while holding j_checkpoint_mutex:
 * put_bh -> __brelse acquires bd_bh_lock and may issue I/O.
 */
static void ext4_handle_release_buffers(struct ext4_handle *eh)
{
    int i;
    for (i = 0; i < eh->nbuffers; i++) {
        if (eh->buffers[i])
            put_bh(eh->buffers[i]);
    }
    eh->nbuffers = 0;
}

/* ==================== Public API ==================== */

static void Ext2FlushDirtyData(PEXT2_VCB Vcb);

handle_t *__ext4_journal_start_sb(void *icb, struct super_block *sb, unsigned int line,
                  int type, int blocks, int rsv_blocks)
{
    journal_t *journal;
    struct ext4_handle *eh;

    if (!sb) return &no_journal;

    journal = EXT4_SB(sb)->s_journal;
    if (!journal) return &no_journal;

    eh = (struct ext4_handle *)kmalloc(sizeof(*eh), GFP_KERNEL);
    if (!eh) {
        return (handle_t *)ERR_PTR(-ENOMEM);
    }

    RtlZeroMemory(eh, sizeof(*eh));
    eh->h.h_buffer_credits = blocks;
    eh->h.h_ref = 1;
    eh->journal = journal;
    eh->sb = sb;
    eh->nbuffers = 0;

    /* Mark handle as valid: set h_transaction to non-NULL non-small value */
    eh->h.h_transaction = (transaction_t *)((ULONG_PTR)1);

    return &eh->h;
}

int __ext4_journal_stop(const char *where, unsigned int line, void *icb, handle_t *handle)
{
    struct ext4_handle *eh;
    struct ext4_handle *pending;
    PEXT2_IRP_CONTEXT IrpContext;
    PEXT2_VCB Vcb;
    int err = 0;
    int i, j;

    if (!ext4_handle_valid(handle))
        return 0;

    eh = to_eh(handle);
    eh->h.h_ref--;

    if (eh->h.h_ref > 0)
        return 0;

    /* If no dirty buffers and no revokes, just free and return. */
    if (eh->nbuffers == 0 && eh->nrevoked == 0) {
        kfree(eh);
        return 0;
    }

    /*
     * Try to defer: merge into VCB-level pending handle instead of
     * committing synchronously.  The pending handle is committed in
     * bulk at flush / unmount time.
     */
    IrpContext = (PEXT2_IRP_CONTEXT)icb;
    Vcb = NULL;
    if (IrpContext && IrpContext->DeviceObject) {
        PDEVICE_OBJECT dev = IrpContext->DeviceObject;
        PEXT2_VCB maybe = (PEXT2_VCB)dev->DeviceExtension;
        if (maybe && maybe->Identifier.Type == EXT2VCB &&
            maybe->Identifier.Size == sizeof(EXT2_VCB))
            Vcb = maybe;
    }

    if (Vcb) {
        journal_t *journal = eh->journal;
        LARGE_INTEGER batchDue;

        mutex_lock(&journal->j_checkpoint_mutex);
        pending = (struct ext4_handle *)Vcb->PendingJournalHandle;

        /* Defensive ownership check: this handle is ALREADY the pending
         * handle.  That means __ext4_journal_stop reached the same eh twice
         * (double-stop / aliased IrpContext->Handle).  Merging or freeing it
         * here would leave Vcb->PendingJournalHandle pointing at freed heap,
         * and kjournald would free it a second time -> Ext2FreePool guard
         * corruption -> int 3 (BSOD 0x7E).  The handle is already owned by
         * the pending slot; drop this redundant stop and let kjournald
         * commit/free it exactly once. */
        if (pending == eh) {
            mutex_unlock(&journal->j_checkpoint_mutex);
            return 0;
        }

        if (pending == NULL) {
            /* Become the pending handle -- don't commit yet. */
            Vcb->PendingJournalHandle = (void *)eh;
            mutex_unlock(&journal->j_checkpoint_mutex);
            /* Arm batch timer: commit after BatchDelayMs */
            if (Vcb->KjournaldThread && !Vcb->BatchTimerArmed) {
                batchDue.QuadPart = (LONGLONG)-10 * 1000 * EXT2_BATCH_DELAY_MS;
                KeSetTimer(&Vcb->BatchTimer, batchDue, &Vcb->BatchDpc);
                Vcb->BatchTimerArmed = TRUE;
            }
            return 0;
        }

        if ((pending->nbuffers + eh->nbuffers) <= MAX_HANDLE_BUFFERS &&
            (pending->nrevoked + eh->nrevoked) <= MAX_HANDLE_REVOKES) {

            /* Merge buffers (deduplicated).  Each eh buffer carries one
             * get_bh reference: transfer it to pending for non-duplicates,
             * release it for duplicates (pending already holds a ref). */
            for (i = 0; i < eh->nbuffers; i++) {
                int dup = 0;
                for (j = 0; j < pending->nbuffers; j++) {
                    if (pending->buffers[j] == eh->buffers[i]) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup)
                    pending->buffers[pending->nbuffers++] = eh->buffers[i];
                else
                    put_bh(eh->buffers[i]);
            }
            /* References handled above; don't double-release on free. */
            eh->nbuffers = 0;

            /* Merge revokes */
            for (i = 0; i < eh->nrevoked; i++)
                pending->revoked[pending->nrevoked++] = eh->revoked[i];

            mutex_unlock(&journal->j_checkpoint_mutex);
            kfree(eh);
            /* Timer already armed from first stop — don't re-arm */
            return 0;
        }

        /* Pending handle full -- swap eh in as the new pending atomically
         * (single critical section) and commit the evicted one.  eh is a
         * fully-formed, finished handle (h_ref == 0), so it's safe for
         * kjournald to grab and commit it immediately if woken.
         *
         * Splitting this into "NULL out, unlock, ... , lock, install eh"
         * leaves a window where another __ext4_journal_stop can claim the
         * NULL slot; this swap then clobbers that handle, leaking it and
         * silently dropping its journal entries (corruption).
         *
         * Data=ordered: flush dirty file data before committing metadata
         * so that data blocks are on disk before metadata references them. */
        Vcb->PendingJournalHandle = (void *)eh;
        mutex_unlock(&journal->j_checkpoint_mutex);

        Ext2FlushDirtyData(Vcb);
        err = journal_commit_sync(pending);
        ext4_handle_release_buffers(pending);
        kfree(pending);

        /* Arm batch timer for the new pending handle */
        if (Vcb->KjournaldThread && !Vcb->BatchTimerArmed) {
            LARGE_INTEGER batchDue;
            batchDue.QuadPart = (LONGLONG)-10 * 1000 * EXT2_BATCH_DELAY_MS;
            KeSetTimer(&Vcb->BatchTimer, batchDue, &Vcb->BatchDpc);
            Vcb->BatchTimerArmed = TRUE;
        }
        return err;
    }

    /* No VCB context -- commit immediately (safety fallback). */
    err = journal_commit_sync(eh);
    ext4_handle_release_buffers(eh);
    kfree(eh);
    return err;
}

void ext4_journal_abort_handle(const char *caller, unsigned int line,
                   const char *err_fn, struct buffer_head *bh,
                   handle_t *handle, int err)
{
}

int __ext4_journal_get_write_access(const char *where, unsigned int line,
                    void *icb, handle_t *handle, struct buffer_head *bh)
{
    if (!ext4_handle_valid(handle))
        return 0;
    return 0;
}

int __ext4_forget(const char *where, unsigned int line, void *icb, handle_t *handle,
          int is_metadata, struct inode *inode,
          struct buffer_head *bh, ext4_fsblk_t blocknr)
{
    return 0;
}

int __ext4_journal_get_create_access(const char *where, unsigned int line,
                void *icb, handle_t *handle, struct buffer_head *bh)
{
    if (!ext4_handle_valid(handle))
        return 0;
    return 0;
}

int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
                 void *icb, handle_t *handle, struct inode *inode,
                 struct buffer_head *bh)
{
    struct ext4_handle *eh;
    int i;

    if (!ext4_handle_valid(handle)) {
        extents_mark_buffer_dirty(bh);
        return 0;
    }

    eh = to_eh(handle);

    /* Deduplicate: check if already tracked */
    for (i = 0; i < eh->nbuffers; i++) {
        if (eh->buffers[i] == bh) {
            extents_mark_buffer_dirty(bh);
            return 0;
        }
    }

    if (eh->nbuffers >= MAX_HANDLE_BUFFERS) {
        printk(KERN_ERR "Ext4Fsd: handle buffer overflow at %s:%d\n", where, line);
        return -ENOSPC;
    }

    /* Pin the buffer: deferred/pending handles outlive the caller's own
     * reference, which is dropped by brelse() right after this call.  Without
     * this get_bh the buffer's b_count can fall to 0, letting the bh reaper
     * free it before kjournald/overflow/flush commits the handle -- a
     * use-after-free in journal_commit_sync (reads bh->b_data / b_blocknr).
     * Released by ext4_handle_release_buffers() after commit. */
    get_bh(bh);
    eh->buffers[eh->nbuffers++] = bh;
    extents_mark_buffer_dirty(bh);
    return 0;
}

int __ext4_handle_dirty_super(const char *where, unsigned int line,
                  handle_t *handle, struct super_block *sb)
{
    return 0;
}

int __ext4_journal_revoke_block(handle_t *handle, ext4_fsblk_t blocknr)
{
    struct ext4_handle *eh;
    int i;

    if (!ext4_handle_valid(handle))
        return 0;

    eh = to_eh(handle);

    for (i = 0; i < eh->nrevoked; i++) {
        if (eh->revoked[i] == (__u64)blocknr)
            return 0;
    }

    if (eh->nrevoked >= MAX_HANDLE_REVOKES) {
        printk(KERN_ERR "Ext4Fsd: handle revoke overflow\n");
        return -ENOSPC;
    }

    eh->revoked[eh->nrevoked++] = (__u64)blocknr;
    return 0;
}

/*
 * Data=ordered: flush all dirty file data to disk.
 * Called BEFORE metadata journal commit — ensures data blocks
 * referenced by metadata are physically on disk, like Linux
 * ext4 data=ordered mode.  Without this, a crash after metadata
 * commit could leave metadata pointing to stale/unwritten data.
 *
 * Safe to call from any context (kjournald, overflow path, flush).
 * No filesystem locks required — CcFlushCache is self-synchronizing.
 */
static void
Ext2FlushDirtyData(PEXT2_VCB Vcb)
{
    PEXT2_FCB Fcb;
    PLIST_ENTRY ListEntry;

    if (IsVcbReadOnly(Vcb))
        return;

    ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);

    for (ListEntry = Vcb->FcbList.Flink;
         ListEntry != &Vcb->FcbList;
         ListEntry = ListEntry->Flink) {

        Fcb = CONTAINING_RECORD(ListEntry, EXT2_FCB, Next);

        if (IsDirectory(Fcb))
            continue;
        if (IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING))
            continue;
        if (Fcb->SectionObject.DataSectionObject == NULL)
            continue;
        /* Ordered mode only needs files whose data was actually written
         * since the last flush; flushing every open FCB on every commit
         * costs a synchronous CcFlushCache per file.  FCB_FILE_MODIFIED is
         * set by Ext2WriteFile/SetInformation before their metadata reaches
         * a journal handle, so a commit can never see the metadata without
         * also seeing the flag.  Deliberately NOT cleared here: the clear
         * points (flush/cleanup/purge) do their own CcFlushCache first. */
        if (!IsFlagOn(Fcb->Flags, FCB_FILE_MODIFIED))
            continue;

        CcFlushCache(&Fcb->SectionObject, NULL, 0, NULL);
    }

    ExReleaseResourceLite(&Vcb->FcbLock);
}

/*
 * Flush and free any pending deferred journal handle on this VCB.
 * Called at explicit flush, volume purge, and VCB destroy time.
 * Must be called while the caller holds appropriate VCB locks.
 */
void Ext2JournalFlushPending(PEXT2_VCB Vcb)
{
    struct ext4_handle *pending;
    journal_t *journal;

    if (!Vcb)
        return;

    journal = EXT4_SB(&Vcb->sb)->s_journal;
    if (!journal)
        return;

    mutex_lock(&journal->j_checkpoint_mutex);
    pending = (struct ext4_handle *)Vcb->PendingJournalHandle;
    if (pending) {
        Vcb->PendingJournalHandle = NULL;
    }
    mutex_unlock(&journal->j_checkpoint_mutex);

    if (pending) {
        /* Data=ordered: flush file data before metadata commit */
        Ext2FlushDirtyData(Vcb);
        journal_commit_sync(pending);
        ext4_handle_release_buffers(pending);
        kfree(pending);
    }
}

/*
 * Batch DPC: fires after BatchDelayMs from the first ext4_journal_stop.
 * Signals kjournald to commit whatever accumulated in the pending handle.
 */
static VOID
Ext2BatchDpc(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2)
{
    PEXT2_VCB Vcb = (PEXT2_VCB)DeferredContext;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    Vcb->BatchTimerArmed = FALSE;
    KeSetEvent(&Vcb->KjournaldWake, 0, FALSE);
}

/*
 * kjournald -- dedicated journal commit thread per mounted volume.
 *
 * Wakes on:
 * - Batch timer (5ms after first stop) — batches burst writes
 * - Force-commit signal (fsync/flush) — immediate commit
 * - Safety-net timeout (10s) — catches stragglers
 *
 * Commits the pending deferred journal handle in bulk.
 * No MainResource or global lock is held during commit.
 */

static VOID
Ext2KjournaldThread(PVOID Context)
{
    PEXT2_VCB Vcb = (PEXT2_VCB)Context;
    journal_t *journal;
    LARGE_INTEGER Timeout;
    struct ext4_handle *pending;

    journal = EXT4_SB(&Vcb->sb)->s_journal;
    if (!journal)
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);

    Timeout.QuadPart = (LONGLONG)-10 * 1000 * 1000 * EXT2_COMMIT_INTERVAL_SECONDS;

    while (!Vcb->KjournaldStop) {

        KeWaitForSingleObject(
            &Vcb->KjournaldWake,
            Executive,
            KernelMode,
            FALSE,
            &Timeout
        );

        KeClearEvent(&Vcb->KjournaldWake);

        if (Vcb->KjournaldStop)
            break;

        /* Swap out the pending handle under lock */
        mutex_lock(&journal->j_checkpoint_mutex);
        pending = (struct ext4_handle *)Vcb->PendingJournalHandle;
        if (pending) {
            Vcb->PendingJournalHandle = NULL;
        }
        mutex_unlock(&journal->j_checkpoint_mutex);

        if (pending) {
            /* Data=ordered: flush file data before metadata commit */
            Ext2FlushDirtyData(Vcb);
            journal_commit_sync(pending);
            ext4_handle_release_buffers(pending);
            kfree(pending);
        }

        /* Signal any waiter (fsync/FlushFileBuffers) that commit is done */
        KeSetEvent(&Vcb->KjournaldDone, 0, FALSE);
    }

    /* Final flush on shutdown */
    Ext2JournalFlushPending(Vcb);

    KeSetEvent(&Vcb->KjournaldDone, 0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
Ext2StartKjournald(PEXT2_VCB Vcb)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    HANDLE handle = NULL;
    LARGE_INTEGER timeout;
    LARGE_INTEGER batchDelay;

    KeInitializeEvent(&Vcb->KjournaldWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Vcb->KjournaldDone, SynchronizationEvent, FALSE);
    Vcb->KjournaldStop = FALSE;
    Vcb->KjournaldThread = NULL;
    Vcb->BatchTimerArmed = FALSE;

    KeInitializeTimer(&Vcb->BatchTimer);
    KeInitializeDpc(&Vcb->BatchDpc, Ext2BatchDpc, (PVOID)Vcb);

    InitializeObjectAttributes(&oa, NULL, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = PsCreateSystemThread(
                 &handle,
                 0,
                 &oa,
                 NULL,
                 NULL,
                 Ext2KjournaldThread,
                 (PVOID)Vcb);

    if (!NT_SUCCESS(status)) {
        DEBUG(DL_ERR, ("Ext4Fsd: failed to start kjournald: %xh\n", status));
        return status;
    }

    ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, NULL, KernelMode,
                              (PVOID *)&Vcb->KjournaldThread, NULL);
    ZwClose(handle);

    /* Wait for thread to enter its loop (up to 2s) */
    timeout.QuadPart = (LONGLONG)-10 * 1000 * 1000 * 2;
    KeWaitForSingleObject(&Vcb->KjournaldDone, Executive, KernelMode, FALSE, &timeout);

    DEBUG(DL_INF, ("Ext4Fsd: kjournald started (commit interval=%us, batch delay=%ums)\n",
                   EXT2_COMMIT_INTERVAL_SECONDS, EXT2_BATCH_DELAY_MS));
    return STATUS_SUCCESS;
}

VOID
Ext2StopKjournald(PEXT2_VCB Vcb)
{
    LARGE_INTEGER timeout;

    if (!Vcb->KjournaldThread)
        return;

    /* Cancel any pending batch timer */
    KeCancelTimer(&Vcb->BatchTimer);
    Vcb->BatchTimerArmed = FALSE;

    Vcb->KjournaldStop = TRUE;
    KeSetEvent(&Vcb->KjournaldWake, 0, FALSE);

    /* Wait up to 15s for thread to exit */
    timeout.QuadPart = (LONGLONG)-10 * 1000 * 1000 * 15;
    KeWaitForSingleObject(Vcb->KjournaldThread, Executive, KernelMode, FALSE, &timeout);

    ObDereferenceObject(Vcb->KjournaldThread);
    Vcb->KjournaldThread = NULL;

    DEBUG(DL_INF, ("Ext4Fsd: kjournald stopped\n"));
}

/*
 * Force an immediate journal commit for this volume.
 * Called from fsync / FlushFileBuffers / flush IRP paths.
 * Cancels the batch timer and wakes kjournald immediately.
 */
NTSTATUS
Ext2JournalForceCommit(PEXT2_VCB Vcb)
{
    journal_t *journal;
    LARGE_INTEGER timeout;

    if (!Vcb)
        return STATUS_SUCCESS;

    journal = EXT4_SB(&Vcb->sb)->s_journal;
    if (!journal)
        return STATUS_SUCCESS;

    /* If no pending handle, nothing to commit.  Read under the checkpoint
     * mutex so we observe a consistent view of the slot rather than racing
     * a concurrent swap in __ext4_journal_stop / kjournald. */
    mutex_lock(&journal->j_checkpoint_mutex);
    if (!Vcb->PendingJournalHandle) {
        mutex_unlock(&journal->j_checkpoint_mutex);
        return STATUS_SUCCESS;
    }
    mutex_unlock(&journal->j_checkpoint_mutex);

    /* Cancel batch timer — we're committing now */
    KeCancelTimer(&Vcb->BatchTimer);
    Vcb->BatchTimerArmed = FALSE;

    /* Wake kjournald */
    KeSetEvent(&Vcb->KjournaldWake, 0, FALSE);

    /* Wait for commit to finish (up to 30s) */
    timeout.QuadPart = (LONGLONG)-10 * 1000 * 1000 * 30;
    KeWaitForSingleObject(&Vcb->KjournaldDone, Executive, KernelMode, FALSE, &timeout);

    return STATUS_SUCCESS;
}
