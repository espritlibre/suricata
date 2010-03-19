/* Implementation of the Wu-Manber pattern matching algorithm.
 *
 * Copyright (c) 2008 Victor Julien <victor@inliniac.net>
 *
 * Ideas:
 *   - the hash contains a list of patterns. Maybe we can 'train' the hash
 *     so the most common patterns always appear first in this list.
 *
 * TODO VJ
 *  - make hash1 a array of ptr and get rid of the flag field in the
 *    WmHashItem
 *  - remove exit() calls
 *  - only calc prefixci_buf for nocase patterns? -- would be in a
 *    loop though, so probably not a performance inprovement.
 *  - make sure runtime counters can be disabled (at compile time)
 */

#include "suricata-common.h"
#include "suricata.h"
#include "util-mpm.h"
#include "util-mpm-wumanber.h"
#include "conf.h"

#include "util-unittest.h"
#include "util-debug.h"

#define INIT_HASH_SIZE 65535

#define HASH16_SIZE 65536
#define HASH16(a,b) (((a)<<8) | (b))
#define HASH15_SIZE 32768
#define HASH15(a,b) (((a)<<7) | (b))
#define HASH14_SIZE 16384
#define HASH14(a,b) (((a)<<6) | (b))
#define HASH12_SIZE 4096
#define HASH12(a,b) (((a)<<4) | (b))
#define HASH9_SIZE 512
#define HASH9(a,b) (((a)<<1) | (b))

static uint32_t wm_hash_size = 0;
static uint32_t wm_bloom_size = 0;

