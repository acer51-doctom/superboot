/*
 * ntfs.c — Read-only NTFS filesystem driver (stub)
 *
 * NTFS identification: the first sector contains "NTFS    " at
 * offset 3 (the OEM ID in the BPB).  Full implementation requires
 * MFT traversal, attribute parsing, and run-list decoding.
 * Probe is implemented; mount/read is a TODO.
 */

#include "vfs.h"

static EFI_STATUS
ntfs_probe(EFI_BLOCK_IO_PROTOCOL *block_io, EFI_DISK_IO_PROTOCOL *disk_io)
{
    UINT8 sector[512];
    EFI_STATUS status;

    if (disk_io) {
        status = disk_io->ReadDisk(
            disk_io, block_io->Media->MediaId, 0, 512, sector);
    } else {
        UINT32 bs = block_io->Media->BlockSize;
        void *tmp = AllocatePool(bs);
        if (!tmp) return EFI_OUT_OF_RESOURCES;
        status = block_io->ReadBlocks(
            block_io, block_io->Media->MediaId, 0, bs, tmp);
        if (!EFI_ERROR(status))
            CopyMem(sector, tmp, 512);
        FreePool(tmp);
    }

    if (EFI_ERROR(status))
        return status;

    /* Check OEM ID at offset 3: "NTFS    " (8 bytes). */
    if (sb_strncmp8((CHAR8 *)(sector + 3), "NTFS    ", 8) == 0)
        return EFI_SUCCESS;

    return EFI_NOT_FOUND;
}

static EFI_STATUS
ntfs_mount(EFI_BLOCK_IO_PROTOCOL *b, EFI_DISK_IO_PROTOCOL *d, void **c)
{ (void)b; (void)d; (void)c; return EFI_UNSUPPORTED; }

static EFI_STATUS
ntfs_read_file(void *c, const CHAR16 *p, void **buf, UINTN *sz)
{ (void)c; (void)p; (void)buf; (void)sz; return EFI_UNSUPPORTED; }

static EFI_STATUS
ntfs_dir_exists(void *c, const CHAR16 *p)
{ (void)c; (void)p; return EFI_UNSUPPORTED; }

static void
ntfs_unmount(void *c) { if (c) FreePool(c); }

VfsDriver sb_vfs_ntfs = {
    .name       = L"ntfs",
    .probe      = ntfs_probe,
    .mount      = ntfs_mount,
    .read_file  = ntfs_read_file,
    .dir_exists = ntfs_dir_exists,
    .unmount    = ntfs_unmount,
};
