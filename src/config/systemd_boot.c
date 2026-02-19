/*
 * systemd_boot.c — systemd-boot (loader.conf + entry .conf) parser
 *
 * systemd-boot stores its config on the ESP:
 *   /loader/loader.conf          — global settings (default, timeout)
 *   /loader/entries/<name>.conf  — one file per boot entry
 *
 * Each entry file is a simple key-value format:
 *   title    Arch Linux
 *   linux    /vmlinuz-linux
 *   initrd   /initramfs-linux.img
 *   options  root=UUID=xxxx rw quiet
 *
 * This is the simplest parser — no scripting, no variables.
 */

#include "config.h"

/* ------------------------------------------------------------------ */
/*  Parse a single entry .conf file                                    */
/* ------------------------------------------------------------------ */

static EFI_STATUS
parse_entry_file(const CHAR8 *data, UINTN size,
                 EFI_HANDLE device, const CHAR16 *config_path,
                 BootTarget *target)
{
    (void)size;

    SetMem(target, sizeof(*target), 0);
    target->config_type = CONFIG_TYPE_SYSTEMD_BOOT;
    target->device_handle = device;
    CopyMem(target->config_path, (void *)config_path,
            StrLen(config_path) * sizeof(CHAR16) + 2);

    CHAR8 *p = (CHAR8 *)data;
    CHAR8  key[64], value[SB_MAX_CMDLINE];

    while (*p) {
        p = sb_skip_whitespace(p);
        if (*p == '#' || *p == '\n') {
            p = sb_next_line(p);
            continue;
        }

        /* Read key. */
        UINTN ki = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
               ki + 1 < sizeof(key))
            key[ki++] = *p++;
        key[ki] = '\0';

        /* Read value (rest of line). */
        p = sb_skip_whitespace(p);
        UINTN vi = 0;
        while (*p && *p != '\n' && vi + 1 < sizeof(value))
            value[vi++] = *p++;
        value[vi] = '\0';

        /* Trim trailing whitespace. */
        while (vi > 0 && (value[vi-1] == ' ' || value[vi-1] == '\t'))
            value[--vi] = '\0';

        if (sb_strcmp8(key, "title") == 0) {
            sb_str8to16(target->title, value, SB_MAX_TITLE);
        }
        else if (sb_strcmp8(key, "linux") == 0) {
            sb_str8to16(target->kernel_path, value, SB_MAX_PATH);
            /* Convert forward slashes to backslashes. */
            for (CHAR16 *c = target->kernel_path; *c; c++)
                if (*c == L'/') *c = L'\\';
        }
        else if (sb_strcmp8(key, "initrd") == 0) {
            if (target->initrd_count < SB_MAX_INITRDS) {
                sb_str8to16(
                    target->initrd_paths[target->initrd_count],
                    value, SB_MAX_PATH);
                for (CHAR16 *c = target->initrd_paths[target->initrd_count]; *c; c++)
                    if (*c == L'/') *c = L'\\';
                target->initrd_count++;
            }
        }
        else if (sb_strcmp8(key, "options") == 0) {
            sb_strcpy8(target->cmdline, value, SB_MAX_CMDLINE);
        }
        else if (sb_strcmp8(key, "efi") == 0) {
            sb_str8to16(target->efi_path, value, SB_MAX_PATH);
            for (CHAR16 *c = target->efi_path; *c; c++)
                if (*c == L'/') *c = L'\\';
            target->is_chainload = TRUE;
        }

        p = sb_next_line(p);
    }

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Main parser: scan /loader/entries/ for .conf files                  */
/*                                                                     */
/*  Note: This parser is unusual because it needs to read *multiple*   */
/*  files.  The scanner calls parse() with the loader.conf contents,   */
/*  but we also need to enumerate /loader/entries/.  We use the VFS    */
/*  to read entry files from the same device.                          */
/* ------------------------------------------------------------------ */

