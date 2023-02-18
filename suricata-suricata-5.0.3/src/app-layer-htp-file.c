/* Copyright (C) 2007-2011 Open Information Security Foundation
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
 * This file provides HTTP protocol file handling support for the engine
 * using HTP library.
 */

#include "suricata.h"
#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "threads.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-radix-tree.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer.h"
#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-htp.h"
#include "app-layer-htp-file.h"

#include "util-spm.h"
#include "util-debug.h"
#include "util-time.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "flow-util.h"

#include "detect-engine.h"
#include "detect-engine-state.h"
#include "detect-parse.h"

#include "conf.h"

#include "util-memcmp.h"

/**
 *  \brief Open the file with "filename" and pass the first chunk
 *         of data if any.
 *
 *  \param s http state
 *  \param filename name of the file
 *  \param filename_len length of the name
 *  \param data data chunk (if any)
 *  \param data_len length of the data portion
 *  \param direction flow direction
 *
 *  \retval  0 ok
 *  \retval -1 error
 *  \retval -2 not handling files on this flow
 */
int HTPFileOpen(HtpState *s, const uint8_t *filename, uint16_t filename_len,
        const uint8_t *data, uint32_t data_len,
        uint64_t txid, uint8_t direction)
{
    int retval = 0;
    uint16_t flags = 0;
    FileContainer *files = NULL;
    const StreamingBufferConfig *sbcfg = NULL;

    SCLogDebug("data %p data_len %"PRIu32, data, data_len);

    if (s == NULL) {
        SCReturnInt(-1);
    }

    if (direction & STREAM_TOCLIENT) {
        if (s->files_tc == NULL) {
            s->files_tc = FileContainerAlloc();
            if (s->files_tc == NULL) {
                retval = -1;
                goto end;
            }
        }

        files = s->files_tc;

        flags = FileFlowToFlags(s->f, STREAM_TOCLIENT);

        if ((s->flags & HTP_FLAG_STORE_FILES_TS) ||
                ((s->flags & HTP_FLAG_STORE_FILES_TX_TS) && txid == s->store_tx_id)) {
            flags |= FILE_STORE;
            flags &= ~FILE_NOSTORE;
        } else if (!(flags & FILE_STORE) && (s->f->file_flags & FLOWFILE_NO_STORE_TC)) {
            flags |= FILE_NOSTORE;
        }

        sbcfg = &s->cfg->response.sbcfg;

    } else {
        if (s->files_ts == NULL) {
            s->files_ts = FileContainerAlloc();
            if (s->files_ts == NULL) {
                retval = -1;
                goto end;
            }
        }

        files = s->files_ts;

        flags = FileFlowToFlags(s->f, STREAM_TOSERVER);
        if ((s->flags & HTP_FLAG_STORE_FILES_TC) ||
                ((s->flags & HTP_FLAG_STORE_FILES_TX_TC) && txid == s->store_tx_id)) {
            flags |= FILE_STORE;
            flags &= ~FILE_NOSTORE;
        } else if (!(flags & FILE_STORE) && (s->f->file_flags & FLOWFILE_NO_STORE_TS)) {
            flags |= FILE_NOSTORE;
        }

        sbcfg = &s->cfg->request.sbcfg;
    }

    if (FileOpenFileWithId(files, sbcfg, s->file_track_id++,
                filename, filename_len,
                data, data_len, flags) != 0)
    {
        retval = -1;
    }

    FileSetTx(files->tail, txid);

end:
    SCReturnInt(retval);
}

/**
 * Performs parsing of the content-range value
 *
 * @param[in] rawvalue
 * @param[out] range
 *
 * @return HTP_OK on success, HTP_ERROR on failure.
 */
