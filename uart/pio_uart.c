#include "pio_uart.h"
#include "hw_uart.h"
#include "cmd.h"
#include "pin_manager.h"
#include "pio_alloc.h"
#include "uart_tx.pio.h"
#include "uart_rx.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// Global instances for PIO UARTs (pio0..pio3)
static pio_uart_inst_t pio_uarts[PIO_UART_MAX];
// Global instances for HW UARTs (hw0..hw1)
extern hw_uart_inst_t hw_uarts[2];

// Track which PIO blocks have TX/RX programs loaded, and their offsets
static struct {
    bool loaded;
    uint offset_tx;
    uint offset_rx;
    int  ref_count;
} pio_uart_programs[2]; // indexed by pio_idx

bool pio_uart_init(pio_uart_inst_t *inst, uint pin_tx, uint pin_rx, uint32_t baud) {
    if (inst->active) return true;

    // Try PIO0 first, then PIO1
    int pio_idx = -1;
    uint sms[2];
    uint offset_tx, offset_rx;
    bool loaded_programs = false;

    for (int try = 0; try < 2; try++) {
        // Check if we can get 2 SMs
        if (!pio_alloc_claim_sm(try, 2, sms)) continue;

        // Check if programs already loaded on this PIO
        if (pio_uart_programs[try].loaded) {
            offset_tx = pio_uart_programs[try].offset_tx;
            offset_rx = pio_uart_programs[try].offset_rx;
            pio_idx = try;
            loaded_programs = false;
            break;
        }

        // Try to load programs
        int off_tx = pio_alloc_add_program(try, &uart_tx_program);
        if (off_tx < 0) {
            pio_alloc_release_sm(try, 2, sms);
            continue;
        }
        int off_rx = pio_alloc_add_program(try, &uart_rx_program);
        if (off_rx < 0) {
            pio_alloc_remove_program(try, &uart_tx_program, off_tx);
            pio_alloc_release_sm(try, 2, sms);
            continue;
        }

        offset_tx = off_tx;
        offset_rx = off_rx;
        pio_idx = try;
        loaded_programs = true;
        break;
    }

    if (pio_idx < 0) return false;

    PIO pio = pio_alloc_get_pio(pio_idx);

    inst->pio = pio;
    inst->pio_idx = pio_idx;
    inst->sm_tx = sms[0];
    inst->sm_rx = sms[1];
    inst->pin_tx = pin_tx;
    inst->pin_rx = pin_rx;
    inst->baud = baud;
    inst->offset_tx = offset_tx;
    inst->offset_rx = offset_rx;
    inst->owns_program = loaded_programs;
    inst->rx_head = 0;
    inst->rx_tail = 0;
    inst->active = true;

    if (loaded_programs) {
        pio_uart_programs[pio_idx].loaded = true;
        pio_uart_programs[pio_idx].offset_tx = offset_tx;
        pio_uart_programs[pio_idx].offset_rx = offset_rx;
        pio_uart_programs[pio_idx].ref_count = 1;
    } else {
        pio_uart_programs[pio_idx].ref_count++;
    }

    // Initialize TX and RX state machines
    uart_tx_program_init(pio, inst->sm_tx, offset_tx, pin_tx, baud);
    uart_rx_program_init(pio, inst->sm_rx, offset_rx, pin_rx, baud);

    return true;
}

void pio_uart_set_baud(pio_uart_inst_t *inst, uint32_t baud) {
    if (!inst->active) return;
    inst->baud = baud;

    float div = (float)clock_get_hz(clk_sys) / (8 * baud);
    pio_sm_set_clkdiv(inst->pio, inst->sm_tx, div);
    pio_sm_clkdiv_restart(inst->pio, inst->sm_tx);
    pio_sm_set_clkdiv(inst->pio, inst->sm_rx, div);
    pio_sm_clkdiv_restart(inst->pio, inst->sm_rx);
}

int pio_uart_write(pio_uart_inst_t *inst, const uint8_t *data, int len) {
    if (!inst->active) return 0;

    int written = 0;
    for (int i = 0; i < len; i++) {
        if (pio_sm_is_tx_fifo_full(inst->pio, inst->sm_tx)) break;
        pio_sm_put(inst->pio, inst->sm_tx, (uint32_t)data[i]);
        written++;
    }
    return written;
}

