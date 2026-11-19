/*
 * startup.S - Minimal startup code for the CH32V203 (QingKe V4B)
 *
 * - Address 0 holds a jump instruction. On reset the CPU starts executing at 0x00000000.
 * - The vector table is in WCH's "absolute address" format: each entry is the handler's address (mtvec mode=3).
 * - INTSYSCR (CSR 0x804) is set to 0, which turns off HPE (hardware register save) and interrupt nesting.
 *   With that, the standard GCC __attribute__((interrupt)) is enough to write handlers.
 *   (See tutorial chapter 01 / 09.)
 */

    .section .init, "ax", @progbits
    .globl _start
    .align 2
_start:
    .option push
    .option norvc
    j       reset_handler
    .option pop

/* ------------------------------------------------------------------ */
/* Vector table                                                        */
/* ------------------------------------------------------------------ */
    .section .vector, "ax", @progbits
    .align 2
    .globl _vector_base
_vector_base:
    .option push
    .option norvc
    .word   _start                      /*  0: reset                    */
    .word   0                           /*  1                           */
    .word   NMI_Handler                 /*  2: NMI                      */
    .word   HardFault_Handler           /*  3: HardFault                */
    .word   0                           /*  4                           */
    .word   Default_Handler             /*  5: Ecall M                  */
    .word   0                           /*  6                           */
    .word   0                           /*  7                           */
    .word   Default_Handler             /*  8: Ecall U                  */
    .word   Default_Handler             /*  9: Breakpoint               */
    .word   0                           /* 10                           */
    .word   0                           /* 11                           */
    .word   SysTick_Handler             /* 12: SysTick                  */
    .word   0                           /* 13                           */
    .word   Default_Handler             /* 14: SW                       */
    .word   0                           /* 15                           */
    .rept   19                          /* 16..34: WWDG .. ADC          */
    .word   Default_Handler
    .endr
    .word   USB_HP_CAN1_TX_IRQHandler   /* 35: USB HP / CAN1 TX         */
    .word   USB_LP_CAN1_RX0_IRQHandler  /* 36: USB LP / CAN1 RX0        */
    .rept   21                          /* 37..57                       */
    .word   Default_Handler
    .endr
    .word   USBWakeUp_IRQHandler        /* 58: USB wakeup               */
    .rept   4                           /* 59..62: USBFS etc. (unused)  */
    .word   Default_Handler
    .endr
    .option pop

/* Handlers that are not defined fall back to Default_Handler (weak symbols) */
    .weak   NMI_Handler
    .weak   HardFault_Handler
    .weak   SysTick_Handler
    .weak   USB_HP_CAN1_TX_IRQHandler
    .weak   USB_LP_CAN1_RX0_IRQHandler
    .weak   USBWakeUp_IRQHandler
    .set    NMI_Handler,                Default_Handler
    .set    HardFault_Handler,          Default_Handler
    .set    SysTick_Handler,            Default_Handler
    .set    USB_HP_CAN1_TX_IRQHandler,  Default_Handler
    .set    USB_LP_CAN1_RX0_IRQHandler, Default_Handler
    .set    USBWakeUp_IRQHandler,       Default_Handler

/* ------------------------------------------------------------------ */
/* Default_Handler: save mcause/mepc to g_fault for later inspection    */
/* with wlink dump, then stop                                           */
/* ------------------------------------------------------------------ */
    .section .text.Default_Handler, "ax", @progbits
    .align 2
    .globl Default_Handler
Default_Handler:
    la      t0, g_fault
    csrr    t1, mcause
    sw      t1, 0(t0)
    csrr    t1, mepc
    sw      t1, 4(t0)
    csrr    t1, mtval
    sw      t1, 8(t0)
    li      t1, 0xDEADBEEF
    sw      t1, 12(t0)
1:  j       1b

/* ------------------------------------------------------------------ */
/* Reset handler                                                       */
/* ------------------------------------------------------------------ */
    .section .text.reset_handler, "ax", @progbits
    .align 2
reset_handler:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, _estack

    /* Copy .data from flash to RAM */
    la      a0, _data_lma
    la      a1, _data_vma
    la      a2, _edata
    bgeu    a1, a2, 2f
1:  lw      t0, 0(a0)
    sw      t0, 0(a1)
    addi    a0, a0, 4
    addi    a1, a1, 4
    bltu    a1, a2, 1b
2:
    /* Zero .bss */
    la      a0, _sbss
    la      a1, _ebss
    bgeu    a0, a1, 2f
1:  sw      zero, 0(a0)
    addi    a0, a0, 4
    bltu    a0, a1, 1b
2:
    /* corecfgr (0xBC0): pipeline / branch prediction setting (same value as the WCH SDK) */
    li      t0, 0x1f
    csrw    0xbc0, t0
    /* INTSYSCR (0x804) = 0: HPE off, nesting off */
    csrw    0x804, zero
    /* mtvec = vector table | 3 (vectored mode + absolute-address entries) */
    la      t0, _vector_base
    ori     t0, t0, 3
    csrw    mtvec, t0

    call    system_init
    call    main
1:  j       1b
