#include "la.h"
#include "cmd.h"
#include "pin_manager.h"
#include "pio_alloc.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pio_instructions.h"
#include "logic_analyzer.pio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Capture buffer (200KB). Ring mode uses a 64KB-aligned region within it,
// computed at runtime to avoid wasting BSS space on alignment padding.
uint32_t la_capture_buffer[LA_BUFFER_SIZE];

static struct {
    // Core resources
    bool       initialized;
    int        pio_idx;
    uint       sm;
    uint       offset;
    int        dma_chan;
    la_state_t state;
    uint32_t   sample_count;

    // Trigger conditions
    la_trig_cond_t triggers[LA_MAX_TRIGGERS];
    int            trig_count;

    // Trigger runtime state
    bool           ring_mode;       // True if triggered capture (ring DMA)
    bool           trig_use_pio;    // True if single-condition PIO fast path
    uint           trig_sm;         // Trigger SM (PIO fast path only)
    uint           trig_offset;     // PIO program offset for trigger
    uint16_t       trig_insns[3];   // Dynamically built trigger program
    pio_program_t  trig_prog;       // Program struct for pio_alloc

    // Ring buffer bookkeeping
    uint32_t      *ring_base;       // 64KB-aligned pointer within la_capture_buffer
    uint32_t       pre_count;       // Samples before trigger
    uint32_t       post_count;      // Samples after trigger
    uint32_t       total_samples;   // pre + post
    uint32_t       trig_remaining;  // DMA transfer_count at trigger
    uint32_t       prev_gpio;       // For edge detection (CPU poll)
} la;

// --- Trigger cleanup ---

