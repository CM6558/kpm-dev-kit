# KPM 开发工具包 (KernelPatch Module Development Kit)

在 Android root 设备上**开发、构建、加载、调试 KernelPatch Module (.kpm)** 的实战笔记与
可复现示例。全部结论均在真机上实测过：

    Android 16 / kernel 6.6.150-android15 GKI (HZ=250) / SukiSU Ultra
    ksud 3.3.0  CONFIG_SUKISU_KPM=y  KPM loader version 3328
    Termux + clang 21 (无需 NDK)

仓库结构：

    README.md                     ← 本文：完整实战笔记
    docs/agent-skill.md           ← 精炼版检查清单（给 AI/脚本用）
    docs/il2cpp-kernel-read.md     ← 内核态读 IL2CPP 游戏进程（基址/MTE/VMA/偏移/findclass）
    templates/kpm_min.c           ← 最小模块骨架（带注释）
    examples/kpm-lab-demo/        ← 可直接构建运行的示例模块
    scripts/kpm.sh                ← build/push/load/ctl/unload/log 快捷脚本

参考源码（自行 clone）：

    git clone https://github.com/ShirkNeko/SukiSU_KernelPatch_patch   # SukiSU 的 KPM loader
    git clone https://github.com/bmax121/KernelPatch                  # 上游 + doc/zh-CN/*.md
    git clone https://github.com/SukiSU-Ultra/SukiSU-Ultra            # ksud 的 kpm.rs 实现

---

## 1. 构建（不需要 NDK）

模块是**可重定位 ELF（ET_REL）**，不是用户态程序，Termux 自带 clang 直接编译：

    cd examples/kpm-lab-demo
    make KP_DIR=/path/to/SukiSU_KernelPatch_patch     # 产出 kpm_lab.kpm

关键参数（与官方 demo 等价）：

    INCLUDE: $(KP_DIR)/kernel/{.,include,patch/include,linux/include,
             linux/arch/arm64/include,linux/tools/arch/arm64/include}
    CFLAGS:  -O2 -fno-PIC -fno-pie -fno-stack-protector -fno-common
             -fno-asynchronous-unwind-tables -fno-unwind-tables
             -U_FORTIFY_SOURCE -fvisibility=hidden
    编译:    clang $CFLAGS -Txxx.lds -c -o x.o x.c
    链接:    clang -r -Txxx.lds -o x.kpm x.o

`.lds` 只是把 `.plt` / `.init.plt` / `.text.ftrace_trampoline` 设为 NOLOAD。

产物自检：

    readelf -h  → Type: REL, Machine: AArch64
    readelf -SW → 必须含 .kpm.info / .kpm.init / .kpm.exit（.kpm.ctl0/.ctl1 可选）
    readelf -sW → 未定义符号只应出现 KP 导出符号或 kf_*（见 §4）

## 2. 加载 / 控制 / 卸载

    su -c 'cp kpm_lab.kpm /data/local/tmp/'
    su -c '/data/adb/ksud kpm load /data/local/tmp/kpm_lab.kpm <init-args>'
    su -c '/data/adb/ksud kpm list'                            # 已加载模块
    su -c '/data/adb/ksud kpm info kpm-lab-demo'               # name/version/args...
    su -c '/data/adb/ksud kpm control kpm-lab-demo info'       # → 0（返回码）
    su -c '/data/adb/ksud kpm control kpm-lab-demo "add 7 8"'  # → 15（值经返回值回传）
    su -c '/data/adb/ksud kpm unload kpm-lab-demo'
    su -c 'dmesg | grep kpm-lab'                               # 内核日志

实测输出（节选）：

    [kpm-lab] init: event=load-file args=lab-42
    [kpm-lab] syms: boottime=ffffffea80d98710 seconds=ffffffea80d9b954 \
        jiffies=ffffffea82da7e80 nr_threads=ffffffea82fb89b0
    [kpm-lab] hook_wrap2(lab_add) -> 0
    [kpm-lab] before lab_add(20, 10)
    [kpm-lab] after lab_add ret=30 (arg1=10 saved via local)
    [+] KP I module_control0: name kpm-lab-demo, args: add 7 8
    [+] KP I module_control0: name: kpm-lab-demo, rc: 15
    [kpm-lab] self hook removed
    [kpm-lab] exit, hook calls total=2

开机自动加载：把 .kpm 放进 `/data/adb/kpm/`（ksud 启动时 booted_load；safe mode 下不加载）。

## 3. KPM 的 ABI（来自 loader 源码 kernel/patch/module/module.c + 实测）

- ELF 必须 `ET_REL` + `EM_AARCH64`；`.kpm.info`（"name=x\0version=y\0…"）、
  `.kpm.init`、`.kpm.exit` 节必须存在。
- `init(args, event, reserved)`：args=加载参数，event="load-file"；
  返回非 0 视为加载失败（loader 会再调一次 exit 清理）。
- `ctl0(args, out_msg, outlen)`：控制接口，见 §5；`exit(reserved)`：必须解除全部 hook。
- 模块代码段由 loader 用 kp_malloc_exec 分配（RWX、非页对齐）。

## 4. 符号解析：只有两扇门

