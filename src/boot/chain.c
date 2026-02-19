/*
 * chain.c — EFI chain-loader (fallback for non-Linux targets)
 *
 * Loads an arbitrary .efi binary from a partition and transfers
 * control via the standard UEFI LoadImage/StartImage mechanism.
 * Used for Windows Boot Manager, other UEFI shells, etc.
 */

#include "../superboot.h"
#include "../fs/vfs.h"

EFI_STATUS
sb_chainload_efi(SuperBootContext *ctx, const BootTarget *target)
{
    EFI_STATUS status;
    void  *buf  = NULL;
    UINTN  size = 0;

    SB_LOG(L"Chain-loading: %s", target->efi_path);

    /* Read the .efi binary via VFS. */
    status = sb_vfs_read_file(target->device_handle,
                              target->efi_path, &buf, &size);
    SB_CHECK(status, L"Failed to read EFI binary");

    /* Build a device path for the target.  We need the disk device
     * path + the file path appended. */
    EFI_DEVICE_PATH_PROTOCOL *dev_path = NULL;

    /* Get the device's existing device path. */
    EFI_DEVICE_PATH_PROTOCOL *disk_path;
    status = ctx->boot_services->HandleProtocol(
                 target->device_handle,
                 &gEfiDevicePathProtocolGuid,
                 (void **)&disk_path);
    if (!EFI_ERROR(status)) {
        dev_path = FileDevicePath(target->device_handle,
                                  (CHAR16 *)target->efi_path);
    }

    /* Load the image from the memory buffer. */
    EFI_HANDLE child_handle = NULL;
    status = ctx->boot_services->LoadImage(
                 FALSE,
                 ctx->image_handle,
                 dev_path,
                 buf, size,
                 &child_handle);
    if (EFI_ERROR(status)) {
        SB_LOG(L"LoadImage failed: %r", status);
        FreePool(buf);
        return status;
    }

    FreePool(buf);

    /* Start the loaded image.  This transfers control and may not
     * return (e.g., Windows Boot Manager). */
    UINTN exit_data_size = 0;
    CHAR16 *exit_data = NULL;

    status = ctx->boot_services->StartImage(
                 child_handle, &exit_data_size, &exit_data);
    if (EFI_ERROR(status))
        SB_LOG(L"StartImage returned: %r", status);

    return status;
}
