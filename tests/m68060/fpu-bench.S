        .equ    iterations,50000000

        .text
        .globl  _start

_start:
        lea     0x00100000,%sp

        /* Keep FP0 at 1.0 while exercising native 68060 arithmetic. */
        .word   0xf23c,0x4000          /* fmove.l #1,FP0 */
        .long   1
        .word   0xf23c,0x4080          /* fmove.l #1,FP1 */
        .long   1
        movel   #iterations,%d0
1:
        .word   0xf200,0x0422          /* fadd FP1,FP0 */
        .word   0xf200,0x0428          /* fsub FP1,FP0 */
        .word   0xf200,0x0423          /* fmul FP1,FP0 */
        .word   0xf200,0x0004          /* fsqrt FP0,FP0 */
        subql   #1,%d0
        bne     1b

        movel   #2,0xff009004
2:      stop    #0x2700
        bra     2b
