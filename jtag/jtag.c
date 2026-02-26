#include "jtag.h"
#include "jtag.pio.h"
#include "cmd.h"
#include "pin_manager.h"
#include "pio_alloc.h"
#include "la.h"
#include "fpga_xilinx.h"
#include "fpga_altera.h"
#include "fpga_lattice.h"
#include "svf_player.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

jtag_t jtag_instance;

bool jtag_init(jtag_t *j, uint pin_tck, uint pin_tms, uint pin_tdi, uint pin_tdo,
               float freq_hz) {
    if (j->initialized) return true;

    // TMS and TDI must be consecutive (TDI = TMS+1) for OUT pins
    if (pin_tdi != pin_tms + 1) return false;

    // Try PIO0, then PIO1
    int pio_idx = -1;
    uint sm;
    int off_io, off_bulk;

    for (int try = 0; try < 2; try++) {
        off_io = pio_alloc_add_program(try, &jtag_io_program);
        if (off_io < 0) continue;

        off_bulk = pio_alloc_add_program(try, &jtag_bulk_write_program);
        if (off_bulk < 0) {
            pio_alloc_remove_program(try, &jtag_io_program, off_io);
            continue;
        }

        if (!pio_alloc_claim_sm(try, 1, &sm)) {
            pio_alloc_remove_program(try, &jtag_bulk_write_program, off_bulk);
            pio_alloc_remove_program(try, &jtag_io_program, off_io);
            continue;
        }

        pio_idx = try;
        break;
    }

    if (pio_idx < 0) return false;

    int dma_chan = pio_alloc_claim_dma();
    if (dma_chan < 0) {
        pio_alloc_release_sm(pio_idx, 1, &sm);
        pio_alloc_remove_program(pio_idx, &jtag_bulk_write_program, off_bulk);
        pio_alloc_remove_program(pio_idx, &jtag_io_program, off_io);
        return false;
    }

    j->pio = pio_alloc_get_pio(pio_idx);
    j->pio_idx = pio_idx;
    j->sm = sm;
    j->offset_io = off_io;
    j->offset_bulk = off_bulk;
    j->freq_hz = freq_hz;
    j->dma_chan = dma_chan;
    j->state = TAP_TEST_LOGIC_RESET;
    j->pin_tck = pin_tck;
    j->pin_tms = pin_tms;
    j->pin_tdi = pin_tdi;
    j->pin_tdo = pin_tdo;
    j->initialized = true;

    // Initialize with jtag_io program
    jtag_io_program_init(j->pio, j->sm, j->offset_io,
                         pin_tck, pin_tms, pin_tdi, pin_tdo, freq_hz);

    // Reset TAP
    jtag_reset(j);

    return true;
}

void jtag_set_freq(jtag_t *j, float freq_hz) {
    j->freq_hz = freq_hz;
    float div = (float)clock_get_hz(clk_sys) / (4.0f * freq_hz);
    pio_sm_set_clkdiv(j->pio, j->sm, div);
    pio_sm_clkdiv_restart(j->pio, j->sm);
}

// Low-level: clock out N bits with packed TMS+TDI data, capture TDO
static void jtag_shift_bits(jtag_t *j, const uint32_t *tms_tdi_words,
                            uint32_t *tdo_words, uint32_t num_bits) {
    if (num_bits == 0) return;

    PIO pio = j->pio;
    uint sm = j->sm;

    // Reinit with jtag_io program
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    jtag_io_program_init(pio, sm, j->offset_io,
                         j->pin_tck, j->pin_tms,
                         j->pin_tdi, j->pin_tdo,
                         j->freq_hz);

    // Preload X with (num_bits - 1)
    pio_sm_put_blocking(pio, sm, num_bits - 1);
    pio_sm_exec(pio, sm, pio_encode_out(pio_x, 32));

    pio_sm_set_enabled(pio, sm, true);

    uint32_t tx_words = (num_bits + 15) / 16;
    uint32_t rx_words = (num_bits + 31) / 32;

    uint32_t tx_sent = 0, rx_recv = 0;
    while (tx_sent < tx_words || rx_recv < rx_words) {
        if (tx_sent < tx_words && !pio_sm_is_tx_fifo_full(pio, sm)) {
            pio_sm_put(pio, sm, tms_tdi_words[tx_sent++]);
        }
        if (rx_recv < rx_words && !pio_sm_is_rx_fifo_empty(pio, sm)) {
            uint32_t val = pio_sm_get(pio, sm);
            if (tdo_words) {
                tdo_words[rx_recv] = val;
            }
            rx_recv++;
        }
    }

    pio_sm_set_enabled(pio, sm, false);
}

