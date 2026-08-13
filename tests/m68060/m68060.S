        .text
        .globl  _start

_start:
        lea     0x00100000,%sp
        lea     unimp_int_handler,%a0
        movel   %a0,0x000000f4
        lea     linef_handler,%a0
        movel   %a0,0x0000002c
        lea     illegal_handler,%a0
        movel   %a0,0x00000010
        lea     unimp_ea_handler,%a0
        movel   %a0,0x000000f0
        lea     fail_handler,%a0
        movel   %a0,0x00000008
        movel   %a0,0x0000000c
        movel   %a0,0x00000038
        clrl    unimp_count
        clrl    linef_count
        clrl    illegal_count
        clrl    unimp_ea_count

        /* 68060 control registers. */
        .word   0x4e7a,0x0808          /* movec PCR,D0 */
        movel   %d0,%d1
        andil   #0xffff0000,%d1
        cmpil   #0x04300000,%d1
        bne     fail01
        movel   #0xa0000000,%d0
        .word   0x4e7b,0x0008          /* movec D0,BUSCR */
        .word   0x4e7a,0x1008          /* movec BUSCR,D1 */
        cmpl    %d0,%d1
        bne     fail02
        movel   #0x00ffa040,%d0
        .word   0x4e7b,0x0006          /* movec D0,DTT0 */
        .word   0x4e7a,0x1006          /* movec DTT0,D1 */
        cmpl    %d0,%d1
        bne     fail03

        /* Native 68060 MMU/cache/data-transfer instructions. */
        lea     source16,%a0
        lea     dest16,%a1
        .word   0xf620,0x9000          /* move16 (A0)+,(A1)+ */
        cmpl    #source16+16,%a0
        bne     fail04
        cmpl    #dest16+16,%a1
        bne     fail05
        movel   dest16,%d0
        cmpl    source16,%d0
        bne     fail06
        .word   0xf518                 /* pflusha */
        lea     source16,%a0
        .word   0xf5c8                 /* plpar (A0), MMU disabled */
        cmpl    #source16,%a0
        bne     fail07
        .word   0xf4d8                 /* cinva */
        .word   0xf478                 /* cpusha */

        /* Instructions emulated by M68060SP must use vector 61. */
        movel   #4,skip_bytes
        lea     source16,%a0
        .word   0x0188,0x0000          /* movep.w D0,(0,A0) */
        .word   0x4e7a,0x0008          /* movec BUSCR,D0 */
        cmpil   #0x50000000,%d0        /* SL/SLE saved, L/LE cleared */
        bne     fail20
        .word   0x00d0,0x0000          /* cmp2.b (A0),D0 */
        .word   0x00d0,0x0800          /* chk2.b (A0),D0 */
        movel   #6,skip_bytes
        .word   0x0cfc,0x0000,0x0000   /* cas2.w */
        movel   #4,skip_bytes
        .word   0x4c00,0x0400          /* mulu.l D0,D0:D0 */
        .word   0x4c40,0x0400          /* divu.l D0,D0:D0 */
        lea     source16+1,%a0
        .word   0x0cd0,0x0000          /* misaligned cas.w D0,D0,(A0) */
        cmpil   #7,unimp_count
        bne     fail08

        /* Removed PTEST is an unimplemented F-line, format 0. */
        movel   #2,skip_bytes
        .word   0xf548

        /* 68060 FPU instructions implemented by M68060SP. */
        movel   #4,skip_bytes
        .word   0xf200,0x5c00          /* fmovecr #0,FP0 */
        .word   0xf200,0x000e          /* fsin FP0,FP0 */
        .word   0xf240,0x0000          /* fsf D0 */
        movel   #6,skip_bytes
        .word   0xf248,0x0000,0x0000   /* fdbf D0,. */
        movel   #4,skip_bytes
        .word   0xf27c,0x0000          /* ftrapf */

        /* PCR.DFP produces a format-4 F-line frame. */
        .word   0x4e7a,0x0808          /* movec PCR,D0 */
        oril    #2,%d0
        .word   0x4e7b,0x0808          /* movec D0,PCR */
        .word   0xf200,0x0022          /* fadd FP0,FP0 */
        andil   #0xfffffffd,%d0
        .word   0x4e7b,0x0808          /* movec D0,PCR */
        cmpil   #7,linef_count
        bne     fail09

        /* FSGLDIV/FSGLMUL are native; immediate .X is vector 60. */
        .word   0xf200,0x0024          /* fsgldiv FP0,FP0 */
        .word   0xf200,0x0027          /* fsglmul FP0,FP0 */
        cmpil   #7,linef_count
        bne     fail18
        movel   #16,skip_bytes
        .word   0xf23c,0x4822          /* fadd.x #immediate,FP0 */
        .long   0,0,0
        cmpil   #1,unimp_ea_count
        bne     fail19

        /* Native arithmetic updates FPSR condition codes from its result. */
        .word   0xf23c,0x4000          /* fmove.l #1,FP0 */
        .long   1
        .word   0xf200,0x0022          /* fadd FP0,FP0 */
        .word   0xf200,0xa800          /* fmove.l FPSR,D0 */
        andil   #0x0f000000,%d0
        bne     fail21

        .word   0xf23c,0x4000          /* fmove.l #-1,FP0 */
        .long   -1
        .word   0xf200,0x0023          /* fmul FP0,FP0 */
        .word   0xf200,0x0028          /* fsub FP0,FP0 */
        .word   0xf200,0xa800          /* fmove.l FPSR,D0 */
        andil   #0x0f000000,%d0
        cmpil   #0x04000000,%d0        /* Z */
        bne     fail22

        .word   0xf23c,0x4000          /* fmove.l #-1,FP0 */
        .long   -1
        .word   0xf200,0x0004          /* fsqrt FP0,FP0 */
        .word   0xf200,0xa800          /* fmove.l FPSR,D0 */
        andil   #0x0f000000,%d0
        cmpil   #0x01000000,%d0        /* A (NaN) */
        bne     fail23

        .word   0xf23c,0x4000          /* fmove.l #-2,FP0 */
        .long   -2
        .word   0xf200,0x0018          /* fabs FP0,FP0 */
        .word   0xf200,0x001a          /* fneg FP0,FP0 */
        .word   0xf200,0xa800          /* fmove.l FPSR,D0 */
        andil   #0x0f000000,%d0
        cmpil   #0x08000000,%d0        /* N */
        bne     fail24

        .word   0xf23c,0x4000          /* fmove.l #-2,FP0 */
        .long   -2
        .word   0xf201,0x6400          /* fmove.s FP0,D1 */
        .word   0xf200,0xa800          /* fmove.l FPSR,D0 */
        andil   #0x0f000000,%d0
        cmpil   #0x08000000,%d0        /* N */
        bne     fail25

        movel   #0x3f800000,%d1
        .word   0xf201,0x4400          /* fmove.s D1,FP0 */
        .word   0xf200,0xa800          /* fmove.l FPSR,D0 */
        andil   #0x0f000000,%d0
        bne     fail26

        /* MMUSR does not exist on the 68060. */
        movel   #4,skip_bytes
        .word   0x4e7a,0x0805          /* movec MMUSR,D0 */
        cmpil   #1,illegal_count
        bne     fail10

        /* 68060 FSAVE is always a three-longword state frame. */
        lea     fpframe,%a0
        .word   0xf350                 /* frestore NULL frame */
        .word   0xf310                 /* fsave (A0) */
        tstl    fpframe
        bne     fail11
        tstl    fpframe+4
        bne     fail12
        tstl    fpframe+8
        bne     fail13
        .word   0xf200,0x0000          /* fmove FP0,FP0 -> IDLE */
        .word   0xf310                 /* fsave (A0) */
        cmpil   #0x00006000,fpframe
        bne     fail14
        .word   0xf350                 /* frestore (A0) */

        /* Lazy logical-shift flags must agree for branches and SR reads. */
        movel   #0x40000000,%d0
        lsll    #2,%d0
        bcc     fail15
        bne     fail15
        bvs     fail15
        bmi     fail15
        movew   %sr,%d1
        andiw   #0x001f,%d1
        cmpiw   #0x0015,%d1            /* X, Z and C */
        bne     fail15

        movel   #0x80000000,%d0
        lsrl    #1,%d0
        bcs     fail16
        beq     fail16
        bvs     fail16
        bmi     fail16
        movew   %sr,%d1
        andiw   #0x001f,%d1
        bne     fail16

        moveq   #0x40,%d0
        lslb    #1,%d0
        bpl     fail17
        bcs     fail17
        beq     fail17
        bvs     fail17
        movew   %sr,%d1
        andiw   #0x001f,%d1
        cmpiw   #0x0008,%d1            /* N */
        bne     fail17

        lea     pass_msg,%a0
        bsr     puts
        bra     halt

