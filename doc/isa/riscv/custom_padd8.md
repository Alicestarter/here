# 自定义指令 `padd8`（Chapter 0x02 作业）

> 课程任务：<https://heu-cs-dphd.github.io/projects/emu-exp-2026/ch2/>
> 一句话：在 custom-0 编码空间新增一条 SIMD 指令 `padd8`，走通「JSON 定义 → 译码 → 执行 → Rust 单测 → C 侧 `.insn` 封装 → 裸机程序」全链路。

## 1. 指令语义

`padd8 rd, rs1, rs2`：把 `rs1`、`rs2` 各切成若干个 8 位无符号 lane（RV64 下 8 个，RV32 下 4 个），
对应 lane 相加，结果按 $2^8$ 取模后拼回 `rd`。lane 之间不产生进位，等价于 SIMD 的 `vpaddb`。

```text
for i in 0..(XLEN/8):
    rd.byte[i] = (rs1.byte[i] + rs2.byte[i]) mod 256
```

等价 C 写法：

```c
uint64_t padd8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += 8) /* XLEN=64 */
        r |= (uint64_t)((((a >> i) & 0xff) + ((b >> i) & 0xff)) & 0xff) << i;
    return r;
}
```

## 2. 编码

选 custom-0（opcode `0b0001011`），R 型：

| 位域 | 宽度 | 值 |
|---|---|---|
| `funct7` | 7 | `0000100` |
| `rs2` | 5 | 变量 |
| `rs1` | 5 | 变量 |
| `funct3` | 3 | `000` |
| `rd` | 5 | 变量 |
| `opcode` | 7 | `0001011` (0x0B) |

- `match = 0x0800000b`（funct7 << 25 | opcode）
- `mask  = 0xfe00707f`

> **为什么不用 `funct7=0000000`？** 上游模板指令 `my_instr0_r` 已经占用了
> custom-0 的 `funct7=0000000 + funct3=000`（`mask = 0xfe00707f`）。
> HERE 的 decoder 对同 mask 的指令按顺序匹配，两条同 key 的指令会互相遮蔽，
> 所以新指令必须错开 `funct7`。

## 3. 改动清单

| 位置 | 内容 |
|---|---|
| `data/instr_dict_custom.json` | 新增 `padd8` 条目（`variable_fields = [rd, rs1, rs2]` → 自动判定为 R 型） |
| `src/isa/riscv/instruction/exec_mapping.rs` | 新增 `RiscvInstr::PADD8` 执行臂，`log::info!` 打印寄存器与结果，尾部走 `normal_exec` 更新 `pc` 与 `minstret` |
| `src/isa/riscv/instruction/exec_mapping.rs` | 新增 `packed_add_u8()`：逐 lane 相加取模的实现 |
| `src/isa/riscv/cpu_test.rs` | 新增 `test_custom_padd8`（3 组用例：无进位 / 每 lane 回绕 / 区分整体 64 位加法） |
| `test_resources/include/custom_ops.h` | `.insn` 内嵌汇编封装 API：`static inline uint64_t padd8(uint64_t, uint64_t)` |
| `test_resources/src/main.c` | `custom_instr_demo()`：调用 `padd8` 并校验结果，失败走 `fail()` |

`build.rs` 无需改动：`rv_custom0` 扩展名与 `data/instr_dict_custom.json` 路径都已在
`#[cfg(feature = "custom-instr")]` 下注册，新增条目会自动生成 `RiscvInstr::PADD8`
与 `TABLE_RVCUSTOM0`。

## 4. 验证

```bash
# ① 编译（必须带 feature，否则自定义指令表不参与构建）
cargo build --features custom-instr

# ② Rust 单测
cargo test --features custom-instr test_custom_padd8

# ③ 裸机程序：重新编译测试程序
cd test_resources && make ARCH=riscv64 && cd ..

# ④ 跑演示程序（需要 TTY，非终端环境用 script 包一层）
script -q /dev/null ./target/debug/here --loglevel info ./test_resources/bin/main.elf
```

期望输出（guest 侧 UART）：

```text
[INFO] custom padd8: 0x0102030405060708 + 0x0101010101010101 = 0x0203040506070809
[INFO] custom padd8 wrap: 0xffffffffffffffff + 0x0101010101010101 = 0x0000000000000000
[INFO] custom padd8 passed
```

模拟器侧日志（`logs/emulator_rCURRENT.log`）：

```text
[00:00.009][INFO ] [CustomInstr] PADD8: x15 = x15(0x102030405060708) + x14(0x101010101010101) = 0x203040506070809
```

反汇编可看到编码落在 `.text` 里（`0x08e7878b` = funct7 `0000100` / rs2 `x15` / rs1 `x14` / funct3 `000` / rd `x15` / opcode `0x0b`）：

