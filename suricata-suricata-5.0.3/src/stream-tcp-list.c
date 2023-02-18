/* Copyright (C) 2007-2016 Open Information Security Foundation
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

/** \file
 *
 *  Segment list functions for insertions, overlap handling, removal and
 *  more.
 */

#include "suricata-common.h"
#include "stream-tcp-private.h"
#include "stream-tcp.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp-inline.h"
#include "stream-tcp-list.h"
#include "util-streaming-buffer.h"
#include "util-print.h"
#include "util-validate.h"

static void StreamTcpRemoveSegmentFromStream(TcpStream *stream, TcpSegment *seg);

static int check_overlap_different_data = 0;

void StreamTcpReassembleConfigEnableOverlapCheck(void)
{
    check_overlap_different_data = 1;
}

/*
 *  Inserts and overlap handling
 */

RB_GENERATE(TCPSEG, TcpSegment, rb, TcpSegmentCompare);

int TcpSegmentCompare(struct TcpSegment *a, struct TcpSegment *b)
{
    if (SEQ_GT(a->seq, b->seq))
        return 1;
    else if (SEQ_LT(a->seq, b->seq))
        return -1;
    else {
        if (a->payload_len == b->payload_len)
            return 0;
        else if (a->payload_len > b->payload_len)
            return 1;
        else
            return -1;
    }
}

/** \internal
 *  \brief insert segment data into the streaming buffer
 *  \param seg segment to store stream offset in
 *  \param data segment data after overlap handling (if any)
 *  \param data_len data length
 */
static inline int InsertSegmentDataCustom(TcpStream *stream, TcpSegment *seg, uint8_t *data, uint16_t data_len)
{
    uint64_t stream_offset;
    uint16_t data_offset;

    if (likely(SEQ_GEQ(seg->seq, stream->base_seq))) {
        stream_offset = STREAM_BASE_OFFSET(stream) + (seg->seq - stream->base_seq);
        data_offset = 0;
    } else {
        /* segment is partly before base_seq */
        data_offset = stream->base_seq - seg->seq;
        stream_offset = STREAM_BASE_OFFSET(stream);
    }

    SCLogDebug("stream %p buffer %p, stream_offset %"PRIu64", "
               "data_offset %"PRIu16", SEQ %u BASE %u, data_len %u",
               stream, &stream->sb, stream_offset,
               data_offset, seg->seq, stream->base_seq, data_len);
    BUG_ON(data_offset > data_len);
    if (data_len == data_offset) {
        SCReturnInt(0);
    }

    if (StreamingBufferInsertAt(&stream->sb, &seg->sbseg,
                data + data_offset,
                data_len - data_offset,
                stream_offset) != 0) {
        SCReturnInt(-1);
    }
#ifdef DEBUG
    {
        const uint8_t *mydata;
        uint32_t mydata_len;
        uint64_t mydata_offset;
        StreamingBufferGetData(&stream->sb, &mydata, &mydata_len, &mydata_offset);

        SCLogDebug("stream %p seg %p data in buffer %p of len %u and offset %"PRIu64,
                stream, seg, &stream->sb, mydata_len, mydata_offset);
        //PrintRawDataFp(stdout, mydata, mydata_len);
    }
#endif
    SCReturnInt(0);
}

/** \internal
 *  \brief check if this segments overlaps with an in-tree seg.
 *  \retval true
 *  \retval false
 */
static inline bool CheckOverlap(struct TCPSEG *tree, TcpSegment *seg)
{
    const uint32_t re = SEG_SEQ_RIGHT_EDGE(seg);
    SCLogDebug("start. SEQ %u payload_len %u. Right edge: %u. Seg %p",
            seg->seq, seg->payload_len, re, seg);

    /* check forward */
    TcpSegment *next = TCPSEG_RB_NEXT(seg);
    if (next) {
        // next has same seq, so data must overlap
        if (SEQ_EQ(next->seq, seg->seq))
            return true;
        // our right edge is beyond next seq, overlap
        if (SEQ_GT(re, next->seq))
            return true;
    }
    /* check backwards */
    TcpSegment *prev = TCPSEG_RB_PREV(seg);
    if (prev) {
        // prev has same seq, so data must overlap
        if (SEQ_EQ(prev->seq, seg->seq))
            return true;
        // prev's right edge is beyond our seq, overlap
        const uint32_t prev_re = SEG_SEQ_RIGHT_EDGE(prev);
        if (SEQ_GT(prev_re, seg->seq))
            return true;
    }

    SCLogDebug("no overlap");
    return false;
}

