#ifndef LA_H
#define LA_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/types.h"

// Capture buffer: 50K samples x 4 bytes = 200KB (linear mode)
#define LA_BUFFER_SIZE   50000

// Ring buffer for triggered capture: 8192 samples x 4 bytes = 32KB
// DMA ring_size field is 4 bits (max 15), so max ring = 2^15 = 32KB
#define LA_RING_SIZE     8192
#define LA_RING_BITS     15        // log2(LA_RING_SIZE * 4) for DMA ring wrap
#define LA_RING_MASK     (LA_RING_SIZE - 1)

// Maximum simultaneous trigger conditions (AND-combined)
#define LA_MAX_TRIGGERS  4

// Capture buffer storage (aligned for DMA ring mode)
extern uint32_t la_capture_buffer[LA_BUFFER_SIZE];

typedef enum {
    LA_STATE_IDLE = 0,
    LA_STATE_CAPTURING,    // Linear DMA capture in progress
    LA_STATE_DONE,         // Capture complete, data ready
    LA_STATE_ARMED,        // Ring DMA running, waiting for trigger
    LA_STATE_TRIGGERED,    // Trigger fired, post-trigger capture running
} la_state_t;

// Trigger condition types
typedef enum {
    TRIG_LEVEL,            // Pin must be HIGH or LOW
    TRIG_EDGE,             // Pin must have RISING or FALLING transition
    TRIG_PATTERN,          // GPIO bits must match mask+value
} la_trig_type_t;

// Single trigger condition
typedef struct {
    la_trig_type_t type;
    uint           pin;       // For level/edge
    bool           polarity;  // HIGH/RISING=true, LOW/FALLING=false
    uint32_t       mask;      // For pattern
    uint32_t       value;     // For pattern
} la_trig_cond_t;

// Initialize LA module (claim PIO SM + DMA via pio_alloc)
bool la_init(void);

// Deinitialize LA module (release PIO SM + DMA)
bool la_deinit(void);

// Start non-blocking capture. divider is PIO clock divider (1.0 = 125MHz).
bool la_capture(uint32_t num_samples, float divider);

// Get current state (also polls for trigger/DMA completion)
la_state_t la_get_state(void);

// Poll trigger conditions - call from main loop for compound trigger support
void la_poll(void);

// Get number of samples captured (valid when DONE)
uint32_t la_get_sample_count(void);

// Check if LA module is initialized
bool la_is_initialized(void);

// Trigger management
bool la_trigger_add(la_trig_type_t type, uint pin, bool polarity,
                    uint32_t mask, uint32_t value);
void la_trigger_clear(void);
int  la_trigger_count(void);

// Start triggered capture with ring DMA
// pre_pct: percentage of total_samples to capture before trigger (0-100)
bool la_arm(uint32_t total_samples, float divider, uint32_t pre_pct);

// Handle LA command
bool cmd_la(int argc, char **argv);

#endif
