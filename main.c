#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "cmd.h"
#include "pin_manager.h"
#include "pio_alloc.h"
#include "gpio_cmd.h"
#include "la.h"
#include "uart/pio_uart.h"
#include "uart/hw_uart.h"

#define VERSION "1.0"

static bool cmd_ping(int argc, char **argv) {
    cmd_ok("pico-fpga v" VERSION);
    return true;
}

static bool cmd_status(int argc, char **argv) {
    char pio_status[128];
    char pin_status[512];

    pio_alloc_status(pio_status, sizeof(pio_status));
    pin_query_all(pin_status, sizeof(pin_status));

    char buf[700];
    snprintf(buf, sizeof(buf), "%s pins=%s", pio_status, pin_status);
    cmd_ok(buf);
    return true;
}

static bool cmd_reset(int argc, char **argv) {
    // Deinit all modules
    la_deinit();
    // Note: UART deinit would go here for each active UART

    // Release all pins
    for (uint i = 0; i < PIN_MAX_GPIO; i++) {
        pin_release(i);
    }

    // Re-init allocator
    pio_alloc_init();

    cmd_ok(NULL);
    return true;
}

static const cmd_entry_t command_table[] = {
    { "PING",   cmd_ping },
    { "STATUS", cmd_status },
    { "RESET",  cmd_reset },
    { "PIN",    cmd_pin },
    { "GPIO",   cmd_gpio },
    { "LA",     cmd_la },
    { "UART",   cmd_uart },
    { NULL,     NULL }
};

int main(void) {
    stdio_init_all();

    // Safe boot: all pins as floating inputs
    pin_manager_init();
    pio_alloc_init();

    // LED for status
    const uint LED_PIN = PICO_DEFAULT_LED_PIN;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    cmd_buf_t cmd_buf;
    cmd_init(&cmd_buf);

    while (true) {
        // Read from USB CDC
        int c = getchar_timeout_us(100);
        if (c != PICO_ERROR_TIMEOUT) {
            if (cmd_feed(&cmd_buf, (char)c)) {
                cmd_parsed_t parsed;
                cmd_parse(&cmd_buf, &parsed);
                cmd_dispatch(&parsed, command_table);
            }
        }

        // Poll UART RX buffers
        pio_uart_poll_all();
        hw_uart_poll_all();
    }

    return 0;
}
