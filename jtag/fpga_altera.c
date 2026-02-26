#include "fpga_altera.h"
#include "pico/stdlib.h"
#include <string.h>

bool altera_match_idcode(uint32_t idcode) {
    uint32_t manufacturer = (idcode >> 1) & 0x7FF;
    return manufacturer == ALTERA_MANUFACTURER_ID;
}

bool altera_program(jtag_t *j, const uint8_t *sof_data, uint32_t sof_len,
                    int ir_len) {
    uint8_t ir_val[2] = {0};

    // 1. CONFIG_IO
    ir_val[0] = ALTERA_IR_CONFIG_IO & 0xFF;
    ir_val[1] = (ALTERA_IR_CONFIG_IO >> 8) & 0xFF;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    // 2. Shift configuration data
    jtag_goto_state(j, TAP_SHIFT_DR);

    uint32_t full_words = sof_len / 4;
    uint32_t remaining_bytes = sof_len % 4;

    if (full_words > 0) {
        jtag_bulk_write(j, (const uint32_t *)sof_data, full_words);
    }

    if (remaining_bytes > 0) {
        uint8_t tail[4] = {0};
        memcpy(tail, &sof_data[full_words * 4], remaining_bytes);
        j->state = TAP_SHIFT_DR;
        jtag_scan_dr(j, tail, NULL, remaining_bytes * 8, TAP_RUN_TEST_IDLE);
    } else {
        j->state = TAP_SHIFT_DR;
        jtag_goto_state(j, TAP_RUN_TEST_IDLE);
    }

    // 3. Initialization clocks
    jtag_run_clocks(j, 134);

    // 4. Verify
    memset(ir_val, 0, sizeof(ir_val));
    ir_val[0] = ALTERA_IR_IDCODE & 0xFF;
    ir_val[1] = (ALTERA_IR_IDCODE >> 8) & 0xFF;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    uint8_t idcode_tdi[4] = {0};
    uint8_t idcode_tdo[4] = {0};
    jtag_scan_dr(j, idcode_tdi, idcode_tdo, 32, TAP_RUN_TEST_IDLE);

    uint32_t idcode = idcode_tdo[0] | (idcode_tdo[1] << 8) |
                      (idcode_tdo[2] << 16) | (idcode_tdo[3] << 24);

    return (idcode != 0 && idcode != 0xFFFFFFFF);
}