void WmInitCtx (MpmCtx *mpm_ctx, int);
void WmThreadInitCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, uint32_t);
void WmDestroyCtx(MpmCtx *mpm_ctx);
void WmThreadDestroyCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx);
int WmAddScanPatternCI(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, uint8_t);
int WmAddScanPatternCS(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, uint8_t);
int WmScanPreparePatterns(MpmCtx *mpm_ctx);
inline uint32_t WmScan(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
inline uint32_t WmSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t WmScan1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t WmScan2Hash9(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t WmScan2Hash12(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t WmScan2Hash14(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t WmScan2Hash15(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t WmScan2Hash16(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
void WmPrintInfo(MpmCtx *mpm_ctx);
void WmPrintSearchStats(MpmThreadCtx *mpm_thread_ctx);
void WmRegisterTests(void);

/* uppercase to lowercase conversion lookup table */
static uint8_t lowercasetable[256];
/* marco to do the actual lookup */
#define wm_tolower(c) lowercasetable[(c)]

#ifdef WUMANBER_COUNTERS
#define COUNT(counter) \
        (counter)
#else
#define COUNT(counter)
#endif /* WUMANBER_COUNTERS */

void MpmWuManberRegister (void) {
    mpm_table[MPM_WUMANBER].name = "wumanber";
    mpm_table[MPM_WUMANBER].max_pattern_length = 0;
    mpm_table[MPM_WUMANBER].InitCtx = WmInitCtx;
    mpm_table[MPM_WUMANBER].InitThreadCtx = WmThreadInitCtx;
    mpm_table[MPM_WUMANBER].DestroyCtx = WmDestroyCtx;
    mpm_table[MPM_WUMANBER].DestroyThreadCtx = WmThreadDestroyCtx;
    mpm_table[MPM_WUMANBER].AddScanPattern = WmAddScanPatternCS;
    mpm_table[MPM_WUMANBER].AddScanPatternNocase = WmAddScanPatternCI;
    //mpm_table[MPM_WUMANBER].AddPattern = WmAddScanPatternCS;
    //mpm_table[MPM_WUMANBER].AddPatternNocase = WmAddScanPatternCI;
    mpm_table[MPM_WUMANBER].Prepare = WmScanPreparePatterns;
    mpm_table[MPM_WUMANBER].Scan = WmScan;
    //mpm_table[MPM_WUMANBER].Search = WmSearch;
    //mpm_table[MPM_WUMANBER].Cleanup = MpmMatchCleanup;
    mpm_table[MPM_WUMANBER].PrintCtx = WmPrintInfo;
    mpm_table[MPM_WUMANBER].PrintThreadCtx = WmPrintSearchStats;
    mpm_table[MPM_WUMANBER].RegisterUnittests = WmRegisterTests;

    /* create table for O(1) lowercase conversion lookup */
    uint8_t c = 0;
    for ( ; c < 255; c++) {
       if (c >= 'A' && c <= 'Z')
           lowercasetable[c] = (c + ('a' - 'A'));
       else
           lowercasetable[c] = c;
    }
}

/* append an endmatch to a pattern
 *
 * Only used in the initialization phase */
static inline void WmEndMatchAppend(MpmCtx *mpm_ctx, WmPattern *p,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid,
    uint8_t nosearch)
{
    MpmEndMatch *em = MpmAllocEndMatch(mpm_ctx);
    if (em == NULL) {
        printf("ERROR: WmAllocEndMatch failed\n");
        return;
    }

    em->id = pid;
    em->sig_id = sid;
    em->depth = depth;
    em->offset = offset;

    if (nosearch)
        em->flags |= MPM_ENDMATCH_NOSEARCH;

    if (p->em == NULL) {
        p->em = em;
        return;
    }

    MpmEndMatch *m = p->em;
    while (m->next) {
        m = m->next;
    }
    m->next = em;
}

void prt (uint8_t *buf, uint16_t buflen) {
    uint16_t i;

    for (i = 0; i < buflen; i++) {
        if (isprint(buf[i])) printf("%c", buf[i]);
        else                 printf("\\x%" PRIX32, buf[i]);
    }
    //printf("\n");
}

void WmPrintInfo(MpmCtx *mpm_ctx) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;

    printf("MPM WuManber Information:\n");
    printf("Memory allocs:   %" PRIu32 "\n", mpm_ctx->memory_cnt);
    printf("Memory alloced:  %" PRIu32 "\n", mpm_ctx->memory_size);
    printf(" Sizeofs:\n");
    printf("  MpmCtx         %" PRIuMAX "\n", (uintmax_t)sizeof(MpmCtx));
    printf("  WmCtx:         %" PRIuMAX "\n", (uintmax_t)sizeof(WmCtx));
    printf("  WmPattern      %" PRIuMAX "\n", (uintmax_t)sizeof(WmPattern));
    printf("  WmHashItem     %" PRIuMAX "\n", (uintmax_t)sizeof(WmHashItem));
    printf("Unique Patterns: %" PRIu32 "\n", mpm_ctx->pattern_cnt);
    printf("Scan Patterns:   %" PRIu32 "\n", mpm_ctx->scan_pattern_cnt);
    printf("Total Patterns:  %" PRIu32 "\n", mpm_ctx->total_pattern_cnt);
    printf("Smallest:        %" PRIu32 "\n", mpm_ctx->scan_minlen);
    printf("Largest:         %" PRIu32 "\n", mpm_ctx->scan_maxlen);
    printf("Max shiftlen:    %" PRIu32 "\n", ctx->scan_shiftlen);
    printf("Hash size:       %" PRIu32 "\n", ctx->scan_hash_size);
    printf("Scan function: ");
    if (ctx->Scan == WmScan1) {
        printf("WmScan1 (allows single byte patterns)\n");
        printf("MBScan funct:  ");
        if (ctx->MBScan == WmScan2Hash16) printf("WmSearch2Hash16\n");
        else if (ctx->MBScan == WmScan2Hash15) printf("WmSearch2Hash15\n");
        else if (ctx->MBScan == WmScan2Hash14) printf("WmSearch2Hash14\n");
        else if (ctx->MBScan == WmScan2Hash12) printf("WmSearch2Hash12\n");
        else if (ctx->MBScan == WmScan2Hash9)  printf("WmSearch2Hash9\n");
    }
    if (ctx->Scan == WmScan2Hash16) printf("WmScan2Hash16 (only for multibyte patterns)\n");
    else if (ctx->Scan == WmScan2Hash15) printf("WmScan2Hash15 (only for multibyte patterns)\n");
    else if (ctx->Scan == WmScan2Hash14) printf("WmScan2Hash14 (only for multibyte patterns)\n");
    else if (ctx->Scan == WmScan2Hash12) printf("WmScan2Hash12 (only for multibyte patterns)\n");
    else if (ctx->Scan == WmScan2Hash9)  printf("WmScan2Hash9 (only for multibyte patterns)\n");
    else printf("ERROR\n");
    printf("\n");
}

static inline WmPattern *WmAllocPattern(MpmCtx *mpm_ctx) {
    WmPattern *p = SCMalloc(sizeof(WmPattern));
    if (p == NULL) {
        printf("ERROR: WmAllocPattern: SCMalloc failed\n");
        exit(EXIT_FAILURE);
    }
    memset(p,0,sizeof(WmPattern));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(WmPattern);
    return p;
}

static inline WmHashItem *
WmAllocHashItem(MpmCtx *mpm_ctx) {
    WmHashItem *hi = SCMalloc(sizeof(WmHashItem));
    if (hi == NULL) {
        printf("ERROR: WmAllocHashItem: SCMalloc failed\n");
        exit(EXIT_FAILURE);
    }
    memset(hi,0,sizeof(WmHashItem));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(WmHashItem);
    return hi;
}

static void WmHashFree(MpmCtx *mpm_ctx, WmHashItem *hi) {
    if (hi == NULL)
        return;

    WmHashItem *t = hi->nxt;
    WmHashFree(mpm_ctx, t);

    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(WmHashItem);
    SCFree(hi);
}

static inline void memcpy_tolower(uint8_t *d, uint8_t *s, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        d[i] = wm_tolower(s[i]);
    }
}

/*
 * INIT HASH START
 */
static inline uint32_t WmInitHash(WmPattern *p) {
    uint32_t hash = p->len * p->cs[0];
    if (p->len > 1)
        hash += p->cs[1];

    return (hash % INIT_HASH_SIZE);
}

static inline uint32_t WmInitHashRaw(uint8_t *pat, uint16_t patlen) {
    uint32_t hash = patlen * pat[0];
    if (patlen > 1)
        hash += pat[1];

    return (hash % INIT_HASH_SIZE);
}

static inline int WmInitHashAdd(WmCtx *ctx, WmPattern *p) {
    uint32_t hash = WmInitHash(p);

    //printf("WmInitHashAdd: %" PRIu32 "\n", hash);

    if (ctx->init_hash[hash] == NULL) {
        ctx->init_hash[hash] = p;
        //printf("WmInitHashAdd: hash %" PRIu32 ", head %p\n", hash, ctx->init_hash[hash]);
        return 0;
    }

    WmPattern *tt = NULL;
    WmPattern *t = ctx->init_hash[hash];

    /* get the list tail */
    do {
        tt = t;
        t = t->next;
    } while (t != NULL);

    tt->next = p;
    //printf("WmInitHashAdd: hash %" PRIu32 ", head %p\n", hash, ctx->init_hash[hash]);

    return 0;
}

static inline int WmCmpPattern(WmPattern *p, uint8_t *pat, uint16_t patlen, char nocase);

static inline WmPattern *WmInitHashLookup(WmCtx *ctx, uint8_t *pat, uint16_t patlen, char nocase) {
    uint32_t hash = WmInitHashRaw(pat,patlen);

    //printf("WmInitHashLookup: %" PRIu32 ", head %p\n", hash, ctx->init_hash[hash]);

    if (ctx->init_hash[hash] == NULL) {
        return NULL;
    }

    WmPattern *t = ctx->init_hash[hash];
    for ( ; t != NULL; t = t->next) {
        if (WmCmpPattern(t,pat,patlen,nocase) == 1)
            return t;
    }

    return NULL;
}

static inline int WmCmpPattern(WmPattern *p, uint8_t *pat, uint16_t patlen, char nocase) {
    if (p->len != patlen)
        return 0;

    if (!((nocase && p->flags & WUMANBER_NOCASE) || (!nocase && !(p->flags & WUMANBER_NOCASE))))
        return 0;

    if (memcmp(p->cs, pat, patlen) != 0)
        return 0;

    return 1;
}

/*
 * INIT HASH END
 */

void WmFreePattern(MpmCtx *mpm_ctx, WmPattern *p) {
    if (p && p->em) {
        MpmEndMatchFreeAll(mpm_ctx, p->em);
    }

    if (p && p->cs && p->cs != p->ci) {
        SCFree(p->cs);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p && p->ci) {
        SCFree(p->ci);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p) {
        SCFree(p);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= sizeof(WmPattern); 
    }
}

/* WmAddPattern
 *
 * pat: ptr to the pattern
 * patlen: length of the pattern
 * nocase: nocase flag: 1 enabled, 0 disable
 * pid: pattern id
 * sid: signature id (internal id)
 */
static inline int WmAddPattern(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen, uint16_t offset, uint16_t depth, char nocase, char scan, uint32_t pid, uint32_t sid, uint8_t nosearch) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;

//    printf("WmAddPattern: ctx %p \"", mpm_ctx); prt(pat, patlen);
//    printf("\" id %" PRIu32 ", nocase %s\n", id, nocase ? "true" : "false");

    if (patlen == 0)
        return 0;

    /* get a memory piece */
    WmPattern *p = WmInitHashLookup(ctx, pat, patlen, nocase);
    if (p == NULL) {
//        printf("WmAddPattern: allocing new pattern\n");
        p = WmAllocPattern(mpm_ctx);
        if (p == NULL)
            goto error;

        p->len = patlen;

        if (nocase) p->flags |= WUMANBER_NOCASE;

        /* setup the case insensitive part of the pattern */
        p->ci = SCMalloc(patlen);
        if (p->ci == NULL) goto error;
        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy_tolower(p->ci, pat, patlen);

        /* setup the case sensitive part of the pattern */
        if (p->flags & WUMANBER_NOCASE) {
            /* nocase means no difference between cs and ci */
            p->cs = p->ci;
        } else {
            if (memcmp(p->ci,pat,p->len) == 0) {
                /* no diff between cs and ci: pat is lowercase */
                p->cs = p->ci;
            } else {
                p->cs = SCMalloc(patlen);
                if (p->cs == NULL) goto error;
                mpm_ctx->memory_cnt++;
                mpm_ctx->memory_size += patlen;
                memcpy(p->cs, pat, patlen);
            }
        }

        if (p->len > 1) {
            p->prefix_cs = (uint16_t)(*(p->cs)+*(p->cs+1));
            p->prefix_ci = (uint16_t)(*(p->ci)+*(p->ci+1));
        }

        //printf("WmAddPattern: ci \""); prt(p->ci,p->len);
        //printf("\" cs \""); prt(p->cs,p->len);
        //printf("\" prefix_ci %" PRIu32 ", prefix_cs %" PRIu32 "\n", p->prefix_ci, p->prefix_cs);

        /* put in the pattern hash */
        WmInitHashAdd(ctx, p);

        if (mpm_ctx->pattern_cnt == 65535) {
            printf("Max search words reached\n");
            exit(1);
        }
        mpm_ctx->pattern_cnt++;

        if (mpm_ctx->scan_maxlen < patlen) mpm_ctx->scan_maxlen = patlen;
        if (mpm_ctx->scan_minlen == 0) mpm_ctx->scan_minlen = patlen;
        else if (mpm_ctx->scan_minlen > patlen) mpm_ctx->scan_minlen = patlen;
    }

    /* we need a match */
    WmEndMatchAppend(mpm_ctx, p, offset, depth, pid, sid, nosearch);

    mpm_ctx->total_pattern_cnt++;
    return 0;

error:
    WmFreePattern(mpm_ctx, p);
    return -1;
}

int WmAddScanPatternCI(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid, uint8_t nosearch)
{
    return WmAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */1, /* scan */1, pid, sid, nosearch);
}

int WmAddScanPatternCS(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid, uint8_t nosearch)
{
    return WmAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */0, /* scan */1, pid, sid, nosearch);
}

static uint32_t WmBloomHash(void *data, uint16_t datalen, uint8_t iter, uint32_t hash_size) {
     uint8_t *d = (uint8_t *)data;
     uint16_t i;
     uint32_t hash = (uint32_t)wm_tolower(*d);

     for (i = 1; i < datalen - 1; i++) {
         hash += (wm_tolower((*d++))) ^ i;
     }
     hash <<= (iter+1);

     hash %= hash_size;
     return hash;
}
/*
static uint32_t BloomHash(void *data, uint16_t datalen, uint8_t iter, uint32_t hash_size) {
     uint8_t *d = (uint8_t *)data;
     uint32_t i;
     uint32_t hash = 0;

     for (i = 0; i < datalen; i++) {
         if (i == 0)      hash += (((uint32_t)*d++));
         else if (i == 1) hash += (((uint32_t)*d++) * datalen);
         else             hash *= (((uint32_t)*d++) * i);
     }

     hash *= (iter + datalen);
     hash %= hash_size;
     return hash;
}
*/
static void WmScanPrepareHash(MpmCtx *mpm_ctx) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
    uint16_t i;
    uint16_t idx = 0;
    uint8_t idx8 = 0;

    ctx->scan_hash = (WmHashItem **)SCMalloc(sizeof(WmHashItem *) * ctx->scan_hash_size);
    if (ctx->scan_hash == NULL) goto error;
    memset(ctx->scan_hash, 0, sizeof(WmHashItem *) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(WmHashItem *) * ctx->scan_hash_size);

    /* alloc the pminlen array */
    ctx->scan_pminlen = (uint8_t *)SCMalloc(sizeof(uint8_t) * ctx->scan_hash_size);
    if (ctx->scan_pminlen == NULL) goto error;
    memset(ctx->scan_pminlen, 0, sizeof(uint8_t) * ctx->scan_hash_size);

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        if(ctx->parray[i]->len == 1) {
            idx8 = (uint8_t)ctx->parray[i]->ci[0];
            if (ctx->scan_hash1[idx8].flags == 0) {
                ctx->scan_hash1[idx8].idx = i;
                ctx->scan_hash1[idx8].flags |= 0x01;
            } else {
                WmHashItem *hi = WmAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                WmHashItem *thi = &ctx->scan_hash1[idx8];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
        } else {
            uint16_t patlen = ctx->scan_shiftlen;

            if (ctx->scan_hash_size == HASH9_SIZE)
                idx = HASH9(ctx->parray[i]->ci[patlen-1], ctx->parray[i]->ci[patlen-2]);
            else if (ctx->scan_hash_size == HASH12_SIZE)
                idx = HASH12(ctx->parray[i]->ci[patlen-1], ctx->parray[i]->ci[patlen-2]);
            else if (ctx->scan_hash_size == HASH14_SIZE)
                idx = HASH14(ctx->parray[i]->ci[patlen-1], ctx->parray[i]->ci[patlen-2]);
            else if (ctx->scan_hash_size == HASH15_SIZE)
                idx = HASH15(ctx->parray[i]->ci[patlen-1], ctx->parray[i]->ci[patlen-2]);
            else
                idx = HASH16(ctx->parray[i]->ci[patlen-1], ctx->parray[i]->ci[patlen-2]);

            if (ctx->scan_hash[idx] == NULL) {
                WmHashItem *hi = WmAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;
                ctx->scan_pminlen[idx] = ctx->parray[i]->len;

                ctx->scan_hash[idx] = hi;
            } else {
                WmHashItem *hi = WmAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                if (ctx->parray[i]->len < ctx->scan_pminlen[idx])
                    ctx->scan_pminlen[idx] = ctx->parray[i]->len;

                /* Append this HashItem to the list */
                WmHashItem *thi = ctx->scan_hash[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
        }
    }

    /* alloc the bloom array */
    ctx->scan_bloom = (BloomFilter **)SCMalloc(sizeof(BloomFilter *) * ctx->scan_hash_size);
    if (ctx->scan_bloom == NULL) goto error;
    memset(ctx->scan_bloom, 0, sizeof(BloomFilter *) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(BloomFilter *) * ctx->scan_hash_size);

    uint32_t h;
    for (h = 0; h < ctx->scan_hash_size; h++) {
        WmHashItem *hi = ctx->scan_hash[h];
        if (hi == NULL)
            continue;

        ctx->scan_bloom[h] = BloomFilterInit(wm_bloom_size, 2, WmBloomHash);
        if (ctx->scan_bloom[h] == NULL)
            continue;

        mpm_ctx->memory_cnt += BloomFilterMemoryCnt(ctx->scan_bloom[h]);
        mpm_ctx->memory_size += BloomFilterMemorySize(ctx->scan_bloom[h]);

        if (ctx->scan_pminlen[h] > 8)
            ctx->scan_pminlen[h] = 8;

        WmHashItem *thi = hi;
        do {
            BloomFilterAdd(ctx->scan_bloom[h], ctx->parray[thi->idx]->ci, ctx->scan_pminlen[h]);
            thi = thi->nxt;
        } while (thi != NULL);
    }
    return;
error:
    return;
}
static void WmScanPrepareShiftTable(MpmCtx *mpm_ctx)
{
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;

    uint16_t shift = 0, k = 0, idx = 0;
    uint32_t i = 0;

    uint16_t smallest = mpm_ctx->scan_minlen;
    if (smallest > 255) smallest = 255;
    if (smallest < 2) smallest = 2;

    ctx->scan_shiftlen = smallest;

    ctx->scan_shifttable = SCMalloc(sizeof(uint16_t) * ctx->scan_hash_size);
    if (ctx->scan_shifttable == NULL)
        return;

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(uint16_t) * ctx->scan_hash_size);

    /* default shift table is set to minimal shift */
    for (i = 0; i < ctx->scan_hash_size; i++)
        ctx->scan_shifttable[i] = ctx->scan_shiftlen;

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        /* ignore one byte patterns */
        if (ctx->parray[i]->len == 1)
            continue;

        /* add the first character of the pattern preceeded by
         * every possible other character. */
        for (k = 0; k < 256; k++) {
            shift = ctx->scan_shiftlen - 1;
            if (shift > 255) shift = 255;

            if (ctx->scan_hash_size == HASH9_SIZE) {
                idx = HASH9(ctx->parray[i]->ci[0], (uint8_t)k);
                //printf("HASH9 idx %" PRIu32 "\n", idx);
            } else if (ctx->scan_hash_size == HASH12_SIZE) {
                idx = HASH12(ctx->parray[i]->ci[0], (uint8_t)k);
                //printf("HASH12 idx %" PRIu32 "\n", idx);
            } else if (ctx->scan_hash_size == HASH14_SIZE) {
                idx = HASH14(ctx->parray[i]->ci[0], (uint8_t)k);
                //printf("HASH14 idx %" PRIu32 "\n", idx);
            } else if (ctx->scan_hash_size == HASH15_SIZE) {
                idx = HASH15(ctx->parray[i]->ci[0], (uint8_t)k);
                //printf("HASH15 idx %" PRIu32 "\n", idx);
            } else {
                idx = HASH16(ctx->parray[i]->ci[0], (uint8_t)k);
                //printf("HASH15 idx %" PRIu32 "\n", idx);
            }
            if (shift < ctx->scan_shifttable[idx]) {
                ctx->scan_shifttable[idx] = shift;
            }
        }

        for (k = 0; k < ctx->scan_shiftlen-1; k++)
        {
            shift = (ctx->scan_shiftlen - 2 - k);
            if (shift > 255) shift = 255;

            if (ctx->scan_hash_size == HASH9_SIZE) {
                idx = HASH9(ctx->parray[i]->ci[k+1], ctx->parray[i]->ci[k]);
                //printf("HASH9 idx %" PRIu32 "\n", idx);
            } else if (ctx->scan_hash_size == HASH12_SIZE) {
                idx = HASH12(ctx->parray[i]->ci[k+1], ctx->parray[i]->ci[k]);
                //printf("HASH12 idx %" PRIu32 "\n", idx);
            } else if (ctx->scan_hash_size == HASH14_SIZE) {
                idx = HASH14(ctx->parray[i]->ci[k+1], ctx->parray[i]->ci[k]);
                //printf("HASH14 idx %" PRIu32 "\n", idx);
            } else if (ctx->scan_hash_size == HASH15_SIZE) {
                idx = HASH15(ctx->parray[i]->ci[k+1], ctx->parray[i]->ci[k]);
                //printf("HASH15 idx %" PRIu32 "\n", idx);
            } else {
                idx = HASH16(ctx->parray[i]->ci[k+1], ctx->parray[i]->ci[k]);
                //printf("HASH15 idx %" PRIu32 "\n", idx);
            }
            if (shift < ctx->scan_shifttable[idx]) {
                ctx->scan_shifttable[idx] = shift;
            }
            //printf("WmPrepareShiftTable: i %" PRIu32 ", k %" PRIu32 ", idx %" PRIu32 ", shift set to %" PRIu32 ": \"%c%c\"\n",
            //    i, k, idx, shift, ctx->parray[i]->ci[k], ctx->parray[i]->ci[k+1]);
        }
    }
}

