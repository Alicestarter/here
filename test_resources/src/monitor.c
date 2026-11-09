/*
 * Chapter 0x04 - 中断、异常与特权级
 *
 * 一个运行在 M-Mode 的极简 monitor（多道批处理内核）：
 *
 *   1. 启动 3 个 U-Mode 任务，靠 mret 的 mstatus.MPP 从 M 态切到 U 态；
 *   2. 通过 ecall 提供系统调用：putchar / putstr / sleep / spawn / exit /
 *      yield / ticks / getpid；
 *   3. 任务 beta 故意执行非法指令与非法访存，monitor 打印
 *      mcause / mepc / mtval，并按策略分别「跳过出错指令」与「结束任务」；
 *   4. 用 CLINT 的 mtime/mtimecmp 产生机器定时器中断，中断处理里做
 *      轮转调度，让 3 个任务公平分时（简单多道批处理）。
 *
 * 上下文结构复用 lib/trap.S 的 TrapContext（34 个 64 位字）：
 *   陷阱入口先用 mscratch 换成内核栈，再在栈上分配 34*8 字节的 TrapContext；
 *   因此每个任务的「栈顶 - 34*8」必须正好是它自己的 TrapContext。
 */

#include "io.h"
#include "power.h"
#include "trap.h"
#include <stdint.h>

/* ---------------- 常量 ---------------- */

#define MAX_TASK 8
#define KSTACK_SIZE 4096
#define USTACK_SIZE 4096

/* QEMU virt / HERE virt 板上的 CLINT */
#define CLINT_BASE 0x2000000UL
#define MTIME_OFFSET 0xbff8UL
#define MTIMECMP_OFFSET 0x4000UL
/* 每个时间片的长度（mtime 计数） */
#define TIMER_INTERVAL 1500UL

/* mcause 低 63 位取值 */
#define CAUSE_ILLEGAL_INST 2
#define CAUSE_BREAKPOINT 3
#define CAUSE_LOAD_FAULT 5
#define CAUSE_STORE_FAULT 7
#define CAUSE_USER_ECALL 8
#define CAUSE_M_TIMER_IRQ 7

/* 本 monitor 的系统调用号（放在 a7） */
#define SYS_PUTCHAR 1
#define SYS_PUTSTR 2
#define SYS_SLEEP 3
#define SYS_SPAWN 4
#define SYS_EXIT 5
#define SYS_YIELD 6
#define SYS_TICKS 7
#define SYS_GETPID 8

enum task_state {
    TASK_FREE = 0,
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_EXITED,
};

/* ---------------- 任务与栈 ---------------- */

/*
 * 布局必须保证 &tf == (任务基址 + KSTACK_SIZE) - sizeof(TrapContext)，
 * 这样 __traps_return(&tf) 里 "addi sp, sp, 34*8" 之后正好回到内核栈顶。
 */
typedef struct {
    uint8_t kstack[KSTACK_SIZE - sizeof(TrapContext)];
    TrapContext tf;
} Task;

_Static_assert(sizeof(Task) == KSTACK_SIZE, "Task 大小必须等于 KSTACK_SIZE");

static Task tasks[MAX_TASK] __attribute__((aligned(16)));
static uint8_t ustack[MAX_TASK][USTACK_SIZE] __attribute__((aligned(16)));

static int state[MAX_TASK];
static uint64_t wake_at[MAX_TASK];
static int exc_hits[MAX_TASK];

static int launched[MAX_TASK];
static int current = -1;

/* ---------------- 统计量（供最后的自检使用） ---------------- */

static volatile uint64_t ticks = 0;
static volatile uint64_t preempts = 0;
static volatile uint64_t switches = 0;
static uint64_t timer_cmp = 0;

static int syscalls[16];

static int exc_illegal = 0;
static int exc_break = 0;
static int exc_load = 0;
static int exc_store = 0;
static int skipped_illegal = 0;
static int killed_on_fault = 0;
static int tasks_started = 0;
static int tasks_exited = 0;

/* ---------------- 小工具 ---------------- */

static inline uint64_t clint_read(uint64_t offset) {
    return *(volatile uint64_t*)(CLINT_BASE + offset);
}

