#ifndef JTAG_TAP_H
#define JTAG_TAP_H

#include <stdint.h>

typedef enum {
    TAP_TEST_LOGIC_RESET = 0,
    TAP_RUN_TEST_IDLE,
    TAP_SELECT_DR_SCAN,
    TAP_CAPTURE_DR,
    TAP_SHIFT_DR,
    TAP_EXIT1_DR,
    TAP_PAUSE_DR,
    TAP_EXIT2_DR,
    TAP_UPDATE_DR,
    TAP_SELECT_IR_SCAN,
    TAP_CAPTURE_IR,
    TAP_SHIFT_IR,
    TAP_EXIT1_IR,
    TAP_PAUSE_IR,
    TAP_EXIT2_IR,
    TAP_UPDATE_IR,
    TAP_STATE_COUNT
} tap_state_t;

// Result of path calculation
typedef struct {
    uint32_t tms_bits;  // TMS bit sequence, LSB first
    uint8_t  num_clocks; // Number of TCK clocks needed
} tap_path_t;

// Get next state given current state and TMS value (0 or 1)
tap_state_t tap_next_state(tap_state_t current, int tms);

// Calculate TMS bit sequence to navigate from one state to another
tap_path_t tap_calc_path(tap_state_t from, tap_state_t to);

// Get human-readable name for a TAP state
const char *tap_state_name(tap_state_t state);

#endif
