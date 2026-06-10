/*
 * COPYRIGHT:        See COPYRIGHT.TXT
 * PROJECT:          Ext2 File System Driver for WinNT/2K/XP
 * FILE:             block.c
 * PROGRAMMER:       Matt Wu <mattwu@163.com>
 * HOMEPAGE:         http://www.ext2fsd.com
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ext2fs.h"

/* IOCTL_DISK_FLUSH_CACHE — flush the disk device's volatile write cache to
 * stable media.  Not defined in older ntdddisk.h; the value below matches
 * the WDK definition (IOCTL_DISK_BASE, function 0x0029). */
#ifndef IOCTL_DISK_FLUSH_CACHE
#define IOCTL_DISK_FLUSH_CACHE CTL_CODE(IOCTL_DISK_BASE, 0x0029, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

/* GLOBALS ***************************************************************/

extern PEXT2_GLOBAL Ext2Global;

/* DEFINITIONS *************************************************************/

NTSTATUS
Ext2ReadWriteBlockSyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context    );

NTSTATUS
Ext2ReadWriteBlockAsyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2LockUserBuffer)
#pragma alloc_text(PAGE, Ext2ReadSync)
#pragma alloc_text(PAGE, Ext2WriteSync)
#pragma alloc_text(PAGE, Ext2ReadDisk)
#pragma alloc_text(PAGE, Ext2DiskIoControl)
#pragma alloc_text(PAGE, Ext2DiskShutDown)
#endif

/* FUNCTIONS ***************************************************************/

PMDL
Ext2CreateMdl (
    IN PVOID Buffer,
    IN ULONG Length,
    IN LOCK_OPERATION op
)
{
    NTSTATUS Status;
    PMDL Mdl = NULL;

    ASSERT (Buffer != NULL);
    Mdl = IoAllocateMdl (Buffer, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        __try {
            if (MmIsNonPagedSystemAddressValid(Buffer)) {
                MmBuildMdlForNonPagedPool(Mdl);
            } else {
                MmProbeAndLockPages(Mdl, KernelMode, op);
            }
            Status = STATUS_SUCCESS;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoFreeMdl (Mdl);
            Mdl = NULL;
            DbgBreak();
            Status = STATUS_INVALID_USER_BUFFER;
        }
    }
    return Mdl;
}

VOID
Ext2DestroyMdl (IN PMDL Mdl)
{
    ASSERT (Mdl != NULL);
    while (Mdl) {
        PMDL Next;
        Next = Mdl->Next;
        Mdl->Next = NULL;
        if (IsFlagOn(Mdl->MdlFlags, MDL_PAGES_LOCKED)) {
            MmUnlockPages (Mdl);
        }
        IoFreeMdl (Mdl);
        Mdl = Next;
    }
}

NTSTATUS
Ext2LockUserBuffer (IN PIRP     Irp,
                    IN ULONG            Length,
                    IN LOCK_OPERATION   Operation)
{
    NTSTATUS Status;
    ASSERT(Irp != NULL);

    if (Irp->MdlAddress != NULL) {
        return STATUS_SUCCESS;
    }

    IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);
    if (Irp->MdlAddress == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        MmProbeAndLockPages(Irp->MdlAddress, Irp->RequestorMode, Operation);
        Status = STATUS_SUCCESS;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        DbgBreak();
        IoFreeMdl(Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        Status = STATUS_INVALID_USER_BUFFER;
    }

    return Status;
}

PVOID
Ext2GetUserBuffer (IN PIRP Irp )
{
    ASSERT(Irp != NULL);

    if (Irp->MdlAddress) {

#if (_WIN32_WINNT >= 0x0500)
        return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
#else
        return MmGetSystemAddressForMdl(Irp->MdlAddress);
#endif
    } else {

        return Irp->UserBuffer;
    }
}

