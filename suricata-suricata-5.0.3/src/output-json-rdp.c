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
 * \author Zach Kelly <zach.kelly@lmco.com>
 *
 * Application layer logger for RDP
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
#include "app-layer-rdp.h"
#include "output-json-rdp.h"
#include "rust-rdp-log-gen.h"

typedef struct LogRdpFileCtx_ {
    LogFileCtx *file_ctx;
    uint32_t    flags;
} LogRdpFileCtx;

typedef struct LogRdpLogThread_ {
    LogRdpFileCtx *rdplog_ctx;
    uint32_t         count;
    MemBuffer       *buffer;
} LogRdpLogThread;

static int JsonRdpLogger(ThreadVars *tv, void *thread_data,
    const Packet *p, Flow *f, void *state, void *tx, uint64_t tx_id)
{
    LogRdpLogThread *thread = thread_data;

    json_t *js = CreateJSONHeader(p, LOG_DIR_PACKET, "rdp");
    if (unlikely(js == NULL)) {
        return TM_ECODE_FAILED;
    }

    json_t *rdp_js = rs_rdp_to_json(tx);
    if (unlikely(rdp_js == NULL)) {
        goto error;
    }
    json_object_set_new(js, "rdp", rdp_js);

    MemBufferReset(thread->buffer);
    OutputJSONBuffer(js, thread->rdplog_ctx->file_ctx, &thread->buffer);
    json_decref(js);

    return TM_ECODE_OK;

error:
    json_decref(js);
    return TM_ECODE_FAILED;
}

static void OutputRdpLogDeInitCtxSub(OutputCtx *output_ctx)
{
    LogRdpFileCtx *rdplog_ctx = (LogRdpFileCtx *)output_ctx->data;
    SCFree(rdplog_ctx);
    SCFree(output_ctx);
}

static OutputInitResult OutputRdpLogInitSub(ConfNode *conf,
    OutputCtx *parent_ctx)
{
    OutputInitResult result = { NULL, false };
    OutputJsonCtx *ajt = parent_ctx->data;

    LogRdpFileCtx *rdplog_ctx = SCCalloc(1, sizeof(*rdplog_ctx));
    if (unlikely(rdplog_ctx == NULL)) {
        return result;
    }
    rdplog_ctx->file_ctx = ajt->file_ctx;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(*output_ctx));
    if (unlikely(output_ctx == NULL)) {
        SCFree(rdplog_ctx);
        return result;
    }
    output_ctx->data = rdplog_ctx;
    output_ctx->DeInit = OutputRdpLogDeInitCtxSub;

    AppLayerParserRegisterLogger(IPPROTO_TCP, ALPROTO_RDP);

    SCLogDebug("rdp log sub-module initialized.");

    result.ctx = output_ctx;
    result.ok = true;
    return result;
}

static TmEcode JsonRdpLogThreadInit(ThreadVars *t, const void *initdata, void **data)
{
    if (initdata == NULL) {
        SCLogDebug("Error getting context for EveLogRdp. \"initdata\" is NULL.");
        return TM_ECODE_FAILED;
    }

    LogRdpLogThread *thread = SCCalloc(1, sizeof(*thread));
    if (unlikely(thread == NULL)) {
        return TM_ECODE_FAILED;
    }

    thread->buffer = MemBufferCreateNew(JSON_OUTPUT_BUFFER_SIZE);
    if (unlikely(thread->buffer == NULL)) {
        SCFree(thread);
        return TM_ECODE_FAILED;
    }

    thread->rdplog_ctx = ((OutputCtx *)initdata)->data;
    *data = (void *)thread;

    return TM_ECODE_OK;
}

static TmEcode JsonRdpLogThreadDeinit(ThreadVars *t, void *data)
{
    LogRdpLogThread *thread = (LogRdpLogThread *)data;
    if (thread == NULL) {
        return TM_ECODE_OK;
    }
    if (thread->buffer != NULL) {
        MemBufferFree(thread->buffer);
    }
    SCFree(thread);
    return TM_ECODE_OK;
}

void JsonRdpLogRegister(void)
{
    if (ConfGetNode("app-layer.protocols.rdp") == NULL) {
        return;
    }
    /* Register as an eve sub-module. */
    OutputRegisterTxSubModule(
        LOGGER_JSON_RDP,
        "eve-log",
        "JsonRdpLog",
        "eve-log.rdp",
        OutputRdpLogInitSub,
        ALPROTO_RDP,
        JsonRdpLogger,
        JsonRdpLogThreadInit,
        JsonRdpLogThreadDeinit,
        NULL
    );

    SCLogDebug("rdp json logger registered.");
}
