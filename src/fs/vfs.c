/*
 * vfs.c — Virtual Filesystem dispatcher
 *
 * Manages a table of mounted devices with their associated drivers.
 * Falls through from UEFI-native SimpleFileSystem to built-in drivers.
 */

#include "vfs.h"

/* ------------------------------------------------------------------ */
/*  Mount table                                                        */
/* ------------------------------------------------------------------ */

#define VFS_MAX_MOUNTS 64

typedef struct {
    EFI_HANDLE   device;
    BOOLEAN      is_native;     /* Using UEFI SimpleFileSystem?       */
    VfsDriver   *driver;        /* Non-NULL only for built-in drivers */
    void        *fs_context;    /* Opaque driver state                */
} VfsMount;

static VfsMount  mounts[VFS_MAX_MOUNTS];
static UINTN     mount_count = 0;

/* Built-in filesystem driver table. */
static VfsDriver *builtin_drivers[] = {
    &sb_vfs_ext4,
    &sb_vfs_btrfs,
    &sb_vfs_xfs,
    &sb_vfs_ntfs,
    NULL
};

/* ------------------------------------------------------------------ */
/*  Initialisation                                                     */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_vfs_init(SuperBootContext *ctx)
{
    mount_count = 0;

    /* Attempt to load external .efi FS drivers from our own dir. */
    sb_vfs_load_external_drivers(ctx);

    return EFI_SUCCESS;
}

void
sb_vfs_shutdown(void)
{
    for (UINTN i = 0; i < mount_count; i++) {
        if (!mounts[i].is_native && mounts[i].driver && mounts[i].fs_context)
            mounts[i].driver->unmount(mounts[i].fs_context);
    }
    mount_count = 0;
}

/* ------------------------------------------------------------------ */
/*  Load external .efi filesystem drivers                              */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_vfs_load_external_drivers(SuperBootContext *ctx)
{
    /*
     * Scan the directory containing the SuperBoot binary for files
     * matching *_fs.efi or *_drv.efi, and load them via
     * BootServices->LoadImage / StartImage.  These drivers register
     * themselves as EFI_SIMPLE_FILE_SYSTEM_PROTOCOL providers.
     */
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_STATUS status;

    status = ctx->boot_services->HandleProtocol(
                 ctx->image_handle, &gEfiLoadedImageProtocolGuid,
                 (void **)&loaded);
    if (EFI_ERROR(status))
        return status;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    status = ctx->boot_services->HandleProtocol(
                 loaded->DeviceHandle,
                 &gEfiSimpleFileSystemProtocolGuid,
                 (void **)&fs);
    if (EFI_ERROR(status))
        return status;

    EFI_FILE_PROTOCOL *root;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status))
        return status;

    /* Open our drivers/ subdirectory. */
    EFI_FILE_PROTOCOL *drv_dir;
    status = root->Open(root, &drv_dir,
                        L"\\EFI\\superboot\\drivers",
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        root->Close(root);
        return EFI_SUCCESS; /* No drivers directory — not an error. */
    }

    UINT8 info_buf[512];
    UINTN buf_size;
    UINTN loaded_count = 0;

    for (;;) {
        buf_size = sizeof(info_buf);
        status = drv_dir->Read(drv_dir, &buf_size, info_buf);
        if (EFI_ERROR(status) || buf_size == 0)
            break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
        if (info->Attribute & EFI_FILE_DIRECTORY)
            continue;

        /* Check for .efi extension. */
        UINTN name_len = StrLen(info->FileName);
        if (name_len < 5)
            continue;
        if (StriCmp(info->FileName + name_len - 4, L".efi") != 0)
            continue;

        /* Build device path for the driver. */
        EFI_DEVICE_PATH_PROTOCOL *dev_path = FileDevicePath(
            loaded->DeviceHandle,
            PoolPrint(L"\\EFI\\superboot\\drivers\\%s", info->FileName));
        if (!dev_path)
            continue;

        EFI_HANDLE drv_handle;
        status = ctx->boot_services->LoadImage(
                     FALSE, ctx->image_handle, dev_path,
                     NULL, 0, &drv_handle);
        if (EFI_ERROR(status))
            continue;

        status = ctx->boot_services->StartImage(drv_handle, NULL, NULL);
        if (EFI_ERROR(status)) {
            ctx->boot_services->UnloadImage(drv_handle);
            continue;
        }

        SB_DBG(ctx, L"Loaded FS driver: %s", info->FileName);
        loaded_count++;
    }

    drv_dir->Close(drv_dir);
    root->Close(root);

    if (loaded_count > 0) {
        /* Reconnect all block I/O handles so new drivers bind. */
        ctx->boot_services->ConnectController(NULL, NULL, NULL, TRUE);
    }

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Open/mount a device                                                */
/* ------------------------------------------------------------------ */

static VfsMount *
find_mount(EFI_HANDLE device)
{
    for (UINTN i = 0; i < mount_count; i++) {
        if (mounts[i].device == device)
            return &mounts[i];
    }
    return NULL;
}

