#include "custom_ops.h"
#include "io.h"
#include "log.h"
#include "power.h"
#include <stdarg.h>
#include <stdint.h>

// 定义 UART 内存映射地址 (QEMU virt 平台)
#define UART_ADDR 0x10000000
#define UART_LSR_ADDR (UART_ADDR + 0x05)
#define UART_LSR_THRE 0x20  // Bit 5: Transmitter Holding Register Empty

extern char starttext[], endtext[], startrodata[], endrodata[], startdata[], enddata[],
    startbss[], endbss[], stack_lower_bound[], stack_top[];

void display_section_info() {
    Log(INFO, ".text section: [%08x, %08x]", (uintptr_t)(starttext), (uintptr_t)endtext);
    Log(INFO, ".rodata section: [%08x, %08x]", (uintptr_t)startrodata,
        (uintptr_t)endrodata);
    Log(INFO, ".data section: [%08x, %08x]", (uintptr_t)startdata, (uintptr_t)enddata);
    Log(INFO, ".bas section: [%08x, %08x]", (uintptr_t)startbss, (uintptr_t)endbss);
    Log(INFO, ".stack section: [%08x, %08x]", (uintptr_t)stack_lower_bound,
        (uintptr_t)stack_top);
}


/* ---- Chapter 0x02: 自定义指令 padd8 演示 ---- */
static int custom_instr_demo(void) {
    uint64_t lhs = 0x0102030405060708ULL;
    uint64_t rhs = 0x0101010101010101ULL;
    uint64_t ret = padd8(lhs, rhs);

    Log(INFO, "custom padd8: %016llx + %016llx = %016llx", lhs, rhs, ret);
    if (ret != 0x0203040506070809ULL) {
        Log(ERROR, "padd8 add case mismatch, expect 0x0203040506070809");
        return -1;
    }

    /* 逐 lane 回绕：0xff + 0x01 = 0x00，结果应为 0x0000000000000000 */
    uint64_t wrap = padd8(0xffffffffffffffffULL, 0x0101010101010101ULL);
    Log(INFO, "custom padd8 wrap: %016llx + %016llx = %016llx", 0xffffffffffffffffULL,
        0x0101010101010101ULL, wrap);
    if (wrap != 0) {
        Log(ERROR, "padd8 wrap case mismatch, expect 0x0000000000000000");
        return -1;
    }

    Log(INFO, "custom padd8 passed");
    return 0;
}

// 程序入口点 (由链接脚本指定)
int main() {
    TEST_START(__BASE_FILE__);
    display_section_info();
    if (custom_instr_demo() != 0) {
        fail();
    }
    printf("Hello Qemu.\nformat test: %8d %08x %4o %c %s\n", 1ll, 255ll, 15, 'c', "test");
    Log(ERROR, "error test.");
    Log(WARN, "warnning test.");
    Log(DEBUG, "debug test.");
    Log(INFO, "info test.");
    Log(TRACE, "trace test.");

    pass();
    return 0;
}