/** \internal
 *  \brief insert the segment into the proper place in the tree
 *         don't worry about the data or overlaps
 *
 *  \retval 2 not inserted, data overlap
 *  \retval 1 inserted with overlap detected
 *  \retval 0 inserted, no overlap
 *  \retval -1 error
 */
static int DoInsertSegment (TcpStream *stream, TcpSegment *seg, TcpSegment **dup_seg, Packet *p)
{
    /* before our base_seq we don't insert it in our list */
    if (SEQ_LEQ(SEG_SEQ_RIGHT_EDGE(seg), stream->base_seq))
    {
        SCLogDebug("not inserting: SEQ+payload %"PRIu32", last_ack %"PRIu32", "
                "base_seq %"PRIu32, (seg->seq + TCP_SEG_LEN(seg)),
                stream->last_ack, stream->base_seq);
        StreamTcpSetEvent(p, STREAM_REASSEMBLY_SEGMENT_BEFORE_BASE_SEQ);
        return -1;
    }

    /* fast track */
    if (RB_EMPTY(&stream->seg_tree)) {
        SCLogDebug("empty tree, inserting seg %p seq %" PRIu32 ", "
                   "len %" PRIu32 "", seg, seg->seq, TCP_SEG_LEN(seg));
        TCPSEG_RB_INSERT(&stream->seg_tree, seg);
        stream->segs_right_edge = SEG_SEQ_RIGHT_EDGE(seg);
        return 0;
    }

    /* insert and then check if there was any overlap with other segments */
    TcpSegment *res = TCPSEG_RB_INSERT(&stream->seg_tree, seg);
    if (res) {
        SCLogDebug("seg has a duplicate in the tree seq %u/%u",
                res->seq, res->payload_len);
        /* exact duplicate SEQ + payload_len */
        *dup_seg = res;
        return 2; // duplicate has overlap by definition.
    } else {
        if (SEQ_GT(SEG_SEQ_RIGHT_EDGE(seg), stream->segs_right_edge))
            stream->segs_right_edge = SEG_SEQ_RIGHT_EDGE(seg);

        /* insert succeeded, now check if we overlap with someone */
        if (CheckOverlap(&stream->seg_tree, seg) == true) {
            SCLogDebug("seg %u has overlap in the tree", seg->seq);
            return 1;
        }
    }
    SCLogDebug("seg %u: no overlap", seg->seq);
    return 0;
}

/** \internal
 *  \brief handle overlap per list segment
 *
 *  For a list segment handle the overlap according to the policy.
 *
 *  The 'buf' parameter points to the memory that will be inserted into
 *  the stream after the overlap checks are complete. As it will
 *  unconditionally overwrite whats in the stream now, the overlap
 *  policies are applied to this buffer. It starts with the 'new' data,
 *  so when the policy states 'old' data has to be used, 'buf' is
 *  updated to contain the 'old' data here.
 *
 *  \param buf stack allocated buffer sized p->payload_len that will be
 *             inserted into the stream buffer
 *
 *  \retval 1 if data was different
 *  \retval 0 data was the same or we didn't check for differences
 */