static EFI_STATUS
systemd_boot_parse(const CHAR8 *config_data, UINTN config_size,
                   EFI_HANDLE device, const CHAR16 *config_path,
                   BootTarget *targets, UINTN *count, UINTN max)
{
    (void)config_size;
    *count = 0;

    /*
     * Parse loader.conf for global settings.
     * We look for "default" and "timeout" lines.
     */
    CHAR8 default_pattern[256] = {0};
    CHAR8 *p = (CHAR8 *)config_data;

    while (*p) {
        p = sb_skip_whitespace(p);
        if (sb_starts_with8(p, "default")) {
            p += 7;
            p = sb_skip_whitespace(p);
            UINTN i = 0;
            while (*p && *p != '\n' && i + 1 < sizeof(default_pattern))
                default_pattern[i++] = *p++;
            default_pattern[i] = '\0';
        }
        p = sb_next_line(p);
    }

    /*
     * Enumerate /loader/entries/ .conf files on this device.
     * We use the UEFI SimpleFileSystem protocol to list the directory.
     */
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_STATUS status;

    status = gBS->HandleProtocol(device,
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (void **)&fs);
    if (EFI_ERROR(status))
        return EFI_SUCCESS; /* No filesystem — nothing to parse. */

    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status))
        return EFI_SUCCESS;

    EFI_FILE_PROTOCOL *entries_dir;
    status = root->Open(root, &entries_dir,
                        L"\\loader\\entries",
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        root->Close(root);
        return EFI_SUCCESS;
    }

    /* Read each entry in the directory. */
    UINT8 info_buf[512];
    UINTN buf_size;

    for (;;) {
        buf_size = sizeof(info_buf);
        status = entries_dir->Read(entries_dir, &buf_size, info_buf);
        if (EFI_ERROR(status) || buf_size == 0)
            break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
        if (info->Attribute & EFI_FILE_DIRECTORY)
            continue;

        /* Check for .conf extension. */
        UINTN name_len = StrLen(info->FileName);
        if (name_len < 6)
            continue;
        if (StriCmp(info->FileName + name_len - 5, L".conf") != 0)
            continue;

        /* Read the entry file. */
        EFI_FILE_PROTOCOL *entry_file;
        status = entries_dir->Open(entries_dir, &entry_file,
                                   info->FileName,
                                   EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status))
            continue;

        UINTN file_size = (UINTN)info->FileSize;
        CHAR8 *file_data = AllocatePool(file_size + 1);
        if (!file_data) {
            entry_file->Close(entry_file);
            continue;
        }

        status = entry_file->Read(entry_file, &file_size, file_data);
        entry_file->Close(entry_file);
        if (EFI_ERROR(status)) {
            FreePool(file_data);
            continue;
        }
        file_data[file_size] = '\0';

        /* Build entry path for provenance tracking. */
        CHAR16 entry_path[SB_MAX_PATH];
        SPrint(entry_path, sizeof(entry_path),
               L"\\loader\\entries\\%s", info->FileName);

        /* Parse the entry. */
        if (*count < max) {
            parse_entry_file(file_data, file_size, device,
                             entry_path, &targets[*count]);
            targets[*count].index = (UINT32)*count;

            /* Mark default entry. */
            if (default_pattern[0]) {
                CHAR8 fname8[256];
                sb_str16to8(fname8, info->FileName, sizeof(fname8));
                if (sb_strstr8(fname8, default_pattern))
                    targets[*count].is_default = TRUE;
            }

            /* Only count entries that have a kernel or chainload. */
            if (targets[*count].kernel_path[0] ||
                targets[*count].is_chainload)
                (*count)++;
        }

        FreePool(file_data);
    }

    entries_dir->Close(entries_dir);
    root->Close(root);

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Config paths to probe                                              */
/* ------------------------------------------------------------------ */

static const CHAR16 *sd_boot_paths[] = {
    L"\\loader\\loader.conf",
    NULL
};

/* ------------------------------------------------------------------ */
/*  Exported parser descriptor                                         */
/* ------------------------------------------------------------------ */

ConfigParser sb_parser_systemd_boot = {
    .name         = L"systemd-boot",
    .type         = CONFIG_TYPE_SYSTEMD_BOOT,
    .config_paths = sd_boot_paths,
    .parse        = systemd_boot_parse,
};
