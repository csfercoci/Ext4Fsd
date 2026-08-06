/*
 * COPYRIGHT:        See COPYRIGHT.TXT
 * PROJECT:          Ext2 File System Driver for WinNT/2K/XP
 * FILE:             misc.c
 * PROGRAMMER:       Matt Wu <mattwu@163.com>
 * HOMEPAGE:         http://www.ext2fsd.com
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ext2fs.h"

/* GLOBALS ***************************************************************/

extern PEXT2_GLOBAL Ext2Global;

/* DEFINITIONS *************************************************************/

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2Sleep)
#endif

ULONG
Ext2Log2(ULONG Value)
{
    ULONG Order = 0;

    ASSERT(Value > 0);

    while (Value) {
        Order++;
        Value >>= 1;
    }

    return (Order - 1);
}

/* The built in functions RtlTimeToSecondsSince1970 and RtlSecondsSince1970ToTime is
 * not "year 2038 safe" because they use 32-bit seconds. The new functions
 * Ext2TimeToSecondsSince1970 and Ext2SecondsSince1970ToTime is a reimplementation
 * that uses 64-bit seconds.
 * 
 * The superblock has new time fields ending in "_hi" containing the high 8-bit of
 * the seconds and the existing time fields contain the lower 32-bit.
 * 
 * The inodes has new time fields ending in "_extra". They contain both the high
 * 2-bit of the seconds, that is bit 33 and 34 and also the nano seconds encoded
 * as (nsec << 2 | epoch) The existing fields contain the low 32-bit of the seconds.
 * 
 * Windows system time is in number of 100 nano seconds.
*/

VOID
Ext2TimeToSecondsSince1970(
    IN PLARGE_INTEGER SysTime,
    OUT PULONG SecondsSince1970LowPart,
    OUT PULONG SecondsSince1970HighPart)
{
    LARGE_INTEGER LinuxTime;

    LinuxTime.QuadPart = SysTime->QuadPart / (ULONGLONG)TICKSPERSEC - SECS_1601_TO_1970;

    *SecondsSince1970LowPart = LinuxTime.LowPart;
    *SecondsSince1970HighPart = LinuxTime.HighPart;
}

VOID
Ext2SecondsSince1970ToTime(
    IN ULONG SecondsSince1970LowPart,
    IN ULONG SecondsSince1970HighPart,
    OUT PLARGE_INTEGER SysTime)
{
    SysTime->QuadPart = (((LONGLONG)SecondsSince1970HighPart << 32) + SecondsSince1970LowPart) * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
}

VOID
Ext2SetInodeTime(
    IN PLARGE_INTEGER SysTime,
    OUT PULONG i_time,
    OUT PULONG i_time_extra)
{
    LARGE_INTEGER LinuxTime;
    ULONG epoch, nano_sec;

    Ext2TimeToSecondsSince1970(SysTime, &LinuxTime.LowPart, &LinuxTime.HighPart);
    epoch = ((LinuxTime.QuadPart - (signed int)LinuxTime.QuadPart) >> 32) & EXT4_EPOCH_MASK;
    nano_sec = (SysTime->QuadPart % (ULONGLONG)TICKSPERSEC) * 100;
    /* i_time is lower 32-bit and i_time_extra is (nsec << 2 | epoch) */
    *i_time = LinuxTime.LowPart;
    *i_time_extra = (epoch | (nano_sec << EXT4_EPOCH_BITS));
}

LARGE_INTEGER
Ext2GetInodeTime(
    IN ULONG i_time,
    IN ULONG i_time_extra)
{
    LARGE_INTEGER LinuxTime, SysTime;
    ULONG epoch, nano_sec;

    /* i_time is lower 32-bit and i_time_extra is (nsec << 2 | epoch) */
    epoch = i_time_extra & EXT4_EPOCH_MASK;
    nano_sec = (i_time_extra & EXT4_NSEC_MASK) >> EXT4_EPOCH_BITS;
    LinuxTime.QuadPart = (signed)i_time + ((ULONGLONG)epoch << 32);
    Ext2SecondsSince1970ToTime(LinuxTime.LowPart, LinuxTime.HighPart, &SysTime);
    SysTime.QuadPart += nano_sec / 100;

    return SysTime;
}