NTSTATUS
Ext2ReadWriteBlockSyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context    )
{
    PEXT2_RW_CONTEXT pContext = (PEXT2_RW_CONTEXT)Context;

    if (Irp != pContext->MasterIrp) {

        if (!NT_SUCCESS(Irp->IoStatus.Status)) {
            pContext->MasterIrp->IoStatus = Irp->IoStatus;
        }

        IoFreeMdl(Irp->MdlAddress);
        IoFreeIrp(Irp );
    }

    if (InterlockedDecrement(&pContext->Blocks) == 0) {

        pContext->MasterIrp->IoStatus.Information = 0;
        if (NT_SUCCESS(pContext->MasterIrp->IoStatus.Status)) {

            pContext->MasterIrp->IoStatus.Information =
                pContext->Length;
        }

        KeSetEvent(&pContext->Event, 0, FALSE);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
Ext2ReadWriteBlockAsyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
)
{
    PEXT2_RW_CONTEXT pContext = (PEXT2_RW_CONTEXT)Context;
    PIO_STACK_LOCATION iosp;

    ASSERT(FALSE == pContext->Wait);

    if (Irp != pContext->MasterIrp) {
        if (!NT_SUCCESS(Irp->IoStatus.Status)) {
            pContext->MasterIrp->IoStatus = Irp->IoStatus;
        }
        if (Irp->MdlAddress != NULL) {
            IoFreeMdl(Irp->MdlAddress);
            Irp->MdlAddress = NULL;
        }
        /* Do NOT IoFreeIrp(Irp) here — the I/O Manager frees associated IRPs
         * automatically as part of decrementing MasterIrp->AssociatedIrp.IrpCount
         * when we return STATUS_SUCCESS.  Calling IoFreeIrp here caused a
         * double-free that corrupted the IRP and triggered bugcheck 0x44
         * (MULTIPLE_IRP_COMPLETE_REQUESTS) via CLASSPNP/storport. */
    }

    if (InterlockedDecrement(&pContext->Blocks) == 0) {

        if (NT_SUCCESS(pContext->MasterIrp->IoStatus.Status)) {

            /* set written bytes to status information */
            pContext->MasterIrp->IoStatus.Information = pContext->Length;

            if (pContext->FileObject != NULL && !IsFlagOn(pContext->MasterIrp->Flags, IRP_PAGING_IO)) {

                /* modify FileObject flags, skip this for volume direct access */
                SetFlag( pContext->FileObject->Flags,
                         IsFlagOn(pContext->Flags, EXT2_RW_CONTEXT_WRITE) ?
                         FO_FILE_MODIFIED : FO_FILE_FAST_IO_READ);

                /* update Current Byteoffset */
                if (IsFlagOn(pContext->FileObject->Flags, FO_SYNCHRONOUS_IO)) {
                    iosp = IoGetCurrentIrpStackLocation(pContext->MasterIrp);
                    pContext->FileObject->CurrentByteOffset.QuadPart =
                        iosp->Parameters.Read.ByteOffset.QuadPart +  pContext->Length;
                }
            }

        } else {

            pContext->MasterIrp->IoStatus.Information = 0;
        }

        /* release the locked resource acquired by the caller */
        if (pContext->Resource) {
            ExReleaseResourceForThread(pContext->Resource, pContext->ThreadId);
        }

        Ext2FreePool(pContext, EXT2_RWC_MAGIC);
        DEC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
    }

    return STATUS_SUCCESS;
}

/*
 * Chunk size for parallel I/O submission.
 * Splitting large contiguous extents into 256 KB pieces lets the NVMe
 * controller keep multiple commands in flight simultaneously instead of
 * serialising at queue-depth 1.
 */
#define EXT2_RW_CHUNK_SIZE  (256 * 1024)

NTSTATUS
Ext2ReadWriteBlocks(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_EXTENT         Chain,
    IN ULONG                Length
    )
{
    PIRP                MasterIrp = IrpContext->Irp;
    PIO_STACK_LOCATION  IrpSp;
    PMDL                Mdl;
    PEXT2_RW_CONTEXT    pContext = NULL;
    PEXT2_EXTENT        Extent;
    NTSTATUS            Status = STATUS_SUCCESS;
    BOOLEAN             bMasterCompleted = FALSE;
    BOOLEAN             bBugCheck = FALSE;
    BOOLEAN             bCanWait;

    PIRP               *ChunkIrps = NULL;
    ULONG               MaxChunks = 0;
    ULONG               nChunks = 0;
    ULONG               i;

    PMDL                SrcMdl;
    PUCHAR              SrcVa;
    ULONG               SrcBytes;

    ASSERT(MasterIrp);

    __try {

        pContext = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_RW_CONTEXT), EXT2_RWC_MAGIC);

        if (!pContext) {
            DEBUG(DL_ERR, ( "Ex2ReadWriteBlocks: failed to allocate pContext.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        INC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
        RtlZeroMemory(pContext, sizeof(EXT2_RW_CONTEXT));
        bCanWait = Ext2CanIWait();
        pContext->Wait = bCanWait;
        pContext->MasterIrp = MasterIrp;
        pContext->Length = Length;
        MasterIrp->IoStatus.Status = STATUS_SUCCESS;
        MasterIrp->IoStatus.Information = 0;

        /*
         * The partial MDLs we build below describe sub-ranges of the master
         * IRP's MDL.  Capture the source MDL's base VA and byte count up front
         * so we can (a) use the MDL's own virtual address as the base for
         * IoBuildPartialMdl (which is what it validates against, not
         * Irp->UserBuffer) and (b) reject any extent range that would fall
         * outside the locked buffer.  Without this check an over-range extent
         * makes IoBuildPartialMdl bugcheck 0x12E (INVALID_MDL_RANGE).
         */
        SrcMdl = MasterIrp->MdlAddress;
        if (SrcMdl == NULL) {
            DEBUG(DL_ERR, ( "Ext2ReadWriteBlocks: master IRP has no MDL.\n"));
            DbgBreak();
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }
        SrcVa    = (PUCHAR)MmGetMdlVirtualAddress(SrcMdl);
        SrcBytes = MmGetMdlByteCount(SrcMdl);

        if (IrpContext->MajorFunction == IRP_MJ_WRITE) {
            SetFlag(pContext->Flags, EXT2_RW_CONTEXT_WRITE);
        }

        if (pContext->Wait) {

            KeInitializeEvent(&(pContext->Event), NotificationEvent, FALSE);

        } else if (IrpContext->Fcb->Identifier.Type == EXT2FCB) {

            if (IsFlagOn(MasterIrp->Flags, IRP_PAGING_IO)) {
                pContext->Resource = &IrpContext->Fcb->PagingIoResource;
            } else {
                pContext->Resource = &IrpContext->Fcb->MainResource;
            }

            pContext->FileObject = IrpContext->FileObject;
            pContext->ThreadId = ExGetCurrentResourceThread();
        }

        for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {
            MaxChunks += (Extent->Length + EXT2_RW_CHUNK_SIZE - 1) / EXT2_RW_CHUNK_SIZE;
        }

        if (MaxChunks == 0) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        ChunkIrps = Ext2AllocatePool(NonPagedPool,
                                     sizeof(PIRP) * MaxChunks,
                                     'RI2E');
        if (!ChunkIrps) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
        RtlZeroMemory(ChunkIrps, sizeof(PIRP) * MaxChunks);

        /*
         * Phase 1: build all associated IRPs for every chunk of every extent.
         *
         * We always use associated IRPs, even for a single contiguous extent,
         * so that all chunks can be submitted before blocking on the event.
         * This keeps the NVMe command queue deep instead of serialising at
         * queue-depth 1 (the old single-IRP-then-wait approach).
         *
         * All IRPs are collected in ChunkIrps[].  IrpCount and pContext->Blocks
         * are set AFTER the build loop, before any submission, to avoid the
         * race where a fast completion fires before IrpCount is initialised.
         */
        for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {

            ULONG   ExtentDone   = 0;
            ULONG   ExtentRemain = Extent->Length;

            while (ExtentRemain > 0) {

                PIRP    ChunkIrp;
                ULONG   ChunkLen    = ExtentRemain;
                ULONG   ChunkOffset = Extent->Offset + ExtentDone;
                LONGLONG ChunkLba   = Extent->Lba    + ExtentDone;

                if (ChunkLen > EXT2_RW_CHUNK_SIZE)
                    ChunkLen = EXT2_RW_CHUNK_SIZE;

                /*
                 * Guard against an extent whose offset/length would describe
                 * memory outside the master IRP's locked buffer.  Building a
                 * partial MDL past the end of the source MDL is a fatal driver
                 * error (bugcheck 0x12E, INVALID_MDL_RANGE), so turn it into a
                 * recoverable failure instead of crashing the box.
                 */
                if ((ULONGLONG)ChunkOffset + ChunkLen > SrcBytes) {
                    DEBUG(DL_ERR, ( "Ext2ReadWriteBlocks: chunk [%u,+%u) exceeds "
                                    "source MDL of %u bytes.\n",
                                    ChunkOffset, ChunkLen, SrcBytes));
                    DbgBreak();
                    Status = STATUS_INVALID_PARAMETER;
                    __leave;
                }

                if (nChunks >= MaxChunks) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                ChunkIrp = IoMakeAssociatedIrp(
                               MasterIrp,
                               (CCHAR)(Vcb->TargetDeviceObject->StackSize + 1) );

                if (!ChunkIrp) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                Mdl = IoAllocateMdl( SrcVa + ChunkOffset,
                                     ChunkLen,
                                     FALSE,
                                     FALSE,
                                     ChunkIrp );

                if (!Mdl) {
                    IoFreeIrp(ChunkIrp);
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                IoBuildPartialMdl( SrcMdl,
                                   Mdl,
                                   SrcVa + ChunkOffset,
                                   ChunkLen );

                IoSetNextIrpStackLocation(ChunkIrp);
                IrpSp = IoGetCurrentIrpStackLocation(ChunkIrp);

                IrpSp->MajorFunction = IrpContext->MajorFunction;
                IrpSp->Parameters.Read.Length = ChunkLen;
                IrpSp->Parameters.Read.ByteOffset.QuadPart = ChunkLba;

                IoSetCompletionRoutine(
                    ChunkIrp,
                    bCanWait ?
                    Ext2ReadWriteBlockSyncCompletionRoutine :
                    Ext2ReadWriteBlockAsyncCompletionRoutine,
                    (PVOID) pContext,
                    TRUE,
                    TRUE,
                    TRUE );

                IrpSp = IoGetNextIrpStackLocation(ChunkIrp);

                IrpSp->MajorFunction = IrpContext->MajorFunction;
                IrpSp->Parameters.Read.Length = ChunkLen;
                IrpSp->Parameters.Read.ByteOffset.QuadPart = ChunkLba;

                if (IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {
                    SetFlag( IrpSp->Flags, SL_WRITE_THROUGH );
                }
                if (IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_VERIFY_READ)) {
                    SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);
                }

                ChunkIrps[nChunks++] = ChunkIrp;

                ExtentDone   += ChunkLen;
                ExtentRemain -= ChunkLen;
            }
        }

        /*
         * Phase 2: set IrpCount and Blocks atomically before any submission.
         *
         * IrpCount must be set before the first IoCallDriver so that a
         * fast-completing chunk cannot decrement it below zero and trigger a
         * spurious master-IRP completion.
         *
         * For bCanWait we add 1 to keep the master alive until we call
         * KeWaitForSingleObject (the sync completion routine fires the event
         * when Blocks reaches 0, which is fine, but we need IrpCount to stay
         * positive until after our wait returns; the extra +1 is decremented
         * implicitly by the I/O Manager when we return from this function and
         * the caller completes the master).
         */
        pContext->Blocks = nChunks;
        MasterIrp->AssociatedIrp.IrpCount = nChunks;
        if (bCanWait) {
            MasterIrp->AssociatedIrp.IrpCount += 1;
        }

        if (!bCanWait) {
            IoMarkIrpPending(pContext->MasterIrp);
        }

        bBugCheck = TRUE;

        /*
         * Phase 3: submit all chunk IRPs to the device in one pass.
         * After this point the completion routines own the IRPs.
         */
        for (i = 0; i < nChunks; i++) {
            Status = IoCallDriver(Vcb->TargetDeviceObject, ChunkIrps[i]);
            ChunkIrps[i] = NULL;   /* owned by completion routine now */
        }

        if (bCanWait) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject( &(pContext->Event),
                                   Executive, KernelMode, FALSE, &Timeout );
            if (Status == STATUS_TIMEOUT) {
                MasterIrp->IoStatus.Status = STATUS_IO_TIMEOUT;
                MasterIrp->IoStatus.Information = 0;
                KeWaitForSingleObject( &(pContext->Event),
                                       Executive, KernelMode, FALSE, NULL );
            }
            KeClearEvent( &(pContext->Event) );
        } else {
            bMasterCompleted = TRUE;
        }

    } __finally {

        /* Free any chunk IRPs that were not yet submitted (build-phase failure) */
        if (ChunkIrps != NULL) {
            for (i = 0; i < nChunks; i++) {
                if (ChunkIrps[i] != NULL) {
                    if (ChunkIrps[i]->MdlAddress != NULL) {
                        IoFreeMdl(ChunkIrps[i]->MdlAddress);
                    }
                    IoFreeIrp(ChunkIrps[i]);
                }
            }
        }

        if (IrpContext->ExceptionInProgress) {

            if (bBugCheck) {
                Ext2BugCheck(EXT2_BUGCHK_BLOCK, 0, 0, 0);
            }

        } else {

            if (bCanWait) {
                if (MasterIrp) {
                    Status = MasterIrp->IoStatus.Status;
                }
                if (pContext) {
                    Ext2FreePool(pContext, EXT2_RWC_MAGIC);
                    DEC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
                }
            } else {
                if (bMasterCompleted) {
                    IrpContext->Irp = NULL;
                    Status = STATUS_PENDING;
                } else if (pContext) {
                    Ext2FreePool(pContext, EXT2_RWC_MAGIC);
                    DEC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
                }
            }
        }

        if (ChunkIrps != NULL) {
            Ext2FreePool(ChunkIrps, 'RI2E');
        }
    }

    return Status;
}

NTSTATUS
Ext2ReadSync(
    IN PEXT2_VCB        Vcb,
    IN ULONGLONG        Offset,
    IN ULONG            Length,
    OUT PVOID           Buffer,
    IN BOOLEAN          bVerify
)
{
    PKEVENT         Event = NULL;

    PIRP            Irp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        Status = STATUS_INSUFFICIENT_RESOURCES;


    ASSERT(Vcb != NULL);
    ASSERT(Vcb->TargetDeviceObject != NULL);
    ASSERT(Buffer != NULL);

    __try {

        Event = Ext2AllocatePool(NonPagedPool, sizeof(KEVENT), 'EK2E');

        if (NULL == Event) {
            DEBUG(DL_ERR, ( "Ex2ReadSync: failed to allocate Event.\n"));
            __leave;
        }

        INC_MEM_COUNT(PS_DISK_EVENT, Event, sizeof(KEVENT));

        KeInitializeEvent(Event, NotificationEvent, FALSE);

        Irp = IoBuildSynchronousFsdRequest(
                  IRP_MJ_READ,
                  Vcb->TargetDeviceObject,
                  Buffer,
                  Length,
                  (PLARGE_INTEGER)(&Offset),
                  Event,
                  &IoStatus
              );

        if (!Irp) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        if (bVerify) {
            SetFlag( IoGetNextIrpStackLocation(Irp)->Flags,
                     SL_OVERRIDE_VERIFY_VOLUME );
        }

        Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject(
                Event,
                Executive,
                KernelMode,
                FALSE,
                &Timeout
            );

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(Irp);
                KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
                Status = STATUS_IO_TIMEOUT;
            } else {
                Status = IoStatus.Status;
            }
        }

    } __finally {

        if (Event) {
            Ext2FreePool(Event, 'EK2E');
            DEC_MEM_COUNT(PS_DISK_EVENT, Event, sizeof(KEVENT));
        }
    }

    return Status;
}

