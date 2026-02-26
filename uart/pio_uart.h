#ifndef PIO_UART_H
#define PIO_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

#define PIO_UART_MAX  4   // pio0..pio3
#define PIO_UART_RX_BUFSZ 512

typedef struct {
    PIO      pio;
    uint     sm_tx;
    uint     sm_rx;
    uint     pin_tx;
    uint     pin_rx;
    uint32_t baud;
    bool     active;
    int      pio_idx;       // Which PIO block (0 or 1)
    uint     offset_tx;     // Per-instance since we may share programs
    uint     offset_rx;
    bool     owns_program;  // True if this instance loaded the programs
    // RX ring buffer
    uint8_t  rx_buf[PIO_UART_RX_BUFSZ];
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
} pio_uart_inst_t;

// Initialize a PIO UART instance.
// Allocates 2 SMs and loads TX/RX programs via pio_alloc.
// pin_tx and pin_rx must already be assigned via pin_manager.
bool pio_uart_init(pio_uart_inst_t *inst, uint pin_tx, uint pin_rx, uint32_t baud);

// Set baud rate
void pio_uart_set_baud(pio_uart_inst_t *inst, uint32_t baud);

// Non-blocking write: returns number of bytes actually written
int pio_uart_write(pio_uart_inst_t *inst, const uint8_t *data, int len);

// Poll RX FIFO and move data into ring buffer (call frequently)
void pio_uart_poll_rx(pio_uart_inst_t *inst);

// Read from RX ring buffer
int pio_uart_read(pio_uart_inst_t *inst, uint8_t *buf, int max_len);

// Bytes available in RX buffer
int pio_uart_rx_available(pio_uart_inst_t *inst);

// Deinitialize: release SMs, programs, pins
void pio_uart_deinit(pio_uart_inst_t *inst);

// Poll all active PIO UARTs (for main loop)
void pio_uart_poll_all(void);

// Handle UART command (dispatches to PIO or HW based on id)
bool cmd_uart(int argc, char **argv);

#endif
