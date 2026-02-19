/*
 * explorer.c — EFI file browser / explorer
 *
 * Presents a navigable view of all mounted partitions and their
 * contents.  The user can browse directories, view file info,
 * and launch .efi binaries directly.
 */

#include "tui.h"
#include "../fs/vfs.h"

/* ------------------------------------------------------------------ */
/*  Directory entry list                                               */
/* ------------------------------------------------------------------ */

#define EXPLORER_MAX_ENTRIES 256

typedef struct {
    CHAR16   name[256];
    BOOLEAN  is_dir;
    UINT64   size;
} ExplorerEntry;

static ExplorerEntry entries[EXPLORER_MAX_ENTRIES];
static UINTN         entry_count;

/* ------------------------------------------------------------------ */
/*  Read directory contents via UEFI SimpleFileSystem                  */
/* ------------------------------------------------------------------ */

static EFI_STATUS
read_directory(EFI_HANDLE device, const CHAR16 *dir_path)
{
    entry_count = 0;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_STATUS status = gBS->HandleProtocol(
                            device, &gEfiSimpleFileSystemProtocolGuid,
                            (void **)&sfs);
    if (EFI_ERROR(status))
        return status;

    EFI_FILE_PROTOCOL *root, *dir;
    status = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(status))
        return status;

    status = root->Open(root, &dir, (CHAR16 *)dir_path,
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        root->Close(root);
        return status;
    }

    /* Add ".." entry for parent navigation. */
    StrCpy(entries[0].name, L"..");
    entries[0].is_dir = TRUE;
    entries[0].size = 0;
    entry_count = 1;

    UINT8 info_buf[1024];
    UINTN buf_size;

    for (;;) {
        buf_size = sizeof(info_buf);
        status = dir->Read(dir, &buf_size, info_buf);
        if (EFI_ERROR(status) || buf_size == 0)
            break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;

        /* Skip "." entries. */
        if (StrCmp(info->FileName, L".") == 0)
            continue;
        if (StrCmp(info->FileName, L"..") == 0)
            continue;

        if (entry_count >= EXPLORER_MAX_ENTRIES)
            break;

        StrCpy(entries[entry_count].name, info->FileName);
        entries[entry_count].is_dir = !!(info->Attribute & EFI_FILE_DIRECTORY);
        entries[entry_count].size = info->FileSize;
        entry_count++;
    }

    dir->Close(dir);
    root->Close(root);
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Draw the file browser                                              */
/* ------------------------------------------------------------------ */

static void
draw_browser(EFI_SYSTEM_TABLE *st, const CHAR16 *path,
             UINTN selected, UINTN scroll_off)
{
    UINTN cols, rows;
    st->ConOut->QueryMode(st->ConOut, st->ConOut->Mode->Mode, &cols, &rows);

    tui_clear(st, TUI_ATTR_NORMAL);

    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_HEADER);
    tui_print_centre(st, 0, L"SuperBoot — EFI File Explorer");
    st->ConOut->SetCursorPosition(st->ConOut, 1, 1);
    Print(L"Path: %s", path);

    UINTN start_row = 3;
    UINTN visible = (rows > start_row + 3) ? rows - start_row - 3 : 1;

    for (UINTN i = 0; i < visible && (scroll_off + i) < entry_count; i++) {
        UINTN idx = scroll_off + i;

        if (idx == selected)
            st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_HILITE);
        else
            st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_NORMAL);

        st->ConOut->SetCursorPosition(st->ConOut, 2, start_row + i);

        if (entries[idx].is_dir) {
            Print(L" [DIR]  %s", entries[idx].name);
        } else {
            Print(L" %10lu  %s", entries[idx].size, entries[idx].name);
        }
    }

    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_HEADER);
    st->ConOut->SetCursorPosition(st->ConOut, 0, rows - 2);
    st->ConOut->OutputString(
        st->ConOut,
        L" [Enter] Open/Run  [Backspace] Up  [Esc] Back to menu");
}

/* ------------------------------------------------------------------ */
/*  Launch an .efi binary                                              */
/* ------------------------------------------------------------------ */