void jtag_reset(jtag_t *j) {
    uint32_t tms_tdi = 0;
    for (int i = 0; i < 5; i++) {
        tms_tdi |= (0x1u << (i * 2)); // tms=1, tdi=0
    }

    jtag_shift_bits(j, &tms_tdi, NULL, 5);
    j->state = TAP_TEST_LOGIC_RESET;
}

void jtag_goto_state(jtag_t *j, tap_state_t target) {
    if (j->state == target) return;

    tap_path_t path = tap_calc_path(j->state, target);
    if (path.num_clocks == 0) return;

    uint32_t tms_tdi = 0;
    for (int i = 0; i < path.num_clocks; i++) {
        int tms = (path.tms_bits >> i) & 1;
        tms_tdi |= ((uint32_t)tms << (i * 2));
    }

    jtag_shift_bits(j, &tms_tdi, NULL, path.num_clocks);
    j->state = target;
}

static void jtag_scan(jtag_t *j, const uint8_t *tdi_data, uint8_t *tdo_data,
                      uint32_t num_bits, tap_state_t shift_state,
                      tap_state_t end_state) {
    jtag_goto_state(j, shift_state);

    if (num_bits == 0) {
        jtag_goto_state(j, end_state);
        return;
    }

    uint32_t total_clocks = num_bits;
    uint32_t tx_words = (total_clocks + 15) / 16;
    uint32_t rx_words = (total_clocks + 31) / 32;
    uint32_t tms_tdi[tx_words];
    uint32_t tdo_raw[rx_words];
    memset(tms_tdi, 0, sizeof(tms_tdi));
    memset(tdo_raw, 0, sizeof(tdo_raw));

    for (uint32_t i = 0; i < num_bits; i++) {
        int tdi_bit = (tdi_data[i / 8] >> (i % 8)) & 1;
        int tms_bit = (i == num_bits - 1) ? 1 : 0;
        uint32_t packed = ((uint32_t)tdi_bit << 1) | tms_bit;
        uint32_t word_idx = i / 16;
        uint32_t bit_pos = (i % 16) * 2;
        tms_tdi[word_idx] |= (packed << bit_pos);
    }

    jtag_shift_bits(j, tms_tdi, tdo_data ? tdo_raw : NULL, total_clocks);

    if (tdo_data) {
        memset(tdo_data, 0, (num_bits + 7) / 8);
        for (uint32_t i = 0; i < num_bits; i++) {
            int tdo_bit = (tdo_raw[i / 32] >> (i % 32)) & 1;
            tdo_data[i / 8] |= (tdo_bit << (i % 8));
        }
    }

    if (shift_state == TAP_SHIFT_DR) {
        j->state = TAP_EXIT1_DR;
    } else {
        j->state = TAP_EXIT1_IR;
    }

    jtag_goto_state(j, end_state);
}

void jtag_scan_ir(jtag_t *j, const uint8_t *tdi_data, uint8_t *tdo_data,
                  uint32_t num_bits, tap_state_t end_state) {
    jtag_scan(j, tdi_data, tdo_data, num_bits, TAP_SHIFT_IR, end_state);
}

void jtag_scan_dr(jtag_t *j, const uint8_t *tdi_data, uint8_t *tdo_data,
                  uint32_t num_bits, tap_state_t end_state) {
    jtag_scan(j, tdi_data, tdo_data, num_bits, TAP_SHIFT_DR, end_state);
}

void jtag_run_clocks(jtag_t *j, uint32_t num_clocks) {
    jtag_goto_state(j, TAP_RUN_TEST_IDLE);

    if (num_clocks == 0) return;

    while (num_clocks > 0) {
        uint32_t chunk = (num_clocks > 256) ? 256 : num_clocks;
        uint32_t words = (chunk + 15) / 16;
        uint32_t zeros[words];
        memset(zeros, 0, sizeof(zeros));
        jtag_shift_bits(j, zeros, NULL, chunk);
        num_clocks -= chunk;
    }
}