static inline void clint_write(uint64_t offset, uint64_t value) {
    *(volatile uint64_t*)(CLINT_BASE + offset) = value;
}

/* 指令长度：低 2 位不是 0b11 表示 16 位压缩指令 */
static inline uint64_t instr_len(uint64_t pc) {
    uint16_t half = *(volatile uint16_t*)pc;
    return ((half & 0x3) == 0x3) ? 4 : 2;
}

/* 用户态系统调用封装 */
static long syscall3(long nr, long a0, long a1, long a2) {
    register long x10 asm("a0") = a0;
    register long x11 asm("a1") = a1;
    register long x12 asm("a2") = a2;
    register long x17 asm("a7") = nr;
    asm volatile("ecall" : "+r"(x10) : "r"(x11), "r"(x12), "r"(x17) : "memory");
    return x10;
}

static void u_putchar(char c) { syscall3(SYS_PUTCHAR, (long)c, 0, 0); }

static void u_putstr(const char* s) { syscall3(SYS_PUTSTR, (long)s, 0, 0); }

static void u_putdec(long v) {
    char buf[24];
    int n = 0;
    if (v < 0) {
        u_putchar('-');
        v = -v;
    }
    if (v == 0) {
        u_putchar('0');
        return;
    }
    while (v > 0 && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0) {
        u_putchar(buf[n]);
    }
}

static void u_sleep(long t) { syscall3(SYS_SLEEP, t, 0, 0); }
static void u_exit(void) { syscall3(SYS_EXIT, 0, 0, 0); }
static void u_yield(void) { syscall3(SYS_YIELD, 0, 0, 0); }
static long u_ticks(void) { return syscall3(SYS_TICKS, 0, 0, 0); }
static long u_spawn(void (*entry)(void)) { return syscall3(SYS_SPAWN, (long)entry, 0, 0); }

/* 首次进入 U 态：没有"被中断的现场"可恢复，直接铺好 CSR 后 mret */
static void boot_task(int idx) __attribute__((noreturn));
static void boot_task(int idx) {
    TrapContext* tf = &tasks[idx].tf;
    uint64_t kstack_top = (uint64_t)((uint8_t*)&tasks[idx] + sizeof(Task));
    uint64_t user_sp = tf->x[2];
    uint64_t pc = tf->mepc;
    uint64_t status = tf->mstatus;

    asm volatile("csrw mepc, %0\n\t"
                 "csrw mstatus, %1\n\t"
                 "csrw mscratch, %2\n\t"
                 "mv sp, %3\n\t"
                 "mret\n\t" ::"r"(pc),
                 "r"(status), "r"(kstack_top), "r"(user_sp)
                 : "memory");
    __builtin_unreachable();
}

/* 保存现场：逐字拷贝，volatile 防止 GCC 把它向量化（模拟器默认 ISA 不含 V） */
static void save_ctx(int idx, const TrapContext* ctx) {
    volatile uint64_t* dst = (volatile uint64_t*)&tasks[idx].tf;
    const volatile uint64_t* src = (const volatile uint64_t*)ctx;
    for (int i = 0; i < (int)(sizeof(TrapContext) / sizeof(uint64_t)); i++) {
        dst[i] = src[i];
    }
}

/* ---------------- 调度 ---------------- */

static void finish(void) __attribute__((noreturn));

static void schedule(void) __attribute__((noreturn));

/* 只剩睡眠任务时的空转：在 M 态直接轮询 mtime，模拟等待下一次时间片 */
static void idle_until_wakeup(void) {
    while (1) {
        uint64_t now = clint_read(MTIME_OFFSET);
        if (now >= timer_cmp) {
            timer_cmp += TIMER_INTERVAL;
            clint_write(MTIMECMP_OFFSET, timer_cmp);
            ticks++;
            preempts++;
            return;
        }
    }
}

