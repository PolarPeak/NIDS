/* Copyright (C) 2015-2016 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "suricata-common.h"
#include "util-streaming-buffer.h"
#include "util-unittest.h"
#include "util-print.h"
#include "util-validate.h"

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 *  \brief Streaming Buffer API
 */

/* memory handling wrappers. If config doesn't define it's own set of
 * functions, use the defaults */
#define MALLOC(cfg, s) \
    (cfg)->Malloc ? (cfg)->Malloc((s)) : SCMalloc((s))
#define CALLOC(cfg, n, s) \
    (cfg)->Calloc ? (cfg)->Calloc((n), (s)) : SCCalloc((n), (s))
#define REALLOC(cfg, ptr, orig_s, s) \
    (cfg)->Realloc ? (cfg)->Realloc((ptr), (orig_s), (s)) : SCRealloc((ptr), (s))
#define FREE(cfg, ptr, s) \
    (cfg)->Free ? (cfg)->Free((ptr), (s)) : SCFree((ptr))

static void SBBFree(StreamingBuffer *sb);

RB_GENERATE(SBB, StreamingBufferBlock, rb, SBBCompare);

int SBBCompare(struct StreamingBufferBlock *a, struct StreamingBufferBlock *b)
{
    SCLogDebug("a %"PRIu64" len %u, b %"PRIu64" len %u",
            a->offset, a->len, b->offset, b->len);

    if (a->offset > b->offset)
        SCReturnInt(1);
    else if (a->offset < b->offset)
        SCReturnInt(-1);
    else {
        if (a->len == 0 || b->len == 0 || a->len ==  b->len)
            SCReturnInt(0);
        else if (a->len > b->len)
            SCReturnInt(1);
        else
            SCReturnInt(-1);
    }
}

/* inclusive compare function that also considers the right edge,
 * not just the offset. */
static inline int InclusiveCompare(StreamingBufferBlock *lookup, StreamingBufferBlock *intree) {
    const uint64_t lre = lookup->offset + lookup->len;
    const uint64_t tre = intree->offset + intree->len;
    if (lre <= intree->offset)   // entirely before
        return -1;
    else if (lre >= intree->offset && lookup->offset < tre && lre <= tre)   // (some) overlap
        return 0;
    else
        return 1;   // entirely after
}

StreamingBufferBlock *SBB_RB_FIND_INCLUSIVE(struct SBB *head, StreamingBufferBlock *elm)
{
    SCLogDebug("looking up %"PRIu64, elm->offset);

    struct StreamingBufferBlock *tmp = RB_ROOT(head);
    struct StreamingBufferBlock *res = NULL;
    while (tmp) {
        SCLogDebug("compare with %"PRIu64"/%u", tmp->offset, tmp->len);
        const int comp = InclusiveCompare(elm, tmp);
        SCLogDebug("compare result: %d", comp);
        if (comp < 0) {
            res = tmp;
            tmp = RB_LEFT(tmp, rb);
        } else if (comp > 0) {
            tmp = RB_RIGHT(tmp, rb);
        } else {
            return tmp;
        }
    }
    return res;
}


static inline int InitBuffer(StreamingBuffer *sb)
{
    sb->buf = CALLOC(sb->cfg, 1, sb->cfg->buf_size);
    if (sb->buf == NULL) {
        return -1;
    }
    sb->buf_size = sb->cfg->buf_size;
    return 0;
}

StreamingBuffer *StreamingBufferInit(const StreamingBufferConfig *cfg)
{
    StreamingBuffer *sb = CALLOC(cfg, 1, sizeof(StreamingBuffer));
    if (sb != NULL) {
        sb->buf_size = cfg->buf_size;
        sb->cfg = cfg;

        if (cfg->buf_size > 0) {
            if (InitBuffer(sb) == 0) {
                return sb;
            }
            FREE(cfg, sb, sizeof(StreamingBuffer));
        /* implied buf_size == 0 */
        } else {
            return sb;
        }
    }
    return NULL;
}

void StreamingBufferClear(StreamingBuffer *sb)
{
    if (sb != NULL) {
        SCLogDebug("sb->buf_size %u max %u", sb->buf_size, sb->buf_size_max);

        SBBFree(sb);
        if (sb->buf != NULL) {
            FREE(sb->cfg, sb->buf, sb->buf_size);
            sb->buf = NULL;
        }
    }
}

void StreamingBufferFree(StreamingBuffer *sb)
{
    if (sb != NULL) {
        StreamingBufferClear(sb);
        FREE(sb->cfg, sb, sizeof(StreamingBuffer));
    }
}

#ifdef DEBUG
static void SBBPrintList(StreamingBuffer *sb)
{
    StreamingBufferBlock *sbb = NULL;
    RB_FOREACH(sbb, SBB, &sb->sbb_tree) {
        SCLogDebug("sbb: offset %"PRIu64", len %u", sbb->offset, sbb->len);
        StreamingBufferBlock *next = SBB_RB_NEXT(sbb);
        if (next) {
            if ((sbb->offset + sbb->len) != next->offset) {
                SCLogDebug("gap: offset %"PRIu64", len %"PRIu64, (sbb->offset + sbb->len),
                        next->offset - (sbb->offset + sbb->len));
            }
        }
    }
}
#endif

/* setup with gap between 2 blocks
 *
 * [block][gap][block]
 **/
