# 内核态读 IL2CPP 游戏进程（真机实测）

> 环境：OnePlus PJZ110 / Android 16 / kernel 6.6.150-android15 GKI / SukiSU（`ksud kpm` loader 3328）。
> 目标：`com.tencent.tmgp.sgame`（`libil2cpp.so` 227,661,664 字节，启用 ARM MTE）。
> 全部结论都在该真机上验证过；验证用的工具是
> [KPM-TEST](https://github.com/CM6558/KPM-TEST) 的 `hero-reader` 模块（ctl0 有 `walk` /
> `findclass` / `dump` / `vma` 等只读诊断命令）。

## 1. 基址：两处 `file_off=0` 的陷阱

目标 maps 里 libil2cpp 出现**两次** `file_off=0`：

```
0x6d28745000-0x6d36063000  r--p  file_off=0        ← 整文件只读映射（不是基址）
0x6e90035000-0x6e93ab9000  r--p  file_off=0        ← 真正的装载基址
0x6e93ab9000-0x6e9632c000  r-xp  file_off=0x3a83000 ← 与上面地址连续
```

判据（不要用"第一个 file_off=0"）：

```
base = r-xp 映射起点 − .text 的 vaddr     # 本版本 .text vaddr = 0x3a84000
```

选错那处时 `base + <静态偏移>` 落在 load 段后面的空隙里，读到 0/垃圾 —— 症状是
"偏移看起来不对"，实际是基址错了。**拿 ELF 程序头（vaddr/memsz）对照，别拿文件大小
对照**：文件 217MB 而虚拟镜像 240MB，BSS 尾部在文件里根本没有。

## 2. ARM MTE：指针先去标记

该游戏进程的指针带高位标记（观察值 `0xb4…`）。直接当地址用会读失败。统一：

```c
#define UNTAG(p) ((unsigned long)(p) & 0x00FFFFFFFFFFFFFFUL)
```

症状：槽里的值明显是堆指针但读不到；高字节不是 `00`/`ff`。

## 3. 内核读 `/proc/<pid>/maps` 会被 SELinux 拦

kthread（`u:r:kernel:s0`）`filp_open("/proc/<pid>/maps")` 返回 `-ENOENT`。所以基址与
候选区收集都要走**纯内核 VMA 遍历**：

```c
addr = 0;
while ((vma = find_vma(mm, addr))) {
    /* 读 vm_start/vm_end/vm_flags/vm_pgoff (偏移见下) */
    if (vm_end <= vm_start || vm_start < addr) break;  /* 防死循环 */
    addr = vm_end;
}
```

`vm_area_struct` 偏移是 `__randomize_layout` 打乱的、KP 也不导出，需要一次性探针：
打印 `find_vma(mm, 已知地址)` 返回结构的前 0x98 字节，按已知值认字段。本机观测：

| off | 字段 | 识别方法 |
|-----|------|----------|
| 0x00 | vm_start | 等于探测地址所在映射的起点 |
| 0x08 | vm_end | 等于该映射的终点 |
| 0x10 | vm_mm | 等于传入的 mm |
| 0x20 | vm_flags | r-xp=0x…75（无 WRITE）、rw-p=0x…73（有 WRITE） |
| 0x78 | vm_pgoff | 等于 maps 的 file offset >> 12 |

VM 位：`READ 0x1 / WRITE 0x2 / EXEC 0x4`；"匿名 rw 堆" = READ|WRITE 且无 EXEC。

## 4. 结构偏移：从导出访问器反汇编出来（全部真机验证）

每个 `il2cpp_*` 访问器就是一两条指令，反汇编即得偏移：

| 结构 | 字段 | 偏移 | 来源访问器 |
|------|------|------|-----------|
| Il2CppDomain | assemblies begin/end | `+0x18` / `+0x20` | `il2cpp_domain_get_assemblies`（helper `adrp #0xe344000 + add #0xeb8`） |
| Il2CppAssembly | image | `+0x00` | `il2cpp_assembly_get_image`（`ldr x0,[x0]`） |
| Il2CppImage | name | `+0x00` | `il2cpp_image_get_name`（`ldr x0,[x0]`） |
| Il2CppImage | class_count | `+0x18` | `il2cpp_image_get_class_count`（`ldr w0,[x0,#0x18]`） |
| Il2CppClass | name | `+0x10` | `il2cpp_class_get_name`（`ldr x0,[x0,#0x10]`） |
| Il2CppClass | namespace | `+0x18` | `il2cpp_class_get_namespace` |
| Il2CppClass | static_field_data | `+0xB8` | `il2cpp_class_get_static_field_data`（`ldr x0,[x0,#0xb8]`） |
| — | s_domain 结构体 | `base + 0xe344ea0` | 由同一 helper 的 ADRP+ADD 推出 |

真机验证：`walk`（domain→assemblies→image）打印 **115 个 assembly 名字全对**
（`Scripts.GameCore.dll`、`mscorlib.dll`…）。这一条能一次证明：基址算法 ✓、去标记 ✓、
链上每个偏移 ✓。**walk 是整条链最便宜的自检。**

注意：**不是所有访问器都能静态读**。`il2cpp_image_get_class(image, i)` 尾调的目标在
文件里是跳转表字节（运行时才成形），`image→classes` 数组偏移静态拿不到 —— 需要时从
IDA 拿，或改用下面的按名定位。

## 5. 按类名定位 Il2CppClass（`findclass`）

不知道 `image→classes` 偏移时，扫目标进程匿名 rw 区找类对象。三条判据同时成立，
假阳性极低：

1. `*(V+0x10)` 指向合法 C 字符串（类名）；
2. `*(V+0x00)` 指向一个 Il2CppImage —— 即 `*(*(V+0x00)+0x00)` 是以 `.dll` 结尾的字符串
   （这条能杀掉"0x20 步长记录数组"这类含名字指针的假阳性）；
3. 类名匹配关键词（精确或子串）。

命中后 `*(V+0xB8)` 就是该类的静态字段。真机用它找回了不在任何符号表里的真实类：

```
ActorManager        Scripts.GameCore.dll   Assets.Scripts.GameLogic   static_fields=0x6d372f6200
ActorDataCenter     Scripts.Base.dll       Assets.Scripts.GameLogic.DataCenter
CLogicActorInfo     Scripts.Base.dll       Assets.Scripts.GameLogic
CKdaDataManager     Scripts.GameCore.dll   Assets.Scripts.GameLogic
```

只判名字（不加判据 2）也会"命中"，但那是别的记录数组 —— **判据 2 不能省**。

## 6. 静态分析口径（省掉重蹈覆辙）

- **文件字节 == 内存字节**才可信：先 dump 设备上某函数的运行时字节与文件比对，
  一致才用文件做地址推导（本 .so `.text` 未加密，成立）。
- **静态地址 → 运行时地址**：`runtime = base + static`。ADRP+ADD 给的是链接期 vaddr，
  别直接当设备地址、也别二次加 base。十六进制多步运算写脚本，别心算（本会话因此
  错过两次）。
- **字节序坑**：反汇编打印的 `d65f03c0` 是**字**的十六进制，不是字节流。
  `bytes.fromhex("d65f03c0")` 喂 capstone 得到空 —— 先转小端字节。
- NDK 里没有 `llvm-mc`，`llvm-objdump -D -b binary` 也不支持；可靠路线是
  `uv pip install --system capstone` 自己解码（只解 ADRP/ADD/LDR/BL/RET 就够跟着
  访问器找到静态）。

## 7. 排障速查（本会话真实踩过的）

| 症状 | 根因 | 解法 |
|------|------|------|
| 加载成功但 init 打印 `FATAL: task_struct_offset 未填充 (pid=-1)` | KP 只填 `comm_offset`，`pid_offset` 恒 -1 | 取 pid 用真实符号 `__task_pid_nr_ns(task, 0, NULL)`，别用偏移 |
| `filp_open("/proc/<pid>/maps")` 返回 -2 | kthread 内核域读 app 的 /proc 被 SELinux 拒 | 纯内核 VMA 遍历 |
| 读到的"指针"读不出任何东西 | ARM MTE 高位标记 | `& 0x00FFFFFFFFFFFFFF` |
| `base+偏移` 读到 0/垃圾 | 用了第一个 file_off=0 映射 | `base = r-xp起点 − .text vaddr` |
| 特征扫描"命中"但坐标是垃圾/永不变 | 匹配上了 Java/Dalvik 区的巧合数据 | 放弃特征扫描，走指针链 |
| capstone 反汇编返回空列表 | 把指令字的 hex 当字节流喂了 | 先转小端字节再喂 |

## 8. 相关仓库

- [KPM-TEST](https://github.com/CM6558/KPM-TEST) — 实战模块 `hero-reader`
  （含 `docs/POINTER_CHAIN.md` 已验证偏移、`docs/IDA_TODO.md` 待 IDA 分析清单）。