static void schedule(void) {
    for (;;) {
        /* 唤醒到点的睡眠任务 */
        for (int i = 0; i < MAX_TASK; i++) {
            if (state[i] == TASK_SLEEPING && ticks >= wake_at[i]) {
                state[i] = TASK_RUNNABLE;
            }
        }

        int start = (current < 0) ? 0 : (current + 1);
        for (int k = 0; k < MAX_TASK; k++) {
            int i = (start + k) % MAX_TASK;
            if (state[i] != TASK_RUNNABLE) {
                continue;
            }
            if (i != current) {
                switches++;
            }
            current = i;
            if (!launched[i]) {
                launched[i] = 1;
                boot_task(i);
            }
            /* 恢复执行：mscratch 指向该任务内核栈顶，__traps_return 会把
               用户 sp 从 TrapContext 里换回 mscratch 再 swap */
            write_csr_mscratch((uint64_t)((uint8_t*)&tasks[i] + sizeof(Task)));
            __traps_return(&tasks[i].tf);
        }

        /* 没有可运行任务：还有睡眠的就等时间片，否则收工 */
        int sleeping = 0;
        for (int i = 0; i < MAX_TASK; i++) {
            if (state[i] == TASK_SLEEPING) {
                sleeping = 1;
            }
        }
        if (sleeping) {
            idle_until_wakeup();
            continue;
        }

        finish();
    }
}

static int spawn(void (*entry)(void)) {
    for (int i = 0; i < MAX_TASK; i++) {
        if (state[i] != TASK_FREE) {
            continue;
        }
        state[i] = TASK_RUNNABLE;
        launched[i] = 0;
        wake_at[i] = 0;
        exc_hits[i] = 0;
        tasks_started++;

        TrapContext* tf = &tasks[i].tf;
        for (int r = 0; r < 32; r++) {
            tf->x[r] = 0;
        }
        tf->x[2] = (uint64_t)&ustack[i][USTACK_SIZE]; /* 用户栈，16 字节对齐 */
        /* MPIE=1，MPP=U：mret 之后 MIE=1 且运行在 U 态 */
        tf->mstatus = 1UL << 7;
        tf->mepc = (uint64_t)entry;
        return i;
    }
    return -1;
}

/* ---------------- 陷阱处理 ---------------- */

static void do_syscall(TrapContext* ctx) {
    long nr = (long)ctx->x[17];
    long a0 = (long)ctx->x[10];

    /* ecall 本身已经执行完，先把 mepc 指向下一条，再处理——
       这样 sleep / yield / exit 之后被恢复的任务会从 ecall 的下一条继续 */
    ctx->mepc += 4;

    if (nr >= 0 && nr < 16) {
        syscalls[nr]++;
    }

    switch (nr) {
    case SYS_PUTCHAR:
        uart_putc((char)a0);
        ctx->x[10] = 0;
        break;

    case SYS_PUTSTR: {
        const char* s = (const char*)a0;
        while (*s) {
            uart_putc(*s++);
        }
        ctx->x[10] = 0;
        break;
    }

    case SYS_SLEEP:
        state[current] = TASK_SLEEPING;
        wake_at[current] = ticks + (a0 > 0 ? (uint64_t)a0 : 1);
        save_ctx(current, ctx);
        schedule();

    case SYS_SPAWN: {
        int tid = spawn((void (*)(void))a0);
        ctx->x[10] = (uint64_t)tid;
        break;
    }

    case SYS_EXIT:
        printf("\n[monitor] task %d -> exit (mepc=%lx)\n", current, (unsigned long)ctx->mepc);
        state[current] = TASK_EXITED;
        tasks_exited++;
        save_ctx(current, ctx);
        schedule();

    case SYS_YIELD:
        save_ctx(current, ctx);
        schedule();

    case SYS_TICKS:
        ctx->x[10] = ticks;
        break;

    case SYS_GETPID:
        ctx->x[10] = (uint64_t)current;
        break;

    default:
        printf("\n[monitor] unknown syscall %ld from task %d\n", nr, current);
        ctx->x[10] = (uint64_t)-1;
        break;
    }

    __traps_return(ctx);
}

