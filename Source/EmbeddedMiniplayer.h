#pragma once

// Automatisch generiert aus ThirdParty/miniplayer/tools/dasm.exe und
// ThirdParty/miniplayer/player.s. Eingebettet, damit die MIDI->SID-Umwandlung
// auch dann funktioniert, wenn die App an einem Ort installiert ist, an dem der
// ThirdParty-Quellbaum nicht neben der .exe liegt (siehe MidiToSidCoreEngine::
// locateMiniplayerFiles() in MidiToSidCore.h).
namespace EmbeddedMiniplayer
{
    extern const unsigned char dasmExeData[];
    extern const int dasmExeDataSize;
    extern const unsigned char playerSData[];
    extern const int playerSDataSize;
}