NTSTATUS
Ext2ReadDisk(
    IN PEXT2_VCB   Vcb,
    IN ULONGLONG   Offset,
    IN ULONG       Size,
    IN PVOID       Buffer,
    IN BOOLEAN     bVerify )
{
    NTSTATUS    Status;
    PUCHAR      Buf;
    ULONG       Length;
    ULONGLONG   Lba;

    Lba = Offset & (~((ULONGLONG)SECTOR_SIZE - 1));
    Length = (ULONG)(Size + Offset + SECTOR_SIZE - 1 - Lba) &
             (~((ULONG)SECTOR_SIZE - 1));

    Buf = Ext2AllocatePool(PagedPool, Length, EXT2_DATA_MAGIC);
    if (!Buf) {
        DEBUG(DL_ERR, ( "Ext2ReadDisk: failed to allocate Buffer.\n"));
        Status = STATUS_INSUFFICIENT_RESOURCES;

        goto errorout;
    }
    INC_MEM_COUNT(PS_DISK_BUFFER, Buf, Length);

    Status = Ext2ReadSync(  Vcb,
                            Lba,
                            Length,
                            Buf,
                            bVerify );

    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_ERR, ("Ext2ReadDisk: disk i/o error: %xh.\n", Status));
        goto errorout;
    }

    RtlCopyMemory(Buffer, &Buf[Offset - Lba], Size);

