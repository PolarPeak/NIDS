/* Copyright (C) 2019 Open Information Security Foundation
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
 * \author Mats Klepsland <mats.klepsland@gmail.com>
 *
 * Implements support for tls.certs keyword.
 */

#include "suricata-common.h"
#include "threads.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-prefilter.h"
#include "detect-engine-content-inspection.h"
#include "detect-content.h"
#include "detect-pcre.h"
#include "detect-tls-certs.h"

#include "flow.h"
#include "flow-util.h"
#include "flow-var.h"

#include "util-debug.h"
#include "util-unittest.h"
#include "util-spm.h"
#include "util-print.h"

#include "stream-tcp.h"

#include "app-layer.h"
#include "app-layer-ssl.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

static int DetectTlsCertsSetup(DetectEngineCtx *, Signature *, const char *);
#ifdef UNITTESTS
static void DetectTlsCertsRegisterTests(void);
#endif
static int DetectEngineInspectTlsCerts(DetectEngineCtx *de_ctx,
	DetectEngineThreadCtx *det_ctx,
	const DetectEngineAppInspectionEngine *engine,
        const Signature *s, Flow *f, uint8_t flags, void *alstate, void *txv,
	uint64_t tx_id);
static int PrefilterMpmTlsCertsRegister(DetectEngineCtx *de_ctx,
        SigGroupHead *sgh, MpmCtx *mpm_ctx,
        const DetectBufferMpmRegistery *mpm_reg, int list_id);

static int g_tls_certs_buffer_id = 0;

struct TlsCertsGetDataArgs {
    int local_id;  /**< used as index into thread inspect array */
    SSLCertsChain *cert;
};

typedef struct PrefilterMpmTlsCerts {
    int list_id;
    const MpmCtx *mpm_ctx;
    const DetectEngineTransforms *transforms;
} PrefilterMpmTlsCerts;

/**
 * \brief Registration function for keyword: tls.certs
 */
void DetectTlsCertsRegister(void)
{
    sigmatch_table[DETECT_AL_TLS_CERTS].name = "tls.certs";
    sigmatch_table[DETECT_AL_TLS_CERTS].desc = "content modifier to match the TLS certificate sticky buffer";
    sigmatch_table[DETECT_AL_TLS_CERTS].url = "/rules/tls-keywords.html#tls-certs";
    sigmatch_table[DETECT_AL_TLS_CERTS].Setup = DetectTlsCertsSetup;
#ifdef UNITTESTS
    sigmatch_table[DETECT_AL_TLS_CERTS].RegisterTests = DetectTlsCertsRegisterTests;
#endif
    sigmatch_table[DETECT_AL_TLS_CERTS].flags |= SIGMATCH_NOOPT;
    sigmatch_table[DETECT_AL_TLS_CERTS].flags |= SIGMATCH_INFO_STICKY_BUFFER;

    DetectAppLayerInspectEngineRegister2("tls.certs", ALPROTO_TLS,
            SIG_FLAG_TOCLIENT, TLS_STATE_CERT_READY,
            DetectEngineInspectTlsCerts, NULL);

    DetectAppLayerMpmRegister2("tls.certs", SIG_FLAG_TOCLIENT, 2,
            PrefilterMpmTlsCertsRegister, NULL, ALPROTO_TLS,
            TLS_STATE_CERT_READY);

    DetectBufferTypeSetDescriptionByName("tls.certs", "TLS certificate");

    g_tls_certs_buffer_id = DetectBufferTypeGetByName("tls.certs");
}

/**
 * \brief This function setup the tls.certs modifier keyword
 *
 * \param de_ctx Pointer to the Detect Engine Context
 * \param s      Pointer to the Signature to which the keyword belongs
 * \param str    Should hold an empty string always
 *
 * \retval  0 On success
 * \retval -1 On failure
 */
static int DetectTlsCertsSetup(DetectEngineCtx *de_ctx, Signature *s,
                               const char *str)
{
    if (DetectBufferSetActiveList(s, g_tls_certs_buffer_id) < 0)
        return -1;

    if (DetectSignatureSetAppProto(s, ALPROTO_TLS) < 0)
        return -1;

    return 0;
}

static InspectionBuffer *TlsCertsGetData(DetectEngineThreadCtx *det_ctx,
        const DetectEngineTransforms *transforms, Flow *f,
	struct TlsCertsGetDataArgs *cbdata, int list_id)
{
    SCEnter();

