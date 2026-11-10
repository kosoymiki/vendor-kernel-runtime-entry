#include <linux/kernel.h>
#include <linux/module.h>

void __ubsan_handle_cfi_check_fail_abort(void *data, void *ptr)
{
	panic("mglru_probe: CFI check failed (data=%pS target=%pS)\n", data, ptr);
}