errorout:

    if (Buf) {
        Ext2FreePool(Buf, EXT2_DATA_MAGIC);
        DEC_MEM_COUNT(PS_DISK_BUFFER, Buf, Length);
    }

    return Status;
}

/*
 * Synchronous, non-cached write of a buffer straight to the underlying disk
 * at a byte Offset.  Mirror of Ext2ReadSync.  This is the real-I/O primitive
 * the journal and metadata writeback use so durability does NOT depend on the
 * Cache Manager lazy writer.  Offset and Length must be sector aligned.
 */
NTSTATUS
Ext2WriteSync(
    IN PEXT2_VCB        Vcb,
    IN ULONGLONG        Offset,
    IN ULONG            Length,
    IN PVOID            Buffer
)
{
    PAGED_CODE();
    PKEVENT         Event = NULL;
    PIRP            Irp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        Status = STATUS_INSUFFICIENT_RESOURCES;

    ASSERT(Vcb != NULL);
    ASSERT(Vcb->TargetDeviceObject != NULL);
    ASSERT(Buffer != NULL);

    if (IsVcbReadOnly(Vcb))
        return STATUS_MEDIA_WRITE_PROTECTED;

    __try {

        Event = Ext2AllocatePool(NonPagedPool, sizeof(KEVENT), 'EK2E');
        if (NULL == Event) {
            DEBUG(DL_ERR, ( "Ext2WriteSync: failed to allocate Event.\n"));
            __leave;
        }
        INC_MEM_COUNT(PS_DISK_EVENT, Event, sizeof(KEVENT));
        KeInitializeEvent(Event, NotificationEvent, FALSE);

        Irp = IoBuildSynchronousFsdRequest(
                  IRP_MJ_WRITE,
                  Vcb->TargetDeviceObject,
                  Buffer,
                  Length,
                  (PLARGE_INTEGER)(&Offset),
                  Event,
                  &IoStatus
              );

        if (!Irp) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject(
                Event, Executive, KernelMode, FALSE, &Timeout);
            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(Irp);
                KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
                Status = STATUS_IO_TIMEOUT;
            } else {
                Status = IoStatus.Status;
            }
        }

    } __finally {

        if (Event) {
            Ext2FreePool(Event, 'EK2E');
            DEC_MEM_COUNT(PS_DISK_EVENT, Event, sizeof(KEVENT));
        }
    }

    return Status;
}