static int DoHandleDataOverlap(TcpStream *stream, const TcpSegment *list,
        const TcpSegment *seg, uint8_t *buf, Packet *p)
{
    SCLogDebug("handle overlap for segment %p seq %u len %u re %u, "
            "list segment %p seq %u len %u re %u", seg, seg->seq,
            p->payload_len, SEG_SEQ_RIGHT_EDGE(seg),
            list, list->seq, TCP_SEG_LEN(list), SEG_SEQ_RIGHT_EDGE(list));

    int data_is_different = 0;
    int use_new_data = 0;

    if (StreamTcpInlineMode()) {
        SCLogDebug("inline mode");
        if (StreamTcpInlineSegmentCompare(stream, p, list) != 0) {
            SCLogDebug("already accepted data not the same as packet data, rewrite packet");
            StreamTcpInlineSegmentReplacePacket(stream, p, list);
            data_is_different = 1;

            /* in inline mode we check for different data unconditionally,
             * but setting events still depends on config */
            if (check_overlap_different_data) {
                StreamTcpSetEvent(p, STREAM_REASSEMBLY_OVERLAP_DIFFERENT_DATA);
            }
        }

    /* IDS mode */
    } else {
        if (check_overlap_different_data) {
            if (StreamTcpInlineSegmentCompare(stream, p, list) != 0) {
                SCLogDebug("data is different from what is in the list");
                data_is_different = 1;
            }
        } else {
            /* if we're not checking, assume it's different */
            data_is_different = 1;
        }

        /* apply overlap policies */

        if (stream->os_policy == OS_POLICY_LAST) {
            /* buf will start with LAST data (from the segment),
             * so if policy is LAST we're now done here. */
            return (check_overlap_different_data && data_is_different);
        }

        /* start at the same seq */
        if (SEQ_EQ(seg->seq, list->seq)) {
            SCLogDebug("seg starts at list segment");

            if (SEQ_LT(SEG_SEQ_RIGHT_EDGE(seg), SEG_SEQ_RIGHT_EDGE(list))) {
                SCLogDebug("seg ends before list end, end overlapped by list");
            } else {
                if (SEQ_GT(SEG_SEQ_RIGHT_EDGE(seg), SEG_SEQ_RIGHT_EDGE(list))) {
                    SCLogDebug("seg ends beyond list end, list overlapped and more");
                    switch (stream->os_policy) {
                        case OS_POLICY_LINUX:
                            if (data_is_different) {
                                use_new_data = 1;
                            }
                            break;
                    }
                } else {
                    SCLogDebug("full overlap");
                }

                switch (stream->os_policy) {
                    case OS_POLICY_OLD_LINUX:
                    case OS_POLICY_SOLARIS:
                    case OS_POLICY_HPUX11:
                        if (data_is_different) {
                            use_new_data = 1;
                        }
                        break;
                }
            }

            /* new seg starts before list segment */
        } else if (SEQ_LT(seg->seq, list->seq)) {
            SCLogDebug("seg starts before list segment");

            if (SEQ_LT(SEG_SEQ_RIGHT_EDGE(seg), SEG_SEQ_RIGHT_EDGE(list))) {
                SCLogDebug("seg ends before list end, end overlapped by list");
            } else {
                if (SEQ_GT(SEG_SEQ_RIGHT_EDGE(seg), SEG_SEQ_RIGHT_EDGE(list))) {
                    SCLogDebug("seg starts before and fully overlaps list and beyond");
                } else {
                    SCLogDebug("seg starts before and fully overlaps list");
                }

                switch (stream->os_policy) {
                    case OS_POLICY_SOLARIS:
                    case OS_POLICY_HPUX11:
                        if (data_is_different) {
                            use_new_data = 1;
                        }
                        break;
                }
            }

            switch (stream->os_policy) {
                case OS_POLICY_BSD:
                case OS_POLICY_HPUX10:
                case OS_POLICY_IRIX:
                case OS_POLICY_WINDOWS:
                case OS_POLICY_WINDOWS2K3:
                case OS_POLICY_OLD_LINUX:
                case OS_POLICY_LINUX:
                case OS_POLICY_MACOS:
                    if (data_is_different) {
                        use_new_data = 1;
                    }
                    break;
            }

            /* new seg starts after list segment */
        } else { //if (SEQ_GT(seg->seq, list->seq)) {
            SCLogDebug("seg starts after list segment");

            if (SEQ_EQ(SEG_SEQ_RIGHT_EDGE(seg), SEG_SEQ_RIGHT_EDGE(list))) {
                SCLogDebug("seg after and is fully overlapped by list");
            } else if (SEQ_GT(SEG_SEQ_RIGHT_EDGE(seg), SEG_SEQ_RIGHT_EDGE(list))) {
                SCLogDebug("seg starts after list and ends after list");

                switch (stream->os_policy) {
                    case OS_POLICY_SOLARIS:
                    case OS_POLICY_HPUX11:
                        if (data_is_different) {
                            use_new_data = 1;
                        }
                        break;
                }
            } else {
                SCLogDebug("seg starts after list and ends before list end");

            }
        }
    }

    SCLogDebug("data_is_different %s, use_new_data %s",
        data_is_different ? "yes" : "no",
        use_new_data ? "yes" : "no");

    /* if the data is different and we don't want to use the new (seg)
     * data, we have to update buf with the list data */
    if (data_is_different && !use_new_data) {
        /* we need to copy list into seg */
        uint16_t list_offset = 0;
        uint16_t seg_offset = 0;
        uint32_t list_len;
        uint16_t seg_len = p->payload_len;
        uint32_t list_seq = list->seq;

        const uint8_t *list_data;
        StreamingBufferSegmentGetData(&stream->sb, &list->sbseg, &list_data, &list_len);
        if (list_data == NULL || list_len == 0)
            return 0;
        BUG_ON(list_len > USHRT_MAX);

        /* if list seg is partially before base_seq, list_len (from stream) and
         * TCP_SEG_LEN(list) will not be the same */
        if (SEQ_GEQ(list->seq, stream->base_seq)) {
            ;
        } else {
            list_seq = stream->base_seq;
            list_len = SEG_SEQ_RIGHT_EDGE(list) - stream->base_seq;
        }

        if (SEQ_LT(seg->seq, list_seq)) {
            seg_offset = list_seq - seg->seq;
            seg_len -= seg_offset;
        } else if (SEQ_GT(seg->seq, list_seq)) {
            list_offset = seg->seq - list_seq;
            list_len -= list_offset;
        }

        if (SEQ_LT(seg->seq + seg_offset + seg_len, list_seq + list_offset + list_len)) {
            list_len -= (list_seq + list_offset + list_len) - (seg->seq + seg_offset + seg_len);
        }
        SCLogDebug("here goes nothing: list %u %u, seg %u %u", list_offset, list_len, seg_offset, seg_len);

        //PrintRawDataFp(stdout, list_data + list_offset, list_len);
        //PrintRawDataFp(stdout, buf + seg_offset, seg_len);

        memcpy(buf + seg_offset, list_data + list_offset, list_len);
        //PrintRawDataFp(stdout, buf, p->payload_len);
    }
    return (check_overlap_different_data && data_is_different);
}

