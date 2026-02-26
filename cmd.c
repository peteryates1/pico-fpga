#include "cmd.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

void cmd_init(cmd_buf_t *buf) {
    buf->len = 0;
}

bool cmd_feed(cmd_buf_t *buf, char c) {
    if (c == '\r') return false;
    if (c == '\n') {
        buf->line[buf->len] = '\0';
        return true;
    }
    if (buf->len < CMD_MAX_LINE - 1) {
        buf->line[buf->len++] = c;
    }
    return false;
}

void cmd_parse(cmd_buf_t *buf, cmd_parsed_t *parsed) {
    parsed->argc = 0;
    char *p = buf->line;

    while (*p && parsed->argc < CMD_MAX_ARGS) {
        // Skip whitespace
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        parsed->argv[parsed->argc++] = p;

        // Find end of token
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }

    // Reset line buffer for next command
    buf->len = 0;
}

bool cmd_dispatch(cmd_parsed_t *parsed, const cmd_entry_t *table) {
    if (parsed->argc == 0) return false;

    for (const cmd_entry_t *e = table; e->name != NULL; e++) {
        if (strcasecmp(parsed->argv[0], e->name) == 0) {
            return e->handler(parsed->argc, parsed->argv);
        }
    }

    cmd_error("unknown command");
    return false;
}

void cmd_ok(const char *msg) {
    if (msg && msg[0]) {
        printf("OK %s\n", msg);
    } else {
        printf("OK\n");
    }
}

void cmd_error(const char *msg) {
    printf("ERROR: %s\n", msg);
}
