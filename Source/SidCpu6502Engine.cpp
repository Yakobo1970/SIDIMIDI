#include "SidCpu6502Engine.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>

//==============================================================================
// SidCpu6502
//==============================================================================

SidCpu6502::SidCpu6502()
{
    reset();
}

void SidCpu6502::reset()
{
    std::fill(ram.begin(), ram.end(), 0x00);
    regA = 0;
    regX = 0;
    regY = 0;
    regSP = 0xFF;
    regPC = 0;

    flagC = false;
    flagZ = false;
    flagI = true;
    flagD = false;
    flagB = false;
    flagV = false;
    flagN = false;
}

void SidCpu6502::setWriteCallback(WriteCallback cb)
{
    onWrite = cb;
}

void SidCpu6502::setReadCallback(ReadCallback cb)
{
    onRead = cb;
}

uint8_t SidCpu6502::readMem(uint16_t addr) const
{
    if (onRead)
    {
        uint8_t val = ram[addr];
        if (onRead(addr, val))
            return val;
    }
    return ram[addr];
}

void SidCpu6502::writeMem(uint16_t addr, uint8_t val)
{
    ram[addr] = val;
#ifdef SID_DEBUG
    // std::cout << "[writeMem] addr=0x" << std::hex << addr << " val=0x" << (int)val << "\n";
#endif
    if (onWrite) onWrite(addr, val);
}

void SidCpu6502::pushStack(uint8_t val)
{
    ram[0x0100 + regSP] = val;
    regSP--;
}

uint8_t SidCpu6502::popStack()
{
    regSP++;
    return ram[0x0100 + regSP];
}

void SidCpu6502::updateZeroAndNegativeFlags(uint8_t val)
{
    flagZ = (val == 0);
    flagN = (val & 0x80) != 0;
}

uint8_t SidCpu6502::fetch8()
{
    return readMem(regPC++);
}

uint16_t SidCpu6502::fetch16()
{
    uint8_t low = fetch8();
    uint8_t high = fetch8();
    return (static_cast<uint16_t>(high) << 8) | low;
}

uint16_t SidCpu6502::getIndirectXAddr()
{
    uint8_t zp = fetch8();
    uint8_t ptr = (zp + regX) & 0xFF;
    return readMem(ptr) | (static_cast<uint16_t>(readMem((ptr + 1) & 0xFF)) << 8);
}

uint16_t SidCpu6502::getIndirectYAddr()
{
    uint8_t  zp = fetch8();
    uint16_t base = readMem(zp) | (static_cast<uint16_t>(readMem((zp + 1) & 0xFF)) << 8);
    return base + regY;
}

void SidCpu6502::doADC(uint8_t val)
{
    uint16_t sum = static_cast<uint16_t>(regA) + static_cast<uint16_t>(val) + (flagC ? 1 : 0);
    flagC = sum > 0xFF;
    flagV = (~(regA ^ val) & (regA ^ sum) & 0x80) != 0;
    regA = static_cast<uint8_t>(sum & 0xFF);
    updateZeroAndNegativeFlags(regA);
}

void SidCpu6502::doSBC(uint8_t val)
{
    uint16_t diff = static_cast<uint16_t>(regA) - static_cast<uint16_t>(val) - (flagC ? 0 : 1);
    flagC = diff <= 0xFF;
    flagV = ((regA ^ val) & (regA ^ diff) & 0x80) != 0;
    regA = static_cast<uint8_t>(diff & 0xFF);
    updateZeroAndNegativeFlags(regA);
}

uint8_t SidCpu6502::doASL(uint8_t val)
{
    flagC = (val & 0x80) != 0;
    uint8_t res = static_cast<uint8_t>((val << 1) & 0xFF);
    updateZeroAndNegativeFlags(res);
    return res;
}

uint8_t SidCpu6502::doLSR(uint8_t val)
{
    flagC = (val & 0x01) != 0;
    uint8_t res = static_cast<uint8_t>(val >> 1);
    updateZeroAndNegativeFlags(res);
    return res;
}

uint8_t SidCpu6502::doROL(uint8_t val)
{
    bool    oldC = flagC;
    flagC = (val & 0x80) != 0;
    uint8_t res = static_cast<uint8_t>((val << 1) | (oldC ? 1 : 0));
    updateZeroAndNegativeFlags(res);
    return res;
}

uint8_t SidCpu6502::doROR(uint8_t val)
{
    bool    oldC = flagC;
    flagC = (val & 0x01) != 0;
    uint8_t res = static_cast<uint8_t>((val >> 1) | (oldC ? 0x80 : 0));
    updateZeroAndNegativeFlags(res);
    return res;
}

void SidCpu6502::doBIT(uint8_t val)
{
    flagZ = (regA & val) == 0;
    flagN = (val & 0x80) != 0;
    flagV = (val & 0x40) != 0;
}