void trap_handler(TrapContext* ctx) {
    uint64_t mcause = read_csr_mcause();
    uint64_t code = mcause & 0x7fffffffffffffffUL;

    /* ---------- 中断 ---------- */
    if (mcause >> 63) {
        if (code == CAUSE_M_TIMER_IRQ) {
            ticks++;
            preempts++;
            timer_cmp = clint_read(MTIME_OFFSET) + TIMER_INTERVAL;
            clint_write(MTIMECMP_OFFSET, timer_cmp);

            if (preempts <= 3) {
                printf("\n[monitor] timer irq #%ld: preempt task %d at mepc=%lx\n",
                       (long)preempts, current, (unsigned long)ctx->mepc);
            }

            /* 中断的恢复方式是「返回原处」：先保存现场，再换任务 */
            if (current >= 0) {
                save_ctx(current, ctx);
            }
            schedule();
        }
        /* 其它中断：打印一次后返回原处 */
        printf("\n[monitor] unexpected interrupt mcause=%lx\n", (unsigned long)mcause);
        __traps_return(ctx);
    }

    /* ---------- ecall（U 态） ---------- */
    if (code == CAUSE_USER_ECALL) {
        do_syscall(ctx);
    }

    /* ---------- 异常 ---------- */
    uint64_t mtval = read_csr_mtval();
    uint64_t mepc = ctx->mepc;
    uint64_t len = instr_len(mepc);
    exc_hits[current]++;

    switch (code) {
    case CAUSE_ILLEGAL_INST:
        exc_illegal++;
        printf("\n[monitor] task %d illegal instruction: mcause=%lx mepc=%lx "
               "mtval=%lx -> skip %ld bytes\n",
               current, (unsigned long)code, (unsigned long)mepc, (unsigned long)mtval,
               (unsigned long)len);
        ctx->mepc = mepc + len;
        skipped_illegal++;
        break;

    case CAUSE_BREAKPOINT:
        exc_break++;
        printf("\n[monitor] task %d breakpoint: mcause=%lx mepc=%lx mtval=%lx "
               "-> skip\n",
               current, (unsigned long)code, (unsigned long)mepc, (unsigned long)mtval);
        ctx->mepc = mepc + len;
        break;

    case CAUSE_LOAD_FAULT:
    case CAUSE_STORE_FAULT:
        if (code == CAUSE_LOAD_FAULT) {
            exc_load++;
        } else {
            exc_store++;
        }
        printf("\n[monitor] task %d access fault: mcause=%lx mepc=%lx mtval=%lx "
               "-> kill task\n",
               current, (unsigned long)code, (unsigned long)mepc, (unsigned long)mtval);
        state[current] = TASK_EXITED;
        tasks_exited++;
        killed_on_fault++;
        save_ctx(current, ctx);
        schedule();

    default:
        printf("\n[monitor] task %d unhandled trap: mcause=%lx mepc=%lx "
               "mtval=%lx -> kill task\n",
               current, (unsigned long)code, (unsigned long)mepc, (unsigned long)mtval);
        state[current] = TASK_EXITED;
        tasks_exited++;
        save_ctx(current, ctx);
        schedule();
    }

    __traps_return(ctx);
}

/* ---------------- U 态任务 ---------------- */

static void task_child(void);

/* 任务 A：分批打印，每个 round 之间主动 sleep，验证 sleep / 退出 */
static void task_alpha(void) {
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 4; i++) {
            u_putchar((char)('A' + round));
        }
        u_putchar(' ');
        u_sleep(2);
    }

    long tid = u_spawn(task_child);
    u_putstr("[alpha] spawned child task ");
    u_putdec(tid);
    u_putstr("\n");
    u_exit();
}

/* 任务 B：制造非法指令与非法访存，验证 mcause/mepc/mtval 与恢复策略 */
static void task_beta(void) {
    u_putstr("[beta] start, about to execute an illegal instruction\n");

    /* 访问不存在的 CSR -> 非法指令异常 */
    asm volatile("csrr x0, 0xfff" ::: "memory");

    u_putstr("[beta] resumed after illegal instruction (skip policy)\n");

    /* 访问 RAM 之外的地址 -> 访存 fault，monitor 直接结束本任务 */
    volatile uint64_t* bad = (volatile uint64_t*)0x400000000UL;
    uint64_t v = *bad;
    (void)v;

    u_putstr("[beta] unreachable: access fault should have killed this task\n");
    u_exit();
}