EFI_STATUS
sb_vfs_open_device(EFI_HANDLE device)
{
    if (find_mount(device))
        return EFI_SUCCESS; /* Already mounted. */

    if (mount_count >= VFS_MAX_MOUNTS)
        return EFI_OUT_OF_RESOURCES;

    VfsMount *m = &mounts[mount_count];
    SetMem(m, sizeof(*m), 0);
    m->device = device;

    /* Try UEFI-native SimpleFileSystem first. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_STATUS status = gBS->HandleProtocol(
                            device, &gEfiSimpleFileSystemProtocolGuid,
                            (void **)&sfs);
    if (!EFI_ERROR(status)) {
        m->is_native = TRUE;
        mount_count++;
        return EFI_SUCCESS;
    }

    /* Fall back to built-in drivers via Block I/O. */
    EFI_BLOCK_IO_PROTOCOL *block_io;
    EFI_DISK_IO_PROTOCOL  *disk_io;

    status = gBS->HandleProtocol(device, &gEfiBlockIoProtocolGuid,
                                 (void **)&block_io);
    if (EFI_ERROR(status))
        return status;

    status = gBS->HandleProtocol(device, &gEfiDiskIoProtocolGuid,
                                 (void **)&disk_io);
    if (EFI_ERROR(status))
        disk_io = NULL; /* Some firmwares don't provide Disk I/O. */

    for (VfsDriver **drv = builtin_drivers; *drv; drv++) {
        if ((*drv)->probe && !EFI_ERROR((*drv)->probe(block_io, disk_io))) {
            void *ctx = NULL;
            status = (*drv)->mount(block_io, disk_io, &ctx);
            if (!EFI_ERROR(status)) {
                m->is_native = FALSE;
                m->driver = *drv;
                m->fs_context = ctx;
                mount_count++;
                return EFI_SUCCESS;
            }
        }
    }

    return EFI_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/*  Read a file from a mounted device                                  */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_vfs_read_file(EFI_HANDLE device, const CHAR16 *path,
                 void **buffer, UINTN *size)
{
    VfsMount *m = find_mount(device);
    if (!m) {
        /* Auto-mount on first access. */
        EFI_STATUS s = sb_vfs_open_device(device);
        if (EFI_ERROR(s))
            return s;
        m = find_mount(device);
        if (!m)
            return EFI_NOT_FOUND;
    }

    /* UEFI-native path: use SimpleFileSystem. */
    if (m->is_native) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
        EFI_STATUS status = gBS->HandleProtocol(
                                device, &gEfiSimpleFileSystemProtocolGuid,
                                (void **)&sfs);
        if (EFI_ERROR(status))
            return status;

        EFI_FILE_PROTOCOL *root, *file;
        status = sfs->OpenVolume(sfs, &root);
        if (EFI_ERROR(status))
            return status;

        status = root->Open(root, &file, (CHAR16 *)path,
                            EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status)) {
            root->Close(root);
            return status;
        }

        /* Get file size. */
        UINT8 info_buf[256];
        UINTN info_size = sizeof(info_buf);
        status = file->GetInfo(file, &gEfiFileInfoGuid,
                               &info_size, info_buf);
        if (EFI_ERROR(status)) {
            file->Close(file);
            root->Close(root);
            return status;
        }

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
        *size = (UINTN)info->FileSize;
        *buffer = AllocatePool(*size + 1);
        if (!*buffer) {
            file->Close(file);
            root->Close(root);
            return EFI_OUT_OF_RESOURCES;
        }

        status = file->Read(file, size, *buffer);
        ((UINT8 *)*buffer)[*size] = 0; /* NUL-terminate for text files. */

        file->Close(file);
        root->Close(root);
        return status;
    }

    /* Built-in driver path. */
    if (m->driver && m->driver->read_file)
        return m->driver->read_file(m->fs_context, path, buffer, size);

    return EFI_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/*  File existence probe                                               */
/* ------------------------------------------------------------------ */

BOOLEAN
sb_vfs_file_exists(EFI_HANDLE device, const CHAR16 *path)
{
    void  *buf = NULL;
    UINTN  sz  = 0;

    /* For existence checks on native FS, we can try Open + Close
     * without reading the full file.  For built-in drivers we do
     * a full read — acceptable since config files are small. */
    VfsMount *m = find_mount(device);
    if (!m)
        sb_vfs_open_device(device);
    m = find_mount(device);
    if (!m)
        return FALSE;

    if (m->is_native) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
        if (EFI_ERROR(gBS->HandleProtocol(device,
                &gEfiSimpleFileSystemProtocolGuid, (void **)&sfs)))
            return FALSE;

        EFI_FILE_PROTOCOL *root, *file;
        if (EFI_ERROR(sfs->OpenVolume(sfs, &root)))
            return FALSE;

        EFI_STATUS s = root->Open(root, &file, (CHAR16 *)path,
                                  EFI_FILE_MODE_READ, 0);
        root->Close(root);
        if (EFI_ERROR(s))
            return FALSE;
        file->Close(file);
        return TRUE;
    }

    EFI_STATUS s = sb_vfs_read_file(device, path, &buf, &sz);
    if (buf)
        FreePool(buf);
    return !EFI_ERROR(s);
}
