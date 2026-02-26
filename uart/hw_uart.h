#ifndef HW_UART_H
#define HW_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/uart.h"

#define HW_UART_MAX       2
#define HW_UART_RX_BUFSZ  512

typedef struct {
    uart_inst_t *uart;
    uint     pin_tx;
    uint     pin_rx;
    uint32_t baud;
    bool     active;
    // RX ring buffer
    uint8_t  rx_buf[HW_UART_RX_BUFSZ];
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
} hw_uart_inst_t;

// Global HW UART instances (hw0, hw1)
extern hw_uart_inst_t hw_uarts[HW_UART_MAX];

// Validate that a pin is a valid HW UART pin for the given uart/direction
// uart_idx: 0 for uart0, 1 for uart1
// is_tx: true for TX, false for RX
bool hw_uart_valid_pin(int uart_idx, uint pin, bool is_tx);

// Initialize a single HW UART instance
// Pin assignments come from pin_manager
bool hw_uart_init_instance(hw_uart_inst_t *inst, uint32_t baud);

// Deinitialize
void hw_uart_deinit_instance(hw_uart_inst_t *inst);

// Set baud rate
void hw_uart_set_baud_instance(hw_uart_inst_t *inst, uint32_t baud);

// Non-blocking write
int hw_uart_write_instance(hw_uart_inst_t *inst, const uint8_t *data, int len);

// Poll RX
void hw_uart_poll_rx_instance(hw_uart_inst_t *inst);

// Read from RX buffer
int hw_uart_read_instance(hw_uart_inst_t *inst, uint8_t *buf, int max_len);

// Bytes available
int hw_uart_rx_available_instance(hw_uart_inst_t *inst);

// Poll all active HW UARTs (for main loop)
void hw_uart_poll_all(void);

// Poll all active PIO UARTs (for main loop)
void pio_uart_poll_all(void);

#endif
