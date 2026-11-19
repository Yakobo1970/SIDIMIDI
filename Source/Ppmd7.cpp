#include "Ppmd7.h"
#include <cstring>
#include <memory>
#include <vector>

namespace sm
{
namespace
{
constexpr int kIntBits = 7, kPeriodBits = 7;
constexpr unsigned kBinScale = 1u << (kIntBits + kPeriodBits);
constexpr int kN1 = 4, kN2 = 4, kN3 = 4, kN4 = (128 + 3 - 1 * kN1 - 2 * kN2 - 3 * kN3) / 4;
constexpr int kNumIndexes = kN1 + kN2 + kN3 + kN4;
constexpr unsigned kMaxFreq = 124;
constexpr unsigned kUnitSize = 12;
constexpr int kMaxOrder = 64;
constexpr juce::uint32 kTop = 1u << 24;

const juce::uint16 kInitBinEsc[] = { 0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051 };
const juce::uint8 kExpEscape[16] = { 25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2 };

struct See { juce::uint16 Summ; juce::uint8 Shift, Count; };
struct State { juce::uint8 Symbol, Freq; juce::uint16 SuccessorLow, SuccessorHigh; };
struct Context { juce::uint16 NumStats, SummFreq; juce::uint32 Stats, Suffix; };
struct Node { juce::uint16 Stamp, NU; juce::uint32 Next, Prev; };
static_assert (sizeof (State) == 6 && sizeof (Context) == 12 && sizeof (Node) == 12, "layout");

inline juce::uint32 successorOf (const State* s) { return (juce::uint32) s->SuccessorLow | ((juce::uint32) s->SuccessorHigh << 16); }
inline void setSuccessor (State* s, juce::uint32 v) { s->SuccessorLow = (juce::uint16) (v & 0xFFFF); s->SuccessorHigh = (juce::uint16) (v >> 16); }
inline State* oneState (Context* c) { return reinterpret_cast<State*> (&c->SummFreq); }

struct RangeDec
{
    const juce::uint8* p = nullptr; const juce::uint8* end = nullptr;
    juce::uint32 range = 0, code = 0;
    bool overrun = false;
    juce::uint8 next() { if (p < end) return *p++; overrun = true; return 0; }
    bool init()
    {
        code = 0; range = 0xFFFFFFFFu;
        if (next() != 0) return false;
        for (int i = 0; i < 4; ++i) code = (code << 8) | next();
        return code < 0xFFFFFFFFu;
    }
    void normalize()
    {
        if (range < kTop) { code = (code << 8) | next(); range <<= 8;
            if (range < kTop) { code = (code << 8) | next(); range <<= 8; } }
    }
    juce::uint32 threshold (juce::uint32 total) { range /= total; return code / range; }
    void decode (juce::uint32 start, juce::uint32 size) { code -= start * range; range *= size; normalize(); }
    juce::uint32 bit (juce::uint32 size0, juce::uint32 total)
    {
        const juce::uint32 bound = (range / total) * size0;
        juce::uint32 sym;
        if (code < bound) { sym = 0; range = bound; }
        else { sym = 1; code -= bound; range -= bound; }
        normalize();
        return sym;
    }
};

class Model
{
public:
    bool alloc (juce::uint32 size)
    {
        size_ = size;
        align_ = 4 - (size & 3);
        mem_.assign ((size_t) align_ + size + kUnitSize, 0);
        base_ = mem_.data();
        return true;
    }

    Model()
    {
        unsigned k = 0;
        for (int i = 0; i < kNumIndexes; ++i)
        {
            unsigned step = (i >= 12 ? 4 : (unsigned) (i >> 2) + 1);
            do { units2Indx_[k++] = (juce::uint8) i; } while (--step);
            indx2Units_[i] = (juce::uint8) k;
        }
        ns2BsIndx_[0] = (0 << 1);
        ns2BsIndx_[1] = (1 << 1);
        std::memset (ns2BsIndx_ + 2, (2 << 1), 9);
        std::memset (ns2BsIndx_ + 11, (3 << 1), 256 - 11);
        unsigned i = 0;
        for (; i < 3; ++i) ns2Indx_[i] = (juce::uint8) i;
        for (unsigned m = i, kk = 1; i < 256; ++i)
        {
            ns2Indx_[i] = (juce::uint8) m;
            if (--kk == 0) kk = (++m) - 2;
        }
        std::memset (hb2Flag_, 0, 0x40);
        std::memset (hb2Flag_ + 0x40, 8, 0x100 - 0x40);
    }

