# Copilot Instructions for Ext4Fsd

## Project Overview
**Ext4Fsd** is a Windows kernel-mode file system driver enabling read/write access to Ext2/3/4 file systems. It consists of two main components:
- **Ext4Fsd (Ext2Srv)**: Windows Driver Model (WDM) driver; targets Windows 10+ on x86, x64, ARM, ARM64
- **Ext2Mgr**: MFC-based management application for mounting/configuring volumes
- **Ext2Srv**: Windows service for drive letter management

## Architecture Essentials

### Driver Structure (Ext4Fsd/)
The driver follows IRP-based architecture with key layers:

1. **Dispatch & Request Handling**: [dispatch.c](Ext4Fsd/dispatch.c) - IRP queuing, oplock completion, buffer locking
2. **Core Operations**: Organized by I/O type (read, write, create, close, directory control, file info)
3. **File System Abstraction**: Linux-compatible data structures mapped to Windows (ext3_super_block → EXT2_SUPER_BLOCK)
4. **Volume Control Block (VCB)**: Central structure in device extension managing mounted volume state

### Key Data Structures
- `EXT2_VCB`: Volume control block (superblock, mount state, resource locks)
- `EXT2_FCB`: File control block (file/directory metadata)
- `EXT2_IRP_CONTEXT`: Request context for async IRP processing
- `EXT2_INODE`: Ext4 inode with extended time fields (nano-second precision, 2038+ support)

### Subsystems

**Extent Management** ([ext4/ext4_extents.c](Ext4Fsd/ext4/ext4_extents.c)):
- Block mapping, allocation, and defragmentation
- Performance-optimized batch operations (Ext2GetBranchBatch)

**Directory Processing** (htree directory cache):
- Hash-tree traversal with 16-entry cache (round-robin replacement)
- 2-3x faster lookup performance

**Checksum Validation** ([ext4/ext4_csum.c](Ext4Fsd/ext4/ext4_csum.c)):
- Hardware-accelerated CRC32C (SSE4.2 intrinsics)
- Fallback to software implementation; runtime CPU feature detection

**Journal/Recovery** ([jbd2/](Ext4Fsd/jbd2/)):
- Ext4 journal commit with batch optimization
- Transaction recovery with checksum validation

## Critical Workflows

### Building the Driver
Visual Studio 2012+ required with Windows Driver Kit (WDK) 10.0:
```
msbuild Ext4Fsd.sln /p:Configuration=Release /p:Platform=x64
```
- Supports: Debug|Win32/x64/ARM/ARM64, Release variants
- Output: .sys driver file (kernel-mode executable)
- PropertySheet.props: Common build settings
- **WDK Include Paths**: GitHub Actions workflow passes WDK paths as MSBuild properties via `/p:KM_IncludePath=...` etc. The vcxproj files reference these as `$(KM_IncludePath)`, `$(Shared_IncludePath)`, `$(UM_IncludePath)`
- **Include Path Format**: Ext2Mgr.vcxproj references kernel-mode headers from WDK; Ext4Fsd.vcxproj includes local kernel headers via relative paths (.\include, .\include\linux, .\include\asm)

### Volume Mount Process
1. Ext2Mgr discovers volumes via WMI/disk enumeration ([Ext2Mgr/enumDisk.cpp](Ext2Mgr/enumDisk.cpp))
2. User selects mount point (drive letter or NTFS mount point)
3. Assignment methods: DOS Devices registry, Mount Manager, Mount Points
4. Driver DriverEntry initializes at first mount ([init.c](Ext4Fsd/init.c))
5. Volume mounts via IRP_MJ_FILE_SYSTEM_CONTROL (FSCTL_MOUNT_VOLUME)

### Debugging
- Kernel debug build with DbgBreak() at [ext2fs.h#L27](Ext4Fsd/include/ext2fs.h#L27)
- DEBUG macro levels: DL_ERR, DL_WRN, DL_INF, DL_DET, DL_FUN
- Performance/memory stats via PS_* pool tags ([common.h#L13](Ext4Fsd/include/common.h#L13))

## Code Patterns & Conventions

### Resource Synchronization
- **MainResource**: VCB-level exclusive lock for mount state, extent allocation
- **PagingIoResource**: Shared lock for concurrent read/write
- Typical pattern:
```c
ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
__try {
    /* critical section */
} __finally {
    ExReleaseResourceLite(&Vcb->MainResource);
}
```

### Error Handling
- NT status codes (NTSTATUS)
- __try/__finally blocks for cleanup (vs. goto for simple cases)
- ASSERT macros validate structure identifiers: `(Vcb->Identifier.Type == EXT2VCB)`

### Performance Optimizations (Recent)
- **CRC32C**: 3-5x via SSE4.2 intrinsics with runtime detection ([ext4_csum.c](Ext4Fsd/ext4/ext4_csum.c))
- **Batch I/O**: Ext2GetBranchBatch reads indirect blocks in bulk (2x improvement)
- **Htree Cache**: 16-entry cache for directory traversal (2-3x faster)
- Prefer shared locks where exclusive not needed; exclusive acquisitions log DL_WRN

### Security Boundaries
- **Path validation**: Check symlink targets; prevent directory traversal
- **Overflow prevention**: ULONGLONG casts for volume sizes; ea_name_len ≤ 255, ea_data_size ≤ 65535
- **File size limits**: Enforce max 0x7fffffffffffffff (signed 64-bit)
- **Write protection**: At 12+ validation checkpoints before journaling

## Common Edits & Patterns

### Adding Ext4 Feature Support
1. Define compatibility flag in [ext2fs.h](Ext4Fsd/include/ext2fs.h) (EXT4_FEATURE_*)
2. Add feature detection in DriverEntry ([init.c](Ext4Fsd/init.c))
3. Implement Linux kernel equivalent from [ext4/](Ext4Fsd/ext4/), [linux/](Ext4Fsd/include/linux/)
4. Update VCB flags/stats in [common.h](Ext4Fsd/include/common.h) PS_* pool tag tracking

### Fixing Concurrency Issues
- Identify lock scope: global (Ext2Global), VCB (MainResource/PagingIoResource), or FCB
- Avoid nested exclusive locks on same resource
- Prefer ExAcquireResourceSharedLite for read-only operations

### Time Field Handling
Ext4 extended time fields (>= 2038):
- Superblock: s_mkfs_time_hi (bits 32-34 of seconds)
- Inode: i_*_time_extra (nano-seconds + epoch as bits 34-33 and 1-0)
- See CHANGELOG for conversion logic

## Integration Points

**Ext2Mgr ↔ Ext2Srv/Driver**:
- IOCTL_APP_VOLUME_PROPERTY ([common.h#L4](Ext4Fsd/include/common.h#L4)): Query/set mount options
- DeviceIoControl for FSCTL_LOCK/DISMOUNT_VOLUME
- Registry: HKLM\System\CurrentControlSet\Services\Ext2Fsd\Parameters\Volumes

**Windows I/O Subsystem**:
- IRP dispatch via Ext2DispatchRequest → function pointers in dispatch.c
- Cache manager integration for read-ahead, lazy write
- File system recognizer for volume mount

## Testing & CI/CD
- GitHub Actions: [.github/workflows/](/.github/workflows/) (build.yml, quality.yml)
- Multi-platform: Win32/x64/ARM/ARM64 builds required before merge
- Manual test: Mount ext4 image on Windows via Ext2Mgr
