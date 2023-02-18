/* Copyright (C) 2018 Open Information Security Foundation
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
 * \author Pierre Chifflier <chifflier@wzdftpd.net>
 */

#include "suricata-common.h"
#include "util-unittest.h"

#include "detect-parse.h"
#include "detect-engine.h"

#include "detect-krb5-errcode.h"

#include "app-layer-krb5.h"
#include "rust-krb-detect-gen.h"

/**
 * \brief Regex for parsing our keyword options
 */
#define PARSE_REGEX  "^\\s*([A-z0-9\\.]+|\"[A-z0-9_\\.]+\")\\s*$"
static pcre *parse_regex;
static pcre_extra *parse_regex_study;

/* Prototypes of functions registered in DetectKrb5ErrCodeRegister below */
static int DetectKrb5ErrCodeMatch (DetectEngineThreadCtx *, Flow *,
                                   uint8_t, void *, void *, const Signature *,
                                   const SigMatchCtx *);
static int DetectKrb5ErrCodeSetup (DetectEngineCtx *, Signature *, const char *);
static void DetectKrb5ErrCodeFree (void *);
static void DetectKrb5ErrCodeRegisterTests (void);

static int DetectEngineInspectKRB5Generic(ThreadVars *tv,
        DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
        const Signature *s, const SigMatchData *smd,
        Flow *f, uint8_t flags, void *alstate,
        void *txv, uint64_t tx_id);

static int g_krb5_err_code_list_id = 0;

/**
 * \brief Registration function for krb5_err_code: keyword
 *
 * This function is called once in the 'lifetime' of the engine.
 */
void DetectKrb5ErrCodeRegister(void) {
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].name = "krb5_err_code";
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].desc = "match Kerberos 5 error code";
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].url = "/rules/kerberos-keywords.html#krb5-err-code";
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].Match = NULL;
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].AppLayerTxMatch = DetectKrb5ErrCodeMatch;
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].Setup = DetectKrb5ErrCodeSetup;
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].Free = DetectKrb5ErrCodeFree;
    sigmatch_table[DETECT_AL_KRB5_ERRCODE].RegisterTests = DetectKrb5ErrCodeRegisterTests;

    DetectAppLayerInspectEngineRegister("krb5_err_code",
            ALPROTO_KRB5, SIG_FLAG_TOSERVER, 0,
            DetectEngineInspectKRB5Generic);

    DetectAppLayerInspectEngineRegister("krb5_err_code",
            ALPROTO_KRB5, SIG_FLAG_TOCLIENT, 0,
            DetectEngineInspectKRB5Generic);

    /* set up the PCRE for keyword parsing */
    DetectSetupParseRegexes(PARSE_REGEX, &parse_regex, &parse_regex_study);

    g_krb5_err_code_list_id = DetectBufferTypeRegister("krb5_err_code");
    SCLogDebug("g_krb5_err_code_list_id %d", g_krb5_err_code_list_id);
}

static int DetectEngineInspectKRB5Generic(ThreadVars *tv,
        DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
        const Signature *s, const SigMatchData *smd,
        Flow *f, uint8_t flags, void *alstate,
        void *txv, uint64_t tx_id)
{
    return DetectEngineInspectGenericList(tv, de_ctx, det_ctx, s, smd,
                                          f, flags, alstate, txv, tx_id);
}

/**
 * \brief This function is used to match KRB5 rule option on a packet
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch with context that we will cast into DetectKrb5Data
 *
 * \retval 0 no match
 * \retval 1 match
 */
static int DetectKrb5ErrCodeMatch (DetectEngineThreadCtx *det_ctx,
                                   Flow *f, uint8_t flags, void *state,
                                   void *txv, const Signature *s,
                                   const SigMatchCtx *ctx)
{
    int32_t err_code;
    int ret;
    const DetectKrb5ErrCodeData *dd = (const DetectKrb5ErrCodeData *)ctx;

    SCEnter();

    ret = rs_krb5_tx_get_errcode(txv, &err_code);
    if (ret != 0)
        SCReturnInt(0);

    if (dd->err_code == err_code)
        SCReturnInt(1);

    SCReturnInt(0);
}

