//! VirtIO console device (device id 3).
//!
//! The console exposes the two mandatory queues of the VirtIO console
//! specification: queue 0 is the receive queue (device -> driver), queue 1 is
//! the transmit queue (driver -> device).
//!
//! * Transmit: the worker copies the guest buffers straight to the emulator's
//!   stdout, i.e. the byte stream the host terminal shows.
//! * Receive: the worker drains an input buffer that the host side can fill with
//!   [`VirtIOConsoleDeviceBuilder::input_bytes`]; whatever is available is
//!   scattered into the guest's writable descriptor buffers. An empty input
//!   finishes the request with a used length of zero, so a driver may simply
//!   re-post the buffer.
//!
//! Both directions are handled on the device task runtime: `notify` only turns
//! the available ring into queued work items, it never touches guest memory
//! beyond descriptor decoding, and the used ring plus the interrupt status bit
//! are published by the worker after the transfer (see
//! [`VirtIOConsoleCompletion::complete`]).

use std::{
    collections::VecDeque,
    io::Write,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU8, Ordering},
    },
    vec::Vec,
};

use log::{error, info};
use tokio::sync::{mpsc, watch};

use crate::{
    device::virtio::{
        config::{VIRTQUEUE_MAX_SIZE, VirtIOFeatureSet, virtio_reserved_feature},
        virtio_device::{VirtIODeviceEnum, VirtIODeviceTrait},
        virtio_mmio::VirtIODeviceStatus,
        virtio_queue::{VirtQueue, VirtQueueCompletion, VirtQueueDesc},
    },
    task_spawner::{DeviceTask, TaskSpawner},
};

pub(super) const VIRTIO_CONSOLE_RECEIVE_QUEUE: usize = 0;
pub(super) const VIRTIO_CONSOLE_TRANSMIT_QUEUE: usize = 1;
const VIRTIO_CONSOLE_QUEUE_COUNT: u32 = 2;
const VIRTIO_CONSOLE_TASK_CAPACITY: usize = VIRTQUEUE_MAX_SIZE as usize;

#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(unused)]
/// Standard VirtIO console feature bits.
pub(crate) enum VirtIOConsoleFeature {
    /// Configuration space carries `cols` / `rows`.
    Size = 1 << 0,
    /// Multiport console (not implemented).
    Multiport = 1 << 1,
    /// Writing `emerg_wr` in the configuration space emits a byte immediately.
    EmergWrite = 1 << 2,
}

impl VirtIOConsoleFeature {
    pub(crate) const fn bit(self) -> VirtIOFeatureSet {
        self as VirtIOFeatureSet
    }
}

#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub(crate) struct VirtioConsoleConfig {
    pub(crate) cols: u32,         // 0x00: number of columns (VIRTIO_CONSOLE_F_SIZE)
    pub(crate) rows: u32,         // 0x04: number of rows    (VIRTIO_CONSOLE_F_SIZE)
    pub(crate) max_nr_ports: u32, // 0x08: number of ports   (VIRTIO_CONSOLE_F_MULTIPORT)
    pub(crate) emerg_wr: u32,     // 0x0c: emergency write   (VIRTIO_CONSOLE_F_EMERG_WRITE)
}

impl Default for VirtioConsoleConfig {
    fn default() -> Self {
        Self {
            cols: 80,
            rows: 24,
            max_nr_ports: 1,
            emerg_wr: 0,
        }
    }
}

impl VirtioConsoleConfig {
    fn as_bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self as *const Self as *const u8, size_of::<Self>()) }
    }

    fn as_bytes_mut(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self as *mut Self as *mut u8, size_of::<Self>()) }
    }
}

#[derive(Clone, Copy)]
/// One guest-RAM segment retained for an asynchronous console transfer.
struct ConsoleTaskRamBuffer {
    addr: usize,
    len: usize,
}

/// Guest-visible state required to publish a completed console request.
struct VirtIOConsoleCompletion {
    queue_completion: VirtQueueCompletion,
    isr: Arc<AtomicU8>,
}

impl VirtIOConsoleCompletion {
    /// Publish the used-ring entry first, then raise the interrupt status bit.
    ///
    /// The `Release` store orders the DMA writes in front of the interrupt, so
    /// the PLIC sample taken by `irq_level` observes a finished transfer.
    fn complete(self, used_len: u32) {
        self.queue_completion.complete(used_len);
        self.isr.fetch_or(1, Ordering::Release);
    }
}

struct VirtIOConsoleTransmitTask {
    buffers: Vec<ConsoleTaskRamBuffer>,
    completion: VirtIOConsoleCompletion,
}