    void init (unsigned maxOrder)
    {
        maxOrder_ = maxOrder;
        restart();
        dummySee_.Shift = (juce::uint8) kPeriodBits;
        dummySee_.Summ = 0;
        dummySee_.Count = 64;
    }

    // -1 = end mark, -2 = data error
    int decodeSymbol (RangeDec& rc);

private:
    Context* minContext_ = nullptr; Context* maxContext_ = nullptr;
    State* foundState_ = nullptr;
    unsigned orderFall_ = 0, initEsc_ = 0, prevSuccess_ = 0, maxOrder_ = 0, hiBitsFlag_ = 0;
    juce::int32 runLength_ = 0, initRL_ = 0;
    juce::uint32 size_ = 0, glueCount_ = 0, align_ = 0;
    std::vector<juce::uint8> mem_;
    juce::uint8 *base_ = nullptr, *loUnit_ = nullptr, *hiUnit_ = nullptr, *text_ = nullptr, *unitsStart_ = nullptr;
    juce::uint8 indx2Units_[kNumIndexes] {}, units2Indx_[128] {};
    juce::uint32 freeList_[kNumIndexes] {};
    juce::uint8 ns2Indx_[256] {}, ns2BsIndx_[256] {}, hb2Flag_[256] {};
    See dummySee_ {}, see_[25][16] {};
    juce::uint16 binSumm_[128][64] {};

    juce::uint32 ref (const void* p) const { return (juce::uint32) ((const juce::uint8*) p - base_); }
    juce::uint8* ptr (juce::uint32 r) const { return base_ + r; }
    Context* ctx (juce::uint32 r) const { return reinterpret_cast<Context*> (base_ + r); }
    State* stats (const Context* c) const { return reinterpret_cast<State*> (base_ + c->Stats); }
    Context* suffix (const Context* c) const { return ctx (c->Suffix); }
    Node* node (juce::uint32 r) const { return reinterpret_cast<Node*> (base_ + r); }
    unsigned i2u (unsigned i) const { return indx2Units_[i]; }
    unsigned u2i (unsigned nu) const { return units2Indx_[nu - 1]; }
    static unsigned u2b (unsigned nu) { return nu * kUnitSize; }

    void insertNode (void* nodePtr, unsigned indx)
    {
        *static_cast<juce::uint32*> (nodePtr) = freeList_[indx];
        freeList_[indx] = ref (nodePtr);
    }
    void* removeNode (unsigned indx)
    {
        auto* n = reinterpret_cast<juce::uint32*> (ptr (freeList_[indx]));
        freeList_[indx] = *n;
        return n;
    }
    void splitBlock (void* p, unsigned oldIndx, unsigned newIndx)
    {
        const unsigned nu = i2u (oldIndx) - i2u (newIndx);
        auto* q = static_cast<juce::uint8*> (p) + u2b (i2u (newIndx));
        unsigned i = u2i (nu);
        if (i2u (i) != nu)
        {
            const unsigned k = i2u (--i);
            insertNode (q + u2b (k), nu - k - 1);
        }
        insertNode (q, i);
    }

