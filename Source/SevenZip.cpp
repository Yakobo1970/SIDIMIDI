#include "SevenZip.h"
#include "Ppmd7.h"
#include <array>
#include <cstring>
#include <memory>

namespace sm
{
namespace
{
// ================================================================================================ CRC32
const std::array<juce::uint32, 256>& crcTable()
{
    static const auto table = [] {
        std::array<juce::uint32, 256> t {};
        for (juce::uint32 i = 0; i < 256; ++i)
        {
            juce::uint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// ================================================================================================ LZMA / LZMA2 decoder
class LzmaDecoder
{
public:
    std::function<bool (const juce::uint8*, size_t)> sink;   // receives the decoded bytes in order; false = stop
    bool failed = false;

    explicit LzmaDecoder (juce::uint32 dictionarySize)
    {
        dictSize = std::max<juce::uint32> (dictionarySize, 4096);
        dict.assign (dictSize, 0);
    }

    void resetDictionary() { dictPos = 0; flushPos = 0; totalOut = 0; }

    void setProps (juce::uint8 d)
    {
        if (d >= 9 * 5 * 5) { failed = true; return; }
        lc = d % 9; d /= 9; lp = d % 5; pb = d / 5;
        literalProbs.assign ((size_t) 0x300 << (lc + lp), 1024);
    }

    void resetState()
    {
        const auto fill = [] (auto& v) { std::fill (v.begin(), v.end(), (juce::uint16) 1024); };
        fill (isMatch); fill (isRep); fill (isRepG0); fill (isRepG1); fill (isRepG2); fill (isRep0Long);
        for (auto& a : posSlot) fill (a);
        fill (posDecoders); fill (align);
        lenDec.reset(); repLenDec.reset();
        std::fill (literalProbs.begin(), literalProbs.end(), (juce::uint16) 1024);
        state = 0; rep0 = rep1 = rep2 = rep3 = 0;
    }

    // one range-coded block of exactly outSize bytes (LZMA2 chunk, or a whole 7z LZMA stream)
    bool decodeBlock (const juce::uint8* in, size_t inSize, juce::uint64 outSize, bool endMarkerAllowed)
    {
        if (inSize < 5 || in[0] != 0) { failed = true; return false; }
        rcIn = in + 5; rcEnd = in + inSize; rcRange = 0xFFFFFFFFu;
        rcCode = ((juce::uint32) in[1] << 24) | ((juce::uint32) in[2] << 16) | ((juce::uint32) in[3] << 8) | in[4];
        rcOverrun = false;
        juce::uint64 produced = 0;
        while (produced < outSize)
        {
            const juce::uint32 posState = (juce::uint32) (totalOut & ((1u << pb) - 1));
            if (bit (isMatch[(state << 4) + posState]) == 0)
            {
                // literal
                const juce::uint8 prev = totalOut > 0 ? dict[dictPos == 0 ? dictSize - 1 : dictPos - 1] : 0;
                const size_t litState = (((size_t) (totalOut & ((1u << lp) - 1))) << lc) + (size_t) (prev >> (8 - lc));
                juce::uint16* probs = &literalProbs[0x300 * litState];
                unsigned symbol = 1;
                if (state >= 7)
                {
                    unsigned matchByte = dict[dictPos >= (size_t) rep0 + 1 ? dictPos - rep0 - 1 : dictPos + dictSize - rep0 - 1];
                    do
                    {
                        const unsigned matchBit = (matchByte >> 7) & 1;
                        matchByte <<= 1;
                        const unsigned b = bit (probs[((1 + matchBit) << 8) + symbol]);
                        symbol = (symbol << 1) | b;
                        if (matchBit != b) break;
                    } while (symbol < 0x100);
                }
                while (symbol < 0x100) symbol = (symbol << 1) | bit (probs[symbol]);
                put ((juce::uint8) symbol);
                ++produced;
                state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            }
            else
            {
                juce::uint32 len;
                if (bit (isRep[state]) == 0)
                {
                    rep3 = rep2; rep2 = rep1; rep1 = rep0;
                    len = lenDec.decode (*this, posState);
                    state = state < 7 ? 7 : 10;
                    // distance
                    const juce::uint32 lenState = std::min<juce::uint32> (len, 3);
                    unsigned slot = 1;
                    for (int i = 0; i < 6; ++i) slot = (slot << 1) | bit (posSlot[lenState][slot]);
                    slot -= 64;
                    if (slot < 4) rep0 = slot;
                    else
                    {
                        const unsigned numDirect = (slot >> 1) - 1;
                        juce::uint32 dist = (2 | (slot & 1)) << numDirect;
                        if (slot < 14)
                        {
                            // reverse bit tree in posDecoders starting at dist - slot
                            const size_t base = dist - slot;
                            unsigned m = 1, sym = 0;
                            for (unsigned i = 0; i < numDirect; ++i)
                            {
                                const unsigned b = bit (posDecoders[base + m]);
                                m = (m << 1) + b;
                                sym |= b << i;
                            }
                            dist += sym;
                        }
                        else
                        {
                            dist += direct (numDirect - 4) << 4;
                            unsigned m = 1, sym = 0;
                            for (unsigned i = 0; i < 4; ++i)
                            {
                                const unsigned b = bit (align[m]);
                                m = (m << 1) + b;
                                sym |= b << i;
                            }
                            dist += sym;
                        }
                        rep0 = dist;
                    }
                    if (rep0 == 0xFFFFFFFFu)
                    {
                        if (endMarkerAllowed) return true;
                        failed = true; return false;
                    }
                }
                else
                {
                    if (totalOut == 0) { failed = true; return false; }
                    if (bit (isRepG0[state]) == 0)
                    {
                        if (bit (isRep0Long[(state << 4) + posState]) == 0)
                        {
                            state = state < 7 ? 9 : 11;
                            put (dict[dictPos >= (size_t) rep0 + 1 ? dictPos - rep0 - 1 : dictPos + dictSize - rep0 - 1]);
                            ++produced;
                            continue;
                        }
                    }
                    else
                    {
                        juce::uint32 dist;
                        if (bit (isRepG1[state]) == 0) dist = rep1;
                        else
                        {
                            if (bit (isRepG2[state]) == 0) dist = rep2;
                            else { dist = rep3; rep3 = rep2; }
                            rep2 = rep1;
                        }
                        rep1 = rep0;
                        rep0 = dist;
                    }
                    len = repLenDec.decode (*this, posState);
                    state = state < 7 ? 8 : 11;
                }
                len += 2;
                if ((juce::uint64) rep0 >= totalOut || rep0 >= dictSize) { failed = true; return false; }
                if (len > outSize - produced) len = (juce::uint32) (outSize - produced);   // a match must not run over the block end
                for (juce::uint32 i = 0; i < len; ++i)
                    put (dict[dictPos >= (size_t) rep0 + 1 ? dictPos - rep0 - 1 : dictPos + dictSize - rep0 - 1]);
                produced += len;
            }
            if (rcOverrun || failed) { failed = true; return false; }
        }
        return ! rcOverrun && flush();
    }

    // LZMA2: uncompressed chunk
    bool copyRaw (const juce::uint8* in, size_t n)
    {
        for (size_t i = 0; i < n; ++i) put (in[i]);
        return flush();
    }

    bool flush()
    {
        if (dictPos > flushPos)
        {
            if (sink && ! sink (dict.data() + flushPos, dictPos - flushPos)) { failed = true; return false; }
            flushPos = dictPos;
        }
        return ! failed;
    }

    juce::uint64 totalOut = 0;

private:
    // ---- range decoder
    juce::uint32 rcRange = 0, rcCode = 0;
    const juce::uint8* rcIn = nullptr;
    const juce::uint8* rcEnd = nullptr;
    bool rcOverrun = false;

    void normalise()
    {
        if (rcRange < (1u << 24))
        {
            rcRange <<= 8;
            if (rcIn < rcEnd) rcCode = (rcCode << 8) | *rcIn++;
            else { rcCode <<= 8; rcOverrun = true; }
        }
    }

    unsigned bit (juce::uint16& prob)
    {
        const juce::uint32 bound = (rcRange >> 11) * prob;
        unsigned result;
        if (rcCode < bound) { rcRange = bound; prob = (juce::uint16) (prob + ((2048 - prob) >> 5)); result = 0; }
        else { rcRange -= bound; rcCode -= bound; prob = (juce::uint16) (prob - (prob >> 5)); result = 1; }
        normalise();
        return result;
    }

    juce::uint32 direct (unsigned numBits)
    {
        juce::uint32 res = 0;
        for (unsigned i = 0; i < numBits; ++i)
        {
            rcRange >>= 1;
            rcCode -= rcRange;
            const juce::uint32 t = 0u - (rcCode >> 31);
            rcCode += rcRange & t;
            normalise();
            res = (res << 1) + t + 1;
        }
        return res;
    }

    struct LenDecoder
    {
        juce::uint16 choice = 1024, choice2 = 1024;
        std::array<std::array<juce::uint16, 8>, 16> low, mid;
        std::array<juce::uint16, 256> high;
        LenDecoder() { reset(); }
        void reset()
        {
            choice = choice2 = 1024;
            for (auto& a : low) a.fill (1024);
            for (auto& a : mid) a.fill (1024);
            high.fill (1024);
        }
        juce::uint32 decode (LzmaDecoder& d, juce::uint32 posState)
        {
            if (d.bit (choice) == 0) return tree (d, low[posState].data(), 3);
            if (d.bit (choice2) == 0) return 8 + tree (d, mid[posState].data(), 3);
            return 16 + tree (d, high.data(), 8);
        }
        static juce::uint32 tree (LzmaDecoder& d, juce::uint16* probs, unsigned bits)
        {
            unsigned m = 1;
            for (unsigned i = 0; i < bits; ++i) m = (m << 1) + d.bit (probs[m]);
            return m - (1u << bits);
        }
    };

    void put (juce::uint8 b)
    {
        if (dictPos == dictSize)
        {
            flush();
            dictPos = 0; flushPos = 0;
        }
        dict[dictPos++] = b;
        ++totalOut;
    }

    unsigned lc = 3, lp = 0, pb = 2;
    juce::uint32 state = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    std::vector<juce::uint16> literalProbs;
    std::array<juce::uint16, 12 << 4> isMatch, isRep0Long;
    std::array<juce::uint16, 12> isRep, isRepG0, isRepG1, isRepG2;
    std::array<std::array<juce::uint16, 64>, 4> posSlot;
    std::array<juce::uint16, 115> posDecoders;
    std::array<juce::uint16, 16> align;
    LenDecoder lenDec, repLenDec;
    std::vector<juce::uint8> dict;
    juce::uint32 dictSize = 0;
    size_t dictPos = 0, flushPos = 0;
};

// ================================================================================================ filters
void bcjX86Decode (juce::uint8* data, size_t size)
{
    static const juce::uint8 maskToAllowed[8] = { 1, 1, 1, 0, 1, 0, 0, 0 };
    static const juce::uint8 maskToBitNumber[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
    auto test = [] (juce::uint8 b) { return b == 0 || b == 0xFF; };
    if (size < 5) return;
    size_t bufferPos = 0, prevPosT = (size_t) 0 - 1;
    juce::uint32 prevMask = 0;
    const juce::uint32 ip = 5;
    for (;;)
    {
        juce::uint8* p = data + bufferPos;
        const juce::uint8* limit = data + size - 4;
        for (; p < limit; ++p) if ((*p & 0xFE) == 0xE8) break;
        bufferPos = (size_t) (p - data);
        if (p >= limit) break;
        prevPosT = bufferPos - prevPosT;
        if (prevPosT > 3) prevMask = 0;
        else
        {
            prevMask = (prevMask << ((int) prevPosT - 1)) & 0x7;
            if (prevMask != 0)
            {
                const juce::uint8 b = p[4 - maskToBitNumber[prevMask]];
                if (! maskToAllowed[prevMask] || test (b))
                {
                    prevPosT = bufferPos;
                    prevMask = ((prevMask << 1) & 0x7) | 1;
                    ++bufferPos;
                    continue;
                }
            }
        }
        prevPosT = bufferPos;
        if (test (p[4]))
        {
            juce::uint32 src = ((juce::uint32) p[4] << 24) | ((juce::uint32) p[3] << 16) | ((juce::uint32) p[2] << 8) | p[1];
            juce::uint32 dest;
            for (;;)
            {
                dest = src - (ip + (juce::uint32) bufferPos);
                if (prevMask == 0) break;
                const int index = maskToBitNumber[prevMask] * 8;
                const juce::uint8 b = (juce::uint8) (dest >> (24 - index));
                if (! test (b)) break;
                src = dest ^ ((1u << (32 - index)) - 1);
            }
            p[4] = (juce::uint8) (~(((dest >> 24) & 1) - 1));
            p[3] = (juce::uint8) (dest >> 16);
            p[2] = (juce::uint8) (dest >> 8);
            p[1] = (juce::uint8) dest;
            bufferPos += 5;
        }
        else
        {
            prevMask = ((prevMask << 1) & 0x7) | 1;
            ++bufferPos;
        }
    }
}

void deltaDecode (juce::uint8* data, size_t size, size_t distance)
{
    for (size_t i = distance; i < size; ++i) data[i] = (juce::uint8) (data[i] + data[i - distance]);
}

// ================================================================================================ 7z structures
enum : juce::uint8 { kEnd = 0, kHeader = 1, kArchiveProperties = 2, kAdditionalStreamsInfo = 3, kMainStreamsInfo = 4, kFilesInfo = 5,
                     kPackInfo = 6, kUnPackInfo = 7, kSubStreamsInfo = 8, kSize = 9, kCRC = 10, kFolder = 11, kCodersUnPackSize = 12,
                     kNumUnPackStream = 13, kEmptyStream = 14, kEmptyFile = 15, kAnti = 16, kNames = 17, kCTime = 18, kATime = 19,
                     kMTime = 20, kWinAttributes = 21, kComment = 22, kEncodedHeader = 23, kStartPos = 24, kDummy = 25 };

enum : juce::uint64 { mCopy = 0x00, mDelta = 0x03, mBcjOld = 0x04, mLzma2 = 0x21, mLzma = 0x030101, mPpmd = 0x030401, mBcj = 0x03030103, mBcj2 = 0x0303011B };

struct Coder { juce::uint64 method = 0; int numIn = 1, numOut = 1; std::vector<juce::uint8> props; };
struct Bind { int inIndex = 0, outIndex = 0; };
struct Folder
{
    std::vector<Coder> coders;
    std::vector<Bind> binds;
    std::vector<int> packed;                 // in-stream indices fed by pack streams
    std::vector<juce::uint64> unpackSizes;   // per out stream
    bool hasCrc = false;
    juce::uint32 crc = 0;
    int numIn = 0, numOut = 0;
    int firstPack = 0;
    int numSubstreams = 1;

    int mainOut() const
    {
        for (int o = 0; o < numOut; ++o)
        {
            bool bound = false;
            for (auto& b : binds) if (b.outIndex == o) bound = true;
            if (! bound) return o;
        }
        return -1;
    }
    juce::uint64 mainSize() const { const int o = mainOut(); return o >= 0 && o < (int) unpackSizes.size() ? unpackSizes[(size_t) o] : 0; }
};

struct StreamsInfo
{
    juce::uint64 packPos = 0;
    std::vector<juce::uint64> packSizes;
    std::vector<Folder> folders;
    std::vector<juce::uint64> subSizes;
    std::vector<bool> subHasCrc;
    std::vector<juce::uint32> subCrc;
};

struct Reader
{
    const juce::uint8* p = nullptr;
    size_t size = 0, pos = 0;
    bool bad = false;

    juce::uint8 byte() { if (pos >= size) { bad = true; return 0; } return p[pos++]; }
    juce::uint64 number()
    {
        const juce::uint8 first = byte();
        juce::uint8 mask = 0x80;
        juce::uint64 value = 0;
        for (int i = 0; i < 8; ++i)
        {
            if ((first & mask) == 0)
            {
                const juce::uint64 high = first & (mask - 1u);
                return value + (high << (8 * i));
            }
            value |= (juce::uint64) byte() << (8 * i);
            mask >>= 1;
        }
        return value;
    }
    juce::uint32 u32() { juce::uint32 v = 0; for (int i = 0; i < 4; ++i) v |= (juce::uint32) byte() << (8 * i); return v; }
    void skip (juce::uint64 n) { if (n > size - pos) { bad = true; pos = size; } else pos += (size_t) n; }
    int count (juce::uint64 limit = 100000000) { const auto n = number(); if (n > limit) { bad = true; return 0; } return (int) n; }
    std::vector<bool> bits (int n)
    {
        std::vector<bool> v ((size_t) n);
        juce::uint8 cur = 0, mask = 0;
        for (int i = 0; i < n; ++i)
        {
            if (mask == 0) { cur = byte(); mask = 0x80; }
            v[(size_t) i] = (cur & mask) != 0;
            mask >>= 1;
        }
        return v;
    }
    std::vector<bool> optionalBits (int n)   // "allDefined" byte, else a bit vector
    {
        const juce::uint8 all = byte();
        if (all != 0) return std::vector<bool> ((size_t) n, true);
        return bits (n);
    }
    void digests (int n, std::vector<bool>& defined, std::vector<juce::uint32>& crcs)
    {
        defined = optionalBits (n);
        crcs.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i) if (defined[(size_t) i]) crcs[(size_t) i] = u32();
    }
};

bool readFolder (Reader& r, Folder& f)
{
    const int numCoders = r.count (64);
    if (numCoders < 1 || r.bad) return false;
    for (int i = 0; i < numCoders; ++i)
    {
        Coder c;
        const juce::uint8 b = r.byte();
        const int idSize = b & 0xF;
        if (b & 0x80) return false;
        for (int k = 0; k < idSize; ++k) c.method = (c.method << 8) | r.byte();
        if (b & 0x10) { c.numIn = r.count (32); c.numOut = r.count (32); }
        if (b & 0x20) { const int n = r.count (4096); for (int k = 0; k < n; ++k) c.props.push_back (r.byte()); }
        f.numIn += c.numIn; f.numOut += c.numOut;
        f.coders.push_back (std::move (c));
    }
    const int numBind = f.numOut - 1;
    if (numBind < 0) return false;
    for (int i = 0; i < numBind; ++i) { Bind b; b.inIndex = r.count (64); b.outIndex = r.count (64); f.binds.push_back (b); }
    const int numPacked = f.numIn - numBind;
    if (numPacked < 1) return false;
    if (numPacked == 1)
    {
        for (int i = 0; i < f.numIn; ++i)
        {
            bool bound = false;
            for (auto& b : f.binds) if (b.inIndex == i) bound = true;
            if (! bound) { f.packed.push_back (i); break; }
        }
    }
    else
        for (int i = 0; i < numPacked; ++i) f.packed.push_back (r.count (64));
    return ! r.bad && f.packed.size() == (size_t) numPacked;
}

bool readStreamsInfo (Reader& r, StreamsInfo& s)
{
    juce::uint8 id = r.byte();
    if (id == kPackInfo)
    {
        s.packPos = r.number();
        const int n = r.count();
        for (;;)
        {
            id = r.byte();
            if (id == kEnd || r.bad) break;
            if (id == kSize) { s.packSizes.assign ((size_t) n, 0); for (auto& v : s.packSizes) v = r.number(); }
            else if (id == kCRC) { std::vector<bool> d; std::vector<juce::uint32> c; r.digests (n, d, c); }
            else return false;
        }
        id = r.byte();
    }
    if (id == kUnPackInfo)
    {
        if (r.byte() != kFolder) return false;
        const int nf = r.count();
        if (r.byte() != 0) return false;   // external
        s.folders.resize ((size_t) nf);
        int pack = 0;
        for (auto& f : s.folders)
        {
            if (! readFolder (r, f)) return false;
            f.firstPack = pack;
            pack += (int) f.packed.size();
        }
        if (r.byte() != kCodersUnPackSize) return false;
        for (auto& f : s.folders) { f.unpackSizes.assign ((size_t) f.numOut, 0); for (auto& v : f.unpackSizes) v = r.number(); }
        for (;;)
        {
            id = r.byte();
            if (id == kEnd || r.bad) break;
            if (id == kCRC)
            {
                std::vector<bool> d; std::vector<juce::uint32> c;
                r.digests (nf, d, c);
                for (int i = 0; i < nf; ++i) { s.folders[(size_t) i].hasCrc = d[(size_t) i]; s.folders[(size_t) i].crc = c[(size_t) i]; }
            }
            else return false;
        }
        id = r.byte();
    }
    // sub streams (default: one per folder)
    for (auto& f : s.folders) f.numSubstreams = 1;
    bool haveSizes = false, hadSub = false;
    if (id == kSubStreamsInfo)
    {
        hadSub = true;
        for (;;)
        {
            id = r.byte();
            if (id == kNumUnPackStream) { for (auto& f : s.folders) f.numSubstreams = r.count(); }
            else if (id == kSize)
            {
                haveSizes = true;
                for (auto& f : s.folders)
                {
                    if (f.numSubstreams == 0) continue;
                    juce::uint64 sum = 0;
                    for (int j = 0; j < f.numSubstreams - 1; ++j) { const auto v = r.number(); s.subSizes.push_back (v); sum += v; }
                    if (f.mainSize() < sum) return false;
                    s.subSizes.push_back (f.mainSize() - sum);
                }
            }
            else break;   // kCRC or kEnd of the sub streams info
            if (r.bad) return false;
        }
    }
    if (! haveSizes)
        for (auto& f : s.folders) if (f.numSubstreams == 1) s.subSizes.push_back (f.mainSize());
    // CRCs of the sub streams
    int total = 0, unknown = 0;
    for (auto& f : s.folders) { total += f.numSubstreams; if (! (f.numSubstreams == 1 && f.hasCrc)) unknown += f.numSubstreams; }
    s.subHasCrc.assign ((size_t) total, false);
    s.subCrc.assign ((size_t) total, 0);
    std::vector<bool> d; std::vector<juce::uint32> c;
    bool haveCrc = false;
    if (hadSub)
    {
        if (id == kCRC) { r.digests (unknown, d, c); haveCrc = true; id = r.byte(); }   // now: kEnd of the sub streams info
        if (id != kEnd) return false;
        id = r.byte();                                                                     // kEnd of the streams info
    }
    int idx = 0, k = 0;
    for (auto& f : s.folders)
        for (int j = 0; j < f.numSubstreams; ++j, ++idx)
        {
            if (f.numSubstreams == 1 && f.hasCrc) { s.subHasCrc[(size_t) idx] = true; s.subCrc[(size_t) idx] = f.crc; }
            else if (haveCrc && k < (int) d.size()) { s.subHasCrc[(size_t) idx] = d[(size_t) k]; s.subCrc[(size_t) idx] = c[(size_t) k]; ++k; }
        }
    return ! r.bad && id == kEnd && (int) s.subSizes.size() == total;
}

juce::String utf16ToString (const juce::uint8* p, size_t units)
{
    std::string out;
    for (size_t i = 0; i < units; ++i)
    {
        juce::uint32 c = (juce::uint32) (p[2 * i] | (p[2 * i + 1] << 8));
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units)
        {
            const juce::uint32 c2 = (juce::uint32) (p[2 * i + 2] | (p[2 * i + 3] << 8));
            if (c2 >= 0xDC00 && c2 < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); ++i; }
        }
        if (c < 0x80) out += (char) c;
        else if (c < 0x800) { out += (char) (0xC0 | (c >> 6)); out += (char) (0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char) (0xE0 | (c >> 12)); out += (char) (0x80 | ((c >> 6) & 0x3F)); out += (char) (0x80 | (c & 0x3F)); }
        else { out += (char) (0xF0 | (c >> 18)); out += (char) (0x80 | ((c >> 12) & 0x3F)); out += (char) (0x80 | ((c >> 6) & 0x3F)); out += (char) (0x80 | (c & 0x3F)); }
    }
    return juce::String::fromUTF8 (out.data(), (int) out.size());
}

// the archive in memory with its parsed directory
struct Archive
{
    juce::MemoryBlock data;
    StreamsInfo streams;
    std::vector<SevenZip::Entry> entries;
    std::vector<int> entrySub;   // per entry: index of the sub stream (-1 = none)

    const juce::uint8* bytes() const { return static_cast<const juce::uint8*> (data.getData()); }

    bool pack (const Folder& f, int packedIdx, const juce::uint8*& ptr, size_t& n) const
    {
        const int global = f.firstPack + packedIdx;
        if (global < 0 || global >= (int) streams.packSizes.size()) return false;
        juce::uint64 off = 32 + streams.packPos;
        for (int i = 0; i < global; ++i) off += streams.packSizes[(size_t) i];
        const auto sz = streams.packSizes[(size_t) global];
        if (off + sz > data.getSize()) return false;
        ptr = bytes() + off;
        n = (size_t) sz;
        return true;
    }
};

// ---- decode one folder; chunks of the main output stream go to sink
bool decodeCoderToSink (const Archive& a, const Folder& f, const Coder& c, const juce::uint8* in, size_t inSize, juce::uint64 outSize,
                        const std::function<bool (const juce::uint8*, size_t)>& sink, juce::String& error)
{
    if (c.method == mCopy)
    {
        if (inSize < outSize) { error = "Archiv abgeschnitten"; return false; }
        return sink (in, (size_t) outSize);
    }
    if (c.method == mLzma)
    {
        if (c.props.size() < 5) { error = "LZMA-Eigenschaften fehlen"; return false; }
        juce::uint32 dict = (juce::uint32) c.props[1] | ((juce::uint32) c.props[2] << 8) | ((juce::uint32) c.props[3] << 16) | ((juce::uint32) c.props[4] << 24);
        dict = (juce::uint32) std::min<juce::uint64> (dict, std::max<juce::uint64> (outSize, 4096));
        LzmaDecoder d (dict);
        d.sink = sink;
        d.setProps (c.props[0]);
        d.resetState();
        d.resetDictionary();
        if (! d.decodeBlock (in, inSize, outSize, true)) { error = "LZMA-Daten fehlerhaft"; return false; }
        return true;
    }
    if (c.method == mLzma2)
    {
        if (c.props.empty()) { error = "LZMA2-Eigenschaften fehlen"; return false; }
        const unsigned b = c.props[0];
        if (b > 40) { error = "LZMA2-Wörterbuch ungültig"; return false; }
        juce::uint64 dict = b == 40 ? 0xFFFFFFFFu : ((juce::uint64) (2 | (b & 1)) << (b / 2 + 11));
        dict = std::min<juce::uint64> (dict, std::max<juce::uint64> (outSize, 4096));
        LzmaDecoder d (dict > 0x7FFFFFFFu ? 0x7FFFFFFFu : (juce::uint32) dict);
        d.sink = sink;
        size_t pos = 0;
        bool needProps = true, needDict = true;
        for (;;)
        {
            if (pos >= inSize) { error = "LZMA2-Daten abgeschnitten"; return false; }
            const juce::uint8 ctrl = in[pos++];
            if (ctrl == 0) break;
            if (ctrl == 1 || ctrl == 2)
            {
                if (pos + 2 > inSize) { error = "LZMA2-Daten abgeschnitten"; return false; }
                const size_t n = (((size_t) in[pos] << 8) | in[pos + 1]) + 1;
                pos += 2;
                if (ctrl == 1) { d.resetDictionary(); needDict = false; }
                else if (needDict) { error = "LZMA2-Daten fehlerhaft"; return false; }
                if (pos + n > inSize) { error = "LZMA2-Daten abgeschnitten"; return false; }
                if (! d.copyRaw (in + pos, n)) { error = "Schreiben fehlgeschlagen"; return false; }
                pos += n;
            }
            else if (ctrl >= 0x80)
            {
                if (pos + 4 > inSize) { error = "LZMA2-Daten abgeschnitten"; return false; }
                const size_t unpack = (((size_t) (ctrl & 0x1F)) << 16) + (((size_t) in[pos] << 8) | in[pos + 1]) + 1;
                const size_t packSz = (((size_t) in[pos + 2] << 8) | in[pos + 3]) + 1;
                pos += 4;
                const int mode = (ctrl >> 5) & 3;
                if (mode == 3) { d.resetDictionary(); needDict = false; }
                else if (needDict) { error = "LZMA2-Daten fehlerhaft"; return false; }
                if (mode >= 2) { if (pos >= inSize) { error = "LZMA2-Daten abgeschnitten"; return false; } d.setProps (in[pos++]); needProps = false; }
                else if (needProps) { error = "LZMA2-Daten fehlerhaft"; return false; }
                if (mode >= 1) d.resetState();
                if (pos + packSz > inSize) { error = "LZMA2-Daten abgeschnitten"; return false; }
                if (! d.decodeBlock (in + pos, packSz, unpack, false)) { error = "LZMA2-Daten fehlerhaft"; return false; }
                pos += packSz;
            }
            else { error = "LZMA2-Daten fehlerhaft"; return false; }
        }
        return d.flush();
    }
    if (c.method == mPpmd)
    {
        if (c.props.size() < 5) { error = "PPMd-Eigenschaften fehlen"; return false; }
        const juce::uint32 mem = (juce::uint32) c.props[1] | ((juce::uint32) c.props[2] << 8) | ((juce::uint32) c.props[3] << 16) | ((juce::uint32) c.props[4] << 24);
        return ppmd7Decode (in, inSize, c.props[0], mem, outSize, sink, error);
    }
    error = "Nicht unterstützte Komprimierung (Methode " + juce::String::toHexString ((juce::int64) c.method) + ")";
    (void) a; (void) f;
    return false;
}

// BCJ2 (x86 branch converter with 4 input streams: main, call, jump, range-coded flags)
bool bcj2Decode (const std::vector<juce::uint8>& main, const std::vector<juce::uint8>& call, const std::vector<juce::uint8>& jump,
                 const std::vector<juce::uint8>& rcData, std::vector<juce::uint8>& out, size_t outSize)
{
    juce::uint16 prob[256 + 2];
    for (auto& p : prob) p = 1 << 10;
    size_t inPos = 0, callPos = 0, jumpPos = 0, rcPos = 0, outPos = 0;
    juce::uint32 range = 0xFFFFFFFFu, code = 0;
    juce::uint8 prev = 0;
    out.assign (outSize, 0);
    for (int i = 0; i < 5; ++i) { if (rcPos >= rcData.size()) return false; code = (code << 8) | rcData[rcPos++]; }
    if (outSize == 0) return true;
    auto isJ = [] (juce::uint8 b0, juce::uint8 b1) { return (b1 & 0xFE) == 0xE8 || (b0 == 0x0F && (b1 & 0xF0) == 0x80); };
    for (;;)
    {
        size_t limit = main.size() - inPos;
        if (outSize - outPos < limit) limit = outSize - outPos;
        while (limit != 0)
        {
            const juce::uint8 b = main[inPos];
            out[outPos++] = b;
            if (isJ (prev, b)) break;
            ++inPos;
            prev = b;
            --limit;
        }
        if (limit == 0 || outPos == outSize) break;
        const juce::uint8 b = main[inPos++];
        juce::uint16* p = b == 0xE8 ? prob + prev : b == 0xE9 ? prob + 256 : prob + 257;
        const juce::uint32 ttt = *p;
        const juce::uint32 bound = (range >> 11) * ttt;
        bool one;
        if (code < bound) { range = bound; *p = (juce::uint16) (ttt + ((2048 - ttt) >> 5)); one = false; }
        else { range -= bound; code -= bound; *p = (juce::uint16) (ttt - (ttt >> 5)); one = true; }
        if (range < (1u << 24)) { if (rcPos >= rcData.size()) return false; range <<= 8; code = (code << 8) | rcData[rcPos++]; }
        if (! one) { prev = b; continue; }
        const juce::uint8* v;
        if (b == 0xE8) { if (callPos + 4 > call.size()) return false; v = call.data() + callPos; callPos += 4; }
        else { if (jumpPos + 4 > jump.size()) return false; v = jump.data() + jumpPos; jumpPos += 4; }
        const juce::uint32 dest = (((juce::uint32) v[0] << 24) | ((juce::uint32) v[1] << 16) | ((juce::uint32) v[2] << 8) | v[3]) - ((juce::uint32) outPos + 4);
        out[outPos++] = (juce::uint8) dest;
        if (outPos == outSize) break;
        out[outPos++] = (juce::uint8) (dest >> 8);
        if (outPos == outSize) break;
        out[outPos++] = (juce::uint8) (dest >> 16);
        if (outPos == outSize) break;
        out[outPos++] = prev = (juce::uint8) (dest >> 24);
    }
    return outPos == outSize;
}

// decodes one out-stream of a folder into memory (general coder graph; used for folders with BCJ2)
bool decodeOutStream (const Archive& a, const Folder& f, int outIdx, std::vector<juce::uint8>& out, juce::String& error, int depth = 0)
{
    if (depth > 8) { error = "Nicht unterstützte Filterkette"; return false; }
    int coderIdx = -1, inBase = 0, outBase = 0;
    for (size_t i = 0; i < f.coders.size(); ++i)
    {
        if (outIdx < outBase + f.coders[i].numOut) { coderIdx = (int) i; break; }
        inBase += f.coders[i].numIn;
        outBase += f.coders[i].numOut;
    }
    if (coderIdx < 0) { error = "Archiv fehlerhaft"; return false; }
    const auto& c = f.coders[(size_t) coderIdx];
    auto getIn = [&] (int k, std::vector<juce::uint8>& buf) -> bool
    {
        const int inIdx = inBase + k;
        for (auto& b : f.binds)
            if (b.inIndex == inIdx) return decodeOutStream (a, f, b.outIndex, buf, error, depth + 1);
        for (size_t pi = 0; pi < f.packed.size(); ++pi)
            if (f.packed[pi] == inIdx)
            {
                const juce::uint8* p; size_t n;
                if (! a.pack (f, (int) pi, p, n)) { error = "Archiv abgeschnitten"; return false; }
                buf.assign (p, p + n);
                return true;
            }
        error = "Archiv fehlerhaft";
        return false;
    };
    const auto outSize = f.unpackSizes[(size_t) outIdx];
    if (c.method == mBcj2)
    {
        std::vector<juce::uint8> in0, in1, in2, in3;
        if (c.numIn != 4 || ! getIn (0, in0) || ! getIn (1, in1) || ! getIn (2, in2) || ! getIn (3, in3)) { if (error.isEmpty()) error = "BCJ2-Daten fehlerhaft"; return false; }
        if (! bcj2Decode (in0, in1, in2, in3, out, (size_t) outSize)) { error = "BCJ2-Daten fehlerhaft"; return false; }
        return true;
    }
    if (c.numIn != 1 || c.numOut != 1) { error = "Nicht unterstützte Filterkette"; return false; }
    std::vector<juce::uint8> in;
    if (! getIn (0, in)) return false;
    if (c.method == mDelta) { deltaDecode (in.data(), in.size(), (c.props.empty() ? 1u : (size_t) c.props[0] + 1)); out = std::move (in); return true; }
    if (c.method == mBcj || c.method == mBcjOld) { bcjX86Decode (in.data(), in.size()); out = std::move (in); return true; }
    out.clear();
    out.reserve ((size_t) outSize);
    auto collect = [&out] (const juce::uint8* p, size_t len) { out.insert (out.end(), p, p + len); return true; };
    return decodeCoderToSink (a, f, c, in.data(), in.size(), outSize, collect, error);
}

bool decodeFolder (const Archive& a, const Folder& f, const std::function<bool (const juce::uint8*, size_t)>& sink, juce::String& error)
{
    for (auto& c : f.coders)
        if (c.method == mBcj2)
        {
            std::vector<juce::uint8> data;
            const int mo = f.mainOut();
            if (mo < 0 || ! decodeOutStream (a, f, mo, data, error)) { if (error.isEmpty()) error = "Archiv fehlerhaft"; return false; }
            return sink (data.data(), data.size());
        }
    if (f.coders.size() == 1)
    {
        const juce::uint8* in; size_t n;
        if (! a.pack (f, 0, in, n)) { error = "Archiv abgeschnitten"; return false; }
        return decodeCoderToSink (a, f, f.coders[0], in, n, f.mainSize(), sink, error);
    }
    if (f.coders.size() == 2)
    {
        // filter + compressor: decode the compressor into memory, then run the filter
        int filterIdx = -1;
        for (int i = 0; i < 2; ++i)
        {
            const auto m = f.coders[(size_t) i].method;
            if (m == mBcj || m == mBcjOld || m == mDelta) filterIdx = i;
        }
        if (filterIdx < 0 || f.packed.size() != 1)
        {
            error = "Nicht unterstützte Filterkette (" + juce::String::toHexString ((juce::int64) f.coders[0].method) + " + " + juce::String::toHexString ((juce::int64) f.coders[1].method)
                    + ", " + juce::String ((int) f.packed.size()) + " Pack-Streams)";
            return false;
        }
        const int compIdx = 1 - filterIdx;
        const juce::uint8* in; size_t n;
        if (! a.pack (f, 0, in, n)) { error = "Archiv abgeschnitten"; return false; }
        // compressor out stream index: coder 0 has out 0, coder 1 has out 1 (both have one out)
        const auto compSize = f.unpackSizes[(size_t) compIdx];
        std::vector<juce::uint8> buf;
        buf.reserve ((size_t) compSize);
        auto collect = [&buf] (const juce::uint8* p, size_t len) { buf.insert (buf.end(), p, p + len); return true; };
        if (! decodeCoderToSink (a, f, f.coders[(size_t) compIdx], in, n, compSize, collect, error)) return false;
        const auto& flt = f.coders[(size_t) filterIdx];
        if (flt.method == mDelta) deltaDecode (buf.data(), buf.size(), (flt.props.empty() ? 1u : (size_t) flt.props[0] + 1));
        else bcjX86Decode (buf.data(), buf.size());
        return sink (buf.data(), buf.size());
    }
    error = "Nicht unterstützte Filterkette (" + juce::String ((int) f.coders.size()) + " Coder:";
    for (auto& c : f.coders) error << " " << juce::String::toHexString ((juce::int64) c.method);
    error << ")";
    return false;
}

bool parseArchive (const juce::File& file, Archive& a, juce::String& error)
{
    if (! file.loadFileAsData (a.data)) { error = "Archiv lässt sich nicht lesen"; return false; }
    const auto* d = a.bytes();
    const size_t total = a.data.getSize();
    static const juce::uint8 sig[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
    if (total < 32 || std::memcmp (d, sig, 6) != 0) { error = "Kein 7z-Archiv"; return false; }
    auto le64 = [&] (size_t o) { juce::uint64 v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | d[o + (size_t) i]; return v; };
    const auto nextOff = le64 (12), nextSize = le64 (20);
    const juce::uint32 nextCrc = (juce::uint32) d[28] | ((juce::uint32) d[29] << 8) | ((juce::uint32) d[30] << 16) | ((juce::uint32) d[31] << 24);
    if (nextSize == 0) return true;   // empty archive
    if (32 + nextOff + nextSize > total) { error = "Archiv abgeschnitten"; return false; }
    if (SevenZip::crc32 (d + 32 + nextOff, (size_t) nextSize) != nextCrc) { error = "Archiv-Kopf beschädigt (Prüfsumme)"; return false; }

    std::vector<juce::uint8> header (d + 32 + nextOff, d + 32 + nextOff + nextSize);
    for (int guard = 0; guard < 4; ++guard)
    {
        Reader r { header.data(), header.size(), 0, false };
        const juce::uint8 id = r.byte();
        if (id == kEncodedHeader)
        {
            StreamsInfo si;
            if (! readStreamsInfo (r, si) || si.folders.empty()) { error = "Archiv-Kopf fehlerhaft"; return false; }
            Archive tmp;
            tmp.data.swapWith (a.data);
            tmp.streams = si;
            std::vector<juce::uint8> out;
            out.reserve ((size_t) si.folders[0].mainSize());
            auto collect = [&out] (const juce::uint8* p, size_t n) { out.insert (out.end(), p, p + n); return true; };
            const bool decoded = decodeFolder (tmp, si.folders[0], collect, error);
            a.data.swapWith (tmp.data);
            if (! decoded) return false;
            header = std::move (out);
            continue;
        }
        if (id != kHeader) { error = "Archiv-Kopf fehlerhaft"; return false; }
        juce::uint8 t = r.byte();
        if (t == kArchiveProperties)
        {
            for (;;) { const auto pt = r.byte(); if (pt == kEnd || r.bad) break; r.skip (r.number()); }
            t = r.byte();
        }
        if (t == kAdditionalStreamsInfo) { StreamsInfo dummy; if (! readStreamsInfo (r, dummy)) return false; t = r.byte(); }
        if (t == kMainStreamsInfo) { if (! readStreamsInfo (r, a.streams)) { error = "Archiv-Kopf fehlerhaft"; return false; } t = r.byte(); }
        if (t == kFilesInfo)
        {
            const int numFiles = r.count();
            std::vector<bool> emptyStream, emptyFile;
            std::vector<juce::String> names;
            std::vector<juce::uint32> attrs;
            std::vector<bool> attrDefined;
            int numEmpty = 0;
            for (;;)
            {
                const auto pt = r.byte();
                if (pt == kEnd || r.bad) break;
                const auto psize = r.number();
                const size_t endPos = r.pos + (size_t) psize;
                if (endPos > r.size) { error = "Archiv-Kopf fehlerhaft"; return false; }
                if (pt == kEmptyStream) { emptyStream = r.bits (numFiles); numEmpty = 0; for (bool b : emptyStream) numEmpty += b; }
                else if (pt == kEmptyFile) emptyFile = r.bits (numEmpty);
                else if (pt == kNames)
                {
                    if (r.byte() != 0) { error = "Externe Dateinamen werden nicht unterstützt"; return false; }
                    size_t i = r.pos;
                    for (int k = 0; k < numFiles; ++k)
                    {
                        size_t j = i;
                        while (j + 1 < endPos && (r.p[j] | r.p[j + 1]) != 0) j += 2;
                        names.push_back (utf16ToString (r.p + i, (j - i) / 2));
                        i = j + 2;
                    }
                }
                else if (pt == kWinAttributes)
                {
                    attrDefined = r.optionalBits (numFiles);
                    if (r.byte() == 0)
                    {
                        attrs.assign ((size_t) numFiles, 0);
                        for (int k = 0; k < numFiles; ++k) if (attrDefined[(size_t) k]) attrs[(size_t) k] = r.u32();
                    }
                }
                r.pos = endPos;
            }
            if ((int) names.size() != numFiles) { error = "Dateinamen fehlen im Archiv"; return false; }
            int sub = 0, emptyIdx = 0;
            for (int i = 0; i < numFiles; ++i)
            {
                SevenZip::Entry e;
                e.path = names[(size_t) i].replaceCharacter ('\\', '/');
                const bool isEmpty = ! emptyStream.empty() && emptyStream[(size_t) i];
                if (! attrs.empty()) e.attributes = attrs[(size_t) i];
                int subIndex = -1;
                if (isEmpty)
                {
                    const bool isFile = ! emptyFile.empty() && emptyIdx < (int) emptyFile.size() && emptyFile[(size_t) emptyIdx];
                    e.isDir = ! isFile;
                    ++emptyIdx;
                }
                else
                {
                    subIndex = sub++;
                    if (subIndex < (int) a.streams.subSizes.size())
                    {
                        e.size = a.streams.subSizes[(size_t) subIndex];
                        e.hasCrc = a.streams.subHasCrc[(size_t) subIndex];
                        e.crc = a.streams.subCrc[(size_t) subIndex];
                    }
                }
                if ((e.attributes & 0x10) != 0 && isEmpty) e.isDir = true;
                a.entries.push_back (std::move (e));
                a.entrySub.push_back (subIndex);
            }
            // folders of the entries
            std::vector<int> folderOfSub;
            for (size_t fi = 0; fi < a.streams.folders.size(); ++fi)
                for (int j = 0; j < a.streams.folders[fi].numSubstreams; ++j) folderOfSub.push_back ((int) fi);
            for (size_t ei = 0; ei < a.entries.size(); ++ei)
                if (a.entrySub[ei] >= 0 && a.entrySub[ei] < (int) folderOfSub.size()) a.entries[ei].folder = folderOfSub[(size_t) a.entrySub[ei]];
        }
        return ! r.bad;
    }
    error = "Archiv-Kopf fehlerhaft";
    return false;
}

bool pathIsSafe (const juce::String& p)
{
    if (p.isEmpty() || p.startsWithChar ('/') || p.contains (":")) return false;
    for (const auto& part : juce::StringArray::fromTokens (p, "/", {}))
        if (part == "..") return false;
    return true;
}

bool extractImpl (const juce::File& file, const juce::File& destDir, const std::function<bool (const SevenZip::Entry&)>& wanted,
                  const SevenZip::Progress& progress, juce::String& error, juce::StringArray* skipped)
{
    Archive a;
    if (! parseArchive (file, a, error)) return false;
    destDir.createDirectory();
    juce::uint64 totalBytes = 0, doneBytes = 0;
    for (auto& e : a.entries) if (! e.isDir && wanted (e)) totalBytes += e.size;

    // directories and empty files first
    for (auto& e : a.entries)
    {
        if (! wanted (e)) continue;
        if (! pathIsSafe (e.path)) { error = "Unsicherer Pfad im Archiv: " + e.path; return false; }
        const auto target = destDir.getChildFile (e.path);
        if (e.isDir) target.createDirectory();
        else if (e.folder < 0) { target.getParentDirectory().createDirectory(); target.replaceWithData ("", 0); }
    }

    std::vector<std::vector<int>> membersOf (a.streams.folders.size());
    for (size_t ei = 0; ei < a.entries.size(); ++ei)
        if (a.entries[ei].folder >= 0) membersOf[(size_t) a.entries[ei].folder].push_back ((int) ei);   // already in sub stream order
    for (size_t fi = 0; fi < a.streams.folders.size(); ++fi)
    {
        const auto& folder = a.streams.folders[fi];
        const auto& members = membersOf[fi];
        bool any = false;
        for (int m : members) if (wanted (a.entries[(size_t) m])) { any = true; break; }
        if (! any) continue;

        size_t cur = 0;                      // index into members
        juce::uint64 left = members.empty() ? 0 : a.entries[(size_t) members[0]].size;
        std::unique_ptr<juce::FileOutputStream> out;
        juce::uint32 crc = 0;
        bool abort = false;
        juce::String curName;
        auto openCurrent = [&]
        {
            out.reset();
            crc = 0;
            if (cur >= members.size()) return;
            const auto& e = a.entries[(size_t) members[cur]];
            curName = e.path;
            if (wanted (e))
            {
                const auto target = destDir.getChildFile (e.path);
                target.getParentDirectory().createDirectory();
                target.deleteFile();
                out = std::make_unique<juce::FileOutputStream> (target);
                if (! out->openedOk()) { out.reset(); abort = true; error = "Datei lässt sich nicht schreiben: " + e.path; }
            }
        };
        auto closeCurrent = [&]
        {
            if (cur < members.size())
            {
                const auto& e = a.entries[(size_t) members[cur]];
                if (out != nullptr) { out->flush(); out.reset(); if (e.hasCrc && crc != e.crc) { abort = true; error = "Prüfsummenfehler: " + e.path; } }
            }
        };
        openCurrent();
        auto sink = [&] (const juce::uint8* p, size_t n)
        {
            while (n > 0 && ! abort)
            {
                while (cur < members.size() && left == 0) { closeCurrent(); ++cur; left = cur < members.size() ? a.entries[(size_t) members[cur]].size : 0; openCurrent(); }
                if (cur >= members.size()) return ! abort;
                const size_t take = (size_t) std::min<juce::uint64> (left, n);
                if (out != nullptr) { out->write (p, take); crc = SevenZip::crc32 (p, take, crc); doneBytes += take; }
                left -= take; p += take; n -= take;
                if (progress && ! progress (doneBytes, totalBytes, curName)) { abort = true; error = "Abgebrochen"; }
            }
            return ! abort;
        };
        juce::String decodeError;
        const bool ok = decodeFolder (a, folder, sink, decodeError);
        if (ok && ! abort)
        {
            while (cur < members.size() && left == 0) { closeCurrent(); ++cur; left = cur < members.size() ? a.entries[(size_t) members[cur]].size : 0; openCurrent(); }
            closeCurrent();
        }
        if (abort) return false;
        if (! ok)
        {
            if (skipped != nullptr && decodeError.startsWith ("Nicht unterst"))
            {
                out.reset();
                for (int m : members) skipped->add (a.entries[(size_t) m].path);
                continue;
            }
            error = decodeError;
            return false;
        }
    }
    return true;
}
} // namespace

juce::uint32 SevenZip::crc32 (const void* data, size_t size, juce::uint32 crc)
{
    const auto& t = crcTable();
    crc = ~crc;
    const auto* p = static_cast<const juce::uint8*> (data);
    for (size_t i = 0; i < size; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool SevenZip::list (const juce::File& archive, std::vector<Entry>& out, juce::String& error)
{
    Archive a;
    if (! parseArchive (archive, a, error)) return false;
    out = a.entries;
    return true;
}

bool SevenZip::extractAll (const juce::File& archive, const juce::File& destDir, const Progress& progress, juce::String& error, juce::StringArray* skipped)
{
    return extractImpl (archive, destDir, [] (const Entry&) { return true; }, progress, error, skipped);
}

bool SevenZip::extractMatching (const juce::File& archive, const juce::File& destDir, const juce::String& wildcard, const Progress& progress, juce::String& error)
{
    return extractImpl (archive, destDir, [&] (const Entry& e) { return e.path.matchesWildcard (wildcard, true); }, progress, error, nullptr);
}
} // namespace sm
