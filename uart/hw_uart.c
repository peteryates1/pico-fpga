#include "hw_uart.h"
#include "pio_uart.h"
#include "pin_manager.h"
#include "hardware/gpio.h"

hw_uart_inst_t hw_uarts[HW_UART_MAX];

// Valid HW UART pins for RP2040:
// UART0 TX: 0, 12, 16   RX: 1, 13, 17
// UART1 TX: 4, 8, 20    RX: 5, 9, 21
static const uint uart0_tx_pins[] = {0, 12, 16};
static const uint uart0_rx_pins[] = {1, 13, 17};
static const uint uart1_tx_pins[] = {4, 8, 20};
static const uint uart1_rx_pins[] = {5, 9, 21};

bool hw_uart_valid_pin(int uart_idx, uint pin, bool is_tx) {
    const uint *valid;
    int count;

    if (uart_idx == 0) {
        if (is_tx) { valid = uart0_tx_pins; count = 3; }
        else       { valid = uart0_rx_pins; count = 3; }
    } else if (uart_idx == 1) {
        if (is_tx) { valid = uart1_tx_pins; count = 3; }
        else       { valid = uart1_rx_pins; count = 3; }
    } else {
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (valid[i] == pin) return true;
    }
    return false;
}

bool hw_uart_init_instance(hw_uart_inst_t *inst, uint32_t baud) {
    if (inst->active) return true;

    // Determine which HW UART this is
    int idx = (int)(inst - hw_uarts);
    if (idx < 0 || idx >= HW_UART_MAX) return false;

    inst->uart = (idx == 0) ? uart0 : uart1;

    // Find TX and RX pins from pin_manager
    int tx_pin = -1, rx_pin = -1;
    int owner = 100 + idx;

    for (uint g = 0; g < PIN_MAX_GPIO; g++) {
        if (pin_get_func(g) == PIN_FUNC_UART_TX && pin_get_owner(g) == owner)
            tx_pin = g;
        if (pin_get_func(g) == PIN_FUNC_UART_RX && pin_get_owner(g) == owner)
            rx_pin = g;
    }

    if (tx_pin < 0 || rx_pin < 0) return false;

    // Validate pins
    if (!hw_uart_valid_pin(idx, tx_pin, true)) return false;
    if (!hw_uart_valid_pin(idx, rx_pin, false)) return false;

    inst->pin_tx = tx_pin;
    inst->pin_rx = rx_pin;
    inst->baud = baud;
    inst->rx_head = 0;
    inst->rx_tail = 0;

    uart_init(inst->uart, baud);
    gpio_set_function(inst->pin_tx, GPIO_FUNC_UART);
    gpio_set_function(inst->pin_rx, GPIO_FUNC_UART);
    uart_set_format(inst->uart, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(inst->uart, false, false);
    uart_set_fifo_enabled(inst->uart, true);

    inst->active = true;
    return true;
}

void hw_uart_deinit_instance(hw_uart_inst_t *inst) {
    if (!inst->active) return;
    uart_deinit(inst->uart);
    inst->active = false;
}

void hw_uart_set_baud_instance(hw_uart_inst_t *inst, uint32_t baud) {
    if (!inst->active) return;
    inst->baud = baud;
    uart_set_baudrate(inst->uart, baud);
}

int hw_uart_write_instance(hw_uart_inst_t *inst, const uint8_t *data, int len) {
    if (!inst->active) return 0;

    int written = 0;
    for (int i = 0; i < len; i++) {
        if (!uart_is_writable(inst->uart)) break;
        uart_putc_raw(inst->uart, data[i]);
        written++;
    }
    return written;
}

void hw_uart_poll_rx_instance(hw_uart_inst_t *inst) {
    if (!inst->active) return;

    while (uart_is_readable(inst->uart)) {
        uint8_t byte = uart_getc(inst->uart);
        uint16_t next_head = (inst->rx_head + 1) % HW_UART_RX_BUFSZ;
        if (next_head != inst->rx_tail) {
            inst->rx_buf[inst->rx_head] = byte;
            inst->rx_head = next_head;
        }
    }
}

int hw_uart_read_instance(hw_uart_inst_t *inst, uint8_t *buf, int max_len) {
    int count = 0;
    while (count < max_len && inst->rx_tail != inst->rx_head) {
        buf[count++] = inst->rx_buf[inst->rx_tail];
        inst->rx_tail = (inst->rx_tail + 1) % HW_UART_RX_BUFSZ;
    }
    return count;
}

int hw_uart_rx_available_instance(hw_uart_inst_t *inst) {
    return (inst->rx_head - inst->rx_tail + HW_UART_RX_BUFSZ) % HW_UART_RX_BUFSZ;
}

void hw_uart_poll_all(void) {
    for (int i = 0; i < HW_UART_MAX; i++) {
        hw_uart_poll_rx_instance(&hw_uarts[i]);
    }
}

