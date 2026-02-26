#ifndef FPGA_ALTERA_H
#define FPGA_ALTERA_H

#include <stdint.h>
#include <stdbool.h>
#include "jtag.h"

#define ALTERA_MANUFACTURER_ID 0x06E

#define ALTERA_IR_IDCODE      0x006
#define ALTERA_IR_CONFIG_IO   0x00D
#define ALTERA_IR_BYPASS      0x3FF

bool altera_program(jtag_t *j, const uint8_t *sof_data, uint32_t sof_len,
                    int ir_len);
bool altera_match_idcode(uint32_t idcode);

#endif
