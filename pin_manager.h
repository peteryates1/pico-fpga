#ifndef PIN_MANAGER_H
#define PIN_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/types.h"

// Pin functions
typedef enum {
    PIN_FUNC_NONE = 0,
    PIN_FUNC_INPUT,
    PIN_FUNC_OUTPUT,
    PIN_FUNC_LA,
    PIN_FUNC_UART_TX,
    PIN_FUNC_UART_RX,
} pin_func_t;

// Pull configuration
typedef enum {
    PIN_PULL_NONE = 0,
    PIN_PULL_UP,
    PIN_PULL_DOWN,
} pin_pull_t;

// Per-pin state
typedef struct {
    pin_func_t func;
    pin_pull_t pull;
    int        owner_id;    // UART id, -1 for non-UART
} pin_state_t;

#define PIN_MAX_GPIO 29

// Initialize pin manager (all pins to NONE)
void pin_manager_init(void);

// Check if a GPIO is usable (not reserved)
bool pin_is_valid(uint gpio);

// Assign a function to a pin. Returns false on conflict.
// owner_id is used for UART to identify instance, -1 otherwise.
bool pin_assign(uint gpio, pin_func_t func, pin_pull_t pull, int owner_id);

// Release a pin (back to floating input, NONE)
bool pin_release(uint gpio);

// Get current function of a pin
pin_func_t pin_get_func(uint gpio);

// Get owner id of a pin
int pin_get_owner(uint gpio);

// Release all pins with a specific function and owner_id
// If owner_id < 0, releases all pins with that function regardless of owner.
void pin_release_func(pin_func_t func, int owner_id);

// Get function name string
const char *pin_func_name(pin_func_t func);

// Format pin query into buffer. Returns bytes written.
int pin_query_all(char *buf, int buflen);

// Format single pin query
int pin_query_one(uint gpio, char *buf, int buflen);

// Count pins with a given function
int pin_count_func(pin_func_t func);

// Handle PIN command
bool cmd_pin(int argc, char **argv);

#endif
