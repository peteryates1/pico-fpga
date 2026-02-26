#include "gpio_cmd.h"
#include "pin_manager.h"
#include "cmd.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool cmd_gpio(int argc, char **argv) {
    if (argc < 2) {
        cmd_error("usage: GPIO READ <gpio> | WRITE <gpio> 0|1 | READ_ALL");
        return true;
    }

    if (strcasecmp(argv[1], "READ") == 0) {
        if (argc < 3) { cmd_error("usage: GPIO READ <gpio>"); return true; }
        uint gpio = atoi(argv[2]);
        if (!pin_is_valid(gpio)) { cmd_error("invalid gpio"); return true; }

        pin_func_t func = pin_get_func(gpio);
        if (func != PIN_FUNC_INPUT && func != PIN_FUNC_OUTPUT) {
            cmd_error("pin not configured as INPUT or OUTPUT");
            return true;
        }

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", gpio_get(gpio) ? 1 : 0);
        cmd_ok(buf);
        return true;
    }

    if (strcasecmp(argv[1], "WRITE") == 0) {
        if (argc < 4) { cmd_error("usage: GPIO WRITE <gpio> 0|1"); return true; }
        uint gpio = atoi(argv[2]);
        if (!pin_is_valid(gpio)) { cmd_error("invalid gpio"); return true; }

        if (pin_get_func(gpio) != PIN_FUNC_OUTPUT) {
            cmd_error("pin not configured as OUTPUT");
            return true;
        }

        int val = atoi(argv[3]);
        gpio_put(gpio, val ? 1 : 0);
        cmd_ok(NULL);
        return true;
    }

    if (strcasecmp(argv[1], "READ_ALL") == 0) {
        uint32_t all = gpio_get_all();
        char buf[16];
        snprintf(buf, sizeof(buf), "%08lx", (unsigned long)all);
        cmd_ok(buf);
        return true;
    }

    cmd_error("unknown GPIO subcommand");
    return true;
}
