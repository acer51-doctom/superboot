/*
 * tui.h — Text User Interface
 *
 * Uses UEFI Simple Text Output Protocol (and optionally Graphics
 * Output Protocol for box-drawing) to render the boot menu and
 * file browser.
 */

#ifndef SUPERBOOT_TUI_H
#define SUPERBOOT_TUI_H

#include "../superboot.h"

/* Key codes beyond simple ASCII. */
#define TUI_KEY_UP      0x0001
#define TUI_KEY_DOWN    0x0002
#define TUI_KEY_ENTER   0x000D
#define TUI_KEY_ESCAPE  0x0017
#define TUI_KEY_TAB     0x0009
#define TUI_KEY_F1      0x000B
#define TUI_KEY_F2      0x000C
#define TUI_KEY_F5      0x000F
#define TUI_KEY_F10     0x0014

/* Colours (UEFI text mode attributes). */
#define TUI_FG_WHITE    EFI_WHITE
#define TUI_FG_CYAN     EFI_CYAN
#define TUI_FG_YELLOW   EFI_YELLOW
#define TUI_BG_BLUE     (EFI_BACKGROUND_BLUE)
#define TUI_BG_BLACK    (EFI_BACKGROUND_BLACK)

#define TUI_ATTR_NORMAL (TUI_FG_WHITE  | TUI_BG_BLUE)
#define TUI_ATTR_HILITE (TUI_FG_YELLOW | TUI_BG_BLACK)
#define TUI_ATTR_HEADER (TUI_FG_CYAN   | TUI_BG_BLUE)

/* Read a single keystroke, translating scan codes. */
UINT16 tui_read_key(EFI_SYSTEM_TABLE *st);

/* Clear screen and set attribute. */
void tui_clear(EFI_SYSTEM_TABLE *st, UINTN attr);

/* Print a centred string on a given row. */
void tui_print_centre(EFI_SYSTEM_TABLE *st, UINTN row, const CHAR16 *text);

#endif /* SUPERBOOT_TUI_H */
