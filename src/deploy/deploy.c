/*
 * deploy.c — Non-destructive deployment to an internal ESP
 *
 * Copies the SuperBoot binary from the USB drive to the host machine's
 * EFI System Partition and creates a UEFI boot entry (BootXXXX
 * variable).  Does NOT modify any existing boot entries or files.
 *
 * Steps:
 *   1. Locate the SuperBoot binary on the current boot device.
 *   2. Find the internal ESP (GPT partition type C12A7328-...).
 *   3. Create \EFI\superboot\ directory on the ESP.
 *   4. Copy the binary.
 *   5. Create a UEFI Boot#### variable pointing to it.
 *   6. Optionally prepend it to BootOrder.
 */

#include "deploy.h"

/* EFI System Partition GUID. */
static EFI_GUID EspPartitionTypeGuid = {
    0xC12A7328, 0xF81F, 0x11D2,
    { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};

/* ------------------------------------------------------------------ */
/*  Find the internal ESP                                              */
/* ------------------------------------------------------------------ */

static EFI_HANDLE
find_internal_esp(SuperBootContext *ctx, EFI_HANDLE exclude)
{
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    EFI_HANDLE result = NULL;

    ctx->boot_services->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
        NULL, &handle_count, &handles);

    for (UINTN i = 0; i < handle_count; i++) {
        if (handles[i] == exclude)
            continue;

        /* Check if this partition has the ESP type GUID. */
        EFI_DEVICE_PATH_PROTOCOL *dp;
        EFI_STATUS s = ctx->boot_services->HandleProtocol(
                           handles[i], &gEfiDevicePathProtocolGuid,
                           (void **)&dp);
        if (EFI_ERROR(s))
            continue;

        /*
         * Walk the device path looking for a MEDIA_HARDDRIVE_DP node
         * with the ESP partition type GUID.
         */
        EFI_DEVICE_PATH_PROTOCOL *node = dp;
        while (!IsDevicePathEnd(node)) {
            if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
                DevicePathSubType(node) == MEDIA_HARDDRIVE_DP) {
                HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)node;
                if (hd->SignatureType == SIGNATURE_TYPE_GUID &&
                    CompareMem(hd->Signature, &EspPartitionTypeGuid.Data1,
                               sizeof(EFI_GUID)) == 0) {
                    /* Heuristic: skip if it's a removable device (USB). */
                    EFI_BLOCK_IO_PROTOCOL *bio;
                    s = ctx->boot_services->HandleProtocol(
                            handles[i], &gEfiBlockIoProtocolGuid,
                            (void **)&bio);
                    if (!EFI_ERROR(s) && !bio->Media->RemovableMedia) {
                        result = handles[i];
                        goto done;
                    }
                }
            }
            node = NextDevicePathNode(node);
        }
    }

done:
    if (handles)
        FreePool(handles);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Copy file between ESPs                                             */
/* ------------------------------------------------------------------ */

static EFI_STATUS
copy_self_to_esp(SuperBootContext *ctx, EFI_HANDLE target_esp)
{
    EFI_STATUS status;

    /* Open source: our own binary. */
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    status = ctx->boot_services->HandleProtocol(
                 ctx->image_handle, &gEfiLoadedImageProtocolGuid,
                 (void **)&loaded);
    SB_CHECK(status, L"Cannot locate loaded image");

    /* Read our own binary. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *src_fs;
    status = ctx->boot_services->HandleProtocol(
                 loaded->DeviceHandle,
                 &gEfiSimpleFileSystemProtocolGuid,
                 (void **)&src_fs);
    SB_CHECK(status, L"Cannot open source FS");

    EFI_FILE_PROTOCOL *src_root;
    status = src_fs->OpenVolume(src_fs, &src_root);
    SB_CHECK(status, L"Cannot open source volume");

    /* Determine our file path from the loaded image. */
    EFI_DEVICE_PATH_PROTOCOL *fp = loaded->FilePath;
    CHAR16 *self_path = DevicePathToStr(fp);
    if (!self_path)
        return EFI_NOT_FOUND;

    EFI_FILE_PROTOCOL *src_file;
    status = src_root->Open(src_root, &src_file, self_path,
                            EFI_FILE_MODE_READ, 0);
    src_root->Close(src_root);
    FreePool(self_path);
    SB_CHECK(status, L"Cannot open self binary");

    /* Get file size. */
    UINT8 info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = src_file->GetInfo(src_file, &gEfiFileInfoGuid,
                               &info_size, info_buf);
    SB_CHECK(status, L"Cannot stat self binary");
    UINTN file_size = (UINTN)((EFI_FILE_INFO *)info_buf)->FileSize;

    void *buf = AllocatePool(file_size);
    if (!buf) {
        src_file->Close(src_file);
        return EFI_OUT_OF_RESOURCES;
    }
    status = src_file->Read(src_file, &file_size, buf);
    src_file->Close(src_file);
    if (EFI_ERROR(status)) {
        FreePool(buf);
        return status;
    }

    /* Open destination ESP. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *dst_fs;
    status = ctx->boot_services->HandleProtocol(
                 target_esp, &gEfiSimpleFileSystemProtocolGuid,
                 (void **)&dst_fs);
    if (EFI_ERROR(status)) {
        FreePool(buf);
        return status;
    }

    EFI_FILE_PROTOCOL *dst_root;
    status = dst_fs->OpenVolume(dst_fs, &dst_root);
    if (EFI_ERROR(status)) {
        FreePool(buf);
        return status;
    }

    /* Create \EFI\superboot\ directory. */
    EFI_FILE_PROTOCOL *dir;
    status = dst_root->Open(dst_root, &dir, SB_DEPLOY_DIR,
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                            EFI_FILE_MODE_CREATE,
                            EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(status))
        dir->Close(dir);

    /* Write the binary. */
    EFI_FILE_PROTOCOL *dst_file;
    status = dst_root->Open(dst_root, &dst_file, SB_DEPLOY_BINARY,
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                            EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) {
        dst_root->Close(dst_root);
        FreePool(buf);
        return status;
    }

    status = dst_file->Write(dst_file, &file_size, buf);
    dst_file->Close(dst_file);
    dst_root->Close(dst_root);
    FreePool(buf);

    return status;
}

