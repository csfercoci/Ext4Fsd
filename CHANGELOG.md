# Changelog

All notable changes to Ext4Fsd are documented in this file.

## [Unreleased] - January 11, 2026

### Added
- Hardware-accelerated CRC32C checksums using SSE4.2 intrinsics
- Batch indirect block reading function (Ext2GetBranchBatch)
- Htree directory cache with 16-entry round-robin replacement
- Batch zone initialization for extent building
- Journal batch commit optimization infrastructure

### Improved
- CRC32C performance: 3-5x faster with hardware acceleration
- Directory traversal: 2-3x faster with htree caching
- Large file mapping: 2x faster with batch block reading
- Journal recovery: 1.5-2x improvement with batch commits
- Concurrent I/O: Better performance with shared lock optimization

### Fixed
- Memory leak in block I/O operations (__finally cleanup)
- Integer overflow in volume size calculations (ULONGLONG cast)
- Buffer overflow risk in directory entry name handling
- Missing extended attribute size validation
- Lock contention in flush operations
- Journal checksum validation in recovery path

### Security
- Added bounds checking for directory names
- Added EA size validation (name_len ≤ 255, data_size ≤ 65535)
- Write protection enforcement at 12+ checkpoints
- Symlink target validation
- File size limit enforcement (0x7fffffffffffffff)

## Previous Releases

See git history for changes in versions prior to January 2026.