int HTPParseContentRange(bstr * rawvalue, HtpContentRange *range)
{
    unsigned char *data = bstr_ptr(rawvalue);
    size_t len = bstr_len(rawvalue);
    size_t pos = 0;
    size_t last_pos;

    // skip spaces and units
    while (pos < len && data[pos] == ' ')
        pos++;
    while (pos < len && data[pos] != ' ')
        pos++;
    while (pos < len && data[pos] == ' ')
        pos++;

    // initialize to unseen
    range->start = -1;
    range->end = -1;
    range->size = -1;

    if (pos == len) {
        // missing data
        return -1;
    }

    if (data[pos] == '*') {
        // case with size only
        if (len <= pos + 1 || data[pos+1] != '/') {
            range->size = -1;
            return -1;
        }
        pos += 2;
        range->size = bstr_util_mem_to_pint(data + pos, len - pos, 10, &last_pos);
    } else {
        // case with start and end
        range->start = bstr_util_mem_to_pint(data + pos, len - pos, 10, &last_pos);
        pos += last_pos;
        if (len <= pos + 1 || data[pos] != '-') {
            return -1;
        }
        pos++;
        range->end = bstr_util_mem_to_pint(data + pos, len - pos, 10, &last_pos);
        pos += last_pos;
        if (len <= pos + 1 || data[pos] != '/') {
            return -1;
        }
        pos++;
        if (data[pos] != '*') {
            // case with size
            range->size = bstr_util_mem_to_pint(data + pos, len - pos, 10, &last_pos);
        }
    }

    return 0;
}

/**
 *  \brief Sets range for a file
 *
 *  \param s http state
 *  \param rawvalue raw header value
 *
 *  \retval 0 ok
 *  \retval -1 error
 *  \retval -2 error parsing
 *  \retval -3 error negative end in range
 */
int HTPFileSetRange(HtpState *s, bstr *rawvalue)
{
    SCEnter();

    if (s == NULL) {
        SCReturnInt(-1);
    }

    FileContainer * files = s->files_tc;
    if (files == NULL) {
        SCLogDebug("no files in state");
        SCReturnInt(-1);
    }

    HtpContentRange crparsed;
    if (HTPParseContentRange(rawvalue, &crparsed) != 0) {
        SCLogDebug("parsing range failed");
        SCReturnInt(-2);
    }
    if (crparsed.end <= 0) {
        SCLogDebug("negative end in range");
        SCReturnInt(-3);
    }
    int retval = FileSetRange(files, crparsed.start, crparsed.end);
    if (retval == -1) {
        SCLogDebug("set range failed");
    }
    SCReturnInt(retval);
}

/**
 *  \brief Store a chunk of data in the flow
 *
 *  \param s http state
 *  \param data data chunk (if any)
 *  \param data_len length of the data portion
 *  \param direction flow direction
 *
 *  \retval 0 ok
 *  \retval -1 error
 *  \retval -2 file doesn't need storing
 */
int HTPFileStoreChunk(HtpState *s, const uint8_t *data, uint32_t data_len,
        uint8_t direction)
{
    SCEnter();

    int retval = 0;
    int result = 0;
    FileContainer *files = NULL;

    if (s == NULL) {
        SCReturnInt(-1);
    }

    if (direction & STREAM_TOCLIENT) {
        files = s->files_tc;
    } else {
        files = s->files_ts;
    }

    if (files == NULL) {
        SCLogDebug("no files in state");
        retval = -1;
        goto end;
    }

    result = FileAppendData(files, data, data_len);
    if (result == -1) {
        SCLogDebug("appending data failed");
        retval = -1;
    } else if (result == -2) {
        retval = -2;
    }

end:
    SCReturnInt(retval);
}

/**
 *  \brief Close the file in the flow
 *
 *  \param s http state
 *  \param data data chunk if any
 *  \param data_len length of the data portion
 *  \param flags flags to indicate events
 *  \param direction flow direction
 *
 *  Currently on the FLOW_FILE_TRUNCATED flag is implemented, indicating
 *  that the file isn't complete but we're stopping storing it.
 *
 *  \retval 0 ok
 *  \retval -1 error
 *  \retval -2 not storing files on this flow/tx
 */
int HTPFileClose(HtpState *s, const uint8_t *data, uint32_t data_len,
        uint8_t flags, uint8_t direction)
{
    SCEnter();

    int retval = 0;
    int result = 0;
    FileContainer *files = NULL;

    if (s == NULL) {
        SCReturnInt(-1);
    }

    if (direction & STREAM_TOCLIENT) {
        files = s->files_tc;
    } else {
        files = s->files_ts;
    }

    if (files == NULL) {
        retval = -1;
        goto end;
    }

    result = FileCloseFile(files, data, data_len, flags);
    if (result == -1) {
        retval = -1;
    } else if (result == -2) {
        retval = -2;
    }

end:
    SCReturnInt(retval);
}