static void SBBInit(StreamingBuffer *sb,
                    uint32_t rel_offset, uint32_t data_len)
{
    DEBUG_VALIDATE_BUG_ON(!RB_EMPTY(&sb->sbb_tree));
    DEBUG_VALIDATE_BUG_ON(sb->buf_offset > sb->stream_offset + rel_offset);

    /* need to set up 2: existing data block and new data block */
    StreamingBufferBlock *sbb = CALLOC(sb->cfg, 1, sizeof(*sbb));
    if (sbb == NULL) {
        return;
    }
    sbb->offset = sb->stream_offset;
    sbb->len = sb->buf_offset;

    StreamingBufferBlock *sbb2 = CALLOC(sb->cfg, 1, sizeof(*sbb2));
    if (sbb2 == NULL) {
        FREE(sb->cfg, sbb, sizeof(*sbb));
        return;
    }
    sbb2->offset = sb->stream_offset + rel_offset;
    sbb2->len = data_len;

    sb->head = sbb;
    SBB_RB_INSERT(&sb->sbb_tree, sbb);
    SBB_RB_INSERT(&sb->sbb_tree, sbb2);

    SCLogDebug("sbb1 %"PRIu64", len %u, sbb2 %"PRIu64", len %u",
            sbb->offset, sbb->len, sbb2->offset, sbb2->len);
#ifdef DEBUG
    SBBPrintList(sb);
#endif
    BUG_ON(sbb2->offset < sbb->len);
}

/* setup with leading gap
 *
 * [gap][block]
 **/
static void SBBInitLeadingGap(StreamingBuffer *sb,
                              uint64_t offset, uint32_t data_len)
{
    DEBUG_VALIDATE_BUG_ON(!RB_EMPTY(&sb->sbb_tree));

    StreamingBufferBlock *sbb = CALLOC(sb->cfg, 1, sizeof(*sbb));
    if (sbb == NULL)
        return;
    sbb->offset = offset;
    sbb->len = data_len;

    sb->head = sbb;
    SBB_RB_INSERT(&sb->sbb_tree, sbb);

    SCLogDebug("sbb %"PRIu64", len %u",
            sbb->offset, sbb->len);
#ifdef DEBUG
    SBBPrintList(sb);
#endif
}

static inline void ConsolidateFwd(StreamingBuffer *sb,
        struct SBB *tree, StreamingBufferBlock *sa)
{
    uint64_t sa_re = sa->offset + sa->len;
    StreamingBufferBlock *tr, *s = sa;
    RB_FOREACH_FROM(tr, SBB, s) {
        if (sa == tr)
            continue;

        const uint64_t tr_re = tr->offset + tr->len;
        SCLogDebug("-> (fwd) tr %p %"PRIu64"/%u re %"PRIu64,
                tr, tr->offset, tr->len, tr_re);

        if (sa_re < tr->offset)
            break; // entirely before

        /*
            sa:     [   ]
            tr: [           ]
            sa:     [   ]
            tr:     [       ]
            sa:     [   ]
            tr: [       ]
        */
        if (sa->offset >= tr->offset && sa_re <= tr_re) {
            sa->len = tr->len;
            sa->offset = tr->offset;
            sa_re = sa->offset + sa->len;
            SCLogDebug("-> (fwd) tr %p %"PRIu64"/%u REMOVED ECLIPSED2", tr, tr->offset, tr->len);
            SBB_RB_REMOVE(tree, tr);
            FREE(sb->cfg, tr, sizeof(StreamingBufferBlock));
        /*
            sa: [         ]
            tr: [         ]
            sa: [         ]
            tr:    [      ]
            sa: [         ]
            tr:    [   ]
        */
        } else if (sa->offset <= tr->offset && sa_re >= tr_re) {
            SCLogDebug("-> (fwd) tr %p %"PRIu64"/%u REMOVED ECLIPSED", tr, tr->offset, tr->len);
            SBB_RB_REMOVE(tree, tr);
            FREE(sb->cfg, tr, sizeof(StreamingBufferBlock));
        /*
            sa: [         ]
            tr:      [         ]
            sa: [       ]
            tr:         [       ]
        */
        } else if (sa->offset < tr->offset && // starts before
                   sa_re >= tr->offset && sa_re < tr_re) // ends inside
        {
            // merge
            sa->len = tr_re - sa->offset;
            sa_re = sa->offset + sa->len;
            SCLogDebug("-> (fwd) tr %p %"PRIu64"/%u REMOVED MERGED", tr, tr->offset, tr->len);
            SBB_RB_REMOVE(tree, tr);
            FREE(sb->cfg, tr, sizeof(StreamingBufferBlock));
        }
    }
}

static inline void ConsolidateBackward(StreamingBuffer *sb,
        struct SBB *tree, StreamingBufferBlock *sa)
{
    uint64_t sa_re = sa->offset + sa->len;
    StreamingBufferBlock *tr, *s = sa;
    RB_FOREACH_REVERSE_FROM(tr, SBB, s) {
        if (sa == tr)
            continue;
        const uint64_t tr_re = tr->offset + tr->len;
        SCLogDebug("-> (bwd) tr %p %"PRIu64"/%u", tr, tr->offset, tr->len);

        if (sa->offset > tr_re)
            break; // entirely after

        if (sa->offset >= tr->offset && sa_re <= tr_re) {
            sa->len = tr->len;
            sa->offset = tr->offset;
            sa_re = sa->offset + sa->len;
            SCLogDebug("-> (bwd) tr %p %"PRIu64"/%u REMOVED ECLIPSED2", tr, tr->offset, tr->len);
            if (sb->head == tr)
                sb->head = sa;
            SBB_RB_REMOVE(tree, tr);
            FREE(sb->cfg, tr, sizeof(StreamingBufferBlock));
        /*
            sa: [         ]
            tr: [         ]
            sa:    [      ]
            tr: [         ]
            sa:    [   ]
            tr: [         ]
        */
        } else if (sa->offset <= tr->offset && sa_re >= tr_re) {
            SCLogDebug("-> (bwd) tr %p %"PRIu64"/%u REMOVED ECLIPSED", tr, tr->offset, tr->len);
            if (sb->head == tr)
                sb->head = sa;
            SBB_RB_REMOVE(tree, tr);
            FREE(sb->cfg, tr, sizeof(StreamingBufferBlock));
        /*
            sa:     [   ]
            tr: [   ]
            sa:    [    ]
            tr: [   ]
        */
        } else if (sa->offset > tr->offset && sa_re > tr_re && sa->offset <= tr_re) {
            // merge
            sa->len = sa_re - tr->offset;
            sa->offset = tr->offset;
            sa_re = sa->offset + sa->len;
            SCLogDebug("-> (bwd) tr %p %"PRIu64"/%u REMOVED MERGED", tr, tr->offset, tr->len);
            if (sb->head == tr)
                sb->head = sa;
            SBB_RB_REMOVE(tree, tr);
            FREE(sb->cfg, tr, sizeof(StreamingBufferBlock));
        }
    }
}