static BOOLEAN
Ext2IsUtf8(struct nls_table *PageTable)
{
    return PageTable && !strcmp(PageTable->charset, "utf8");
}

static ULONG
Ext2Utf8Decode(IN const UCHAR *Input, IN ULONG InputLength, OUT PULONG CodePoint)
{
    ULONG Value;
    ULONG Length;
    ULONG Minimum;

    if (InputLength == 0) {
        return 0;
    }

    if (Input[0] < 0x80) {
        *CodePoint = Input[0];
        return 1;
    }

    if (Input[0] >= 0xc2 && Input[0] <= 0xdf) {
        Value = Input[0] & 0x1f;
        Length = 2;
        Minimum = 0x80;
    } else if (Input[0] >= 0xe0 && Input[0] <= 0xef) {
        Value = Input[0] & 0x0f;
        Length = 3;
        Minimum = 0x800;
    } else if (Input[0] >= 0xf0 && Input[0] <= 0xf4) {
        Value = Input[0] & 0x07;
        Length = 4;
        Minimum = 0x10000;
    } else {
        return 0;
    }

    if (InputLength < Length) {
        return 0;
    }

    while (--Length) {
        Input++;
        if ((*Input & 0xc0) != 0x80) {
            return 0;
        }
        Value = (Value << 6) | (*Input & 0x3f);
    }

    if ((Value < Minimum) ||
        (Value >= 0xd800 && Value <= 0xdfff) ||
        Value > 0x10ffff) {
        return 0;
    }

    *CodePoint = Value;
    return (Value < 0x800) ? 2 : ((Value < 0x10000) ? 3 : 4);
}

static ULONG
Ext2Utf8Encode(IN ULONG CodePoint, OUT PUCHAR Output)
{
    if (CodePoint < 0x80) {
        if (Output) {
            Output[0] = (UCHAR)CodePoint;
        }
        return 1;
    }
    if (CodePoint < 0x800) {
        if (Output) {
            Output[0] = (UCHAR)(0xc0 | (CodePoint >> 6));
            Output[1] = (UCHAR)(0x80 | (CodePoint & 0x3f));
        }
        return 2;
    }
    if (CodePoint < 0x10000) {
        if (Output) {
            Output[0] = (UCHAR)(0xe0 | (CodePoint >> 12));
            Output[1] = (UCHAR)(0x80 | ((CodePoint >> 6) & 0x3f));
            Output[2] = (UCHAR)(0x80 | (CodePoint & 0x3f));
        }
        return 3;
    }
    if (CodePoint <= 0x10ffff) {
        if (Output) {
            Output[0] = (UCHAR)(0xf0 | (CodePoint >> 18));
            Output[1] = (UCHAR)(0x80 | ((CodePoint >> 12) & 0x3f));
            Output[2] = (UCHAR)(0x80 | ((CodePoint >> 6) & 0x3f));
            Output[3] = (UCHAR)(0x80 | (CodePoint & 0x3f));
        }
        return 4;
    }
    return 0;
}