struct VirtIOConsoleReceiveTask {
    buffers: Vec<ConsoleTaskRamBuffer>,
    completion: VirtIOConsoleCompletion,
}

/// Work item processed by the asynchronous VirtIO console worker.
enum VirtIOConsoleAsyncTask {
    Transmit(VirtIOConsoleTransmitTask),
    Receive(VirtIOConsoleReceiveTask),
    /// Acknowledge after all tasks submitted before this marker are complete.
    DrainBarrier(std::sync::mpsc::SyncSender<()>),
}

/// Executes queued console transfers on the device task runtime.
pub(super) struct VirtIOConsoleAsyncWorker {
    receiver: mpsc::Receiver<VirtIOConsoleAsyncTask>,
    input: Arc<Mutex<VecDeque<u8>>>,
}

impl DeviceTask for VirtIOConsoleAsyncWorker {
    async fn run(mut self, mut cancel: watch::Receiver<bool>) {
        loop {
            let task = tokio::select! {
                _ = cancel.changed() => return,
                task = self.receiver.recv() => match task {
                    Some(task) => task,
                    None => return,
                },
            };

            self.run_task(task);
            if *cancel.borrow() {
                return;
            }
        }
    }
}

impl VirtIOConsoleAsyncWorker {
    fn new(
        receiver: mpsc::Receiver<VirtIOConsoleAsyncTask>,
        input: Arc<Mutex<VecDeque<u8>>>,
    ) -> Self {
        Self { receiver, input }
    }

    fn run_task(&mut self, task: VirtIOConsoleAsyncTask) {
        // Synchronize the writes the guest published before QueueNotify.
        std::sync::atomic::fence(Ordering::Acquire);

        match task {
            VirtIOConsoleAsyncTask::Transmit(task) => self.transmit(task),
            VirtIOConsoleAsyncTask::Receive(task) => self.receive(task),
            VirtIOConsoleAsyncTask::DrainBarrier(ack) => {
                let _ = ack.send(());
            }
        }
    }

    fn transmit(&mut self, task: VirtIOConsoleTransmitTask) {
        let mut total = 0u32;
        let stdout = std::io::stdout();
        let mut out = stdout.lock();
        for buffer in &task.buffers {
            let data = unsafe { std::slice::from_raw_parts(buffer.addr as *const u8, buffer.len) };
            if out.write_all(data).is_err() {
                error!("[virtio-console] failed to write transmit buffer to stdout");
                break;
            }
            total += data.len() as u32;
        }
        let _ = out.flush();
        task.completion.complete(total);
    }

    fn receive(&mut self, task: VirtIOConsoleReceiveTask) {
        let mut total = 0u32;
        let mut input = self
            .input
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        for buffer in &task.buffers {
            if input.is_empty() {
                break;
            }
            let data =
                unsafe { std::slice::from_raw_parts_mut(buffer.addr as *mut u8, buffer.len) };
            for slot in data.iter_mut() {
                match input.pop_front() {
                    Some(byte) => {
                        *slot = byte;
                        total += 1;
                    }
                    None => break,
                }
            }
        }
        task.completion.complete(total);
    }

    #[cfg(test)]
    pub(super) fn process_one(&mut self) -> bool {
        match self.receiver.try_recv() {
            Ok(task) => {
                self.run_task(task);
                true
            }
            Err(mpsc::error::TryRecvError::Empty | mpsc::error::TryRecvError::Disconnected) => {
                false
            }
        }
    }
}

// ======================================
//          VirtIO Console Device
// ======================================
pub(crate) struct VirtIOConsoleDevice {
    pub(crate) name: &'static str,
    pub(crate) status: u8,
    pub(crate) isr: Arc<AtomicU8>,

    host_feature: VirtIOFeatureSet,
    guest_feature: VirtIOFeatureSet,

    pub(crate) generation: u32,
    ram_base_raw: usize,

    sender: mpsc::Sender<VirtIOConsoleAsyncTask>,

    queues: [VirtQueue; VIRTIO_CONSOLE_QUEUE_COUNT as usize],
    /// Index of the queue the driver currently addresses through MMIO.
    ///
    /// `VirtIODeviceTrait::queue_select` takes `&self`, so the selection lives
    /// in a `Cell` instead of a plain field.
    queue_select: std::cell::Cell<usize>,

    pub(super) config_region: VirtioConsoleConfig,
}

