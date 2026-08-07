/*
 * COPYRIGHT:        See COPYRIGHT.TXT
 * PROJECT:          Ext2 File System Driver for WinNT/2K/XP
 * FILE:             flush.c
 * PROGRAMMER:       Matt Wu <mattwu@163.com>
 * HOMEPAGE:         http://www.ext2fsd.com
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ext2fs.h"

/* GLOBALS ***************************************************************/

extern PEXT2_GLOBAL Ext2Global;

/* DEFINITIONS *************************************************************/

NTSTATUS
Ext2FlushCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context  )

{
    if (Irp->PendingReturned)
        IoMarkIrpPending( Irp );

    if (Irp->IoStatus.Status == STATUS_INVALID_DEVICE_REQUEST)
        Irp->IoStatus.Status = STATUS_SUCCESS;

    return STATUS_SUCCESS;
}

/*
 * Flush file data + optional mtime update.  When CommitJournal is FALSE the
 * caller will force one journal commit after flushing many files (bulk path).
 */
static NTSTATUS
Ext2FlushFileInternal (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_CCB            Ccb,
    IN BOOLEAN              CommitJournal
)
{
    IO_STATUS_BLOCK     IoStatus = {0};
    NTSTATUS            Status;
    BOOLEAN             MetadataDirty = FALSE;

    ASSERT(Fcb != NULL);
    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    __try {

        /* do nothing if target fie was deleted */
        if (FlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
            IoStatus.Status = STATUS_FILE_DELETED;
            __leave;
        }

        /* update timestamp and achieve attribute */
        if (Ccb != NULL) {

            if (!IsFlagOn(Ccb->Flags, CCB_LAST_WRITE_UPDATED)) {

                LARGE_INTEGER   SysTime;
                KeQuerySystemTimePrecise(&SysTime);

                Ext2SetInodeTime(&SysTime, &Fcb->Inode->i_mtime, &Fcb->Inode->i_mtime_extra);
                Fcb->Mcb->LastWriteTime = Ext2GetInodeTime(Fcb->Inode->i_mtime, Fcb->Inode->i_mtime_extra);
                Ext2SaveInode(IrpContext, Fcb->Vcb, Fcb->Inode);
                MetadataDirty = TRUE;
            }
        }

        if (IsDirectory(Fcb)) {
            IoStatus.Status = STATUS_SUCCESS;
            /* Directory metadata (mtime) may still need a journal commit. */
            if (CommitJournal && MetadataDirty) {
                Status = Ext2JournalForceCommit(Fcb->Vcb);
                if (!NT_SUCCESS(Status)) {
                    IoStatus.Status = Status;
                }
            }
            __leave;
        }

        DEBUG(DL_INF, ( "Ext2FlushFile: Flushing File Inode=%xh %S ...\n",
                        Fcb->Inode->i_ino, Fcb->Mcb->ShortName.Buffer));

        /*
         * Prefer ordered dirty ranges (data=ordered) over full-file flush.
         * Explorer copy + multi-flush then only rewrites recently dirtied
         * windows instead of the entire growing target file each time.
         * Do not clear FCB_FILE_MODIFIED until force-commit succeeds so a
         * failed journal barrier still sees the file as ordered-dirty.
         */
        Status = Ext2FlushFcbOrderedData(Fcb);
        if (!NT_SUCCESS(Status)) {
            IoStatus.Status = Status;
            __leave;
        }
        IoStatus.Status = STATUS_SUCCESS;

        if (IsFlagOn(Fcb->Flags, FCB_ALLOC_IN_WRITE)) {
            Ext2SaveInode(IrpContext, Fcb->Vcb, Fcb->Inode);
            ClearFlag(Fcb->Flags, FCB_ALLOC_IN_WRITE);
            MetadataDirty = TRUE;
        }

        /*
         * Per-file path: always force-commit so data=ordered homes and any
         * metadata from this flush become durable.  Bulk Ext2FlushFiles sets
         * CommitJournal=FALSE and issues one Ext2JournalForceCommit after
         * all files; ranges stay empty and FCB_FILE_MODIFIED sticks until
         * that barrier succeeds (caller must clear on success).
         */
        if (CommitJournal) {
            Status = Ext2JournalForceCommit(Fcb->Vcb);
            if (!NT_SUCCESS(Status)) {
                IoStatus.Status = Status;
                __leave;
            }
            Ext2ResetOrderedDirtyRanges(Fcb);
            ClearFlag(Fcb->Flags, FCB_FILE_MODIFIED);
        } else if (!MetadataDirty) {
            /* Bulk path: data is on disk; leave MODIFIED until volume commit. */
            ;
        }

    } __finally {

        /* do cleanup here */
    }

    return IoStatus.Status;
}