void jtag_bulk_write(jtag_t *j, const uint32_t *data, uint32_t data_words) {
    if (data_words == 0) return;

    PIO pio = j->pio;
    uint sm = j->sm;

    // Switch SM to bulk write program
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    jtag_bulk_write_program_init(pio, sm, j->offset_bulk,
                                 j->pin_tck, j->pin_tdi,
                                 j->freq_hz);

    // Set TMS=0 via GPIO
    gpio_init(j->pin_tms);
    gpio_set_dir(j->pin_tms, GPIO_OUT);
    gpio_put(j->pin_tms, 0);

    // Configure DMA
    dma_channel_config dma_cfg = dma_channel_get_default_config(j->dma_chan);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(pio, sm, true));

    dma_channel_configure(j->dma_chan, &dma_cfg,
                          &pio->txf[sm],
                          data,
                          data_words,
                          false);

    pio_sm_set_enabled(pio, sm, true);
    dma_channel_start(j->dma_chan);

    dma_channel_wait_for_finish_blocking(j->dma_chan);

    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }
    busy_wait_us(1);

    pio_sm_set_enabled(pio, sm, false);

    // Reclaim TMS pin for PIO
    pio_gpio_init(pio, j->pin_tms);
}

int jtag_detect(jtag_t *j, uint32_t *idcodes, int max_devices) {
    jtag_reset(j);
    jtag_goto_state(j, TAP_SHIFT_DR);

    int count = 0;
    uint32_t total_bits = max_devices * 32;
    uint32_t tx_words = (total_bits + 15) / 16;
    uint32_t rx_words = (total_bits + 31) / 32;

    uint32_t tms_tdi_packed[tx_words];
    uint32_t tdo_raw[rx_words];
    memset(tms_tdi_packed, 0, sizeof(tms_tdi_packed));
    memset(tdo_raw, 0, sizeof(tdo_raw));

    for (uint32_t i = 0; i < total_bits; i++) {
        int tms = (i == total_bits - 1) ? 1 : 0;
        uint32_t packed = (1u << 1) | tms;
        uint32_t word_idx = i / 16;
        uint32_t bit_pos = (i % 16) * 2;
        tms_tdi_packed[word_idx] |= (packed << bit_pos);
    }

    jtag_shift_bits(j, tms_tdi_packed, tdo_raw, total_bits);

    uint32_t bit_pos = 0;
    while (bit_pos < total_bits && count < max_devices) {
        int bit0 = (tdo_raw[bit_pos / 32] >> (bit_pos % 32)) & 1;
        if (bit0 == 0) {
            bit_pos++;
            continue;
        }
        if (bit0 == 1 && (bit_pos + 32) <= total_bits) {
            uint32_t idcode = 0;
            for (int b = 0; b < 32; b++) {
                uint32_t bi = bit_pos + b;
                int bit = (tdo_raw[bi / 32] >> (bi % 32)) & 1;
                idcode |= ((uint32_t)bit << b);
            }
            if (idcode == 0xFFFFFFFF) break;
            idcodes[count++] = idcode;
            bit_pos += 32;
        } else {
            break;
        }
    }

    j->state = TAP_EXIT1_DR;
    jtag_goto_state(j, TAP_RUN_TEST_IDLE);

    return count;
}

void jtag_deinit(jtag_t *j) {
    if (!j->initialized) return;

    PIO pio = j->pio;

    pio_sm_set_enabled(pio, j->sm, false);
    pio_alloc_release_sm(j->pio_idx, 1, &j->sm);
    pio_alloc_remove_program(j->pio_idx, &jtag_io_program, j->offset_io);
    pio_alloc_remove_program(j->pio_idx, &jtag_bulk_write_program, j->offset_bulk);
    pio_alloc_release_dma(j->dma_chan);

    // Release pins back to GPIO
    gpio_init(j->pin_tck);
    gpio_init(j->pin_tms);
    gpio_init(j->pin_tdi);
    gpio_init(j->pin_tdo);

    j->initialized = false;
}

