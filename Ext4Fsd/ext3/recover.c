/*
 * COPYRIGHT:        See COPYRIGHT.TXT
 * PROJECT:          Ext2 File System Driver for WinNT/2K/XP
 * FILE:             recover.c
 * PROGRAMMER:       Matt Wu <mattwu@163.com>
 * HOMEPAGE:         http://www.ext2fsd.com
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include <ext2fs.h>
#include <linux/jbd2.h>
#include "linux/ext4_jbd2.h"

/* GLOBALS ***************************************************************/

extern PEXT2_GLOBAL Ext2Global;

/* DEFINITIONS *************************************************************/

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2LoadInternalJournal)
#pragma alloc_text(PAGE, Ext2CheckJournal)
#pragma alloc_text(PAGE, Ext2RecoverJournal)
#endif

PEXT2_MCB
Ext2LoadInternalJournal(
    PEXT2_VCB         Vcb,
    ULONG             jNo
)
{
    PEXT2_MCB   Jcb = NULL;

    Jcb = Ext2AllocateMcb(Vcb, NULL, NULL, 0);
    if (!Jcb) {
        goto errorout;
    }

    Jcb->Inode.i_ino = jNo;
    Jcb->Inode.i_sb = &Vcb->sb;
    if (!Ext2LoadInode(Vcb, &Jcb->Inode)) {
        DbgBreak();
        Ext2FreeMcb(Vcb, Jcb);
        goto errorout;
    }

errorout:

    return Jcb;
}

INT
Ext2CheckJournal(
    PEXT2_VCB          Vcb,
    PULONG             jNo
)
{
    struct ext3_super_block* esb = NULL;

    /* check ext3 super block */
    esb = (struct ext3_super_block *)Vcb->SuperBlock;
    if (IsFlagOn(esb->s_feature_incompat,
                 EXT3_FEATURE_INCOMPAT_RECOVER)) {
        SetLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
    }

    /* must stop here if volume is read-only */
    if (IsVcbReadOnly(Vcb)) {
        goto errorout;
    }

    /* journal is external ? */
    if (esb->s_journal_inum == 0) {
        goto errorout;
    }

    /* oops: volume is corrupted */
    if (esb->s_journal_dev) {
        goto errorout;
    }

    /* return the journal inode number */
    *jNo = esb->s_journal_inum;

    return TRUE;

errorout:

    return FALSE;
}

INT
Ext2RecoverJournal(
    PEXT2_IRP_CONTEXT  IrpContext,
    PEXT2_VCB          Vcb
)
{
    INT rc = 0;
    ULONG                   jNo = 0;
    PEXT2_MCB               jcb = NULL;
    struct block_device *   bd = &Vcb->bd;
    struct super_block *    sb = &Vcb->sb;
    struct inode *          ji = NULL;
    journal_t *             journal = NULL;
    struct ext3_super_block *esb;

    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);

    /* check journal inode number */
    if (!Ext2CheckJournal(Vcb, &jNo)) {
        rc = -1;
        goto errorout;
    }

    /* allocate journal Mcb */
    jcb =  Ext2LoadInternalJournal(Vcb, jNo);
    if (!jcb) {
        rc = -6;
        goto errorout;
    }

    /* allocate journal inode */
    ji = &jcb->Inode;

    /* initialize journal file from inode */
    journal = jbd2_journal_init_inode(ji);

    /* initialzation succeeds ? */
    if (!journal) {
        DEBUG(DL_ERR, ( "jbd2_journal_init_inode failed\n"));
        iput(ji);
        rc = -8;
        goto errorout;
    }

	if (ext4_has_feature_journal_needs_recovery(sb)) {
        /* loading the journal will do a recover */
		rc = jbd2_journal_load(journal);

        if (0 != rc) {
            DEBUG(DL_ERR, ( "Ext2Fsd: recover_journal: failed "
                 "to recover journal data. rc=%d\n", rc));
            rc = -9;
            //goto errorout;
        }

        /* reload super_block and group_description */
        Ext2RefreshSuper(IrpContext, Vcb);
        Ext2RefreshGroup(IrpContext, Vcb);

        /* clear recover flag in sb */
        if (rc == 0) {
            ClearLongFlag(
                Vcb->SuperBlock->s_feature_incompat,
                EXT3_FEATURE_INCOMPAT_RECOVER);
            Ext2SaveSuper(IrpContext, Vcb);
            sync_blockdev(bd);
            ClearLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
        }
    }
    else {
        /* if the journal is clean wipe it */
		rc = jbd2_journal_wipe(journal, !IsVcbReadOnly(Vcb));
    }

