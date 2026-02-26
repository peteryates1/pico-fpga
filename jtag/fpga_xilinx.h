#ifndef FPGA_XILINX_H
#define FPGA_XILINX_H

#include <stdint.h>
#include <stdbool.h>
#include "jtag.h"

#define XILINX_MANUFACTURER_ID 0x049

#define XILINX_IR_IDCODE   0x09
#define XILINX_IR_JPROGRAM 0x0B
#define XILINX_IR_CFG_IN   0x05
#define XILINX_IR_JSTART   0x0C
#define XILINX_IR_BYPASS   0x3F

bool xilinx_program(jtag_t *j, const uint8_t *bitstream, uint32_t bitstream_len,
                    int ir_len);
bool xilinx_match_idcode(uint32_t idcode);

#endif