static int Insert(StreamingBuffer *sb, struct SBB *tree,
        uint32_t rel_offset, uint32_t len)
{
    SCLogDebug("* inserting: %u/%u\n", rel_offset, len);

    StreamingBufferBlock *sbb = CALLOC(sb->cfg, 1, sizeof(*sbb));
    if (sbb == NULL)
        return -1;
    sbb->offset = sb->stream_offset + rel_offset;
    sbb->len = len;
    StreamingBufferBlock *res = SBB_RB_INSERT(tree, sbb);
    if (res) {
        // exact overlap
        SCLogDebug("* insert failed: exact match in tree with %p %"PRIu64"/%u", res, res->offset, res->len);
        FREE(sb->cfg, sbb, sizeof(StreamingBufferBlock));
        return 0;
    }
    if (SBB_RB_PREV(sbb) == NULL) {
        sb->head = sbb;
    } else {
        ConsolidateBackward(sb, tree, sbb);
    }
    ConsolidateFwd(sb, tree, sbb);
#ifdef DEBUG
    SBBPrintList(sb);
#endif
    return 0;
}

static void SBBUpdate(StreamingBuffer *sb,
                      uint32_t rel_offset, uint32_t data_len)
{
    Insert(sb, &sb->sbb_tree, rel_offset, data_len);
}

static void SBBFree(StreamingBuffer *sb)
{
    StreamingBufferBlock *sbb = NULL, *safe = NULL;
    RB_FOREACH_SAFE(sbb, SBB, &sb->sbb_tree, safe) {
        SBB_RB_REMOVE(&sb->sbb_tree, sbb);
        FREE(sb->cfg, sbb, sizeof(StreamingBufferBlock));
    }
    sb->head = NULL;
}

static void SBBPrune(StreamingBuffer *sb)
{
    SCLogDebug("pruning %p to %"PRIu64, sb, sb->stream_offset);
    StreamingBufferBlock *sbb = NULL, *safe = NULL;
    RB_FOREACH_SAFE(sbb, SBB, &sb->sbb_tree, safe) {
        /* completely beyond window, we're done */
        if (sbb->offset > sb->stream_offset) {
            sb->head = sbb;
            break;
        }

        /* partly before, partly beyond. Adjust */
        if (sbb->offset < sb->stream_offset &&
            sbb->offset + sbb->len > sb->stream_offset) {
            uint32_t shrink_by = sb->stream_offset - sbb->offset;
            DEBUG_VALIDATE_BUG_ON(shrink_by > sbb->len);
            if (sbb->len >= shrink_by) {
                sbb->len -=  shrink_by;
                sbb->offset += shrink_by;
                DEBUG_VALIDATE_BUG_ON(sbb->offset != sb->stream_offset);
            }
            sb->head = sbb;
            break;
        }

        SBB_RB_REMOVE(&sb->sbb_tree, sbb);
        /* either we set it again for the next sbb, or there isn't any */
        sb->head = NULL;
        SCLogDebug("sb %p removed %p %"PRIu64", %u", sb, sbb, sbb->offset, sbb->len);
        FREE(sb->cfg, sbb, sizeof(StreamingBufferBlock));
    }
}

/**
 * \internal
 * \brief move buffer forward by 'slide'
 */
static void AutoSlide(StreamingBuffer *sb)
{
    uint32_t size = sb->cfg->buf_slide;
    uint32_t slide = sb->buf_offset - size;
    SCLogDebug("sliding %u forward, size of original buffer left after slide %u", slide, size);
    memmove(sb->buf, sb->buf+slide, size);
    sb->stream_offset += slide;
    sb->buf_offset = size;
    SBBPrune(sb);
}

static int __attribute__((warn_unused_result))
GrowToSize(StreamingBuffer *sb, uint32_t size)
{
    /* try to grow in multiples of sb->cfg->buf_size */
    uint32_t x = sb->cfg->buf_size ? size % sb->cfg->buf_size : 0;
    uint32_t base = size - x;
    uint32_t grow = base + sb->cfg->buf_size;

    void *ptr = REALLOC(sb->cfg, sb->buf, sb->buf_size, grow);
    if (ptr == NULL)
        return -1;

    /* for safe printing and general caution, lets memset the
     * new data to 0 */
    size_t diff = grow - sb->buf_size;
    void *new_mem = ((char *)ptr) + sb->buf_size;
    memset(new_mem, 0, diff);

    sb->buf = ptr;
    sb->buf_size = grow;
    SCLogDebug("grown buffer to %u", grow);
#ifdef DEBUG
    if (sb->buf_size > sb->buf_size_max) {
        sb->buf_size_max = sb->buf_size;
    }
#endif
    return 0;
}

/** \internal
 *  \brief try to double the buffer size
 *  \retval 0 ok
 *  \retval -1 failed, buffer unchanged
 */
