/*
 * vfs.h — Virtual Filesystem abstraction
 *
 * SuperBoot needs to read files from FAT32, ext4, BTRFS, XFS, and
 * NTFS partitions.  The VFS layer provides a unified interface.
 *
 * Strategy:
 *   1. For partitions that UEFI already understands (FAT32, and any
 *      filesystem with a loaded EFI driver), use the native
 *      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.
 *   2. For unsupported filesystems, use built-in read-only drivers
 *      that operate on raw EFI_BLOCK_IO_PROTOCOL access.
 *   3. External .efi filesystem drivers can be loaded at init time
 *      from the SuperBoot ESP directory to extend coverage.
 */

#ifndef SUPERBOOT_VFS_H
#define SUPERBOOT_VFS_H

#include "../superboot.h"

/* ------------------------------------------------------------------ */
/*  Filesystem driver vtable                                           */
/* ------------------------------------------------------------------ */

typedef struct VfsDriver {
    const CHAR16 *name;        /* e.g. L"ext4", L"btrfs" */

    /*
     * probe() — does this partition contain our filesystem?
     * Reads the superblock via block_io and checks the magic number.
     * Returns EFI_SUCCESS if this driver claims the partition.
     */
    EFI_STATUS (*probe)(EFI_BLOCK_IO_PROTOCOL *block_io,
                        EFI_DISK_IO_PROTOCOL  *disk_io);

    /*
     * mount() — prepare internal state for reading files.
     * Returns an opaque context pointer stored by the VFS layer.
     */
    EFI_STATUS (*mount)(EFI_BLOCK_IO_PROTOCOL *block_io,
                        EFI_DISK_IO_PROTOCOL  *disk_io,
                        void **fs_context);

    /*
     * read_file() — read an entire file into a caller-freed buffer.
     * Path uses forward-slash separators, e.g. "/boot/vmlinuz".
     * Allocates *buffer via AllocatePool; caller must FreePool.
     */
    EFI_STATUS (*read_file)(void *fs_context, const CHAR16 *path,
                            void **buffer, UINTN *size);

    /*
     * dir_exists() — check if a directory path exists.
     */
    EFI_STATUS (*dir_exists)(void *fs_context, const CHAR16 *path);

    /*
     * unmount() — free internal state.
     */
    void (*unmount)(void *fs_context);
} VfsDriver;

/* ------------------------------------------------------------------ */
/*  Built-in driver declarations                                       */
/* ------------------------------------------------------------------ */

extern VfsDriver sb_vfs_ext4;
extern VfsDriver sb_vfs_btrfs;
extern VfsDriver sb_vfs_xfs;
extern VfsDriver sb_vfs_ntfs;

/* ------------------------------------------------------------------ */
/*  VFS layer API (implemented in vfs.c)                               */
/* ------------------------------------------------------------------ */

/*
 * sb_vfs_load_external_drivers() — scan SuperBoot's own directory
 * for .efi filesystem driver images and load them.
 */
EFI_STATUS sb_vfs_load_external_drivers(SuperBootContext *ctx);

/*
 * sb_vfs_open_device() — given a UEFI handle for a partition,
 * attempt to open it.  Tries UEFI SimpleFileSystem first, then
 * probes built-in drivers.
 *
 * Returns EFI_SUCCESS and sets *is_native if the UEFI protocol
 * worked, or mounts via a built-in driver otherwise.
 */
EFI_STATUS sb_vfs_open_device(EFI_HANDLE device);

/*
 * sb_vfs_file_exists() — quick probe for a file's existence.
 */
BOOLEAN sb_vfs_file_exists(EFI_HANDLE device, const CHAR16 *path);

#endif /* SUPERBOOT_VFS_H */