static ULONG
Ext2Utf8ToUnicode(IN OUT PUNICODE_STRING Unicode, IN PANSI_STRING Mbs)
{
    ULONG i = 0;
    ULONG Length = 0;
    ULONG CodePoint;
    ULONG CharLength;

    while (i < Mbs->Length) {
        CharLength = Ext2Utf8Decode((PUCHAR)Mbs->Buffer + i,
                                    Mbs->Length - i, &CodePoint);
        if (CharLength == 0) {
            return 0;
        }
        Length += (CodePoint < 0x10000) ? sizeof(WCHAR) : 2 * sizeof(WCHAR);
        i += CharLength;
    }

    if (Unicode) {
        if (Unicode->MaximumLength < Length) {
            return 0;
        }
        Unicode->Length = 0;
        i = 0;
        while (i < Mbs->Length) {
            CharLength = Ext2Utf8Decode((PUCHAR)Mbs->Buffer + i,
                                        Mbs->Length - i, &CodePoint);
            if (CodePoint < 0x10000) {
                Unicode->Buffer[Unicode->Length / sizeof(WCHAR)] = (WCHAR)CodePoint;
                Unicode->Length += sizeof(WCHAR);
            } else {
                CodePoint -= 0x10000;
                Unicode->Buffer[Unicode->Length / sizeof(WCHAR)] =
                    (WCHAR)(0xd800 + (CodePoint >> 10));
                Unicode->Buffer[Unicode->Length / sizeof(WCHAR) + 1] =
                    (WCHAR)(0xdc00 + (CodePoint & 0x3ff));
                Unicode->Length += 2 * sizeof(WCHAR);
            }
            i += CharLength;
        }
    }

    return Length;
}

static ULONG
Ext2UnicodeToUtf8(IN OUT PANSI_STRING Mbs, IN PUNICODE_STRING Unicode)
{
    ULONG i = 0;
    ULONG Length = 0;
    ULONG CodePoint;
    ULONG CharLength;

    while (i < Unicode->Length / sizeof(WCHAR)) {
        CodePoint = Unicode->Buffer[i++];
        if (CodePoint >= 0xd800 && CodePoint <= 0xdbff) {
            if (i == Unicode->Length / sizeof(WCHAR) ||
                Unicode->Buffer[i] < 0xdc00 || Unicode->Buffer[i] > 0xdfff) {
                return 0;
            }
            CodePoint = 0x10000 + ((CodePoint - 0xd800) << 10) +
                        (Unicode->Buffer[i++] - 0xdc00);
        } else if (CodePoint >= 0xdc00 && CodePoint <= 0xdfff) {
            return 0;
        }
        CharLength = Ext2Utf8Encode(CodePoint, NULL);
        if (CharLength == 0) {
            return 0;
        }
        Length += CharLength;
    }

    if (Mbs) {
        if (Mbs->MaximumLength < Length) {
            return 0;
        }
        Mbs->Length = 0;
        i = 0;
        while (i < Unicode->Length / sizeof(WCHAR)) {
            CodePoint = Unicode->Buffer[i++];
            if (CodePoint >= 0xd800 && CodePoint <= 0xdbff) {
                CodePoint = 0x10000 + ((CodePoint - 0xd800) << 10) +
                            (Unicode->Buffer[i++] - 0xdc00);
            }
            CharLength = Ext2Utf8Encode(CodePoint, (PUCHAR)Mbs->Buffer + Mbs->Length);
            Mbs->Length += (USHORT)CharLength;
        }
    }

    return Length;
}

ULONG
Ext2MbsToUnicode(
    struct nls_table *     PageTable,
    IN OUT PUNICODE_STRING Unicode,
    IN     PANSI_STRING    Mbs   )
{
    ULONG Length = 0;
    int i, mbc = 0;
    WCHAR  uc;

    if (Ext2IsUtf8(PageTable)) {
        return Ext2Utf8ToUnicode(Unicode, Mbs);
    }

    /* Count the length of the resulting Unicode. */
    for (i = 0; i < Mbs->Length; i += mbc) {

        mbc = PageTable->char2uni(
                  (PUCHAR)&(Mbs->Buffer[i]),
                  Mbs->Length - i,
                  &uc
              );

        if (mbc <= 0) {

            /* invalid character. */
            if (mbc == 0 && Length > 0) {
                break;
            }
            return 0;
        }

        Length += 2;
    }

    if (Unicode) {
        if (Unicode->MaximumLength < Length) {

            DbgBreak();
            return 0;
        }

        Unicode->Length = 0;
        mbc = 0;

        for (i = 0; i < Mbs->Length; i += mbc) {

            mbc = PageTable->char2uni(
                      (PUCHAR)&(Mbs->Buffer[i]),
                      Mbs->Length - i,
                      &uc
                  );
            Unicode->Buffer[Unicode->Length/2] = uc;
            Unicode->Length += 2;
        }
    }

    return Length;
}

