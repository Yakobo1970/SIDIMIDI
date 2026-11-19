// SIDiMIDI - state of the SID chip (3 voices + filter/volume block) rebuilt from register writes.
#pragma once
#include <cstdint>

//==============================================================================
// Control-Register-Bits: bit0=Gate, bit1=Sync, bit2=Ringmod, bit3=Test, bit4-7=Waveform.
struct SIDVoiceData
{
    uint16_t frequency = 0;
    uint16_t pulseWidth = 0;
    uint8_t  controlReg = 0;
    uint8_t  attackDecay = 0;
    uint8_t  sustainRel = 0;

    bool isGateActive() const { return (controlReg & 0x01) != 0; }
    bool isSyncActive() const { return (controlReg & 0x02) != 0; }
    bool isRingModActive() const { return (controlReg & 0x04) != 0; }
    bool isTestActive() const { return (controlReg & 0x08) != 0; }
    uint8_t getWaveform() const { return (controlReg >> 4) & 0x0F; }
    uint8_t getAttackNibble() const { return (attackDecay >> 4) & 0x0F; }
    uint8_t getDecayNibble() const { return attackDecay & 0x0F; }
    uint8_t getSustainNibble() const { return (sustainRel >> 4) & 0x0F; }
    uint8_t getReleaseNibble() const { return sustainRel & 0x0F; }
};

//==============================================================================
// Kompletter SID-Chip-Zustand (3 Voices + gemeinsamer Filter-/Lautstärkeblock),
// aus einem RegEvent-Strom rekonstruiert. Einzige Quelle der reg->Feld-Dekodierung,
// genutzt sowohl vom MIDI-Exporter als auch von der Live-SID-Vorschau, damit beide
// nicht auseinanderdriften.
struct SidChipState
{
    SIDVoiceData voices[3];

    // Gemeinsamer Filter-/Lautstärkeblock ($D415-$D418)
    uint16_t filterCutoff11 = 0;          // 11-bit: (FC_HI<<3)|(FC_LO&0x07)
    uint8_t  resonanceNibble = 0;         // High-Nibble von $D417
    uint8_t  routingMask = 0;             // Low-Nibble von $D417: bit0-2=Voice1-3 durch Filter geroutet, bit3=External
    uint8_t  filterModeBits = 0;          // Bits 4-6 von $D418 (LP/BP/HP, kombinierbar), hier auf 0-7 normalisiert
    bool     voice3Disconnected = false;  // Bit7 von $D418
    // Low-Nibble von $D418. Default 15 (volle Lautstärke) statt 0: reale Hardware wird vom
    // KERNAL beim Boot bereits auf Lautstärke gesetzt, was wir nicht emulieren - ein Tune, der
    // $D418 nie explizit anfasst, soll deshalb nicht standardmäßig stumm sein.
    uint8_t  masterVolumeNibble = 15;

    bool isVoiceRoutedThroughFilter(int voiceIdx) const { return (routingMask & (1 << voiceIdx)) != 0; }

    // Wendet ein SID-Register-Event (reg 0x00-0x18) auf den Zustand an.
    // Rückgabe: betroffener Voice-Index (0-2), oder -1 für den Filter-/Lautstärkeblock.
    int applyRegisterEvent(uint8_t reg, uint8_t value)
    {
        int voiceIdx = -1;
        if (reg <= 0x06) voiceIdx = 0;
        else if (reg >= 0x07 && reg <= 0x0D) voiceIdx = 1;
        else if (reg >= 0x0E && reg <= 0x14) voiceIdx = 2;

        if (voiceIdx >= 0)
        {
            auto& voice = voices[voiceIdx];
            int regOffset = reg - (voiceIdx * 7);
            switch (regOffset)
            {
            case 0: voice.frequency = (voice.frequency & 0xFF00) | value; break;
            case 1: voice.frequency = (voice.frequency & 0x00FF) | ((uint16_t)value << 8); break;
            case 2: voice.pulseWidth = (voice.pulseWidth & 0x0F00) | value; break;
            case 3: voice.pulseWidth = (voice.pulseWidth & 0x00FF) | (((uint16_t)value & 0x0F) << 8); break;
            case 4: voice.controlReg = value; break;
            case 5: voice.attackDecay = value; break;
            case 6: voice.sustainRel = value; break;
            default: break;
            }
            return voiceIdx;
        }

        switch (reg)
        {
        case 0x15: filterCutoff11 = (filterCutoff11 & 0xFFF8) | (value & 0x07); break;
        case 0x16: filterCutoff11 = (filterCutoff11 & 0x0007) | ((uint16_t)value << 3); break;
        case 0x17: resonanceNibble = (value >> 4) & 0x0F; routingMask = value & 0x0F; break;
        case 0x18:
            filterModeBits = (value >> 4) & 0x07;
            voice3Disconnected = (value & 0x80) != 0;
            masterVolumeNibble = value & 0x0F;
            break;
        default: break;
        }
        return -1;
    }
};