#ifdef UNITTESTS
static int HTPFileParserTest01(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();
    HtpState *http_state = NULL;
    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);

    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

static int HTPFileParserTest02(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 337\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf3, httplen3);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf4, httplen4);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);
    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);
    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

static int HTPFileParserTest03(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 337\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "file";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    uint8_t httpbuf5[] = "content\r\n";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */

    uint8_t httpbuf6[] = "-----------------------------277531038314945--";
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf3, httplen3);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf4, httplen4);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 5 size %u <<<<\n", httplen5);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf5, httplen5);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 6 size %u <<<<\n", httplen6);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf6, httplen6);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);

    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->head);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);
    FAIL_IF(FileDataSize(http_state->files_ts->head) != 11);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

static int HTPFileParserTest04(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 373\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "file0123456789abcdefghijklmnopqrstuvwxyz";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    uint8_t httpbuf5[] = "content\r\n";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */

    uint8_t httpbuf6[] = "-----------------------------277531038314945--";
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf3, httplen3);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf4, httplen4);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 5 size %u <<<<\n", httplen5);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf5, httplen5);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 6 size %u <<<<\n", httplen6);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf6, httplen6);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);

    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->head);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

static int HTPFileParserTest05(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 544\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------277531038314945\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "Content-Disposition: form-data; name=\"uploadfile_1\"; filename=\"somepicture2.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "FILECONTENT\r\n"
        "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 size %u <<<<\n", httplen1);
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);

    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->head);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);

    FAIL_IF(http_state->files_ts->head == http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->head->next != http_state->files_ts->tail);

    FAIL_IF(StreamingBufferCompareRawData(http_state->files_ts->head->sb,
                (uint8_t *)"filecontent", 11) != 1);

    FAIL_IF(StreamingBufferCompareRawData(http_state->files_ts->tail->sb,
                (uint8_t *)"FILECONTENT", 11) != 1);
    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

/** \test first multipart part contains file but doesn't end in first chunk */
static int HTPFileParserTest06(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 544\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------27753103831494";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "5\r\nContent-Disposition: form-data; name=\"uploadfile_1\"; filename=\"somepicture2.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "FILECONTENT\r\n"
        "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 size %u <<<<\n", httplen1);
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);

    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->head);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);

    FAIL_IF(http_state->files_ts->head == http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->head->next != http_state->files_ts->tail);

    FAIL_IF(StreamingBufferCompareRawData(http_state->files_ts->head->sb,
                (uint8_t *)"filecontent", 11) != 1);

    FAIL_IF(StreamingBufferCompareRawData(http_state->files_ts->tail->sb,
                (uint8_t *)"FILECONTENT", 11) != 1);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

/** \test POST, but not multipart */
static int HTPFileParserTest07(void)
{
    uint8_t httpbuf1[] = "POST /filename HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Length: 11\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "FILECONTENT";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 size %u <<<<\n", httplen1);
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);
    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);

    FAIL_IF(StreamingBufferCompareRawData(http_state->files_ts->tail->sb,
                (uint8_t *)"FILECONTENT", 11) != 1);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

static int HTPFileParserTest08(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "filecontent\r\n\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();
    HtpState *http_state = NULL;
    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    void *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP,f->alstate, 0);
    FAIL_IF_NULL(tx);

    AppLayerDecoderEvents *decoder_events = AppLayerParserGetEventsByTx(IPPROTO_TCP, ALPROTO_HTTP, tx);
    FAIL_IF_NULL(decoder_events);

    FAIL_IF(decoder_events->cnt != 2);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

/** \test invalid header: Somereallylongheaderstr: has no value */
static int HTPFileParserTest09(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 337\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Somereallylongheaderstr:\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf3, httplen3);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf4, httplen4);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    void *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP,f->alstate, 0);
    FAIL_IF_NULL(tx);

    AppLayerDecoderEvents *decoder_events = AppLayerParserGetEventsByTx(IPPROTO_TCP, ALPROTO_HTTP, tx);
    FAIL_IF_NULL(decoder_events);

    FAIL_IF(decoder_events->cnt != 1);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

