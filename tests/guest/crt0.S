@ crt0.S - AIF header + task start-up for guest test programs.
@ Start-up follows the Portfolio convention measured from the SDK's cstartup:
@ r5 = argc, r6 = argv, r7 = KernelBase; call vector -30, then main, then -37.
    .arm
    .section .aifhdr, "ax"
    .global _aif
_aif:
    mov     r0, r0                  @ 0x00 no decompression
    mov     r0, r0                  @ 0x04 no self-relocation
    mov     r0, r0                  @ 0x08 no zero-init code (memory starts zeroed)
    bl      _start                  @ 0x0C entry
    swi     0xFFFFFF                @ 0x10 program exit instruction
    .word   __ro_size               @ 0x14 read-only size (includes header)
    .word   0                       @ 0x18 read-write size
    .word   0                       @ 0x1C debug size
    .word   __zi_size               @ 0x20 zero-init size
    .word   0                       @ 0x24 debug type
    .word   __image_base            @ 0x28 image base
    .word   0                       @ 0x2C workspace
    .word   32                      @ 0x30 address mode
    .word   0, 0, 0                 @ 0x34 data base, reserved
    .word   0                       @ 0x40 debug init
    .space  0x80 - 0x44             @ 0x44 zero-init code area (unused)

    .text
    .global _start
_start:
    ldr     r2, =KernelBase
    str     r7, [r2]
    mov     r0, r5
    mov     r1, r6
    mov     r9, r7
    mov     lr, pc
    ldr     pc, [r9, #-120]         @ vector -30: start-up init (preserves r0/r1)
    bl      main
    ldr     r9, =KernelBase
    ldr     r9, [r9]
    ldr     pc, [r9, #-148]         @ vector -37: exit(r0)

    .bss
    .global KernelBase
    .align 2
KernelBase:
    .space 4