    void glueFreeBlocks()
    {
        const juce::uint32 head = align_ + size_;
        juce::uint32 n = head;
        glueCount_ = 255;

        // doubly-linked list of all free blocks
        for (int i = 0; i < kNumIndexes; ++i)
        {
            const juce::uint16 nu = (juce::uint16) i2u ((unsigned) i);
            juce::uint32 next = freeList_[i];
            freeList_[i] = 0;
            while (next != 0)
            {
                Node* nd = node (next);
                nd->Next = n;
                node (n)->Prev = next;
                n = next;
                next = *reinterpret_cast<const juce::uint32*> (nd);
                nd->Stamp = 0;
                nd->NU = nu;
            }
        }
        node (head)->Stamp = 1;
        node (head)->Next = n;
        node (n)->Prev = head;
        if (loUnit_ != hiUnit_) reinterpret_cast<Node*> (loUnit_)->Stamp = 1;

        // glue neighbours
        while (n != head)
        {
            Node* nd = node (n);
            juce::uint32 nu = nd->NU;
            for (;;)
            {
                Node* nd2 = node (n) + nu;
                nu += nd2->NU;
                if (nd2->Stamp != 0 || nu >= 0x10000) break;
                node (nd2->Prev)->Next = nd2->Next;
                node (nd2->Next)->Prev = nd2->Prev;
                nd->NU = (juce::uint16) nu;
            }
            n = nd->Next;
        }

        // refill the free lists
        for (n = node (head)->Next; n != head;)
        {
            Node* nd = node (n);
            const juce::uint32 next = nd->Next;
            unsigned nu = nd->NU;
            for (; nu > 128; nu -= 128, nd += 128) insertNode (nd, kNumIndexes - 1);
            unsigned i = u2i (nu);
            if (i2u (i) != nu)
            {
                const unsigned k = i2u (--i);
                insertNode (nd + k, nu - k - 1);
            }
            insertNode (nd, i);
            n = next;
        }
    }

    void* allocUnitsRare (unsigned indx)
    {
        if (glueCount_ == 0)
        {
            glueFreeBlocks();
            if (freeList_[indx] != 0) return removeNode (indx);
        }
        unsigned i = indx;
        do
        {
            if (++i == (unsigned) kNumIndexes)
            {
                const juce::uint32 numBytes = u2b (i2u (indx));
                --glueCount_;
                if ((juce::uint32) (unitsStart_ - text_) > numBytes) { unitsStart_ -= numBytes; return unitsStart_; }
                return nullptr;
            }
        } while (freeList_[i] == 0);
        void* r = removeNode (i);
        splitBlock (r, i, indx);
        return r;
    }

    void* allocUnits (unsigned indx)
    {
        if (freeList_[indx] != 0) return removeNode (indx);
        const juce::uint32 numBytes = u2b (i2u (indx));
        if (numBytes <= (juce::uint32) (hiUnit_ - loUnit_))
        {
            void* r = loUnit_;
            loUnit_ += numBytes;
            return r;
        }
        return allocUnitsRare (indx);
    }

    static void copyUnits (void* dest, const void* src, unsigned num) { std::memcpy (dest, src, (size_t) num * kUnitSize); }

    void* shrinkUnits (void* oldPtr, unsigned oldNU, unsigned newNU)
    {
        const unsigned i0 = u2i (oldNU), i1 = u2i (newNU);
        if (i0 == i1) return oldPtr;
        if (freeList_[i1] != 0)
        {
            void* p = removeNode (i1);
            copyUnits (p, oldPtr, newNU);
            insertNode (oldPtr, i0);
            return p;
        }
        splitBlock (oldPtr, i0, i1);
        return oldPtr;
    }

    void restart()
    {
        std::memset (freeList_, 0, sizeof (freeList_));
        text_ = base_ + align_;
        hiUnit_ = text_ + size_;
        loUnit_ = unitsStart_ = hiUnit_ - size_ / 8 / kUnitSize * 7 * kUnitSize;
        glueCount_ = 0;

        orderFall_ = maxOrder_;
        runLength_ = initRL_ = -(juce::int32) ((maxOrder_ < 12) ? maxOrder_ : 12) - 1;
        prevSuccess_ = 0;

        hiUnit_ -= kUnitSize;
        minContext_ = maxContext_ = reinterpret_cast<Context*> (hiUnit_);
        minContext_->Suffix = 0;
        minContext_->NumStats = 256;
        minContext_->SummFreq = 256 + 1;
        foundState_ = reinterpret_cast<State*> (loUnit_);
        loUnit_ += u2b (256 / 2);
        minContext_->Stats = ref (foundState_);
        for (unsigned i = 0; i < 256; ++i)
        {
            State* s = &foundState_[i];
            s->Symbol = (juce::uint8) i;
            s->Freq = 1;
            setSuccessor (s, 0);
        }
        for (unsigned i = 0; i < 128; ++i)
            for (unsigned k = 0; k < 8; ++k)
            {
                juce::uint16* dest = binSumm_[i] + k;
                const juce::uint16 val = (juce::uint16) (kBinScale - kInitBinEsc[k] / (i + 2));
                for (unsigned m = 0; m < 64; m += 8) dest[m] = val;
            }
        for (unsigned i = 0; i < 25; ++i)
            for (unsigned k = 0; k < 16; ++k)
            {
                See* s = &see_[i][k];
                s->Shift = (juce::uint8) (kPeriodBits - 4);
                s->Summ = (juce::uint16) ((5 * i + 10) << s->Shift);
                s->Count = 4;
            }
    }

