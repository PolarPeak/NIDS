/* Copyright (C) 2015 Open Information Security Foundation
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

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * NFS application layer detector and parser
 */

#include "suricata-common.h"
#include "stream.h"
#include "conf.h"

#include "util-unittest.h"

#include "app-layer-detect-proto.h"
#include "app-layer-parser.h"

#include "app-layer-nfs-udp.h"

#include "rust.h"
#include "rust-nfs-nfs-gen.h"

/* The default port to probe for echo traffic if not provided in the
 * configuration file. */
#define NFS_DEFAULT_PORT "2049"

/* The minimum size for a RFC message. For some protocols this might
 * be the size of a header. TODO actual min size is likely larger */
#define NFS_MIN_FRAME_LEN 32

/* Enum of app-layer events for an echo protocol. Normally you might
 * have events for errors in parsing data, like unexpected data being
 * received. For echo we'll make something up, and log an app-layer
 * level alert if an empty message is received.
 *
 * Example rule:
 *
 * alert nfs any any -> any any (msg:"SURICATA NFS empty message"; \
 *    app-layer-event:nfs.empty_message; sid:X; rev:Y;)
 */
enum {
    NFS_DECODER_EVENT_EMPTY_MESSAGE,
};

SCEnumCharMap nfs_udp_decoder_event_table[] = {
    {"EMPTY_MESSAGE", NFS_DECODER_EVENT_EMPTY_MESSAGE},
    { NULL, 0 }
};

static void *NFSStateAlloc(void)
{
    return rs_nfs_state_new();
}

static void NFSStateFree(void *state)
{
    rs_nfs_state_free(state);
}

/**
 * \brief Callback from the application layer to have a transaction freed.
 *
 * \param state a void pointer to the NFSState object.
 * \param tx_id the transaction ID to free.
 */
static void NFSStateTxFree(void *state, uint64_t tx_id)
{
    rs_nfs_state_tx_free(state, tx_id);
}

static int NFSStateGetEventInfo(const char *event_name, int *event_id,
    AppLayerEventType *event_type)
{
    return rs_nfs_state_get_event_info(event_name, event_id, event_type);
}

static int NFSStateGetEventInfoById(int event_id, const char **event_name,
    AppLayerEventType *event_type)
{
    *event_name = "NFS UDP event name (generic)";
    *event_type = APP_LAYER_EVENT_TYPE_TRANSACTION;
    return 0;
}

static AppLayerDecoderEvents *NFSGetEvents(void *tx)
{
    return rs_nfs_state_get_events(tx);
}

/**
 * \brief Probe the input to see if it looks like echo.
 *
 * \retval ALPROTO_NFS if it looks like echo, otherwise
 *     ALPROTO_UNKNOWN.
 */
static AppProto NFSProbingParser(Flow *f, uint8_t direction,
        const uint8_t *input, uint32_t input_len, uint8_t *rdir)
{
    SCLogDebug("probing");
    if (input_len < NFS_MIN_FRAME_LEN) {
        SCLogDebug("unknown");
        return ALPROTO_UNKNOWN;
    }

    int8_t r = 0;
    if (direction & STREAM_TOSERVER)
        r = rs_nfs_probe_udp_ts(input, input_len);
    else
        r = rs_nfs_probe_udp_tc(input, input_len);

    if (r == 1) {
        SCLogDebug("nfs");
        return ALPROTO_NFS;
    } else if (r == -1) {
        SCLogDebug("failed");
        return ALPROTO_FAILED;
    }

    SCLogDebug("Protocol not detected as ALPROTO_NFS.");
    return ALPROTO_UNKNOWN;
}

static int NFSParseRequest(Flow *f, void *state,
    AppLayerParserState *pstate, const uint8_t *input, uint32_t input_len,
    void *local_data, const uint8_t flags)
{
    uint16_t file_flags = FileFlowToFlags(f, STREAM_TOSERVER);
    rs_nfs_setfileflags(0, state, file_flags);

    return rs_nfs_parse_request_udp(f, state, pstate, input, input_len, local_data);
}

