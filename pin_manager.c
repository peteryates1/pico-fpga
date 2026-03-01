#include "pin_manager.h"
#include "cmd.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static pin_state_t pins[PIN_MAX_GPIO];

void pin_manager_init(void) {
    memset(pins, 0, sizeof(pins));
    // Set all usable GPIOs as floating inputs
    for (uint i = 0; i < PIN_MAX_GPIO; i++) {
        if (pin_is_valid(i)) {
            gpio_init(i);
            gpio_set_dir(i, GPIO_IN);
            gpio_disable_pulls(i);
        }
    }
}

bool pin_is_valid(uint gpio) {
    if (gpio >= PIN_MAX_GPIO) return false;
    // GPIO 23-25 reserved (board functions on Pico)
    if (gpio >= 23 && gpio <= 25) return false;
    return true;
}

bool pin_assign(uint gpio, pin_func_t func, pin_pull_t pull, int owner_id) {
    if (!pin_is_valid(gpio)) return false;
    if (pins[gpio].func != PIN_FUNC_NONE) return false;

    pins[gpio].func = func;
    pins[gpio].pull = pull;
    pins[gpio].owner_id = owner_id;

    // Configure GPIO hardware for INPUT/OUTPUT functions
    if (func == PIN_FUNC_INPUT) {
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        switch (pull) {
        case PIN_PULL_UP:   gpio_pull_up(gpio); break;
        case PIN_PULL_DOWN: gpio_pull_down(gpio); break;
        default:            gpio_disable_pulls(gpio); break;
        }
    } else if (func == PIN_FUNC_OUTPUT) {
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_OUT);
        gpio_put(gpio, 0);
    }
    // For LA, UART: GPIO hardware setup is done by the module at INIT time

    return true;
}

bool pin_release(uint gpio) {
    if (!pin_is_valid(gpio)) return false;
    if (pins[gpio].func == PIN_FUNC_NONE) return true; // already released

    pins[gpio].func = PIN_FUNC_NONE;
    pins[gpio].pull = PIN_PULL_NONE;
    pins[gpio].owner_id = -1;

    // Reset to floating input
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_disable_pulls(gpio);

    return true;
}

pin_func_t pin_get_func(uint gpio) {
    if (gpio >= PIN_MAX_GPIO) return PIN_FUNC_NONE;
    return pins[gpio].func;
}

int pin_get_owner(uint gpio) {
    if (gpio >= PIN_MAX_GPIO) return -1;
    return pins[gpio].owner_id;
}

void pin_release_func(pin_func_t func, int owner_id) {
    for (uint i = 0; i < PIN_MAX_GPIO; i++) {
        if (pins[i].func == func) {
            if (owner_id < 0 || pins[i].owner_id == owner_id) {
                pin_release(i);
            }
        }
    }
}

const char *pin_func_name(pin_func_t func) {
    switch (func) {
    case PIN_FUNC_NONE:     return "NONE";
    case PIN_FUNC_INPUT:    return "INPUT";
    case PIN_FUNC_OUTPUT:   return "OUTPUT";
    case PIN_FUNC_LA:       return "LA";
    case PIN_FUNC_UART_TX:  return "UART_TX";
    case PIN_FUNC_UART_RX:  return "UART_RX";
    default:                return "?";
    }
}

int pin_query_all(char *buf, int buflen) {
    int pos = 0;
    bool first = true;
    for (uint i = 0; i < PIN_MAX_GPIO; i++) {
        if (!pin_is_valid(i)) continue;
        if (pins[i].func == PIN_FUNC_NONE) continue;

        int n;
        if (pins[i].func == PIN_FUNC_UART_TX || pins[i].func == PIN_FUNC_UART_RX) {
            n = snprintf(buf + pos, buflen - pos, "%s%u:%s(%d)",
                         first ? "" : " ", i, pin_func_name(pins[i].func),
                         pins[i].owner_id);
        } else {
            n = snprintf(buf + pos, buflen - pos, "%s%u:%s",
                         first ? "" : " ", i, pin_func_name(pins[i].func));
        }
        if (n > 0) pos += n;
        first = false;
    }
    if (first) {
        pos = snprintf(buf, buflen, "no pins assigned");
    }
    return pos;
}

