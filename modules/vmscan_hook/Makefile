obj-m += vmscan_hook.o
vmscan_hook-y := vmscan_hook_core.o vmscan_hook_cfi.o vmscan_hook_cfi_jt.o
ccflags-y += -DMODULE
ccflags-y += -fno-pic -mcmodel=small
ccflags-y += -fno-stack-protector
