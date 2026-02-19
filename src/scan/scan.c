/*
 * scan.c — Block device scanner
 *
 * Enumerates all UEFI block device handles, opens each partition via
 * the VFS layer, probes for known config files, and feeds them to the
 * registered config parsers.
 */

#include "scan.h"
#include "../config/config.h"
#include "../fs/vfs.h"

/* ------------------------------------------------------------------ */
/*  Probe a single partition for boot configs                          */
/* ------------------------------------------------------------------ */

static EFI_STATUS
scan_partition(SuperBootContext *ctx, EFI_HANDLE device)
{
    EFI_STATUS status;

    /* Try to mount / open the device. */
    status = sb_vfs_open_device(device);
    if (EFI_ERROR(status))
        return status;

    /* Iterate over all registered config parsers. */
    const ConfigParser **parsers = sb_config_get_parsers();

    for (const ConfigParser **pp = parsers; *pp; pp++) {
        const ConfigParser *parser = *pp;

        /* Try each config path this parser knows about. */
        for (const CHAR16 **path = parser->config_paths; *path; path++) {
            if (!sb_vfs_file_exists(device, *path))
                continue;

            SB_DBG(ctx, L"Found %s: %s", parser->name, *path);

            /* Read the config file. */
            void  *data = NULL;
            UINTN  size = 0;
            status = sb_vfs_read_file(device, *path, &data, &size);
            if (EFI_ERROR(status))
                continue;

            /* Parse it. */
            UINTN remaining = SB_MAX_TARGETS - ctx->targets.count;
            if (remaining == 0) {
                FreePool(data);
                return EFI_SUCCESS;
            }

            UINTN found = 0;
            status = parser->parse(
                         (CHAR8 *)data, size, device, *path,
                         &ctx->targets.entries[ctx->targets.count],
                         &found, remaining);

            if (!EFI_ERROR(status) && found > 0) {
                SB_LOG(L"  %s: %u entries from %s",
                       parser->name, found, *path);
                ctx->targets.count += found;
            }

            FreePool(data);

            /* Only use the first matching config path per parser
             * per partition (e.g., don't parse both /boot/grub/grub.cfg
             * and /grub/grub.cfg on the same partition). */
            break;
        }
    }

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Public API: scan all connected block devices                       */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_scan_all_devices(SuperBootContext *ctx)
{
    EFI_STATUS status;
    EFI_HANDLE *handles = NULL;
    UINTN       handle_count = 0;

    SB_LOG(L"Scanning for bootable configurations...");

    /*
     * Enumerate all handles that provide the Block I/O protocol.
     * This includes both whole-disk devices and individual partitions.
     * We only care about partitions (logical partitions have
     * BlockIO->Media->LogicalPartition == TRUE).
     */
    status = ctx->boot_services->LocateHandleBuffer(
                 ByProtocol,
                 &gEfiBlockIoProtocolGuid,
                 NULL,
                 &handle_count,
                 &handles);
    if (EFI_ERROR(status)) {
        SB_LOG(L"No block devices found.");
        return status;
    }

    SB_LOG(L"Found %u block I/O handles.", handle_count);

    for (UINTN i = 0; i < handle_count; i++) {
        /* Filter: only scan logical partitions, not whole disks. */
        EFI_BLOCK_IO_PROTOCOL *block_io;
        status = ctx->boot_services->HandleProtocol(
                     handles[i], &gEfiBlockIoProtocolGuid,
                     (void **)&block_io);
        if (EFI_ERROR(status))
            continue;

        if (!block_io->Media->LogicalPartition)
            continue;

        /* Skip non-present media. */
        if (!block_io->Media->MediaPresent)
            continue;

        SB_DBG(ctx, L"Scanning partition handle %u (MediaId=%u, BlockSize=%u)",
               i, block_io->Media->MediaId,
               block_io->Media->BlockSize);

        scan_partition(ctx, handles[i]);

        if (ctx->targets.count >= SB_MAX_TARGETS)
            break;
    }

    if (handles)
        FreePool(handles);

    return (ctx->targets.count > 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}