/* ------------------------------------------------------------------ */
/*  Create a UEFI boot entry                                           */
/* ------------------------------------------------------------------ */

static EFI_STATUS
create_boot_entry(SuperBootContext *ctx, EFI_HANDLE target_esp)
{
    /*
     * Find a free Boot#### variable number.
     * We scan Boot0000–Boot00FF looking for an unused slot.
     */
    UINT16 boot_num = 0xFFFF;
    for (UINT16 i = 0; i <= 0x00FF; i++) {
        CHAR16 varname[32];
        SPrint(varname, sizeof(varname), L"Boot%04X", i);

        UINTN size = 0;
        EFI_STATUS s = ctx->runtime_services->GetVariable(
                           varname, &gEfiGlobalVariableGuid,
                           NULL, &size, NULL);
        if (s == EFI_NOT_FOUND) {
            boot_num = i;
            break;
        }
    }

    if (boot_num == 0xFFFF) {
        SB_LOG(L"No free Boot#### slot found.");
        return EFI_OUT_OF_RESOURCES;
    }

    /*
     * Build the EFI_LOAD_OPTION structure.
     * Layout: Attributes(4) + FilePathListLen(2) + Description(var) +
     *         DevicePath(var)
     */
    EFI_DEVICE_PATH_PROTOCOL *dp = FileDevicePath(target_esp,
                                                   SB_DEPLOY_BINARY);
    if (!dp)
        return EFI_OUT_OF_RESOURCES;

    UINTN dp_size = DevicePathSize(dp);
    UINTN desc_size = (StrLen(SB_DEPLOY_LABEL) + 1) * sizeof(CHAR16);
    UINTN opt_size = 4 + 2 + desc_size + dp_size;

    UINT8 *opt = AllocateZeroPool(opt_size);
    if (!opt)
        return EFI_OUT_OF_RESOURCES;

    UINT8 *p = opt;

    /* Attributes: LOAD_OPTION_ACTIVE */
    *(UINT32 *)p = 0x00000001;
    p += 4;

    /* FilePathListLength */
    *(UINT16 *)p = (UINT16)dp_size;
    p += 2;

    /* Description */
    CopyMem(p, SB_DEPLOY_LABEL, desc_size);
    p += desc_size;

    /* Device path */
    CopyMem(p, dp, dp_size);

    CHAR16 varname[32];
    SPrint(varname, sizeof(varname), L"Boot%04X", boot_num);

    EFI_STATUS status = ctx->runtime_services->SetVariable(
                            varname, &gEfiGlobalVariableGuid,
                            EFI_VARIABLE_NON_VOLATILE |
                            EFI_VARIABLE_BOOTSERVICE_ACCESS |
                            EFI_VARIABLE_RUNTIME_ACCESS,
                            opt_size, opt);

    FreePool(opt);
    FreePool(dp);

    if (EFI_ERROR(status))
        return status;

    SB_LOG(L"Created boot entry: %s", varname);

    /*
     * Prepend to BootOrder.
     */
    UINTN order_size = 0;
    ctx->runtime_services->GetVariable(
        L"BootOrder", &gEfiGlobalVariableGuid, NULL, &order_size, NULL);

    UINTN new_order_size = order_size + sizeof(UINT16);
    UINT16 *new_order = AllocatePool(new_order_size);
    if (!new_order)
        return EFI_SUCCESS; /* Non-fatal: entry exists, just not ordered. */

    new_order[0] = boot_num;
    if (order_size > 0) {
        UINT16 *old_order = (UINT16 *)(new_order + 1);
        ctx->runtime_services->GetVariable(
            L"BootOrder", &gEfiGlobalVariableGuid, NULL,
            &order_size, old_order);
    }

    ctx->runtime_services->SetVariable(
        L"BootOrder", &gEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE |
        EFI_VARIABLE_BOOTSERVICE_ACCESS |
        EFI_VARIABLE_RUNTIME_ACCESS,
        new_order_size, new_order);

    FreePool(new_order);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_deploy_to_esp(SuperBootContext *ctx)
{
    SB_LOG(L"\n=== SuperBoot Deployment ===");

    /* Get our own device handle (the USB). */
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_STATUS status = ctx->boot_services->HandleProtocol(
                            ctx->image_handle, &gEfiLoadedImageProtocolGuid,
                            (void **)&loaded);
    SB_CHECK(status, L"Cannot locate self");

    /* Find an internal (non-removable) ESP. */
    EFI_HANDLE esp = find_internal_esp(ctx, loaded->DeviceHandle);
    if (!esp) {
        SB_LOG(L"No internal ESP found. Is there an EFI System Partition?");
        return EFI_NOT_FOUND;
    }

    SB_LOG(L"Found internal ESP. Copying SuperBoot...");

    status = copy_self_to_esp(ctx, esp);
    SB_CHECK(status, L"Failed to copy binary");

    SB_LOG(L"Creating UEFI boot entry...");

    status = create_boot_entry(ctx, esp);
    SB_CHECK(status, L"Failed to create boot entry");

    SB_LOG(L"Deployment complete. SuperBoot is now installed on the internal disk.");
    SB_LOG(L"Press any key to continue...");

    tui_read_key(ctx->system_table);
    return EFI_SUCCESS;
}
