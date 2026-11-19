#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <functional>

// 6502 CPU
class SidCpu6502
{
public:
    SidCpu6502();
    void reset();

    using WriteCallback = std::function<void(uint16_t addr, uint8_t val)>;
    void setWriteCallback(WriteCallback cb);

    // Liefert true, wenn val vom Callback überschrieben wurde (für simulierte
    // Hardware-Register wie Rasterzeile $D012 oder CIA-Timer $DC04-$DC0D)
    using ReadCallback = std::function<bool(uint16_t addr, uint8_t& val)>;
    void setReadCallback(ReadCallback cb);

    uint8_t  readMem(uint16_t addr) const;
    void     writeMem(uint16_t addr, uint8_t val);

    void     pushStack(uint8_t val);
    uint8_t  popStack();

    int      executeNextInstruction();

    std::array<uint8_t, 65536> ram;
    uint8_t  regA;
    uint8_t  regX;
    uint8_t  regY;
    uint8_t  regSP;
    uint16_t regPC;

    bool flagC;
    bool flagZ;
    bool flagI;
    bool flagD;
    bool flagB;
    bool flagV;
    bool flagN;

private:
    WriteCallback onWrite;
    ReadCallback  onRead;

    void     updateZeroAndNegativeFlags(uint8_t val);
    uint8_t  fetch8();
    uint16_t fetch16();
    uint16_t getIndirectXAddr();
    uint16_t getIndirectYAddr();

    void     doADC(uint8_t val);
    void     doSBC(uint8_t val);
    uint8_t  doASL(uint8_t val);
    uint8_t  doLSR(uint8_t val);
    uint8_t  doROL(uint8_t val);
    uint8_t  doROR(uint8_t val);
    void     doBIT(uint8_t val);

    void     branchIf(bool condition);
    void     compare(uint8_t regVal, uint8_t operand);
};

// Tracker Engine (umbenannt von SidTrackerEngine zu SidCpU6502Engine)
class SidCpU6502Engine
{
public:
    // PAL-Zeitbasis, auch für die Sub-Frame-Tick-Berechnung in SidToMidiCore.h
    static constexpr uint32_t kCyclesPerLine = 63;
    static constexpr uint32_t kLinesPerFrame = 312;
    static constexpr uint32_t kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;

    // Alias oder direktes Struct, damit RegEvent in der AudioPlayerEngine gefunden wird
    struct RegEvent
    {
        uint32_t frameIndex = 0;
        uint8_t  reg = 0;
        uint8_t  value = 0;
        // 6502-Zyklen seit Beginn dieses Frames zum Zeitpunkt des Schreibzugriffs
        // (0..kCyclesPerFrame-1). Erlaubt der MIDI-Konvertierung, mehrere echte
        // Schreibzugriffe auf dasselbe Register innerhalb eines Frames (Arpeggios,
        // Software-Digi-Samples, Multiplex-Tricks) zeitlich korrekt zu verteilen,
        // statt sie alle auf denselben Tick zu kollabieren.
        uint32_t cycleInFrame = 0;
    };

    static std::vector<RegEvent> traceSidExecution(
        const uint8_t* codeData, size_t codeSize,
        uint16_t loadAddr, uint16_t initAddrIn,
        uint16_t playAddr, uint16_t subsong,
        uint32_t totalFrames = 30000);

    // Full version: speed (video frame or CIA 1 timer), clock, cancel callback, result details.
    struct TraceOptions
    {
        bool ciaSpeed = false;          // the sub-tune's speed flag (SID header): play routine is called by the CIA 1 timer
        bool ntsc = false;              // NTSC machine (clock and default rate)
        double maxSeconds = 25.0;       // safety limit for the emulation time (wall clock)
        std::function<bool()> cancel;   // return true to abort
    };
    struct TraceResult
    {
        std::vector<RegEvent> events;
        double framesPerSecond = 50.125;  // how often the play routine is called per second
        uint32_t frames = 0;              // play calls emulated
        bool timedOut = false, cancelled = false, noPlay = false;
    };
    static TraceResult traceSidExecutionEx (
        const uint8_t* codeData, size_t codeSize,
        uint16_t loadAddr, uint16_t initAddrIn,
        uint16_t playAddr, uint16_t subsong,
        uint32_t totalFrames, const TraceOptions& options);
};