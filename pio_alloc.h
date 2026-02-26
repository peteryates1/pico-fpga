#ifndef PIO_ALLOC_H
#define PIO_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

// Track PIO SM/instruction/DMA usage across both PIO blocks.
// Modules request resources at INIT and release at DEINIT.

typedef struct {
    // SM usage per PIO block: bitmask of claimed SMs (bits 0-3)
    uint8_t sm_used[2];
    // Instruction memory used per PIO block
    uint8_t insn_used[2];
    // DMA channels used (bitmask)
    uint16_t dma_used;
} pio_alloc_t;

// Global allocator instance
extern pio_alloc_t pio_alloc;

// Initialize allocator
void pio_alloc_init(void);

// Try to claim SMs on a PIO block. Returns true on success.
// Claims 'count' SMs, fills sm_ids[] with the SM numbers.
bool pio_alloc_claim_sm(int pio_idx, int count, uint *sm_ids);

// Release SMs
void pio_alloc_release_sm(int pio_idx, int count, const uint *sm_ids);

// Add a PIO program. Returns offset, or -1 on failure.
int pio_alloc_add_program(int pio_idx, const pio_program_t *program);

// Remove a PIO program
void pio_alloc_remove_program(int pio_idx, const pio_program_t *program, uint offset);

// Claim a DMA channel. Returns channel number, or -1 on failure.
int pio_alloc_claim_dma(void);

// Release a DMA channel
void pio_alloc_release_dma(int chan);

// Get PIO instance from index (0 or 1)
PIO pio_alloc_get_pio(int pio_idx);

// Query: how many SMs used on a PIO block
int pio_alloc_sm_count(int pio_idx);

// Query: how many instruction words used on a PIO block
int pio_alloc_insn_count(int pio_idx);

// Query: how many DMA channels used
int pio_alloc_dma_count(void);

// Format status string
int pio_alloc_status(char *buf, int buflen);

#endif
