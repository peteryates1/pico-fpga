#ifndef SVF_PLAYER_H
#define SVF_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "jtag.h"

#define SVF_MAX_LINE 4096
#define SVF_MAX_BITS 4096

typedef struct {
    jtag_t *jtag;

    // Header/trailer bits for IR and DR scans
    uint32_t hir_bits;
    uint8_t  hir_tdi[SVF_MAX_BITS / 8];
    uint32_t tir_bits;
    uint8_t  tir_tdi[SVF_MAX_BITS / 8];
    uint32_t hdr_bits;
    uint8_t  hdr_tdi[SVF_MAX_BITS / 8];
    uint32_t tdr_bits;
    uint8_t  tdr_tdi[SVF_MAX_BITS / 8];

    // End states
    tap_state_t endir;
    tap_state_t enddr;
    tap_state_t run_state;

    // Error tracking
    bool     error;
    char     error_msg[128];
    uint32_t line_num;
} svf_player_t;

void svf_init(svf_player_t *s, jtag_t *jtag);
bool svf_exec_line(svf_player_t *s, const char *line);

#endif