int SidCpu6502::executeNextInstruction()
{
    uint8_t opcode = fetch8();

    switch (opcode)
    {
        // NOP
    case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: return 2;
    case 0x04: case 0x14: case 0x34: case 0x44: case 0x54: case 0x64: case 0x74:
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: case 0xD4: case 0xF4:
        fetch8(); return 3;
    case 0x0C: case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
        fetch16(); return 4;

        // LDA
    case 0xA9: regA = fetch8(); updateZeroAndNegativeFlags(regA); return 2;
    case 0xA5: regA = readMem(fetch8()); updateZeroAndNegativeFlags(regA); return 3;
    case 0xB5: regA = readMem((fetch8() + regX) & 0xFF); updateZeroAndNegativeFlags(regA); return 4;
    case 0xAD: regA = readMem(fetch16()); updateZeroAndNegativeFlags(regA); return 4;
    case 0xBD: regA = readMem(fetch16() + regX); updateZeroAndNegativeFlags(regA); return 4;
    case 0xB9: regA = readMem(fetch16() + regY); updateZeroAndNegativeFlags(regA); return 4;
    case 0xA1: regA = readMem(getIndirectXAddr()); updateZeroAndNegativeFlags(regA); return 6;
    case 0xB1: regA = readMem(getIndirectYAddr()); updateZeroAndNegativeFlags(regA); return 5;

        // STA
    case 0x85: writeMem(fetch8(), regA); return 3;
    case 0x95: writeMem((fetch8() + regX) & 0xFF, regA); return 4;
    case 0x8D: writeMem(fetch16(), regA); return 4;
    case 0x9D: writeMem(fetch16() + regX, regA); return 5;
    case 0x99: writeMem(fetch16() + regY, regA); return 5;
    case 0x81: writeMem(getIndirectXAddr(), regA); return 6;
    case 0x91: writeMem(getIndirectYAddr(), regA); return 6;

        // LDX
    case 0xA2: regX = fetch8(); updateZeroAndNegativeFlags(regX); return 2;
    case 0xA6: regX = readMem(fetch8()); updateZeroAndNegativeFlags(regX); return 3;
    case 0xB6: regX = readMem((fetch8() + regY) & 0xFF); updateZeroAndNegativeFlags(regX); return 4;
    case 0xAE: regX = readMem(fetch16()); updateZeroAndNegativeFlags(regX); return 4;
    case 0xBE: regX = readMem(fetch16() + regY); updateZeroAndNegativeFlags(regX); return 4;

        // STX
    case 0x86: writeMem(fetch8(), regX); return 3;
    case 0x96: writeMem((fetch8() + regY) & 0xFF, regX); return 4;
    case 0x8E: writeMem(fetch16(), regX); return 4;

        // LDY
    case 0xA0: regY = fetch8(); updateZeroAndNegativeFlags(regY); return 2;
    case 0xA4: regY = readMem(fetch8()); updateZeroAndNegativeFlags(regY); return 3;
    case 0xB4: regY = readMem((fetch8() + regX) & 0xFF); updateZeroAndNegativeFlags(regY); return 4;
    case 0xAC: regY = readMem(fetch16()); updateZeroAndNegativeFlags(regY); return 4;
    case 0xBC: regY = readMem(fetch16() + regX); updateZeroAndNegativeFlags(regY); return 4;

        // STY
    case 0x84: writeMem(fetch8(), regY); return 3;
    case 0x94: writeMem((fetch8() + regX) & 0xFF, regY); return 4;
    case 0x8C: writeMem(fetch16(), regY); return 4;

        // ADC
    case 0x69: doADC(fetch8()); return 2;
    case 0x65: doADC(readMem(fetch8())); return 3;
    case 0x75: doADC(readMem((fetch8() + regX) & 0xFF)); return 4;
    case 0x6D: doADC(readMem(fetch16())); return 4;
    case 0x7D: doADC(readMem(fetch16() + regX)); return 4;
    case 0x79: doADC(readMem(fetch16() + regY)); return 4;
    case 0x61: doADC(readMem(getIndirectXAddr())); return 6;
    case 0x71: doADC(readMem(getIndirectYAddr())); return 5;

        // SBC
    case 0xE9: doSBC(fetch8()); return 2;
    case 0xE5: doSBC(readMem(fetch8())); return 3;
    case 0xF5: doSBC(readMem((fetch8() + regX) & 0xFF)); return 4;
    case 0xED: doSBC(readMem(fetch16())); return 4;
    case 0xFD: doSBC(readMem(fetch16() + regX)); return 4;
    case 0xF9: doSBC(readMem(fetch16() + regY)); return 4;
    case 0xE1: doSBC(readMem(getIndirectXAddr())); return 6;
    case 0xF1: doSBC(readMem(getIndirectYAddr())); return 5;

        // ASL
    case 0x0A: regA = doASL(regA); return 2;
    case 0x06: { uint8_t a = fetch8(); writeMem(a, doASL(readMem(a))); return 5; }
    case 0x16: { uint8_t a = (fetch8() + regX) & 0xFF; writeMem(a, doASL(readMem(a))); return 6; }
    case 0x0E: { uint16_t a = fetch16(); writeMem(a, doASL(readMem(a))); return 6; }
    case 0x1E: { uint16_t a = fetch16() + regX; writeMem(a, doASL(readMem(a))); return 7; }

             // LSR
    case 0x4A: regA = doLSR(regA); return 2;
    case 0x46: { uint8_t a = fetch8(); writeMem(a, doLSR(readMem(a))); return 5; }
    case 0x56: { uint8_t a = (fetch8() + regX) & 0xFF; writeMem(a, doLSR(readMem(a))); return 6; }
    case 0x4E: { uint16_t a = fetch16(); writeMem(a, doLSR(readMem(a))); return 6; }
    case 0x5E: { uint16_t a = fetch16() + regX; writeMem(a, doLSR(readMem(a))); return 7; }

             // ROL
    case 0x2A: regA = doROL(regA); return 2;
    case 0x26: { uint8_t a = fetch8(); writeMem(a, doROL(readMem(a))); return 5; }
    case 0x36: { uint8_t a = (fetch8() + regX) & 0xFF; writeMem(a, doROL(readMem(a))); return 6; }
    case 0x2E: { uint16_t a = fetch16(); writeMem(a, doROL(readMem(a))); return 6; }
    case 0x3E: { uint16_t a = fetch16() + regX; writeMem(a, doROL(readMem(a))); return 7; }

             // ROR
    case 0x6A: regA = doROR(regA); return 2;
    case 0x66: { uint8_t a = fetch8(); writeMem(a, doROR(readMem(a))); return 5; }
    case 0x76: { uint8_t a = (fetch8() + regX) & 0xFF; writeMem(a, doROR(readMem(a))); return 6; }
    case 0x6E: { uint16_t a = fetch16(); writeMem(a, doROR(readMem(a))); return 6; }
    case 0x7E: { uint16_t a = fetch16() + regX; writeMem(a, doROR(readMem(a))); return 7; }

             // BIT
    case 0x24: doBIT(readMem(fetch8())); return 3;
    case 0x2C: doBIT(readMem(fetch16())); return 4;

        // Transfers
    case 0xAA: regX = regA; updateZeroAndNegativeFlags(regX); return 2;
    case 0x8A: regA = regX; updateZeroAndNegativeFlags(regA); return 2;
    case 0xA8: regY = regA; updateZeroAndNegativeFlags(regY); return 2;
    case 0x98: regA = regY; updateZeroAndNegativeFlags(regA); return 2;
    case 0xBA: regX = regSP; updateZeroAndNegativeFlags(regX); return 2;
    case 0x9A: regSP = regX; return 2;

        // Stack Operations
    case 0x48: pushStack(regA); return 3;
    case 0x68: regA = popStack(); updateZeroAndNegativeFlags(regA); return 4;
    case 0x08: { // PHP
        uint8_t p = (flagN ? 0x80 : 0) | (flagV ? 0x40 : 0) | 0x20 | 0x10 |
            (flagD ? 0x08 : 0) | (flagI ? 0x04 : 0) | (flagZ ? 0x02 : 0) | (flagC ? 0x01 : 0);
        pushStack(p);
        return 3;
    }
    case 0x28: { // PLP
        uint8_t p = popStack();
        flagN = (p & 0x80) != 0;
        flagV = (p & 0x40) != 0;
        flagD = (p & 0x08) != 0;
        flagI = (p & 0x04) != 0;
        flagZ = (p & 0x02) != 0;
        flagC = (p & 0x01) != 0;
        return 4;
    }

             // JSR / RTS / RTI / JMP
    case 0x20: {
        uint16_t target = fetch16();
        uint16_t ret = regPC - 1;
        pushStack(static_cast<uint8_t>((ret >> 8) & 0xFF));
        pushStack(static_cast<uint8_t>(ret & 0xFF));
        regPC = target;
        return 6;
    }
    case 0x60: { // RTS
        uint8_t low = popStack();
        uint8_t high = popStack();
        regPC = (static_cast<uint16_t>(high) << 8) | low;
        regPC++;
        return 6;
    }
    case 0x40: { // RTI
        uint8_t p = popStack();
        flagN = (p & 0x80) != 0;
        flagV = (p & 0x40) != 0;
        flagD = (p & 0x08) != 0;
        flagI = (p & 0x04) != 0;
        flagZ = (p & 0x02) != 0;
        flagC = (p & 0x01) != 0;
        uint8_t low = popStack();
        uint8_t high = popStack();
        regPC = (static_cast<uint16_t>(high) << 8) | low;
        return 6;
    }
    case 0x4C: regPC = fetch16(); return 3;
    case 0x6C: {
        uint16_t ptr = fetch16();
        uint8_t  low = readMem(ptr);
        uint8_t  high = readMem((ptr & 0xFF00) | ((ptr + 1) & 0xFF));
        regPC = (static_cast<uint16_t>(high) << 8) | low;
        return 5;
    }

             // CMP / CPX / CPY
    case 0xC9: compare(regA, fetch8()); return 2;
    case 0xC5: compare(regA, readMem(fetch8())); return 3;
    case 0xD5: compare(regA, readMem((fetch8() + regX) & 0xFF)); return 4;
    case 0xCD: compare(regA, readMem(fetch16())); return 4;
    case 0xDD: compare(regA, readMem(fetch16() + regX)); return 4;
    case 0xD9: compare(regA, readMem(fetch16() + regY)); return 4;
    case 0xC1: compare(regA, readMem(getIndirectXAddr())); return 6;
    case 0xD1: compare(regA, readMem(getIndirectYAddr())); return 5;

    case 0xE0: compare(regX, fetch8()); return 2;
    case 0xE4: compare(regX, readMem(fetch8())); return 3;
    case 0xEC: compare(regX, readMem(fetch16())); return 4;

    case 0xC0: compare(regY, fetch8()); return 2;
    case 0xC4: compare(regY, readMem(fetch8())); return 3;
    case 0xCC: compare(regY, readMem(fetch16())); return 4;

        // AND / ORA / EOR
    case 0x29: regA &= fetch8(); updateZeroAndNegativeFlags(regA); return 2;
    case 0x25: regA &= readMem(fetch8()); updateZeroAndNegativeFlags(regA); return 3;
    case 0x35: regA &= readMem((fetch8() + regX) & 0xFF); updateZeroAndNegativeFlags(regA); return 4;
    case 0x2D: regA &= readMem(fetch16()); updateZeroAndNegativeFlags(regA); return 4;
    case 0x3D: regA &= readMem(fetch16() + regX); updateZeroAndNegativeFlags(regA); return 4;
    case 0x39: regA &= readMem(fetch16() + regY); updateZeroAndNegativeFlags(regA); return 4;
    case 0x21: regA &= readMem(getIndirectXAddr()); updateZeroAndNegativeFlags(regA); return 6;
    case 0x31: regA &= readMem(getIndirectYAddr()); updateZeroAndNegativeFlags(regA); return 5;

    case 0x09: regA |= fetch8(); updateZeroAndNegativeFlags(regA); return 2;
    case 0x05: regA |= readMem(fetch8()); updateZeroAndNegativeFlags(regA); return 3;
    case 0x15: regA |= readMem((fetch8() + regX) & 0xFF); updateZeroAndNegativeFlags(regA); return 4;
    case 0x0D: regA |= readMem(fetch16()); updateZeroAndNegativeFlags(regA); return 4;
    case 0x1D: regA |= readMem(fetch16() + regX); updateZeroAndNegativeFlags(regA); return 4;
    case 0x19: regA |= readMem(fetch16() + regY); updateZeroAndNegativeFlags(regA); return 4;
    case 0x01: regA |= readMem(getIndirectXAddr()); updateZeroAndNegativeFlags(regA); return 6;
    case 0x11: regA |= readMem(getIndirectYAddr()); updateZeroAndNegativeFlags(regA); return 5;

    case 0x49: regA ^= fetch8(); updateZeroAndNegativeFlags(regA); return 2;
    case 0x45: regA ^= readMem(fetch8()); updateZeroAndNegativeFlags(regA); return 3;
    case 0x55: regA ^= readMem((fetch8() + regX) & 0xFF); updateZeroAndNegativeFlags(regA); return 4;
    case 0x4D: regA ^= readMem(fetch16()); updateZeroAndNegativeFlags(regA); return 4;
    case 0x5D: regA ^= readMem(fetch16() + regX); updateZeroAndNegativeFlags(regA); return 4;
    case 0x59: regA ^= readMem(fetch16() + regY); updateZeroAndNegativeFlags(regA); return 4;
    case 0x41: regA ^= readMem(getIndirectXAddr()); updateZeroAndNegativeFlags(regA); return 6;
    case 0x51: regA ^= readMem(getIndirectYAddr()); updateZeroAndNegativeFlags(regA); return 5;

        // Branches
    case 0xF0: branchIf(flagZ); return 2;
    case 0xD0: branchIf(!flagZ); return 2;
    case 0x90: branchIf(!flagC); return 2;
    case 0xB0: branchIf(flagC); return 2;
    case 0x10: branchIf(!flagN); return 2;
    case 0x30: branchIf(flagN); return 2;
    case 0x50: branchIf(!flagV); return 2;
    case 0x70: branchIf(flagV); return 2;

        // INC / DEC / INX / DEX / INY / DEY
    case 0xE6: { uint8_t a = fetch8(); uint8_t v = readMem(a) + 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 5; }
    case 0xF6: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = readMem(a) + 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 6; }
    case 0xC6: { uint8_t a = fetch8(); uint8_t v = readMem(a) - 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 5; }
    case 0xD6: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = readMem(a) - 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 6; }
    case 0xEE: { uint16_t a = fetch16(); uint8_t v = readMem(a) + 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 6; }
    case 0xFE: { uint16_t a = fetch16() + regX; uint8_t v = readMem(a) + 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 7; }
    case 0xCE: { uint16_t a = fetch16(); uint8_t v = readMem(a) - 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 6; }
    case 0xDE: { uint16_t a = fetch16() + regX; uint8_t v = readMem(a) - 1; writeMem(a, v); updateZeroAndNegativeFlags(v); return 7; }
    case 0xE8: regX++; updateZeroAndNegativeFlags(regX); return 2;
    case 0xCA: regX--; updateZeroAndNegativeFlags(regX); return 2;
    case 0xC8: regY++; updateZeroAndNegativeFlags(regY); return 2;
    case 0x88: regY--; updateZeroAndNegativeFlags(regY); return 2;

        // Flags
    case 0x18: flagC = false; return 2;
    case 0x38: flagC = true; return 2;
    case 0x58: flagI = false; return 2;
    case 0x78: flagI = true; return 2;
    case 0xD8: flagD = false; return 2;
    case 0xF8: flagD = true; return 2;
    case 0xB8: flagV = false; return 2;

        // LAX
    case 0xA7: { uint8_t v = readMem(fetch8()); regA = v; regX = v; updateZeroAndNegativeFlags(v); return 3; }
    case 0xB7: { uint8_t v = readMem((fetch8() + regY) & 0xFF); regA = v; regX = v; updateZeroAndNegativeFlags(v); return 4; }
    case 0xAF: { uint8_t v = readMem(fetch16()); regA = v; regX = v; updateZeroAndNegativeFlags(v); return 4; }
    case 0xBF: { uint8_t v = readMem(fetch16() + regY); regA = v; regX = v; updateZeroAndNegativeFlags(v); return 4; }
    case 0xA3: { uint8_t v = readMem(getIndirectXAddr()); regA = v; regX = v; updateZeroAndNegativeFlags(v); return 6; }
    case 0xB3: { uint8_t v = readMem(getIndirectYAddr()); regA = v; regX = v; updateZeroAndNegativeFlags(v); return 5; }

             // SAX
    case 0x87: writeMem(fetch8(), regA & regX); return 3;
    case 0x97: writeMem((fetch8() + regY) & 0xFF, regA & regX); return 4;
    case 0x8F: writeMem(fetch16(), regA & regX); return 4;
    case 0x83: writeMem(getIndirectXAddr(), regA & regX); return 6;

        // DCP
    case 0xC7: { uint8_t a = fetch8(); uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 5; }
    case 0xD7: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 6; }
    case 0xCF: { uint16_t a = fetch16(); uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 6; }
    case 0xDF: { uint16_t a = fetch16() + regX; uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 7; }
    case 0xDB: { uint16_t a = fetch16() + regY; uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 7; }
    case 0xC3: { uint16_t a = getIndirectXAddr(); uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 8; }
    case 0xD3: { uint16_t a = getIndirectYAddr(); uint8_t v = readMem(a) - 1; writeMem(a, v); compare(regA, v); return 8; }

             // ISB / ISC
    case 0xE7: { uint8_t a = fetch8(); uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 5; }
    case 0xF7: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 6; }
    case 0xEF: { uint16_t a = fetch16(); uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 6; }
    case 0xFF: { uint16_t a = fetch16() + regX; uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 7; }
    case 0xFB: { uint16_t a = fetch16() + regY; uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 7; }
    case 0xE3: { uint16_t a = getIndirectXAddr(); uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 8; }
    case 0xF3: { uint16_t a = getIndirectYAddr(); uint8_t v = readMem(a) + 1; writeMem(a, v); doSBC(v); return 8; }

             // SLO
    case 0x07: { uint8_t a = fetch8(); uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 5; }
    case 0x17: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 6; }
    case 0x0F: { uint16_t a = fetch16(); uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 6; }
    case 0x1F: { uint16_t a = fetch16() + regX; uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 7; }
    case 0x1B: { uint16_t a = fetch16() + regY; uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 7; }
    case 0x03: { uint16_t a = getIndirectXAddr(); uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 8; }
    case 0x13: { uint16_t a = getIndirectYAddr(); uint8_t v = doASL(readMem(a)); writeMem(a, v); regA |= v; updateZeroAndNegativeFlags(regA); return 8; }

             // RLA
    case 0x27: { uint8_t a = fetch8(); uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 5; }
    case 0x37: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 6; }
    case 0x2F: { uint16_t a = fetch16(); uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 6; }
    case 0x3F: { uint16_t a = fetch16() + regX; uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 7; }
    case 0x3B: { uint16_t a = fetch16() + regY; uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 7; }
    case 0x23: { uint16_t a = getIndirectXAddr(); uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 8; }
    case 0x33: { uint16_t a = getIndirectYAddr(); uint8_t v = doROL(readMem(a)); writeMem(a, v); regA &= v; updateZeroAndNegativeFlags(regA); return 8; }

             // SRE (LSR + EOR)
    case 0x47: { uint8_t a = fetch8(); uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 5; }
    case 0x57: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 6; }
    case 0x4F: { uint16_t a = fetch16(); uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 6; }
    case 0x5F: { uint16_t a = fetch16() + regX; uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 7; }
    case 0x5B: { uint16_t a = fetch16() + regY; uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 7; }
    case 0x43: { uint16_t a = getIndirectXAddr(); uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 8; }
    case 0x53: { uint16_t a = getIndirectYAddr(); uint8_t v = doLSR(readMem(a)); writeMem(a, v); regA ^= v; updateZeroAndNegativeFlags(regA); return 8; }

             // RRA (ROR + ADC)
    case 0x67: { uint8_t a = fetch8(); uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 5; }
    case 0x77: { uint8_t a = (fetch8() + regX) & 0xFF; uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 6; }
    case 0x6F: { uint16_t a = fetch16(); uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 6; }
    case 0x7F: { uint16_t a = fetch16() + regX; uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 7; }
    case 0x7B: { uint16_t a = fetch16() + regY; uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 7; }
    case 0x63: { uint16_t a = getIndirectXAddr(); uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 8; }
    case 0x73: { uint16_t a = getIndirectYAddr(); uint8_t v = doROR(readMem(a)); writeMem(a, v); doADC(v); return 8; }

             // ANC / ALR / ARR / SBX / SBC(dup) - stabile Immediate-Varianten
    case 0x0B: case 0x2B: // ANC #imm
        regA &= fetch8(); updateZeroAndNegativeFlags(regA); flagC = flagN; return 2;
    case 0x4B: // ALR/ASR #imm
        regA &= fetch8(); regA = doLSR(regA); return 2;
    case 0x6B: { // ARR #imm
        regA &= fetch8();
        regA = doROR(regA);
        flagC = (regA & 0x40) != 0;
        flagV = (((regA >> 6) ^ (regA >> 5)) & 0x01) != 0;
        return 2;
    }
    case 0xCB: { // SBX/AXS #imm
        uint8_t t = regA & regX;
        uint8_t imm = fetch8();
        flagC = t >= imm;
        regX = static_cast<uint8_t>(t - imm);
        updateZeroAndNegativeFlags(regX);
        return 2;
    }
    case 0xEB: doSBC(fetch8()); return 2; // SBC #imm (Duplikat von 0xE9)

    case 0x00: { // BRK - liest ein (verworfenes) Padding-Byte, sonst Bytestrom-Versatz
        fetch8();
        return 7;
    }

    default:
#ifdef SID_DEBUG
        std::cerr << "[6502] UNBEKANNTER OPCODE: 0x" << std::hex << std::uppercase
            << (int)opcode << " bei PC=0x" << (regPC - 1) << std::dec
            << " -- Bytestrom vermutlich ab hier verschoben!\n";
#endif
        return 2;
    }
}

void SidCpu6502::branchIf(bool condition)
{
    int8_t offset = static_cast<int8_t>(fetch8());
    if (condition)
        regPC += offset;
}

void SidCpu6502::compare(uint8_t regVal, uint8_t operand)
{
    uint16_t result = static_cast<uint16_t>(regVal) - static_cast<uint16_t>(operand);
    flagC = regVal >= operand;
    flagZ = (result & 0xFF) == 0;
    flagN = (result & 0x80) != 0;
}

//==============================================================================
// SidCpU6502Engine
//==============================================================================

std::vector<SidCpU6502Engine::RegEvent> SidCpU6502Engine::traceSidExecution(
    const uint8_t* codeData, size_t codeSize,
    uint16_t loadAddr, uint16_t initAddrIn,
    uint16_t playAddr, uint16_t subsong,
    uint32_t totalFrames)
{
    TraceOptions o;
    o.maxSeconds = 4.0;
    return traceSidExecutionEx (codeData, codeSize, loadAddr, initAddrIn, playAddr, subsong, totalFrames, o).events;
}

