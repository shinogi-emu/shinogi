        .equ    iterations,50000000

        .text
        .globl  _start

_start:
        lea     0x00100000,%sp
        lea     fpvalue,%a0
        .word   0xf23c,0x4000          /* fmove.l #1,FP0 */
        .long   1
        movel   #iterations,%d0
1:
        .word   0xf201,0x6400          /* fmove.s FP0,D1 */
        .word   0xf201,0x4400          /* fmove.s D1,FP0 */
        .word   0xf210,0x7400          /* fmove.d FP0,(A0) */
        .word   0xf210,0x5400          /* fmove.d (A0),FP0 */
        subql   #1,%d0
        bne     1b

        movel   #2,0xff009004
2:      stop    #0x2700
        bra     2b

        .data
        .balign 8
fpvalue:
        .space  8
