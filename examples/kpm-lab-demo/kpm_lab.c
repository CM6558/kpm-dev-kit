/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kpm-lab-demo — KernelPatch Module 实验室示例
 *
 * 目的: 用一个模块把 KPM 开发的核心 API 面全部跑一遍
 *   1. 元信息宏 KPM_NAME/VERSION/LICENSE/AUTHOR/DESCRIPTION
 *   2. 生命周期 KPM_INIT / KPM_CTL0 / KPM_EXIT (args / event 传递)
 *   3. kallsyms_lookup_name: 查函数地址 + 查全局变量地址并读取
 *   4. ctl0 通过 compat_copy_to_user 向用户态回传数据
 *   5. inline hook: hook_wrap / hook_unwrap 链式 hook, before/after,
 *      fargs->local 传上下文, after 覆盖返回值
 *
 * 只 hook 模块内部函数 (lab_add), 不触碰任何内核行为, 可安全加载/卸载。
 */

#include <compiler.h>
#include <kpmodule.h>
#include <hook.h>
#include <kallsyms.h>
#include <common.h>
#include <kputils.h>
#include <linux/printk.h>
#include <linux/kernel.h>   /* snprintf -> kfunc(snprintf) */
#include <linux/string.h>

KPM_NAME("kpm-lab-demo");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("hermes");
KPM_DESCRIPTION("KPM lab: lifecycle + kallsyms + self inline-hook");

/* ---- 通过 kallsyms_lookup_name 解析的内核符号 ---- */
static long long (*fn_boottime)(void);               /* ktime_get_boottime -> ktime_t(ns) */
static long long (*fn_seconds)(void);                /* ktime_get_seconds  -> s64 */
static unsigned long *var_jiffies;                   /* jiffies */
static int *var_nr_threads;                          /* nr_threads */

/* ---- 自 hook 演示状态 ---- */
static volatile int lab_hook_calls;
static volatile int lab_sink;   /* 副作用: 阻止 -O2 把 lab_add 调用常量折叠掉 */
static int lab_hook_installed;

int __noinline lab_add(int a, int b)
{
    lab_sink = a;
    return a + b;
}

static void before_lab_add(hook_fargs2_t *args, void *udata)
{
    lab_hook_calls++;
    args->local.data0 = args->arg1; /* before -> after 传上下文 */
    pr_info("[kpm-lab] before lab_add(%d, %d)\n", (int)args->arg0, (int)args->arg1);
}

static void after_lab_add(hook_fargs2_t *args, void *udata)
{
    pr_info("[kpm-lab] after lab_add ret=%d (arg1=%d saved via local)\n",
            (int)args->ret, (int)args->local.data0);
}

static int install_self_hook(void)
{
    hook_err_t err;
    if (lab_hook_installed)
        return 0;
    err = hook_wrap2((void *)lab_add, before_lab_add, after_lab_add, NULL);
    pr_info("[kpm-lab] hook_wrap2(lab_add) -> %d\n", err);
    if (err == HOOK_NO_ERR)
        lab_hook_installed = 1;
    return lab_hook_installed;
}

static void remove_self_hook(void)
{
    if (lab_hook_installed) {
        hook_unwrap((void *)lab_add, before_lab_add, after_lab_add);
        lab_hook_installed = 0;
        pr_info("[kpm-lab] self hook removed\n");
    }
}

