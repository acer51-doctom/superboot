/*
 * btrfs.c — Read-only BTRFS filesystem driver (stub)
 *
 * BTRFS is a CoW B-tree filesystem with a fundamentally different
 * on-disk format from ext4.  A full implementation requires:
 *   - Superblock parsing (at 64 KiB offset)
 *   - Chunk tree traversal for logical→physical address mapping
 *   - Root tree navigation to find the FS tree
 *   - B-tree search for inode items, dir items, extent data
 *   - Handling of subvolumes (common with Arch/Fedora installs)
 *
 * This stub provides the probe function (magic check) and outlines
 * the mount/read structure.  Full implementation is tracked as a
 * TODO milestone.
 */

#include "vfs.h"

#define BTRFS_SUPER_MAGIC        0x4D5F53665248425FULL  /* "_BHRfS_M" */
#define BTRFS_SUPERBLOCK_OFFSET  0x10000                /* 64 KiB    */

#pragma pack(1)
typedef struct {
    UINT8  csum[32];
    UINT8  fsid[16];
    UINT64 bytenr;
    UINT64 flags;
    UINT64 magic;
    UINT64 generation;
    UINT64 root;
    UINT64 chunk_root;
    UINT64 log_root;
    /* ... many more fields */
} BtrfsSuperblock;
#pragma pack()

static EFI_STATUS
btrfs_probe(EFI_BLOCK_IO_PROTOCOL *block_io, EFI_DISK_IO_PROTOCOL *disk_io)
{
    BtrfsSuperblock sb;
    EFI_STATUS status;

    if (disk_io) {
        status = disk_io->ReadDisk(
            disk_io, block_io->Media->MediaId,
            BTRFS_SUPERBLOCK_OFFSET, sizeof(sb), &sb);
    } else {
        UINT32 bs = block_io->Media->BlockSize;
        UINTN  read_size = sizeof(sb) < bs ? bs : sizeof(sb);
        void *tmp = AllocatePool(read_size);
        if (!tmp) return EFI_OUT_OF_RESOURCES;
        UINT64 lba = BTRFS_SUPERBLOCK_OFFSET / bs;
        status = block_io->ReadBlocks(
            block_io, block_io->Media->MediaId, lba, read_size, tmp);
        if (!EFI_ERROR(status))
            CopyMem(&sb, tmp, sizeof(sb));
        FreePool(tmp);
    }

    if (EFI_ERROR(status))
        return status;

    return (sb.magic == BTRFS_SUPER_MAGIC) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS
btrfs_mount(EFI_BLOCK_IO_PROTOCOL *block_io, EFI_DISK_IO_PROTOCOL *disk_io,
            void **fs_context)
{
    (void)block_io; (void)disk_io; (void)fs_context;
    /* TODO: Implement BTRFS chunk tree + root tree parsing. */
    return EFI_UNSUPPORTED;
}

static EFI_STATUS
btrfs_read_file(void *fs_context, const CHAR16 *path,
                void **buffer, UINTN *size)
{
    (void)fs_context; (void)path; (void)buffer; (void)size;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS
btrfs_dir_exists(void *fs_context, const CHAR16 *path)
{
    (void)fs_context; (void)path;
    return EFI_UNSUPPORTED;
}

static void
btrfs_unmount(void *fs_context)
{
    if (fs_context) FreePool(fs_context);
}

VfsDriver sb_vfs_btrfs = {
    .name       = L"btrfs",
    .probe      = btrfs_probe,
    .mount      = btrfs_mount,
    .read_file  = btrfs_read_file,
    .dir_exists = btrfs_dir_exists,
    .unmount    = btrfs_unmount,
};
