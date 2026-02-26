#include "fpga_lattice.h"
#include "pico/stdlib.h"
#include <string.h>

bool lattice_match_idcode(uint32_t idcode) {
    uint32_t manufacturer = (idcode >> 1) & 0x7FF;
    return manufacturer == LATTICE_MANUFACTURER_ID;
}

static bool lattice_wait_not_busy(jtag_t *j, int ir_len, int timeout_ms) {
    uint8_t ir_val[1];
    ir_val[0] = LATTICE_IR_LSC_CHECK_BUSY;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    for (int i = 0; i < timeout_ms; i++) {
        uint8_t tdi[1] = {0};
        uint8_t tdo[1] = {0};
        jtag_scan_dr(j, tdi, tdo, 1, TAP_RUN_TEST_IDLE);

        if ((tdo[0] & 1) == 0) {
            return true;
        }
        sleep_ms(1);
    }
    return false;
}

static uint32_t lattice_read_status(jtag_t *j, int ir_len) {
    uint8_t ir_val[1];
    ir_val[0] = LATTICE_IR_LSC_READ_STATUS;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    uint8_t tdi[4] = {0};
    uint8_t tdo[4] = {0};
    jtag_scan_dr(j, tdi, tdo, 32, TAP_RUN_TEST_IDLE);

    return tdo[0] | (tdo[1] << 8) | (tdo[2] << 16) | (tdo[3] << 24);
}

bool lattice_program(jtag_t *j, const uint8_t *bitstream, uint32_t bitstream_len,
                     int ir_len) {
    uint8_t ir_val[1];

    // 1. ISC_ENABLE
    ir_val[0] = LATTICE_IR_ISC_ENABLE;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);
    uint8_t isc_enable_dr[1] = {0x00};
    jtag_scan_dr(j, isc_enable_dr, NULL, 8, TAP_RUN_TEST_IDLE);
    jtag_run_clocks(j, 1000);

    // 2. ISC_ERASE
    ir_val[0] = LATTICE_IR_ISC_ERASE;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);
    uint8_t erase_dr[1] = {0x01};
    jtag_scan_dr(j, erase_dr, NULL, 8, TAP_RUN_TEST_IDLE);

    if (!lattice_wait_not_busy(j, ir_len, 1000)) {
        return false;
    }

    // 3. LSC_BITSTREAM_BURST
    ir_val[0] = LATTICE_IR_LSC_BITSTREAM_BURST;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    jtag_goto_state(j, TAP_SHIFT_DR);

    uint32_t full_words = bitstream_len / 4;
    uint32_t remaining_bytes = bitstream_len % 4;

    if (full_words > 0) {
        jtag_bulk_write(j, (const uint32_t *)bitstream, full_words);
    }

    if (remaining_bytes > 0) {
        uint8_t tail[4] = {0};
        memcpy(tail, &bitstream[full_words * 4], remaining_bytes);
        j->state = TAP_SHIFT_DR;
        jtag_scan_dr(j, tail, NULL, remaining_bytes * 8, TAP_RUN_TEST_IDLE);
    } else {
        j->state = TAP_SHIFT_DR;
        jtag_goto_state(j, TAP_RUN_TEST_IDLE);
    }

    if (!lattice_wait_not_busy(j, ir_len, 2000)) {
        return false;
    }

    // 4. ISC_DISABLE
    ir_val[0] = LATTICE_IR_ISC_DISABLE;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);
    jtag_run_clocks(j, 1000);

    // 5. Verify status
    uint32_t status = lattice_read_status(j, ir_len);
    bool done = (status >> 8) & 1;
    bool bse_error = (status >> 13) & 1;

    return done && !bse_error;
}
