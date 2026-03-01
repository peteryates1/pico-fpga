#ifndef LA_H
#define LA_H

#include <stdint.h>
#include <stdbool.h>

// Capture buffer: 50K samples × 4 bytes = 200KB
#define LA_BUFFER_SIZE 50000

// Capture buffer storage
extern uint32_t la_capture_buffer[LA_BUFFER_SIZE];

typedef enum {
    LA_STATE_IDLE = 0,
    LA_STATE_CAPTURING,
    LA_STATE_DONE,
} la_state_t;

// Initialize LA module (claim PIO SM + DMA via pio_alloc)
bool la_init(void);

// Deinitialize LA module (release PIO SM + DMA)
bool la_deinit(void);

// Start non-blocking capture. divider is PIO clock divider (1.0 = 125MHz).
bool la_capture(uint32_t num_samples, float divider);

// Get current state
la_state_t la_get_state(void);

// Get number of samples captured (valid when DONE)
uint32_t la_get_sample_count(void);

// Check if LA module is initialized
bool la_is_initialized(void);

// Handle LA command
bool cmd_la(int argc, char **argv);

#endif
