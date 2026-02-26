#ifndef CMD_H
#define CMD_H

#include <stdint.h>
#include <stdbool.h>

#define CMD_MAX_LINE   512
#define CMD_MAX_ARGS   16

typedef struct {
    char line[CMD_MAX_LINE];
    int  len;
} cmd_buf_t;

// Token-parsed command
typedef struct {
    char *argv[CMD_MAX_ARGS];
    int   argc;
} cmd_parsed_t;

// Command handler function type
// argc/argv style. Returns true if command was handled.
typedef bool (*cmd_handler_t)(int argc, char **argv);

typedef struct {
    const char    *name;
    cmd_handler_t  handler;
} cmd_entry_t;

// Initialize command buffer
void cmd_init(cmd_buf_t *buf);

// Feed a character. Returns true when a complete line is ready.
bool cmd_feed(cmd_buf_t *buf, char c);

// Parse the line buffer into argc/argv tokens (modifies line in-place)
void cmd_parse(cmd_buf_t *buf, cmd_parsed_t *parsed);

// Dispatch parsed command against a table of handlers (NULL-terminated)
// Returns true if a handler matched and executed.
bool cmd_dispatch(cmd_parsed_t *parsed, const cmd_entry_t *table);

// Helper: send OK response with optional message
void cmd_ok(const char *msg);

// Helper: send ERROR response
void cmd_error(const char *msg);

#endif