static int __attribute__((warn_unused_result)) Grow(StreamingBuffer *sb)
{
    uint32_t grow = sb->buf_size * 2;
    void *ptr = REALLOC(sb->cfg, sb->buf, sb->buf_size, grow);
    if (ptr == NULL)
        return -1;

    /* for safe printing and general caution, lets memset the
     * new data to 0 */
    size_t diff = grow - sb->buf_size;
    void *new_mem = ((char *)ptr) + sb->buf_size;
    memset(new_mem, 0, diff);

    sb->buf = ptr;
    sb->buf_size = grow;
    SCLogDebug("grown buffer to %u", grow);
#ifdef DEBUG
    if (sb->buf_size > sb->buf_size_max) {
        sb->buf_size_max = sb->buf_size;
    }
#endif
    return 0;
}

/**
 *  \brief slide to absolute offset
 *  \todo if sliding beyond window, we could perhaps reset?
 */
void StreamingBufferSlideToOffset(StreamingBuffer *sb, uint64_t offset)
{
    if (offset > sb->stream_offset &&
        offset <= sb->stream_offset + sb->buf_offset)
    {
        uint32_t slide = offset - sb->stream_offset;
        uint32_t size = sb->buf_offset - slide;
        SCLogDebug("sliding %u forward, size of original buffer left after slide %u", slide, size);
        memmove(sb->buf, sb->buf+slide, size);
        sb->stream_offset += slide;
        sb->buf_offset = size;
        SBBPrune(sb);
    }
}

void StreamingBufferSlide(StreamingBuffer *sb, uint32_t slide)
{
    uint32_t size = sb->buf_offset - slide;
    SCLogDebug("sliding %u forward, size of original buffer left after slide %u", slide, size);
    memmove(sb->buf, sb->buf+slide, size);
    sb->stream_offset += slide;
    sb->buf_offset = size;
    SBBPrune(sb);
}

#define DATA_FITS(sb, len) \
    ((sb)->buf_offset + (len) <= (sb)->buf_size)

StreamingBufferSegment *StreamingBufferAppendRaw(StreamingBuffer *sb, const uint8_t *data, uint32_t data_len)
{
    if (sb->buf == NULL) {
        if (InitBuffer(sb) == -1)
            return NULL;
    }

    if (!DATA_FITS(sb, data_len)) {
        if (sb->cfg->flags & STREAMING_BUFFER_AUTOSLIDE)
            AutoSlide(sb);
        if (sb->buf_size == 0) {
            if (GrowToSize(sb, data_len) != 0)
                return NULL;
        } else {
            while (!DATA_FITS(sb, data_len)) {
                if (Grow(sb) != 0) {
                    return NULL;
                }
            }
        }
    }
    if (!DATA_FITS(sb, data_len)) {
        return NULL;
    }

    StreamingBufferSegment *seg = CALLOC(sb->cfg, 1, sizeof(StreamingBufferSegment));
    if (seg != NULL) {
        memcpy(sb->buf + sb->buf_offset, data, data_len);
        seg->stream_offset = sb->stream_offset + sb->buf_offset;
        seg->segment_len = data_len;
        uint32_t rel_offset = sb->buf_offset;
        sb->buf_offset += data_len;

        if (!RB_EMPTY(&sb->sbb_tree)) {
            SBBUpdate(sb, rel_offset, data_len);
        }
        return seg;
    }
    return NULL;
}

int StreamingBufferAppend(StreamingBuffer *sb, StreamingBufferSegment *seg,
                          const uint8_t *data, uint32_t data_len)
{
    BUG_ON(seg == NULL);

    if (sb->buf == NULL) {
        if (InitBuffer(sb) == -1)
            return -1;
    }

    if (!DATA_FITS(sb, data_len)) {
        if (sb->cfg->flags & STREAMING_BUFFER_AUTOSLIDE)
            AutoSlide(sb);
        if (sb->buf_size == 0) {
            if (GrowToSize(sb, data_len) != 0)
                return -1;
        } else {
            while (!DATA_FITS(sb, data_len)) {
                if (Grow(sb) != 0) {
                    return -1;
                }
            }
        }
    }
    if (!DATA_FITS(sb, data_len)) {
        return -1;
    }

    memcpy(sb->buf + sb->buf_offset, data, data_len);
    seg->stream_offset = sb->stream_offset + sb->buf_offset;
    seg->segment_len = data_len;
    uint32_t rel_offset = sb->buf_offset;
    sb->buf_offset += data_len;

    if (!RB_EMPTY(&sb->sbb_tree)) {
        SBBUpdate(sb, rel_offset, data_len);
    }
    return 0;
}

/**
 *  \brief add data w/o tracking a segment
 */
int StreamingBufferAppendNoTrack(StreamingBuffer *sb,
                                 const uint8_t *data, uint32_t data_len)
{
    if (sb->buf == NULL) {
        if (InitBuffer(sb) == -1)
            return -1;
    }

    if (!DATA_FITS(sb, data_len)) {
        if (sb->cfg->flags & STREAMING_BUFFER_AUTOSLIDE)
            AutoSlide(sb);
        if (sb->buf_size == 0) {
            if (GrowToSize(sb, data_len) != 0)
                return -1;
        } else {
            while (!DATA_FITS(sb, data_len)) {
                if (Grow(sb) != 0) {
                    return -1;
                }
            }
        }
    }
    if (!DATA_FITS(sb, data_len)) {
        return -1;
    }

    memcpy(sb->buf + sb->buf_offset, data, data_len);
    uint32_t rel_offset = sb->buf_offset;
    sb->buf_offset += data_len;

    if (!RB_EMPTY(&sb->sbb_tree)) {
        SBBUpdate(sb, rel_offset, data_len);
    }
    return 0;
}

