# Out-of-tree module build. Pass KB_CFI=1 when the target kernel has
# CONFIG_CFI_CLANG=y; the CFI bridge objects are then linked.
obj-m += kbootstrap.o
kbootstrap-y := kbootstrap_core.o
KB_CFI ?= 1
ifeq ($(KB_CFI),1)
kbootstrap-y += kbootstrap_cfi.o kbootstrap_cfi_jt.o
endif
ccflags-y += -DMODULE
ccflags-y += -fno-pic -mcmodel=small