int pin_query_one(uint gpio, char *buf, int buflen) {
    if (!pin_is_valid(gpio)) {
        return snprintf(buf, buflen, "%u:RESERVED", gpio);
    }
    if (pins[gpio].func == PIN_FUNC_UART_TX || pins[gpio].func == PIN_FUNC_UART_RX) {
        return snprintf(buf, buflen, "%u:%s(%d)", gpio,
                        pin_func_name(pins[gpio].func), pins[gpio].owner_id);
    }
    return snprintf(buf, buflen, "%u:%s", gpio, pin_func_name(pins[gpio].func));
}

// Parse UART id string like "pio0", "pio1", "hw0", "hw1"
// Returns 0-3 for pio0-pio3, 100-101 for hw0-hw1, -1 on error
static int parse_uart_id(const char *s) {
    if (strncasecmp(s, "pio", 3) == 0) {
        int n = atoi(s + 3);
        if (n >= 0 && n <= 3) return n;
    } else if (strncasecmp(s, "hw", 2) == 0) {
        int n = atoi(s + 2);
        if (n >= 0 && n <= 1) return 100 + n;
    }
    return -1;
}

bool cmd_pin(int argc, char **argv) {
    // PIN QUERY [gpio]
    if (argc >= 2 && strcasecmp(argv[1], "QUERY") == 0) {
        char buf[512];
        if (argc >= 3) {
            uint gpio = atoi(argv[2]);
            pin_query_one(gpio, buf, sizeof(buf));
        } else {
            pin_query_all(buf, sizeof(buf));
        }
        cmd_ok(buf);
        return true;
    }

    // PIN <gpio> RELEASE
    if (argc >= 3 && strcasecmp(argv[2], "RELEASE") == 0) {
        uint gpio = atoi(argv[1]);
        if (!pin_is_valid(gpio)) {
            cmd_error("invalid gpio");
            return true;
        }
        pin_release(gpio);
        cmd_ok(NULL);
        return true;
    }

    // PIN <gpio> FUNC <function> [args...]
    if (argc < 4 || strcasecmp(argv[2], "FUNC") != 0) {
        cmd_error("usage: PIN <gpio> FUNC <function> | PIN <gpio> RELEASE | PIN QUERY");
        return true;
    }

    uint gpio = atoi(argv[1]);
    if (!pin_is_valid(gpio)) {
        cmd_error("invalid gpio");
        return true;
    }

    const char *func_str = argv[3];
    pin_func_t func;
    pin_pull_t pull = PIN_PULL_NONE;
    int owner_id = -1;

    if (strcasecmp(func_str, "INPUT") == 0) {
        func = PIN_FUNC_INPUT;
        if (argc >= 5) {
            if (strcasecmp(argv[4], "PULLUP") == 0) pull = PIN_PULL_UP;
            else if (strcasecmp(argv[4], "PULLDOWN") == 0) pull = PIN_PULL_DOWN;
        }
    } else if (strcasecmp(func_str, "OUTPUT") == 0) {
        func = PIN_FUNC_OUTPUT;
    } else if (strcasecmp(func_str, "LA") == 0) {
        func = PIN_FUNC_LA;
    } else if (strcasecmp(func_str, "UART_TX") == 0) {
        if (argc < 5) { cmd_error("UART_TX requires <id>"); return true; }
        owner_id = parse_uart_id(argv[4]);
        if (owner_id < 0) { cmd_error("invalid UART id"); return true; }
        func = PIN_FUNC_UART_TX;
    } else if (strcasecmp(func_str, "UART_RX") == 0) {
        if (argc < 5) { cmd_error("UART_RX requires <id>"); return true; }
        owner_id = parse_uart_id(argv[4]);
        if (owner_id < 0) { cmd_error("invalid UART id"); return true; }
        func = PIN_FUNC_UART_RX;
    } else {
        cmd_error("unknown function");
        return true;
    }

    if (pins[gpio].func != PIN_FUNC_NONE) {
        cmd_error("pin already assigned, RELEASE first");
        return true;
    }

    if (!pin_assign(gpio, func, pull, owner_id)) {
        cmd_error("pin assignment failed");
        return true;
    }

    cmd_ok(NULL);
    return true;
}