int WmScanPreparePatterns(MpmCtx *mpm_ctx) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;

    /* alloc the pattern array */
    ctx->parray = (WmPattern **)SCMalloc(mpm_ctx->pattern_cnt * sizeof(WmPattern *));
    if (ctx->parray == NULL) goto error;
    memset(ctx->parray, 0, mpm_ctx->pattern_cnt * sizeof(WmPattern *));
    //printf("mpm_ctx %p, parray %p\n", mpm_ctx,ctx->parray);
    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (mpm_ctx->pattern_cnt * sizeof(WmPattern *));

    /* populate it with the patterns in the hash */
    uint32_t i = 0, p = 0;
    for (i = 0; i < INIT_HASH_SIZE; i++) {
        WmPattern *node = ctx->init_hash[i], *nnode = NULL;
        for ( ; node != NULL; ) {
            nnode = node->next;
            node->next = NULL;

            ctx->parray[p] = node;

            p++;
            node = nnode;
        }
    }
    /* we no longer need the hash, so free it's memory */
    SCFree(ctx->init_hash);
    ctx->init_hash = NULL;

    /* TODO VJ these values are chosen pretty much randomly, so
     * we should do some performance testing
     * */

    /* scan */
    if (ctx->scan_hash_size == 0) {
        if (mpm_ctx->scan_pattern_cnt < 50) {
            ctx->scan_hash_size = HASH9_SIZE;
        } else if(mpm_ctx->scan_pattern_cnt < 300) {
            ctx->scan_hash_size = HASH12_SIZE;
        } else if(mpm_ctx->scan_pattern_cnt < 1200) {
            ctx->scan_hash_size = HASH14_SIZE;
        } else if(mpm_ctx->scan_pattern_cnt < 2400) {
            ctx->scan_hash_size = HASH15_SIZE;
        } else {
            ctx->scan_hash_size = HASH16_SIZE;
        }
    }

    WmScanPrepareShiftTable(mpm_ctx);
    WmScanPrepareHash(mpm_ctx);

    if (ctx->scan_hash_size == HASH9_SIZE) {
        ctx->MBScan = WmScan2Hash9;
        ctx->Scan = WmScan2Hash9;
    } else if (ctx->scan_hash_size == HASH12_SIZE) {
        ctx->MBScan = WmScan2Hash12;
        ctx->Scan = WmScan2Hash12;
    } else if (ctx->scan_hash_size == HASH14_SIZE) {
        ctx->MBScan = WmScan2Hash14;
        ctx->Scan = WmScan2Hash14;
    } else if (ctx->scan_hash_size == HASH15_SIZE) {
        ctx->MBScan = WmScan2Hash15;
        ctx->Scan = WmScan2Hash15;
    } else {
        ctx->MBScan = WmScan2Hash16;
        ctx->Scan = WmScan2Hash16;
    }

    if (mpm_ctx->scan_minlen == 1) {
        ctx->Scan = WmScan1;
    }

    return 0;