#define DATA_FITS_AT_OFFSET(sb, len, offset) \
    ((offset) + (len) <= (sb)->buf_size)

/**
 *  \param offset offset relative to StreamingBuffer::stream_offset
 */
int StreamingBufferInsertAt(StreamingBuffer *sb, StreamingBufferSegment *seg,
                            const uint8_t *data, uint32_t data_len,
                            uint64_t offset)
{
    BUG_ON(seg == NULL);

    if (offset < sb->stream_offset)
        return -1;

    if (sb->buf == NULL) {
        if (InitBuffer(sb) == -1)
            return -1;
    }

    uint32_t rel_offset = offset - sb->stream_offset;
    if (!DATA_FITS_AT_OFFSET(sb, data_len, rel_offset)) {
        if (sb->cfg->flags & STREAMING_BUFFER_AUTOSLIDE) {
            AutoSlide(sb);
            rel_offset = offset - sb->stream_offset;
        }
        if (!DATA_FITS_AT_OFFSET(sb, data_len, rel_offset)) {
            if (GrowToSize(sb, (rel_offset + data_len)) != 0)
                return -1;
        }
    }
    if (!DATA_FITS_AT_OFFSET(sb, data_len, rel_offset)) {
        return -1;
    }

    memcpy(sb->buf + rel_offset, data, data_len);
    seg->stream_offset = offset;
    seg->segment_len = data_len;

    SCLogDebug("rel_offset %u sb->stream_offset %"PRIu64", buf_offset %u",
            rel_offset, sb->stream_offset, sb->buf_offset);

    if (RB_EMPTY(&sb->sbb_tree)) {
        SCLogDebug("empty sbb list");

        if (sb->stream_offset == offset) {
            SCLogDebug("empty sbb list: block exactly what was expected, fall through");
            /* empty list, data is exactly what is expected (append),
             * so do nothing */
        } else if ((rel_offset + data_len) <= sb->buf_offset) {
            SCLogDebug("empty sbb list: block is within existing region");
        } else {
            if (sb->buf_offset && rel_offset == sb->buf_offset) {
                // nothing to do
            } else if (rel_offset < sb->buf_offset) {
                // nothing to do
            } else if (sb->buf_offset) {
                /* existing data, but there is a gap between us */
                SBBInit(sb, rel_offset, data_len);
            } else {
                /* gap before data in empty list */
                SCLogDebug("empty sbb list: invoking SBBInitLeadingGap");
                SBBInitLeadingGap(sb, offset, data_len);
            }
        }
    } else {
        /* already have blocks, so append new block based on new data */
        SBBUpdate(sb, rel_offset, data_len);
    }

    if (rel_offset + data_len > sb->buf_offset)
        sb->buf_offset = rel_offset + data_len;

    return 0;
}

int StreamingBufferSegmentIsBeforeWindow(const StreamingBuffer *sb,
                                         const StreamingBufferSegment *seg)
{
    if (seg->stream_offset < sb->stream_offset) {
        if (seg->stream_offset + seg->segment_len <= sb->stream_offset) {
            return 1;
        }
    }
    return 0;
}

/** \brief get the data for one SBB */
void StreamingBufferSBBGetData(const StreamingBuffer *sb,
                               const StreamingBufferBlock *sbb,
                               const uint8_t **data, uint32_t *data_len)
{
    if (sbb->offset >= sb->stream_offset) {
        uint64_t offset = sbb->offset - sb->stream_offset;
        *data = sb->buf + offset;
        if (offset + sbb->len > sb->buf_offset)
            *data_len = sb->buf_offset - offset;
        else
            *data_len = sbb->len;
        return;
    } else {
        uint64_t offset = sb->stream_offset - sbb->offset;
        if (offset < sbb->len) {
            *data = sb->buf;
            *data_len = sbb->len - offset;
            return;
        }
    }
    *data = NULL;
    *data_len = 0;
    return;
}

/** \brief get the data for one SBB */
void StreamingBufferSBBGetDataAtOffset(const StreamingBuffer *sb,
                                       const StreamingBufferBlock *sbb,
                                       const uint8_t **data, uint32_t *data_len,
                                       uint64_t offset)
{
    if (offset >= sbb->offset && offset < (sbb->offset + sbb->len)) {
        uint32_t sbblen = sbb->len - (offset - sbb->offset);

        if (offset >= sb->stream_offset) {
            uint64_t data_offset = offset - sb->stream_offset;
            *data = sb->buf + data_offset;
            if (data_offset + sbblen > sb->buf_size)
                *data_len = sb->buf_size - data_offset;
            else
                *data_len = sbblen;
            BUG_ON(*data_len > sbblen);
            return;
        } else {
            uint64_t data_offset = sb->stream_offset - sbb->offset;
            if (data_offset < sbblen) {
                *data = sb->buf;
                *data_len = sbblen - data_offset;
                BUG_ON(*data_len > sbblen);
                return;
            }
        }
    }

    *data = NULL;
    *data_len = 0;
    return;
}

void StreamingBufferSegmentGetData(const StreamingBuffer *sb,
                                   const StreamingBufferSegment *seg,
                                   const uint8_t **data, uint32_t *data_len)
{
    if (likely(sb->buf)) {
        if (seg->stream_offset >= sb->stream_offset) {
            uint64_t offset = seg->stream_offset - sb->stream_offset;
            *data = sb->buf + offset;
            if (offset + seg->segment_len > sb->buf_size)
                *data_len = sb->buf_size - offset;
            else
                *data_len = seg->segment_len;
            return;
        } else {
            uint64_t offset = sb->stream_offset - seg->stream_offset;
            if (offset < seg->segment_len) {
                *data = sb->buf;
                *data_len = seg->segment_len - offset;
                return;
            }
        }
    }
    *data = NULL;
    *data_len = 0;
    return;
}

