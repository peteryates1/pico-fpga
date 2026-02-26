#include "fpga_xilinx.h"
#include "pico/stdlib.h"
#include <string.h>

bool xilinx_match_idcode(uint32_t idcode) {
    uint32_t manufacturer = (idcode >> 1) & 0x7FF;
    return manufacturer == XILINX_MANUFACTURER_ID;
}

bool xilinx_program(jtag_t *j, const uint8_t *bitstream, uint32_t bitstream_len,
                    int ir_len) {
    uint8_t ir_val[2];

    // 1. JPROGRAM
    memset(ir_val, 0, sizeof(ir_val));
    ir_val[0] = XILINX_IR_JPROGRAM;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    // 2. Wait for INIT_B
    sleep_ms(100);
    jtag_run_clocks(j, 10000);

    // 3. CFG_IN
    memset(ir_val, 0, sizeof(ir_val));
    ir_val[0] = XILINX_IR_CFG_IN;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    // 4. Shift bitstream
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

    // 5. JSTART
    memset(ir_val, 0, sizeof(ir_val));
    ir_val[0] = XILINX_IR_JSTART;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    // 6. RUNTEST
    jtag_run_clocks(j, 64);

    // 7. Verify
    memset(ir_val, 0, sizeof(ir_val));
    ir_val[0] = XILINX_IR_IDCODE;
    jtag_scan_ir(j, ir_val, NULL, ir_len, TAP_RUN_TEST_IDLE);

    uint8_t idcode_tdi[4] = {0};
    uint8_t idcode_tdo[4] = {0};
    jtag_scan_dr(j, idcode_tdi, idcode_tdo, 32, TAP_RUN_TEST_IDLE);

    uint32_t idcode = idcode_tdo[0] | (idcode_tdo[1] << 8) |
                      (idcode_tdo[2] << 16) | (idcode_tdo[3] << 24);

    return (idcode != 0 && idcode != 0xFFFFFFFF);
}