error:
    return -1;
}

void WmPrintSearchStats(MpmThreadCtx *mpm_thread_ctx) {
#ifdef WUMANBER_COUNTERS
    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx->ctx;

    printf("Shift 0: %" PRIu32 "\n", tctx->scan_stat_shift_null);
    printf("Loop match: %" PRIu32 "\n", tctx->scan_stat_loop_match);
    printf("Loop no match: %" PRIu32 "\n", tctx->scan_stat_loop_no_match);
    printf("Num shifts: %" PRIu32 "\n", tctx->scan_stat_num_shift);
    printf("Total shifts: %" PRIu32 "\n", tctx->scan_stat_total_shift);
#endif /* WUMANBER_COUNTERS */
}

static inline int
memcmp_lowercase(uint8_t *s1, uint8_t *s2, uint16_t n) {
    size_t i;

    /* check backwards because we already tested the first
     * 2 to 4 chars. This way we are more likely to detect
     * a miss and thus speed up a little... */
    for (i = n - 1; i; i--) {
        if (wm_tolower(*(s2+i)) != s1[i])
            return 1;
    }

    return 0;
}

inline uint32_t WmScan(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
    return ctx->Scan(mpm_ctx, mpm_thread_ctx, pmq, buf, buflen);
}

/* SCAN FUNCTIONS */
uint32_t WmScan2Hash9(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
#ifdef WUMANBER_COUNTERS
    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx->ctx;
#endif /* WUMANBER_COUNTERS */
    uint32_t cnt = 0;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint16_t sl = ctx->scan_shiftlen;
    uint16_t h;
    uint8_t shift;
    WmHashItem *thi, *hi;
    WmPattern *p;
    uint16_t prefixci_buf;
    uint16_t prefixcs_buf;

    if (buflen == 0)
        return 0;

    //printf("BUF(%" PRIu32 ") ", buflen); prt(buf,buflen); printf(" (sl %" PRIu32 ")\n", sl);

    buf+=(sl-1);

    while (buf <= bufend) {
        h = HASH9(wm_tolower(*buf),(wm_tolower(*(buf-1))));
        shift = ctx->scan_shifttable[h];
        //printf("%p %" PRIu32 " search: h %" PRIu32 ", shift %" PRIu32 "\n", buf, buf - bufmin, h, shift);

        if (shift == 0) {
            COUNT(tctx->scan_stat_shift_null++);

            hi = ctx->scan_hash[h];
            //printf("search: hi %p\n", hi);
            if (hi != NULL) {
                /* get our patterns from the hash */
                if (ctx->scan_bloom[h] != NULL) {
                    COUNT(tctx->scan_stat_pminlen_calls++);
                    COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                    if ((bufend - (buf-sl)) < ctx->scan_pminlen[h]) {
                        goto skip_loop;
                    } else {
                        COUNT(tctx->scan_stat_bloom_calls++);

                        if (BloomFilterTest(ctx->scan_bloom[h], buf-sl+1, ctx->scan_pminlen[h]) == 0) {
                            COUNT(tctx->scan_stat_bloom_hits++);
                            goto skip_loop;
                        }
                    }
                }

                prefixci_buf = (uint16_t)(wm_tolower(*(buf-sl+1)) + wm_tolower(*(buf-sl+2)));
                prefixcs_buf = (uint16_t)(*(buf-sl+1) + *(buf-sl+2));

                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    //printf("WmSearch2: p->prefix_ci %" PRIu32 ", p->prefix_cs %" PRIu32 "\n",
                    //    p->prefix_ci, p->prefix_cs);

                    if (p->flags & WUMANBER_NOCASE) {
                        if (p->prefix_ci != prefixci_buf || p->len > (bufend-(buf-sl)))
                            continue;

                        if (memcmp_lowercase(p->ci, buf-sl+1, p->len) == 0) {
                            //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);

                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    } else {
                        if (p->prefix_cs != prefixcs_buf || p->len > (bufend-(buf-sl)))
                            continue;
                        if (memcmp(p->cs, buf-sl+1, p->len) == 0) {
                            //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);

                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    }
                }
            }
skip_loop:
            shift = 1;
        } else {
            COUNT(tctx->scan_stat_total_shift += shift);
            COUNT(tctx->scan_stat_num_shift++);
        }
        buf += shift;
    }

    //printf("cnt %" PRIu32 "\n", cnt);
    return cnt;
}

uint32_t WmScan2Hash12(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
#ifdef WUMANBER_COUNTERS
    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx->ctx;
#endif /* WUMANBER_COUNTERS */
    uint32_t cnt = 0;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint16_t sl = ctx->scan_shiftlen;
    uint16_t h;
    uint8_t shift;
    WmHashItem *thi, *hi;
    WmPattern *p;
    uint16_t prefixci_buf;
    uint16_t prefixcs_buf;

    if (buflen == 0)
        return 0;

    //printf("BUF(%" PRIu32 ") ", buflen); prt(buf,buflen); printf("\n");

    buf+=(sl-1);
    //buf++;

    while (buf <= bufend) {
        //h = (wm_tolower(*buf)<<8)+(wm_tolower(*(buf-1)));
        h = HASH12(wm_tolower(*buf),(wm_tolower(*(buf-1))));
        shift = ctx->scan_shifttable[h];
        //printf("search: h %" PRIu32 ", shift %" PRIu32 "\n", h, shift);

        if (shift == 0) {
            COUNT(tctx->scan_stat_shift_null++);
            /* get our hash item */
            hi = ctx->scan_hash[h];
            //printf("search: hi %p\n", hi);
            if (hi != NULL) {
                /* get our patterns from the hash */
                if (ctx->scan_bloom[h] != NULL) {
                    COUNT(tctx->scan_stat_pminlen_calls++);
                    COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                    if ((bufend - (buf-sl)) < ctx->scan_pminlen[h]) {
                        goto skip_loop;
                    } else {
                        COUNT(tctx->scan_stat_bloom_calls++);

                        if (BloomFilterTest(ctx->scan_bloom[h], buf-sl+1, ctx->scan_pminlen[h]) == 0) {
                            COUNT(tctx->scan_stat_bloom_hits++);
                            goto skip_loop;
                        }
                    }
                }

                prefixci_buf = (uint16_t)(wm_tolower(*(buf-sl+1)) + wm_tolower(*(buf-sl+2)));
                prefixcs_buf = (uint16_t)(*(buf-sl+1) + *(buf-sl+2));
                //printf("WmSearch2: prefixci_buf %" PRIu32 ", prefixcs_buf %" PRIu32 "\n", prefixci_buf, prefixcs_buf);
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    //printf("WmSearch2: p->prefix_ci %" PRIu32 ", p->prefix_cs %" PRIu32 "\n",
                    //    p->prefix_ci, p->prefix_cs);

                    if (p->flags & WUMANBER_NOCASE) {
                        if (p->prefix_ci != prefixci_buf || p->len > (bufend-(buf-sl)))
                            continue;

                        if (memcmp_lowercase(p->ci, buf-sl+1, p->len) == 0) {
                            //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);

                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    } else {
                        if (p->prefix_cs != prefixcs_buf || p->len > (bufend-(buf-sl)))
                            continue;
                        if (memcmp(p->cs, buf-sl+1, p->len) == 0) {
                            //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);

                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    }
                }
            }
skip_loop:
            shift = 1;
        } else {
            COUNT(tctx->scan_stat_total_shift += shift);
            COUNT(tctx->scan_stat_num_shift++);
        }
        buf += shift;
    }

    return cnt;
}

uint32_t WmScan2Hash14(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
#ifdef WUMANBER_COUNTERS
    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx->ctx;
#endif /* WUMANBER_COUNTERS */
    uint32_t cnt = 0;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint16_t sl = ctx->scan_shiftlen;
    uint16_t h;
    uint8_t shift;
    WmHashItem *thi, *hi;
    WmPattern *p;
    uint16_t prefixci_buf;
    uint16_t prefixcs_buf;

    if (buflen == 0)
        return 0;

    //printf("BUF(%" PRIu32 ") ", buflen); prt(buf,buflen); printf("\n");

    buf+=(sl-1);
    //buf++;

    while (buf <= bufend) {
        //h = (wm_tolower(*buf)<<8)+(wm_tolower(*(buf-1)));
        h = HASH14(wm_tolower(*buf),(wm_tolower(*(buf-1))));
        shift = ctx->scan_shifttable[h];
        //printf("search: h %" PRIu32 ", shift %" PRIu32 "\n", h, shift);

        if (shift == 0) {
            COUNT(tctx->scan_stat_shift_null++);
            /* get our hash item */
            hi = ctx->scan_hash[h];
            //printf("search: hi %p\n", hi);
            if (hi != NULL) {
                /* get our patterns from the hash */
                if (ctx->scan_bloom[h] != NULL) {
                    COUNT(tctx->scan_stat_pminlen_calls++);
                    COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                    if ((bufend - (buf-sl)) < ctx->scan_pminlen[h]) {
                        goto skip_loop;
                    } else {
                        COUNT(tctx->scan_stat_bloom_calls++);

                        if (BloomFilterTest(ctx->scan_bloom[h], buf-sl+1, ctx->scan_pminlen[h]) == 0) {
                            COUNT(tctx->scan_stat_bloom_hits++);
                            goto skip_loop;
                        }
                    }
                }

                prefixci_buf = (uint16_t)(wm_tolower(*(buf-sl+1)) + wm_tolower(*(buf-sl+2)));
                prefixcs_buf = (uint16_t)(*(buf-sl+1) + *(buf-sl+2));
                //printf("WmSearch2: prefixci_buf %" PRIu32 ", prefixcs_buf %" PRIu32 "\n", prefixci_buf, prefixcs_buf);
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    //printf("WmSearch2: p->prefix_ci %" PRIu32 ", p->prefix_cs %" PRIu32 "\n",
                    //    p->prefix_ci, p->prefix_cs);

                    if (p->flags & WUMANBER_NOCASE) {
                        if (p->prefix_ci != prefixci_buf || p->len > (bufend-(buf-sl)))
                            continue;

                        if (memcmp_lowercase(p->ci, buf-sl+1, p->len) == 0) {
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);
                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    } else {
                        if (p->prefix_cs != prefixcs_buf || p->len > (bufend-(buf-sl)))
                            continue;
                        if (memcmp(p->cs, buf-sl+1, p->len) == 0) {
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);
                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    }
                }
            }
skip_loop:
            shift = 1;
        } else {
            COUNT(tctx->scan_stat_total_shift += shift);
            COUNT(tctx->scan_stat_num_shift++);
        }
        buf += shift;
    }

    return cnt;
}

uint32_t WmScan2Hash15(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
#ifdef WUMANBER_COUNTERS
    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx->ctx;
#endif /* WUMANBER_COUNTERS */
    uint32_t cnt = 0;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint16_t sl = ctx->scan_shiftlen;
    uint16_t h;
    uint8_t shift;
    WmHashItem *thi, *hi;
    WmPattern *p;
    uint16_t prefixci_buf;
    uint16_t prefixcs_buf;

    if (buflen == 0)
        return 0;

    //printf("BUF(%" PRIu32 ") ", buflen); prt(buf,buflen); printf("\n");

    buf+=(sl-1);
    //buf++;

    while (buf <= bufend) {
        //h = (wm_tolower(*buf)<<8)+(wm_tolower(*(buf-1)));
        h = HASH15(wm_tolower(*buf),(wm_tolower(*(buf-1))));
        shift = ctx->scan_shifttable[h];
        //printf("search: h %" PRIu32 ", shift %" PRIu32 "\n", h, shift);

        if (shift == 0) {
            COUNT(tctx->scan_stat_shift_null++);
            /* get our hash item */
            hi = ctx->scan_hash[h];
            //printf("search: hi %p\n", hi);
            if (hi != NULL) {
                /* get our patterns from the hash */
                if (ctx->scan_bloom[h] != NULL) {
                    COUNT(tctx->scan_stat_pminlen_calls++);
                    COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                    if ((bufend - (buf-sl)) < ctx->scan_pminlen[h]) {
                        goto skip_loop;
                    } else {
                        COUNT(tctx->scan_stat_bloom_calls++);

                        if (BloomFilterTest(ctx->scan_bloom[h], buf-sl+1, ctx->scan_pminlen[h]) == 0) {
                            COUNT(tctx->scan_stat_bloom_hits++);
                            goto skip_loop;
                        }
                    }
                }

                prefixci_buf = (uint16_t)(wm_tolower(*(buf-sl+1)) + wm_tolower(*(buf-sl+2)));
                prefixcs_buf = (uint16_t)(*(buf-sl+1) + *(buf-sl+2));
                //printf("WmSearch2: prefixci_buf %" PRIu32 ", prefixcs_buf %" PRIu32 "\n", prefixci_buf, prefixcs_buf);
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    //printf("WmSearch2: p->prefix_ci %" PRIu32 ", p->prefix_cs %" PRIu32 "\n",
                    //    p->prefix_ci, p->prefix_cs);

                    if (p->flags & WUMANBER_NOCASE) {
                        if (p->prefix_ci != prefixci_buf || p->len > (bufend-(buf-sl)))
                            continue;

                        if (memcmp_lowercase(p->ci, buf-sl+1, p->len) == 0) {
                            //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);
                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    } else {
                        if (p->prefix_cs != prefixcs_buf || p->len > (bufend-(buf-sl)))
                            continue;
                        if (memcmp(p->cs, buf-sl+1, p->len) == 0) {
                            //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);
                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    }
                }
            }
skip_loop:
            shift = 1;
        } else {
            COUNT(tctx->scan_stat_total_shift += shift);
            COUNT(tctx->scan_stat_num_shift++);
        }
        buf += shift;
    }

    return cnt;
}

uint32_t WmScan2Hash16(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
#ifdef WUMANBER_COUNTERS
    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx->ctx;
#endif /* WUMANBER_COUNTERS */
    uint32_t cnt = 0;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint16_t sl = ctx->scan_shiftlen;
    uint16_t h;
    uint8_t shift;
    WmHashItem *thi, *hi;
    WmPattern *p;
    uint16_t prefixci_buf;
    uint16_t prefixcs_buf;

    if (buflen == 0)
        return 0;

    //printf("BUF(%" PRIu32 ") ", buflen); prt(buf,buflen); printf("\n");

    buf+=(sl-1);
    //buf++;

    while (buf <= bufend) {
        //h = (wm_tolower(*buf)<<8)+(wm_tolower(*(buf-1)));
        h = HASH16(wm_tolower(*buf),(wm_tolower(*(buf-1))));
        shift = ctx->scan_shifttable[h];
        //printf("search: h %" PRIu32 ", shift %" PRIu32 "\n", h, shift);

        if (shift == 0) {
            COUNT(tctx->scan_stat_shift_null++);
            /* get our hash item */
            hi = ctx->scan_hash[h];
            //printf("search: hi %p\n", hi);
            if (hi != NULL) {
                /* get our patterns from the hash */
                if (ctx->scan_bloom[h] != NULL) {
                    COUNT(tctx->scan_stat_pminlen_calls++);
                    COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                    if ((bufend - (buf-sl)) < ctx->scan_pminlen[h]) {
                        goto skip_loop;
                    } else {
                        COUNT(tctx->scan_stat_bloom_calls++);

                        if (BloomFilterTest(ctx->scan_bloom[h], buf-sl+1, ctx->scan_pminlen[h]) == 0) {
                            COUNT(tctx->scan_stat_bloom_hits++);
                            goto skip_loop;
                        }
                    }
                }

                prefixci_buf = (uint16_t)(wm_tolower(*(buf-sl+1)) + wm_tolower(*(buf-sl+2)));
                prefixcs_buf = (uint16_t)(*(buf-sl+1) + *(buf-sl+2));
                //printf("WmSearch2: prefixci_buf %" PRIu32 ", prefixcs_buf %" PRIu32 "\n", prefixci_buf, prefixcs_buf);
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    //printf("WmSearch2: p->prefix_ci %" PRIu32 ", p->prefix_cs %" PRIu32 "\n",
                    //    p->prefix_ci, p->prefix_cs);

                    if (p->flags & WUMANBER_NOCASE) {
                        if (p->prefix_ci != prefixci_buf || p->len > (bufend-(buf-sl)))
                            continue;

                        if (memcmp_lowercase(p->ci, buf-sl+1, p->len) == 0) {
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);
                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    } else {
                        if (p->prefix_cs != prefixcs_buf || p->len > (bufend-(buf-sl)))
                            continue;
                        if (memcmp(p->cs, buf-sl+1, p->len) == 0) {
                            COUNT(tctx->scan_stat_loop_match++);

                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf-sl+1 - bufmin), p->len);
                        } else {
                            COUNT(tctx->scan_stat_loop_no_match++);
                        }
                    }
                }
            }
skip_loop:
            shift = 1;
        } else {
            COUNT(tctx->scan_stat_total_shift += shift);
            COUNT(tctx->scan_stat_num_shift++);
        }
        buf += shift;
    }

    return cnt;
}

