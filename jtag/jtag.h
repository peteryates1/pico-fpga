#ifndef JTAG_H
#define JTAG_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"
#include "jtag_tap.h"

// Default TCK frequency
#define JTAG_DEFAULT_FREQ_HZ 6250000  // 6.25 MHz

typedef struct {
    PIO pio;
    int pio_idx;                // 0 or 1
    uint sm;                    // State machine for jtag_io
    uint offset_io;             // Instruction memory offset for jtag_io
    uint offset_bulk;           // Instruction memory offset for jtag_bulk_write
    tap_state_t state;          // Current TAP state
    float freq_hz;              // Current TCK frequency
    int dma_chan;               // DMA channel for bulk writes
    // Pin assignments (parameterized)
    uint pin_tck;
    uint pin_tms;
    uint pin_tdi;
    uint pin_tdo;
    bool initialized;
} jtag_t;

// Global JTAG instance
extern jtag_t jtag_instance;

// Initialize JTAG with specified pins
// Allocates PIO SM + DMA via pio_alloc
bool jtag_init(jtag_t *j, uint pin_tck, uint pin_tms, uint pin_tdi, uint pin_tdo,
               float freq_hz);

// Set TCK frequency
void jtag_set_freq(jtag_t *j, float freq_hz);

// Reset TAP (5x TMS=1 clocks)
void jtag_reset(jtag_t *j);

// Navigate TAP state machine to target state
void jtag_goto_state(jtag_t *j, tap_state_t target);

// Scan IR: shift num_bits from tdi_data[], capture TDO into tdo_data[]
void jtag_scan_ir(jtag_t *j, const uint8_t *tdi_data, uint8_t *tdo_data,
                  uint32_t num_bits, tap_state_t end_state);

// Scan DR: shift num_bits from tdi_data[], capture TDO into tdo_data[]
void jtag_scan_dr(jtag_t *j, const uint8_t *tdi_data, uint8_t *tdo_data,
                  uint32_t num_bits, tap_state_t end_state);

// Clock TCK num_clocks times with TMS=0 (RUNTEST in Run-Test/Idle)
void jtag_run_clocks(jtag_t *j, uint32_t num_clocks);

// DMA bulk write: shift data_words 32-bit words to TDI with TMS=0
void jtag_bulk_write(jtag_t *j, const uint32_t *data, uint32_t data_words);

// Read IDCODE from all devices in the chain
int jtag_detect(jtag_t *j, uint32_t *idcodes, int max_devices);

// Deinitialize JTAG: release all resources
void jtag_deinit(jtag_t *j);

// Handle JTAG command
bool cmd_jtag(int argc, char **argv);

#endif