ULONG
Ext2UnicodeToMbs (
    struct nls_table *  PageTable,
    IN OUT PANSI_STRING Mbs,
    IN PUNICODE_STRING  Unicode)
{
    ULONG Length = 0;
    UCHAR mbs[0x10];
    int i, mbc;

    if (Ext2IsUtf8(PageTable)) {
        return Ext2UnicodeToUtf8(Mbs, Unicode);
    }

    /* Count the length of the resulting mbc-8. */
    for (i = 0; i < (Unicode->Length / 2); i++) {

        RtlZeroMemory(mbs, 0x10);
        mbc = PageTable->uni2char(
                  Unicode->Buffer[i],
                  mbs,
                  0x10
              );

        if (mbc <= 0) {

            /* Invalid character. */
            return 0;
        }

        Length += mbc;
    }

    if (Mbs) {

        if (Mbs->MaximumLength < Length) {

            DbgBreak();
            return 0;
        }

        Mbs->Length = 0;

        for (i = 0; i < (Unicode->Length / 2); i++) {

            mbc = PageTable->uni2char(
                      Unicode->Buffer[i],
                      mbs,
                      0x10
                  );

            RtlCopyMemory(
                (PUCHAR)&(Mbs->Buffer[Mbs->Length]),
                &mbs[0],
                mbc
            );

            Mbs->Length += (USHORT)mbc;
        }
    }

    return Length;
}

ULONG
Ext2OEMToUnicodeSize(
    IN PEXT2_VCB        Vcb,
    IN PANSI_STRING     Oem
)
{
    ULONG   Length = 0;

    if (Vcb->Codepage.PageTable) {
        Length = Ext2MbsToUnicode(Vcb->Codepage.PageTable, NULL, Oem);
        if (Length > 0 || Ext2IsUtf8(Vcb->Codepage.PageTable)) {
            goto errorout;
        }
    }

    Length = RtlOemStringToCountedUnicodeSize(Oem);

errorout:

    return Length;
}

NTSTATUS
Ext2OEMToUnicode(
    IN PEXT2_VCB           Vcb,
    IN OUT PUNICODE_STRING Unicode,
    IN     POEM_STRING     Oem
)
{
    NTSTATUS  Status;

    if (Vcb->Codepage.PageTable) {
        Status = Ext2MbsToUnicode(Vcb->Codepage.PageTable,
                                  Unicode, Oem);

        if (Status >0 && Status == Unicode->Length) {
            Status = STATUS_SUCCESS;
            goto errorout;
        }
        if (Ext2IsUtf8(Vcb->Codepage.PageTable)) {
            Status = STATUS_OBJECT_NAME_INVALID;
            goto errorout;
        }
    }

    Status = RtlOemStringToUnicodeString(
                 Unicode, Oem, FALSE );

    if (!NT_SUCCESS(Status)) {
        DbgBreak();
        goto errorout;
    }

errorout:

    return Status;
}

ULONG
Ext2UnicodeToOEMSize(
    IN PEXT2_VCB       Vcb,
    IN PUNICODE_STRING Unicode
)
{
    ULONG   Length = 0;

    if (Vcb->Codepage.PageTable) {
        Length = Ext2UnicodeToMbs(Vcb->Codepage.PageTable,
                                  NULL, Unicode);
        if (Length > 0) {
            return Length;
        }

        DbgBreak();
    }

    return RtlxUnicodeStringToOemSize(Unicode);
}