uint32_t WmScan1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint32_t cnt = 0;
    WmPattern *p;
    WmHashItem *thi, *hi;

    if (buflen == 0)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    if (mpm_ctx->scan_minlen == 1) {
        while (buf <= bufend) {
            uint8_t h = wm_tolower(*buf);
            hi = &ctx->scan_hash1[h];

            if (hi->flags & 0x01) {
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    if (p->len != 1)
                        continue;

                    if (p->flags & WUMANBER_NOCASE) {
                        if (wm_tolower(*buf) == p->ci[0]) {
                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf+1 - bufmin), p->len);
                        }
                    } else {
                        if (*buf == p->cs[0]) {
                            cnt += MpmVerifyMatch(mpm_thread_ctx, pmq, p->em, (buf+1 - bufmin), p->len);
                        }
                    }
                }
            }
            buf += 1;
        }
    }
    //printf("WmScan1: after 1byte cnt %" PRIu32 "\n", cnt);
    if (mpm_ctx->scan_maxlen > 1) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBScan(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("WmScan1: after 2+byte cnt %" PRIu32 "\n", cnt);
    }
    return cnt;
}

/**
 * \brief   Function to get the user defined values for wumanber algorithm from
 *          the config file 'suricata.yaml'
 */
void WmGetConfig()
{
    ConfNode *wm_conf;
    const char *hash_val = NULL;
    const char *bloom_val = NULL;

    /* init defaults */
    wm_hash_size = HASHSIZE_LOW;
    wm_bloom_size = BLOOMSIZE_MEDIUM;

    ConfNode *pm = ConfGetNode("pattern-matcher");

    if (pm != NULL) {

        TAILQ_FOREACH(wm_conf, &pm->head, next) {
            if (strncmp(wm_conf->val, "wumanber", 8) == 0) {
                hash_val = ConfNodeLookupChildValue(wm_conf->head.tqh_first,
                                                    "hash_size");
                bloom_val = ConfNodeLookupChildValue(wm_conf->head.tqh_first,
                                                     "bf_size");

                if (hash_val != NULL)
                    wm_hash_size = MpmGetHashSize(hash_val);

                if (bloom_val != NULL)
                    wm_bloom_size = MpmGetBloomSize(bloom_val);

                SCLogDebug("hash size is %"PRIu32" and bloom size is %"PRIu32"",
                        wm_hash_size, wm_bloom_size);
            }
        }
    }
}