impl VirtIOConsoleDevice {
    fn new(
        name: &'static str,
        ram_base_raw: *mut u8,
        sender: mpsc::Sender<VirtIOConsoleAsyncTask>,
    ) -> Self {
        info!("build virtio console device.");

        Self {
            name,
            status: 0,
            isr: Arc::new(AtomicU8::new(0)),
            host_feature: virtio_reserved_feature::VERSION_1
                | VirtIOConsoleFeature::Size.bit()
                | VirtIOConsoleFeature::EmergWrite.bit(),
            guest_feature: 0,
            generation: 0,
            ram_base_raw: ram_base_raw as usize,
            sender,
            queues: [
                VirtQueue::new(ram_base_raw, 0),
                VirtQueue::new(ram_base_raw, 0),
            ],
            queue_select: std::cell::Cell::new(0),
            config_region: VirtioConsoleConfig::default(),
        }
    }

    fn current_queue(&mut self) -> &mut VirtQueue {
        &mut self.queues[self.queue_select.get()]
    }

    /// Turn every available descriptor chain of `queue_idx` into a worker task.
    fn submit_requests(&mut self, queue_idx: usize) -> bool {
        if queue_idx >= self.queues.len() {
            error!("[virtio-console] invalid queue index {queue_idx}");
            return false;
        }
        let Ok(permit) = self.sender.try_reserve() else {
            return false;
        };

        let ram_base_raw = self.ram_base_raw;
        let mut buffers: Vec<ConsoleTaskRamBuffer> = Vec::new();
        let Some(completion) =
            self.queues[queue_idx].take_one_request(|desc: &VirtQueueDesc, _idx: usize| {
                buffers.push(ConsoleTaskRamBuffer {
                    addr: desc.get_request_package::<u8>(ram_base_raw) as usize,
                    len: desc.len as usize,
                });
            })
        else {
            return false;
        };

        let completion = VirtIOConsoleCompletion {
            queue_completion: completion,
            isr: self.isr.clone(),
        };
        let task = if queue_idx == VIRTIO_CONSOLE_RECEIVE_QUEUE {
            VirtIOConsoleAsyncTask::Receive(VirtIOConsoleReceiveTask {
                buffers,
                completion,
            })
        } else {
            VirtIOConsoleAsyncTask::Transmit(VirtIOConsoleTransmitTask {
                buffers,
                completion,
            })
        };
        permit.send(task);
        true
    }
}

impl VirtIODeviceTrait for VirtIOConsoleDevice {
    fn get_device_id(&self) -> u16 {
        VirtIODeviceEnum::VirtIOConsole as u16
    }

    fn status(&mut self) -> &mut u8 {
        &mut self.status
    }

    fn get_generation(&self) -> u32 {
        self.generation
    }

    fn reset(&mut self) {
        info!("reset virtio console device.");

        // Drain in-flight transfers before the device forgets the queues: the
        // worker is single threaded, so reaching the barrier means every
        // request submitted earlier has already published its completion.
        let (drain_tx, drain_rx) = std::sync::mpsc::sync_channel(0);
        if self
            .sender
            .blocking_send(VirtIOConsoleAsyncTask::DrainBarrier(drain_tx))
            .is_ok()
        {
            let _ = drain_rx.recv();
        }

        self.status = 0;
        self.guest_feature = 0;
        self.queue_select.set(0);
        for queue in &mut self.queues {
            queue.reset();
        }
        self.isr.store(0, Ordering::Release);
    }

    fn isr(&self) -> &AtomicU8 {
        &self.isr
    }

    fn irq_level(&mut self) -> bool {
        self.isr.load(Ordering::Acquire) != 0
            && (self.queues[VIRTIO_CONSOLE_RECEIVE_QUEUE].interrupts_enabled()
                || self.queues[VIRTIO_CONSOLE_TRANSMIT_QUEUE].interrupts_enabled())
    }

    fn get_host_feature(&self) -> VirtIOFeatureSet {
        self.host_feature
    }

    fn set_feature(&mut self, feature: VirtIOFeatureSet) {
        info!("set virtio console feature: {feature:#x}.");
        if feature & virtio_reserved_feature::VERSION_1 == 0
            || self.host_feature & feature != feature
        {
            self.status &= !VirtIODeviceStatus::FEATURES_OK.bits();
        } else {
            self.guest_feature = feature;
        }
    }

    fn set_queue_num(&mut self, num: u32) {
        self.current_queue().set_queue_num(num);
    }

    fn queue_ready(&self) -> bool {
        self.queues[self.queue_select.get()].ready()
    }

    fn queue_select(&self, idx: u32) {
        if (idx as usize) < self.queues.len() {
            self.queue_select.set(idx as usize);
        }
    }

    fn get_num_of_queue(&self) -> u32 {
        VIRTIO_CONSOLE_QUEUE_COUNT
    }