unimp_int_handler:
        addql   #1,unimp_count
        movel   skip_bytes,%d0
        addl    %d0,2(%sp)
        rte

linef_handler:
        addql   #1,linef_count
        movew   6(%sp),%d0
        andiw   #0xf000,%d0
        bne     1f
        movel   skip_bytes,%d0
        addl    %d0,2(%sp)
1:      rte

illegal_handler:
        addql   #1,illegal_count
        movel   skip_bytes,%d0
        addl    %d0,2(%sp)
        rte

unimp_ea_handler:
        addql   #1,unimp_ea_count
        movel   skip_bytes,%d0
        addl    %d0,2(%sp)
        rte

fail_handler:
        bra     fail

fail01: lea f01_msg,%a6; bra fail
fail02: lea f02_msg,%a6; bra fail
fail03: lea f03_msg,%a6; bra fail
fail04: lea f04_msg,%a6; bra fail
fail05: lea f05_msg,%a6; bra fail
fail06: lea f06_msg,%a6; bra fail
fail07: lea f07_msg,%a6; bra fail
fail08: lea f08_msg,%a6; bra fail
fail09: lea f09_msg,%a6; bra fail
fail10: lea f10_msg,%a6; bra fail
fail11: lea f11_msg,%a6; bra fail
fail12: lea f12_msg,%a6; bra fail
fail13: lea f13_msg,%a6; bra fail
fail14: lea f14_msg,%a6; bra fail
fail15: lea f15_msg,%a6; bra fail
fail16: lea f16_msg,%a6; bra fail
fail17: lea f17_msg,%a6; bra fail
fail18: lea f18_msg,%a6; bra fail
fail19: lea f19_msg,%a6; bra fail
fail20: lea f20_msg,%a6; bra fail
fail21: lea f21_msg,%a6; bra fail
fail22: lea f22_msg,%a6; bra fail
fail23: lea f23_msg,%a6; bra fail
fail24: lea f24_msg,%a6; bra fail
fail25: lea f25_msg,%a6; bra fail
fail26: lea f26_msg,%a6; bra fail