    Context* createSuccessors (bool skip)
    {
        State upState;
        Context* c = minContext_;
        const juce::uint32 upBranch = successorOf (foundState_);
        State* ps[kMaxOrder];
        unsigned numPs = 0;

        if (! skip) ps[numPs++] = foundState_;
        while (c->Suffix)
        {
            State* s;
            c = suffix (c);
            if (c->NumStats != 1)
            {
                for (s = stats (c); s->Symbol != foundState_->Symbol; ++s) {}
            }
            else s = oneState (c);
            const juce::uint32 successor = successorOf (s);
            if (successor != upBranch)
            {
                c = ctx (successor);
                if (numPs == 0) return c;
                break;
            }
            ps[numPs++] = s;
        }

        upState.Symbol = *ptr (upBranch);
        setSuccessor (&upState, upBranch + 1);

        if (c->NumStats == 1) upState.Freq = oneState (c)->Freq;
        else
        {
            State* s;
            for (s = stats (c); s->Symbol != upState.Symbol; ++s) {}
            const juce::uint32 cf = (juce::uint32) s->Freq - 1;
            const juce::uint32 s0 = (juce::uint32) c->SummFreq - c->NumStats - cf;
            upState.Freq = (juce::uint8) (1 + ((2 * cf <= s0) ? (5 * cf > s0 ? 1 : 0) : ((2 * cf + 3 * s0 - 1) / (2 * s0))));
        }

        do
        {
            Context* c1;
            if (hiUnit_ != loUnit_) { hiUnit_ -= kUnitSize; c1 = reinterpret_cast<Context*> (hiUnit_); }
            else if (freeList_[0] != 0) c1 = static_cast<Context*> (removeNode (0));
            else
            {
                c1 = static_cast<Context*> (allocUnitsRare (0));
                if (c1 == nullptr) return nullptr;
            }
            c1->NumStats = 1;
            *oneState (c1) = upState;
            c1->Suffix = ref (c);
            setSuccessor (ps[--numPs], ref (c1));
            c = c1;
        } while (numPs != 0);
        return c;
    }

    static void swapStates (State* a, State* b) { const State t = *a; *a = *b; *b = t; }

