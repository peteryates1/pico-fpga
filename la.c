#include "la.h"
#include "cmd.h"
#include "pin_manager.h"
#include "pio_alloc.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "logic_analyzer.pio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t la_capture_buffer[LA_BUFFER_SIZE];

static struct {
    bool     initialized;
    int      pio_idx;
    uint     sm;
    uint     offset;
    int      dma_chan;
    la_state_t state;
    uint32_t sample_count;
} la;

bool la_init(void) {
    if (la.initialized) return true;

    // Try PIO0 first, then PIO1
    int pio_idx = -1;
    int offset = -1;
    uint sm;

    for (int try = 0; try < 2; try++) {
        offset = pio_alloc_add_program(try, &logic_capture_program);
        if (offset >= 0) {
            if (pio_alloc_claim_sm(try, 1, &sm)) {
                pio_idx = try;
                break;
            }
            pio_alloc_remove_program(try, &logic_capture_program, offset);
            offset = -1;
        }
    }

    if (pio_idx < 0) return false;

    int dma_chan = pio_alloc_claim_dma();
    if (dma_chan < 0) {
        pio_alloc_release_sm(pio_idx, 1, &sm);
        pio_alloc_remove_program(pio_idx, &logic_capture_program, offset);
        return false;
    }

    la.initialized = true;
    la.pio_idx = pio_idx;
    la.sm = sm;
    la.offset = offset;
    la.dma_chan = dma_chan;
    la.state = LA_STATE_IDLE;
    la.sample_count = 0;

    return true;
}

bool la_deinit(void) {
    if (!la.initialized) return true;

    // Abort any in-progress capture
    PIO pio = pio_alloc_get_pio(la.pio_idx);
    pio_sm_set_enabled(pio, la.sm, false);
    dma_channel_abort(la.dma_chan);

    pio_alloc_release_dma(la.dma_chan);
    pio_alloc_release_sm(la.pio_idx, 1, &la.sm);
    pio_alloc_remove_program(la.pio_idx, &logic_capture_program, la.offset);

    la.initialized = false;
    la.state = LA_STATE_IDLE;
    return true;
}

bool la_capture(uint32_t num_samples, float divider) {
    if (!la.initialized) return false;
    if (la.state == LA_STATE_CAPTURING) return false;

    if (num_samples > LA_BUFFER_SIZE)
        num_samples = LA_BUFFER_SIZE;
    if (num_samples == 0)
        num_samples = LA_BUFFER_SIZE;

    PIO pio = pio_alloc_get_pio(la.pio_idx);

    // Configure pins: find all LA-assigned pins, use lowest as base
    // The PIO program samples starting at GPIO 0 anyway (in pins, 32)
    // so we just need the GPIO hardware set up as inputs — that's done by pin_manager

    // Stop SM, clear FIFO, reinit
    pio_sm_set_enabled(pio, la.sm, false);
    pio_sm_clear_fifos(pio, la.sm);
    pio_sm_restart(pio, la.sm);

    logic_capture_program_init(pio, la.sm, la.offset, divider);

    // Configure DMA: PIO RX FIFO → capture buffer
    dma_channel_config c = dma_channel_get_default_config(la.dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, la.sm, false));

    dma_channel_configure(
        la.dma_chan, &c,
        la_capture_buffer,
        &pio->rxf[la.sm],
        num_samples,
        true
    );

    pio_sm_set_enabled(pio, la.sm, true);

    la.state = LA_STATE_CAPTURING;
    la.sample_count = num_samples;
    return true;
}

la_state_t la_get_state(void) {
    if (!la.initialized) return LA_STATE_IDLE;

    if (la.state == LA_STATE_CAPTURING) {
        // Check if DMA finished
        if (!dma_channel_is_busy(la.dma_chan)) {
            PIO pio = pio_alloc_get_pio(la.pio_idx);
            pio_sm_set_enabled(pio, la.sm, false);
            la.state = LA_STATE_DONE;
        }
    }
    return la.state;
}

uint32_t la_get_sample_count(void) {
    return la.sample_count;
}

bool la_is_initialized(void) {
    return la.initialized;
}

bool cmd_la(int argc, char **argv) {
    if (argc < 2) {
        cmd_error("usage: LA INIT|CAPTURE|STATUS|DATA|DEINIT");
        return true;
    }

    if (strcasecmp(argv[1], "INIT") == 0) {
        if (la_init()) cmd_ok(NULL);
        else cmd_error("failed to allocate PIO/DMA resources");
        return true;
    }

    if (strcasecmp(argv[1], "DEINIT") == 0) {
        la_deinit();
        cmd_ok(NULL);
        return true;
    }

    if (strcasecmp(argv[1], "CAPTURE") == 0) {
        if (!la.initialized) { cmd_error("not initialized"); return true; }

        uint32_t samples = LA_BUFFER_SIZE;
        float divider = 1.0f;

        if (argc >= 3) samples = strtoul(argv[2], NULL, 10);
        if (argc >= 4) divider = strtof(argv[3], NULL);

        if (divider < 1.0f) divider = 1.0f;

        if (la_capture(samples, divider))
            cmd_ok("capturing");
        else
            cmd_error("capture failed");
        return true;
    }

    if (strcasecmp(argv[1], "STATUS") == 0) {
        la_state_t st = la_get_state();
        char buf[64];
        switch (st) {
        case LA_STATE_IDLE:      snprintf(buf, sizeof(buf), "idle"); break;
        case LA_STATE_CAPTURING: snprintf(buf, sizeof(buf), "capturing"); break;
        case LA_STATE_DONE:      snprintf(buf, sizeof(buf), "done %lu", (unsigned long)la.sample_count); break;
        }
        cmd_ok(buf);
        return true;
    }

    if (strcasecmp(argv[1], "DATA") == 0) {
        if (!la.initialized) { cmd_error("not initialized"); return true; }
        if (la_get_state() == LA_STATE_CAPTURING) { cmd_error("capture in progress"); return true; }

        uint32_t offset = 0;
        uint32_t count = la.sample_count;

        if (argc >= 3) offset = strtoul(argv[2], NULL, 10);
        if (argc >= 4) count = strtoul(argv[3], NULL, 10);

        if (offset >= la.sample_count) { cmd_error("offset out of range"); return true; }
        if (offset + count > la.sample_count) count = la.sample_count - offset;

        printf("OK %lu\n", (unsigned long)count);

        // Output 8 samples per line as hex
        for (uint32_t i = 0; i < count; i++) {
            printf("%08lx", (unsigned long)la_capture_buffer[offset + i]);
            if ((i % 8) == 7 || i == count - 1)
                printf("\n");
            else
                printf(" ");
        }

        return true;
    }

    cmd_error("unknown LA subcommand");
    return true;
}
