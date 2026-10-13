/* Minimal KPM skeleton — build with: make KP_DIR=~/KP-SUKISU
 * (see ~/kpm-lab/README.md for the Makefile and full field notes) */
#include <compiler.h>
#include <kpmodule.h>
#include <hook.h>
#include <kallsyms.h>
#include <common.h>
#include <kputils.h>
#include <linux/printk.h>
#include <linux/kernel.h>   /* snprintf -> kfunc(snprintf) */
#include <linux/string.h>

KPM_NAME("kpm-example");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("me");
KPM_DESCRIPTION("minimal kpm");

static unsigned long (*fn_target)(void);   /* kallsyms-resolved kernel fn */

static void before_cb(hook_fargs1_t *args, void *udata) { /* args->arg0, args->local.* */ }
static void after_cb(hook_fargs1_t *args, void *udata)  { /* args->ret writable */ }

static long my_init(const char *args, const char *event, void *reserved)
{
    hook_err_t err;
    pr_info("[kpm-example] init event=%s args=%s kpver=%x\n",
            event ? event : "", args ? args : "", kpver);

    fn_target = (void *)kallsyms_lookup_name("some_symbol");
    if (!fn_target) pr_warn("[kpm-example] symbol absent - degrade\n");

    err = hook_wrap1((void *)kallsyms_lookup_name("some_func"), before_cb, after_cb, NULL);
    if (err != HOOK_NO_ERR) { pr_err("[kpm-example] hook err %d\n", err); return -1; }
    return 0;
}

/* SukiSU: out_msg==NULL, return value is the only reply (negative -> errno in ksud) */
static long my_ctl0(const char *args, char *__user out_msg, int outlen)
{
    char buf[128];
    int n;
    pr_info("[kpm-example] ctl0: %s\n", args ? args : "(null)");
    n = snprintf(buf, sizeof(buf), "ok: %s", args ? args : "");
    if (out_msg && outlen > 0 && n > 0)  /* APatch native path only */
        compat_copy_to_user(out_msg, buf, n + 1 > outlen ? outlen : n + 1);
    return 0;
}

static long my_exit(void *reserved)
{
    hook_unwrap((void *)kallsyms_lookup_name("some_func"), before_cb, after_cb);
    pr_info("[kpm-example] exit\n");
    return 0;
}

KPM_INIT(my_init);
KPM_CTL0(my_ctl0);
KPM_EXIT(my_exit);