NTSTATUS
Ext2FlushVolume (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN BOOLEAN              bShutDown
)
{
    DEBUG(DL_INF, ( "Ext2FlushVolume: Flushing Vcb ...\n"));

    ExAcquireSharedStarveExclusive(&Vcb->PagingIoResource, TRUE);
    ExReleaseResourceLite(&Vcb->PagingIoResource);

    return Ext2FlushVcb(Vcb);
}

NTSTATUS
Ext2FlushFile (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_CCB            Ccb
)
{
    return Ext2FlushFileInternal(IrpContext, Fcb, Ccb, TRUE);
}

NTSTATUS
Ext2FlushFiles(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN BOOLEAN              bShutDown
)
{
    IO_STATUS_BLOCK    IoStatus;

    PEXT2_FCB       Fcb;
    PLIST_ENTRY     ListEntry;
    LIST_ENTRY      FlushList;
    PEXT2_FLUSH_FCB_ENTRY Entry;
    NTSTATUS        Status;

    if (IsVcbReadOnly(Vcb)) {
        return STATUS_SUCCESS;
    }

    IoStatus.Status = STATUS_SUCCESS;
    InitializeListHead(&FlushList);

    DEBUG(DL_INF, ( "Flushing Files ...\n"));

    /* Snapshot FcbList under FcbLock, then flush without holding it. */
    ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
    for (ListEntry = Vcb->FcbList.Flink;
         ListEntry != &Vcb->FcbList;
         ListEntry = ListEntry->Flink ) {

        Entry = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_FLUSH_FCB_ENTRY),
                                 EXT2_FLIST_MAGIC);
        if (!Entry) {
            IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        Fcb = CONTAINING_RECORD(ListEntry, EXT2_FCB, Next);
        Ext2ReferXcb(&Fcb->ReferenceCount);
        Entry->Fcb = Fcb;
        InsertTailList(&FlushList, &Entry->Link);
    }
    ExReleaseResourceLite(&Vcb->FcbLock);

    while (!IsListEmpty(&FlushList)) {

        ListEntry = RemoveHeadList(&FlushList);
        Entry = CONTAINING_RECORD(ListEntry, EXT2_FLUSH_FCB_ENTRY, Link);
        Fcb = Entry->Fcb;
        Ext2FreePool(Entry, EXT2_FLIST_MAGIC);

        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        /* No per-file journal force — one commit after the loop. */
        Status = Ext2FlushFileInternal(IrpContext, Fcb, NULL, FALSE);
        if (NT_SUCCESS(IoStatus.Status) && !NT_SUCCESS(Status)) {
            IoStatus.Status = Status;
        }
        ExReleaseResourceLite(&Fcb->MainResource);

        Ext2ReleaseFcb(Fcb);
    }

    /* Single journal barrier for the whole bulk flush (shutdown/dismount). */
    Status = Ext2JournalForceCommit(Vcb);
    if (NT_SUCCESS(IoStatus.Status) && !NT_SUCCESS(Status)) {
        IoStatus.Status = Status;
    }

    /* Clear ordered state only after the volume commit barrier succeeds. */
    if (NT_SUCCESS(Status)) {
        ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
        for (ListEntry = Vcb->FcbList.Flink;
             ListEntry != &Vcb->FcbList;
             ListEntry = ListEntry->Flink) {

            Fcb = CONTAINING_RECORD(ListEntry, EXT2_FCB, Next);
            if (IsDirectory(Fcb))
                continue;
            Ext2ResetOrderedDirtyRanges(Fcb);
            ClearFlag(Fcb->Flags, FCB_FILE_MODIFIED);
        }
        ExReleaseResourceLite(&Vcb->FcbLock);
    }

    return IoStatus.Status;
}

