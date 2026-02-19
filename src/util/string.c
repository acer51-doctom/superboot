/*
 * string.c — ASCII (CHAR8) string utilities
 *
 * UEFI's standard library provides wide-string (CHAR16) helpers, but
 * bootloader configs and kernel command lines are ASCII.  These
 * functions fill the gap.
 */

#include "util.h"

INTN
sb_strcmp8(const CHAR8 *a, const CHAR8 *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (INTN)*a - (INTN)*b;
}

INTN
sb_strncmp8(const CHAR8 *a, const CHAR8 *b, UINTN n)
{
    for (UINTN i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0')
            return (INTN)a[i] - (INTN)b[i];
    }
    return 0;
}

UINTN
sb_strlen8(const CHAR8 *a)
{
    UINTN len = 0;
    while (a[len]) len++;
    return len;
}

CHAR8 *
sb_strstr8(const CHAR8 *haystack, const CHAR8 *needle)
{
    if (!*needle) return (CHAR8 *)haystack;
    UINTN nlen = sb_strlen8(needle);

    for (; *haystack; haystack++) {
        if (sb_strncmp8(haystack, needle, nlen) == 0)
            return (CHAR8 *)haystack;
    }
    return NULL;
}

void
sb_strcpy8(CHAR8 *dst, const CHAR8 *src, UINTN max)
{
    UINTN i;
    for (i = 0; i + 1 < max && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

void
sb_str8to16(CHAR16 *dst, const CHAR8 *src, UINTN max)
{
    UINTN i;
    for (i = 0; i + 1 < max && src[i]; i++)
        dst[i] = (CHAR16)(UINT8)src[i];
    dst[i] = L'\0';
}

void
sb_str16to8(CHAR8 *dst, const CHAR16 *src, UINTN max)
{
    UINTN i;
    for (i = 0; i + 1 < max && src[i]; i++)
        dst[i] = (src[i] < 0x80) ? (CHAR8)src[i] : '?';
    dst[i] = '\0';
}

CHAR8 *
sb_skip_whitespace(CHAR8 *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

CHAR8 *
sb_next_line(CHAR8 *p)
{
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    return p;
}

BOOLEAN
sb_starts_with8(const CHAR8 *s, const CHAR8 *prefix)
{
    while (*prefix) {
        if (*s != *prefix) return FALSE;
        s++;
        prefix++;
    }
    return TRUE;
}
