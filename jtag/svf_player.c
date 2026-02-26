#include "svf_player.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void svf_init(svf_player_t *s, jtag_t *jtag) {
    memset(s, 0, sizeof(*s));
    s->jtag = jtag;
    s->endir = TAP_RUN_TEST_IDLE;
    s->enddr = TAP_RUN_TEST_IDLE;
    s->run_state = TAP_RUN_TEST_IDLE;
}

static tap_state_t svf_parse_state(const char *name) {
    if (strcasecmp(name, "RESET") == 0)        return TAP_TEST_LOGIC_RESET;
    if (strcasecmp(name, "IDLE") == 0)          return TAP_RUN_TEST_IDLE;
    if (strcasecmp(name, "DRSELECT") == 0)      return TAP_SELECT_DR_SCAN;
    if (strcasecmp(name, "DRCAPTURE") == 0)     return TAP_CAPTURE_DR;
    if (strcasecmp(name, "DRSHIFT") == 0)       return TAP_SHIFT_DR;
    if (strcasecmp(name, "DREXIT1") == 0)       return TAP_EXIT1_DR;
    if (strcasecmp(name, "DRPAUSE") == 0)       return TAP_PAUSE_DR;
    if (strcasecmp(name, "DREXIT2") == 0)       return TAP_EXIT2_DR;
    if (strcasecmp(name, "DRUPDATE") == 0)      return TAP_UPDATE_DR;
    if (strcasecmp(name, "IRSELECT") == 0)      return TAP_SELECT_IR_SCAN;
    if (strcasecmp(name, "IRCAPTURE") == 0)     return TAP_CAPTURE_IR;
    if (strcasecmp(name, "IRSHIFT") == 0)       return TAP_SHIFT_IR;
    if (strcasecmp(name, "IREXIT1") == 0)       return TAP_EXIT1_IR;
    if (strcasecmp(name, "IRPAUSE") == 0)       return TAP_PAUSE_IR;
    if (strcasecmp(name, "IREXIT2") == 0)       return TAP_EXIT2_IR;
    if (strcasecmp(name, "IRUPDATE") == 0)      return TAP_UPDATE_IR;
    return (tap_state_t)-1;
}

static int svf_parse_hex(const char *hex, uint8_t *out, int max_bytes) {
    int hex_len = strlen(hex);
    memset(out, 0, max_bytes);

    int byte_idx = 0;
    for (int i = hex_len - 1; i >= 0 && byte_idx < max_bytes; i--) {
        char c = hex[i];
        uint8_t nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else continue;

        int pos = (hex_len - 1 - i);
        byte_idx = pos / 2;
        if (byte_idx >= max_bytes) break;
        if (pos % 2 == 0)
            out[byte_idx] = nibble;
        else
            out[byte_idx] |= (nibble << 4);
    }
    return (hex_len + 1) / 2;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *read_token(const char *p, char *tok, int tok_size) {
    p = skip_ws(p);
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')' && *p != ';') {
        if (i < tok_size - 1) tok[i++] = *p;
        p++;
    }
    tok[i] = '\0';
    return p;
}

