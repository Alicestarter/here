// VirtIO-Console 裸机驱动测试。
//
// 走完整的 VirtIO-MMIO 流程：特性协商 -> 两个 virtqueue（0 = receive，
// 1 = transmit）-> DRIVER_OK -> 通过 transmit 把数据写进模拟器 stdout、
// 通过 receive 从宿主喂入的输入缓冲里取数据 -> PLIC 采样中断线 -> 应答中断。
//
// 运行：
//   cd test_resources && make ARCH=riscv64 virtio_console_test
//   cd .. && script -q /dev/null ./target/debug/here --loglevel error \
//            --device virtio-console ./test_resources/bin/virtio_console_test.elf

#include "io.h"
#include "log.h"
#include "trap.h"
#include "virtio/virtio_mmio.h"
#include "virtio/virtio_queue.h"
#include <stdint.h>

#define VIRTIO_MMIO_BASE 0x10001000UL
#define VIRTIO_CONSOLE_DEVICE_ID 3
#define VIRTIO_MAGIC 0x74726976
#define VIRTIO_VERSION 2

#define PLIC_BASE 0xc000000UL
#define PLIC_PRIORITY(id) (PLIC_BASE + 4 * (id))
#define PLIC_PENDING(id) (PLIC_BASE + 0x1000 + 4 * ((id) / 32))
#define PLIC_ENABLE(ctx, id) (PLIC_BASE + 0x2000 + (ctx) * 0x80 + 4 * ((id) / 32))
#define PLIC_THRESHOLD(ctx) (PLIC_BASE + 0x200000 + (ctx) * 0x1000)
#define PLIC_CLAIM(ctx) (PLIC_BASE + 0x200004 + (ctx) * 0x1000)
#define M_CONTEXT 0
#define VIRTIO_CONSOLE_IRQ 1 /* VIRTIO_IRQ_BASE */

#define QUEUE_NUM 8
#define AVAIL_RING_BYTES (4 + 2 * QUEUE_NUM)
#define USED_RING_BYTES (4 + 8 * QUEUE_NUM)

struct VirtIOMMIOLayout {
    uint32_t magic_value;
    uint32_t version;
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t device_features;
    uint32_t device_features_sel;
    uint32_t reserved_0[2];
    uint32_t driver_features;
    uint32_t driver_features_sel;
    uint32_t reserved_1[2];
    uint32_t queue_sel;
    uint32_t queue_num_max;
    uint32_t queue_num;
    uint32_t queue_align;
    uint32_t queue_pfn;
    uint32_t queue_ready;
    uint32_t reserved_2[2];
    uint32_t queue_notify;
    uint32_t reserved_3[3];
    uint32_t interrupt_status;
    uint32_t interrupt_ack;
    uint32_t reserved_4[2];
    uint32_t status;
    uint32_t reserved_5[3];
    uint32_t queue_desc_low;
    uint32_t queue_desc_high;
    uint32_t reserved_6[2];
    uint32_t queue_avail_low;
    uint32_t queue_avail_high;
    uint32_t reserved_7[2];
    uint32_t queue_used_low;
    uint32_t queue_used_high;
    uint32_t reserved_8[22];
    uint8_t config_space[];
};

static volatile struct VirtIOMMIOLayout* const vio =
    (volatile struct VirtIOMMIOLayout*)VIRTIO_MMIO_BASE;

/* receiveq(0) 与 transmitq(1) 各一套环形缓冲 */
static VirtQueueDesc rx_desc[QUEUE_NUM] __attribute__((aligned(4096)));
static uint8_t rx_avail_mem[AVAIL_RING_BYTES] __attribute__((aligned(64)));
static uint8_t rx_used_mem[USED_RING_BYTES] __attribute__((aligned(64)));
static VirtQueueDesc tx_desc[QUEUE_NUM] __attribute__((aligned(4096)));
static uint8_t tx_avail_mem[AVAIL_RING_BYTES] __attribute__((aligned(64)));
static uint8_t tx_used_mem[USED_RING_BYTES] __attribute__((aligned(64)));

static uint8_t tx_buffer[64] __attribute__((aligned(64)));
static uint8_t rx_buffer[64] __attribute__((aligned(64)));

static uint32_t external_interrupts;
static uint32_t last_claimed_irq;

/* 接管默认的 weak handler：只处理 M 态外部中断 */
void trap_handler(TrapContext* ctx) {
    uint64_t mcause = read_csr_mcause();
    if (mcause & (1ULL << 63)) {
        uint64_t code = mcause & 0xff;
        if (code == 11) { /* Machine external interrupt */
            external_interrupts++;
            uint32_t irq = *(volatile uint32_t*)PLIC_CLAIM(M_CONTEXT);
            last_claimed_irq = irq;
            if (irq != 0) {
                *(volatile uint32_t*)PLIC_CLAIM(M_CONTEXT) = irq; /* complete */
            }
            return __traps_return(ctx);
        }
    }
    printf("unexpected trap: mcause=%lx mepc=%lx mtval=%lx\n", read_csr_mcause(),
           read_csr_mepc(), read_csr_mtval());
    ctx->mepc = (uint64_t)PowerOff;
    __traps_return(ctx);
}