static long lab_init(const char *args, const char *event, void *reserved)
{
    int r;

    pr_info("[kpm-lab] init: event=%s args=%s\n", event ? event : "(null)",
            args ? args : "(null)");
    pr_info("[kpm-lab] kpver=%x kver=%x\n", kpver, kver);

    /* 查函数 */
    fn_boottime = (void *)kallsyms_lookup_name("ktime_get_boottime");
    fn_seconds = (void *)kallsyms_lookup_name("ktime_get_seconds");
    /* 查全局变量 */
    var_jiffies = (void *)kallsyms_lookup_name("jiffies");
    var_nr_threads = (void *)kallsyms_lookup_name("nr_threads");

    pr_info("[kpm-lab] syms: boottime=%llx seconds=%llx jiffies=%llx nr_threads=%llx\n",
            (unsigned long long)fn_boottime, (unsigned long long)fn_seconds,
            (unsigned long long)var_jiffies, (unsigned long long)var_nr_threads);

    if (var_jiffies)
        pr_info("[kpm-lab] jiffies=%lu\n", *var_jiffies);
    if (var_nr_threads)
        pr_info("[kpm-lab] nr_threads=%d\n", *var_nr_threads);
    if (fn_boottime)
        pr_info("[kpm-lab] boottime=%lld s\n", fn_boottime() / 1000000000LL);
    else if (fn_seconds)
        pr_info("[kpm-lab] seconds=%lld\n", fn_seconds());

    /* 未 hook 时调用一次 */
    r = lab_add(20, 10);
    pr_info("[kpm-lab] lab_add(20,10) before hook = %d\n", r);

    install_self_hook();

    r = lab_add(20, 10);
    pr_info("[kpm-lab] lab_add(20,10) after hook  = %d (hook calls=%d)\n",
            r, lab_hook_calls);

    return 0;
}

static long lab_ctl0(const char *args, char *__user out_msg, int outlen)
{
    char buf[256];
    int n = 0;
    long rc = 0;

    pr_info("[kpm-lab] ctl0: %s\n", args ? args : "(null)");

    if (!args || !strcmp(args, "info")) {
        long long uptime = 0;
        if (fn_boottime)
            uptime = fn_boottime() / 1000000000LL;
        else if (fn_seconds)
            uptime = fn_seconds();
        n = snprintf(buf, sizeof(buf),
                     "kpm-lab-demo v1.0.0 kpver=%x kver=%x uptime=%llds "
                     "jiffies=%lu threads=%d hook=%d calls=%d",
                     kpver, kver, uptime,
                     var_jiffies ? *var_jiffies : 0UL,
                     var_nr_threads ? *var_nr_threads : -1,
                     lab_hook_installed, lab_hook_calls);
        if (n < 0)
            n = 0;
        pr_info("[kpm-lab] info: %s\n", buf);
    } else if (!strncmp(args, "add ", 4)) {
        /* add A B — 经过被 hook 的 lab_add, 结果用返回值带回用户态 */
        int a = 0, b = 0;
        const char *p = args + 4;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
        rc = lab_add(a, b);
        n = snprintf(buf, sizeof(buf), "add(%d, %d) = %ld (hook calls=%d)",
                     a, b, rc, lab_hook_calls);
        if (n < 0)
            n = 0;
        pr_info("[kpm-lab] %s\n", buf);
    } else {
        n = snprintf(buf, sizeof(buf), "echo: %s", args);
        if (n < 0)
            n = 0;
        pr_info("[kpm-lab] %s\n", buf);
    }

    /* 两条用户态通路:
     *  - APatch/KernelPatch 原生: out_msg/outlen 有效, 拷贝字符串回去
     *  - SukiSU (ksud kpm control): out_msg=NULL, outlen=0, 只能靠返回值(rc)
     *    负数 rc 会被 ksud 当成 errno 打印 */
    if (out_msg && outlen > 0) {
        int len = (int)strlen(buf);
        if (len > outlen - 1)
            len = outlen - 1;
        if (len > 0)
            compat_copy_to_user(out_msg, buf, len + 1);
    }
    return rc;
}

static long lab_exit(void *reserved)
{
    remove_self_hook();
    pr_info("[kpm-lab] exit, hook calls total=%d\n", lab_hook_calls);
    return 0;
}

KPM_INIT(lab_init);
KPM_CTL0(lab_ctl0);
KPM_EXIT(lab_exit);