    fn set_desc(&mut self, addr: u64) {
        self.current_queue().set_desc(addr);
    }

    fn set_avail(&mut self, addr: u64) {
        self.current_queue().set_avail(addr);
    }

    fn set_used(&mut self, addr: u64) {
        self.current_queue().set_used(addr);
    }

    fn manage_one_request(&mut self) -> bool {
        self.submit_requests(self.queue_select.get())
    }

    fn notify(&mut self, queue_idx: u32) {
        let idx = queue_idx as usize;
        if idx >= self.queues.len() {
            error!("[virtio-console] notify for unknown queue {queue_idx}");
            return;
        }
        loop {
            if !self.submit_requests(idx) {
                break;
            }
        }
    }

    fn read_config(&mut self, offset: u64, len: u32) -> u64 {
        let offset = offset as usize;
        let len = len as usize;
        let bytes = self.config_region.as_bytes();
        if offset.checked_add(len).is_none_or(|end| end > bytes.len()) {
            error!("virtio-console config read out of range: offset={offset:#x}, len={len}");
            return 0;
        }

        let mut value = 0u64;
        for (idx, byte) in bytes[offset..offset + len].iter().enumerate() {
            value |= (*byte as u64) << (idx * u8::BITS as usize);
        }
        value
    }

    fn write_config(&mut self, offset: u64, len: u32, data: u64) {
        let offset = offset as usize;
        let len = len as usize;
        let bytes = self.config_region.as_bytes_mut();
        if offset.checked_add(len).is_none_or(|end| end > bytes.len()) {
            error!("virtio-console config write out of range: offset={offset:#x}, len={len}");
            return;
        }

        for (idx, byte) in bytes[offset..offset + len].iter_mut().enumerate() {
            *byte = (data >> (idx * u8::BITS as usize)) as u8;
        }

        // `emerg_wr` is write-only: the device emits the byte immediately.
        if offset <= 0x0c && offset + len >= 0x10 {
            let byte = self.config_region.emerg_wr as u8;
            let mut out = std::io::stdout().lock();
            let _ = out.write_all(&[byte]);
            let _ = out.flush();
        }
    }
}

#[cfg(test)]
impl VirtIOConsoleDevice {
    pub(crate) fn queue(&mut self, idx: usize) -> &mut VirtQueue {
        &mut self.queues[idx]
    }
}

pub struct VirtIOConsoleDeviceBuilder {
    device: VirtIOConsoleDevice,
    receiver: mpsc::Receiver<VirtIOConsoleAsyncTask>,
    input: Arc<Mutex<VecDeque<u8>>>,
}

impl VirtIOConsoleDeviceBuilder {
    pub fn new(ram_base_raw: *mut u8) -> Self {
        let (sender, receiver) = mpsc::channel(VIRTIO_CONSOLE_TASK_CAPACITY);
        let input = Arc::new(Mutex::new(VecDeque::new()));
        Self {
            device: VirtIOConsoleDevice::new("Unnamed VirtIO Console Device", ram_base_raw, sender),
            receiver,
            input,
        }
    }

    pub fn name(mut self, name: &'static str) -> Self {
        self.device.name = name;
        self
    }

    pub fn generation(mut self, generation: u32) -> Self {
        self.device.generation = generation;
        self
    }

    /// Preload bytes that the guest can read from the receive queue.
    pub fn input_bytes(self, bytes: &[u8]) -> Self {
        self.input
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .extend(bytes.iter().copied());
        self
    }

    pub fn get_and_spawner_task(self, task_spawner: &mut TaskSpawner) -> VirtIOConsoleDevice {
        let Self {
            device,
            receiver,
            input,
        } = self;
        task_spawner.register(VirtIOConsoleAsyncWorker::new(receiver, input));
        device
    }

    #[cfg(test)]
    pub(crate) fn get(self) -> VirtIOConsoleDevice {
        self.get_with_worker().0
    }

    #[cfg(test)]
    pub(super) fn get_with_worker(self) -> (VirtIOConsoleDevice, VirtIOConsoleAsyncWorker) {
        let Self {
            device,
            receiver,
            input,
        } = self;
        (device, VirtIOConsoleAsyncWorker::new(receiver, input))
    }
}

#[cfg(test)]
mod test {
    use std::slice;

    use crate::{
        device::virtio::virtio_queue::{
            VirtQueueAvail, VirtQueueAvailFlag, VirtQueueDescFlag, VirtQueueUsed, VirtQueueUsedFlag,
        },
        ram::Ram,
        ram_config,
    };

    use super::*;

    const QUEUE_NUM: usize = 8;