/** \test empty entries */
static int HTPFileParserTest10(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 337\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Somereallylongheaderstr: with a good value\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf3, httplen3);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf4, httplen4);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    void *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP,f->alstate, 0);
    FAIL_IF_NULL(tx);
    AppLayerDecoderEvents *decoder_events = AppLayerParserGetEventsByTx(IPPROTO_TCP, ALPROTO_HTTP, tx);
    FAIL_IF_NOT_NULL(decoder_events);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

/** \test filedata cut in two pieces */
static int HTPFileParserTest11(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=----WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Length: 1102\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "Content-Disposition: form-data; name=\"PROGRESS_URL\"\r\n"
                         "\r\n"
                         "http://somserver.com/progress.php?UPLOAD_IDENTIFIER=XXXXXXXXX.XXXXXXXXXX.XXXXXXXX.XX.X\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"DESTINATION_DIR\"\r\n"
                         "\r\n"
                         "10\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"js_enabled\"\r\n"
                         "\r\n"
                         "1"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"signature\"\r\n"
                         "\r\n"
                         "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"upload_files\"\r\n"
                         "\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"terms\"\r\n"
                         "\r\n"
                         "1"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"file[]\"\r\n"
                         "\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"description[]\"\r\n"
                         "\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo\r\n"
                         "Content-Disposition: form-data; name=\"upload_file[]\"; filename=\"filename.doc\"\r\n"
                         "Content-Type: application/msword\r\n"
                         "\r\n"
                         "FILE";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "CONTENT\r\n"
                         "------WebKitFormBoundaryBRDbP74mBhBxsIdo--";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&ssn, 0, sizeof(ssn));

    Flow *f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    FAIL_IF_NULL(f);
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER,
                            httpbuf2, httplen2);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER,
                            httpbuf3, httplen3);
    FAIL_IF_NOT(r == 0);

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParserParse(NULL, alp_tctx, f, ALPROTO_HTTP,
                            STREAM_TOSERVER | STREAM_EOF, httpbuf4, httplen4);
    FAIL_IF_NOT(r == 0);

    http_state = f->alstate;
    FAIL_IF_NULL(http_state);

    void *txtmp = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP,f->alstate, 0);
    FAIL_IF_NULL(txtmp);

    AppLayerDecoderEvents *decoder_events = AppLayerParserGetEventsByTx(IPPROTO_TCP, ALPROTO_HTTP, txtmp);
    FAIL_IF_NOT_NULL(decoder_events);

    htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, http_state, 0);
    FAIL_IF_NULL(tx);
    FAIL_IF_NULL(tx->request_method);

    FAIL_IF(memcmp(bstr_util_strdup_to_c(tx->request_method), "POST", 4) != 0);

    FAIL_IF_NULL(http_state->files_ts);
    FAIL_IF_NULL(http_state->files_ts->tail);
    FAIL_IF(http_state->files_ts->tail->state != FILE_STATE_CLOSED);

    FAIL_IF(StreamingBufferCompareRawData(http_state->files_ts->tail->sb,
                (uint8_t *)"FILECONTENT", 11) != 1);

    AppLayerParserThreadCtxFree(alp_tctx);
    StreamTcpFreeConfig(TRUE);
    UTHFreeFlow(f);
    PASS;
}

void AppLayerHtpFileRegisterTests (void);
#include "tests/app-layer-htp-file.c"
#endif /* UNITTESTS */

void HTPFileParserRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("HTPFileParserTest01", HTPFileParserTest01);
    UtRegisterTest("HTPFileParserTest02", HTPFileParserTest02);
    UtRegisterTest("HTPFileParserTest03", HTPFileParserTest03);
    UtRegisterTest("HTPFileParserTest04", HTPFileParserTest04);
    UtRegisterTest("HTPFileParserTest05", HTPFileParserTest05);
    UtRegisterTest("HTPFileParserTest06", HTPFileParserTest06);
    UtRegisterTest("HTPFileParserTest07", HTPFileParserTest07);
    UtRegisterTest("HTPFileParserTest08", HTPFileParserTest08);
    UtRegisterTest("HTPFileParserTest09", HTPFileParserTest09);
    UtRegisterTest("HTPFileParserTest10", HTPFileParserTest10);
    UtRegisterTest("HTPFileParserTest11", HTPFileParserTest11);
#endif /* UNITTESTS */
}