errorout:

    if (rc == 0 && journal) {
        EXT4_SB(sb)->s_journal = journal;
        EXT4_SB(sb)->s_journal_mcb = jcb;
    } else {
        if (journal) {
            jbd2_journal_destroy(journal);
        }
        if (jcb) {
            Ext2FreeMcb(Vcb, jcb);
        }
    }

    ExReleaseResourceLite(&Vcb->MainResource);
    return rc;
}

/*
 * Open a JBD2 transaction on the IRP context.
 *
 * If the volume has no journal (ext2 / mount failed to load journal),
 * this is a no-op that returns STATUS_SUCCESS — callers can still call
 * Ext2DirtyMetadata, which falls back to direct mark_buffer_dirty.
 *
 * Asserts no transaction is already active. For nested call sites use
 * Ext2JournalNestedStart instead.
 */
NTSTATUS
Ext2JournalStart(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Blocks
)
{
    handle_t *handle;

    if (!IrpContext)
        return STATUS_SUCCESS;

    ASSERT(IrpContext->Handle == NULL);

    if (!Vcb || !EXT4_SB(&Vcb->sb)->s_journal) {
        /* no journal — leave Handle NULL, callers fall back to legacy path */
        return STATUS_SUCCESS;
    }

    handle = ext4_journal_start_sb(IrpContext, &Vcb->sb, 0, (int)Blocks);
    if (IS_ERR(handle)) {
        DEBUG(DL_ERR, ("Ext2JournalStart: failed rc=%d\n", (int)PTR_ERR(handle)));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IrpContext->Handle = handle;
    return STATUS_SUCCESS;
}

/*
 * Commit and close the active JBD2 transaction on this IRP context.
 * Safe no-op if no handle is active.
 */
NTSTATUS
Ext2JournalStop(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    int err;

    UNREFERENCED_PARAMETER(Vcb);

    if (!IrpContext || !IrpContext->Handle)
        return STATUS_SUCCESS;

    err = ext4_journal_stop(IrpContext, (handle_t *)IrpContext->Handle);
    IrpContext->Handle = NULL;

    if (err == -EIO)
        return STATUS_UNEXPECTED_IO_ERROR;
    if (err == -ENOSPC)
        return STATUS_DISK_FULL;
    if (err)
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

/*
 * Conditional start: opens a transaction only if none is already active.
 * Returns TRUE if this call opened the transaction (caller must Stop).
 * Returns FALSE if a transaction was already active (caller must NOT Stop).
 *
 * Use this in callees that may be invoked both from journaled top-level
 * paths and from standalone (no outer transaction) paths.
 */
BOOLEAN
Ext2JournalNestedStart(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Blocks
)
{
    if (!IrpContext)
        return FALSE;

    if (IrpContext->Handle)
        return FALSE;

    if (!NT_SUCCESS(Ext2JournalStart(IrpContext, Vcb, Blocks)))
        return FALSE;

    return IrpContext->Handle != NULL;
}

/*
 * Record a revoke for the given block in the active transaction.
 * Safe no-op if no transaction is active or no journal present.
 * The revoke prevents log replay from overwriting the block with
 * stale metadata after it is freed and reused.
 */
VOID
Ext2JournalRevokeBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN ULONGLONG            BlockNr
)
{
    if (!IrpContext || !IrpContext->Handle)
        return;

    (void)ext4_journal_revoke_block((handle_t *)IrpContext->Handle,
                                    (ext4_fsblk_t)BlockNr);
}

/* ============================================================
 *  Orphan inode list — runtime maintenance + mount-time replay
 * ============================================================
 *
 * On-disk: SB->s_last_orphan = head inode#; chain linked via
 * inode->i_dtime (NEXT_ORPHAN). Terminator: i_dtime == 0.
 *
 * Inserted at unlink-start (before truncate / inode free) so a
 * crash mid-delete leaves a discoverable list. Mount-time
 * replay walks chain, finishes each pending unlink, splices
 * out, persists SB.
 *
 * Concurrency: callers hold Vcb->MainResource (or have already
 * serialized via FS top-level locks). No internal lock here.
 *
 * No-op if no journal (best effort; without journal we can't
 * make the SB+inode update atomic anyway).
 */

static BOOLEAN
Ext2OrphanLoadInode(
    IN PEXT2_VCB            Vcb,
    IN ULONG                Ino,
    OUT struct inode *      Inode
)
{
    RtlZeroMemory(Inode, sizeof(*Inode));
    Inode->i_ino = Ino;
    Inode->i_sb  = &Vcb->sb;
    return Ext2LoadInode(Vcb, Inode);
}

NTSTATUS
Ext2OrphanAdd(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN struct inode *       Inode
)
{
    ULONG OldHead;

    if (!Vcb || !Inode || Inode->i_ino == 0)
        return STATUS_SUCCESS;

    if (!EXT4_SB(&Vcb->sb)->s_journal)
        return STATUS_SUCCESS;

    /* idempotent: if already on chain (NEXT_ORPHAN nonzero or head==self), skip */
    OldHead = le32_to_cpu(Vcb->SuperBlock->s_last_orphan);
    if (OldHead == Inode->i_ino)
        return STATUS_SUCCESS;

    /*
     * Repurpose i_dtime as NEXT_ORPHAN. Caller must NOT have set
     * deletion time yet — that happens after OrphanDel.
     */
    Inode->i_dtime = OldHead;
    Vcb->SuperBlock->s_last_orphan = cpu_to_le32(Inode->i_ino);

    Ext2SaveInode(IrpContext, Vcb, Inode);
    Ext2SaveSuper(IrpContext, Vcb);

    DEBUG(DL_INF, ("Ext2OrphanAdd: ino=%xh next=%xh\n",
                   Inode->i_ino, OldHead));
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2OrphanDel(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN struct inode *       Inode
)
{
    ULONG ino;
    ULONG cur;
    ULONG guard = 0;
    struct inode prev;

    if (!Vcb || !Inode || Inode->i_ino == 0)
        return STATUS_SUCCESS;

    if (!EXT4_SB(&Vcb->sb)->s_journal)
        return STATUS_SUCCESS;

    ino = Inode->i_ino;
    cur = le32_to_cpu(Vcb->SuperBlock->s_last_orphan);
    if (cur == 0)
        return STATUS_SUCCESS;

    if (cur == ino) {
        Vcb->SuperBlock->s_last_orphan = cpu_to_le32(Inode->i_dtime);
        Ext2SaveSuper(IrpContext, Vcb);
        Inode->i_dtime = 0;
        DEBUG(DL_INF, ("Ext2OrphanDel: ino=%xh (head)\n", ino));
        return STATUS_SUCCESS;
    }

    /* walk chain in search of predecessor */
    while (cur != 0 && cur != ino && guard++ < 0x10000) {
        if (!Ext2OrphanLoadInode(Vcb, cur, &prev)) {
            DEBUG(DL_ERR, ("Ext2OrphanDel: load %xh failed\n", cur));
            break;
        }
        if (prev.i_dtime == ino) {
            prev.i_dtime = Inode->i_dtime;
            Ext2SaveInode(IrpContext, Vcb, &prev);
            Inode->i_dtime = 0;
            DEBUG(DL_INF, ("Ext2OrphanDel: ino=%xh (after %xh)\n",
                           ino, prev.i_ino));
            return STATUS_SUCCESS;
        }
        cur = prev.i_dtime;
    }

    /* not on list — paranoia: just clear our pointer */
    Inode->i_dtime = 0;
    DEBUG(DL_WRN, ("Ext2OrphanDel: ino=%xh not on chain\n", ino));
    return STATUS_SUCCESS;
}

/*
 * Mount-time replay. Called after Ext2RecoverJournal has
 * replayed the log. Walks s_last_orphan chain, finishing each
 * unfinished delete (i_nlink == 0).
 *
 * MVP: only handles i_nlink == 0 case (truncate-orphan with
 * nlink>0 is not yet emitted by this driver). For each entry
 * we open a top-level transaction, build a transient MCB to
 * reuse Ext2TruncateFile (frees blocks + extents/indirects),
 * then free the inode. SB pointer advanced and persisted per
 * entry so partial replay across crash is safe.
 *
 * No-op for read-only mounts and for volumes without a journal.
 */
NTSTATUS
Ext2ProcessOrphanList(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    ULONG       ino;
    ULONG       processed = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    if (!Vcb || IsVcbReadOnly(Vcb))
        return STATUS_SUCCESS;

    if (!EXT4_SB(&Vcb->sb)->s_journal)
        return STATUS_SUCCESS;

    while ((ino = le32_to_cpu(Vcb->SuperBlock->s_last_orphan)) != 0
            && processed < 0x10000) {

        PEXT2_MCB   Mcb = NULL;
        ULONG       next;
        BOOLEAN     OwnsTxn = FALSE;
        LARGE_INTEGER Zero;

        Mcb = Ext2AllocateMcb(Vcb, NULL, NULL, 0);
        if (!Mcb) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        Mcb->Inode.i_ino = ino;
        Mcb->Inode.i_sb  = &Vcb->sb;
        if (!Ext2LoadInode(Vcb, &Mcb->Inode)) {
            DEBUG(DL_ERR, ("Ext2ProcessOrphanList: load %xh failed\n", ino));
            Ext2FreeMcb(Vcb, Mcb);
            /* break chain to avoid loop */
            Vcb->SuperBlock->s_last_orphan = 0;
            Ext2SaveSuper(IrpContext, Vcb);
            break;
        }

        next = Mcb->Inode.i_dtime;

        OwnsTxn = Ext2JournalNestedStart(IrpContext, Vcb, 64);

        DEBUG(DL_INF, ("Ext2ProcessOrphanList: ino=%xh nlink=%u next=%xh\n",
                       ino, Mcb->Inode.i_nlink, next));

        if (Mcb->Inode.i_nlink == 0) {
            /* finish unfinished delete */
            Zero.QuadPart = 0;
            (void)Ext2TruncateFile(IrpContext, Vcb, Mcb, &Zero);
            /* clear NEXT_ORPHAN before final SaveInode so dtime
               serializes as deletion time, not chain pointer */
            Mcb->Inode.i_dtime = 0;
            Ext2SaveInode(IrpContext, Vcb, &Mcb->Inode);
            Ext2FreeInode(IrpContext, Vcb, ino,
                          S_ISDIR(Mcb->Inode.i_mode) ? EXT2_FT_DIR
                                                     : EXT2_FT_REG_FILE);
        } else {
            /* truncate-orphan path — not emitted by this driver yet,
               but tolerate by just splicing out */
            DEBUG(DL_WRN, ("Ext2ProcessOrphanList: ino=%xh nlink>0 skipping truncate\n", ino));
        }

        /* splice head -> next, persist */
        Vcb->SuperBlock->s_last_orphan = cpu_to_le32(next);
        Ext2SaveSuper(IrpContext, Vcb);

        if (OwnsTxn)
            Ext2JournalStop(IrpContext, Vcb);

        Ext2FreeMcb(Vcb, Mcb);
        processed++;
    }

    if (processed)
        DEBUG(DL_INF, ("Ext2ProcessOrphanList: replayed %u entries\n", processed));

    return Status;
}
