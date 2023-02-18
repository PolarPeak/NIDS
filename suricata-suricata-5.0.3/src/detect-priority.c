/* Copyright (C) 2007-2019 Open Information Security Foundation
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
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Implements the priority keyword
 */

#include "suricata-common.h"
#include "detect.h"
#include "detect-parse.h"
#include "detect-priority.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "util-error.h"
#include "util-debug.h"
#include "util-unittest.h"

#define PARSE_REGEX "^\\s*(\\d+|\"\\d+\")\\s*$"

static pcre *regex = NULL;
static pcre_extra *regex_study = NULL;

static int DetectPrioritySetup (DetectEngineCtx *, Signature *, const char *);
void SCPriorityRegisterTests(void);

/**
 * \brief Registers the handler functions for the "priority" keyword
 */
void DetectPriorityRegister (void)
{
    sigmatch_table[DETECT_PRIORITY].name = "priority";
    sigmatch_table[DETECT_PRIORITY].desc = "rules with a higher priority will be examined first";
    sigmatch_table[DETECT_PRIORITY].url = "/rules/meta.html#priority";
    sigmatch_table[DETECT_PRIORITY].Setup = DetectPrioritySetup;
    sigmatch_table[DETECT_PRIORITY].RegisterTests = SCPriorityRegisterTests;

    DetectSetupParseRegexes(PARSE_REGEX, &regex, &regex_study);
}

static int DetectPrioritySetup (DetectEngineCtx *de_ctx, Signature *s, const char *rawstr)
{
    char copy_str[128] = "";

#define MAX_SUBSTRINGS 30
    int ret = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(regex, regex_study, rawstr, strlen(rawstr), 0, 0, ov, 30);
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_MATCH, "Invalid Priority in Signature "
                     "- %s", rawstr);
        return -1;
    }

    ret = pcre_copy_substring((char *)rawstr, ov, 30, 1, copy_str, sizeof(copy_str));
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_copy_substring failed");
        return -1;
    }

    long prio = 0;
    char *endptr = NULL;
    prio = strtol(copy_str, &endptr, 10);
    if (endptr == NULL || *endptr != '\0') {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Saw an invalid character as arg "
                   "to priority keyword");
        return -1;
    }

    if (s->init_data->init_flags & SIG_FLAG_INIT_PRIO_EXPLICT) {
        SCLogWarning(SC_ERR_CONFLICTING_RULE_KEYWORDS, "duplicate priority "
                "keyword. Using highest priority in the rule");
        s->prio = MIN(s->prio, prio);
    } else {
        s->prio = prio;
        s->init_data->init_flags |= SIG_FLAG_INIT_PRIO_EXPLICT;
    }
    return 0;
}

/*------------------------------Unittests-------------------------------------*/

#ifdef UNITTESTS

static int DetectPriorityTest01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"Priority test\"; priority:2; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    FAIL_IF_NOT(de_ctx->sig_list->prio == 2);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectPriorityTest02(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    Signature *sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:1; sid:1;)");
    FAIL_IF_NULL(sig);
    FAIL_IF_NOT(sig->prio == 1);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:boo; sid:2;)");
    FAIL_IF_NOT_NULL(sig);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:10boo; sid:3;)");
    FAIL_IF_NOT_NULL(sig);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:b10oo; sid:4;)");
    FAIL_IF_NOT_NULL(sig);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:boo10; sid:5;)");
    FAIL_IF_NOT_NULL(sig);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:-1; sid:6;)");
    FAIL_IF_NOT_NULL(sig);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; sid:7;)");
    FAIL_IF_NULL(sig);
    FAIL_IF_NOT(sig->prio == 3);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:5; priority:4; sid:8;)");
    FAIL_IF_NULL(sig);
    FAIL_IF_NOT(sig->prio == 4);

    sig = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                  "(msg:\"Priority test\"; priority:5; priority:4; "
                  "priority:1; sid:9;)");
    FAIL_IF_NULL(sig);
    FAIL_IF_NOT(sig->prio == 1);

    DetectEngineCtxFree(de_ctx);
    PASS;
}
#endif /* UNITTESTS */

/**
 * \brief This function registers unit tests for Classification Config API.
 */
void SCPriorityRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectPriorityTest01", DetectPriorityTest01);
    UtRegisterTest("DetectPriorityTest02", DetectPriorityTest02);
#endif /* UNITTESTS */
}
