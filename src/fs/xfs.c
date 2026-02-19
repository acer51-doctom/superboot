/*
 * xfs.c — Read-only XFS filesystem driver (stub)
 *
 * XFS uses a different superblock magic and B+tree structure.
 * Probe is implemented; mount/read is a TODO.
 */

#include "vfs.h"

#define XFS_SUPER_MAGIC  0x58465342  /* "XFSB" */

#pragma pack(1)
typedef struct {
    UINT32 sb_magicnum;
    UINT32 sb_blocksize;
    UINT64 sb_dblocks;
    UINT64 sb_rblocks;
    UINT64 sb_rextents;
    UINT8  sb_uuid[16];
    /* ... */
} XfsSuperblock;
#pragma pack()

static EFI_STATUS
xfs_probe(EFI_BLOCK_IO_PROTOCOL *block_io, EFI_DISK_IO_PROTOCOL *disk_io)
{
    XfsSuperblock sb;
    EFI_STATUS status;

    if (disk_io) {
        status = disk_io->ReadDisk(
            disk_io, block_io->Media->MediaId, 0, sizeof(sb), &sb);
    } else {
        UINT32 bs = block_io->Media->BlockSize;
        void *tmp = AllocatePool(bs);
        if (!tmp) return EFI_OUT_OF_RESOURCES;
        status = block_io->ReadBlocks(
            block_io, block_io->Media->MediaId, 0, bs, tmp);
        if (!EFI_ERROR(status))
            CopyMem(&sb, tmp, sizeof(sb));
        FreePool(tmp);
    }

    if (EFI_ERROR(status))
        return status;

    /* XFS stores magic in big-endian. */
    UINT32 magic = ((sb.sb_magicnum >> 24) & 0xFF)
                 | ((sb.sb_magicnum >>  8) & 0xFF00)
                 | ((sb.sb_magicnum <<  8) & 0xFF0000)
                 | ((sb.sb_magicnum << 24) & 0xFF000000);

    return (magic == XFS_SUPER_MAGIC) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS
xfs_mount(EFI_BLOCK_IO_PROTOCOL *b, EFI_DISK_IO_PROTOCOL *d, void **c)
{ (void)b; (void)d; (void)c; return EFI_UNSUPPORTED; }

static EFI_STATUS
xfs_read_file(void *c, const CHAR16 *p, void **buf, UINTN *sz)
{ (void)c; (void)p; (void)buf; (void)sz; return EFI_UNSUPPORTED; }

static EFI_STATUS
xfs_dir_exists(void *c, const CHAR16 *p)
{ (void)c; (void)p; return EFI_UNSUPPORTED; }

static void
xfs_unmount(void *c) { if (c) FreePool(c); }

VfsDriver sb_vfs_xfs = {
    .name       = L"xfs",
    .probe      = xfs_probe,
    .mount      = xfs_mount,
    .read_file  = xfs_read_file,
    .dir_exists = xfs_dir_exists,
    .unmount    = xfs_unmount,
};
