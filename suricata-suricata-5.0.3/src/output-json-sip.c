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
 * \author Giuseppe Longo <giuseppe@glongo.it>
 *
 * Implement JSON/eve logging app-layer SIP.
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-unittest.h"
#include "util-buffer.h"
#include "util-debug.h"
#include "util-byte.h"

#include "output.h"
#include "output-json.h"

#include "app-layer.h"
#include "app-layer-parser.h"

#include "app-layer-sip.h"
#include "output-json-sip.h"

#include "rust.h"
#include "rust-sip-log-gen.h"

typedef struct LogSIPFileCtx_ {
    LogFileCtx *file_ctx;
    OutputJsonCommonSettings cfg;
} LogSIPFileCtx;

typedef struct LogSIPLogThread_ {
    LogSIPFileCtx *siplog_ctx;
    MemBuffer          *buffer;
} LogSIPLogThread;

json_t *JsonSIPAddMetadata(const Flow *f, uint64_t tx_id)
{
    SIPState *state = FlowGetAppState(f);
    if (state) {
        SIPTransaction *tx = AppLayerParserGetTx(f->proto, ALPROTO_SIP, state, tx_id);
        if (tx) {
            return rs_sip_log_json(state, tx);
        }
    }

    return NULL;
}

static int JsonSIPLogger(ThreadVars *tv, void *thread_data,
    const Packet *p, Flow *f, void *state, void *tx, uint64_t tx_id)
{
    SIPTransaction *siptx = tx;
    LogSIPLogThread *thread = thread_data;
    json_t *js, *sipjs;

    js = CreateJSONHeader(p, LOG_DIR_PACKET, "sip");
    if (unlikely(js == NULL)) {
        return TM_ECODE_FAILED;
    }

    JsonAddCommonOptions(&thread->siplog_ctx->cfg, p, f, js);

    sipjs = rs_sip_log_json(state, siptx);
    if (unlikely(sipjs == NULL)) {
      goto error;
    }
    json_object_set_new(js, "sip", sipjs);

    MemBufferReset(thread->buffer);
    OutputJSONBuffer(js, thread->siplog_ctx->file_ctx, &thread->buffer);

    json_decref(js);
    return TM_ECODE_OK;

error:
    json_decref(js);
    return TM_ECODE_FAILED;
}

static void OutputSIPLogDeInitCtxSub(OutputCtx *output_ctx)
{
    LogSIPFileCtx *siplog_ctx = (LogSIPFileCtx *)output_ctx->data;
    SCFree(siplog_ctx);
    SCFree(output_ctx);
}

static OutputInitResult OutputSIPLogInitSub(ConfNode *conf,
    OutputCtx *parent_ctx)
{
    OutputInitResult result = { NULL, false };
    OutputJsonCtx *ajt = parent_ctx->data;

    LogSIPFileCtx *siplog_ctx = SCCalloc(1, sizeof(*siplog_ctx));
    if (unlikely(siplog_ctx == NULL)) {
        return result;
    }
    siplog_ctx->file_ctx = ajt->file_ctx;
    siplog_ctx->cfg = ajt->cfg;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(*output_ctx));
    if (unlikely(output_ctx == NULL)) {
        SCFree(siplog_ctx);
        return result;
    }
    output_ctx->data = siplog_ctx;
    output_ctx->DeInit = OutputSIPLogDeInitCtxSub;

    SCLogDebug("SIP log sub-module initialized.");

    AppLayerParserRegisterLogger(IPPROTO_UDP, ALPROTO_SIP);

    result.ctx = output_ctx;
    result.ok = true;
    return result;
}

#define OUTPUT_BUFFER_SIZE 65535

static TmEcode JsonSIPLogThreadInit(ThreadVars *t, const void *initdata, void **data)
{
    LogSIPLogThread *thread = SCCalloc(1, sizeof(*thread));
    if (unlikely(thread == NULL)) {
        return TM_ECODE_FAILED;
    }

    if (initdata == NULL) {
        SCLogDebug("Error getting context for EveLogSIP.  \"initdata\" is NULL.");
        SCFree(thread);
        return TM_ECODE_FAILED;
    }

    thread->buffer = MemBufferCreateNew(OUTPUT_BUFFER_SIZE);
    if (unlikely(thread->buffer == NULL)) {
        SCFree(thread);
        return TM_ECODE_FAILED;
    }

    thread->siplog_ctx = ((OutputCtx *)initdata)->data;
    *data = (void *)thread;

    return TM_ECODE_OK;
}

static TmEcode JsonSIPLogThreadDeinit(ThreadVars *t, void *data)
{
    LogSIPLogThread *thread = (LogSIPLogThread *)data;
    if (thread == NULL) {
        return TM_ECODE_OK;
    }
    if (thread->buffer != NULL) {
        MemBufferFree(thread->buffer);
    }
    SCFree(thread);
    return TM_ECODE_OK;
}

void JsonSIPLogRegister(void)
{
    /* Register as an eve sub-module. */
    OutputRegisterTxSubModule(LOGGER_JSON_SIP, "eve-log", "JsonSIPLog",
        "eve-log.sip", OutputSIPLogInitSub, ALPROTO_SIP,
        JsonSIPLogger, JsonSIPLogThreadInit,
        JsonSIPLogThreadDeinit, NULL);

    SCLogDebug("SIP JSON logger registered.");
}
