/*
 * menu.c — TUI boot menu
 *
 * Displays the list of discovered BootTargets.  The user can navigate
 * with arrow keys, press Enter to boot, 'e' to edit the command line,
 * 'f' to open the file browser, or 'd' to deploy SuperBoot.
 *
 * If a timeout is set and no key is pressed, the default entry boots
 * automatically.
 */

#include "tui.h"

/* ------------------------------------------------------------------ */
/*  TUI helpers                                                        */
/* ------------------------------------------------------------------ */

UINT16
tui_read_key(EFI_SYSTEM_TABLE *st)
{
    EFI_INPUT_KEY key;
    UINTN index;

    st->BootServices->WaitForEvent(1, &st->ConIn->WaitForKey, &index);
    st->ConIn->ReadKeyStroke(st->ConIn, &key);

    if (key.ScanCode != 0) {
        switch (key.ScanCode) {
        case 0x01: return TUI_KEY_UP;
        case 0x02: return TUI_KEY_DOWN;
        case 0x17: return TUI_KEY_ESCAPE;
        case 0x0B: return TUI_KEY_F1;
        case 0x0C: return TUI_KEY_F2;
        case 0x0F: return TUI_KEY_F5;
        case 0x14: return TUI_KEY_F10;
        default:   return 0;
        }
    }
    return (UINT16)key.UnicodeChar;
}

void
tui_clear(EFI_SYSTEM_TABLE *st, UINTN attr)
{
    st->ConOut->SetAttribute(st->ConOut, attr);
    st->ConOut->ClearScreen(st->ConOut);
}

void
tui_print_centre(EFI_SYSTEM_TABLE *st, UINTN row, const CHAR16 *text)
{
    UINTN cols, rows;
    st->ConOut->QueryMode(st->ConOut, st->ConOut->Mode->Mode, &cols, &rows);

    UINTN len = StrLen(text);
    UINTN col = (len < cols) ? (cols - len) / 2 : 0;

    st->ConOut->SetCursorPosition(st->ConOut, col, row);
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)text);
}

/* ------------------------------------------------------------------ */
/*  Draw the menu                                                      */
/* ------------------------------------------------------------------ */

static void
draw_menu(SuperBootContext *ctx, UINTN selected, UINTN timeout_remaining)
{
    EFI_SYSTEM_TABLE *st = ctx->system_table;
    UINTN cols, rows;
    st->ConOut->QueryMode(st->ConOut, st->ConOut->Mode->Mode, &cols, &rows);

    tui_clear(st, TUI_ATTR_NORMAL);

    /* Header. */
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_HEADER);
    tui_print_centre(st, 0, L"SuperBoot — Universal Meta-Bootloader");

    CHAR16 sub[80];
    SPrint(sub, sizeof(sub), L"%u entries found", ctx->targets.count);
    tui_print_centre(st, 1, sub);

    /* Entry list. */
    UINTN start_row = 3;
    UINTN visible = (rows > start_row + 4) ? rows - start_row - 4 : 1;

    /* Scroll window. */
    UINTN scroll_off = 0;
    if (selected >= visible)
        scroll_off = selected - visible + 1;

    for (UINTN i = 0; i < visible && (scroll_off + i) < ctx->targets.count; i++) {
        UINTN idx = scroll_off + i;
        const BootTarget *t = &ctx->targets.entries[idx];

        if (idx == selected)
            st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_HILITE);
        else
            st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_NORMAL);

        st->ConOut->SetCursorPosition(st->ConOut, 2, start_row + i);

        /* Source tag. */
        const CHAR16 *tag;
        switch (t->config_type) {
        case CONFIG_TYPE_GRUB:        tag = L"[GRUB]";    break;
        case CONFIG_TYPE_SYSTEMD_BOOT: tag = L"[SD-BOOT]"; break;
        case CONFIG_TYPE_LIMINE:      tag = L"[LIMINE]";  break;
        default:                      tag = L"[???]";     break;
        }

        CHAR16 line[256];
        SPrint(line, sizeof(line), L" %s %s", tag, t->title);

        /* Pad to fill the row. */
        UINTN llen = StrLen(line);
        while (llen + 3 < cols && llen + 1 < 256) {
            line[llen++] = L' ';
        }
        line[llen] = L'\0';

        st->ConOut->OutputString(st->ConOut, line);
    }

    /* Footer / help. */
    st->ConOut->SetAttribute(st->ConOut, TUI_ATTR_HEADER);
    st->ConOut->SetCursorPosition(st->ConOut, 0, rows - 2);
    st->ConOut->OutputString(
        st->ConOut,
        L" [Enter] Boot  [e] Edit cmdline  [f] File browser  [d] Deploy  [Esc] Reboot");

    if (timeout_remaining > 0) {
        CHAR16 tbuf[64];
        SPrint(tbuf, sizeof(tbuf),
               L" Auto-boot in %u seconds...", timeout_remaining);
        st->ConOut->SetCursorPosition(st->ConOut, 0, rows - 1);
        st->ConOut->OutputString(st->ConOut, tbuf);
    }
}