```text
80001020:   08e7878b    .insn   4, 0x08e7878b
```

## 5. C 侧 API

```c
#include "custom_ops.h"

uint64_t r = padd8(0x0102030405060708ULL, 0x0101010101010101ULL); // 0x0203040506070809
```

GAS 的 `.insn r` 语法为 `.insn r opcode, funct3, funct7, rd, rs1, rs2`，
因此封装写作 `.insn r 0x0b, 0, 4, %0, %1, %2`。

## 6. 提交

```bash
git add data/instr_dict_custom.json src/isa/riscv/instruction/exec_mapping.rs \
        src/isa/riscv/cpu_test.rs test_resources/include/custom_ops.h \
        test_resources/src/main.c doc/isa/riscv/custom_padd8.md
git commit -m "feat(ch2): add custom padd8 SIMD instruction"
git tag ch2自定义指令
git push origin master --tags
```

---

# Chapter 0x03：后端执行架构与自定义计算单元

## 1. 从「执行臂内联」到「计算单元 + 路由」

Chapter 0x02 把 lane 加法直接写进了 `exec_mapping.rs` 的执行臂里（闭包内联）。
Chapter 0x03 把它下沉为一等公民的**计算单元**：

| 层 | 文件 | 职责 |
| --- | --- | --- |
| 译码层 | `data/instr_dict_custom.json` | 指令字 → `RiscvInstr::PADD8` / `RiscvInstr::PADDUS8` |
| 路由层 | `src/isa/riscv/instruction/exec_mapping.rs` | `=> exec_arith::<ExecPadd8>`，只做分派 |
| 计算层 | `src/isa/riscv/instruction/exec_function.rs` | `impl ExecTrait<Result<WordType, Exception>>`，纯粹的 lane 运算 |
| 框架层 | `exec_arith` / `normal_exec` | 取操作数 → 写 rd → `pc += 4` → `Minstret += 1` |

这样做的收益：

- 计算逻辑与「指令长什么样」解耦，`ExecPadd8` 可以被 R 型、I 型甚至向量指令复用；
- `exec_arith` 统一负责 `rd` 写入与 PC 推进，异常时（`?`）提前返回，不会污染 CPU 状态；
- `RVCPU::execute` 在分派返回后无条件执行 `reg_file[0] = 0`，因此 `rd = x0` 的写入天然被丢弃。

## 2. 计算单元

```rust
pub(in super::super) struct ExecPadd8;

impl ExecTrait<Result<WordType, Exception>> for ExecPadd8 {
    fn exec(a: WordType, b: WordType) -> Result<WordType, Exception> {
        let lhs = a.to_le_bytes();
        let rhs = b.to_le_bytes();
        let mut lanes = [0u8; PACKED_U8_LANES];
        for (idx, lane) in lanes.iter_mut().enumerate() {
            *lane = lhs[idx].wrapping_add(rhs[idx]); // 进位链在 lane 边界被切断
        }
        Ok(WordType::from_le_bytes(lanes))
    }
}
```

`ExecPaddus8` 的唯一区别是把 `wrapping_add` 换成 `saturating_add`（溢出钳位 255）。

## 3. 路由

```rust
#[cfg(feature = "custom-instr")]
RiscvInstr::PADD8 => exec_arith::<ExecPadd8>,
#[cfg(feature = "custom-instr")]
RiscvInstr::PADDUS8 => exec_arith::<ExecPaddus8>,
```

## 4. 验证

Rust 侧（`src/isa/riscv/cpu_test.rs`）：

```bash
cargo test --features custom-instr --lib custom
```

- `test_custom_padd8_backend`：常规值 / 全 0 / 全 1 / 低 lane 进位隔离 / 中 lane 进位隔离
- `test_custom_padd8_rd_zero_is_discarded`：`rd = x0` 结果被丢弃，`x0` 恒为 0
- `test_custom_paddus8_backend`：饱和（0xff+0xff=0xff）与非饱和（0x7f+0x01=0x80）分支
- `test_custom_padd8_pipeline_step`：`addi / addi / padd8 / padd8` 四条指令连续 step，检查 `x3=15`、`x4=30`、`pc = BASE_ADDR + 16`

C 侧（`test_resources/src/main.c`）：

```bash
cargo run --features custom-instr -- ./test_resources/bin/main.elf
```

`custom_golden_demo()` 用 `golden_padd8` / `golden_paddus8` 两个软件参考模型，
对 10 组测试向量（常规值、全 0、全 1、低/中 lane 进位隔离、饱和边界、随机混合）
逐条比对硬件执行结果，打印 `[PASS]/[FAIL]` 并统计通过数。