void pio_uart_poll_rx(pio_uart_inst_t *inst) {
    if (!inst->active) return;

    while (!pio_sm_is_rx_fifo_empty(inst->pio, inst->sm_rx)) {
        io_rw_8 *rxfifo_shift = (io_rw_8 *)&inst->pio->rxf[inst->sm_rx] + 3;
        uint8_t byte = *rxfifo_shift;

        uint16_t next_head = (inst->rx_head + 1) % PIO_UART_RX_BUFSZ;
        if (next_head != inst->rx_tail) {
            inst->rx_buf[inst->rx_head] = byte;
            inst->rx_head = next_head;
        }
    }
}

int pio_uart_read(pio_uart_inst_t *inst, uint8_t *buf, int max_len) {
    int count = 0;
    while (count < max_len && inst->rx_tail != inst->rx_head) {
        buf[count++] = inst->rx_buf[inst->rx_tail];
        inst->rx_tail = (inst->rx_tail + 1) % PIO_UART_RX_BUFSZ;
    }
    return count;
}

int pio_uart_rx_available(pio_uart_inst_t *inst) {
    return (inst->rx_head - inst->rx_tail + PIO_UART_RX_BUFSZ) % PIO_UART_RX_BUFSZ;
}

void pio_uart_poll_all(void) {
    for (int i = 0; i < PIO_UART_MAX; i++) {
        pio_uart_poll_rx(&pio_uarts[i]);
    }
}

void pio_uart_deinit(pio_uart_inst_t *inst) {
    if (!inst->active) return;

    pio_sm_set_enabled(inst->pio, inst->sm_tx, false);
    pio_sm_set_enabled(inst->pio, inst->sm_rx, false);

    uint sms[2] = { inst->sm_tx, inst->sm_rx };
    pio_alloc_release_sm(inst->pio_idx, 2, sms);

    // Release programs if we're the last user
    pio_uart_programs[inst->pio_idx].ref_count--;
    if (pio_uart_programs[inst->pio_idx].ref_count <= 0) {
        pio_alloc_remove_program(inst->pio_idx, &uart_tx_program, inst->offset_tx);
        pio_alloc_remove_program(inst->pio_idx, &uart_rx_program, inst->offset_rx);
        pio_uart_programs[inst->pio_idx].loaded = false;
        pio_uart_programs[inst->pio_idx].ref_count = 0;
    }

    inst->active = false;
}