    void updateModel()
    {
        juce::uint32 fSuccessor = successorOf (foundState_);
        Context* c;

        if (foundState_->Freq < kMaxFreq / 4 && minContext_->Suffix != 0)
        {
            c = suffix (minContext_);
            if (c->NumStats == 1)
            {
                State* s = oneState (c);
                if (s->Freq < 32) s->Freq++;
            }
            else
            {
                State* s = stats (c);
                if (s->Symbol != foundState_->Symbol)
                {
                    do { ++s; } while (s->Symbol != foundState_->Symbol);
                    if (s[0].Freq >= s[-1].Freq) { swapStates (&s[0], &s[-1]); --s; }
                }
                if (s->Freq < kMaxFreq - 9) { s->Freq = (juce::uint8) (s->Freq + 2); c->SummFreq = (juce::uint16) (c->SummFreq + 2); }
            }
        }

        if (orderFall_ == 0)
        {
            minContext_ = maxContext_ = createSuccessors (true);
            if (minContext_ == nullptr) { restart(); return; }
            setSuccessor (foundState_, ref (minContext_));
            return;
        }

        *text_++ = foundState_->Symbol;
        juce::uint32 successor = ref (text_);
        if (text_ >= unitsStart_) { restart(); return; }

        if (fSuccessor)
        {
            if (fSuccessor <= successor)
            {
                Context* cs = createSuccessors (false);
                if (cs == nullptr) { restart(); return; }
                fSuccessor = ref (cs);
            }
            if (--orderFall_ == 0)
            {
                successor = fSuccessor;
                text_ -= (maxContext_ != minContext_) ? 1 : 0;
            }
        }
        else
        {
            setSuccessor (foundState_, successor);
            fSuccessor = ref (minContext_);
        }

        const unsigned ns = minContext_->NumStats;
        const unsigned s0 = minContext_->SummFreq - ns - (foundState_->Freq - 1);

        for (c = maxContext_; c != minContext_; c = suffix (c))
        {
            unsigned ns1;
            juce::uint32 cf, sf;
            if ((ns1 = c->NumStats) != 1)
            {
                if ((ns1 & 1) == 0)
                {
                    const unsigned oldNU = ns1 >> 1;
                    const unsigned i = u2i (oldNU);
                    if (i != u2i (oldNU + 1))
                    {
                        void* p = allocUnits (i + 1);
                        if (p == nullptr) { restart(); return; }
                        void* oldPtr = stats (c);
                        copyUnits (p, oldPtr, oldNU);
                        insertNode (oldPtr, i);
                        c->Stats = ref (p);
                    }
                }
                c->SummFreq = (juce::uint16) (c->SummFreq + (2 * ns1 < ns ? 1 : 0) + 2 * (((4 * ns1 <= ns) ? 1 : 0) & ((c->SummFreq <= 8 * ns1) ? 1 : 0)));
            }
            else
            {
                State* s = static_cast<State*> (allocUnits (0));
                if (s == nullptr) { restart(); return; }
                *s = *oneState (c);
                c->Stats = ref (s);
                if (s->Freq < kMaxFreq / 4 - 1) s->Freq = (juce::uint8) (s->Freq << 1); else s->Freq = (juce::uint8) (kMaxFreq - 4);
                c->SummFreq = (juce::uint16) (s->Freq + initEsc_ + (ns > 3 ? 1 : 0));
            }
            cf = 2 * (juce::uint32) foundState_->Freq * (c->SummFreq + 6);
            sf = (juce::uint32) s0 + c->SummFreq;
            if (cf < 6 * sf)
            {
                cf = 1 + (cf > sf ? 1 : 0) + (cf >= 4 * sf ? 1 : 0);
                c->SummFreq = (juce::uint16) (c->SummFreq + 3);
            }
            else
            {
                cf = 4 + (cf >= 9 * sf ? 1 : 0) + (cf >= 12 * sf ? 1 : 0) + (cf >= 15 * sf ? 1 : 0);
                c->SummFreq = (juce::uint16) (c->SummFreq + cf);
            }
            State* s = stats (c) + ns1;
            setSuccessor (s, successor);
            s->Symbol = foundState_->Symbol;
            s->Freq = (juce::uint8) cf;
            c->NumStats = (juce::uint16) (ns1 + 1);
        }
        maxContext_ = minContext_ = ctx (fSuccessor);
    }

    void rescale()
    {
        State* st = stats (minContext_);
        State* s = foundState_;
        if (s != st)
        {
            const State tmp = *s;
            do { s[0] = s[-1]; } while (--s != st);
            *s = tmp;
        }
        unsigned escFreq = minContext_->SummFreq - s->Freq;
        s->Freq = (juce::uint8) (s->Freq + 4);
        const unsigned adder = (orderFall_ != 0) ? 1 : 0;
        s->Freq = (juce::uint8) ((s->Freq + adder) >> 1);
        unsigned sumFreq = s->Freq;

        unsigned i = (unsigned) minContext_->NumStats - 1;
        do
        {
            escFreq -= (++s)->Freq;
            s->Freq = (juce::uint8) ((s->Freq + adder) >> 1);
            sumFreq += s->Freq;
            if (s[0].Freq > s[-1].Freq)
            {
                State* s1 = s;
                const State tmp = *s1;
                do { s1[0] = s1[-1]; } while (--s1 != st && tmp.Freq > s1[-1].Freq);
                *s1 = tmp;
            }
        } while (--i);

        if (s->Freq == 0)
        {
            const unsigned numStats = minContext_->NumStats;
            do { ++i; } while ((--s)->Freq == 0);
            escFreq += i;
            minContext_->NumStats = (juce::uint16) (minContext_->NumStats - i);
            if (minContext_->NumStats == 1)
            {
                State tmp = *st;
                do
                {
                    tmp.Freq = (juce::uint8) (tmp.Freq - (tmp.Freq >> 1));
                    escFreq >>= 1;
                } while (escFreq > 1);
                insertNode (st, u2i ((numStats + 1) >> 1));
                foundState_ = oneState (minContext_);
                *foundState_ = tmp;
                return;
            }
            const unsigned n0 = (numStats + 1) >> 1, n1 = ((unsigned) minContext_->NumStats + 1) >> 1;
            if (n0 != n1) minContext_->Stats = ref (shrinkUnits (st, n0, n1));
        }
        minContext_->SummFreq = (juce::uint16) (sumFreq + escFreq - (escFreq >> 1));
        foundState_ = stats (minContext_);
    }

