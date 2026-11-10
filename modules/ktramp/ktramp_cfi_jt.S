/* CFI scaffolding for kbootstrap on the RMX3709 vendor ABI (5.10.236).
 * Same proven pattern as ae_bore/ae_guard: cfi_init() reads these tables and
 * rewrites mod->init/mod->exit; __cfi_check is a module-local no-op so the
 * vendor CFI runtime does not reject our (non-CFI) code. Security tradeoff:
 * CFI is disabled for this module only; documented in docs/ENTRYPOINT.md.
 */
    .section .ref.text,"ax",%progbits
    .balign 4
    .global init_module.cfi_jt
    .type init_module.cfi_jt, %function
init_module.cfi_jt:
    bti c
    b init_module

    .section .ref.text,"ax",%progbits
    .balign 4
    .global cleanup_module.cfi_jt
    .type cleanup_module.cfi_jt, %function
cleanup_module.cfi_jt:
    bti c
    b cleanup_module

    .section .ref.data,"aw",%progbits
    .balign 8
    .global __cfi_jt_init_module
    .type __cfi_jt_init_module, %object
__cfi_jt_init_module:
    .quad init_module.cfi_jt

    .balign 8
    .global __cfi_jt_cleanup_module
    .type __cfi_jt_cleanup_module, %object
__cfi_jt_cleanup_module:
    .quad cleanup_module.cfi_jt

    .text
    .balign 4
    .global __cfi_check
    .type __cfi_check, %function
__cfi_check:
    ret

    .section .note.GNU-stack,"",%progbits
