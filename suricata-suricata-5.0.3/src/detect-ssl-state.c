/* Copyright (C) 2007-2020 Open Information Security Foundation
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
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Implements support for ssl_state keyword.
 */

#include "suricata-common.h"
#include "threads.h"
#include "debug.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"

#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "app-layer.h"
#include "app-layer-parser.h"

#include "detect-ssl-state.h"

#include "stream-tcp.h"
#include "app-layer-ssl.h"

#define PARSE_REGEX1 "^(!?)([_a-zA-Z0-9]+)(.*)$"
static pcre *parse_regex1;
static pcre_extra *parse_regex1_study;

#define PARSE_REGEX2 "^(?:\\s*[|,]\\s*(!?)([_a-zA-Z0-9]+))(.*)$"
static pcre *parse_regex2;
static pcre_extra *parse_regex2_study;

static int DetectSslStateMatch(DetectEngineThreadCtx *,
        Flow *, uint8_t, void *, void *,
        const Signature *, const SigMatchCtx *);
static int DetectSslStateSetup(DetectEngineCtx *, Signature *, const char *);
#ifdef UNITTESTS
static void DetectSslStateRegisterTests(void);
#endif
static void DetectSslStateFree(void *);

static int InspectTlsGeneric(ThreadVars *tv,
        DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
        const Signature *s, const SigMatchData *smd,
        Flow *f, uint8_t flags, void *alstate,
        void *txv, uint64_t tx_id);

static int g_tls_generic_list_id = 0;

/**
 * \brief Registers the keyword handlers for the "ssl_state" keyword.
 */
void DetectSslStateRegister(void)
{
    sigmatch_table[DETECT_AL_SSL_STATE].name = "ssl_state";
    sigmatch_table[DETECT_AL_SSL_STATE].desc = "match the state of the SSL connection";
    sigmatch_table[DETECT_AL_SSL_STATE].url = "/rules/tls-keywords.html#ssl-state";
    sigmatch_table[DETECT_AL_SSL_STATE].AppLayerTxMatch = DetectSslStateMatch;
    sigmatch_table[DETECT_AL_SSL_STATE].Setup = DetectSslStateSetup;
    sigmatch_table[DETECT_AL_SSL_STATE].Free  = DetectSslStateFree;
#ifdef UNITTESTS
    sigmatch_table[DETECT_AL_SSL_STATE].RegisterTests = DetectSslStateRegisterTests;
#endif
    DetectSetupParseRegexes(PARSE_REGEX1, &parse_regex1, &parse_regex1_study);
    DetectSetupParseRegexes(PARSE_REGEX2, &parse_regex2, &parse_regex2_study);

    g_tls_generic_list_id = DetectBufferTypeRegister("tls_generic");

    DetectBufferTypeSetDescriptionByName("tls_generic",
            "generic ssl/tls inspection");

    DetectAppLayerInspectEngineRegister("tls_generic",
            ALPROTO_TLS, SIG_FLAG_TOSERVER, 0,
            InspectTlsGeneric);
    DetectAppLayerInspectEngineRegister("tls_generic",
            ALPROTO_TLS, SIG_FLAG_TOCLIENT, 0,
            InspectTlsGeneric);
}

static int InspectTlsGeneric(ThreadVars *tv,
        DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
        const Signature *s, const SigMatchData *smd,
        Flow *f, uint8_t flags, void *alstate,
        void *txv, uint64_t tx_id)
{
    return DetectEngineInspectGenericList(tv, de_ctx, det_ctx, s, smd,
                                          f, flags, alstate, txv, tx_id);
}

/**
 * \brief App layer match function ssl_state keyword.
 *
 * \param tv      Pointer to threadvars.
 * \param det_ctx Pointer to the thread's detection context.
 * \param f       Pointer to the flow.
 * \param flags   Flags.
 * \param state   App layer state.
 * \param s       Sig we are currently inspecting.
 * \param m       SigMatch we are currently inspecting.
 *
 * \retval 1 Match.
 * \retval 0 No match.
 */
static int DetectSslStateMatch(DetectEngineThreadCtx *det_ctx,
        Flow *f, uint8_t flags, void *alstate, void *txv,
        const Signature *s, const SigMatchCtx *m)
{
    const DetectSslStateData *ssd = (const DetectSslStateData *)m;
    SSLState *ssl_state = (SSLState *)alstate;
    if (ssl_state == NULL) {
        SCLogDebug("no app state, no match");
        return 0;
    }

    uint32_t ssl_flags = ssl_state->current_flags;

    if ((ssd->flags & ssl_flags) ^ ssd->mask) {
        return 1;
    }

    return 0;
}

/**
 * \brief Parse the arg supplied with ssl_state and return it in a
 *        DetectSslStateData instance.
 *
 * \param arg Pointer to the string to be parsed.
 *
 * \retval ssd  Pointer to DetectSslStateData on success.
 * \retval NULL On failure.
 */