void WmInitCtx (MpmCtx *mpm_ctx, int module_handle) {
    SCLogDebug("mpm_ctx %p", mpm_ctx);

    mpm_ctx->ctx = SCMalloc(sizeof(WmCtx));
    if (mpm_ctx->ctx == NULL)
        return;

    memset(mpm_ctx->ctx, 0, sizeof(WmCtx));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(WmCtx);

    /* initialize the hash we use to speed up pattern insertions */
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
    ctx->init_hash = SCMalloc(sizeof(WmPattern *) * INIT_HASH_SIZE);
    if (ctx->init_hash == NULL)
        return;

    memset(ctx->init_hash, 0, sizeof(WmPattern *) * INIT_HASH_SIZE);

    /* Initialize the defaults value from the config file. The given check make
       sure that we query config file only once for config values */
    if (wm_hash_size == 0)
        WmGetConfig();

}

void WmDestroyCtx(MpmCtx *mpm_ctx) {
    WmCtx *ctx = (WmCtx *)mpm_ctx->ctx;
    if (ctx == NULL)
        return;

    if (ctx->init_hash) {
        SCFree(ctx->init_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (INIT_HASH_SIZE * sizeof(WmPattern *));
    }

    if (ctx->parray) {
        uint32_t i;
        for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
            if (ctx->parray[i] != NULL) {
                WmFreePattern(mpm_ctx, ctx->parray[i]);
            }
        }

        SCFree(ctx->parray);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (mpm_ctx->pattern_cnt * sizeof(WmPattern));
    }

    if (ctx->scan_bloom) {
        uint32_t h;
        for (h = 0; h < ctx->scan_hash_size; h++) {
            if (ctx->scan_bloom[h] == NULL)
                continue;

            mpm_ctx->memory_cnt -= BloomFilterMemoryCnt(ctx->scan_bloom[h]);
            mpm_ctx->memory_size -= BloomFilterMemorySize(ctx->scan_bloom[h]);

            BloomFilterFree(ctx->scan_bloom[h]);
        }

        SCFree(ctx->scan_bloom);

        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(BloomFilter *) * ctx->scan_hash_size);
    }

    if (ctx->scan_hash) {
        uint32_t h;
        for (h = 0; h < ctx->scan_hash_size; h++) {
            if (ctx->scan_hash[h] == NULL)
                continue;

            WmHashFree(mpm_ctx, ctx->scan_hash[h]);
        }

        SCFree(ctx->scan_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(WmHashItem) * ctx->scan_hash_size);
    }

    if (ctx->scan_shifttable) {
        SCFree(ctx->scan_shifttable);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(uint16_t) * ctx->scan_hash_size);
    }

    if (ctx->scan_pminlen) {
        SCFree(ctx->scan_pminlen);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(uint8_t) * ctx->scan_hash_size);
    }

    SCFree(mpm_ctx->ctx);
    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(WmCtx);
}

void WmThreadInitCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, uint32_t matchsize) {
    memset(mpm_thread_ctx, 0, sizeof(MpmThreadCtx));

    if (sizeof(WmThreadCtx) > 0) { /* size can be 0 when optimized */
        mpm_thread_ctx->ctx = SCMalloc(sizeof(WmThreadCtx));
        if (mpm_thread_ctx->ctx == NULL)
            return;

        memset(mpm_thread_ctx->ctx, 0, sizeof(WmThreadCtx));

        mpm_thread_ctx->memory_cnt++;
        mpm_thread_ctx->memory_size += sizeof(WmThreadCtx);
    }
}

void WmThreadDestroyCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx) {
    WmThreadCtx *ctx = (WmThreadCtx *)mpm_thread_ctx->ctx;
    if (ctx != NULL) { /* size can be 0 when optimized */
        mpm_thread_ctx->memory_cnt--;
        mpm_thread_ctx->memory_size -= sizeof(WmThreadCtx);
        SCFree(mpm_thread_ctx->ctx);
    }
}

/*
 * ONLY TESTS BELOW THIS COMMENT
 */

#ifdef UNITTESTS
int WmTestInitCtx01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    WmInitCtx(&mpm_ctx, -1);

    if (mpm_ctx.ctx != NULL)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitCtx02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    WmInitCtx(&mpm_ctx, -1);

    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    if (ctx->parray == NULL)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitCtx03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    if (mpm_table[MPM_WUMANBER].Scan == WmScan)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestThreadInitCtx01 (void) {
    if (sizeof(WmThreadCtx) > 0) {
        int result = 0;
        MpmCtx mpm_ctx;
        memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
        MpmThreadCtx mpm_thread_ctx;

        MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
        WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

        if (mpm_thread_ctx.memory_cnt == 2)
            result = 1;

        WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
        WmDestroyCtx(&mpm_ctx);
        return result;
    } else {
        return 1;
    }
}

int WmTestThreadInitCtx02 (void) {
#ifdef WUMANBER_COUNTERS
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    WmThreadCtx *tctx = (WmThreadCtx *)mpm_thread_ctx.ctx;

    if (tctx->search_stat_shift_null == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
#else
    int result = 1;
#endif
    return result;
}

int WmTestInitAddPattern01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    int ret = WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 0, 1234, 0, 0);
    if (ret == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitAddPattern02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 0, 1234, 0, 0);
    if (ctx->init_hash != NULL)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitAddPattern03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 0, 1234, 0, 0);
    WmPattern *pat = WmInitHashLookup(ctx, (uint8_t *)"abcd", 4, 1);
    if (pat != NULL) {
        if (pat->len == 4)
            result = 1;
    }

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitAddPattern04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 0, 1234, 0, 0);
    WmPattern *pat = WmInitHashLookup(ctx, (uint8_t *)"abcd", 4, 1);
    if (pat != NULL) {
        if (pat->flags & WUMANBER_NOCASE)
            result = 1;
    }

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitAddPattern05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 1234, 0, 0);
    WmPattern *pat = WmInitHashLookup(ctx, (uint8_t *)"abcd", 4, 0);
    if (pat != NULL) {
        if (!(pat->flags & WUMANBER_NOCASE))
            result = 1;
    }

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestInitAddPattern06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 0, 1234, 0, 0);
    WmPattern *pat = WmInitHashLookup(ctx, (uint8_t *)"abcd", 4, 1);
    if (pat != NULL) {
        if (memcmp(pat->cs, "abcd", 4) == 0)
            result = 1;
    }

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestPrepare01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"a", 1, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);

    if (ctx->Scan == WmScan1)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestPrepare02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    if (ctx->scan_shiftlen == 4)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestPrepare03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    if (ctx->scan_shifttable[1] == 4)
        result = 1;
    else
        printf("4 != %" PRIu32 ": ", ctx->scan_shifttable[1]);

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestPrepare04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"a", 1, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);

    if (ctx->Scan == WmScan1)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestPrepare05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    if (ctx->scan_shiftlen == 4)
        result = 1;

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestPrepare06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    if (ctx->scan_shifttable[1] == 4)
        result = 1;
    else
        printf("4 != %" PRIu32 ": ", ctx->scan_shifttable[1]);

    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //mpm_ctx.PrintCtx(&mpm_ctx);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch01Hash12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);

    ctx->scan_hash_size = HASH12_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //mpm_ctx.PrintCtx(&mpm_ctx);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch01Hash14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);

    ctx->scan_hash_size = HASH14_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //mpm_ctx.PrintCtx(&mpm_ctx);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch01Hash15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);

    ctx->scan_hash_size = HASH15_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //mpm_ctx.PrintCtx(&mpm_ctx);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch01Hash16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;

    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);

    ctx->scan_hash_size = HASH16_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //mpm_ctx.PrintCtx(&mpm_ctx);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abce", 4);

    if (cnt == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);
    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"bcde", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"efgh", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"eFgH", 4, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdEfGh", 8);

    if (cnt == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch07 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"eFgH", 4, 0, 0, 1, 1, 1, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdEfGh", 8);

    if (cnt == 2)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch08 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"abcde", 5, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"bcde",  4, 0, 0, 1, 1, 1, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 2)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch09 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"ab", 2, 0, 0, 1, 1, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"ab", 2);

    if (cnt == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch10 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    WmAddPattern(&mpm_ctx, (uint8_t *)"bc", 2, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"gh", 2, 0, 0, 1, 1, 1, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 2)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch11 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);

    WmAddPattern(&mpm_ctx, (uint8_t *)"a", 1, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"d", 1, 0, 0, 1, 1, 1, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"h", 1, 0, 0, 1, 1, 2, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 3)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"d", 1, 0, 0, 1, 1, 1, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"Z", 1, 0, 0, 1, 1, 2, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 ": ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch13 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"a", 1, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"de",2, 0, 0, 1, 1, 1, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"h", 1, 0, 0, 1, 1, 2, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 ": ", cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"de",2, 0, 0, 1, 1, 1, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"Z", 1, 0, 0, 1, 1, 2, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    if (cnt == 2)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

