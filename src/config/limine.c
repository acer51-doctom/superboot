/*
 * limine.c — Limine bootloader config parser
 *
 * Limine uses a simple key:value format with section headers:
 *
 *   timeout: 5
 *
 *   /Arch Linux
 *       protocol: linux
 *       kernel_path: boot():/boot/vmlinuz-linux
 *       kernel_cmdline: root=UUID=xxxx rw
 *       module_path: boot():/boot/initramfs-linux.img
 *
 * Sections start with /<title> and contain indented key: value pairs.
 * The `boot()` device specifier refers to the boot device (ESP).
 */

#include "config.h"

/* ------------------------------------------------------------------ */
/*  Path translation: strip Limine device prefixes                     */
/*                                                                     */
/*  Limine paths look like:                                            */
/*    boot():/path      — boot device                                  */
/*    guid(XXXX):/path  — partition by GUID                            */
/*    /path             — relative to config root                      */
/* ------------------------------------------------------------------ */

static void
limine_path_to_uefi(const CHAR8 *src, CHAR16 *dst, UINTN max)
{
    const CHAR8 *p = src;

    /* Strip device prefix: skip past "):" */
    const CHAR8 *colon = sb_strstr8(p, "):");
    if (colon)
        p = colon + 2;

    /* Convert to wide + backslash. */
    UINTN i = 0;
    if (*p != '/' && *p != '\\' && i + 1 < max)
        dst[i++] = L'\\';
    while (*p && i + 1 < max) {
        dst[i++] = (*p == '/') ? L'\\' : (CHAR16)*p;
        p++;
    }
    dst[i] = L'\0';
}

/* ------------------------------------------------------------------ */
/*  Main parser                                                        */
/* ------------------------------------------------------------------ */

static EFI_STATUS
limine_parse(const CHAR8 *config_data, UINTN config_size,
             EFI_HANDLE device, const CHAR16 *config_path,
             BootTarget *targets, UINTN *count, UINTN max)
{
    (void)config_size;
    *count = 0;

    CHAR8 *p = (CHAR8 *)config_data;
    BootTarget *cur = NULL;
    BOOLEAN in_section = FALSE;

    while (*p) {
        p = sb_skip_whitespace(p);

        /* Skip blank lines and comments. */
        if (*p == '\n') { p++; continue; }
        if (*p == '#')  { p = sb_next_line(p); continue; }

        /* Section header: /Title */
        if (*p == '/' && !in_section) {
            p++; /* skip the '/' */
            if (*count >= max) break;

            cur = &targets[*count];
            SetMem(cur, sizeof(*cur), 0);
            cur->config_type = CONFIG_TYPE_LIMINE;
            cur->device_handle = device;
            CopyMem(cur->config_path, config_path,
                    StrLen(config_path) * sizeof(CHAR16) + 2);
            cur->index = (UINT32)*count;

            /* Title is the rest of the line. */
            CHAR8 title[SB_MAX_TITLE];
            UINTN ti = 0;
            while (*p && *p != '\n' && ti + 1 < sizeof(title))
                title[ti++] = *p++;
            title[ti] = '\0';
            sb_str8to16(cur->title, title, SB_MAX_TITLE);

            in_section = TRUE;
            p = sb_next_line(p);
            continue;
        }

        /* New section also closes the previous one. */
        if (*p == '/' && in_section && cur) {
            if (cur->kernel_path[0] || cur->is_chainload)
                (*count)++;
            in_section = FALSE;
            cur = NULL;
            continue; /* Re-process this line as a new section. */
        }

        /* Key: value pair (must be indented if in a section). */
        if (in_section && cur) {
            CHAR8 key[128];
            UINTN ki = 0;
            while (*p && *p != ':' && *p != '\n' && ki + 1 < sizeof(key))
                key[ki++] = *p++;
            key[ki] = '\0';

            if (*p == ':') p++;
            p = sb_skip_whitespace(p);

            CHAR8 value[SB_MAX_CMDLINE];
            UINTN vi = 0;
            while (*p && *p != '\n' && vi + 1 < sizeof(value))
                value[vi++] = *p++;
            value[vi] = '\0';
            /* Trim trailing whitespace. */
            while (vi > 0 && (value[vi-1] == ' ' || value[vi-1] == '\t'))
                value[--vi] = '\0';

            if (sb_strcmp8(key, "kernel_path") == 0) {
                limine_path_to_uefi(value, cur->kernel_path, SB_MAX_PATH);
            }
            else if (sb_strcmp8(key, "kernel_cmdline") == 0 ||
                     sb_strcmp8(key, "cmdline") == 0) {
                sb_strcpy8(cur->cmdline, value, SB_MAX_CMDLINE);
            }
            else if (sb_strcmp8(key, "module_path") == 0) {
                if (cur->initrd_count < SB_MAX_INITRDS) {
                    limine_path_to_uefi(
                        value,
                        cur->initrd_paths[cur->initrd_count],
                        SB_MAX_PATH);
                    cur->initrd_count++;
                }
            }
            else if (sb_strcmp8(key, "protocol") == 0) {
                if (sb_strcmp8(value, "chainload") == 0)
                    cur->is_chainload = TRUE;
            }
            else if (sb_strcmp8(key, "path") == 0 ||
                     sb_strcmp8(key, "image_path") == 0) {
                limine_path_to_uefi(value, cur->efi_path, SB_MAX_PATH);
                cur->is_chainload = TRUE;
            }
        }

        p = sb_next_line(p);
    }

    /* Close last section. */
    if (in_section && cur &&
        (cur->kernel_path[0] || cur->is_chainload))
        (*count)++;

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Config paths to probe                                              */
/* ------------------------------------------------------------------ */

static const CHAR16 *limine_paths[] = {
    L"\\limine.cfg",
    L"\\boot\\limine\\limine.cfg",
    L"\\EFI\\BOOT\\limine.cfg",
    NULL
};

/* ------------------------------------------------------------------ */
/*  Exported parser descriptor                                         */
/* ------------------------------------------------------------------ */

ConfigParser sb_parser_limine = {
    .name         = L"Limine",
    .type         = CONFIG_TYPE_LIMINE,
    .config_paths = limine_paths,
    .parse        = limine_parse,
};