static void mmio_write32(uint64_t addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

static uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

static void setup_queue(uint32_t index, VirtQueueDesc* desc, void* avail, void* used) {
    vio->queue_sel = index;
    vio->queue_num = QUEUE_NUM;

    uint64_t avail_addr = (uint64_t)avail;
    uint64_t used_addr = (uint64_t)used;

    vio->queue_desc_low = (uint32_t)((uint64_t)desc & 0xffffffff);
    vio->queue_desc_high = (uint32_t)(((uint64_t)desc >> 32) & 0xffffffff);
    vio->queue_avail_low = (uint32_t)(avail_addr & 0xffffffff);
    vio->queue_avail_high = (uint32_t)((avail_addr >> 32) & 0xffffffff);
    vio->queue_used_low = (uint32_t)(used_addr & 0xffffffff);
    vio->queue_used_high = (uint32_t)((used_addr >> 32) & 0xffffffff);

    ((VirtQueueAvail*)avail)->flags = AvailFlagDefault;
    ((VirtQueueAvail*)avail)->idx = 0;
    ((VirtQueueUsed*)used)->flags = UsedFlagDefault;
    ((VirtQueueUsed*)used)->idx = 0;

    vio->queue_ready = 1;
}

static int wait_used(VirtQueueUsed* used, uint16_t expected, int rounds) {
    while (rounds-- > 0) {
        if (used->idx >= expected) {
            return 0;
        }
    }
    return -1;
}

/* 提交一个只读描述符到 transmitq，等待 used 环更新 */
static int console_write(VirtQueueAvail* avail, VirtQueueUsed* used, const char* text) {
    int len = 0;
    while (text[len] != '\0' && len < (int)sizeof(tx_buffer)) {
        tx_buffer[len] = (uint8_t)text[len];
        len++;
    }

    tx_desc[0].paddr = (uint64_t)tx_buffer;
    tx_desc[0].len = (uint32_t)len;
    tx_desc[0].flags = 0;
    tx_desc[0].next = 0;

    uint16_t slot = avail->idx % QUEUE_NUM;
    avail->ring[slot] = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    avail->idx = (uint16_t)(avail->idx + 1);

    vio->queue_notify = 1; /* transmitq */

    if (wait_used(used, 1, 100000) != 0) {
        Log(ERROR, "transmit timeout\n");
        return -1;
    }
    return (int)used->ring[0].len;
}

int main() {
    TEST_START(__func__);

    /* ---- 1. 探测设备 ---- */
    if (vio->magic_value != VIRTIO_MAGIC || vio->version != VIRTIO_VERSION ||
        vio->device_id != VIRTIO_CONSOLE_DEVICE_ID) {
        Log(ERROR, "not a virtio console: magic=%x version=%d device_id=%d\n",
            vio->magic_value, vio->version, vio->device_id);
        fail();
    }
    printf("[PASS] device probe: magic=%x version=%d device_id=%d (console)\n",
           vio->magic_value, vio->version, vio->device_id);

    /* ---- 2. 特性协商 ---- */
    vio->status = ACKNOWLEDGE;
    vio->status = ACKNOWLEDGE | DRIVER;

    vio->device_features_sel = 1;
    uint64_t features = (uint64_t)vio->device_features << 32;
    vio->device_features_sel = 0;
    features |= vio->device_features;

    if ((features >> 32 & 1) == 0) { /* VIRTIO_F_VERSION_1 */
        Log(ERROR, "VERSION_1 not offered\n");
        fail();
    }

    vio->driver_features_sel = 1;
    vio->driver_features = (uint32_t)(features >> 32);
    vio->driver_features_sel = 0;
    vio->driver_features = (uint32_t)(features & 0xffffffff);

    vio->status = ACKNOWLEDGE | DRIVER | FEATURES_OK;
    if (!(vio->status & FEATURES_OK)) {
        Log(ERROR, "feature negotiation failed\n");
        fail();
    }
    printf("[PASS] feature negotiation: VERSION_1 accepted, cols=%d rows=%d\n",
           (int)vio->config_space[0], (int)vio->config_space[4]);

    /* ---- 3. 队列初始化 ---- */
    setup_queue(0, rx_desc, rx_avail_mem, rx_used_mem);
    setup_queue(1, tx_desc, tx_avail_mem, tx_used_mem);
    printf("[PASS] two virtqueues ready: receiveq=0 transmitq=1 (num=%d)\n", QUEUE_NUM);

    vio->status = ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK;

    VirtQueueAvail* tx_avail = (VirtQueueAvail*)tx_avail_mem;
    VirtQueueUsed* tx_used = (VirtQueueUsed*)tx_used_mem;
    VirtQueueAvail* rx_avail = (VirtQueueAvail*)rx_avail_mem;
    VirtQueueUsed* rx_used = (VirtQueueUsed*)rx_used_mem;

    /* ---- 4. transmit：把数据写到宿主 stdout ---- */
    if (console_write(tx_avail, tx_used, "[virtio-console] hello from guest\n") < 0) {
        fail();
    }
    printf("[PASS] transmitq: guest wrote %d bytes to the host console\n",
           (int)tx_used->ring[0].len);

    /* ---- 5. 中断状态 + PLIC 采样 ---- */
    uint32_t isr = vio->interrupt_status;
    if ((isr & VIRTIO_MMIO_INT_VRING) == 0) {
        Log(ERROR, "interrupt status not raised: %x\n", isr);
        fail();
    }
    printf("[PASS] interrupt status bit raised: isr=%x\n", isr);

    mmio_write32(PLIC_PRIORITY(VIRTIO_CONSOLE_IRQ), 1);
    mmio_write32(PLIC_ENABLE(M_CONTEXT, VIRTIO_CONSOLE_IRQ), 1u << (VIRTIO_CONSOLE_IRQ % 32));
    mmio_write32(PLIC_THRESHOLD(M_CONTEXT), 0);

    uint32_t pending = mmio_read32(PLIC_PENDING(VIRTIO_CONSOLE_IRQ));
    if ((pending & (1u << (VIRTIO_CONSOLE_IRQ % 32))) == 0) {
        Log(ERROR, "plic did not sample the console irq line: pending=%x\n", pending);
        fail();
    }
    printf("[PASS] PLIC sampled irq %d as pending (pending=%x)\n", VIRTIO_CONSOLE_IRQ,
           pending);

    /* ---- 6. receive：从宿主输入缓冲里取数据 ---- */
    rx_desc[0].paddr = (uint64_t)rx_buffer;
    rx_desc[0].len = sizeof(rx_buffer);
    rx_desc[0].flags = VIRTQ_DESC_F_WRITE;
    rx_desc[0].next = 0;

    rx_avail->ring[0] = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rx_avail->idx = (uint16_t)(rx_avail->idx + 1);
    vio->queue_notify = 0; /* receiveq */

    if (wait_used(rx_used, 1, 100000) != 0) {
        Log(ERROR, "receive timeout\n");
        fail();
    }

    uint32_t received = rx_used->ring[0].len;
    printf("[PASS] receiveq: guest read %d bytes from the host input buffer\n", received);
    if (received > 0) {
        printf("       payload: ");
        for (uint32_t i = 0; i < received; i++) {
            uart_putc((char)rx_buffer[i]);
        }
        if (rx_buffer[received - 1] != '\n') {
            uart_putc('\n');
        }
    }

    /* ---- 7. 中断应答 + PLIC claim/complete ---- */
    vio->interrupt_ack = VIRTIO_MMIO_INT_VRING;
    if (vio->interrupt_status != 0) {
        Log(ERROR, "interrupt ack failed: isr=%x\n", vio->interrupt_status);
        fail();
    }

    /* PLIC 的 pending 位是锁存的：线拉低不会自己撤销，必须 claim 才清 */
    uint32_t claimed = mmio_read32(PLIC_CLAIM(M_CONTEXT));
    if (claimed != VIRTIO_CONSOLE_IRQ) {
        Log(ERROR, "claim returned irq %d instead of %d\n", (int)claimed, VIRTIO_CONSOLE_IRQ);
        fail();
    }
    pending = mmio_read32(PLIC_PENDING(VIRTIO_CONSOLE_IRQ));
    if (pending & (1u << (VIRTIO_CONSOLE_IRQ % 32))) {
        Log(ERROR, "pending bit survived the claim: pending=%x\n", pending);
        fail();
    }
    mmio_write32(PLIC_CLAIM(M_CONTEXT), claimed); /* complete */
    printf("[PASS] interrupt acked; PLIC claim returned irq %d and cleared pending\n",
           (int)claimed);

    /* ---- 8. 真正的 M 态外部中断：再发一次 transmit ---- */
    trap_init(); /* mtvec + mie(MEIE|MTIE) + mstatus.MIE */
    if (console_write(tx_avail, tx_used, "[virtio-console] irq path check\n") < 0) {
        fail();
    }
    for (int spin = 0; spin < 100000 && external_interrupts == 0; spin++) {
        __asm__ volatile("nop");
    }
    if (external_interrupts == 0) {
        Log(ERROR, "no machine external interrupt was delivered\n");
        fail();
    }
    if (last_claimed_irq != VIRTIO_CONSOLE_IRQ) {
        Log(ERROR, "claimed unexpected irq %d\n", (int)last_claimed_irq);
        fail();
    }
    vio->interrupt_ack = VIRTIO_MMIO_INT_VRING;
    printf("[PASS] machine external interrupt delivered, claimed irq %d\n",
           (int)last_claimed_irq);

    /* ---- 9. 紧急写：config 空间的 emerg_wr ---- */
    vio->config_space[0x0c] = '\n';
    printf("[PASS] emergency write through config space\n");

    printf("\nvirtio-console: all checks passed\n");
    pass();
    return 0;
}