/** \todo VJ disabled because it tests the old match storage */
#if 0
int WmTestSearch15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"de",2, 0, 0, 1, 1, 1, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"Z", 1, 0, 0, 1, 1, 2, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    uint32_t len = mpm_thread_ctx.match[1].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddPattern(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 1, 1, 0, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"de",2, 0, 0, 1, 1, 1, 0, 0);
    WmAddPattern(&mpm_ctx, (uint8_t *)"Z", 1, 0, 0, 1, 1, 2, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefgh", 8);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch17 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch18Hash12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH12_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;

    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch18Hash14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH14_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;

    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch18Hash15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH15_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;

    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch18 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH16_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch18Hash16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH16_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch19 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCI(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch19Hash12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCI(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH12_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch19Hash14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCI(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH14_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch19Hash15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCI(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH15_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch19Hash16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCI(&mpm_ctx, (uint8_t *)"/VideoAccessCodecInstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH16_SIZE;
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstaLL.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch20 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch20Hash12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH12_SIZE; /* force hash12 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;

   if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch20Hash14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH14_SIZE; /* force hash14 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch20Hash15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH15_SIZE; /* force hash15 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch20Hash16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH16_SIZE; /* force hash16 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/VideoAccessCodecInstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 0)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

int WmTestSearch21 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);

    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/videoaccesscodecinstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch21Hash12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH12_SIZE; /* force hash16 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //WmPrintInfo(&mpm_ctx);
    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/videoaccesscodecinstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch21Hash14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH14_SIZE; /* force hash16 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //WmPrintInfo(&mpm_ctx);
    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/videoaccesscodecinstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch21Hash15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH15_SIZE; /* force hash16 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //WmPrintInfo(&mpm_ctx);
    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/videoaccesscodecinstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch21Hash16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"/videoaccesscodecinstall.exe", 28, 0, 0, 0, 0, 0);
    ctx->scan_hash_size = HASH16_SIZE; /* force hash16 */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1);
    //WmPrintInfo(&mpm_ctx);
    ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"/videoaccesscodecinstall.exe", 28);

    uint32_t len = mpm_thread_ctx.match[0].len;


    if (len == 1)
        result = 1;

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}
#endif
static int WmTestSearch22Hash9 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0); /* should match 30 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0); /* should match 29 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0); /* should match 28 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0); /* 26 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0); /* 21 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0, 0); /* 1 */
    /* total matches: 135 */

    ctx->scan_hash_size = HASH9_SIZE; /* force hash size */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);


    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch22Hash12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0); /* should match 30 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0); /* should match 29 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0); /* should match 28 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0); /* 26 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0); /* 21 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0, 0); /* 1 */
    /* total matches: 135 */

    ctx->scan_hash_size = HASH12_SIZE; /* force hash size */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);


    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch22Hash14 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0); /* should match 30 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0); /* should match 29 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0); /* should match 28 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0); /* 26 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0); /* 21 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0, 0); /* 1 */
    /* total matches: 135 */

    ctx->scan_hash_size = HASH14_SIZE; /* force hash size */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);


    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch22Hash15 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0); /* should match 30 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0); /* should match 29 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0); /* should match 28 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0); /* 26 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0); /* 21 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0, 0); /* 1 */
    /* total matches: 135 */

    ctx->scan_hash_size = HASH15_SIZE; /* force hash size */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);


    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}

static int WmTestSearch22Hash16 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    memset(&mpm_ctx, 0x00, sizeof(MpmCtx));
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_WUMANBER, -1);
    WmCtx *ctx = (WmCtx *)mpm_ctx.ctx;

    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0); /* should match 30 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0); /* should match 29 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0); /* should match 28 times */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0); /* 26 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0); /* 21 */
    WmAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0, 0); /* 1 */
    /* total matches: 135 */

    ctx->scan_hash_size = HASH16_SIZE; /* force hash size */
    WmScanPreparePatterns(&mpm_ctx);
    WmThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = ctx->Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);


    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    WmThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    WmDestroyCtx(&mpm_ctx);
    return result;
}
#endif /* UNITTESTS */

void WmRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("WmTestInitCtx01", WmTestInitCtx01, 1);
    UtRegisterTest("WmTestInitCtx02", WmTestInitCtx02, 1);
    UtRegisterTest("WmTestInitCtx03", WmTestInitCtx03, 1);

    UtRegisterTest("WmTestThreadInitCtx01", WmTestThreadInitCtx01, 1);
    UtRegisterTest("WmTestThreadInitCtx02", WmTestThreadInitCtx02, 1);

    UtRegisterTest("WmTestInitAddPattern01", WmTestInitAddPattern01, 1);
    UtRegisterTest("WmTestInitAddPattern02", WmTestInitAddPattern02, 1);
    UtRegisterTest("WmTestInitAddPattern03", WmTestInitAddPattern03, 1);
    UtRegisterTest("WmTestInitAddPattern04", WmTestInitAddPattern04, 1);
    UtRegisterTest("WmTestInitAddPattern05", WmTestInitAddPattern05, 1);
    UtRegisterTest("WmTestInitAddPattern06", WmTestInitAddPattern06, 1);

    UtRegisterTest("WmTestPrepare01", WmTestPrepare01, 1);
    UtRegisterTest("WmTestPrepare02", WmTestPrepare02, 1);
    UtRegisterTest("WmTestPrepare03", WmTestPrepare03, 1);
    UtRegisterTest("WmTestPrepare04", WmTestPrepare01, 1);
    UtRegisterTest("WmTestPrepare05", WmTestPrepare02, 1);
    UtRegisterTest("WmTestPrepare06", WmTestPrepare03, 1);

    UtRegisterTest("WmTestSearch01", WmTestSearch01, 1);
    UtRegisterTest("WmTestSearch01Hash12", WmTestSearch01Hash12, 1);
    UtRegisterTest("WmTestSearch01Hash14", WmTestSearch01Hash14, 1);
    UtRegisterTest("WmTestSearch01Hash15", WmTestSearch01Hash15, 1);
    UtRegisterTest("WmTestSearch01Hash16", WmTestSearch01Hash16, 1);

    UtRegisterTest("WmTestSearch02", WmTestSearch02, 1);
    UtRegisterTest("WmTestSearch03", WmTestSearch03, 1);
    UtRegisterTest("WmTestSearch04", WmTestSearch04, 1);
    UtRegisterTest("WmTestSearch05", WmTestSearch05, 1);
    UtRegisterTest("WmTestSearch06", WmTestSearch06, 1);
    UtRegisterTest("WmTestSearch07", WmTestSearch07, 1);
    UtRegisterTest("WmTestSearch08", WmTestSearch08, 1);
    UtRegisterTest("WmTestSearch09", WmTestSearch09, 1);
    UtRegisterTest("WmTestSearch10", WmTestSearch10, 1);
    UtRegisterTest("WmTestSearch11", WmTestSearch11, 1);
    UtRegisterTest("WmTestSearch12", WmTestSearch12, 1);
    UtRegisterTest("WmTestSearch13", WmTestSearch13, 1);

    UtRegisterTest("WmTestSearch14", WmTestSearch14, 1);
#if 0
    UtRegisterTest("WmTestSearch15", WmTestSearch15, 1);
    UtRegisterTest("WmTestSearch16", WmTestSearch16, 1);
    UtRegisterTest("WmTestSearch17", WmTestSearch17, 1);

    UtRegisterTest("WmTestSearch18", WmTestSearch18, 1);
    UtRegisterTest("WmTestSearch18Hash12", WmTestSearch18Hash12, 1);
    UtRegisterTest("WmTestSearch18Hash14", WmTestSearch18Hash14, 1);
    UtRegisterTest("WmTestSearch18Hash15", WmTestSearch18Hash15, 1);
    UtRegisterTest("WmTestSearch18Hash16", WmTestSearch18Hash16, 1);

    UtRegisterTest("WmTestSearch19", WmTestSearch19, 1);
    UtRegisterTest("WmTestSearch19Hash12", WmTestSearch19Hash12, 1);
    UtRegisterTest("WmTestSearch19Hash14", WmTestSearch19Hash14, 1);
    UtRegisterTest("WmTestSearch19Hash15", WmTestSearch19Hash15, 1);
    UtRegisterTest("WmTestSearch19Hash16", WmTestSearch19Hash16, 1);

    UtRegisterTest("WmTestSearch20", WmTestSearch20, 1);
    UtRegisterTest("WmTestSearch20Hash12", WmTestSearch20Hash12, 1);
    UtRegisterTest("WmTestSearch20Hash14", WmTestSearch20Hash14, 1);
    UtRegisterTest("WmTestSearch20Hash15", WmTestSearch20Hash15, 1);
    UtRegisterTest("WmTestSearch20Hash16", WmTestSearch20Hash16, 1);

    UtRegisterTest("WmTestSearch21", WmTestSearch21, 1);
    UtRegisterTest("WmTestSearch21Hash12", WmTestSearch21Hash12, 1);
    UtRegisterTest("WmTestSearch21Hash14", WmTestSearch21Hash14, 1);
    UtRegisterTest("WmTestSearch21Hash15", WmTestSearch21Hash15, 1);
    UtRegisterTest("WmTestSearch21Hash16", WmTestSearch21Hash16, 1);
#endif
    UtRegisterTest("WmTestSearch22Hash9", WmTestSearch22Hash9, 1);
    UtRegisterTest("WmTestSearch22Hash12", WmTestSearch22Hash12, 1);
    UtRegisterTest("WmTestSearch22Hash14", WmTestSearch22Hash14, 1);
    UtRegisterTest("WmTestSearch22Hash15", WmTestSearch22Hash15, 1);
    UtRegisterTest("WmTestSearch22Hash16", WmTestSearch22Hash16, 1);
#endif /* UNITTESTS */
}

