---
name: kpm-module-development
description: Use when building/loading KPM kernel modules on Android.
tags: [android, kernel, kpm, kernelpatch, sukisu, termux, hook]
author: curator
version: 1.0
---

# KPM (KernelPatch Module) development on-device

**Trigger:** user wants to write/build/load/debug a `.kpm` kernel module on an Android
root device (SukiSU Ultra `ksud kpm ...`, APatch/KernelPatch). Also load when a KPM fails
to compile, fails to load ("unknown symbol"), or its hook never fires.

## Environment on this device (verified)

- Android 16, kernel 6.6.150-android15 GKI (HZ=250), SukiSU Ultra (`CONFIG_SUKISU_KPM=y`),
  `ksud kpm load/unload/list/info/control/num/version` (loader version 3328).
- Loader source = `~/KP-SUKISU` (ShirkNeko/SukiSU_KernelPatch_patch) — build against its headers.
  Upstream docs: `~/KP-UPSTREAM/doc/zh-CN/*.md`. ksud side: `~/suki-src/userspace/ksud/src/kpm.rs`
  + `~/suki-src/kernel/kpm/kpm.c`. Working lab + write-up: `~/kpm-lab/README.md`,
  demo module `~/kpm-lab/kpm-lab-demo/`.

## Build (no NDK needed)

Termux clang (21.x) works directly — the module is a relocatable ELF, not a userspace binary:

```make
make KP_DIR=~/KP-SUKISU   # clang -I{kernel/.,kernel/include,kernel/patch/include,
                          #   kernel/linux/include,kernel/linux/arch/arm64/include,
                          #   kernel/linux/tools/arch/arm64/include}
                          # -O2 -fno-PIC -fno-pie -fno-stack-protector -fno-common
                          # -fno-asynchronous-unwind-tables -fno-unwind-tables
                          # -U_FORTIFY_SOURCE -fvisibility=hidden
# compile: clang $CFLAGS -Txxx.lds -c -o x.o x.c
# link:    clang -r -Txxx.lds -o x.kpm x.o
```

The `.lds` only NOLOADs `.plt/.init.plt/.text.ftrace_trampoline`. Self-check: `readelf -h`
(ET_REL, AArch64) + `readelf -SW` must show `.kpm.info/.kpm.init/.kpm.exit`.

## ABI rules (from kernel/patch/module/module.c)

- `.kpm.info` holds `name=x\0version=y\0...`; name is unique and used by unload/control.
- `init(args, event, reserved)` — `event="load-file"` on file load; non-zero return fails load
  (loader then calls exit for cleanup). `exit()` must unwrap every hook.
- **Undefined symbols are resolved ONLY against KP's export table** (`KP_EXPORT_SYMBOL` in
  `.kp.symbol` of kpimg, `symbol_lookup_name()`, fallback `sukisu_compact_find_symbol`).
  kallsyms is NOT consulted. So direct calls are limited to KP exports (`printk`,
  `kallsyms_lookup_name`, `hook_wrap`/`hook_unwrap_remove`, `fp_hook*`, `hook_syscalln`,
  `compat_copy_to_user`, `kstorage` API, `hotpatch`, `raw_syscall0-6`, `sys_call_table`,
  `kver`, `kpver`, ...) and to the `kf_*` kfunc wrappers pulled in by the stub headers
  (`<linux/string.h>`, `<linux/kernel.h>` → snprintf etc.). Anything else → resolve at
  runtime with `kallsyms_lookup_name("fn")` and call through a pointer.
- `kallsyms_lookup_name` returns 0 when absent (e.g. `ktime_get_boottime_seconds` is absent
  on 6.6.150 GKI; `ktime_get_boottime`, `ktime_get_seconds`, `jiffies`, `nr_threads` exist).
  Always null-check; never assume a symbol exists.

## ctl0 has NO string channel on SukiSU

Chain: `ksud kpm control <name> <args>` → ioctl → `sukisu_handle_kpm` → `sukisu_kpm_control`
stub → KP patch hook → `module_control0(name, args, out_msg=NULL, outlen=0)` → `ctl0(args, NULL, 0)`.
So on SukiSU: **return int is the only reply channel** (ksud prints it; negative values are
printed as errno strings; empty args are rejected with -EINVAL before ctl0 runs). Use dmesg
(`pr_info`) or kstorage for details. On APatch the native supercall passes a real buffer —
guard the copy: `if (out_msg && outlen > 0) compat_copy_to_user(...)`.

## Inline hook

- `hook_wrap2(func, before, after, udata)` → `HOOK_NO_ERR==0`; callback
  `void cb(hook_fargs2_t*, void*)`; `args->argN`, `args->ret` (writable in after),
  `args->local.data0..7` (before→after), `args->skip_origin=1` to skip the original.
- `hook_unwrap(func, before, after)` in exit. Use wrap/unwrap (chain, ≤16 items), not `hook()`.
- **Pitfall:** with `-O2` a self-test call to a trivial function gets constant-folded
  (`lab_add(20,10)` → no `bl`, hook never fires even though `hook_wrap` returns 0). Give the
  callee a non-removable side effect (e.g. `volatile` store) before trusting the test.
  If a hook "installs" but never triggers: `llvm-objdump -dr x.kpm | grep bl`.

## Verify / deploy

```sh
su -c '/data/adb/ksud kpm load /data/local/tmp/x.kpm args'   # then: list/info/control
su -c 'dmesg | grep -E "\[x\]|module_control0|load_module|unknown symbol"'
su -c '/data/adb/ksud kpm unload x'                          # confirm exit + hook removal
```

Auto-load at boot: drop `.kpm` files in `/data/adb/kpm/` (ksud booted_load; skipped in safe mode).

## Reporting

Lead with: build command used, `ksud kpm load` result, dmesg evidence lines, and what was
unloaded after the test. Leave the device with `ksud kpm num` back at its starting value.