/* 任务 C：长循环 + 主动 yield，验证时间片轮转与 ecall 服务 */
static void task_gamma(void) {
    for (int i = 0; i < 30; i++) {
        u_putchar((char)('0' + (i % 10)));
        if ((i % 5) == 4) {
            u_putchar(' ');
        }
        if ((i % 4) == 3) {
            u_yield();
        }
    }

    u_putstr("[gamma] ticks seen = ");
    u_putdec(u_ticks());
    u_putstr("\n");
    u_exit();
}

/* alpha 派生的子任务 */
static void task_child(void) {
    u_putstr("[child ] hello from a spawned U-mode task\n");
    u_exit();
}

/* ---------------- 收尾自检 ---------------- */

static int failures = 0;

static void check(int cond, const char* what) {
    if (cond) {
        printf("  [PASS] %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        failures++;
    }
}

static void finish(void) {
    printf("\n================ monitor summary ================\n");
    printf("tasks started=%d exited=%d, timer ticks=%ld, preemptions=%ld, switches=%ld\n",
           tasks_started, tasks_exited, (long)ticks, (long)preempts,
           (long)switches);
    printf("syscalls: putchar=%d putstr=%d sleep=%d spawn=%d exit=%d yield=%d ticks=%d\n",
           syscalls[SYS_PUTCHAR], syscalls[SYS_PUTSTR], syscalls[SYS_SLEEP],
           syscalls[SYS_SPAWN], syscalls[SYS_EXIT], syscalls[SYS_YIELD],
           syscalls[SYS_TICKS]);
    printf("exceptions: illegal=%d breakpoint=%d load_fault=%d store_fault=%d\n", exc_illegal,
           exc_break, exc_load, exc_store);

    check(tasks_started >= 4, "M 态 monitor 启动了多个 U 态任务");
    check(preempts >= 3, "CLINT 定时器中断多次触发，发生抢占");
    check(switches >= 4, "时间片轮转带来了多次上下文切换");
    check(syscalls[SYS_PUTCHAR] > 0, "ecall 服务 putchar 可用");
    check(syscalls[SYS_SLEEP] > 0, "ecall 服务 sleep 可用");
    check(syscalls[SYS_SPAWN] > 0, "ecall 服务 spawn 可用");
    check(syscalls[SYS_EXIT] >= 3, "ecall 服务 exit 可用");
    check(exc_illegal == 1, "捕获到 1 次非法指令异常");
    check(skipped_illegal == 1, "非法指令按「跳过出错指令」恢复，任务继续执行");
    check(exc_load + exc_store >= 1, "捕获到访存 fault（mcause=5/7）");
    check(killed_on_fault == 1, "访存 fault 按「结束任务」处理");
    check(tasks_exited == tasks_started, "所有任务最终都正常结束");

    printf("=================================================\n");
    if (failures != 0) {
        printf("monitor: %d check(s) FAILED\n", failures);
        fail();
    }
    printf("monitor: all checks passed\n");
    pass();
    __builtin_unreachable();
}

/* ---------------- 入口 ---------------- */

int main() {
    TEST_START(__BASE_FILE__);

    /* 先关中断，逐个初始化 */
    write_csr_mstatus(read_csr_mstatus() & ~(1UL << 3));
    write_csr_mtvec((uint64_t)(*__traps_entry));
    write_csr_mie(read_csr_mie() | (1UL << 7)); /* MTIE：机器定时器中断 */
    write_csr_mscratch(0);

    for (int i = 0; i < MAX_TASK; i++) {
        state[i] = TASK_FREE;
        exc_hits[i] = 0;
        launched[i] = 0;
    }

    int a = spawn(task_alpha);
    int b = spawn(task_beta);
    int g = spawn(task_gamma);

    printf("monitor: M-mode kernel up, tasks alpha=%d beta=%d gamma=%d\n", a, b, g);
    printf("monitor: mstatus=%lx mtvec=%lx mie=%lx\n", (unsigned long)read_csr_mstatus(),
           (unsigned long)read_csr_mtvec(), (unsigned long)read_csr_mie());

    /* 装载第一个时间片 */
    timer_cmp = clint_read(MTIME_OFFSET) + TIMER_INTERVAL;
    clint_write(MTIMECMP_OFFSET, timer_cmp);

    /* 打开 M 态中断，切入第一个 U 态任务 */
    write_csr_mstatus(read_csr_mstatus() | (1UL << 3));
    schedule();
}
