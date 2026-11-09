#pragma once

#include <stdint.h>

/*
 * 自定义指令 padd8 —— custom-0 编码空间
 *
 *   funct7 = 0b0000100
 *   funct3 = 0b000
 *   opcode = 0b0001011 (0x0B)
 *
 * 语义：把 rs1 / rs2 各切成若干个 8 位无符号 lane（RV64: 8 个，RV32: 4 个），
 *       对应 lane 相加，结果按 2^8 取模后拼回 rd（lane 之间不会互相进位）。
 *
 * 对应 HERE 侧定义见 data/instr_dict_custom.json 的 "padd8" 条目，
 * 执行见 src/isa/riscv/instruction/exec_mapping.rs。
 */
static inline uint64_t padd8(uint64_t lhs, uint64_t rhs) {
    uint64_t rd;
    __asm__ volatile(".insn r 0x0b, 0, 4, %0, %1, %2" : "=r"(rd) : "r"(lhs), "r"(rhs));
    return rd;
}