/**
 *  \retval 1 data is the same
 *  \retval 0 data is different
 */
int StreamingBufferSegmentCompareRawData(const StreamingBuffer *sb,
                                         const StreamingBufferSegment *seg,
                                         const uint8_t *rawdata, uint32_t rawdata_len)
{
    const uint8_t *segdata = NULL;
    uint32_t segdata_len = 0;
    StreamingBufferSegmentGetData(sb, seg, &segdata, &segdata_len);
    if (segdata && segdata_len &&
        segdata_len == rawdata_len &&
        memcmp(segdata, rawdata, segdata_len) == 0)
    {
        return 1;
    }
    return 0;
}

int StreamingBufferGetData(const StreamingBuffer *sb,
        const uint8_t **data, uint32_t *data_len,
        uint64_t *stream_offset)
{
    if (sb != NULL && sb->buf != NULL) {
        *data = sb->buf;
        *data_len = sb->buf_offset;
        *stream_offset = sb->stream_offset;
        return 1;
    } else {
        *data = NULL;
        *data_len = 0;
        *stream_offset = 0;
        return 0;
    }
}

int StreamingBufferGetDataAtOffset (const StreamingBuffer *sb,
        const uint8_t **data, uint32_t *data_len,
        uint64_t offset)
{
    if (sb != NULL && sb->buf != NULL &&
            offset >= sb->stream_offset &&
            offset < (sb->stream_offset + sb->buf_offset))
    {
        uint32_t skip = offset - sb->stream_offset;
        *data = sb->buf + skip;
        *data_len = sb->buf_offset - skip;
        return 1;
    } else {
        *data = NULL;
        *data_len = 0;
        return 0;
    }
}

/**
 *  \retval 1 data is the same
 *  \retval 0 data is different
 */
int StreamingBufferCompareRawData(const StreamingBuffer *sb,
                                  const uint8_t *rawdata, uint32_t rawdata_len)
{
    const uint8_t *sbdata = NULL;
    uint32_t sbdata_len = 0;
    uint64_t offset = 0;
    StreamingBufferGetData(sb, &sbdata, &sbdata_len, &offset);
    if (offset == 0 &&
        sbdata && sbdata_len &&
        sbdata_len == rawdata_len &&
        memcmp(sbdata, rawdata, sbdata_len) == 0)
    {
        return 1;
    }
    SCLogDebug("sbdata_len %u, offset %"PRIu64, sbdata_len, offset);
    printf("got:\n");
    PrintRawDataFp(stdout, sbdata,sbdata_len);
    printf("wanted:\n");
    PrintRawDataFp(stdout, rawdata,rawdata_len);
    return 0;
}

#ifdef UNITTESTS
static void Dump(StreamingBuffer *sb)
{
    PrintRawDataFp(stdout, sb->buf, sb->buf_offset);
}

static void DumpSegment(StreamingBuffer *sb, StreamingBufferSegment *seg)
{
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    StreamingBufferSegmentGetData(sb, seg, &data, &data_len);
    if (data && data_len) {
        PrintRawDataFp(stdout, data, data_len);
    }
}

static int StreamingBufferTest01(void)
{
    StreamingBufferConfig cfg = { STREAMING_BUFFER_AUTOSLIDE, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment *seg1 = StreamingBufferAppendRaw(sb, (const uint8_t *)"ABCDEFGH", 8);
    StreamingBufferSegment *seg2 = StreamingBufferAppendRaw(sb, (const uint8_t *)"01234567", 8);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 16);
    FAIL_IF(seg1->stream_offset != 0);
    FAIL_IF(seg2->stream_offset != 8);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg2));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,seg1,(const uint8_t *)"ABCDEFGH", 8));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,seg2,(const uint8_t *)"01234567", 8));
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment *seg3 = StreamingBufferAppendRaw(sb, (const uint8_t *)"QWERTY", 6);
    FAIL_IF(sb->stream_offset != 8);
    FAIL_IF(sb->buf_offset != 14);
    FAIL_IF(seg3->stream_offset != 16);
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg3));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,seg3,(const uint8_t *)"QWERTY", 6));
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment *seg4 = StreamingBufferAppendRaw(sb, (const uint8_t *)"KLM", 3);
    FAIL_IF(sb->stream_offset != 14);
    FAIL_IF(sb->buf_offset != 11);
    FAIL_IF(seg4->stream_offset != 22);
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg3));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg4));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,seg4,(const uint8_t *)"KLM", 3));
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment *seg5 = StreamingBufferAppendRaw(sb, (const uint8_t *)"!@#$%^&*()_+<>?/,.;:'[]{}-=", 27);
    FAIL_IF(sb->stream_offset != 17);
    FAIL_IF(sb->buf_offset != 35);
    FAIL_IF(seg5->stream_offset != 25);
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,seg1));
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg3));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg4));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg5));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,seg5,(const uint8_t *)"!@#$%^&*()_+<>?/,.;:'[]{}-=", 27));
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment *seg6 = StreamingBufferAppendRaw(sb, (const uint8_t *)"UVWXYZ", 6);
    FAIL_IF(sb->stream_offset != 17);
    FAIL_IF(sb->buf_offset != 41);
    FAIL_IF(seg6->stream_offset != 52);
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,seg1));
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg3));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg4));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg5));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,seg6));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,seg6,(const uint8_t *)"UVWXYZ", 6));
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    SCFree(seg1);
    SCFree(seg2);
    SCFree(seg3);
    SCFree(seg4);
    SCFree(seg5);
    SCFree(seg6);
    StreamingBufferFree(sb);
    PASS;
}