/*
 * Force the underlying disk to flush its volatile write cache to stable media.
 * This is the ordering barrier used by the journal commit: after the journal
 * blocks (or the commit block) are written, this guarantees they are durable
 * before the next phase proceeds.  Best-effort: a device that does not support
 * the IOCTL is treated as success (such devices generally have no volatile
 * cache, or honour FUA semantics on their own).
 */
NTSTATUS
Ext2FlushDiskCache(
    IN PEXT2_VCB        Vcb )
{
    NTSTATUS Status;

    if (IsVcbReadOnly(Vcb))
        return STATUS_SUCCESS;

    Status = Ext2DiskIoControl(
                 Vcb->TargetDeviceObject,
                 IOCTL_DISK_FLUSH_CACHE,
                 NULL, 0,
                 NULL, NULL );

    if (Status == STATUS_INVALID_DEVICE_REQUEST ||
        Status == STATUS_NOT_SUPPORTED) {
        Status = STATUS_SUCCESS;
    }

    return Status;
}

NTSTATUS
Ext2DiskIoControl (
    IN PDEVICE_OBJECT   DeviceObject,
    IN ULONG            IoctlCode,
    IN PVOID            InputBuffer,
    IN ULONG            InputBufferSize,
    IN OUT PVOID        OutputBuffer,
    IN OUT PULONG       OutputBufferSize)
{
    ULONG           OutBufferSize = 0;
    KEVENT          Event;
    PIRP            Irp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        Status;

    ASSERT(DeviceObject != NULL);

    if (OutputBufferSize)
    {
        OutBufferSize = *OutputBufferSize;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(
              IoctlCode,
              DeviceObject,
              InputBuffer,
              InputBufferSize,
              OutputBuffer,
              OutBufferSize,
              FALSE,
              &Event,
              &IoStatus
          );

    if (Irp == NULL) {
        DEBUG(DL_ERR, ( "Ext2DiskIoControl: failed to build Irp!\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(DeviceObject, Irp);

    if (Status == STATUS_PENDING)  {
        LARGE_INTEGER Timeout;
        Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout);
        if (Status == STATUS_TIMEOUT) {
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = STATUS_IO_TIMEOUT;
        } else {
            Status = IoStatus.Status;
        }
    }

    if (OutputBufferSize) {
        *OutputBufferSize = (ULONG)(IoStatus.Information);
    }

    return Status;
}

NTSTATUS
Ext2DiskShutDown(PEXT2_VCB Vcb)
{
    PIRP                Irp;
    KEVENT              Event;

    NTSTATUS            Status;
    IO_STATUS_BLOCK     IoStatus;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_SHUTDOWN,
                                       Vcb->TargetDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &IoStatus);

    if (Irp) {
        Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject(&Event,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  &Timeout);

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(Irp);
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
                Status = STATUS_IO_TIMEOUT;
            } else {
                Status = IoStatus.Status;
            }
        }
    } else  {
        Status = IoStatus.Status;
    }

    return Status;
}
