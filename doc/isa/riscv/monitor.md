# Chapter 0x04 —— M 态 monitor：中断、异常与特权级

本文记录第四章综合实验的落地方式：在 `test_resources/src/monitor.c` 中实现一个运行于
M 态的小型 monitor，由它启动多个 U 态用户任务，并通过 `ecall`、非法指令、访存 fault 和
CLINT 定时器中断四类 trap 演示完整的特权级切换与调度。

## 1. 运行方式

```bash
cd test_resources && make ARCH=riscv64 monitor
cd .. && script -q /dev/null ./target/debug/here --loglevel error ./test_resources/bin/monitor.elf
```

> 裸机程序把结果打印到 UART，`here` 需要 TTY，所以外层套一层 `script`。
> 跑 ch2/ch3 的 `main.elf` 之前请不要忘了 `cargo build --features custom-instr`，
> 否则 `padd8` 会译码失败。

## 2. 任务与栈布局

`lib/trap.S` 的入口在换栈之后执行 `addi sp, sp, -34*8`，把 `TrapContext`（34 个 u64）
铺在内核栈顶之下。因此每个任务的内核栈必须做成「剩余空间 + TrapContext」的形状，让
`&tf` 正好等于 `内核栈顶 - 272`：

```c
#define KSTACK_SIZE 4096
typedef struct {
    uint8_t kstack[KSTACK_SIZE - sizeof(TrapContext)];
    TrapContext tf;
} Task;
_Static_assert(sizeof(Task) == KSTACK_SIZE, "Task must be exactly one kernel stack");
```

## 3. 三次进入 / 离开 U 态

| 场景 | 做法 |
| --- | --- |
| 首次进入 | `boot_task()`：`csrw mepc/mstatus/mscratch` + `mv sp` + `mret`。此时没有可恢复的现场，不能用 `__traps_return`——它靠 `csrrw sp, mscratch, sp` 交换用户 sp，而首次启动时 mscratch 必须是内核栈顶。 |
| trap 进入 | 硬件跳到 `mtvec`，`lib/trap.S` 用 `csrrw` 换到内核栈、保存 34 个寄存器、`mv a0, sp` 后调用 `trap_handler`。 |
| 恢复执行 | `write_csr_mscratch(任务基址 + sizeof(Task))` 再 `__traps_return(&tasks[i].tf)`：`trap.S` 里的 `csrw mscratch, t2` 把用户 sp 写回 mscratch，最后 `csrrw sp, mscratch, sp` 一次性换回用户栈。 |

`launched[i]` 标志负责区分「第一次启动」和「恢复执行」。

## 4. ecall 服务

`do_syscall()` 一进入就把 `ctx->mepc += 4`——ecall 已经执行完，必须让之后被恢复的任务从
下一条指令继续，否则 `yield` / `sleep` 会反复重入 ecall 形成死循环。

| 号 | 服务 | 说明 |
| --- | --- | --- |
| 1 | `putchar` | 逐个字符输出 |
| 2 | `putstr` | 字符串输出 |
| 3 | `sleep(n)` | 按 tick 睡眠，配合定时器唤醒 |
| 4 | `spawn(fn)` | 动态创建新任务 |
| 5 | `exit` | 结束当前任务 |
| 6 | `yield` | 主动让出 CPU |
| 7 | `ticks` | 读取定时器 tick |
| 8 | `getpid` | 取当前任务号 |

## 5. 异常处理策略

| trap | mcause | 恢复方式 |
| --- | --- | --- |
| 非法指令 | 2 | 读 `*(volatile uint16_t *)mepc` 判断指令长度（低 2 位非 `0b11` 为 2 字节），`mepc += len` 跳过，任务继续 |
| 断点 | 3 | 同样跳过 |
| Load/Store fault | 5 / 7 | 标记 `TASK_EXITED`，直接 `schedule()`，结束任务 |
| 定时器中断 | `(1<<63) | 7` | `ticks++`、重设 `mtimecmp`，保存现场后轮转调度（返回原处） |

U 态里用 `asm volatile("csrr x0, 0xfff")` 制造非法指令，用读 `0x400000000UL` 制造 LoadFault，
两者都能被 monitor 捕获并按上表策略恢复。注意 `mtval` 会带上出错指令的编码 / 出错地址。

## 6. 两个踩过的坑

1. `tasks[i].tf = *ctx;` 会被 GCC 向量化为 `vsetvli / vle8.v / vse8.v`，而模拟器默认 ISA
   不声明 V 扩展，于是 M 态自己先吃了一个 `IllegalInstruction`（`mtval=0x0c0777d7` 这种
   看着莫名其妙的四位十六进制就是 vsetvli）。改成 volatile 逐字拷贝即可：

   ```c
   static void save_ctx(int idx, const TrapContext *ctx) {
       volatile uint64_t *dst = (volatile uint64_t *)&tasks[idx].tf;
       const volatile uint64_t *src = (const volatile uint64_t *)ctx;
       for (int i = 0; i < (int)(sizeof(TrapContext) / 8); i++) dst[i] = src[i];
   }
   ```

2. `test_resources/lib/io.c` 的 `vprintf` 不认 `%u` / `%lu`，会原样打印 `%u`；而 `print_hex`
   自带 `0x` 前缀，格式串里再写 `0x%lx` 会输出 `0x0x…`。所以统一用 `%ld` / `%lx`，前缀交给
   函数自己加。

## 7. 预期输出

```
AAAA [beta] start, about to execute an illegal instruction
[monitor] task 1 illegal instruction: mcause=0x2 mepc=0x800022a4 mtval=0xfff02073 -> skip 4 bytes
[beta] resumed after illegal instruction (skip policy)
[monitor] task 1 access fault: mcause=0x5 mepc=0x800022c4 mtval=0x400000000 -> kill task
0
[monitor] timer irq #1: preempt task 2 at mepc=0x80002378
1234 56789 01234
...
================ monitor summary ================
tasks started=4 exited=4, timer ticks=6, preemptions=6, switches=7
syscalls: putchar=53 putstr=7 sleep=3 spawn=1 exit=3 yield=7 ticks=1
exceptions: illegal=1 breakpoint=0 load_fault=1 store_fault=0
  [PASS] M 态 monitor 启动了多个 U 态任务
  ...
monitor: all checks passed
PASS
```

字母（alpha）与数字（gamma）交错出现，就是定时器抢占 + 时间片轮转的直接证据。
