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
    IO_STATUS_BLOCK     IoStatus = {0};
    NTSTATUS            Status;

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
            }
        }

        if (IsDirectory(Fcb)) {
            IoStatus.Status = STATUS_SUCCESS;
            __leave;
        }

        DEBUG(DL_INF, ( "Ext2FlushFile: Flushing File Inode=%xh %S ...\n",
                        Fcb->Inode->i_ino, Fcb->Mcb->ShortName.Buffer));

        CcFlushCache(&(Fcb->SectionObject), NULL, 0, &IoStatus);
        ClearFlag(Fcb->Flags, FCB_FILE_MODIFIED);

        if (IsFlagOn(Fcb->Flags, FCB_ALLOC_IN_WRITE)) {
            Ext2SaveInode(IrpContext, Fcb->Vcb, Fcb->Inode);
            ClearFlag(Fcb->Flags, FCB_ALLOC_IN_WRITE);
        }

        /*
         * Force-commit the pending journal handle so that the metadata
         * we just saved (inode, extent tree, etc.) reaches disk before
         * the caller's fsync / FlushFileBuffers returns.  Without this,
         * the deferred commit by kjournald (10s interval) would leave a
         * window where a crash loses the just-written metadata.
         * A failed/timed-out commit must fail the fsync: the caller's
         * durability contract was not met.
         */
        Status = Ext2JournalForceCommit(Fcb->Vcb);
        if (!NT_SUCCESS(Status)) {
            IoStatus.Status = Status;
        }

        /*
         * NOTE: do NOT flush the whole volume here.  Ext2FlushFile runs once
         * per open file (e.g. Ext2FlushFiles iterates every Fcb at shutdown /
         * dismount); a per-file full-volume CcFlushCache would mean N
         * full-volume flushes and stalls system shutdown.  The deferred
         * journal commit and the single full-volume metadata flush are done
         * once at the volume level (Ext2FlushVolume -> Ext2FlushVcb), which
         * runs right after the per-file pass; the inode home blocks saved
         * above reach disk there.
         */

    } __finally {

        /* do cleanup here */
    }

    return IoStatus.Status;
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
        Ext2FlushFile(IrpContext, Fcb, NULL);
        ExReleaseResourceLite(&Fcb->MainResource);

        Ext2ReleaseFcb(Fcb);
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
