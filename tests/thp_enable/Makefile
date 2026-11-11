obj-m += thp_enable.o
thp_enable-y := thp_enable_core.o thp_enable_cfi.o thp_enable_cfi_jt.o
ccflags-y += -DMODULE
ccflags-y += -fno-pic -mcmodel=small
ccflags-y += -fno-stack-protector
