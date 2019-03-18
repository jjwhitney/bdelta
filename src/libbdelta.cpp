/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <limits>
#include <list>
#include <memory>
#include <stdio.h>
#include <vector>
#include <utility>

#if !defined(TOKEN_SIZE) || TOKEN_SIZE == 1
typedef uint8_t Token;
#elif TOKEN_SIZE == 2
typedef uint16_t Token;
#elif TOKEN_SIZE == 4
typedef uint32_t Token;
#endif

#include "bdelta.h"
#include "checksum.h"


const bool verbose = false;
struct checksum_entry 
{
    Hash::Value cksum; //Rolling checksums
    unsigned loc;
    checksum_entry() {}
    checksum_entry(Hash::Value _cksum, unsigned _loc) : cksum(_cksum), loc(_loc) {}
};

struct Range 
{
    unsigned p, num;
    Range() {}
    Range(unsigned _p, unsigned _num) : p(_p), num(_num) {}
};

struct Match 
{
    unsigned p1, p2, num;
    Match(unsigned _p1, unsigned _p2, unsigned _num) : p1(_p1), p2(_p2), num(_num) {}
};

struct _BDelta_Instance 
{
    const bdelta_readCallback cb;
    void *handle1, *handle2;
    unsigned data1_size, data2_size;
    std::list<Match> matches;
    std::list<Match>::iterator accessplace;
    int access_int;
    int errorcode;

    _BDelta_Instance(bdelta_readCallback _cb, void * _handle1, void * _handle2,
                     unsigned _data1_size, unsigned _data2_size)
        : cb(_cb), handle1(_handle1), handle2(_handle2)
        , data1_size(_data1_size), data2_size(_data2_size)
        , access_int(-1), errorcode(0)
    {}

    const Token *read1(void *buf, unsigned place, unsigned num)
    {
        return (cb == nullptr)? ((const Token*)((char *)handle1 + place)) : ((const Token*)cb(handle1, buf, place, num));
    }
    const Token *read2(void *buf, unsigned place, unsigned num)
    {
        return (cb == nullptr) ? ((const Token*)((char *)handle2 + place)) : ((const Token*)cb(handle2, buf, place, num));
    }
};

struct Checksums_Instance 
{
    const unsigned blocksize;
    const unsigned htablesize;
    checksum_entry ** const htable;    // Points to first match in checksums
    checksum_entry * const checksums;  // Sorted list of all checksums
    unsigned numchecksums;

    Checksums_Instance(unsigned _blocksize, unsigned _htablesize, checksum_entry ** _htable, checksum_entry * _checksums)
        : blocksize(_blocksize), htablesize(_htablesize), htable(_htable)
        , checksums(_checksums), numchecksums(0)
    {
        memset(htable, 0, htablesize * sizeof(htable[0]));
    }
    
    template <class T>
    void add(T&& ck) 
    {
        checksums[++numchecksums] = std::forward<T>(ck);
    }
    unsigned tableIndex(Hash::Value hashValue) 
    {
        return Hash::modulo(hashValue, htablesize);
    }
};


static unsigned match_buf_forward(const void *buf1, const void *buf2, unsigned num)
{ 
    unsigned i = 0;
    while (i < num && ((const Token*)buf1)[i] == ((const Token*)buf2)[i])
        ++i;
    return i;
}

static unsigned match_buf_backward(const void *buf1, const void *buf2, unsigned num)
{ 
    int i = num;
    do
    {
        --i;
    }
    while (i >= 0 && ((const Token*)buf1)[i] == ((const Token*)buf2)[i]);
    return (num - i - 1);
}

static const size_t TOKEN_BUFFER_SIZE = 4096;
static thread_local std::unique_ptr<Token[]> match_forward_buffer;
static unsigned match_forward(BDelta_Instance *b, unsigned p1, unsigned p2)
{ 
    unsigned num = 0, match, numtoread;
    Token * buf1 = match_forward_buffer.get(), * buf2 = buf1 + TOKEN_BUFFER_SIZE;
    do 
    {
        numtoread = std::min<unsigned>(std::min(b->data1_size - p1, b->data2_size - p2), TOKEN_BUFFER_SIZE);
        const Token *read1 = b->read1(buf1, p1, numtoread),
                    *read2 = b->read2(buf2, p2, numtoread);
        p1 += numtoread; p2 += numtoread;
        match = match_buf_forward(read1, read2, numtoread);
        num += match;
    } while (match && match == numtoread);
    return num;
}