    See* makeEscFreq (unsigned numMasked, juce::uint32& escFreq)
    {
        See* see;
        const unsigned nonMasked = (unsigned) minContext_->NumStats - numMasked;
        if (minContext_->NumStats != 256)
        {
            see = see_[ns2Indx_[nonMasked - 1]]
                  + (nonMasked < (unsigned) suffix (minContext_)->NumStats - minContext_->NumStats ? 1 : 0)
                  + 2 * (minContext_->SummFreq < 11 * minContext_->NumStats ? 1 : 0)
                  + 4 * (numMasked > nonMasked ? 1 : 0)
                  + hiBitsFlag_;
            const unsigned r = (see->Summ >> see->Shift);
            see->Summ = (juce::uint16) (see->Summ - r);
            escFreq = r + (r == 0 ? 1 : 0);
        }
        else { see = &dummySee_; escFreq = 1; }
        return see;
    }

    void nextContext()
    {
        Context* c = ctx (successorOf (foundState_));
        if (orderFall_ == 0 && (juce::uint8*) c > text_) minContext_ = maxContext_ = c;
        else updateModel();
    }
    void update1()
    {
        State* s = foundState_;
        s->Freq = (juce::uint8) (s->Freq + 4);
        minContext_->SummFreq = (juce::uint16) (minContext_->SummFreq + 4);
        if (s[0].Freq > s[-1].Freq)
        {
            swapStates (&s[0], &s[-1]);
            foundState_ = --s;
            if (s->Freq > kMaxFreq) rescale();
        }
        nextContext();
    }
    void update1_0()
    {
        prevSuccess_ = (2 * foundState_->Freq > minContext_->SummFreq) ? 1 : 0;
        runLength_ += (juce::int32) prevSuccess_;
        minContext_->SummFreq = (juce::uint16) (minContext_->SummFreq + 4);
        foundState_->Freq = (juce::uint8) (foundState_->Freq + 4);
        if (foundState_->Freq > kMaxFreq) rescale();
        nextContext();
    }
    void updateBin()
    {
        foundState_->Freq = (juce::uint8) (foundState_->Freq + (foundState_->Freq < 128 ? 1 : 0));
        prevSuccess_ = 1;
        runLength_++;
        nextContext();
    }
    void update2()
    {
        minContext_->SummFreq = (juce::uint16) (minContext_->SummFreq + 4);
        foundState_->Freq = (juce::uint8) (foundState_->Freq + 4);
        if (foundState_->Freq > kMaxFreq) rescale();
        runLength_ = initRL_;
        updateModel();
    }
};

int Model::decodeSymbol (RangeDec& rc)
{
    juce::uint8 charMask[256];
    if (minContext_->NumStats != 1)
    {
        State* s = stats (minContext_);
        juce::uint32 count, hiCnt;
        if ((count = rc.threshold (minContext_->SummFreq)) < (hiCnt = s->Freq))
        {
            rc.decode (0, s->Freq);
            foundState_ = s;
            const juce::uint8 symbol = s->Symbol;
            update1_0();
            return symbol;
        }
        prevSuccess_ = 0;
        unsigned i = (unsigned) minContext_->NumStats - 1;
        do
        {
            if ((hiCnt += (++s)->Freq) > count)
            {
                rc.decode (hiCnt - s->Freq, s->Freq);
                foundState_ = s;
                const juce::uint8 symbol = s->Symbol;
                update1();
                return symbol;
            }
        } while (--i);
        if (count >= minContext_->SummFreq) return -2;
        hiBitsFlag_ = hb2Flag_[foundState_->Symbol];
        rc.decode (hiCnt, minContext_->SummFreq - hiCnt);
        std::memset (charMask, 0xFF, sizeof (charMask));
        charMask[s->Symbol] = 0;
        i = (unsigned) minContext_->NumStats - 1;
        do { charMask[(--s)->Symbol] = 0; } while (--i);
    }
    else
    {
        State* one = oneState (minContext_);
        hiBitsFlag_ = hb2Flag_[foundState_->Symbol];
        juce::uint16* prob = &binSumm_[one->Freq - 1][prevSuccess_ + ns2BsIndx_[suffix (minContext_)->NumStats - 1] + hiBitsFlag_
                                                      + 2 * hb2Flag_[one->Symbol] + ((runLength_ >> 26) & 0x20)];
        if (rc.bit (*prob, kBinScale) == 0)
        {
            *prob = (juce::uint16) (*prob + (1 << kIntBits) - ((*prob + (1 << (kPeriodBits - 2))) >> kPeriodBits));
            foundState_ = one;
            const juce::uint8 symbol = one->Symbol;
            updateBin();
            return symbol;
        }
        *prob = (juce::uint16) (*prob - ((*prob + (1 << (kPeriodBits - 2))) >> kPeriodBits));
        initEsc_ = kExpEscape[*prob >> 10];
        std::memset (charMask, 0xFF, sizeof (charMask));
        charMask[one->Symbol] = 0;
        prevSuccess_ = 0;
    }
    for (;;)
    {
        State* ps[256];
        State* s;
        juce::uint32 freqSum, count, hiCnt;
        const unsigned numMasked = minContext_->NumStats;
        do
        {
            orderFall_++;
            if (! minContext_->Suffix) return -1;
            minContext_ = suffix (minContext_);
        } while (minContext_->NumStats == numMasked);
        hiCnt = 0;
        s = stats (minContext_);
        unsigned i = 0;
        const unsigned num = (unsigned) minContext_->NumStats - numMasked;
        do
        {
            const int k = (int) (signed char) charMask[s->Symbol];
            hiCnt += (juce::uint32) (s->Freq & k);
            ps[i] = s++;
            i -= (unsigned) k;
        } while (i != num);

        See* see = makeEscFreq (numMasked, freqSum);
        freqSum += hiCnt;
        count = rc.threshold (freqSum);

        if (count < hiCnt)
        {
            State** pps = ps;
            for (hiCnt = 0; (hiCnt += (*pps)->Freq) <= count; ++pps) {}
            s = *pps;
            rc.decode (hiCnt - s->Freq, s->Freq);
            if (see->Shift < kPeriodBits && --see->Count == 0) { see->Summ = (juce::uint16) (see->Summ << 1); see->Count = (juce::uint8) (3 << see->Shift++); }
            foundState_ = s;
            const juce::uint8 symbol = s->Symbol;
            update2();
            return symbol;
        }
        if (count >= freqSum) return -2;
        rc.decode (hiCnt, freqSum - hiCnt);
        see->Summ = (juce::uint16) (see->Summ + freqSum);
        do { charMask[ps[--i]->Symbol] = 0; } while (i != 0);
    }
}
} // namespace

bool ppmd7Decode (const juce::uint8* in, size_t inSize, juce::uint8 order, juce::uint32 memSize, juce::uint64 outSize,
                  const std::function<bool (const juce::uint8*, size_t)>& sink, juce::String& error)
{
    if (order < 2 || order > kMaxOrder || memSize < (1u << 11) || memSize > 0xFFFFFFFFu - 12 * 3)
    {
        error = "PPMd-Eigenschaften ungültig";
        return false;
    }
    auto model = std::make_unique<Model>();
    model->alloc (memSize);
    RangeDec rc;
    rc.p = in; rc.end = in + inSize;
    if (! rc.init()) { error = "PPMd-Daten fehlerhaft"; return false; }
    model->init (order);

    std::vector<juce::uint8> buf (1 << 16);
    juce::uint64 done = 0;
    while (done < outSize)
    {
        const size_t n = (size_t) std::min<juce::uint64> (buf.size(), outSize - done);
        for (size_t i = 0; i < n; ++i)
        {
            const int sym = model->decodeSymbol (rc);
            if (sym < 0) { error = "PPMd-Daten fehlerhaft oder abgeschnitten"; return false; }
            buf[i] = (juce::uint8) sym;
        }
        if (! sink (buf.data(), n)) { error = "Abgebrochen"; return false; }
        done += n;
    }
    return true;
}
} // namespace sm