static bool svf_parse_scan(svf_player_t *s, const char *p, bool is_ir) {
    char tok[64];
    p = read_token(p, tok, sizeof(tok));
    uint32_t length = strtoul(tok, NULL, 10);

    if (length == 0 || length > SVF_MAX_BITS) {
        snprintf(s->error_msg, sizeof(s->error_msg), "Invalid scan length: %lu", (unsigned long)length);
        s->error = true;
        return false;
    }

    uint32_t num_bytes = (length + 7) / 8;
    uint8_t tdi[SVF_MAX_BITS / 8] = {0};
    uint8_t tdo_expected[SVF_MAX_BITS / 8] = {0};
    uint8_t tdo_mask[SVF_MAX_BITS / 8];
    bool has_tdo = false;

    memset(tdo_mask, 0xFF, sizeof(tdo_mask));

    while (*p) {
        p = skip_ws(p);
        if (*p == ';' || *p == '\0') break;

        p = read_token(p, tok, sizeof(tok));
        if (tok[0] == '\0') break;

        p = skip_ws(p);
        if (*p != '(') continue;
        p++;

        char hex[SVF_MAX_BITS / 4 + 1];
        int hi = 0;
        while (*p && *p != ')') {
            if (!isspace((unsigned char)*p) && hi < (int)sizeof(hex) - 1) {
                hex[hi++] = *p;
            }
            p++;
        }
        hex[hi] = '\0';
        if (*p == ')') p++;

        if (strcasecmp(tok, "TDI") == 0) {
            svf_parse_hex(hex, tdi, num_bytes);
        } else if (strcasecmp(tok, "TDO") == 0) {
            svf_parse_hex(hex, tdo_expected, num_bytes);
            has_tdo = true;
        } else if (strcasecmp(tok, "MASK") == 0) {
            svf_parse_hex(hex, tdo_mask, num_bytes);
        }
    }

    uint32_t hdr_bits = is_ir ? s->hir_bits : s->hdr_bits;
    uint8_t *hdr_tdi  = is_ir ? s->hir_tdi  : s->hdr_tdi;
    uint32_t trl_bits = is_ir ? s->tir_bits : s->tdr_bits;
    uint8_t *trl_tdi  = is_ir ? s->tir_tdi  : s->tdr_tdi;

    uint32_t total_bits = hdr_bits + length + trl_bits;
    uint32_t total_bytes = (total_bits + 7) / 8;

    if (total_bits > SVF_MAX_BITS) {
        snprintf(s->error_msg, sizeof(s->error_msg), "Total scan too long: %lu bits",
                 (unsigned long)total_bits);
        s->error = true;
        return false;
    }

    uint8_t full_tdi[SVF_MAX_BITS / 8] = {0};
    uint32_t bit_pos = 0;

    for (uint32_t i = 0; i < hdr_bits; i++) {
        int bit = (hdr_tdi[i / 8] >> (i % 8)) & 1;
        full_tdi[bit_pos / 8] |= (bit << (bit_pos % 8));
        bit_pos++;
    }
    for (uint32_t i = 0; i < length; i++) {
        int bit = (tdi[i / 8] >> (i % 8)) & 1;
        full_tdi[bit_pos / 8] |= (bit << (bit_pos % 8));
        bit_pos++;
    }
    for (uint32_t i = 0; i < trl_bits; i++) {
        int bit = (trl_tdi[i / 8] >> (i % 8)) & 1;
        full_tdi[bit_pos / 8] |= (bit << (bit_pos % 8));
        bit_pos++;
    }

    uint8_t tdo_actual[SVF_MAX_BITS / 8] = {0};
    tap_state_t end_state = is_ir ? s->endir : s->enddr;

    if (is_ir) {
        jtag_scan_ir(s->jtag, full_tdi, has_tdo ? tdo_actual : NULL,
                     total_bits, end_state);
    } else {
        jtag_scan_dr(s->jtag, full_tdi, has_tdo ? tdo_actual : NULL,
                     total_bits, end_state);
    }

    if (has_tdo) {
        for (uint32_t i = 0; i < length; i++) {
            uint32_t actual_bit_pos = hdr_bits + i;
            int actual = (tdo_actual[actual_bit_pos / 8] >> (actual_bit_pos % 8)) & 1;
            int expected = (tdo_expected[i / 8] >> (i % 8)) & 1;
            int mask = (tdo_mask[i / 8] >> (i % 8)) & 1;
            if (mask && (actual != expected)) {
                snprintf(s->error_msg, sizeof(s->error_msg),
                         "%s TDO mismatch at bit %lu", is_ir ? "SIR" : "SDR",
                         (unsigned long)i);
                s->error = true;
                return false;
            }
        }
    }

    return true;
}