static int StreamingBufferTest02(void)
{
    StreamingBufferConfig cfg = { 0, 8, 24, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferAppend(sb, &seg1, (const uint8_t *)"ABCDEFGH", 8) != 0);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferAppend(sb, &seg2, (const uint8_t *)"01234567", 8) != 0);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 16);
    FAIL_IF(seg1.stream_offset != 0);
    FAIL_IF(seg2.stream_offset != 8);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSlide(sb, 6);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferAppend(sb, &seg3, (const uint8_t *)"QWERTY", 6) != 0);
    FAIL_IF(sb->stream_offset != 6);
    FAIL_IF(sb->buf_offset != 16);
    FAIL_IF(seg3.stream_offset != 16);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg3));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    DumpSegment(sb, &seg3);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSlide(sb, 6);
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg3));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    DumpSegment(sb, &seg3);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferFree(sb);
    PASS;
}

static int StreamingBufferTest03(void)
{
    StreamingBufferConfig cfg = { 0, 8, 24, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferAppend(sb, &seg1, (const uint8_t *)"ABCDEFGH", 8) != 0);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"01234567", 8, 14) != 0);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 22);
    FAIL_IF(seg1.stream_offset != 0);
    FAIL_IF(seg2.stream_offset != 14);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"QWERTY", 6, 8) != 0);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 22);
    FAIL_IF(seg3.stream_offset != 8);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg3));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    DumpSegment(sb, &seg3);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSlide(sb, 10);
    FAIL_IF(!StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg3));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    DumpSegment(sb, &seg3);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferFree(sb);
    PASS;
}

static int StreamingBufferTest04(void)
{
    StreamingBufferConfig cfg = { 0, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferAppend(sb, &seg1, (const uint8_t *)"ABCDEFGH", 8) != 0);
    FAIL_IF(!RB_EMPTY(&sb->sbb_tree));
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"01234567", 8, 14) != 0);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 22);
    FAIL_IF(seg1.stream_offset != 0);
    FAIL_IF(seg2.stream_offset != 14);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(RB_EMPTY(&sb->sbb_tree));
    StreamingBufferBlock *sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF(sbb1 != sb->head);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 8);
    StreamingBufferBlock *sbb2 = SBB_RB_NEXT(sbb1);
    FAIL_IF_NULL(sbb2);
    FAIL_IF(sbb2 == sb->head);
    FAIL_IF(sbb2->offset != 14);
    FAIL_IF(sbb2->len != 8);
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"QWERTY", 6, 8) != 0);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 22);
    FAIL_IF(seg3.stream_offset != 8);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg3));
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1 != sb->head);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 22);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    DumpSegment(sb, &seg3);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    /* far ahead of curve: */
    StreamingBufferSegment seg4;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg4, (const uint8_t *)"XYZ", 3, 124) != 0);
    FAIL_IF(sb->stream_offset != 0);
    FAIL_IF(sb->buf_offset != 127);
    FAIL_IF(sb->buf_size != 128);
    FAIL_IF(seg4.stream_offset != 124);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg3));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(sb,&seg4));
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1 != sb->head);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 22);
    FAIL_IF(!SBB_RB_NEXT(sbb1));
    Dump(sb);
    DumpSegment(sb, &seg1);
    DumpSegment(sb, &seg2);
    DumpSegment(sb, &seg3);
    DumpSegment(sb, &seg4);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,&seg1,(const uint8_t *)"ABCDEFGH", 8));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,&seg2,(const uint8_t *)"01234567", 8));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,&seg3,(const uint8_t *)"QWERTY", 6));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(sb,&seg4,(const uint8_t *)"XYZ", 3));

    StreamingBufferFree(sb);
    PASS;
}

static int StreamingBufferTest05(void)
{
    StreamingBufferConfig cfg = { STREAMING_BUFFER_AUTOSLIDE, 8, 32, NULL, NULL, NULL, NULL };
    StreamingBuffer sb = STREAMING_BUFFER_INITIALIZER(&cfg);

    StreamingBufferSegment *seg1 = StreamingBufferAppendRaw(&sb, (const uint8_t *)"AAAAAAAA", 8);
    StreamingBufferSegment *seg2 = StreamingBufferAppendRaw(&sb, (const uint8_t *)"BBBBBBBB", 8);
    StreamingBufferSegment *seg3 = StreamingBufferAppendRaw(&sb, (const uint8_t *)"CCCCCCCC", 8);
    StreamingBufferSegment *seg4 = StreamingBufferAppendRaw(&sb, (const uint8_t *)"DDDDDDDD", 8);
    FAIL_IF(sb.stream_offset != 0);
    FAIL_IF(sb.buf_offset != 32);
    FAIL_IF(seg1->stream_offset != 0);
    FAIL_IF(seg2->stream_offset != 8);
    FAIL_IF(seg3->stream_offset != 16);
    FAIL_IF(seg4->stream_offset != 24);
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(&sb,seg1));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(&sb,seg2));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(&sb,seg3));
    FAIL_IF(StreamingBufferSegmentIsBeforeWindow(&sb,seg4));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(&sb,seg1,(const uint8_t *)"AAAAAAAA", 8));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(&sb,seg2,(const uint8_t *)"BBBBBBBB", 8));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(&sb,seg3,(const uint8_t *)"CCCCCCCC", 8));
    FAIL_IF(!StreamingBufferSegmentCompareRawData(&sb,seg4,(const uint8_t *)"DDDDDDDD", 8));
    Dump(&sb);
    FAIL_IF_NOT(sb.head == RB_MIN(SBB, &sb.sbb_tree));
    StreamingBufferSegment *seg5 = StreamingBufferAppendRaw(&sb, (const uint8_t *)"EEEEEEEE", 8);
    FAIL_IF(!StreamingBufferSegmentCompareRawData(&sb,seg5,(const uint8_t *)"EEEEEEEE", 8));
    Dump(&sb);
    FAIL_IF_NOT(sb.head == RB_MIN(SBB, &sb.sbb_tree));

    SCFree(seg1);
    SCFree(seg2);
    SCFree(seg3);
    SCFree(seg4);
    SCFree(seg5);
    StreamingBufferClear(&sb);
    PASS;
}