/** \internal
 *  \brief walk segment tree backwards to see if there are overlaps
 *
 *  Walk back from the current segment which is already in the tree.
 *  We walk until we can't possibly overlap anymore.
 */
static int DoHandleDataCheckBackwards(TcpStream *stream,
        TcpSegment *seg, uint8_t *buf, Packet *p)
{
    int retval = 0;

    SCLogDebug("check tree backwards: insert data for segment %p seq %u len %u re %u",
            seg, seg->seq, TCP_SEG_LEN(seg), SEG_SEQ_RIGHT_EDGE(seg));

    /* check backwards */
    TcpSegment *tree_seg = NULL, *s = seg;
    RB_FOREACH_REVERSE_FROM(tree_seg, TCPSEG, s) {
        if (tree_seg == seg)
            continue;

        int overlap = 0;
        if (SEQ_LEQ(SEG_SEQ_RIGHT_EDGE(tree_seg), stream->base_seq)) {
            // segment entirely before base_seq
            ;
        } else if (SEQ_LEQ(tree_seg->seq + tree_seg->payload_len, seg->seq)) {
            SCLogDebug("list segment too far to the left, no more overlap will be found");
            break;
        } else if (SEQ_GT(SEG_SEQ_RIGHT_EDGE(tree_seg), seg->seq)) {
            overlap = 1;
        }

        SCLogDebug("(back) tree seg %u len %u re %u overlap? %s",
                tree_seg->seq, TCP_SEG_LEN(tree_seg),
                SEG_SEQ_RIGHT_EDGE(tree_seg), overlap ? "yes" : "no");

        if (overlap) {
            retval |= DoHandleDataOverlap(stream, tree_seg, seg, buf, p);
        }
    }
    return retval;
}

