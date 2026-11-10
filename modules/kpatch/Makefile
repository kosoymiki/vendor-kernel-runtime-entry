obj-m += kpatch.o
kpatch-y := kpatch_core.o kpatch_cfi.o kpatch_cfi_jt.o
ccflags-y += -DMODULE
ccflags-y += -fno-pic -mcmodel=small
ccflags-y += -fno-stack-protector