/**
 * \brief This function is used to parse options passed via krb5_errcode: keyword
 *
 * \param krb5str Pointer to the user provided krb5_err_code options
 *
 * \retval krb5d pointer to DetectKrb5Data on success
 * \retval NULL on failure
 */
static DetectKrb5ErrCodeData *DetectKrb5ErrCodeParse (const char *krb5str)
{
    DetectKrb5ErrCodeData *krb5d = NULL;
    char arg1[4] = "";
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study,
                    krb5str, strlen(krb5str),
                    0, 0, ov, MAX_SUBSTRINGS);
    if (ret != 2) {
        SCLogError(SC_ERR_PCRE_MATCH, "parse error, ret %" PRId32 "", ret);
        goto error;
    }

    res = pcre_copy_substring((char *) krb5str, ov, MAX_SUBSTRINGS, 1, arg1, sizeof(arg1));
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_copy_substring failed");
        goto error;
    }

    krb5d = SCMalloc(sizeof (DetectKrb5ErrCodeData));
    if (unlikely(krb5d == NULL))
        goto error;
    krb5d->err_code = (int32_t)atoi(arg1);

    return krb5d;

error:
    if (krb5d)
        SCFree(krb5d);
    return NULL;
}

/**
 * \brief parse the options from the 'krb5_err_code' keyword in the rule into
 *        the Signature data structure.
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param krb5str pointer to the user provided options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
static int DetectKrb5ErrCodeSetup (DetectEngineCtx *de_ctx, Signature *s, const char *krb5str)
{
    DetectKrb5ErrCodeData *krb5d = NULL;
    SigMatch *sm = NULL;

    if (DetectSignatureSetAppProto(s, ALPROTO_KRB5) != 0)
        return -1;

    krb5d = DetectKrb5ErrCodeParse(krb5str);
    if (krb5d == NULL)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_AL_KRB5_ERRCODE;
    sm->ctx = (void *)krb5d;

    SigMatchAppendSMToList(s, sm, g_krb5_err_code_list_id);

    return 0;

error:
    if (krb5d != NULL)
        DetectKrb5ErrCodeFree(krb5d);
    if (sm != NULL)
        SCFree(sm);
    return -1;
}

/**
 * \brief this function will free memory associated with DetectKrb5Data
 *
 * \param ptr pointer to DetectKrb5Data
 */
static void DetectKrb5ErrCodeFree(void *ptr) {
    DetectKrb5ErrCodeData *krb5d = (DetectKrb5ErrCodeData *)ptr;

    SCFree(krb5d);
}

#ifdef UNITTESTS

/**
 * \test description of the test
 */

static int DetectKrb5ErrCodeParseTest01 (void)
{
    DetectKrb5ErrCodeData *krb5d = DetectKrb5ErrCodeParse("10");
    FAIL_IF_NULL(krb5d);
    FAIL_IF(!(krb5d->err_code == 10));
    DetectKrb5ErrCodeFree(krb5d);
    PASS;
}

static int DetectKrb5ErrCodeSignatureTest01 (void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    Signature *sig = DetectEngineAppendSig(de_ctx, "alert krb5 any any -> any any (krb5_err_code:10; sid:1; rev:1;)");
    FAIL_IF_NULL(sig);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectKrb5ErrCode
 */
static void DetectKrb5ErrCodeRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("DetectKrb5ErrCodeParseTest01", DetectKrb5ErrCodeParseTest01);
    UtRegisterTest("DetectKrb5ErrCodeSignatureTest01",
                   DetectKrb5ErrCodeSignatureTest01);
#endif /* UNITTESTS */
}
