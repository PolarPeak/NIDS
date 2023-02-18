
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

#include "../detect-parse.h"
#include "../util-unittest.h"

/**
 * \test DetectParseTest01 is a regression test against a memory leak
 * in the case of multiple signatures with different revisions
 * Leak happened in function DetectEngineSignatureIsDuplicate
 */

static int DetectParseTest01 (void)
{
    DetectEngineCtx * de_ctx = DetectEngineCtxInit();
    FAIL_IF(DetectEngineAppendSig(de_ctx, "alert http any any -> any any (msg:\"sid 1 version 0\"; content:\"dummy1\"; sid:1;)") == NULL);
    DetectEngineAppendSig(de_ctx, "alert http any any -> any any (msg:\"sid 2 version 0\"; content:\"dummy2\"; sid:2;)");
    DetectEngineAppendSig(de_ctx, "alert http any any -> any any (msg:\"sid 1 version 1\"; content:\"dummy1.1\"; sid:1; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert http any any -> any any (msg:\"sid 2 version 2\"; content:\"dummy2.1\"; sid:2; rev:1;)");
    FAIL_IF(de_ctx->sig_list->next == NULL);
    DetectEngineCtxFree(de_ctx);

    PASS;
}


/**
 * \brief this function registers unit tests for DetectParse
 */
void DetectParseRegisterTests(void)
{
    UtRegisterTest("DetectParseTest01", DetectParseTest01);
}
