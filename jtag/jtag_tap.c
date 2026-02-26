#include "jtag_tap.h"

// TAP state transition table: [state][tms] = next_state
static const tap_state_t tap_transitions[TAP_STATE_COUNT][2] = {
    /*                          TMS=0                TMS=1              */
    /* TEST_LOGIC_RESET */ { TAP_RUN_TEST_IDLE,  TAP_TEST_LOGIC_RESET },
    /* RUN_TEST_IDLE    */ { TAP_RUN_TEST_IDLE,  TAP_SELECT_DR_SCAN   },
    /* SELECT_DR_SCAN   */ { TAP_CAPTURE_DR,     TAP_SELECT_IR_SCAN   },
    /* CAPTURE_DR       */ { TAP_SHIFT_DR,       TAP_EXIT1_DR         },
    /* SHIFT_DR         */ { TAP_SHIFT_DR,       TAP_EXIT1_DR         },
    /* EXIT1_DR         */ { TAP_PAUSE_DR,       TAP_UPDATE_DR        },
    /* PAUSE_DR         */ { TAP_PAUSE_DR,       TAP_EXIT2_DR         },
    /* EXIT2_DR         */ { TAP_SHIFT_DR,       TAP_UPDATE_DR        },
    /* UPDATE_DR        */ { TAP_RUN_TEST_IDLE,  TAP_SELECT_DR_SCAN   },
    /* SELECT_IR_SCAN   */ { TAP_CAPTURE_IR,     TAP_TEST_LOGIC_RESET },
    /* CAPTURE_IR       */ { TAP_SHIFT_IR,       TAP_EXIT1_IR         },
    /* SHIFT_IR         */ { TAP_SHIFT_IR,       TAP_EXIT1_IR         },
    /* EXIT1_IR         */ { TAP_PAUSE_IR,       TAP_UPDATE_IR        },
    /* PAUSE_IR         */ { TAP_PAUSE_IR,       TAP_EXIT2_IR         },
    /* EXIT2_IR         */ { TAP_SHIFT_IR,       TAP_UPDATE_IR        },
    /* UPDATE_IR        */ { TAP_RUN_TEST_IDLE,  TAP_SELECT_DR_SCAN   },
};

tap_state_t tap_next_state(tap_state_t current, int tms) {
    return tap_transitions[current][tms ? 1 : 0];
}

// BFS shortest path through TAP state machine
tap_path_t tap_calc_path(tap_state_t from, tap_state_t to) {
    tap_path_t result = {0, 0};

    if (from == to) {
        return result;
    }

    uint8_t visited[TAP_STATE_COUNT] = {0};
    tap_state_t parent[TAP_STATE_COUNT];
    uint8_t parent_tms[TAP_STATE_COUNT];
    tap_state_t queue[TAP_STATE_COUNT];
    int head = 0, tail = 0;

    visited[from] = 1;
    queue[tail++] = from;

    while (head < tail) {
        tap_state_t cur = queue[head++];
        for (int tms = 0; tms <= 1; tms++) {
            tap_state_t next = tap_transitions[cur][tms];
            if (!visited[next]) {
                visited[next] = 1;
                parent[next] = cur;
                parent_tms[next] = tms;
                queue[tail++] = next;
                if (next == to) {
                    goto found;
                }
            }
        }
    }

    // Should never happen
    result.tms_bits = 0x3F;
    result.num_clocks = 6;
    return result;

found:;
    tap_state_t path_states[8];
    uint8_t path_tms[8];
    int path_len = 0;

    tap_state_t s = to;
    while (s != from) {
        path_states[path_len] = s;
        path_tms[path_len] = parent_tms[s];
        path_len++;
        s = parent[s];
    }

    result.num_clocks = path_len;
    result.tms_bits = 0;
    for (int i = 0; i < path_len; i++) {
        if (path_tms[path_len - 1 - i]) {
            result.tms_bits |= (1u << i);
        }
    }

    return result;
}

static const char *state_names[TAP_STATE_COUNT] = {
    "Test-Logic-Reset",
    "Run-Test/Idle",
    "Select-DR-Scan",
    "Capture-DR",
    "Shift-DR",
    "Exit1-DR",
    "Pause-DR",
    "Exit2-DR",
    "Update-DR",
    "Select-IR-Scan",
    "Capture-IR",
    "Shift-IR",
    "Exit1-IR",
    "Pause-IR",
    "Exit2-IR",
    "Update-IR",
};

const char *tap_state_name(tap_state_t state) {
    if (state < TAP_STATE_COUNT) {
        return state_names[state];
    }
    return "UNKNOWN";
}