/** \internal
 *  \brief walk segment tree in forward direction to see if there are overlaps
 *
 *  Walk forward from the current segment which is already in the tree.
 *  We walk until the next segs start with a SEQ beyond our right edge.
 */
static int DoHandleDataCheckForward(TcpStream *stream,
        TcpSegment *seg, uint8_t *buf, Packet *p)
{
    int retval = 0;

    uint32_t seg_re = SEG_SEQ_RIGHT_EDGE(seg);

    SCLogDebug("check list forward: insert data for segment %p seq %u len %u re %u",
            seg, seg->seq, TCP_SEG_LEN(seg), seg_re);

    TcpSegment *tree_seg = NULL, *s = seg;
    RB_FOREACH_FROM(tree_seg, TCPSEG, s) {
        if (tree_seg == seg)
            continue;

        int overlap = 0;
        if (SEQ_GT(seg_re, tree_seg->seq))
            overlap = 1;
        else if (SEQ_LEQ(seg_re, tree_seg->seq)) {
            SCLogDebug("tree segment %u too far ahead, "
                    "no more overlaps can happen", tree_seg->seq);
            break;
        }

        SCLogDebug("(fwd) in-tree seg %u len %u re %u overlap? %s",
                tree_seg->seq, TCP_SEG_LEN(tree_seg),
                SEG_SEQ_RIGHT_EDGE(tree_seg), overlap ? "yes" : "no");

        if (overlap) {
            retval |= DoHandleDataOverlap(stream, tree_seg, seg, buf, p);
        }
    }
    return retval;
}

/**
 *  \param dup_seg in-tree duplicate of `seg`
 */
static int DoHandleData(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
        TcpStream *stream, TcpSegment *seg, TcpSegment *tree_seg, Packet *p)
{
    int result = 0;
    TcpSegment *handle = seg;

    SCLogDebug("insert data for segment %p seq %u len %u re %u",
            seg, seg->seq, TCP_SEG_LEN(seg), SEG_SEQ_RIGHT_EDGE(seg));

    /* create temporary buffer to contain the data we will insert. Overlap
     * handling may update it. By using this we don't have to track whether
     * parts of the data are already inserted or not. */
    uint8_t buf[p->payload_len];
    memcpy(buf, p->payload, p->payload_len);

    /* if tree_seg is set, we have an exact duplicate that we need to check */
    if (tree_seg) {
        DoHandleDataOverlap(stream, tree_seg, seg, buf, p);
        handle = tree_seg;
    }

    const bool is_head = !(TCPSEG_RB_PREV(handle));
    const bool is_tail = !(TCPSEG_RB_NEXT(handle));

    /* new list head  */
    if (is_head && !is_tail) {
        result = DoHandleDataCheckForward(stream, handle, buf, p);

    /* new list tail */
    } else if (!is_head && is_tail) {
        result = DoHandleDataCheckBackwards(stream, handle, buf, p);

    /* middle of the list */
    } else if (!is_head && !is_tail) {
        result = DoHandleDataCheckBackwards(stream, handle, buf, p);
        result |= DoHandleDataCheckForward(stream, handle, buf, p);
    }

    /* we had an overlap with different data */
    if (result) {
        StreamTcpSetEvent(p, STREAM_REASSEMBLY_OVERLAP_DIFFERENT_DATA);
        StatsIncr(tv, ra_ctx->counter_tcp_reass_overlap_diff_data);
    }

    /* insert the temp buffer now that we've (possibly) updated
     * it to account for the overlap policies */
    if (InsertSegmentDataCustom(stream, handle, buf, p->payload_len) < 0) {
        return -1;
    }

    return 0;
}

/**
 *  \retval -1 segment not inserted
 *
 *  \param seg segment, this function takes total ownership
 *
 *  In case of error, this function returns the segment to the pool
 */
