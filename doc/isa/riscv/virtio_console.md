# Chapter 0x05 — VirtIO-Console 外设

本章综合实验选定题目：**VirtIO-Console（Device ID 3）**。目标是在模拟器里实现一个
可被 guest 驱动、具备中断能力的控制台设备，并让 guest 通过 virtqueue 与宿主双向收发
数据。拓展实验（多端口、抢占式外部设备调度）不做。

## 1. 设备模型

| 项目 | 取值 |
| --- | --- |
| Device ID | `3`（`VirtIODeviceEnum::VirtIOConsole`） |
| MMIO 基址 | `0x1000_1000`（`board::virt::config::VIRTIO_MMIO_BASE`，`VIRTIO_MMIO_SIZE = 0x1000`） |
| PLIC 中断号 | `1`（`VIRTIO_IRQ_BASE`，第一个 virtio 设备） |
| 队列数 | 2 |
| queue 0 | `receiveq`：device → driver |
| queue 1 | `transmitq`：driver → device |
| host features | `VIRTIO_F_VERSION_1`、`VIRTIO_CONSOLE_F_SIZE`、`VIRTIO_CONSOLE_F_EMERG_WRITE` |
| config space | `cols = 80`、`rows = 24`、`max_nr_ports = 1`、`emerg_wr`（offset `0x0c`） |

`transmitq` 的缓冲区由 device 读出后写到宿主 stdout；`receiveq` 的缓冲区由 device
写入，数据来自启动时预载的输入缓冲。

## 2. 代码结构

`src/device/virtio/virtio_console.rs`

```
VirtIOConsoleDevice      : VirtIODeviceTrait 实现（MMIO 后端可见的寄存器行为）
├─ isr: Arc<AtomicU8>    中断状态位，VIRTIO_MMIO_INT_VRING = 1 << 0
├─ queues: [VirtQueue; 2]
├─ queue_select: Cell<usize>   // trait 的 queue_select 取 &self，用 Cell 存选择
├─ sender: mpsc::Sender<VirtIOConsoleAsyncTask>
└─ config_region: VirtioConsoleConfig

VirtIOConsoleAsyncWorker : DeviceTask（跑在 TaskSpawner 的 tokio runtime 上）
├─ receive(task)  从 input: Arc<Mutex<VecDeque<u8>>> 取字节填 guest 缓冲区
├─ transmit(task) std::io::stdout().lock().write_all(...)
└─ 完成后 VirtIOConsoleCompletion::complete(len) 写 used ring 并 isr.fetch_or(1, Release)
```

关键点：

* **异步不阻塞 CPU 线程**：`notify(queue_idx)` 只做 `sender.try_reserve()` +
  `take_one_request` 收描述符，真正的 I/O 交给 worker；`notify` 内部 `loop` 直到
  队列取空或 channel 满。`DrainBarrier` 用于 `reset()` 时排空在途请求。
* **内存序**：`notify` 由 MMIO 写路径先 `fence(Release)`，worker 的 `run_task` 开头
  对应 `fence(Acquire)`；isr 用 `fetch_or(1, Release)` 置位，PLIC 用 `Acquire` 读，
  保证 guest 能看到 used ring 的更新后再看到中断。
* **queue_select 用 `Cell`**：`VirtIODeviceTrait::queue_select(&self, idx)` 是
  `&self`，与 `VirtQueue` 需要的 `&mut` 冲突，所有 `set_*` / `manage_one_request`
  都通过 `self.queue_select.get()` 取当前队列。
* **config 空间写入**：`write_config` 命中 offset `0x0c`（`emerg_wr`）时立刻把字符
  写到 stdout，等价于 QEMU 的紧急写语义。

注册链路：`src/device/virtio.rs` 声明模块 → `src/board/builder.rs::add_virtio_device`
把 Block / Console 两种设备都擦成 `Box<UnsafeCell<dyn VirtIODeviceTrait>>` 再交给
`VirtIOMMIO` → `add_plic_device(..., irq)` 完成 MMIO + PLIC 双注册。

## 3. 使用方法

```bash
# 编译（无需额外 feature）
cargo build

# 默认输入（"virtio console input ok\n"）
script -q /dev/null ./target/debug/here --loglevel error \
    --device virtio-console ./test_resources/bin/virtio_console_test.elf

# 自定义输入：device 参数后面直接跟文件路径
script -q /dev/null ./target/debug/here --loglevel error \
    --device virtio-console:/etc/hostname ./test_resources/bin/virtio_console_test.elf
```

`DeviceConfig` 的字符串格式为 `<type>[:<path>]`；对 console 而言 `path` 是启动时
预载进 receive 队列的输入文件，留空则用内置问候语。

## 4. 验证

Rust 单测：

```bash
cargo test --lib virtio_console
# console_reports_device_id_and_config   ok   (device_id=3, cols/rows, VERSION_1)
# transmit_and_receive_round_trip       ok   (used ring + isr + 11 字节收包)
```

裸机端（`test_resources/src/virtio_console_test.c`）九项检查：

```
[PASS] device probe: magic=0x74726976 version=2 device_id=3 (console)
[PASS] feature negotiation: VERSION_1 accepted, cols=80 rows=24
[PASS] two virtqueues ready: receiveq=0 transmitq=1 (num=8)
[virtio-console] hello from guest
[PASS] transmitq: guest wrote 34 bytes to the host console
[PASS] interrupt status bit raised: isr=0x1
[PASS] PLIC sampled irq 1 as pending (pending=0x2)
[PASS] receiveq: guest read 24 bytes from the host input buffer
       payload: virtio console input ok
[PASS] interrupt acked; PLIC claim returned irq 1 and cleared pending
[virtio-console] irq path check
[PASS] machine external interrupt delivered, claimed irq 1
[PASS] emergency write through config space
virtio-console: all checks passed
PASS
```

## 5. 踩坑记录

1. **PLIC pending 位是锁存的**。`src/device/plic/mod.rs:143-151` 的注释写得很清楚：
   线拉低不会撤销已受理的 pending，必须 `claim` 才清。所以驱动顺序必须是
   `MMIO InterruptAck → PLIC claim → 检查 pending 已清 → complete`，先 ack 再查
   pending 会一直看到置位。
2. **`queue_select` 的 `&self` 约束**。第一版把它写成普通字段 `usize` 并给
   `queue_select(&mut self, ...)`，与 trait 签名不符；改用 `Cell<usize>`。
3. **两个设备分支的类型不一致**。`match` 里 Block 返回 `VirtIOBlkDevice`、Console 返回
   `VirtIOConsoleDevice` 无法统一，必须先各自 `Box::new(UnsafeCell::new(...))` 擦成
   `Box<UnsafeCell<dyn VirtIODeviceTrait>>`，再交给 `VirtIOMMIO::new`。
4. **`lib/io.c` 的 printf 有限**：`%x` 的输出由 `print_hex` 实现、自带 `0x` 前缀，
   格式串里再写 `0x%x` 会变成 `0x0x…`；也不支持 `%u`/`%lu`。
5. **运行需要 TTY**：用 `script -q /dev/null` 包一层，否则报
   `failed to enable terminal raw mode: Device not configured (os error 6)`。
