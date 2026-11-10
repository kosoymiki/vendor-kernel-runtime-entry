obj-m += ktramp.o
ktramp-y := ktramp_core.o ktramp_cfi.o ktramp_cfi_jt.o
ccflags-y += -DMODULE
ccflags-y += -fno-pic -mcmodel=small
ccflags-y += -fno-stack-protector