// Parse hex string into byte buffer (for JTAG commands)
static int parse_hex_to_bytes(const char *hex, uint8_t *out, int max_bytes) {
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

// Format bytes as hex string
static void bytes_to_hex(const uint8_t *data, int num_bytes, char *out) {
    for (int i = num_bytes - 1; i >= 0; i--) {
        sprintf(out, "%02X", data[i]);
        out += 2;
    }
    *out = '\0';
}

// Bitstream programming state
static struct {
    bool     active;
    char     vendor[16];
    uint32_t total_size;
    uint32_t received;
} program_state;

bool cmd_jtag(int argc, char **argv) {
    if (argc < 2) {
        cmd_error("usage: JTAG INIT|DETECT|RESET|FREQ|SCAN_IR|SCAN_DR|RUNTEST|SVF|PROGRAM|DEINIT");
        return true;
    }

    jtag_t *j = &jtag_instance;

    if (strcasecmp(argv[1], "INIT") == 0) {
        if (j->initialized) { cmd_ok(NULL); return true; }

        float freq = JTAG_DEFAULT_FREQ_HZ;
        if (argc >= 3) freq = strtof(argv[2], NULL);

        // Find JTAG pins from pin_manager
        int tck = -1, tms = -1, tdi = -1, tdo = -1;
        for (uint g = 0; g < PIN_MAX_GPIO; g++) {
            switch (pin_get_func(g)) {
            case PIN_FUNC_JTAG_TCK: tck = g; break;
            case PIN_FUNC_JTAG_TMS: tms = g; break;
            case PIN_FUNC_JTAG_TDI: tdi = g; break;
            case PIN_FUNC_JTAG_TDO: tdo = g; break;
            default: break;
            }
        }

        if (tck < 0 || tms < 0 || tdi < 0 || tdo < 0) {
            cmd_error("assign JTAG pins first (TCK, TMS, TDI, TDO)");
            return true;
        }

        if (!jtag_init(j, tck, tms, tdi, tdo, freq)) {
            cmd_error("JTAG init failed (TMS/TDI must be consecutive, or no PIO resources)");
            return true;
        }

        cmd_ok(NULL);
        return true;
    }

    if (strcasecmp(argv[1], "DEINIT") == 0) {
        jtag_deinit(j);
        cmd_ok(NULL);
        return true;
    }

    // All remaining commands require initialized JTAG
    if (!j->initialized) { cmd_error("not initialized"); return true; }

    if (strcasecmp(argv[1], "DETECT") == 0) {
        uint32_t idcodes[8];
        int count = jtag_detect(j, idcodes, 8);

        char buf[256];
        int pos = snprintf(buf, sizeof(buf), "%d devices", count);
        for (int i = 0; i < count; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                           "\nIDCODE[%d]=%08lX", i, (unsigned long)idcodes[i]);
        }
        cmd_ok(buf);
        return true;
    }

    if (strcasecmp(argv[1], "RESET") == 0) {
        jtag_reset(j);
        cmd_ok(NULL);
        return true;
    }

    if (strcasecmp(argv[1], "FREQ") == 0) {
        if (argc < 3) { cmd_error("usage: JTAG FREQ <hz>"); return true; }
        float freq = strtof(argv[2], NULL);
        jtag_set_freq(j, freq);
        cmd_ok(NULL);
        return true;
    }

    if (strcasecmp(argv[1], "SCAN_IR") == 0 || strcasecmp(argv[1], "SCAN_DR") == 0) {
        bool is_ir = (strcasecmp(argv[1], "SCAN_IR") == 0);
        if (argc < 4) {
            cmd_error(is_ir ? "usage: JTAG SCAN_IR <bits> <tdi_hex>"
                            : "usage: JTAG SCAN_DR <bits> <tdi_hex>");
            return true;
        }

        uint32_t bits = strtoul(argv[2], NULL, 10);
        if (bits == 0 || bits > 4096) { cmd_error("invalid bit count"); return true; }

        uint32_t num_bytes = (bits + 7) / 8;
        uint8_t tdi[512], tdo[512];
        memset(tdi, 0, sizeof(tdi));
        memset(tdo, 0, sizeof(tdo));

        parse_hex_to_bytes(argv[3], tdi, num_bytes);

        if (is_ir)
            jtag_scan_ir(j, tdi, tdo, bits, TAP_RUN_TEST_IDLE);
        else
            jtag_scan_dr(j, tdi, tdo, bits, TAP_RUN_TEST_IDLE);

        char hex_buf[1025];
        bytes_to_hex(tdo, num_bytes, hex_buf);

        char resp[1040];
        snprintf(resp, sizeof(resp), "TDO=%s", hex_buf);
        cmd_ok(resp);
        return true;
    }

    if (strcasecmp(argv[1], "RUNTEST") == 0) {
        if (argc < 3) { cmd_error("usage: JTAG RUNTEST <clocks>"); return true; }
        uint32_t clocks = strtoul(argv[2], NULL, 10);
        jtag_run_clocks(j, clocks);
        cmd_ok(NULL);
        return true;
    }

    if (strcasecmp(argv[1], "SVF") == 0) {
        if (argc < 3) { cmd_error("usage: JTAG SVF <svf_line>"); return true; }

        // Reconstruct the SVF line from remaining args
        static svf_player_t svf;
        static bool svf_inited = false;
        if (!svf_inited) {
            svf_init(&svf, j);
            svf_inited = true;
        }

        // Build line from argv[2..]
        char line[512];
        int pos = 0;
        for (int i = 2; i < argc && pos < (int)sizeof(line) - 1; i++) {
            if (i > 2) line[pos++] = ' ';
            int n = snprintf(line + pos, sizeof(line) - pos, "%s", argv[i]);
            if (n > 0) pos += n;
        }
        line[pos] = '\0';

        if (svf_exec_line(&svf, line)) {
            cmd_ok(NULL);
        } else {
            cmd_error(svf.error_msg);
            svf_inited = false; // Reset on error
        }
        return true;
    }

    if (strcasecmp(argv[1], "PROGRAM") == 0) {
        if (argc < 3) {
            cmd_error("usage: JTAG PROGRAM <vendor> BEGIN <size> | DATA <hex> | DATA END");
            return true;
        }

        if (strcasecmp(argv[2], "DATA") == 0) {
            if (!program_state.active) { cmd_error("no programming in progress"); return true; }

            if (argc >= 4 && strcasecmp(argv[3], "END") == 0) {
                // Finalize programming
                uint8_t *buf = (uint8_t *)la_capture_buffer;
                bool ok = false;

                if (strcasecmp(program_state.vendor, "XILINX") == 0) {
                    ok = xilinx_program(j, buf, program_state.received, 6);
                } else if (strcasecmp(program_state.vendor, "ALTERA") == 0) {
                    ok = altera_program(j, buf, program_state.received, 10);
                } else if (strcasecmp(program_state.vendor, "LATTICE") == 0) {
                    ok = lattice_program(j, buf, program_state.received, 8);
                }

                program_state.active = false;
                if (ok) cmd_ok("done");
                else cmd_error("programming failed");
                return true;
            }

            // DATA <hex_chunk>
            if (argc < 4) { cmd_error("usage: JTAG PROGRAM DATA <hex>"); return true; }

            uint8_t *buf = (uint8_t *)la_capture_buffer;
            uint32_t max_size = LA_BUFFER_SIZE * 4;
            const char *hex = argv[3];
            int hex_len = strlen(hex);

            for (int i = 0; i + 1 < hex_len && program_state.received < max_size; i += 2) {
                char hi = hex[i], lo = hex[i + 1];
                uint8_t val = 0;

                if (hi >= '0' && hi <= '9') val = (hi - '0') << 4;
                else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;
                else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;

                if (lo >= '0' && lo <= '9') val |= lo - '0';
                else if (lo >= 'a' && lo <= 'f') val |= lo - 'a' + 10;
                else if (lo >= 'A' && lo <= 'F') val |= lo - 'A' + 10;

                buf[program_state.received++] = val;
            }

            char resp[32];
            snprintf(resp, sizeof(resp), "%lu/%lu",
                     (unsigned long)program_state.received,
                     (unsigned long)program_state.total_size);
            cmd_ok(resp);
            return true;
        }

        // PROGRAM <vendor> BEGIN <size>
        if (argc < 5 || strcasecmp(argv[3], "BEGIN") != 0) {
            cmd_error("usage: JTAG PROGRAM <vendor> BEGIN <size>");
            return true;
        }

        strncpy(program_state.vendor, argv[2], sizeof(program_state.vendor) - 1);
        program_state.total_size = strtoul(argv[4], NULL, 10);
        program_state.received = 0;
        program_state.active = true;

        cmd_ok("ready");
        return true;
    }

    cmd_error("unknown JTAG subcommand");
    return true;
}