static EFI_STATUS
launch_efi(SuperBootContext *ctx, EFI_HANDLE device, const CHAR16 *path)
{
    Print(L"\nLaunching %s ...\n", path);

    EFI_DEVICE_PATH_PROTOCOL *dp = FileDevicePath(device, (CHAR16 *)path);
    if (!dp)
        return EFI_OUT_OF_RESOURCES;

    EFI_HANDLE child;
    EFI_STATUS status = ctx->boot_services->LoadImage(
                            FALSE, ctx->image_handle, dp,
                            NULL, 0, &child);
    if (EFI_ERROR(status))
        return status;

    return ctx->boot_services->StartImage(child, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Check if filename ends with .efi                                   */
/* ------------------------------------------------------------------ */

static BOOLEAN
is_efi_file(const CHAR16 *name)
{
    UINTN len = StrLen(name);
    if (len < 5)
        return FALSE;
    return StriCmp(name + len - 4, L".efi") == 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_tui_file_browser(SuperBootContext *ctx)
{
    /*
     * Start by listing all partitions with SimpleFileSystem as
     * top-level "drives."  The user picks one, then browses its tree.
     */
    EFI_HANDLE *fs_handles = NULL;
    UINTN       fs_count = 0;

    ctx->boot_services->LocateHandleBuffer(
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
        NULL, &fs_count, &fs_handles);

    if (fs_count == 0) {
        Print(L"No accessible filesystems found.\n");
        tui_read_key(ctx->system_table);
        return EFI_NOT_FOUND;
    }

    /* For now, use the first filesystem as root.
     * TODO: show partition picker. */
    EFI_HANDLE device = fs_handles[0];
    FreePool(fs_handles);

    CHAR16 current_path[SB_MAX_PATH] = L"\\";
    UINTN  selected = 0;

    for (;;) {
        EFI_STATUS status = read_directory(device, current_path);
        if (EFI_ERROR(status)) {
            Print(L"Cannot read directory: %r\n", status);
            tui_read_key(ctx->system_table);
            return status;
        }

        UINTN scroll_off = 0;

        for (;;) {
            if (selected >= entry_count && entry_count > 0)
                selected = entry_count - 1;
            if (selected >= scroll_off + 20)
                scroll_off = selected - 19;
            if (selected < scroll_off)
                scroll_off = selected;

            draw_browser(ctx->system_table, current_path,
                         selected, scroll_off);

            UINT16 key = tui_read_key(ctx->system_table);

            if (key == TUI_KEY_ESCAPE)
                return EFI_SUCCESS;

            if (key == TUI_KEY_UP && selected > 0)
                selected--;
            else if (key == TUI_KEY_DOWN && selected + 1 < entry_count)
                selected++;
            else if (key == TUI_KEY_ENTER && entry_count > 0) {
                ExplorerEntry *e = &entries[selected];

                if (e->is_dir) {
                    if (StrCmp(e->name, L"..") == 0) {
                        /* Go up: strip last path component. */
                        CHAR16 *last = current_path;
                        CHAR16 *sep = NULL;
                        for (CHAR16 *c = current_path; *c; c++) {
                            if (*c == L'\\' && *(c+1) != L'\0')
                                sep = c;
                        }
                        if (sep && sep != current_path)
                            *(sep + 1) = L'\0';
                        else
                            StrCpy(current_path, L"\\");
                        (void)last;
                    } else {
                        /* Descend into directory. */
                        UINTN len = StrLen(current_path);
                        if (len > 1) /* Not just "\\" */
                            StrCat(current_path, L"\\");
                        StrCat(current_path, e->name);
                    }
                    selected = 0;
                    break; /* Re-read directory. */
                }

                /* It's a file.  If .efi, offer to launch it. */
                if (is_efi_file(e->name)) {
                    CHAR16 full[SB_MAX_PATH];
                    SPrint(full, sizeof(full), L"%s\\%s",
                           current_path, e->name);
                    launch_efi(ctx, device, full);
                    /* If it returns, redraw. */
                }
            }
            else if (key == 0x08 /* backspace */) {
                /* Same as selecting ".." */
                CHAR16 *sep = NULL;
                for (CHAR16 *c = current_path; *c; c++) {
                    if (*c == L'\\' && *(c+1) != L'\0')
                        sep = c;
                }
                if (sep && sep != current_path)
                    *(sep + 1) = L'\0';
                else
                    StrCpy(current_path, L"\\");
                selected = 0;
                break;
            }
        }
    }
}
