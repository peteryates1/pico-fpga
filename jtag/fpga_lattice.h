#ifndef FPGA_LATTICE_H
#define FPGA_LATTICE_H

#include <stdint.h>
#include <stdbool.h>
#include "jtag.h"

#define LATTICE_MANUFACTURER_ID 0x032

#define LATTICE_IR_IDCODE              0xE0
#define LATTICE_IR_ISC_ENABLE          0xC6
#define LATTICE_IR_ISC_ERASE           0x0E
#define LATTICE_IR_ISC_DISABLE         0x26
#define LATTICE_IR_LSC_CHECK_BUSY      0xF0
#define LATTICE_IR_LSC_READ_STATUS     0x3C
#define LATTICE_IR_LSC_BITSTREAM_BURST 0x7A
#define LATTICE_IR_BYPASS              0xFF

bool lattice_program(jtag_t *j, const uint8_t *bitstream, uint32_t bitstream_len,
                     int ir_len);
bool lattice_match_idcode(uint32_t idcode);

#endif