static thread_local std::unique_ptr<Token[]> match_backward_buffer;
static unsigned match_backward(BDelta_Instance *b, unsigned p1, unsigned p2, unsigned blocksize)
{
    unsigned num = 0, match, numtoread;
    Token * buf1 = match_backward_buffer.get(), * buf2 = buf1 + TOKEN_BUFFER_SIZE;
    do 
    {
        numtoread = std::min<unsigned>(std::min(std::min(p1, p2), blocksize), TOKEN_BUFFER_SIZE);
        p1 -= numtoread;
        p2 -= numtoread;
        const Token *read1 = b->read1(buf1, p1, numtoread),
                    *read2 = b->read2(buf2, p2, numtoread);
        match = match_buf_backward(read1, read2, numtoread);
        num += match;
    } while (match && match == numtoread);
    return num;
}

// Iterator helper function
template <class T>
static inline T bdelta_next(T i) {return ++i;}

struct UnusedRange 
{
    unsigned p, num;
    std::list<Match>::iterator ml, mr;
    UnusedRange() {}
    UnusedRange(unsigned _p, unsigned _num, std::list<Match>::iterator _ml, std::list<Match>::iterator _mr) 
        : p(_p), num(_num), ml(_ml), mr(_mr)
    {}
};

// Sort first by location, second by match length (larger matches first)
class comparep
{
public:
    bool operator()(UnusedRange r1, UnusedRange r2)
    {
        return ((r1.p != r2.p) ? (r1.p < r2.p) : (r1.num > r2.num));
    }
};

class comparemrp2
{
public:
    bool operator()(UnusedRange r1, UnusedRange r2)
    {
        return ((r1.mr->p2 != r2.mr->p2)? (r1.mr->p2 < r2.mr->p2) : (r1.mr->num > r2.mr->num));
    }
};

class compareMatchP2
{
public:
    bool operator()(Match r1, Match r2)
    {
        return ((r1.p2 != r2.p2) ? (r1.p2 < r2.p2) : (r1.num > r2.num));
    }
};

static void addMatch(BDelta_Instance *b, unsigned p1, unsigned p2, unsigned num, std::list<Match>::iterator place)
{
    Match newMatch = Match(p1, p2, num);
    compareMatchP2 comp;
    while (place != b->matches.begin() && !comp(*place, newMatch))
        --place;
    while (place != b->matches.end() && comp(*place, newMatch))
        ++place;
    b->matches.insert(place, newMatch);
}

template<class T>
T absoluteDifference(T a, T b) 
{
    return std::max(a, b) - std::min(a, b);
}


