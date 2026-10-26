/* PS1 MDEC (macroblock decoder): 1F801820h command/parameter in + data out, 1F801824h
 * status (read) / control (write).  Fed by DMA0, drained by DMA1. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MDEC_FIFO_WORDS 32              /* depth of the input and of the output FIFO */

void     mdec_init(void);
uint32_t mdec_read(uint32_t off);              /* off 0: data/response, 4: status */
void     mdec_write(uint32_t off, uint32_t v); /* off 0: command/parameters, 4: control/reset */

/* DMA side, used by dma.c.  These never call back into dma.c. */
bool     mdec_dma_in_request(void);     /* DREQ0: input FIFO empty and the command expects words */
void     mdec_dma_in(uint32_t w);       /* one DMA0 word into the input FIFO */
bool     mdec_dma_out_ready(void);      /* DMA1 enabled and decoded output available */
uint32_t mdec_dma_out(int32_t *offs);   /* one DMA1 word; *offs = word offset to add to the RAM
                                           address (16x16 macroblock re-ordering, 15/24 bpp) */

/* Implemented in dma.c */
void dma_mdec_update(void);             /* MDEC state changed: let DMA0/DMA1 make progress */
void dma_event(void);                   /* scheduler EV_DMA: delayed DMA1 completion (if enabled) */
