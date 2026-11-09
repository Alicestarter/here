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


/* ================================================================
 * Chapter 0x02 / 0x03: 自定义指令 padd8 / paddus8
 * ================================================================ */

#define LANE_COUNT 8

/* Golden Model（软件参考模型）：逐 lane 做 8 位环绕加法。 */
static uint64_t golden_padd8(uint64_t lhs, uint64_t rhs) {
    uint64_t rd = 0;
    for (int i = 0; i < LANE_COUNT; i++) {
        uint8_t a = (uint8_t)((lhs >> (i * 8)) & 0xff);
        uint8_t b = (uint8_t)((rhs >> (i * 8)) & 0xff);
        uint8_t sum = (uint8_t)(a + b); /* C 的无符号转换成 8 位即回绕 */
        rd |= (uint64_t)sum << (i * 8);
    }
    return rd;
}

/* Golden Model：逐 lane 做 8 位无符号饱和加法。 */
static uint64_t golden_paddus8(uint64_t lhs, uint64_t rhs) {
    uint64_t rd = 0;
    for (int i = 0; i < LANE_COUNT; i++) {
        uint32_t a = (uint32_t)((lhs >> (i * 8)) & 0xff);
        uint32_t b = (uint32_t)((rhs >> (i * 8)) & 0xff);
        uint32_t sum = a + b;
        if (sum > 0xff) {
            sum = 0xff; /* 饱和：钳位到最大值 */
        }
        rd |= (uint64_t)sum << (i * 8);
    }
    return rd;
}

typedef uint64_t (*custom_op_fn)(uint64_t, uint64_t);
typedef uint64_t (*golden_fn)(uint64_t, uint64_t);

struct padd_vector {
    uint64_t lhs;
    uint64_t rhs;
    const char *note;
};

static const struct padd_vector padd_vectors[] = {
    {0x0102030405060708ULL, 0x0101010101010101ULL, "regular"},
    {0x0000000000000000ULL, 0x0000000000000000ULL, "all zero"},
    {0xffffffffffffffffULL, 0xffffffffffffffffULL, "all ones"},
    {0x00000000000000ffULL, 0x0000000000000001ULL, "carry isolation (low lane)"},
    {0x0000000000ff0000ULL, 0x0000000000010000ULL, "carry isolation (mid lane)"},
    {0x000000000000007fULL, 0x0000000000000001ULL, "no saturation (127+1)"},
    {0x00000000000000feULL, 0x0000000000000003ULL, "saturate (254+3)"},
    {0x7f7f7f7f7f7f7f7fULL, 0x0101010101010101ULL, "uniform 127+1"},
    {0xf0f0f0f0f0f0f0f0ULL, 0x0f0f0f0f0f0f0f0fULL, "exact 255 (no overflow)"},
    {0xdeadbeefcafebabeULL, 0x0123456789abcdefULL, "random mix"},
};

static int run_padd_family(const char *name, custom_op_fn hw, golden_fn golden) {
    const int total = (int)(sizeof(padd_vectors) / sizeof(padd_vectors[0]));
    int failed = 0;

    for (int i = 0; i < total; i++) {
        uint64_t lhs = padd_vectors[i].lhs;
        uint64_t rhs = padd_vectors[i].rhs;
        uint64_t expect = golden(lhs, rhs);
        uint64_t actual = hw(lhs, rhs);

        if (actual == expect) {
            printf("  [PASS] %s #%d (%s): %016llx + %016llx = %016llx\n", name, i,
                   padd_vectors[i].note, lhs, rhs, actual);
        } else {
            failed++;
            printf("  [FAIL] %s #%d (%s): %016llx + %016llx = %016llx, expect %016llx\n",
                   name, i, padd_vectors[i].note, lhs, rhs, actual, expect);
        }
    }

    printf("%s golden model: %d/%d passed\n", name, total - failed, total);
    return failed;
}

/* Chapter 0x02 的定点演示，保留作为可读性检查。 */
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

/* Chapter 0x03：硬件执行结果 vs Golden Model 自动比对。 */
static int custom_golden_demo(void) {
    int failed = 0;

    printf("custom instruction golden-model comparison:\n");
    failed += run_padd_family("padd8", padd8, golden_padd8);
    failed += run_padd_family("paddus8", paddus8, golden_paddus8);

    if (failed != 0) {
        printf("custom instr: %d case(s) FAILED\n", failed);
        return -1;
    }

    printf("custom instr: all golden-model cases passed\n");
    return 0;
}

// 程序入口点 (由链接脚本指定)
int main() {
    TEST_START(__BASE_FILE__);
    display_section_info();
    if (custom_instr_demo() != 0) {
        fail();
    }
    if (custom_golden_demo() != 0) {
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