/** \test lots of gaps in block list */
static int StreamingBufferTest06(void)
{
    StreamingBufferConfig cfg = { 0, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferAppend(sb, &seg1, (const uint8_t *)"A", 1) != 0);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"C", 1, 2) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"F", 1, 5) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg4;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg4, (const uint8_t *)"H", 1, 7) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg5;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg5, (const uint8_t *)"ABCDEFGHIJ", 10, 0) != 0);
    Dump(sb);
    StreamingBufferBlock *sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg6;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg6, (const uint8_t *)"abcdefghij", 10, 0) != 0);
    Dump(sb);
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferFree(sb);
    PASS;
}

/** \test lots of gaps in block list */
static int StreamingBufferTest07(void)
{
    StreamingBufferConfig cfg = { 0, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg1, (const uint8_t *)"B", 1, 1) != 0);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"D", 1, 3) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"F", 1, 5) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg4;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg4, (const uint8_t *)"H", 1, 7) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg5;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg5, (const uint8_t *)"ABCDEFGHIJ", 10, 0) != 0);
    Dump(sb);
    StreamingBufferBlock *sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg6;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg6, (const uint8_t *)"abcdefghij", 10, 0) != 0);
    Dump(sb);
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferFree(sb);
    PASS;
}

/** \test lots of gaps in block list */
static int StreamingBufferTest08(void)
{
    StreamingBufferConfig cfg = { 0, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg1, (const uint8_t *)"B", 1, 1) != 0);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"D", 1, 3) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"F", 1, 5) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg4;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg4, (const uint8_t *)"H", 1, 7) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg5;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg5, (const uint8_t *)"ABCDEFGHIJ", 10, 0) != 0);
    Dump(sb);
    StreamingBufferBlock *sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg6;
    FAIL_IF(StreamingBufferAppend(sb, &seg6, (const uint8_t *)"abcdefghij", 10) != 0);
    Dump(sb);
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 20);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferFree(sb);
    PASS;
}

/** \test lots of gaps in block list */
static int StreamingBufferTest09(void)
{
    StreamingBufferConfig cfg = { 0, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg1, (const uint8_t *)"B", 1, 1) != 0);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"D", 1, 3) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"H", 1, 7) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg4;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg4, (const uint8_t *)"F", 1, 5) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg5;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg5, (const uint8_t *)"ABCDEFGHIJ", 10, 0) != 0);
    Dump(sb);
    StreamingBufferBlock *sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg6;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg6, (const uint8_t *)"abcdefghij", 10, 0) != 0);
    Dump(sb);
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferFree(sb);
    PASS;
}

/** \test lots of gaps in block list */
static int StreamingBufferTest10(void)
{
    StreamingBufferConfig cfg = { 0, 8, 16, NULL, NULL, NULL, NULL };
    StreamingBuffer *sb = StreamingBufferInit(&cfg);
    FAIL_IF(sb == NULL);

    StreamingBufferSegment seg1;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg1, (const uint8_t *)"A", 1, 0) != 0);
    Dump(sb);
    StreamingBufferSegment seg2;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg2, (const uint8_t *)"D", 1, 3) != 0);
    Dump(sb);
    StreamingBufferSegment seg3;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg3, (const uint8_t *)"H", 1, 7) != 0);
    Dump(sb);

    StreamingBufferSegment seg4;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg4, (const uint8_t *)"B", 1, 1) != 0);
    Dump(sb);
    StreamingBufferSegment seg5;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg5, (const uint8_t *)"C", 1, 2) != 0);
    Dump(sb);
    StreamingBufferSegment seg6;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg6, (const uint8_t *)"G", 1, 6) != 0);
    Dump(sb);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));

    StreamingBufferSegment seg7;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg7, (const uint8_t *)"ABCDEFGHIJ", 10, 0) != 0);
    Dump(sb);
    StreamingBufferBlock *sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));

    StreamingBufferSegment seg8;
    FAIL_IF(StreamingBufferInsertAt(sb, &seg8, (const uint8_t *)"abcdefghij", 10, 0) != 0);
    Dump(sb);
    sbb1 = RB_MIN(SBB, &sb->sbb_tree);
    FAIL_IF_NOT(sb->head == RB_MIN(SBB, &sb->sbb_tree));
    FAIL_IF_NULL(sbb1);
    FAIL_IF(sbb1->offset != 0);
    FAIL_IF(sbb1->len != 10);
    FAIL_IF(SBB_RB_NEXT(sbb1));

    StreamingBufferFree(sb);
    PASS;
}

#endif

void StreamingBufferRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("StreamingBufferTest01", StreamingBufferTest01);
    UtRegisterTest("StreamingBufferTest02", StreamingBufferTest02);
    UtRegisterTest("StreamingBufferTest03", StreamingBufferTest03);
    UtRegisterTest("StreamingBufferTest04", StreamingBufferTest04);
    UtRegisterTest("StreamingBufferTest05", StreamingBufferTest05);
    UtRegisterTest("StreamingBufferTest06", StreamingBufferTest06);
    UtRegisterTest("StreamingBufferTest07", StreamingBufferTest07);
    UtRegisterTest("StreamingBufferTest08", StreamingBufferTest08);
    UtRegisterTest("StreamingBufferTest09", StreamingBufferTest09);
    UtRegisterTest("StreamingBufferTest10", StreamingBufferTest10);
#endif
}