static constexpr size_t BUFFER_DEFAULT_SIZE = 16 * 1024;
static thread_local std::vector<Token> findMatchesBuffer;
static void findMatches(BDelta_Instance *b, Checksums_Instance *h, unsigned minMatchSize, unsigned start, unsigned end, unsigned place, std::list<Match>::iterator iterPlace)
{
    const unsigned blocksize = h->blocksize;

    findMatchesBuffer.resize(blocksize * 2);
    Token * buf1 = findMatchesBuffer.data(), * buf2 = buf1 + blocksize;

    unsigned best1 = 0, best2 = 0, bestnum = 0;
    unsigned processMatchesPos = 0;
    const Token *inbuf = b->read2(buf1, start, blocksize),
                *outbuf = nullptr;
    Hash hash = Hash(inbuf, blocksize);
    unsigned buf_loc = blocksize;
    for (unsigned j = start + blocksize; ; ++j) 
    {
        unsigned thisTableIndex = h->tableIndex(hash.getValue());
        checksum_entry *c = h->htable[thisTableIndex];
        if (c) 
        {
            do 
            {
                if (c->cksum == hash.getValue()) 
                {
                    unsigned p1 = c->loc, p2 = j - blocksize;
                    unsigned fnum = match_forward(b, p1, p2);
                    if (fnum >= blocksize) 
                    {
                        unsigned bnum = match_backward(b, p1, p2, blocksize);
                        unsigned num = fnum + bnum;
                        if (num >= minMatchSize) 
                        {
                            p1 -= bnum; p2 -= bnum;
                            bool foundBetter;
                            if (bestnum) 
                            {
                                double oldValue = double(bestnum) / (absoluteDifference(place, best1) + blocksize * 2),
                                       newValue = double(num) / (absoluteDifference(place, p1) + blocksize * 2);
                                foundBetter = newValue > oldValue;
                            } 
                            else 
                            {
                                foundBetter = true;
                                processMatchesPos = std::min(j + blocksize - 1, end);
                            }
                            if (foundBetter) 
                            {
                                best1 = p1;
                                best2 = p2;
                                bestnum = num;
                            }

                        }
                    }
                }
                ++c;
            } while (h->tableIndex(c->cksum) == thisTableIndex);
        }

        if (bestnum && j >= processMatchesPos) 
        {
            addMatch(b, best1, best2, bestnum, iterPlace);
            place = best1 + bestnum;
            unsigned matchEnd = best2 + bestnum;
            if (matchEnd > j) 
            {
                if (matchEnd >= end)
                    j = end;
                else 
                {
                    // Fast forward over matched area.
                    j = matchEnd - blocksize;
                    inbuf = b->read2(buf1, j, blocksize);
                    hash = Hash(inbuf, blocksize);
                    buf_loc = blocksize;
                    j += blocksize;
                }
            }
            bestnum = 0;
        }

        if (buf_loc == blocksize) 
        {
            buf_loc = 0;
            std::swap(inbuf, outbuf);
            inbuf = b->read2(outbuf == buf1 ? buf2 : buf1, j, std::min(end - j, blocksize));
        }

        if (j >= end)
            break;

        hash.advance(outbuf[buf_loc], inbuf[buf_loc]);
        ++buf_loc;
    }
}

struct Checksums_Compare 
{
    Checksums_Instance &ci;
    explicit Checksums_Compare(Checksums_Instance &h) : ci(h) {}
    bool operator() (checksum_entry c1, checksum_entry c2) 
    {
        unsigned ti1 = ci.tableIndex(c1.cksum), ti2 = ci.tableIndex(c2.cksum);
        if (ti1 == ti2)
        {
            if (c1.cksum == c2.cksum)
                return c1.loc < c2.loc;
            else
                return c1.cksum < c2.cksum;
        }
        else
            return ti1 < ti2;
    }
};

static thread_local std::vector<checksum_entry*> bdelta_pass_2_htable;
static thread_local std::vector<checksum_entry>  bdelta_pass_2_hchecksums;
static thread_local std::vector<Token>           bdelta_pass_2_buf;
static thread_local std::vector<UnusedRange>     bdelta_pass_unused;

BDelta_Instance *bdelta_init_alg(unsigned data1_size, unsigned data2_size,
                                 bdelta_readCallback cb, void *handle1, void *handle2,
                                 unsigned tokenSize) 
{
    if (tokenSize != sizeof(Token)) 
    {
        printf("Error: BDelta library compiled for token size of %lu.\n", (unsigned long)sizeof (Token));
        return 0;
    }
    BDelta_Instance *b = new BDelta_Instance(cb, handle1, handle2, data1_size, data2_size);
    if (!b) 
        return nullptr;

    match_forward_buffer.reset(new Token[TOKEN_BUFFER_SIZE * 2]);
    match_backward_buffer.reset(new Token[TOKEN_BUFFER_SIZE * 2]);

    findMatchesBuffer.resize(BUFFER_DEFAULT_SIZE);
    bdelta_pass_2_htable.resize(BUFFER_DEFAULT_SIZE);
    bdelta_pass_2_hchecksums.resize(BUFFER_DEFAULT_SIZE);
    bdelta_pass_2_buf.resize(BUFFER_DEFAULT_SIZE);
    bdelta_pass_unused.resize(BUFFER_DEFAULT_SIZE);

    return b;
}

void bdelta_done_alg(BDelta_Instance *b) 
{
    b->matches.clear();
    delete b;

    match_forward_buffer.reset();
    match_backward_buffer.reset();

    findMatchesBuffer.clear();
    bdelta_pass_2_htable.clear();
    bdelta_pass_2_hchecksums.clear();
    bdelta_pass_2_buf.clear();
    bdelta_pass_unused.clear();

    findMatchesBuffer.shrink_to_fit();
    bdelta_pass_2_htable.shrink_to_fit();
    bdelta_pass_2_hchecksums.shrink_to_fit();
    bdelta_pass_2_buf.shrink_to_fit();
    bdelta_pass_unused.shrink_to_fit();
}