// Parse hex string into byte buffer. Returns number of bytes.
static int parse_hex_bytes(const char *hex, uint8_t *out, int max_len) {
    int count = 0;
    while (*hex && count < max_len) {
        // Skip spaces
        while (*hex && isspace((unsigned char)*hex)) hex++;
        if (!*hex) break;

        char hi = *hex++;
        if (!*hex) break;
        char lo = *hex++;

        uint8_t val = 0;
        if (hi >= '0' && hi <= '9') val = (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;
        else break;

        if (lo >= '0' && lo <= '9') val |= lo - '0';
        else if (lo >= 'a' && lo <= 'f') val |= lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') val |= lo - 'A' + 10;
        else break;

        out[count++] = val;
    }
    return count;
}

// Parse UART id: "pio0".."pio3" → 0..3, "hw0".."hw1" → 100..101
static int parse_uart_id(const char *s) {
    if (strncasecmp(s, "pio", 3) == 0) {
        int n = atoi(s + 3);
        if (n >= 0 && n < PIO_UART_MAX) return n;
    } else if (strncasecmp(s, "hw", 2) == 0) {
        int n = atoi(s + 2);
        if (n >= 0 && n <= 1) return 100 + n;
    }
    return -1;
}

bool cmd_uart(int argc, char **argv) {
    if (argc < 3) {
        cmd_error("usage: UART <id> <command> [args]");
        return true;
    }

    int id = parse_uart_id(argv[1]);
    if (id < 0) { cmd_error("invalid UART id (pio0..pio3, hw0..hw1)"); return true; }

    bool is_hw = (id >= 100);
    int hw_idx = id - 100;
    int pio_idx = id;

    const char *subcmd = argv[2];

    // INIT
    if (strcasecmp(subcmd, "INIT") == 0) {
        if (argc < 4) { cmd_error("usage: UART <id> INIT <baud>"); return true; }
        uint32_t baud = strtoul(argv[3], NULL, 10);
        if (baud == 0) { cmd_error("invalid baud"); return true; }

        if (is_hw) {
            if (!hw_uart_init_instance(&hw_uarts[hw_idx], baud)) {
                cmd_error("HW UART init failed");
                return true;
            }
        } else {
            // Find TX and RX pins assigned to this UART
            int tx_pin = -1, rx_pin = -1;
            for (uint g = 0; g < PIN_MAX_GPIO; g++) {
                if (pin_get_func(g) == PIN_FUNC_UART_TX && pin_get_owner(g) == pio_idx)
                    tx_pin = g;
                if (pin_get_func(g) == PIN_FUNC_UART_RX && pin_get_owner(g) == pio_idx)
                    rx_pin = g;
            }
            if (tx_pin < 0 || rx_pin < 0) {
                cmd_error("assign TX and RX pins first (PIN <gpio> FUNC UART_TX/RX)");
                return true;
            }
            if (!pio_uart_init(&pio_uarts[pio_idx], tx_pin, rx_pin, baud)) {
                cmd_error("PIO UART init failed (no PIO resources)");
                return true;
            }
        }
        cmd_ok(NULL);
        return true;
    }

    // DEINIT
    if (strcasecmp(subcmd, "DEINIT") == 0) {
        if (is_hw) {
            hw_uart_deinit_instance(&hw_uarts[hw_idx]);
        } else {
            pio_uart_deinit(&pio_uarts[pio_idx]);
        }
        cmd_ok(NULL);
        return true;
    }

    // BAUD
    if (strcasecmp(subcmd, "BAUD") == 0) {
        if (argc < 4) { cmd_error("usage: UART <id> BAUD <baud>"); return true; }
        uint32_t baud = strtoul(argv[3], NULL, 10);
        if (baud == 0) { cmd_error("invalid baud"); return true; }

        if (is_hw) {
            hw_uart_set_baud_instance(&hw_uarts[hw_idx], baud);
        } else {
            if (!pio_uarts[pio_idx].active) { cmd_error("not initialized"); return true; }
            pio_uart_set_baud(&pio_uarts[pio_idx], baud);
        }
        cmd_ok(NULL);
        return true;
    }

    // SEND <hex_data>
    if (strcasecmp(subcmd, "SEND") == 0) {
        if (argc < 4) { cmd_error("usage: UART <id> SEND <hex_data>"); return true; }

        uint8_t data[256];
        int len = parse_hex_bytes(argv[3], data, sizeof(data));
        if (len == 0) { cmd_error("invalid hex data"); return true; }

        int written;
        if (is_hw) {
            if (!hw_uarts[hw_idx].active) { cmd_error("not initialized"); return true; }
            written = hw_uart_write_instance(&hw_uarts[hw_idx], data, len);
        } else {
            if (!pio_uarts[pio_idx].active) { cmd_error("not initialized"); return true; }
            written = pio_uart_write(&pio_uarts[pio_idx], data, len);
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", written);
        cmd_ok(buf);
        return true;
    }

    // RECV [max] [timeout_ms]
    if (strcasecmp(subcmd, "RECV") == 0) {
        int max_bytes = 256;
        int timeout_ms = 100;
        if (argc >= 4) max_bytes = atoi(argv[3]);
        if (argc >= 5) timeout_ms = atoi(argv[4]);
        if (max_bytes > 512) max_bytes = 512;

        uint8_t data[512];
        int total = 0;
        absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

        while (total < max_bytes && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
            if (is_hw) {
                if (!hw_uarts[hw_idx].active) { cmd_error("not initialized"); return true; }
                hw_uart_poll_rx_instance(&hw_uarts[hw_idx]);
                int n = hw_uart_read_instance(&hw_uarts[hw_idx], data + total, max_bytes - total);
                total += n;
            } else {
                if (!pio_uarts[pio_idx].active) { cmd_error("not initialized"); return true; }
                pio_uart_poll_rx(&pio_uarts[pio_idx]);
                int n = pio_uart_read(&pio_uarts[pio_idx], data + total, max_bytes - total);
                total += n;
            }
            if (total > 0) break; // Return as soon as we have data
        }

        // Format as hex
        char hex[1025]; // 512 bytes * 2 + 1
        for (int i = 0; i < total; i++) {
            snprintf(hex + i * 2, 3, "%02x", data[i]);
        }
        hex[total * 2] = '\0';
        cmd_ok(hex);
        return true;
    }

    cmd_error("unknown UART subcommand");
    return true;
}