/* ------------------------------------------------------------------ */
/*  Inline command-line editor                                         */
/* ------------------------------------------------------------------ */

static void
edit_cmdline(SuperBootContext *ctx, BootTarget *target)
{
    EFI_SYSTEM_TABLE *st = ctx->system_table;

    tui_clear(st, TUI_ATTR_NORMAL);
    st->ConOut->SetCursorPosition(st->ConOut, 0, 0);
    Print(L"Edit kernel command line for: %s\n\n", target->title);
    Print(L"Current: %a\n\n", target->cmdline);
    Print(L"Enter new command line (empty = keep current):\n> ");

    /* Simple line input. */
    CHAR8  buf[SB_MAX_CMDLINE];
    UINTN  pos = 0;

    for (;;) {
        UINT16 key = tui_read_key(st);

        if (key == TUI_KEY_ESCAPE)
            return; /* Cancel. */

        if (key == TUI_KEY_ENTER || key == '\r' || key == '\n') {
            buf[pos] = '\0';
            if (pos > 0)
                CopyMem(target->cmdline, buf, pos + 1);
            return;
        }

        if (key == 0x08 /* backspace */) {
            if (pos > 0) {
                pos--;
                Print(L"\b \b");
            }
            continue;
        }

        if (key >= 0x20 && key < 0x7F && pos + 1 < SB_MAX_CMDLINE) {
            buf[pos++] = (CHAR8)key;
            CHAR16 ch[2] = { (CHAR16)key, 0 };
            st->ConOut->OutputString(st->ConOut, ch);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Main menu loop                                                     */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_tui_run_menu(SuperBootContext *ctx)
{
    if (ctx->targets.count == 0)
        return EFI_NOT_FOUND;

    /* Find the default entry. */
    UINTN selected = 0;
    for (UINTN i = 0; i < ctx->targets.count; i++) {
        if (ctx->targets.entries[i].is_default) {
            selected = i;
            break;
        }
    }

    UINTN timeout = ctx->timeout_sec;

    for (;;) {
        draw_menu(ctx, selected, timeout);

        /* Wait for key with 1-second timeout for countdown. */
        if (timeout > 0) {
            EFI_EVENT timer;
            ctx->boot_services->CreateEvent(
                EVT_TIMER, 0, NULL, NULL, &timer);
            ctx->boot_services->SetTimer(
                timer, TimerRelative, 10000000); /* 1 second */

            EFI_EVENT events[2] = {
                ctx->system_table->ConIn->WaitForKey,
                timer
            };
            UINTN index;
            ctx->boot_services->WaitForEvent(2, events, &index);
            ctx->boot_services->CloseEvent(timer);

            if (index == 1) {
                /* Timer fired, no key pressed. */
                timeout--;
                if (timeout == 0) {
                    ctx->selected = selected;
                    return EFI_SUCCESS;
                }
                continue;
            }
            /* Key was pressed — cancel timeout. */
            timeout = 0;
        }

        UINT16 key = tui_read_key(ctx->system_table);

        switch (key) {
        case TUI_KEY_UP:
            if (selected > 0) selected--;
            break;

        case TUI_KEY_DOWN:
            if (selected + 1 < ctx->targets.count) selected++;
            break;

        case TUI_KEY_ENTER:
            ctx->selected = selected;
            return EFI_SUCCESS;

        case 'e':
        case 'E':
            edit_cmdline(ctx, &ctx->targets.entries[selected]);
            break;

        case 'f':
        case 'F':
            sb_tui_file_browser(ctx);
            break;

        case 'd':
        case 'D':
            sb_deploy_to_esp(ctx);
            break;

        case TUI_KEY_ESCAPE:
            /* Reboot. */
            ctx->runtime_services->ResetSystem(
                EfiResetCold, EFI_SUCCESS, 0, NULL);
            break;
        }
    }
}