static void la_cleanup_trigger(void) {
    if (la.trig_use_pio) {
        PIO pio = pio_alloc_get_pio(la.pio_idx);
        pio_sm_set_enabled(pio, la.trig_sm, false);

        // Disable IRQ
        uint irq_num = (la.pio_idx == 0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
        irq_set_enabled(irq_num, false);
        pio_set_irq0_source_enabled(pio, pis_interrupt0, false);
        pio_interrupt_clear(pio, 0);

        // Release resources
        pio_alloc_release_sm(la.pio_idx, 1, &la.trig_sm);
        pio_alloc_remove_program(la.pio_idx, &la.trig_prog, la.trig_offset);
        la.trig_use_pio = false;
    }
    // Note: ring_mode is NOT cleared here — it must persist for DATA readout.
    // It gets cleared when starting a new capture (la_capture or la_arm).
}

// --- Trigger ISR (PIO fast path) ---

static void la_trigger_isr(void) {
    PIO pio = pio_alloc_get_pio(la.pio_idx);

    // Disable IRQ source first to prevent re-entry
    pio_set_irq0_source_enabled(pio, pis_interrupt0, false);

    // Record trigger position
    la.trig_remaining = dma_channel_hw_addr(la.dma_chan)->transfer_count;

    // Stop trigger SM
    pio_sm_set_enabled(pio, la.trig_sm, false);

    // Clear IRQ flag
    pio_interrupt_clear(pio, 0);

    la.state = LA_STATE_TRIGGERED;
}

// --- Compound trigger polling (CPU path) ---

static void la_poll_compound_trigger(void) {
    uint32_t gpio = gpio_get_all();
    bool all_met = true;

    for (int i = 0; i < la.trig_count; i++) {
        la_trig_cond_t *t = &la.triggers[i];
        bool met = false;

        switch (t->type) {
        case TRIG_LEVEL:
            met = (((gpio >> t->pin) & 1) == (t->polarity ? 1u : 0u));
            break;
        case TRIG_EDGE: {
            bool changed = ((gpio ^ la.prev_gpio) >> t->pin) & 1;
            bool level = (gpio >> t->pin) & 1;
            met = changed && (level == t->polarity);
            break;
        }
        case TRIG_PATTERN:
            met = ((gpio & t->mask) == t->value);
            break;
        }

        if (!met) { all_met = false; break; }
    }

    la.prev_gpio = gpio;

    if (all_met) {
        la.trig_remaining = dma_channel_hw_addr(la.dma_chan)->transfer_count;
        la.state = LA_STATE_TRIGGERED;
    }
}

// --- Build PIO trigger program for single condition ---

static bool la_build_trigger_program(void) {
    la_trig_cond_t *t = &la.triggers[0];
    int n = 0;

    switch (t->type) {
    case TRIG_LEVEL:
        // wait <polarity> gpio <pin>; irq set 0
        la.trig_insns[n++] = pio_encode_wait_gpio(t->polarity, t->pin);
        la.trig_insns[n++] = pio_encode_irq_set(0, false);
        break;
    case TRIG_EDGE:
        if (t->polarity) {
            // Rising: wait 0 gpio <pin>; wait 1 gpio <pin>; irq set 0
            la.trig_insns[n++] = pio_encode_wait_gpio(false, t->pin);
            la.trig_insns[n++] = pio_encode_wait_gpio(true, t->pin);
        } else {
            // Falling: wait 1 gpio <pin>; wait 0 gpio <pin>; irq set 0
            la.trig_insns[n++] = pio_encode_wait_gpio(true, t->pin);
            la.trig_insns[n++] = pio_encode_wait_gpio(false, t->pin);
        }
        la.trig_insns[n++] = pio_encode_irq_set(0, false);
        break;
    default:
        return false;  // Pattern type not supported on PIO path
    }

    la.trig_prog.instructions = la.trig_insns;
    la.trig_prog.length = n;
    la.trig_prog.origin = -1;

    // Try to load on same PIO as capture SM
    int offset = pio_alloc_add_program(la.pio_idx, &la.trig_prog);
    if (offset < 0) return false;

    la.trig_offset = (uint)offset;

    // Claim a SM on the same PIO block
    if (!pio_alloc_claim_sm(la.pio_idx, 1, &la.trig_sm)) {
        pio_alloc_remove_program(la.pio_idx, &la.trig_prog, la.trig_offset);
        return false;
    }

    return true;
}

// --- Public API ---

bool la_init(void) {
    if (la.initialized) return true;

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
    la.trig_count = 0;
    la.ring_mode = false;
    la.trig_use_pio = false;

    // Compute 32KB-aligned ring buffer base within the capture buffer
    uintptr_t aligned = ((uintptr_t)la_capture_buffer + (1u << LA_RING_BITS) - 1)
                        & ~((uintptr_t)(1u << LA_RING_BITS) - 1);
    la.ring_base = (uint32_t *)aligned;

    return true;
}

bool la_deinit(void) {
    if (!la.initialized) return true;

    PIO pio = pio_alloc_get_pio(la.pio_idx);
    pio_sm_set_enabled(pio, la.sm, false);
    dma_channel_abort(la.dma_chan);

    la_cleanup_trigger();

    pio_alloc_release_dma(la.dma_chan);
    pio_alloc_release_sm(la.pio_idx, 1, &la.sm);
    pio_alloc_remove_program(la.pio_idx, &logic_capture_program, la.offset);

    la.initialized = false;
    la.state = LA_STATE_IDLE;
    la.trig_count = 0;
    return true;
}

bool la_capture(uint32_t num_samples, float divider) {
    if (!la.initialized) return false;
    if (la.state == LA_STATE_CAPTURING || la.state == LA_STATE_ARMED ||
        la.state == LA_STATE_TRIGGERED) return false;

    if (num_samples > LA_BUFFER_SIZE)
        num_samples = LA_BUFFER_SIZE;
    if (num_samples == 0)
        num_samples = LA_BUFFER_SIZE;

    PIO pio = pio_alloc_get_pio(la.pio_idx);

    pio_sm_set_enabled(pio, la.sm, false);
    pio_sm_clear_fifos(pio, la.sm);
    pio_sm_restart(pio, la.sm);

    logic_capture_program_init(pio, la.sm, la.offset, divider);

    // Configure DMA: PIO RX FIFO -> capture buffer (linear)
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
    la.ring_mode = false;
    return true;
}

bool la_trigger_add(la_trig_type_t type, uint pin, bool polarity,
                    uint32_t mask, uint32_t value) {
    if (la.trig_count >= LA_MAX_TRIGGERS) return false;

    // Validate pin for level/edge types
    if (type == TRIG_LEVEL || type == TRIG_EDGE) {
        if (!pin_is_valid(pin)) return false;
    }

    la_trig_cond_t *t = &la.triggers[la.trig_count];
    t->type = type;
    t->pin = pin;
    t->polarity = polarity;
    t->mask = mask;
    t->value = value;
    la.trig_count++;
    return true;
}

void la_trigger_clear(void) {
    la.trig_count = 0;
}

int la_trigger_count(void) {
    return la.trig_count;
}

bool la_arm(uint32_t total_samples, float divider, uint32_t pre_pct) {
    if (!la.initialized) return false;
    if (la.state == LA_STATE_CAPTURING || la.state == LA_STATE_ARMED ||
        la.state == LA_STATE_TRIGGERED) return false;
    if (la.trig_count == 0) return false;

    // Clamp to ring buffer size
    if (total_samples > LA_RING_SIZE) total_samples = LA_RING_SIZE;
    if (total_samples == 0) total_samples = LA_RING_SIZE;
    if (pre_pct > 100) pre_pct = 100;
    if (divider < 1.0f) divider = 1.0f;

    uint32_t pre_count = (total_samples * pre_pct) / 100;
    uint32_t post_count = total_samples - pre_count;

    PIO pio = pio_alloc_get_pio(la.pio_idx);

    // Setup capture SM
    pio_sm_set_enabled(pio, la.sm, false);
    pio_sm_clear_fifos(pio, la.sm);
    pio_sm_restart(pio, la.sm);
    logic_capture_program_init(pio, la.sm, la.offset, divider);

    // Configure DMA in ring mode
    dma_channel_config c = dma_channel_get_default_config(la.dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, la.sm, false));
    channel_config_set_ring(&c, true, LA_RING_BITS);  // Wrap write address

    dma_channel_configure(
        la.dma_chan, &c,
        la.ring_base,
        &pio->rxf[la.sm],
        0xFFFFFFFF,  // Run until we stop it
        true
    );

    // Determine trigger mode
    la.trig_use_pio = (la.trig_count == 1 && la.triggers[0].type != TRIG_PATTERN);

    if (la.trig_use_pio) {
        // Build and load PIO trigger program
        if (!la_build_trigger_program()) {
            dma_channel_abort(la.dma_chan);
            return false;
        }

        // Configure trigger SM
        pio_sm_set_enabled(pio, la.trig_sm, false);
        pio_sm_clear_fifos(pio, la.trig_sm);
        pio_sm_restart(pio, la.trig_sm);

        pio_sm_config sc = pio_get_default_sm_config();
        sm_config_set_wrap(&sc, la.trig_offset,
                           la.trig_offset + la.trig_prog.length - 1);
        sm_config_set_clkdiv(&sc, 1.0f);  // Full speed for best trigger response
        pio_sm_init(pio, la.trig_sm, la.trig_offset, &sc);

        // Setup IRQ
        pio_interrupt_clear(pio, 0);
        uint irq_num = (la.pio_idx == 0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
        pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
        irq_set_exclusive_handler(irq_num, la_trigger_isr);
        irq_set_enabled(irq_num, true);

        pio_sm_set_enabled(pio, la.trig_sm, true);
    } else {
        // CPU poll path - initialize prev_gpio for edge detection
        la.prev_gpio = gpio_get_all();
    }

    // Start capture SM
    pio_sm_set_enabled(pio, la.sm, true);

    la.pre_count = pre_count;
    la.post_count = post_count;
    la.total_samples = total_samples;
    la.sample_count = total_samples;
    la.ring_mode = true;
    la.state = LA_STATE_ARMED;

    return true;
}

la_state_t la_get_state(void) {
    if (!la.initialized) return LA_STATE_IDLE;

    if (la.state == LA_STATE_CAPTURING) {
        if (!dma_channel_is_busy(la.dma_chan)) {
            PIO pio = pio_alloc_get_pio(la.pio_idx);
            pio_sm_set_enabled(pio, la.sm, false);
            la.state = LA_STATE_DONE;
        }
    } else if (la.state == LA_STATE_TRIGGERED) {
        // Check if post-trigger capture is complete
        uint32_t current_remaining = dma_channel_hw_addr(la.dma_chan)->transfer_count;
        uint32_t samples_since_trigger = la.trig_remaining - current_remaining;
        if (samples_since_trigger >= la.post_count || !dma_channel_is_busy(la.dma_chan)) {
            PIO pio = pio_alloc_get_pio(la.pio_idx);
            dma_channel_abort(la.dma_chan);
            pio_sm_set_enabled(pio, la.sm, false);
            la_cleanup_trigger();
            la.state = LA_STATE_DONE;
        }
    }

    return la.state;
}

void la_poll(void) {
    if (!la.initialized) return;

    if (la.state == LA_STATE_ARMED && !la.trig_use_pio) {
        la_poll_compound_trigger();
    } else if (la.state == LA_STATE_TRIGGERED) {
        // Also check post-trigger completion from poll
        la_get_state();
    }
}

uint32_t la_get_sample_count(void) {
    return la.sample_count;
}

bool la_is_initialized(void) {
    return la.initialized;
}

// --- Command handling ---

static void cmd_la_trigger(int argc, char **argv) {
    // argv[0]="LA", argv[1]="TRIGGER", argv[2]=subcommand, ...
    if (argc < 3) {
        cmd_error("usage: LA TRIGGER ADD|CLEAR|STATUS");
        return;
    }

    if (strcasecmp(argv[2], "CLEAR") == 0) {
        la_trigger_clear();
        cmd_ok(NULL);
        return;
    }

    if (strcasecmp(argv[2], "STATUS") == 0) {
        if (la.trig_count == 0) {
            cmd_ok("0 conditions");
            return;
        }
        // Build status string
        char buf[256];
        int pos = snprintf(buf, sizeof(buf), "%d conditions:", la.trig_count);
        for (int i = 0; i < la.trig_count && pos < (int)sizeof(buf) - 1; i++) {
            la_trig_cond_t *t = &la.triggers[i];
            switch (t->type) {
            case TRIG_LEVEL:
                pos += snprintf(buf + pos, sizeof(buf) - pos, " LEVEL %u %s",
                                t->pin, t->polarity ? "HIGH" : "LOW");
                break;
            case TRIG_EDGE:
                pos += snprintf(buf + pos, sizeof(buf) - pos, " EDGE %u %s",
                                t->pin, t->polarity ? "RISING" : "FALLING");
                break;
            case TRIG_PATTERN:
                pos += snprintf(buf + pos, sizeof(buf) - pos, " PATTERN %08lx %08lx",
                                (unsigned long)t->mask, (unsigned long)t->value);
                break;
            }
            if (i < la.trig_count - 1 && pos < (int)sizeof(buf) - 1)
                pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        }
        cmd_ok(buf);
        return;
    }

    if (strcasecmp(argv[2], "ADD") == 0) {
        if (argc < 4) {
            cmd_error("usage: LA TRIGGER ADD LEVEL|EDGE|PATTERN ...");
            return;
        }

        if (strcasecmp(argv[3], "LEVEL") == 0) {
            if (argc < 6) { cmd_error("usage: LA TRIGGER ADD LEVEL <pin> HIGH|LOW"); return; }
            uint pin = strtoul(argv[4], NULL, 10);
            bool polarity;
            if (strcasecmp(argv[5], "HIGH") == 0) polarity = true;
            else if (strcasecmp(argv[5], "LOW") == 0) polarity = false;
            else { cmd_error("expected HIGH or LOW"); return; }

            if (la_trigger_add(TRIG_LEVEL, pin, polarity, 0, 0))
                cmd_ok(NULL);
            else
                cmd_error("failed (max triggers or invalid pin)");
            return;
        }

        if (strcasecmp(argv[3], "EDGE") == 0) {
            if (argc < 6) { cmd_error("usage: LA TRIGGER ADD EDGE <pin> RISING|FALLING"); return; }
            uint pin = strtoul(argv[4], NULL, 10);
            bool polarity;
            if (strcasecmp(argv[5], "RISING") == 0) polarity = true;
            else if (strcasecmp(argv[5], "FALLING") == 0) polarity = false;
            else { cmd_error("expected RISING or FALLING"); return; }

            if (la_trigger_add(TRIG_EDGE, pin, polarity, 0, 0))
                cmd_ok(NULL);
            else
                cmd_error("failed (max triggers or invalid pin)");
            return;
        }

        if (strcasecmp(argv[3], "PATTERN") == 0) {
            if (argc < 6) { cmd_error("usage: LA TRIGGER ADD PATTERN <mask_hex> <val_hex>"); return; }
            uint32_t mask = strtoul(argv[4], NULL, 16);
            uint32_t value = strtoul(argv[5], NULL, 16);

            if (la_trigger_add(TRIG_PATTERN, 0, false, mask, value))
                cmd_ok(NULL);
            else
                cmd_error("failed (max triggers)");
            return;
        }

        cmd_error("unknown trigger type (LEVEL, EDGE, PATTERN)");
        return;
    }

    cmd_error("unknown TRIGGER subcommand (ADD, CLEAR, STATUS)");
}

bool cmd_la(int argc, char **argv) {
    if (argc < 2) {
        cmd_error("usage: LA INIT|CAPTURE|ARM|TRIGGER|STATUS|DATA|DEINIT");
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

    if (strcasecmp(argv[1], "TRIGGER") == 0) {
        if (!la.initialized) { cmd_error("not initialized"); return true; }
        cmd_la_trigger(argc, argv);
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

    if (strcasecmp(argv[1], "ARM") == 0) {
        if (!la.initialized) { cmd_error("not initialized"); return true; }

        uint32_t samples = LA_RING_SIZE;
        float divider = 1.0f;
        uint32_t pre_pct = 50;

        if (argc >= 3) samples = strtoul(argv[2], NULL, 10);
        if (argc >= 4) divider = strtof(argv[3], NULL);
        if (argc >= 5) pre_pct = strtoul(argv[4], NULL, 10);

        if (la_arm(samples, divider, pre_pct))
            cmd_ok("armed");
        else
            cmd_error("arm failed (no triggers or resource error)");
        return true;
    }

    if (strcasecmp(argv[1], "STATUS") == 0) {
        la_state_t st = la_get_state();
        char buf[64];
        switch (st) {
        case LA_STATE_IDLE:      snprintf(buf, sizeof(buf), "idle"); break;
        case LA_STATE_CAPTURING: snprintf(buf, sizeof(buf), "capturing"); break;
        case LA_STATE_DONE:      snprintf(buf, sizeof(buf), "done %lu", (unsigned long)la.sample_count); break;
        case LA_STATE_ARMED:     snprintf(buf, sizeof(buf), "armed"); break;
        case LA_STATE_TRIGGERED: snprintf(buf, sizeof(buf), "triggered"); break;
        }
        cmd_ok(buf);
        return true;
    }

    if (strcasecmp(argv[1], "DATA") == 0) {
        if (!la.initialized) { cmd_error("not initialized"); return true; }
        la_state_t st = la_get_state();
        if (st == LA_STATE_CAPTURING || st == LA_STATE_ARMED || st == LA_STATE_TRIGGERED) {
            cmd_error("capture in progress");
            return true;
        }

        uint32_t offset = 0;
        uint32_t count = la.sample_count;

        if (argc >= 3) offset = strtoul(argv[2], NULL, 10);
        if (argc >= 4) count = strtoul(argv[3], NULL, 10);

        if (offset >= la.sample_count) { cmd_error("offset out of range"); return true; }
        if (offset + count > la.sample_count) count = la.sample_count - offset;

        printf("OK %lu\n", (unsigned long)count);

        if (la.ring_mode) {
            // Ring buffer readout: unwrap from trigger position
            uint32_t trig_idx = (0xFFFFFFFF - la.trig_remaining) & LA_RING_MASK;
            uint32_t start_idx = (trig_idx - la.pre_count) & LA_RING_MASK;

            for (uint32_t i = 0; i < count; i++) {
                uint32_t idx = (start_idx + offset + i) & LA_RING_MASK;
                printf("%08lx", (unsigned long)la.ring_base[idx]);
                if ((i % 8) == 7 || i == count - 1)
                    printf("\n");
                else
                    printf(" ");
            }
        } else {
            // Linear buffer readout
            for (uint32_t i = 0; i < count; i++) {
                printf("%08lx", (unsigned long)la_capture_buffer[offset + i]);
                if ((i % 8) == 7 || i == count - 1)
                    printf("\n");
                else
                    printf(" ");
            }
        }

        return true;
    }

    cmd_error("unknown LA subcommand");
    return true;
}