static bool svf_parse_header_trailer(svf_player_t *s, const char *p,
                                     uint32_t *bits_out, uint8_t *tdi_out) {
    char tok[64];
    p = read_token(p, tok, sizeof(tok));
    uint32_t length = strtoul(tok, NULL, 10);

    *bits_out = length;
    memset(tdi_out, 0, SVF_MAX_BITS / 8);

    if (length == 0) return true;

    while (*p) {
        p = skip_ws(p);
        if (*p == ';' || *p == '\0') break;
        p = read_token(p, tok, sizeof(tok));
        if (strcasecmp(tok, "TDI") == 0) {
            p = skip_ws(p);
            if (*p == '(') {
                p++;
                char hex[SVF_MAX_BITS / 4 + 1];
                int hi = 0;
                while (*p && *p != ')') {
                    if (!isspace((unsigned char)*p) && hi < (int)sizeof(hex) - 1)
                        hex[hi++] = *p;
                    p++;
                }
                hex[hi] = '\0';
                svf_parse_hex(hex, tdi_out, (length + 7) / 8);
            }
        }
    }
    return true;
}

bool svf_exec_line(svf_player_t *s, const char *line) {
    s->line_num++;

    const char *p = skip_ws(line);
    if (*p == '\0' || *p == '!' || *p == '/') return true;

    char cmd[32];
    p = read_token(p, cmd, sizeof(cmd));

    if (strcasecmp(cmd, "SIR") == 0) {
        return svf_parse_scan(s, p, true);
    }
    if (strcasecmp(cmd, "SDR") == 0) {
        return svf_parse_scan(s, p, false);
    }
    if (strcasecmp(cmd, "STATE") == 0) {
        while (*p && *p != ';') {
            char tok[32];
            p = read_token(p, tok, sizeof(tok));
            if (tok[0] == '\0') break;
            tap_state_t st = svf_parse_state(tok);
            if ((int)st < 0) {
                snprintf(s->error_msg, sizeof(s->error_msg),
                         "Unknown state: %s", tok);
                s->error = true;
                return false;
            }
            jtag_goto_state(s->jtag, st);
        }
        return true;
    }
    if (strcasecmp(cmd, "RUNTEST") == 0) {
        char tok[32];
        uint32_t run_count = 0;
        tap_state_t run_st = s->run_state;

        p = read_token(p, tok, sizeof(tok));
        tap_state_t maybe_state = svf_parse_state(tok);
        if ((int)maybe_state >= 0) {
            run_st = maybe_state;
            p = read_token(p, tok, sizeof(tok));
        }
        run_count = strtoul(tok, NULL, 10);

        jtag_goto_state(s->jtag, run_st);
        jtag_run_clocks(s->jtag, run_count);
        return true;
    }
    if (strcasecmp(cmd, "ENDIR") == 0) {
        char tok[32];
        p = read_token(p, tok, sizeof(tok));
        tap_state_t st = svf_parse_state(tok);
        if ((int)st >= 0) s->endir = st;
        return true;
    }
    if (strcasecmp(cmd, "ENDDR") == 0) {
        char tok[32];
        p = read_token(p, tok, sizeof(tok));
        tap_state_t st = svf_parse_state(tok);
        if ((int)st >= 0) s->enddr = st;
        return true;
    }
    if (strcasecmp(cmd, "FREQUENCY") == 0) {
        char tok[32];
        p = read_token(p, tok, sizeof(tok));
        float freq = strtof(tok, NULL);
        if (freq > 0) {
            jtag_set_freq(s->jtag, freq);
        }
        return true;
    }
    if (strcasecmp(cmd, "TRST") == 0) {
        char tok[32];
        p = read_token(p, tok, sizeof(tok));
        if (strcasecmp(tok, "ON") == 0 || strcasecmp(tok, "ABSENT") == 0) {
            jtag_reset(s->jtag);
        }
        return true;
    }
    if (strcasecmp(cmd, "HIR") == 0) {
        return svf_parse_header_trailer(s, p, &s->hir_bits, s->hir_tdi);
    }
    if (strcasecmp(cmd, "TIR") == 0) {
        return svf_parse_header_trailer(s, p, &s->tir_bits, s->tir_tdi);
    }
    if (strcasecmp(cmd, "HDR") == 0) {
        return svf_parse_header_trailer(s, p, &s->hdr_bits, s->hdr_tdi);
    }
    if (strcasecmp(cmd, "TDR") == 0) {
        return svf_parse_header_trailer(s, p, &s->tdr_bits, s->tdr_tdi);
    }

    // Unknown commands silently ignored
    return true;
}