static int NFSParseResponse(Flow *f, void *state, AppLayerParserState *pstate,
    const uint8_t *input, uint32_t input_len, void *local_data,
    const uint8_t flags)
{
    uint16_t file_flags = FileFlowToFlags(f, STREAM_TOCLIENT);
    rs_nfs_setfileflags(1, state, file_flags);

    return rs_nfs_parse_response_udp(f, state, pstate, input, input_len, local_data);
}

static uint64_t NFSGetTxCnt(void *state)
{
    return rs_nfs_state_get_tx_count(state);
}

static void *NFSGetTx(void *state, uint64_t tx_id)
{
    return rs_nfs_state_get_tx(state, tx_id);
}

static AppLayerGetTxIterTuple RustNFSGetTxIterator(
        const uint8_t ipproto, const AppProto alproto,
        void *alstate, uint64_t min_tx_id, uint64_t max_tx_id,
        AppLayerGetTxIterState *istate)
{
    return rs_nfs_state_get_tx_iterator(alstate, min_tx_id, (uint64_t *)istate);
}

static void NFSSetTxLogged(void *state, void *vtx, LoggerId logged)
{
    rs_nfs_tx_set_logged(state, vtx, logged);
}

static LoggerId NFSGetTxLogged(void *state, void *vtx)
{
    return rs_nfs_tx_get_logged(state, vtx);
}

/**
 * \brief Called by the application layer.
 *
 * In most cases 1 can be returned here.
 */
static int NFSGetAlstateProgressCompletionStatus(uint8_t direction) {
    return rs_nfs_state_progress_completion_status(direction);
}

/**
 * \brief Return the state of a transaction in a given direction.
 *
 * In the case of the echo protocol, the existence of a transaction
 * means that the request is done. However, some protocols that may
 * need multiple chunks of data to complete the request may need more
 * than just the existence of a transaction for the request to be
 * considered complete.
 *
 * For the response to be considered done, the response for a request
 * needs to be seen.  The response_done flag is set on response for
 * checking here.
 */
static int NFSGetStateProgress(void *tx, uint8_t direction)
{
    return rs_nfs_tx_get_alstate_progress(tx, direction);
}

/**
 * \brief get stored tx detect state
 */
static DetectEngineState *NFSGetTxDetectState(void *vtx)
{
    return rs_nfs_state_get_tx_detect_state(vtx);
}

/**
 * \brief set store tx detect state
 */
static int NFSSetTxDetectState(void *vtx, DetectEngineState *s)
{
    rs_nfs_state_set_tx_detect_state(vtx, s);
    return 0;
}

static FileContainer *NFSGetFiles(void *state, uint8_t direction)
{
    return rs_nfs_getfiles(direction, state);
}

static void NFSSetDetectFlags(void *tx, uint8_t dir, uint64_t flags)
{
    rs_nfs_tx_set_detect_flags(tx, dir, flags);
}

static uint64_t NFSGetDetectFlags(void *tx, uint8_t dir)
{
    return rs_nfs_tx_get_detect_flags(tx, dir);
}

static StreamingBufferConfig sbcfg = STREAMING_BUFFER_CONFIG_INITIALIZER;
static SuricataFileContext sfc = { &sbcfg };

