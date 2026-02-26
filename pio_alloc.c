#include "pio_alloc.h"
#include "hardware/dma.h"
#include <stdio.h>
#include <string.h>

pio_alloc_t pio_alloc;

void pio_alloc_init(void) {
    memset(&pio_alloc, 0, sizeof(pio_alloc));
}

PIO pio_alloc_get_pio(int pio_idx) {
    return (pio_idx == 0) ? pio0 : pio1;
}

bool pio_alloc_claim_sm(int pio_idx, int count, uint *sm_ids) {
    if (pio_idx < 0 || pio_idx > 1) return false;
    PIO pio = pio_alloc_get_pio(pio_idx);

    for (int i = 0; i < count; i++) {
        int sm = pio_claim_unused_sm(pio, false);
        if (sm < 0) {
            // Roll back
            for (int k = 0; k < i; k++) {
                pio_sm_unclaim(pio, sm_ids[k]);
                pio_alloc.sm_used[pio_idx] &= ~(1u << sm_ids[k]);
            }
            return false;
        }
        sm_ids[i] = (uint)sm;
        pio_alloc.sm_used[pio_idx] |= (1u << sm);
    }
    return true;
}

void pio_alloc_release_sm(int pio_idx, int count, const uint *sm_ids) {
    if (pio_idx < 0 || pio_idx > 1) return;
    PIO pio = pio_alloc_get_pio(pio_idx);

    for (int i = 0; i < count; i++) {
        pio_sm_set_enabled(pio, sm_ids[i], false);
        pio_sm_unclaim(pio, sm_ids[i]);
        pio_alloc.sm_used[pio_idx] &= ~(1u << sm_ids[i]);
    }
}

int pio_alloc_add_program(int pio_idx, const pio_program_t *program) {
    if (pio_idx < 0 || pio_idx > 1) return -1;
    PIO pio = pio_alloc_get_pio(pio_idx);

    if (!pio_can_add_program(pio, program)) return -1;

    uint offset = pio_add_program(pio, program);
    pio_alloc.insn_used[pio_idx] += program->length;
    return (int)offset;
}

void pio_alloc_remove_program(int pio_idx, const pio_program_t *program, uint offset) {
    if (pio_idx < 0 || pio_idx > 1) return;
    PIO pio = pio_alloc_get_pio(pio_idx);

    pio_remove_program(pio, program, offset);
    if (pio_alloc.insn_used[pio_idx] >= program->length)
        pio_alloc.insn_used[pio_idx] -= program->length;
    else
        pio_alloc.insn_used[pio_idx] = 0;
}

int pio_alloc_claim_dma(void) {
    int chan = dma_claim_unused_channel(false);
    if (chan < 0) return -1;
    pio_alloc.dma_used |= (1u << chan);
    return chan;
}

void pio_alloc_release_dma(int chan) {
    if (chan < 0) return;
    dma_channel_unclaim(chan);
    pio_alloc.dma_used &= ~(1u << chan);
}

int pio_alloc_sm_count(int pio_idx) {
    if (pio_idx < 0 || pio_idx > 1) return 0;
    return __builtin_popcount(pio_alloc.sm_used[pio_idx]);
}

int pio_alloc_insn_count(int pio_idx) {
    if (pio_idx < 0 || pio_idx > 1) return 0;
    return pio_alloc.insn_used[pio_idx];
}

int pio_alloc_dma_count(void) {
    return __builtin_popcount(pio_alloc.dma_used);
}

int pio_alloc_status(char *buf, int buflen) {
    return snprintf(buf, buflen, "pio0=%d/%d pio1=%d/%d dma=%d",
                    pio_alloc_sm_count(0), pio_alloc_insn_count(0),
                    pio_alloc_sm_count(1), pio_alloc_insn_count(1),
                    pio_alloc_dma_count());
}