static DetectSslStateData *DetectSslStateParse(const char *arg)
{
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov1[MAX_SUBSTRINGS];
    int ov2[MAX_SUBSTRINGS];
    char str1[64];
    char str2[64];
    int negate = 0;
    uint32_t flags = 0, mask = 0;
    DetectSslStateData *ssd = NULL;

    ret = pcre_exec(parse_regex1, parse_regex1_study, arg, strlen(arg), 0, 0,
                    ov1, MAX_SUBSTRINGS);
    if (ret < 1) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid arg \"%s\" supplied to "
                   "ssl_state keyword.", arg);
        goto error;
    }

    res = pcre_copy_substring((char *)arg, ov1, MAX_SUBSTRINGS, 1, str1, sizeof(str1));
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_COPY_SUBSTRING, "pcre_copy_substring failed");
        goto error;
    }
    negate = !strcmp("!", str1);

    res = pcre_copy_substring((char *)arg, ov1, MAX_SUBSTRINGS, 2, str1, sizeof(str1));
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_COPY_SUBSTRING, "pcre_copy_substring failed");
        goto error;
    }

    if (strcmp("client_hello", str1) == 0) {
        flags |= DETECT_SSL_STATE_CLIENT_HELLO;
        if (negate)
            mask |= DETECT_SSL_STATE_CLIENT_HELLO;
    } else if (strcmp("server_hello", str1) == 0) {
        flags |= DETECT_SSL_STATE_SERVER_HELLO;
        if (negate)
            mask |= DETECT_SSL_STATE_SERVER_HELLO;
    } else if (strcmp("client_keyx", str1) == 0) {
        flags |= DETECT_SSL_STATE_CLIENT_KEYX;
        if (negate)
            mask |= DETECT_SSL_STATE_CLIENT_KEYX;
    } else if (strcmp("server_keyx", str1) == 0) {
        flags |= DETECT_SSL_STATE_SERVER_KEYX;
        if (negate)
            mask |= DETECT_SSL_STATE_SERVER_KEYX;
    } else if (strcmp("unknown", str1) == 0) {
        flags |= DETECT_SSL_STATE_UNKNOWN;
        if (negate)
            mask |= DETECT_SSL_STATE_UNKNOWN;
    } else {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Found invalid option \"%s\" "
                   "in ssl_state keyword.", str1);
        goto error;
    }

    res = pcre_copy_substring((char *)arg, ov1, MAX_SUBSTRINGS, 3, str1, sizeof(str1));
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_COPY_SUBSTRING, "pcre_copy_substring failed");
        goto error;
    }
    while (res > 0) {
        ret = pcre_exec(parse_regex2, parse_regex2_study, str1, strlen(str1), 0, 0,
                        ov2, MAX_SUBSTRINGS);
        if (ret < 1) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid arg \"%s\" supplied to "
                       "ssl_state keyword.", arg);
            goto error;
        }

        res = pcre_copy_substring((char *)str1, ov2, MAX_SUBSTRINGS, 1, str2, sizeof(str2));
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_COPY_SUBSTRING, "pcre_copy_substring failed");
            goto error;
        }
        negate = !strcmp("!", str2);

        res = pcre_copy_substring((char *)str1, ov2, MAX_SUBSTRINGS, 2, str2, sizeof(str2));
        if (res <= 0) {
            SCLogError(SC_ERR_PCRE_COPY_SUBSTRING, "pcre_copy_substring failed");
            goto error;
        }
        if (strcmp("client_hello", str2) == 0) {
            flags |= DETECT_SSL_STATE_CLIENT_HELLO;
            if (negate)
                mask |= DETECT_SSL_STATE_CLIENT_HELLO;
        } else if (strcmp("server_hello", str2) == 0) {
            flags |= DETECT_SSL_STATE_SERVER_HELLO;
            if (negate)
                mask |= DETECT_SSL_STATE_SERVER_HELLO;
        } else if (strcmp("client_keyx", str2) == 0) {
            flags |= DETECT_SSL_STATE_CLIENT_KEYX;
            if (negate)
                mask |= DETECT_SSL_STATE_CLIENT_KEYX;
        } else if (strcmp("server_keyx", str2) == 0) {
            flags |= DETECT_SSL_STATE_SERVER_KEYX;
            if (negate)
                mask |= DETECT_SSL_STATE_SERVER_KEYX;
        } else if (strcmp("unknown", str2) == 0) {
            flags |= DETECT_SSL_STATE_UNKNOWN;
            if (negate)
                mask |= DETECT_SSL_STATE_UNKNOWN;
        } else {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Found invalid option \"%s\" "
                       "in ssl_state keyword.", str2);
            goto error;
        }

        res = pcre_copy_substring((char *)str1, ov2, MAX_SUBSTRINGS, 3, str2, sizeof(str2));
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_COPY_SUBSTRING, "pcre_copy_substring failed");
            goto error;
        }

        memcpy(str1, str2, sizeof(str1));
    }

    if ( (ssd = SCMalloc(sizeof(DetectSslStateData))) == NULL) {
        goto error;
    }
    ssd->flags = flags;
    ssd->mask = mask;

    return ssd;

error:
    return NULL;
}

 /**
 * \internal
 * \brief Setup function for ssl_state keyword.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 * \param s      Pointer to the Current Signature
 * \param arg    String holding the arg.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
static int DetectSslStateSetup(DetectEngineCtx *de_ctx, Signature *s, const char *arg)
{
    DetectSslStateData *ssd = NULL;
    SigMatch *sm = NULL;

    if (DetectSignatureSetAppProto(s, ALPROTO_TLS) != 0)
        return -1;

    ssd = DetectSslStateParse(arg);
    if (ssd == NULL)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_AL_SSL_STATE;
    sm->ctx = (SigMatchCtx*)ssd;

    SigMatchAppendSMToList(s, sm, g_tls_generic_list_id);
    return 0;

error:
    if (ssd != NULL)
        DetectSslStateFree(ssd);
    if (sm != NULL)
        SCFree(sm);
    return -1;
}

/**
 * \brief Free memory associated with DetectSslStateData.
 *
 * \param ptr pointer to the data to be freed.
 */
static void DetectSslStateFree(void *ptr)
{
    if (ptr != NULL)
        SCFree(ptr);

    return;
}

#ifdef UNITTESTS
#include "tests/detect-ssl-state.c"
#endif