NTSTATUS
Ext2UnicodeToOEM (
    IN PEXT2_VCB        Vcb,
    IN OUT POEM_STRING  Oem,
    IN PUNICODE_STRING  Unicode)
{
    NTSTATUS Status;

    if (Vcb->Codepage.PageTable) {

        Status = Ext2UnicodeToMbs(Vcb->Codepage.PageTable,
                                  Oem, Unicode);
        if (Status > 0 && Status == Oem->Length) {
            Status = STATUS_SUCCESS;
        } else {
            Status = STATUS_UNSUCCESSFUL;
            DbgBreak();
        }

        goto errorout;
    }

    Status = RtlUnicodeStringToOemString(
                 Oem, Unicode, FALSE );

    if (!NT_SUCCESS(Status))
    {
        DbgBreak();
        goto errorout;
    }

errorout:

    return Status;
}

VOID
Ext2Sleep(ULONG ms)
{
    LARGE_INTEGER Timeout;
    Timeout.QuadPart = (LONGLONG)ms*1000*(-10); /* ms/1000 sec*/
    KeDelayExecutionThread(KernelMode, TRUE, &Timeout);
}

int Ext2LinuxError (NTSTATUS Status)
{
    switch (Status) {
    case STATUS_ACCESS_DENIED:
        return (-EACCES);

    case STATUS_ACCESS_VIOLATION:
        return (-EFAULT);

    case STATUS_BUFFER_TOO_SMALL:
        return (-ETOOSMALL);

    case STATUS_INVALID_PARAMETER:
        return (-EINVAL);

    case STATUS_NOT_IMPLEMENTED:
    case STATUS_NOT_SUPPORTED:
        return (-EOPNOTSUPP);

    case STATUS_INVALID_ADDRESS:
    case STATUS_INVALID_ADDRESS_COMPONENT:
        return (-EADDRNOTAVAIL);

    case STATUS_NO_SUCH_DEVICE:
    case STATUS_NO_SUCH_FILE:
    case STATUS_OBJECT_NAME_NOT_FOUND:
    case STATUS_OBJECT_PATH_NOT_FOUND:
    case STATUS_NETWORK_BUSY:
    case STATUS_INVALID_NETWORK_RESPONSE:
    case STATUS_UNEXPECTED_NETWORK_ERROR:
        return (-ENETDOWN);

    case STATUS_BAD_NETWORK_PATH:
    case STATUS_NETWORK_UNREACHABLE:
    case STATUS_PROTOCOL_UNREACHABLE:
        return (-ENETUNREACH);

    case STATUS_LOCAL_DISCONNECT:
    case STATUS_TRANSACTION_ABORTED:
    case STATUS_CONNECTION_ABORTED:
        return (-ECONNABORTED);

    case STATUS_REMOTE_DISCONNECT:
    case STATUS_LINK_FAILED:
    case STATUS_CONNECTION_DISCONNECTED:
    case STATUS_CONNECTION_RESET:
    case STATUS_PORT_UNREACHABLE:
        return (-ECONNRESET);

    case STATUS_INSUFFICIENT_RESOURCES:
        return (-ENOMEM);

    case STATUS_PAGEFILE_QUOTA:
    case STATUS_NO_MEMORY:
    case STATUS_CONFLICTING_ADDRESSES:
    case STATUS_QUOTA_EXCEEDED:
    case STATUS_TOO_MANY_PAGING_FILES:
    case STATUS_WORKING_SET_QUOTA:
    case STATUS_COMMITMENT_LIMIT:
    case STATUS_TOO_MANY_ADDRESSES:
    case STATUS_REMOTE_RESOURCES:
        return (-ENOBUFS);

    case STATUS_INVALID_CONNECTION:
        return (-ENOTCONN);

    case STATUS_PIPE_DISCONNECTED:
        return (-ESHUTDOWN);

    case STATUS_TIMEOUT:
    case STATUS_IO_TIMEOUT:
    case STATUS_LINK_TIMEOUT:
        return (-ETIMEDOUT);

    case STATUS_REMOTE_NOT_LISTENING:
    case STATUS_CONNECTION_REFUSED:
        return (-ECONNREFUSED);

    case STATUS_HOST_UNREACHABLE:
        return (-EHOSTUNREACH);

    case STATUS_CANT_WAIT:
    case STATUS_PENDING:
        return (-EAGAIN);

    case STATUS_DEVICE_NOT_READY:
        return (-EIO);

    case STATUS_CANCELLED:
    case STATUS_REQUEST_ABORTED:
        return (-EINTR);

    case STATUS_BUFFER_OVERFLOW:
    case STATUS_INVALID_BUFFER_SIZE:
        return (-EMSGSIZE);

    case STATUS_ADDRESS_ALREADY_EXISTS:
        return (-EADDRINUSE);
    }

    if (NT_SUCCESS (Status))
        return 0;

    return (-EINVAL);
}