// Adapted from http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
static unsigned roundUpPowerOf2(unsigned v)
{
    --v;
    for (int i = 1; i <= 16; i *= 2)
        v |= (v >> i);
    return (v + 1);
}

static void bdelta_pass_2(BDelta_Instance *b, unsigned blocksize, unsigned minMatchSize, UnusedRange *unused, unsigned numunused, UnusedRange *unused2, unsigned numunused2)
{
    b->access_int = -1;

    unsigned numblocks = 0;
    for (unsigned i = 0; i < numunused; ++i)
        numblocks += unused[i].num;
    numblocks /= blocksize;

    bdelta_pass_2_htable.resize(std::max((unsigned)2, roundUpPowerOf2(numblocks)));
    bdelta_pass_2_hchecksums.resize(numblocks + 2);

    Checksums_Instance h(blocksize, bdelta_pass_2_htable.size(), bdelta_pass_2_htable.data(), bdelta_pass_2_hchecksums.data());

    bdelta_pass_2_buf.resize(blocksize);
    for (unsigned i = 0; i < numunused; ++i)
    {
        unsigned first = unused[i].p, last = unused[i].p + unused[i].num;
        for (unsigned loc = first; loc + blocksize <= last; loc += blocksize) 
        {
            const Token *read = b->read1(bdelta_pass_2_buf.data(), loc, blocksize);
            Hash::Value blocksum = Hash(read, blocksize).getValue();
            // Adjacent checksums are never repeated.
            h.add(checksum_entry(blocksum, loc));
        }
    }

    if (h.numchecksums) 
    {
        std::sort(h.checksums, h.checksums + h.numchecksums, Checksums_Compare(h));
        const unsigned maxIdenticalChecksums = 2;
        unsigned writeLoc = 0, readLoc, testAhead;
        for (readLoc = 0; readLoc < h.numchecksums; readLoc = testAhead) 
        {
            for (testAhead = readLoc; testAhead < h.numchecksums && h.checksums[readLoc].cksum == h.checksums[testAhead].cksum; ++testAhead)
                ;
            if (testAhead - readLoc <= maxIdenticalChecksums)
                for (unsigned i = readLoc; i < testAhead; ++i)
                    h.checksums[writeLoc++] = h.checksums[i];
        }
        h.numchecksums = writeLoc;
    }
    h.checksums[h.numchecksums].cksum = std::numeric_limits<Hash::Value>::max(); // If there's only one checksum, we might hit this and not know it,
    h.checksums[h.numchecksums].loc = 0; // So we'll just read from the beginning of the file to prevent crashes.
    h.checksums[h.numchecksums + 1].cksum = 0;

    for (int i = h.numchecksums - 1; i >= 0; --i)
        h.htable[h.tableIndex(h.checksums[i].cksum)] = &h.checksums[i];

    for (unsigned i = 0; i < numunused2; ++i)
    {
        if (unused2[i].num >= blocksize)
            findMatches(b, &h, minMatchSize, unused2[i].p, unused2[i].p + unused2[i].num, unused[i].p, unused2[i].mr);
    }
}

void bdelta_swap_inputs(BDelta_Instance *b) 
{
    for (std::list<Match>::iterator l = b->matches.begin(); l != b->matches.end(); ++l)
        std::swap(l->p1, l->p2);
    std::swap(b->data1_size, b->data2_size);
    std::swap(b->handle1, b->handle2);
    b->matches.sort(compareMatchP2());
}

void bdelta_clean_matches(BDelta_Instance *b, unsigned flags) 
{
    std::list<Match>::iterator nextL = b->matches.begin();
    if (nextL == b->matches.end()) 
        return;
    while (true) 
    {
        std::list<Match>::iterator l = nextL;
        if (++nextL == b->matches.end())
            break;

        int overlap = l->p2 + l->num - nextL->p2;
        if (overlap >= 0) 
        {
            if ((unsigned)overlap >= nextL->num) 
            {
                b->matches.erase(nextL);
                nextL = l;
                continue;
            }
            if (flags & BDELTA_REMOVE_OVERLAP)
                l->num -= overlap;
        }
    }
}