fail:
        moveal  %a6,%a0
        bsr     puts
halt:
        movel   #2,0xff009004
1:      stop    #0x2700
        bra     1b

puts:
        moveb   (%a0)+,%d0
        beq     2f
        movel   %d0,0xff008000
        bra     puts
2:      rts

        .data
        .balign 16
source16:
        .long   0x11223344,0x55667788,0x99aabbcc,0xddeeff00
        .balign 16
dest16:
        .space  16
fpframe:
        .space  12
skip_bytes:
        .long   0
unimp_count:
        .long   0
linef_count:
        .long   0
illegal_count:
        .long   0
unimp_ea_count:
        .long   0
pass_msg:
        .asciz  "M68060 TEST PASS\n"
fail_msg:
        .asciz  "M68060 TEST FAIL\n"
f01_msg: .asciz "FAIL 01 PCR\n"
f02_msg: .asciz "FAIL 02 BUSCR\n"
f03_msg: .asciz "FAIL 03 DTT0\n"
f04_msg: .asciz "FAIL 04 MOVE16 A0\n"
f05_msg: .asciz "FAIL 05 MOVE16 A1\n"
f06_msg: .asciz "FAIL 06 MOVE16 DATA\n"
f07_msg: .asciz "FAIL 07 PLPA\n"
f08_msg: .asciz "FAIL 08 VECTOR61\n"
f09_msg: .asciz "FAIL 09 LINEF\n"
f10_msg: .asciz "FAIL 10 ILLEGAL\n"
f11_msg: .asciz "FAIL 11 FSAVE NULL0\n"
f12_msg: .asciz "FAIL 12 FSAVE NULL1\n"
f13_msg: .asciz "FAIL 13 FSAVE NULL2\n"
f14_msg: .asciz "FAIL 14 FSAVE IDLE\n"
f15_msg: .asciz "FAIL 15 LSL.L FLAGS\n"
f16_msg: .asciz "FAIL 16 LSR.L FLAGS\n"
f17_msg: .asciz "FAIL 17 LSL.B FLAGS\n"
f18_msg: .asciz "FAIL 18 FSGL NATIVE\n"
f19_msg: .asciz "FAIL 19 VECTOR60 PC\n"
f20_msg: .asciz "FAIL 20 BUSCR SHADOW\n"
f21_msg: .asciz "FAIL 21 FADD FPSR\n"
f22_msg: .asciz "FAIL 22 FMUL/FSUB FPSR\n"
f23_msg: .asciz "FAIL 23 FSQRT FPSR\n"
f24_msg: .asciz "FAIL 24 FABS/FNEG FPSR\n"
f25_msg: .asciz "FAIL 25 FMOVE OUT FPSR\n"
f26_msg: .asciz "FAIL 26 FMOVE IN FPSR\n"