NTSTATUS Ext2WinntError(int rc)
{
    switch (rc) {

    case 0:
        return STATUS_SUCCESS;

    case -EPERM:
    case -EACCES:
        return STATUS_ACCESS_DENIED;

    case -ENOENT:
        return  STATUS_OBJECT_NAME_NOT_FOUND;

    case -EFAULT:
        return STATUS_ACCESS_VIOLATION;

    case -ETOOSMALL:
        return STATUS_BUFFER_TOO_SMALL;

    case -EBADMSG:
    case -EBADF:
    case -EINVAL:
    case -EFBIG:
        return STATUS_INVALID_PARAMETER;

    case -EBUSY:
        return STATUS_DEVICE_BUSY;

/*    case -ENOSYS:
        return STATUS_NOT_IMPLEMENTED;*/

    case -ENOSPC:
        return STATUS_DISK_FULL;

    case -EOPNOTSUPP:
        return STATUS_NOT_SUPPORTED;

/*    case -EDEADLK:
        return STATUS_POSSIBLE_DEADLOCK;*/

    case -EEXIST:
        return STATUS_OBJECT_NAME_COLLISION;

    case -EIO:
        return STATUS_UNEXPECTED_IO_ERROR;

    case -ENOTDIR:
        return STATUS_NOT_A_DIRECTORY;

    case -EISDIR:
        return STATUS_FILE_IS_A_DIRECTORY;

/*    case -ENOTEMPTY:
        return STATUS_DIRECTORY_NOT_EMPTY;*/

    case -ENODEV:
        return STATUS_NO_SUCH_DEVICE;

    case -ENXIO:
        return STATUS_INVALID_ADDRESS;

    case -EADDRNOTAVAIL:
        return STATUS_INVALID_ADDRESS;

    case -ENETDOWN:
        return STATUS_UNEXPECTED_NETWORK_ERROR;

    case -ENETUNREACH:
        return STATUS_NETWORK_UNREACHABLE;

    case -ECONNABORTED:
        return STATUS_CONNECTION_ABORTED;

    case -ECONNRESET:
        return STATUS_CONNECTION_RESET;

    case -ENOMEM:
        return STATUS_INSUFFICIENT_RESOURCES;

    case -ENOBUFS:
        return STATUS_NO_MEMORY;

    case -ENOTCONN:
        return STATUS_INVALID_CONNECTION;

    case -ESHUTDOWN:
        return STATUS_CONNECTION_DISCONNECTED;

    case -ETIMEDOUT:
        return STATUS_TIMEOUT;

    case -ECONNREFUSED:
        return STATUS_CONNECTION_REFUSED;

    case -EHOSTUNREACH:
        return STATUS_HOST_UNREACHABLE;

    case -EAGAIN:
        return STATUS_CANT_WAIT;

    case -EINTR:
        return  STATUS_CANCELLED;

    case -EMSGSIZE:
        return STATUS_INVALID_BUFFER_SIZE;

    case -EADDRINUSE:
        return STATUS_ADDRESS_ALREADY_EXISTS;
    }

    return STATUS_UNSUCCESSFUL;
}

BOOLEAN Ext2IsDot(PUNICODE_STRING name)
{
    return (name->Length == 2 && name->Buffer[0] == L'.');
}

BOOLEAN Ext2IsDotDot(PUNICODE_STRING name)
{
    return (name->Length == 4 && name->Buffer[0] == L'.' &&
            name->Buffer[1] == L'.');
}