    fn setup_queue(device: &mut VirtIOConsoleDevice, ram: &mut Ram, queue_idx: usize, base: u64) {
        device.queue_select(queue_idx as u32);
        device.set_queue_num(QUEUE_NUM as u32);

        let desc_base = base;
        let avail_base = base + 0x100;
        let used_base = base + 0x200;
        device.set_desc(desc_base);
        device.set_avail(avail_base);
        device.set_used(used_base);

        unsafe {
            let avail = &mut ram[(avail_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueAvail;
            avail.as_mut().unwrap().init(VirtQueueAvailFlag::Default);
            let used = &mut ram[(used_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueUsed;
            used.as_mut().unwrap().init(VirtQueueUsedFlag::Default);
        }
    }

    fn push_desc(
        ram: &mut Ram,
        desc_base: u64,
        idx: usize,
        addr: u64,
        len: u32,
        flags: VirtQueueDescFlag,
    ) {
        unsafe {
            let desc = &mut ram[(desc_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut crate::device::virtio::virtio_queue::VirtQueueDesc;
            desc.add(idx).as_mut().unwrap().init(addr, len, flags, 0);
        }
    }

    fn push_avail(ram: &mut Ram, avail_base: u64, desc_idx: u16) {
        unsafe {
            let avail = &mut ram[(avail_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueAvail;
            let avail = avail.as_mut().unwrap();
            let ring = VirtQueueAvail::mut_ring(avail as *mut _ as u64, QUEUE_NUM as u32);
            ring[0] = desc_idx;
            avail.idx_atomic_add(1);
        }
    }

    fn used_index(ram: &mut Ram, used_base: u64) -> u16 {
        unsafe {
            let used = &mut ram[(used_base - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueUsed;
            used.as_mut().unwrap().get_index()
        }
    }

    #[test]
    fn console_reports_device_id_and_config() {
        let mut ram = Ram::new();
        let ram_base = &mut ram[0] as *mut u8;
        let mut device = VirtIOConsoleDeviceBuilder::new(ram_base).get();

        assert_eq!(
            device.get_device_id(),
            VirtIODeviceEnum::VirtIOConsole as u16
        );
        assert_eq!(device.get_num_of_queue(), 2);
        assert_eq!(device.read_config(0, 4), 80);
        assert_eq!(device.read_config(4, 4), 24);
        assert_ne!(
            device.get_host_feature() & virtio_reserved_feature::VERSION_1,
            0
        );
    }

    #[test]
    fn transmit_and_receive_round_trip() {
        let mut ram = Ram::new();
        let ram_base = &mut ram[0] as *mut u8;
        let (mut device, mut worker) = VirtIOConsoleDeviceBuilder::new(ram_base)
            .input_bytes(b"hello guest")
            .get_with_worker();

        // --- transmit queue: guest -> host ---
        let base = 0x8000_2000u64;
        setup_queue(&mut device, &mut ram, VIRTIO_CONSOLE_TRANSMIT_QUEUE, base);
        let payload_addr = 0x8000_3000u64;
        unsafe {
            let buf = slice::from_raw_parts_mut(
                &mut ram[(payload_addr - ram_config::BASE_ADDR) as usize] as *mut u8,
                4,
            );
            buf.copy_from_slice(b"ping");
        }
        push_desc(
            &mut ram,
            base,
            0,
            payload_addr,
            4,
            VirtQueueDescFlag::empty(),
        );
        push_avail(&mut ram, base + 0x100, 0);

        device.notify(VIRTIO_CONSOLE_TRANSMIT_QUEUE as u32);
        assert!(worker.process_one());
        assert_eq!(used_index(&mut ram, base + 0x200), 1);
        assert_ne!(device.isr().load(Ordering::Acquire) & 1, 0);

        // --- receive queue: host -> guest ---
        let base = 0x8000_4000u64;
        setup_queue(&mut device, &mut ram, VIRTIO_CONSOLE_RECEIVE_QUEUE, base);
        let sink_addr = 0x8000_5000u64;
        push_desc(
            &mut ram,
            base,
            0,
            sink_addr,
            11,
            VirtQueueDescFlag::VIRTQ_DESC_F_WRITE,
        );
        push_avail(&mut ram, base + 0x100, 0);

        device.notify(VIRTIO_CONSOLE_RECEIVE_QUEUE as u32);
        assert!(worker.process_one());
        assert_eq!(used_index(&mut ram, base + 0x200), 1);

        let received = unsafe {
            slice::from_raw_parts(
                &ram[(sink_addr - ram_config::BASE_ADDR) as usize] as *const u8,
                11,
            )
        };
        assert_eq!(received, b"hello guest");
    }
}
