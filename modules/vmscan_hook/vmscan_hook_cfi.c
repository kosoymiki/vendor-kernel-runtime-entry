#include <linux/kernel.h>
#include <linux/module.h>
void __ubsan_handle_cfi_check_fail_abort(void*d,void*p){panic("ktramp: CFI fail %pS %pS\n",d,p);}