    InspectionBufferMultipleForList *fb = InspectionBufferGetMulti(det_ctx, list_id);
    InspectionBuffer *buffer = InspectionBufferMultipleForListGet(fb, cbdata->local_id);
    if (buffer == NULL)
        return NULL;

    const SSLState *ssl_state = (SSLState *)f->alstate;

    if (TAILQ_EMPTY(&ssl_state->server_connp.certs)) {
        return NULL;
    }

    if (cbdata->cert == NULL) {
        cbdata->cert = TAILQ_FIRST(&ssl_state->server_connp.certs);
    } else {
	cbdata->cert = TAILQ_NEXT(cbdata->cert, next);
    }

    if (cbdata->cert == NULL) {
        return NULL;
    }

    InspectionBufferSetup(buffer, cbdata->cert->cert_data,
		          cbdata->cert->cert_len);
    InspectionBufferApplyTransforms(buffer, transforms);

    SCReturnPtr(buffer, "InspectionBuffer");
}

static int DetectEngineInspectTlsCerts(DetectEngineCtx *de_ctx,
	DetectEngineThreadCtx *det_ctx,
	const DetectEngineAppInspectionEngine *engine,
        const Signature *s, Flow *f, uint8_t flags, void *alstate, void *txv,
	uint64_t tx_id)
{
    const DetectEngineTransforms *transforms = NULL;
    if (!engine->mpm) {
        transforms = engine->v2.transforms;
    }

    struct TlsCertsGetDataArgs cbdata = { 0, NULL };

    while (1)
    {
        InspectionBuffer *buffer = TlsCertsGetData(det_ctx, transforms, f,
			                           &cbdata, engine->sm_list);
        if (buffer == NULL || buffer->inspect == NULL)
            break;

        det_ctx->buffer_offset = 0;
        det_ctx->discontinue_matching = 0;
        det_ctx->inspection_recursion_counter = 0;

        const int match = DetectEngineContentInspection(de_ctx, det_ctx, s, engine->smd,
                                              NULL, f, (uint8_t *)buffer->inspect,
                                              buffer->inspect_len,
                                              buffer->inspect_offset, DETECT_CI_FLAGS_SINGLE,
                                              DETECT_ENGINE_CONTENT_INSPECTION_MODE_STATE);
        if (match == 1) {
            return DETECT_ENGINE_INSPECT_SIG_MATCH;
        }

	cbdata.local_id++;
    }

    return DETECT_ENGINE_INSPECT_SIG_NO_MATCH;
}

static void PrefilterTxTlsCerts(DetectEngineThreadCtx *det_ctx,
        const void *pectx, Packet *p, Flow *f, void *txv,
        const uint64_t idx, const uint8_t flags)
{
    SCEnter();

    const PrefilterMpmTlsCerts *ctx = (const PrefilterMpmTlsCerts *)pectx;
    const MpmCtx *mpm_ctx = ctx->mpm_ctx;
    const int list_id = ctx->list_id;

    struct TlsCertsGetDataArgs cbdata = { 0, NULL };

    while (1)
    {
        InspectionBuffer *buffer = TlsCertsGetData(det_ctx, ctx->transforms,
                                                   f, &cbdata, list_id);
        if (buffer == NULL)
            break;

        if (buffer->inspect_len >= mpm_ctx->minlen) {
            (void)mpm_table[mpm_ctx->mpm_type].Search(mpm_ctx,
                    &det_ctx->mtcu, &det_ctx->pmq,
                    buffer->inspect, buffer->inspect_len);
        }

        cbdata.local_id++;
    }
}

static void PrefilterMpmTlsCertsFree(void *ptr)
{
    SCFree(ptr);
}

static int PrefilterMpmTlsCertsRegister(DetectEngineCtx *de_ctx,
        SigGroupHead *sgh, MpmCtx *mpm_ctx,
        const DetectBufferMpmRegistery *mpm_reg, int list_id)
{
    PrefilterMpmTlsCerts *pectx = SCCalloc(1, sizeof(*pectx));
    if (pectx == NULL)
        return -1;

    pectx->list_id = list_id;
    pectx->mpm_ctx = mpm_ctx;
    pectx->transforms = &mpm_reg->transforms;

    return PrefilterAppendTxEngine(de_ctx, sgh, PrefilterTxTlsCerts,
            mpm_reg->app_v2.alproto, mpm_reg->app_v2.tx_min_progress,
            pectx, PrefilterMpmTlsCertsFree, mpm_reg->name);
}

#ifdef UNITTESTS
#include "tests/detect-tls-certs.c"
#endif