int StreamTcpReassembleInsertSegment(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
        TcpStream *stream, TcpSegment *seg, Packet *p,
        uint32_t pkt_seq, uint8_t *pkt_data, uint16_t pkt_datalen)
{
    SCEnter();

    TcpSegment *dup_seg = NULL;

    /* insert segment into list. Note: doesn't handle the data */
    int r = DoInsertSegment (stream, seg, &dup_seg, p);
    SCLogDebug("DoInsertSegment returned %d", r);
    if (r < 0) {
        StatsIncr(tv, ra_ctx->counter_tcp_reass_list_fail);
        StreamTcpSegmentReturntoPool(seg);
        SCReturnInt(-1);
    }

    if (likely(r == 0)) {
        /* no overlap, straight data insert */
        int res = InsertSegmentDataCustom(stream, seg, pkt_data, pkt_datalen);
        if (res < 0) {
            StatsIncr(tv, ra_ctx->counter_tcp_reass_data_normal_fail);
            StreamTcpRemoveSegmentFromStream(stream, seg);
            StreamTcpSegmentReturntoPool(seg);
            SCReturnInt(-1);
        }

    } else if (r == 1 || r == 2) {
        SCLogDebug("overlap (%s%s)", r == 1 ? "normal" : "", r == 2 ? "duplicate" : "");

        if (r == 2) {
            SCLogDebug("dup_seg %p", dup_seg);
        }

        /* XXX should we exclude 'retransmissions' here? */
        StatsIncr(tv, ra_ctx->counter_tcp_reass_overlap);

        /* now let's consider the data in the overlap case */
        int res = DoHandleData(tv, ra_ctx, stream, seg, dup_seg, p);
        if (res < 0) {
            StatsIncr(tv, ra_ctx->counter_tcp_reass_data_overlap_fail);

            if (r == 1) // r == 2 mean seg wasn't added to stream
                StreamTcpRemoveSegmentFromStream(stream, seg);

            StreamTcpSegmentReturntoPool(seg);
            SCReturnInt(-1);
        }
        if (r == 2) {
            SCLogDebug("duplicate segment %u/%u, discard it",
                    seg->seq, seg->payload_len);

            StreamTcpSegmentReturntoPool(seg);
#ifdef DEBUG
            if (SCLogDebugEnabled()) {
                TcpSegment *s = NULL, *safe = NULL;
                RB_FOREACH_SAFE(s, TCPSEG, &stream->seg_tree, safe)
                {
                    SCLogDebug("tree: seg %p, SEQ %"PRIu32", LEN %"PRIu16", SUM %"PRIu32"%s%s%s",
                            s, s->seq, TCP_SEG_LEN(s),
                            (uint32_t)(s->seq + TCP_SEG_LEN(s)),
                            s->seq == seg->seq ? " DUPLICATE" : "",
                            TCPSEG_RB_PREV(s) == NULL ? " HEAD" : "",
                            TCPSEG_RB_NEXT(s) == NULL ? " TAIL" : "");
                }
            }
#endif
        }
    }

    SCReturnInt(0);
}


/*
 * Pruning & removal
 */


static inline bool SegmentInUse(const TcpStream *stream, const TcpSegment *seg)
{
    /* if proto detect isn't done, we're not returning */
    if (!(stream->flags & (STREAMTCP_STREAM_FLAG_GAP|STREAMTCP_STREAM_FLAG_NOREASSEMBLY))) {
        if (!(StreamTcpIsSetStreamFlagAppProtoDetectionCompleted(stream))) {
            SCReturnInt(true);
        }
    }

    SCReturnInt(false);
}


/** \internal
 *  \brief check if we can remove a segment from our segment list
 *
 *  \retval true
 *  \retval false
 */
static inline bool StreamTcpReturnSegmentCheck(const TcpStream *stream, const TcpSegment *seg)
{
    if (SegmentInUse(stream, seg)) {
        SCReturnInt(false);
    }

    if (!(StreamingBufferSegmentIsBeforeWindow(&stream->sb, &seg->sbseg))) {
        SCReturnInt(false);
    }

    SCReturnInt(true);
}

