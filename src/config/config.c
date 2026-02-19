/*
 * config.c — Parser registry
 */

#include "config.h"

static const ConfigParser *parsers[] = {
    &sb_parser_grub,
    &sb_parser_systemd_boot,
    &sb_parser_limine,
    NULL
};

const ConfigParser **
sb_config_get_parsers(void)
{
    return parsers;
}