loader 对模块内未定义符号只做一件事（module.c `simplify_symbols`）：
`symbol_lookup_name(name)` → KP 导出表（`KP_EXPORT_SYMBOL`，kpimg 的 `.kp.symbol` 节），
兜底 `sukisu_compact_find_symbol`。**不会查普通 kallsyms。**

可以直接引用的只有：

1. KP 导出符号（实测）：`printk`、`kallsyms_lookup_name`、`hook`/`unhook`、
   `hook_wrap`/`hook_unwrap_remove`、`fp_hook*`、`hook_syscalln` 系列、`hotpatch`、
   `compat_copy_to_user`、`compat_strncpy_from_user`、`write_kstorage`/`read_kstorage`、
   `commit_su`、`task_su`、`raw_syscall0-6`、`sys_call_table`、`kver`、`kpver` 等。
2. `kf_*` kfunc 包装（由 stub 头引入，实测可解析）：`<linux/string.h>` 提供
   strcmp/strncmp/strlen/strncat/strstr/memcpy…；`<linux/kernel.h>` 提供
   sprintf/snprintf/sscanf…（宏映射到 `kfunc(xx)`）。
   实测 UND 表曾出现 `kf_snprintf / kf_strcmp / kf_strncmp`，全部解析成功。

其它任意内核函数/变量：init 里
`(void *)kallsyms_lookup_name("name")` 解析成指针再调用。

- 返回 0 = 内核里没有该符号，**必须判空**。
- 实测：`ktime_get_boottime_seconds` 在 6.6.150 GKI 中不存在（返回 0）；
  `ktime_get_boottime` / `ktime_get_seconds` / `jiffies` / `nr_threads` 存在。
- 顺带：本机 HZ=250（jiffies 差值 ÷ 250 = dmesg 时间戳）。

## 5. ctl0 的用户态通道（SukiSU 与 APatch 不同！）

调用链（ksud 3.3.0 + SukiSU 内核 + KernelPatch patch）：

    ksud kpm control <name> <args>
      → ioctl(KSU_IOCTL_KPM, {SUKISU_KPM_CONTROL, name, args, &ret})
      → sukisu_handle_kpm()          [kernel/kpm/kpm.c]
      → sukisu_kpm_control()         [SukiSU 内核 stub]
      → 被 KP patch 的 before hook 接管（kernel/patch/sukisu/sukisu.c）:
        module_control0(name, args, out_msg=NULL, outlen=0)
      → mod->ctl0(args, NULL, 0)；返回值原样写回 ret

结论（实测）：

- **out_msg 恒为 NULL、outlen 恒为 0**：SukiSU 上没有字符串响应通道。
  `ctl0` 的 int 返回值是唯一回传：ksud 直接打印；负数按 errno 打印
  （-EINVAL → "Invalid argument (os error 22)"）；空 args 在 kernel 层即被拒。
- 更详细的结果 → 写 dmesg（pr_info）或使用 kstorage。
- 同时兼容 APatch 原生路径（那边 out_msg 有效）的写法：

      if (out_msg && outlen > 0) { ... compat_copy_to_user(out_msg, buf, len + 1); }

## 6. inline hook 实测与坑

- `hook_wrap2(func, before, after, udata)` → 0 (HOOK_NO_ERR)；回调
  `void cb(hook_fargs2_t *args, void *udata)`；参数 `args->argN`；
  `args->ret`（after 中可改）；`args->local.data0-7`（before→after 传值）；
  `args->skip_origin = 1` 跳过原函数。
- 解除：`hook_unwrap(func, before, after)`；多模块同函数用 wrap 链（≤16 项），
  不要用 `hook()`。
- **坑（实测）**：`-O2` 会把自测调用**常量折叠**（`lab_add(20,10)` 直接变 30，
  没有 bl 指令），表现为"hook 装上但从不触发"。给被调函数加不可消除副作用
  （如 volatile 写入）再测。排查：`llvm-objdump -dr x.kpm | grep bl`。

## 7. 最小骨架

见 `templates/kpm_min.c` 或 `examples/kpm-lab-demo/kpm_lab.c`：

    #include <compiler.h> <kpmodule.h> <hook.h> <kallsyms.h> <common.h>
    #include <kputils.h> <linux/printk.h> <linux/kernel.h> <linux/string.h>

    KPM_NAME("..."); KPM_VERSION("1.0.0"); KPM_LICENSE("GPL v2");
    KPM_AUTHOR("..."); KPM_DESCRIPTION("...");   // name 全局唯一

    static long my_init(const char *args, const char *event, void *reserved) { ... }
    static long my_ctl0(const char *args, char *__user out_msg, int outlen)  { ... }
    static long my_exit(void *reserved) { /* unwrap 全部 hook */ }

    KPM_INIT(my_init); KPM_CTL0(my_ctl0); KPM_EXIT(my_exit);

## 8. 排查清单

    dmesg | grep -E '\[tag\]|module_control0|load_module|unload_module|unknown symbol'
    ksud kpm list / info <name> / num / version
    readelf -sW x.kpm | awk '$7=="UND"{print $8}'    # 未定义符号是否都能解析
    llvm-objdump -dr x.kpm                           # 调用/重定位是否真实存在

加载失败时 dmesg 里 KP 会打印 `unknown symbol: xxx` 或具体 -errno 原因。