static inline uint64_t GetLeftEdge(TcpSession *ssn, TcpStream *stream)
{
    bool use_app = true;
    bool use_raw = true;
    bool use_log = true;

    uint64_t left_edge = 0;
    if ((ssn->flags & STREAMTCP_FLAG_APP_LAYER_DISABLED) ||
          (stream->flags & STREAMTCP_STREAM_FLAG_GAP))
    {
        use_app = false; // app is dead
    }

    if (stream->flags & STREAMTCP_STREAM_FLAG_DISABLE_RAW) {
        use_raw = false; // raw is dead
    }
    if (!stream_config.streaming_log_api) {
        use_log = false;
    }

    if (use_raw) {
        uint64_t raw_progress = STREAM_RAW_PROGRESS(stream);

        if (StreamTcpInlineMode() == TRUE) {
            uint32_t chunk_size = (stream == &ssn->client) ?
                stream_config.reassembly_toserver_chunk_size :
                stream_config.reassembly_toclient_chunk_size;
            if (raw_progress < (uint64_t)chunk_size) {
                raw_progress = 0;
            } else {
                raw_progress -= (uint64_t)chunk_size;
            }
        }

        /* apply min inspect depth: if it is set we need to keep data
         * before the raw progress. */
        if (use_app && stream->min_inspect_depth) {
            if (raw_progress < stream->min_inspect_depth)
                raw_progress = 0;
            else
                raw_progress -= stream->min_inspect_depth;

            SCLogDebug("stream->min_inspect_depth %u, raw_progress %"PRIu64,
                    stream->min_inspect_depth, raw_progress);
        }

        if (use_app) {
            left_edge = MIN(STREAM_APP_PROGRESS(stream), raw_progress);
            SCLogDebug("left_edge %"PRIu64", using both app:%"PRIu64", raw:%"PRIu64,
                    left_edge, STREAM_APP_PROGRESS(stream), raw_progress);
        } else {
            left_edge = raw_progress;
            SCLogDebug("left_edge %"PRIu64", using only raw:%"PRIu64,
                    left_edge, raw_progress);
        }
    } else if (use_app) {
        left_edge = STREAM_APP_PROGRESS(stream);
        SCLogDebug("left_edge %"PRIu64", using only app:%"PRIu64,
                left_edge, STREAM_APP_PROGRESS(stream));
    } else {
        left_edge = STREAM_BASE_OFFSET(stream) + stream->sb.buf_offset;
        SCLogDebug("no app & raw: left_edge %"PRIu64" (full stream)", left_edge);
    }

    if (use_log) {
        if (use_app || use_raw) {
            left_edge = MIN(left_edge, STREAM_LOG_PROGRESS(stream));
        } else {
            left_edge = STREAM_LOG_PROGRESS(stream);
        }
    }

    /* in inline mode keep at least unack'd segments so we can check for overlaps */
    if (StreamTcpInlineMode() == TRUE) {
        uint64_t last_ack_abs = STREAM_BASE_OFFSET(stream);
        if (STREAM_LASTACK_GT_BASESEQ(stream)) {
            /* get window of data that is acked */
            const uint32_t delta = stream->last_ack - stream->base_seq;
            DEBUG_VALIDATE_BUG_ON(delta > 10000000ULL && delta > stream->window);
            /* get max absolute offset */
            last_ack_abs += delta;
        }
        left_edge = MIN(left_edge, last_ack_abs);

    /* if we're told to look for overlaps with different data we should
     * consider data that is ack'd as well. Injected packets may have
     * been ack'd or injected packet may be too late. */
    } else if (check_overlap_different_data) {
        const uint32_t window = stream->window ? stream->window : 4096;
        if (window < left_edge)
            left_edge -= window;
        else
            left_edge = 0;

        SCLogDebug("stream:%p left_edge %"PRIu64, stream, left_edge);
    }

    if (left_edge > 0) {
        /* we know left edge based on the progress values now,
         * lets adjust it to make sure in-use segments still have
         * data */
        TcpSegment *seg = NULL;
        RB_FOREACH(seg, TCPSEG, &stream->seg_tree) {
            if (TCP_SEG_OFFSET(seg) > left_edge) {
                SCLogDebug("seg beyond left_edge, we're done");
                break;
            }

            if (SegmentInUse(stream, seg)) {
                left_edge = TCP_SEG_OFFSET(seg);
                SCLogDebug("in-use seg before left_edge, adjust to %"PRIu64" and bail", left_edge);
                break;
            }
        }
    }

    return left_edge;
}