NTSTATUS
Ext2Flush (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS                Status = STATUS_SUCCESS;

    PIRP                    Irp = NULL;
    PIO_STACK_LOCATION      IrpSp = NULL;

    PEXT2_VCB               Vcb = NULL;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_FCBVCB            FcbOrVcb = NULL;
    PEXT2_CCB               Ccb = NULL;
    PFILE_OBJECT            FileObject = NULL;

    PDEVICE_OBJECT          DeviceObject = NULL;

    BOOLEAN                 MainResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        //
        // This request is not allowed on the main device object
        //
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        ASSERT(IsMounted(Vcb));
        if (IsVcbReadOnly(Vcb)) {
            Status =  STATUS_SUCCESS;
            __leave;
        }

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        FileObject = IrpContext->FileObject;
        FcbOrVcb = (PEXT2_FCBVCB) FileObject->FsContext;
        ASSERT(FcbOrVcb != NULL);

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Ccb == NULL) {
            Status =  STATUS_SUCCESS;
            __leave;
        }

        MainResourceAcquired =
            ExAcquireResourceExclusiveLite(&FcbOrVcb->MainResource, TRUE);
        ASSERT(MainResourceAcquired);
        DEBUG(DL_INF, ("Ext2Flush-pre:  total mcb records=%u\n",
                       FsRtlNumberOfRunsInLargeMcb(&Vcb->Extents)));

        if (FcbOrVcb->Identifier.Type == EXT2VCB) {

            Ext2VerifyVcb(IrpContext, Vcb);
            Status = Ext2FlushFiles(IrpContext, (PEXT2_VCB)(FcbOrVcb), FALSE);

            if (NT_SUCCESS(Status)) {
                Status = Ext2FlushVolume(IrpContext, (PEXT2_VCB)(FcbOrVcb), FALSE);
            }

            if (NT_SUCCESS(Status) && IsFlagOn(Vcb->Volume->Flags, FO_FILE_MODIFIED)) {
                ClearFlag(Vcb->Volume->Flags, FO_FILE_MODIFIED);
            }

        } else if (FcbOrVcb->Identifier.Type == EXT2FCB) {

            Fcb = (PEXT2_FCB)(FcbOrVcb);

            Status = Ext2FlushFile(IrpContext, Fcb, Ccb);
            if (NT_SUCCESS(Status)) {
                if (IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {
                    Fcb->Mcb->FileAttr |= FILE_ATTRIBUTE_ARCHIVE;
                    ClearFlag(FileObject->Flags, FO_FILE_MODIFIED);
                }
            }
        }

        DEBUG(DL_INF, ("Ext2Flush-post: total mcb records=%u\n",
                       FsRtlNumberOfRunsInLargeMcb(&Vcb->Extents)));

    } __finally {

        if (MainResourceAcquired) {
            ExReleaseResourceLite(&FcbOrVcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {

            if (Vcb && Irp && IrpSp && !IsVcbReadOnly(Vcb)) {

                // Call the disk driver to flush the physial media.
                NTSTATUS DriverStatus;
                PIO_STACK_LOCATION NextIrpSp;

                NextIrpSp = IoGetNextIrpStackLocation(Irp);

                *NextIrpSp = *IrpSp;

                IoSetCompletionRoutine( Irp,
                                        Ext2FlushCompletionRoutine,
                                        NULL,
                                        TRUE,
                                        TRUE,
                                        TRUE );

                DriverStatus = IoCallDriver(Vcb->TargetDeviceObject, Irp);

                Status = (DriverStatus == STATUS_INVALID_DEVICE_REQUEST) ?
                         Status : DriverStatus;

                IrpContext->Irp = Irp = NULL;
            }

            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}