void RegisterNFSUDPParsers(void)
{
    const char *proto_name = "nfs";

    /* Check if NFS TCP detection is enabled. If it does not exist in
     * the configuration file then it will be enabled by default. */
    if (AppLayerProtoDetectConfProtoDetectionEnabled("udp", proto_name)) {

        rs_nfs_init(&sfc);

        SCLogDebug("NFS UDP protocol detection enabled.");

        AppLayerProtoDetectRegisterProtocol(ALPROTO_NFS, proto_name);

        if (RunmodeIsUnittests()) {

            SCLogDebug("Unittest mode, registering default configuration.");
            AppLayerProtoDetectPPRegister(IPPROTO_UDP, NFS_DEFAULT_PORT,
                ALPROTO_NFS, 0, NFS_MIN_FRAME_LEN, STREAM_TOSERVER,
                NFSProbingParser, NFSProbingParser);

        }
        else {

            if (!AppLayerProtoDetectPPParseConfPorts("udp", IPPROTO_UDP,
                    proto_name, ALPROTO_NFS, 0, NFS_MIN_FRAME_LEN,
                    NFSProbingParser, NFSProbingParser)) {
                SCLogDebug("No NFS app-layer configuration, enabling NFS"
                    " detection TCP detection on port %s.",
                    NFS_DEFAULT_PORT);
                AppLayerProtoDetectPPRegister(IPPROTO_UDP,
                    NFS_DEFAULT_PORT, ALPROTO_NFS, 0,
                    NFS_MIN_FRAME_LEN, STREAM_TOSERVER,
                    NFSProbingParser, NFSProbingParser);
            }

        }

    }

    else {
        SCLogDebug("Protocol detecter and parser disabled for NFS.");
        return;
    }

    if (AppLayerParserConfParserEnabled("udp", proto_name))
    {
        SCLogDebug("Registering NFS protocol parser.");

        /* Register functions for state allocation and freeing. A
         * state is allocated for every new NFS flow. */
        AppLayerParserRegisterStateFuncs(IPPROTO_UDP, ALPROTO_NFS,
            NFSStateAlloc, NFSStateFree);

        /* Register request parser for parsing frame from server to client. */
        AppLayerParserRegisterParser(IPPROTO_UDP, ALPROTO_NFS,
            STREAM_TOSERVER, NFSParseRequest);

        /* Register response parser for parsing frames from server to client. */
        AppLayerParserRegisterParser(IPPROTO_UDP, ALPROTO_NFS,
            STREAM_TOCLIENT, NFSParseResponse);

        /* Register a function to be called by the application layer
         * when a transaction is to be freed. */
        AppLayerParserRegisterTxFreeFunc(IPPROTO_UDP, ALPROTO_NFS,
            NFSStateTxFree);

        AppLayerParserRegisterLoggerFuncs(IPPROTO_UDP, ALPROTO_NFS,
            NFSGetTxLogged, NFSSetTxLogged);

        /* Register a function to return the current transaction count. */
        AppLayerParserRegisterGetTxCnt(IPPROTO_UDP, ALPROTO_NFS,
            NFSGetTxCnt);

        /* Transaction handling. */
        AppLayerParserRegisterGetStateProgressCompletionStatus(ALPROTO_NFS,
            NFSGetAlstateProgressCompletionStatus);
        AppLayerParserRegisterGetStateProgressFunc(IPPROTO_UDP,
            ALPROTO_NFS, NFSGetStateProgress);
        AppLayerParserRegisterGetTx(IPPROTO_UDP, ALPROTO_NFS,
            NFSGetTx);
        AppLayerParserRegisterGetTxIterator(IPPROTO_UDP, ALPROTO_NFS,
                RustNFSGetTxIterator);

        AppLayerParserRegisterGetFilesFunc(IPPROTO_UDP, ALPROTO_NFS, NFSGetFiles);

        /* What is this being registered for? */
        AppLayerParserRegisterDetectStateFuncs(IPPROTO_UDP, ALPROTO_NFS,
            NFSGetTxDetectState, NFSSetTxDetectState);

        AppLayerParserRegisterGetEventInfo(IPPROTO_UDP, ALPROTO_NFS,
            NFSStateGetEventInfo);

        AppLayerParserRegisterGetEventInfoById(IPPROTO_UDP, ALPROTO_NFS,
            NFSStateGetEventInfoById);

        AppLayerParserRegisterGetEventsFunc(IPPROTO_UDP, ALPROTO_NFS,
            NFSGetEvents);

        AppLayerParserRegisterDetectFlagsFuncs(IPPROTO_UDP, ALPROTO_NFS,
                                               NFSGetDetectFlags, NFSSetDetectFlags);

    }
    else {
        SCLogNotice("NFS protocol parsing disabled.");
    }

#ifdef UNITTESTS
    AppLayerParserRegisterProtocolUnittests(IPPROTO_UDP, ALPROTO_NFS,
        NFSUDPParserRegisterTests);
#endif
}

#ifdef UNITTESTS
#endif

void NFSUDPParserRegisterTests(void)
{
#ifdef UNITTESTS
#endif
}
