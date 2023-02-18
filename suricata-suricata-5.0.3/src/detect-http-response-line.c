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

/**
 * \ingroup httplayer
 *
 * @{
 */


/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Implements support for the http_response_line keyword.
 */

#include "suricata-common.h"
#include "threads.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"
#include "detect-engine-prefilter.h"
#include "detect-engine-content-inspection.h"
#include "detect-content.h"
#include "detect-pcre.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-spm.h"

#include "app-layer.h"
#include "app-layer-parser.h"

#include "app-layer-htp.h"
#include "stream-tcp.h"
#include "detect-http-response-line.h"

static int DetectHttpResponseLineSetup(DetectEngineCtx *, Signature *, const char *);
static void DetectHttpResponseLineRegisterTests(void);
static InspectionBuffer *GetData(DetectEngineThreadCtx *det_ctx,
        const DetectEngineTransforms *transforms,
        Flow *_f, const uint8_t _flow_flags,
        void *txv, const int list_id);
static int g_http_response_line_id = 0;

/**
 * \brief Registers the keyword handlers for the "http_response_line" keyword.
 */
void DetectHttpResponseLineRegister(void)
{
    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].name = "http.response_line";
    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].alias = "http_response_line";
    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].desc = "content modifier to match only on the HTTP response line";
    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].url = "/rules/http-keywords.html#http-response-line";
    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].Setup = DetectHttpResponseLineSetup;
    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].RegisterTests = DetectHttpResponseLineRegisterTests;

    sigmatch_table[DETECT_AL_HTTP_RESPONSE_LINE].flags |= SIGMATCH_NOOPT|SIGMATCH_INFO_STICKY_BUFFER;

    DetectAppLayerInspectEngineRegister2("http_response_line",
            ALPROTO_HTTP, SIG_FLAG_TOCLIENT, HTP_RESPONSE_LINE,
            DetectEngineInspectBufferGeneric,
            GetData);

    DetectAppLayerMpmRegister2("http_response_line", SIG_FLAG_TOCLIENT, 2,
            PrefilterGenericMpmRegister, GetData,
            ALPROTO_HTTP, HTP_RESPONSE_LINE);

    DetectBufferTypeSetDescriptionByName("http_response_line",
            "http response line");

    g_http_response_line_id = DetectBufferTypeGetByName("http_response_line");
}

/**
 * \brief The setup function for the http_response_line keyword for a signature.
 *
 * \param de_ctx Pointer to the detection engine context.
 * \param s      Pointer to the signature for the current Signature being
 *               parsed from the rules.
 * \param m      Pointer to the head of the SigMatch for the current rule
 *               being parsed.
 * \param arg    Pointer to the string holding the keyword value.
 *
 * \retval  0 On success
 * \retval -1 On failure
 */
static int DetectHttpResponseLineSetup(DetectEngineCtx *de_ctx, Signature *s, const char *arg)
{
    if (DetectBufferSetActiveList(s, g_http_response_line_id) < 0)
        return -1;

    if (DetectSignatureSetAppProto(s, ALPROTO_HTTP) < 0)
        return -1;

    return 0;
}

static InspectionBuffer *GetData(DetectEngineThreadCtx *det_ctx,
        const DetectEngineTransforms *transforms,
        Flow *_f, const uint8_t _flow_flags,
        void *txv, const int list_id)
{
    InspectionBuffer *buffer = InspectionBufferGet(det_ctx, list_id);
    if (buffer->inspect == NULL) {
        htp_tx_t *tx = (htp_tx_t *)txv;
        if (unlikely(tx->response_line == NULL)) {
            return NULL;
        }
        const uint32_t data_len = bstr_len(tx->response_line);
        const uint8_t *data = bstr_ptr(tx->response_line);

        InspectionBufferSetup(buffer, data, data_len);
        InspectionBufferApplyTransforms(buffer, transforms);
    }
    return buffer;
}

/************************************Unittests*********************************/

#ifdef UNITTESTS

#include "stream-tcp-reassemble.h"

/**
 * \test Test that a signature containting a http_response_line is correctly parsed
 *       and the keyword is registered.
 */
static int DetectHttpResponseLineTest01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(http_response_line; content:\"200 OK\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    DetectEngineCtxFree(de_ctx);
    PASS;
}


/**
 *\test Test that the http_response_line content matches against a http request
 *      which holds the content.
 */
static int DetectHttpResponseLineTest02(void)
{
    TcpSession ssn;
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineCtx *de_ctx = NULL;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    Flow f;
    uint8_t http_buf[] =
        "GET /index.html HTTP/1.0\r\n"
        "Host: www.openinfosecfoundation.org\r\n"
        "User-Agent: This is dummy message body\r\n"
        "Content-Type: text/html\r\n"
        "\r\n";
    uint32_t http_len = sizeof(http_buf) - 1;
    uint8_t http_buf2[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "message";
    uint32_t http_len2 = sizeof(http_buf2) - 1;

    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();
    FAIL_IF_NULL(alp_tctx);

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    FAIL_IF_NULL(p);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= (FLOW_PKT_TOSERVER|FLOW_PKT_ESTABLISHED);
    p->flags |= PKT_HAS_FLOW | PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any "
                               "(http_response_line; content:\"HTTP/1.0 200 OK\"; "
                               "sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParserParse(&th_v, alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, http_buf, http_len);
    FAIL_IF(r != 0);

    http_state = f.alstate;
    FAIL_IF_NULL(http_state);

    r = AppLayerParserParse(&th_v, alp_tctx, &f, ALPROTO_HTTP, STREAM_TOCLIENT, http_buf2, http_len2);
    FAIL_IF(r != 0);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));

    p->flowflags = (FLOW_PKT_TOCLIENT|FLOW_PKT_ESTABLISHED);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(!(PacketAlertCheck(p, 1)));

    AppLayerParserThreadCtxFree(alp_tctx);
    DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    PASS;
}

#endif /* UNITTESTS */

void DetectHttpResponseLineRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectHttpResponseLineTest01", DetectHttpResponseLineTest01);
    UtRegisterTest("DetectHttpResponseLineTest02", DetectHttpResponseLineTest02);
#endif /* UNITTESTS */

    return;
}
/**
 * @}
 */