SidCpU6502Engine::TraceResult SidCpU6502Engine::traceSidExecutionEx(
    const uint8_t* codeData, size_t codeSize,
    uint16_t loadAddr, uint16_t initAddrIn,
    uint16_t playAddr, uint16_t subsong,
    uint32_t totalFrames, const TraceOptions& options)
{
    TraceResult result;
    std::vector<RegEvent>& events = result.events;
    SidCpu6502 cpu;

    for (size_t i = 0; i < codeSize && (loadAddr + i) < 65536; ++i)
        cpu.ram[loadAddr + i] = codeData[i];

    uint32_t currentFrame = 0;
    uint16_t dynamicIrqVec = 0;
    uint16_t hardwareIrqVec = 0; // Manche Player installieren ihren Handler direkt am 6502-Hardwarevektor $FFFE/$FFFF statt am KERNAL-Softwarevektor $0314/$0315

    auto read16 = [&](uint16_t addr)->uint16_t {
        return static_cast<uint16_t>(cpu.ram[addr]) | (static_cast<uint16_t>(cpu.ram[(addr + 1) & 0xFFFF]) << 8);
        };

    uint8_t lastSidRegs[0x20] = { 0 };

    // --- Simulierte Hardware-Zeitgeber ---
    // Viele Player (u.a. Ben Daglish) warten aktiv auf die Rasterzeile ($D011/$D012)
    // oder auf ein CIA-Timer-A-Underflow-Flag ($DC0D), um sample-genau zu synchronisieren.
    // Ohne diese Simulation bleibt die CPU in solchen Polling-Schleifen für immer stecken.
    const uint32_t cyclesPerLine = SidCpU6502Engine::kCyclesPerLine;
    const uint32_t linesPerFrame = SidCpU6502Engine::kLinesPerFrame;
    uint32_t rasterCyclesInFrame = 0;
    uint16_t rasterLine = 0;

    uint16_t ciaTimerALatch = 0xFFFF;
    bool     ciaLatchWritten = false;
    uint16_t ciaTimerACounter = 0xFFFF;
    bool     ciaTimerARunning = false;
    bool     ciaTimerAContinuous = true;
    bool     ciaTimerAIrqPending = false;

    // CIA2 ($DD00-$DD0F) - eigener Timer-A, unabhängig von CIA1. Auf einem
    // real gebooteten C64 initialisiert das KERNAL diesen Timer als frei
    // laufenden Zähler (u.a. für den RS232-Treiber). Manche Player (u.a. Rob
    // Hubbard) schreiben nie selbst ein Latch, sondern nutzen das sich
    // ständig ändernde Low-Byte als billige Pseudozufallsquelle (Schleife
    // "lies $DD04, wiederhole bis Wert < Schwelle"). Da unser Emulator kein
    // KERNAL bootet, muss dieser Timer deshalb - anders als CIA1, den Songs
    // immer explizit selbst konfigurieren - von Anfang an mit einem KLEINEN
    // Default-Latch laufen: Ein realistischer KERNAL-Wert ist uns unbekannt,
    // aber 0xFFFF würde bei jedem force-load (Bit4 von $DD0E) einen Durchlauf
    // von bis zu ~65000 Zyklen erzwingen - das sprengt das Zyklenbudget einer
    // einzigen Frame-Berechnung um ein Vielfaches und lässt die Schleife wie
    // eine Endlosschleife wirken, obwohl sie auf echter Hardware nur wenige
    // Mikrosekunden braucht.
    uint16_t cia2TimerALatch = 0x00FF;
    uint16_t cia2TimerACounter = 0x00FF;
    bool     cia2TimerARunning = true;
    bool     cia2TimerAContinuous = true;
    bool     cia2TimerAIrqPending = false;

    auto advanceTiming = [&](int cyclesUsed) {
        if (cyclesUsed <= 0) return;

        rasterCyclesInFrame += (uint32_t)cyclesUsed;
        rasterLine = (uint16_t)((rasterCyclesInFrame / cyclesPerLine) % linesPerFrame);

        if (ciaTimerARunning)
        {
            if (ciaTimerACounter <= (uint16_t)cyclesUsed)
            {
                uint16_t remainder = (uint16_t)(cyclesUsed - ciaTimerACounter);
                ciaTimerAIrqPending = true;
                if (ciaTimerAContinuous && ciaTimerALatch > 0)
                    ciaTimerACounter = (ciaTimerALatch > remainder) ? (ciaTimerALatch - remainder) : ciaTimerALatch;
                else
                    ciaTimerARunning = false;
            }
            else
            {
                ciaTimerACounter -= (uint16_t)cyclesUsed;
            }
        }

        if (cia2TimerARunning)
        {
            if (cia2TimerACounter <= (uint16_t)cyclesUsed)
            {
                uint16_t remainder = (uint16_t)(cyclesUsed - cia2TimerACounter);
                cia2TimerAIrqPending = true;
                if (cia2TimerAContinuous && cia2TimerALatch > 0)
                    cia2TimerACounter = (cia2TimerALatch > remainder) ? (cia2TimerALatch - remainder) : cia2TimerALatch;
                else
                    cia2TimerARunning = false;
            }
            else
            {
                cia2TimerACounter -= (uint16_t)cyclesUsed;
            }
        }
        };

    // --- Diagnose: welche I/O-Register fasst diese Routine WIRKLICH an? ---
    // (einmalig pro Adresse geloggt, damit die Konsole nicht flutet)
    std::array<bool, 0x1000> ioReadSeen{};
    std::array<bool, 0x1000> ioWriteSeen{};

    // Manche Player (u.a. "Game Over" von Martin Galway) kopieren/entpacken sich während
    // Init selbst in den Bereich $E000-$FFFF, statt komplett innerhalb des geladenen Songcode-
    // Bereichs zu bleiben - eine auf echter Hardware übliche Technik, um BASIC/KERNAL-ROM
    // auszublenden und den freiwerdenden Platz zu nutzen. Ohne diese Erkennung stuft die
    // "kernelChained"-Heuristik weiter unten so einen Sprung fälschlich als "verkettet sich
    // an echtes KERNAL-ROM" ein und bricht die Play-Routine ab, bevor sie überhaupt etwas
    // Hörbares tut. Ein Byte gilt hier als "echter, selbst geschriebener Songcode", sobald der
    // Song es zur Laufzeit selbst beschrieben hat.
    std::array<bool, 0x2000> highRamWritten{};

    cpu.setWriteCallback([&](uint16_t addr, uint8_t val) {
        if (addr >= 0xE000)
            highRamWritten[addr - 0xE000] = true;

        if (addr >= 0xD000 && addr <= 0xDFFF)
        {
            uint16_t idx = addr - 0xD000;
            if (!ioWriteSeen[idx])
            {
                ioWriteSeen[idx] = true;
#ifdef SID_DEBUG
                std::cerr << "[IO-WRITE erstmalig] $" << std::hex << std::uppercase << addr
                    << " = 0x" << (int)val << " bei PC=0x" << cpu.regPC
                    << std::dec << " (Frame " << currentFrame << ")\n";
#endif
            }
        }

        if (addr >= 0xD400 && addr <= 0xD41F)
        {
            uint8_t regIdx = static_cast<uint8_t>(addr - 0xD400);

            if (lastSidRegs[regIdx] != val)
            {
                lastSidRegs[regIdx] = val;

                RegEvent evt;
                evt.frameIndex = currentFrame;
                evt.reg = regIdx;
                evt.value = val;
                evt.cycleInFrame = rasterCyclesInFrame;
                events.push_back(evt);
            }
        }

        if (addr == 0x0314)
            dynamicIrqVec = (dynamicIrqVec & 0xFF00) | val;
        else if (addr == 0x0315)
            dynamicIrqVec = (dynamicIrqVec & 0x00FF) | (static_cast<uint16_t>(val) << 8);
        else if (addr == 0xFFFE)
            hardwareIrqVec = (hardwareIrqVec & 0xFF00) | val;
        else if (addr == 0xFFFF)
            hardwareIrqVec = (hardwareIrqVec & 0x00FF) | (static_cast<uint16_t>(val) << 8);

        // CIA1 Timer A: Latch-Bytes
        if (addr == 0xDC04)
        {
            ciaTimerALatch = (ciaTimerALatch & 0xFF00) | val;
            ciaLatchWritten = true;
        }
        else if (addr == 0xDC05)
        {
            ciaTimerALatch = (ciaTimerALatch & 0x00FF) | (static_cast<uint16_t>(val) << 8);
            ciaLatchWritten = true;
        }
        // CIA1 Control Register A: bit0 = Start/Stop, bit3 = One-Shot, bit4 = Force Load
        else if (addr == 0xDC0E)
        {
            if (val & 0x10) ciaTimerACounter = ciaTimerALatch;
            ciaTimerAContinuous = (val & 0x08) == 0;
            ciaTimerARunning = (val & 0x01) != 0;
        }

        // CIA2 Timer A: Latch-Bytes
        if (addr == 0xDD04)
            cia2TimerALatch = (cia2TimerALatch & 0xFF00) | val;
        else if (addr == 0xDD05)
            cia2TimerALatch = (cia2TimerALatch & 0x00FF) | (static_cast<uint16_t>(val) << 8);
        // CIA2 Control Register A: bit0 = Start/Stop, bit3 = One-Shot, bit4 = Force Load
        else if (addr == 0xDD0E)
        {
            if (val & 0x10) cia2TimerACounter = cia2TimerALatch;
            cia2TimerAContinuous = (val & 0x08) == 0;
            cia2TimerARunning = (val & 0x01) != 0;
        }
        });

    cpu.setReadCallback([&](uint16_t addr, uint8_t& val) -> bool {
        if (addr >= 0xD000 && addr <= 0xDFFF)
        {
            uint16_t idx = addr - 0xD000;
            if (!ioReadSeen[idx])
            {
                ioReadSeen[idx] = true;
#ifdef SID_DEBUG
                std::cerr << "[IO-READ erstmalig] $" << std::hex << std::uppercase << addr
                    << " bei PC=0x" << cpu.regPC << std::dec << " (Frame " << currentFrame << ")\n";
#endif
            }
        }

        if (addr == 0xD012) { val = (uint8_t)(rasterLine & 0xFF); return true; }
        if (addr == 0xD011) { val = (cpu.ram[0xD011] & 0x7F) | ((rasterLine & 0x100) ? 0x80 : 0); return true; }
        if (addr == 0xDC04) { val = (uint8_t)(ciaTimerACounter & 0xFF); return true; }
        if (addr == 0xDC05) { val = (uint8_t)(ciaTimerACounter >> 8); return true; }
        if (addr == 0xDC0D)
        {
            val = ciaTimerAIrqPending ? 0x81 : 0x00;
            ciaTimerAIrqPending = false; // echte Hardware löscht das Flag beim Lesen
            return true;
        }
        // CIA1 Port A/B ($DC00/$DC01): Joystick/Tastatur-Leitungen sind aktiv-Low
        // und liegen im Leerlauf (nichts gedrückt) auf 0xFF. Ohne diese Simulation
        // liefert unser genulltes RAM 0x00 ("alles gleichzeitig gedrückt"), worauf
        // manche Init-Routinen in eine Warteschleife laufen, die nie endet.
        if (addr == 0xDC00 || addr == 0xDC01) { val = 0xFF; return true; }
        if (addr == 0xDD04) { val = (uint8_t)(cia2TimerACounter & 0xFF); return true; }
        if (addr == 0xDD05) { val = (uint8_t)(cia2TimerACounter >> 8); return true; }
        if (addr == 0xDD0D)
        {
            val = cia2TimerAIrqPending ? 0x81 : 0x00;
            cia2TimerAIrqPending = false;
            return true;
        }
        return false;
        });

    uint16_t initAddr = (initAddrIn != 0) ? initAddrIn : loadAddr;

    // Trampolin-Adresse für Init/Play-Sentinels: Standardmäßig 0x0FFE-0x1000,
    // wie zuvor fest verdrahtet. Liegt der geladene Songcode selbst in diesem
    // Bereich (z.B. loadAddr <= 0x1000), würde das feste Schreiben hier den
    // echten Code überschreiben und die Emulation korrumpieren - deshalb wird
    // bei Überlappung auf eine alternative, garantiert freie Adresse ausgewichen.
    auto overlapsCode = [&](uint16_t addr, int len) {
        uint32_t a0 = addr, a1 = a0 + (uint32_t)len;
        uint32_t c0 = loadAddr, c1 = c0 + (uint32_t)codeSize;
        return a0 < c1 && c0 < a1;
        };

    uint16_t trampolineBase = 0x0FFE;
    for (uint16_t candidate : { (uint16_t)0x0FFE, (uint16_t)0xFFFA, (uint16_t)0x02F0 })
    {
        if (!overlapsCode(candidate, 3))
        {
            trampolineBase = candidate;
            break;
        }
    }

    uint16_t initDummyReturn = trampolineBase;
    cpu.ram[trampolineBase] = 0x4C; // JMP <trampolineBase> (Endlosschleife als Sentinel)
    cpu.ram[trampolineBase + 1] = (uint8_t)(trampolineBase & 0xFF);
    cpu.ram[trampolineBase + 2] = (uint8_t)(trampolineBase >> 8);

    cpu.regA = static_cast<uint8_t>(subsong > 0 ? subsong - 1 : 0);
    cpu.regX = 0;
    cpu.regY = 0;

    uint8_t savedInitSP = cpu.regSP;
    // Die SID-Init-Routine wird wie ein normales Unterprogramm aufgerufen
    // (entspricht einem JSR) und kehrt daher mit RTS zurück, das die vom
    // Stack geholte Adresse automatisch um 1 erhöht - deshalb hier
    // "Zieladresse - 1" pushen, damit RTS exakt auf initDummyReturn landet.
    cpu.pushStack(static_cast<uint8_t>(((initDummyReturn - 1) >> 8) & 0xFF));
    cpu.pushStack(static_cast<uint8_t>((initDummyReturn - 1) & 0xFF));
    cpu.regPC = initAddr;

    int maxInitCycles = 500000;
    while (cpu.regPC > 0x0001 && cpu.regPC != initDummyReturn && maxInitCycles-- > 0)
    {
        advanceTiming(cpu.executeNextInstruction());
    }

    cpu.regSP = savedInitSP;

    // --- PLAY-ADRESSE ERMITTELN ---
    // Reihenfolge: zuerst der während Init tatsächlich beschriebene
    // KERNAL-Softwarevektor ($0314/$0315), sonst dessen aktueller RAM-Inhalt,
    // sonst der 6502-Hardwarevektor ($FFFE/$FFFF) - manche Player (u.a. einige
    // Martin-Galway-Tunes) installieren ihren Handler direkt dort, ohne den
    // Softwarevektor überhaupt anzufassen.
    uint16_t detectedPlayAddr = dynamicIrqVec != 0 ? dynamicIrqVec
        : (read16(0x0314) != 0 ? read16(0x0314) : hardwareIrqVec);

    // Zwei grundsätzlich verschiedene Aufrufkonventionen:
    //  - Hat der Song während Init einen eigenen IRQ-Handler via $0314/$0315
    //    installiert, wird dieser wie ein echter Hardware-Interrupt betreten
    //    und kehrt mit RTI zurück (RTI stellt zusätzlich die Flags wieder her
    //    und erhöht die vom Stack geholte Adresse NICHT).
    //  - Andernfalls wird die im Header angegebene playAddress als normales
    //    Unterprogramm aufgerufen (wie ein JSR) und kehrt mit RTS zurück
    //    (RTS erhöht die vom Stack geholte Adresse automatisch um 1).
    // Die beiden Fälle brauchen unterschiedlich viele/andere Stack-Bytes beim
    // simulierten "Aufruf" - eine falsche Annahme lässt die Rückkehr um ein
    // Byte danebenlanden und die CPU in unbeschriebenen Speicher laufen.
    //
    // Ein erkannter Vektor gilt als "echter" installierter Handler, wenn er
    // innerhalb des geladenen Songcodes liegt (der Song kann schließlich nur
    // dort selbst Code untergebracht haben) - vorher stand hier eine fest
    // verdrahtete Untergrenze von 0x1000, die Songs mit Ladeadresse unterhalb
    // von 0x1000 (z.B. "Game Over" von Martin Galway, Ladeadresse 0x0F00)
    // fälschlich als nicht-interruptgetrieben einstufte: der Vektor zeigte
    // korrekt auf die eigene Player-Routine bei z.B. 0x0F50, lag damit aber
    // unterhalb der harten 0x1000-Schwelle und wurde verworfen - die Folge war
    // ein stummer Song, weil stattdessen wiederholt init() statt der echten
    // Player-Routine aufgerufen wurde.
    bool playIsInterruptDriven = (detectedPlayAddr >= loadAddr &&
        (size_t)(detectedPlayAddr - loadAddr) < codeSize);
    uint16_t effectivePlayAddr = playIsInterruptDriven ? detectedPlayAddr : playAddr;

    // Fallback falls Play-Adresse im Header explizit angegeben ist und der Vektor leer/ungültig blieb
    if (effectivePlayAddr == 0 && playAddr != 0)
    {
        effectivePlayAddr = playAddr;
        playIsInterruptDriven = false;
    }


    {
        const double clockHz = options.ntsc ? 1022727.0 : 985248.0;
        const double videoHz = options.ntsc ? 59.8261 : 50.1245;
        if (options.ciaSpeed)
        {
            const double latch = ciaLatchWritten ? (ciaTimerALatch == 0 ? 65536.0 : (double) ciaTimerALatch) : (options.ntsc ? 17045.0 : 16421.0);
            result.framesPerSecond = clockHz / std::max (1.0, latch);
        }
        else
            result.framesPerSecond = videoHz;
    }

    if (effectivePlayAddr == 0)
    {
        result.noPlay = true;
        return result;
    }

    uint16_t dummyReturn = (uint16_t)(trampolineBase + 1);
    cpu.ram[dummyReturn] = playIsInterruptDriven ? 0x40 : 0x60; // RTI für simulierte Hardware-Interrupts, sonst RTS für direkten Unterprogrammaufruf

    auto startTime = std::chrono::steady_clock::now();

    for (currentFrame = 0; currentFrame < totalFrames; ++currentFrame)
    {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if ((double) elapsedMs > options.maxSeconds * 1000.0)
        {
            result.timedOut = true;
            break;
        }
        if (options.cancel && (currentFrame & 15) == 0 && options.cancel())
        {
            result.cancelled = true;
            break;
        }

        uint8_t frameSP = cpu.regSP;

        if (playIsInterruptDriven)
        {
            // Die Routine wurde vom Song selbst als IRQ-Handler installiert
            // und kehrt daher mit RTI zurück, das die vom Stack geholte
            // Adresse NICHT erhöht (und zusätzlich die Flags restauriert) -
            // deshalb hier die exakte Zieladresse pushen (kein "-1").
            cpu.pushStack(static_cast<uint8_t>((dummyReturn >> 8) & 0xFF));
            cpu.pushStack(static_cast<uint8_t>(dummyReturn & 0xFF));

            uint8_t pReg = (cpu.flagN ? 0x80 : 0) | (cpu.flagV ? 0x40 : 0) | 0x20 | 0x10 |
                (cpu.flagD ? 0x08 : 0) | 0x04 | (cpu.flagZ ? 0x02 : 0) | (cpu.flagC ? 0x01 : 0);
            cpu.pushStack(pReg);
            cpu.flagI = true;
        }
        else
        {
            // Die im Header angegebene playAddress wird wie ein normales
            // Unterprogramm aufgerufen (JSR-Semantik) und kehrt mit RTS
            // zurück, das die vom Stack geholte Adresse automatisch um 1
            // erhöht - deshalb "Zieladresse - 1" pushen und kein P-Byte,
            // da RTS (anders als RTI) die Flags nicht vom Stack restauriert.
            cpu.pushStack(static_cast<uint8_t>(((dummyReturn - 1) >> 8) & 0xFF));
            cpu.pushStack(static_cast<uint8_t>((dummyReturn - 1) & 0xFF));
        }

        cpu.regPC = effectivePlayAddr;
        rasterCyclesInFrame = 0; // Rasterzeile beginnt bei jedem simulierten Frame-Aufruf wieder bei 0

        int maxPlayCycles = 100000;
        uint16_t lastPC = 0xFFFF;
        int stuckCount = 0;
        bool kernelChained = false;

        while (cpu.regPC > 0x0001 && cpu.regPC != dummyReturn && maxPlayCycles-- > 0)
        {
            // Manche Player "verketten" sich am Ende ihrer IRQ-Routine bewusst
            // an die KERNAL-Standard-IRQ-Behandlung (i.d.R. rund um $EA31),
            // damit Jiffy-Clock/Tastaturabfrage weiterlaufen (u.a. Rob
            // Hubbard, David Whittaker). Da wir kein KERNAL-ROM einbinden, ist
            // ein Sprung in den ROM-Bereich außerhalb des geladenen Songcodes
            // gleichbedeutend mit "die eigentliche Frame-Arbeit ist fertig" -
            // sonst würde die CPU im genullten Pseudo-ROM in unbeschriebenen
            // Speicher weiterlaufen, bis sie zufällig auf dummyReturn trifft.
            if (cpu.regPC >= 0xE000 &&
                !(cpu.regPC >= loadAddr && (size_t)(cpu.regPC - loadAddr) < codeSize) &&
                !highRamWritten[cpu.regPC - 0xE000])
            {
                kernelChained = true;
                break;
            }
            if (cpu.regPC == lastPC)
            {
                stuckCount++;
                if (stuckCount > 200) break;
            }
            else
            {
                lastPC = cpu.regPC;
                stuckCount = 0;
            }

            advanceTiming(cpu.executeNextInstruction());
        }

#ifdef SID_DEBUG
        if (cpu.regPC != dummyReturn && !kernelChained && currentFrame < 30)
        {
            std::string reason = (stuckCount > 200) ? "PC haengt fest" : "Zyklen-Limit erreicht (nie RTS/RTI erreicht)";
            std::cerr << "[FRAME ABBRUCH] Frame " << currentFrame << ": " << reason
                << " bei PC=0x" << std::hex << std::uppercase << cpu.regPC << std::dec << "\n";
        }
#endif

        cpu.regSP = frameSP;
    }

    result.frames = currentFrame;
    return result;
}