static void StreamTcpRemoveSegmentFromStream(TcpStream *stream, TcpSegment *seg)
{
    RB_REMOVE(TCPSEG, &stream->seg_tree, seg);
}

/** \brief Remove idle TcpSegments from TcpSession
 *
 *  Checks app progress and raw progress and progresses them
 *  if needed, slides the streaming buffer, then gets rid of
 *  excess segments.
 *
 *  \param f flow
 *  \param flags direction flags
 */
void StreamTcpPruneSession(Flow *f, uint8_t flags)
{
    SCEnter();

    if (f == NULL || f->protoctx == NULL) {
        SCReturn;
    }

    TcpSession *ssn = f->protoctx;
    TcpStream *stream = NULL;

    if (flags & STREAM_TOSERVER) {
        stream = &ssn->client;
    } else if (flags & STREAM_TOCLIENT) {
        stream = &ssn->server;
    } else {
        SCReturn;
    }

    if (stream->flags & STREAMTCP_STREAM_FLAG_NOREASSEMBLY) {
        return;
    }

    if (stream->flags & STREAMTCP_STREAM_FLAG_DEPTH_REACHED) {
        stream->flags |= STREAMTCP_STREAM_FLAG_NOREASSEMBLY;
        SCLogDebug("ssn %p / stream %p: reassembly depth reached, "
                 "STREAMTCP_STREAM_FLAG_NOREASSEMBLY set", ssn, stream);
        StreamTcpReturnStreamSegments(stream);
        StreamingBufferClear(&stream->sb);
        return;

    } else if (((ssn->flags & STREAMTCP_FLAG_APP_LAYER_DISABLED) ||
                (stream->flags & STREAMTCP_STREAM_FLAG_GAP))     &&
               (stream->flags & STREAMTCP_STREAM_FLAG_DISABLE_RAW))
    {
        SCLogDebug("ssn %p / stream %p: both app and raw are done, "
                 "STREAMTCP_STREAM_FLAG_NOREASSEMBLY set", ssn, stream);
        stream->flags |= STREAMTCP_STREAM_FLAG_NOREASSEMBLY;
        StreamTcpReturnStreamSegments(stream);
        StreamingBufferClear(&stream->sb);
        return;
    }

    const uint64_t left_edge = GetLeftEdge(ssn, stream);
    if (left_edge && left_edge > STREAM_BASE_OFFSET(stream)) {
        uint32_t slide = left_edge - STREAM_BASE_OFFSET(stream);
        SCLogDebug("buffer sliding %u to offset %"PRIu64, slide, left_edge);
        StreamingBufferSlideToOffset(&stream->sb, left_edge);
        stream->base_seq += slide;

        if (slide <= stream->app_progress_rel) {
            stream->app_progress_rel -= slide;
        } else {
            stream->app_progress_rel = 0;
        }
        if (slide <= stream->raw_progress_rel) {
            stream->raw_progress_rel -= slide;
        } else {
            stream->raw_progress_rel = 0;
        }
        if (slide <= stream->log_progress_rel) {
            stream->log_progress_rel -= slide;
        } else {
            stream->log_progress_rel = 0;
        }

        SCLogDebug("stream base_seq %u at stream offset %"PRIu64,
                stream->base_seq, STREAM_BASE_OFFSET(stream));
    }

    /* loop through the segments and remove all not in use */
    TcpSegment *seg = NULL, *safe = NULL;
    RB_FOREACH_SAFE(seg, TCPSEG, &stream->seg_tree, safe)
    {
        SCLogDebug("seg %p, SEQ %"PRIu32", LEN %"PRIu16", SUM %"PRIu32,
                seg, seg->seq, TCP_SEG_LEN(seg),
                (uint32_t)(seg->seq + TCP_SEG_LEN(seg)));

        if (StreamTcpReturnSegmentCheck(stream, seg) == 0) {
            SCLogDebug("not removing segment");
            break;
        }

        StreamTcpRemoveSegmentFromStream(stream, seg);
        StreamTcpSegmentReturntoPool(seg);
        SCLogDebug("removed segment");
        continue;
    }

    SCReturn;
}


/*
 *  unittests
 */

#ifdef UNITTESTS
#include "tests/stream-tcp-list.c"
#endif