void bdelta_showMatches(BDelta_Instance *b)
{
    for (std::list<Match>::iterator l = b->matches.begin(); l != b->matches.end(); ++l)
        printf("(%d, %d, %d), ", l->p1, l->p2, l->num);
    printf ("\n\n");
}

static void get_unused_blocks(UnusedRange *unused, unsigned *numunusedptr)
{
    unsigned nextStartPos = 0;
    for (unsigned i = 1; i < *numunusedptr; ++i) 
    {
        unsigned startPos = nextStartPos;
        nextStartPos = std::max(startPos, unused[i].p + unused[i].num);
        unused[i] = UnusedRange(startPos, unused[i].p < startPos ? 0 : unused[i].p - startPos, unused[i-1].mr, unused[i].mr);
    }
}

inline bool isZeroMatch(Match &m) { return m.num == 0; }

void bdelta_pass(BDelta_Instance *b, unsigned blocksize, unsigned minMatchSize, unsigned maxHoleSize, unsigned flags) 
{
    // Place an empty Match at beginning so we can assume there's a Match to the left of every hole.
    b->matches.emplace_front(0, 0, 0);
    // Trick for including the free range at the end.
    b->matches.emplace_back(b->data1_size, b->data2_size, 0);

    const size_t BUFFER_SIZE = b->matches.size() + 1;
    bdelta_pass_unused.resize(BUFFER_SIZE * 2);
    UnusedRange *unused = bdelta_pass_unused.data(),
                *unused2 = unused + BUFFER_SIZE;

    unsigned numunused = 0, numunused2 = 0;
    for (std::list<Match>::iterator l = b->matches.begin(); l != b->matches.end(); ++l) 
    {
        unused[numunused++] = UnusedRange(l->p1, l->num, l, l);
        unused2[numunused2++] = UnusedRange(l->p2, l->num, l, l);
    }

    std::sort(unused + 1, unused + numunused, comparep()); // Leave empty match at beginning

    get_unused_blocks(unused,  &numunused);
    get_unused_blocks(unused2, &numunused2);

    if (flags & BDELTA_GLOBAL)
        bdelta_pass_2(b, blocksize, minMatchSize, unused, numunused, unused2, numunused2);
    else 
    {
        std::sort(unused + 1, unused + numunused, comparemrp2());
        for (unsigned i = 1; i < numunused; ++i)
        {
            UnusedRange u1 = unused[i], u2 = unused2[i];
            if (u1.num >= blocksize && u2.num >= blocksize)
                if (!maxHoleSize || (u1.num <= maxHoleSize && u2.num <= maxHoleSize))
                    if (!(flags & BDELTA_SIDES_ORDERED) || (bdelta_next(u1.ml) == u1.mr && bdelta_next(u2.ml) == u2.mr))
                        bdelta_pass_2(b, blocksize, minMatchSize, &u1, 1, &u2, 1);
        }
    }

    if (verbose) 
        printf("pass (blocksize: %u, matches: %u)\n", blocksize, (unsigned)b->matches.size());

    // Get rid of the dummy values we placed at the ends.
    b->matches.erase(std::find_if(b->matches.begin(), b->matches.end(), isZeroMatch));
    b->matches.pop_back();
}

unsigned bdelta_numMatches(BDelta_Instance *b) 
{
    return b->matches.size();
}

void bdelta_getMatch(BDelta_Instance *b, unsigned matchNum, unsigned *p1, unsigned *p2, unsigned *num) 
{
    int &access_int = b->access_int;
    std::list<Match>::iterator &accessplace = b->accessplace;
    if (access_int == -1) 
    {
        access_int = 0;
        accessplace = b->matches.begin();
    }
    while ((unsigned)access_int < matchNum) 
    {
        ++accessplace;
        ++access_int;
    }
    while ((unsigned)access_int > matchNum) 
    {
        --accessplace;
        --access_int;
    }
    *p1 = accessplace->p1;
    *p2 = accessplace->p2;
    *num = accessplace->num;
}

inline int bdelta_getError(BDelta_Instance *instance) 
{
    return instance->errorcode;
}
