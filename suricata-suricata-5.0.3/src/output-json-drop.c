/* Copyright (C) 2007-2013 Open Information Security Foundation
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
 * \author Tom DeCanio <td@npulsetech.com>
 *
 * JSON Drop log module to log the dropped packet information
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "tm-threads.h"
#include "threadvars.h"
#include "util-debug.h"

#include "decode-ipv4.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-reference.h"

#include "output.h"
#include "output-json.h"
#include "output-json-alert.h"
#include "output-json-drop.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-classification-config.h"
#include "util-privs.h"
#include "util-print.h"
#include "util-proto-name.h"
#include "util-logopenfile.h"
#include "util-time.h"
#include "util-buffer.h"

#define MODULE_NAME "JsonDropLog"

#define LOG_DROP_ALERTS 1

typedef struct JsonDropOutputCtx_ {
    LogFileCtx *file_ctx;
    uint8_t flags;
    OutputJsonCommonSettings cfg;
} JsonDropOutputCtx;

typedef struct JsonDropLogThread_ {
    JsonDropOutputCtx *drop_ctx;
    MemBuffer *buffer;
} JsonDropLogThread;

/* default to true as this has been the default behavior for a long time */
static int g_droplog_flows_start = 1;

/**
 * \brief   Log the dropped packets in netfilter format when engine is running
 *          in inline mode
 *
 * \param tv    Pointer the current thread variables
 * \param p     Pointer the packet which is being logged
 *
 * \return return TM_EODE_OK on success
 */
static int DropLogJSON (JsonDropLogThread *aft, const Packet *p)
{
    JsonDropOutputCtx *drop_ctx = aft->drop_ctx;

    json_t *js = CreateJSONHeader(p, LOG_DIR_PACKET, "drop");
    if (unlikely(js == NULL))
        return TM_ECODE_OK;

    JsonAddCommonOptions(&drop_ctx->cfg, p, p->flow, js);

    json_t *djs = json_object();
    if (unlikely(djs == NULL)) {
        json_decref(js);
        return TM_ECODE_OK;
    }

    /* reset */
    MemBufferReset(aft->buffer);

    uint16_t proto = 0;
    if (PKT_IS_IPV4(p)) {
        json_object_set_new(djs, "len", json_integer(IPV4_GET_IPLEN(p)));
        json_object_set_new(djs, "tos", json_integer(IPV4_GET_IPTOS(p)));
        json_object_set_new(djs, "ttl", json_integer(IPV4_GET_IPTTL(p)));
        json_object_set_new(djs, "ipid", json_integer(IPV4_GET_IPID(p)));
        proto = IPV4_GET_IPPROTO(p);
    } else if (PKT_IS_IPV6(p)) {
        json_object_set_new(djs, "len", json_integer(IPV6_GET_PLEN(p)));
        json_object_set_new(djs, "tc", json_integer(IPV6_GET_CLASS(p)));
        json_object_set_new(djs, "hoplimit", json_integer(IPV6_GET_HLIM(p)));
        json_object_set_new(djs, "flowlbl", json_integer(IPV6_GET_FLOW(p)));
        proto = IPV6_GET_L4PROTO(p);
    }
    switch (proto) {
        case IPPROTO_TCP:
            if (PKT_IS_TCP(p)) {
                json_object_set_new(djs, "tcpseq", json_integer(TCP_GET_SEQ(p)));
                json_object_set_new(djs, "tcpack", json_integer(TCP_GET_ACK(p)));
                json_object_set_new(djs, "tcpwin", json_integer(TCP_GET_WINDOW(p)));
                json_object_set_new(djs, "syn", TCP_ISSET_FLAG_SYN(p) ? json_true() : json_false());
                json_object_set_new(djs, "ack", TCP_ISSET_FLAG_ACK(p) ? json_true() : json_false());
                json_object_set_new(djs, "psh", TCP_ISSET_FLAG_PUSH(p) ? json_true() : json_false());
                json_object_set_new(djs, "rst", TCP_ISSET_FLAG_RST(p) ? json_true() : json_false());
                json_object_set_new(djs, "urg", TCP_ISSET_FLAG_URG(p) ? json_true() : json_false());
                json_object_set_new(djs, "fin", TCP_ISSET_FLAG_FIN(p) ? json_true() : json_false());
                json_object_set_new(djs, "tcpres", json_integer(TCP_GET_RAW_X2(p->tcph)));
                json_object_set_new(djs, "tcpurgp", json_integer(TCP_GET_URG_POINTER(p)));
            }
            break;
        case IPPROTO_UDP:
            if (PKT_IS_UDP(p)) {
                json_object_set_new(djs, "udplen", json_integer(UDP_GET_LEN(p)));
            }
            break;
        case IPPROTO_ICMP:
            if (PKT_IS_ICMPV4(p)) {
                json_object_set_new(djs, "icmp_id", json_integer(ICMPV4_GET_ID(p)));
                json_object_set_new(djs, "icmp_seq", json_integer(ICMPV4_GET_SEQ(p)));
            } else if(PKT_IS_ICMPV6(p)) {
                json_object_set_new(djs, "icmp_id", json_integer(ICMPV6_GET_ID(p)));
                json_object_set_new(djs, "icmp_seq", json_integer(ICMPV6_GET_SEQ(p)));
            }
            break;
    }
    json_object_set_new(js, "drop", djs);

    if (aft->drop_ctx->flags & LOG_DROP_ALERTS) {
        int logged = 0;
        int i;
        for (i = 0; i < p->alerts.cnt; i++) {
            const PacketAlert *pa = &p->alerts.alerts[i];
            if (unlikely(pa->s == NULL)) {
                continue;
            }
            if ((pa->action & (ACTION_REJECT|ACTION_REJECT_DST|ACTION_REJECT_BOTH)) ||
               ((pa->action & ACTION_DROP) && EngineModeIsIPS()))
            {
                AlertJsonHeader(NULL, p, pa, js, 0);
                logged = 1;
            }
        }
        if (logged == 0) {
            if (p->alerts.drop.action != 0) {
                const PacketAlert *pa = &p->alerts.drop;
                AlertJsonHeader(NULL, p, pa, js, 0);
            }
        }
    }

    OutputJSONBuffer(js, aft->drop_ctx->file_ctx, &aft->buffer);
    json_object_del(js, "drop");
    json_object_clear(js);
    json_decref(js);

    return TM_ECODE_OK;
}

static TmEcode JsonDropLogThreadInit(ThreadVars *t, const void *initdata, void **data)
{
    JsonDropLogThread *aft = SCMalloc(sizeof(JsonDropLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(*aft));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for EveLogDrop.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    aft->buffer = MemBufferCreateNew(JSON_OUTPUT_BUFFER_SIZE);
    if (aft->buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    /** Use the Ouptut Context (file pointer and mutex) */
    aft->drop_ctx = ((OutputCtx *)initdata)->data;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

static TmEcode JsonDropLogThreadDeinit(ThreadVars *t, void *data)
{
    JsonDropLogThread *aft = (JsonDropLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->buffer);

    /* clear memory */
    memset(aft, 0, sizeof(*aft));

    SCFree(aft);
    return TM_ECODE_OK;
}

static void JsonDropOutputCtxFree(JsonDropOutputCtx *drop_ctx)
{
    if (drop_ctx != NULL) {
        if (drop_ctx->file_ctx != NULL)
            LogFileFreeCtx(drop_ctx->file_ctx);
        SCFree(drop_ctx);
    }
}

static void JsonDropLogDeInitCtx(OutputCtx *output_ctx)
{
    OutputDropLoggerDisable();

    JsonDropOutputCtx *drop_ctx = output_ctx->data;
    JsonDropOutputCtxFree(drop_ctx);
    SCFree(output_ctx);
}

static void JsonDropLogDeInitCtxSub(OutputCtx *output_ctx)
{
    OutputDropLoggerDisable();

    JsonDropOutputCtx *drop_ctx = output_ctx->data;
    SCFree(drop_ctx);
    SCLogDebug("cleaning up sub output_ctx %p", output_ctx);
    SCFree(output_ctx);
}

#define DEFAULT_LOG_FILENAME "drop.json"
static OutputInitResult JsonDropLogInitCtx(ConfNode *conf)
{
    OutputInitResult result = { NULL, false };
    if (OutputDropLoggerEnable() != 0) {
        SCLogError(SC_ERR_CONF_YAML_ERROR, "only one 'drop' logger "
            "can be enabled");
        return result;
    }

    JsonDropOutputCtx *drop_ctx = SCCalloc(1, sizeof(*drop_ctx));
    if (drop_ctx == NULL)
        return result;

    drop_ctx->file_ctx = LogFileNewCtx();
    if (drop_ctx->file_ctx == NULL) {
        JsonDropOutputCtxFree(drop_ctx);
        return result;
    }

    if (SCConfLogOpenGeneric(conf, drop_ctx->file_ctx, DEFAULT_LOG_FILENAME, 1) < 0) {
        JsonDropOutputCtxFree(drop_ctx);
        return result;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        JsonDropOutputCtxFree(drop_ctx);
        return result;
    }

    if (conf) {
        const char *extended = ConfNodeLookupChildValue(conf, "alerts");
        if (extended != NULL) {
            if (ConfValIsTrue(extended)) {
                drop_ctx->flags = LOG_DROP_ALERTS;
            }
        }
        extended = ConfNodeLookupChildValue(conf, "flows");
        if (extended != NULL) {
            if (strcasecmp(extended, "start") == 0) {
                g_droplog_flows_start = 1;
            } else if (strcasecmp(extended, "all") == 0) {
                g_droplog_flows_start = 0;
            } else {
                SCLogWarning(SC_ERR_CONF_YAML_ERROR, "valid options for "
                        "'flow' are 'start' and 'all'");
            }
        }
    }

    output_ctx->data = drop_ctx;
    output_ctx->DeInit = JsonDropLogDeInitCtx;

    result.ctx = output_ctx;
    result.ok = true;
    return result;
}

static OutputInitResult JsonDropLogInitCtxSub(ConfNode *conf, OutputCtx *parent_ctx)
{
    OutputInitResult result = { NULL, false };
    if (OutputDropLoggerEnable() != 0) {
        SCLogError(SC_ERR_CONF_YAML_ERROR, "only one 'drop' logger "
            "can be enabled");
        return result;
    }

    OutputJsonCtx *ajt = parent_ctx->data;

    JsonDropOutputCtx *drop_ctx = SCCalloc(1, sizeof(*drop_ctx));
    if (drop_ctx == NULL)
        return result;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        JsonDropOutputCtxFree(drop_ctx);
        return result;
    }

    if (conf) {
        const char *extended = ConfNodeLookupChildValue(conf, "alerts");
        if (extended != NULL) {
            if (ConfValIsTrue(extended)) {
                drop_ctx->flags = LOG_DROP_ALERTS;
            }
        }
        extended = ConfNodeLookupChildValue(conf, "flows");
        if (extended != NULL) {
            if (strcasecmp(extended, "start") == 0) {
                g_droplog_flows_start = 1;
            } else if (strcasecmp(extended, "all") == 0) {
                g_droplog_flows_start = 0;
            } else {
                SCLogWarning(SC_ERR_CONF_YAML_ERROR, "valid options for "
                        "'flow' are 'start' and 'all'");
            }
        }
    }

    drop_ctx->file_ctx = ajt->file_ctx;
    drop_ctx->cfg = ajt->cfg;

    output_ctx->data = drop_ctx;
    output_ctx->DeInit = JsonDropLogDeInitCtxSub;

    result.ctx = output_ctx;
    result.ok = true;
    return result;
}

/**
 * \brief   Log the dropped packets when engine is running in inline mode
 *
 * \param tv    Pointer the current thread variables
 * \param data  Pointer to the droplog struct
 * \param p     Pointer the packet which is being logged
 *
 * \retval 0 on succes
 */
static int JsonDropLogger(ThreadVars *tv, void *thread_data, const Packet *p)
{
    JsonDropLogThread *td = thread_data;
    int r = DropLogJSON(td, p);
    if (r < 0)
        return -1;

    if (!g_droplog_flows_start)
        return 0;

    if (p->flow) {
        if (p->flow->flags & FLOW_ACTION_DROP) {
            if (PKT_IS_TOSERVER(p) && !(p->flow->flags & FLOW_TOSERVER_DROP_LOGGED))
                p->flow->flags |= FLOW_TOSERVER_DROP_LOGGED;
            else if (PKT_IS_TOCLIENT(p) && !(p->flow->flags & FLOW_TOCLIENT_DROP_LOGGED))
                p->flow->flags |= FLOW_TOCLIENT_DROP_LOGGED;
        }
    }
    return 0;
}


/**
 * \brief Check if we need to drop-log this packet
 *
 * \param tv    Pointer the current thread variables
 * \param p     Pointer the packet which is tested
 *
 * \retval bool TRUE or FALSE
 */
static int JsonDropLogCondition(ThreadVars *tv, const Packet *p)
{
    if (!EngineModeIsIPS()) {
        SCLogDebug("engine is not running in inline mode, so returning");
        return FALSE;
    }
    if (PKT_IS_PSEUDOPKT(p)) {
        SCLogDebug("drop log doesn't log pseudo packets");
        return FALSE;
    }

    if (g_droplog_flows_start && p->flow != NULL) {
        int ret = FALSE;

        /* for a flow that will be dropped fully, log just once per direction */
        if (p->flow->flags & FLOW_ACTION_DROP) {
            if (PKT_IS_TOSERVER(p) && !(p->flow->flags & FLOW_TOSERVER_DROP_LOGGED))
                ret = TRUE;
            else if (PKT_IS_TOCLIENT(p) && !(p->flow->flags & FLOW_TOCLIENT_DROP_LOGGED))
                ret = TRUE;
        }

        /* if drop is caused by signature, log anyway */
        if (p->alerts.drop.action != 0)
            ret = TRUE;

        return ret;
    } else if (PACKET_TEST_ACTION(p, ACTION_DROP)) {
        return TRUE;
    }

    return FALSE;
}

void JsonDropLogRegister (void)
{
    OutputRegisterPacketModule(LOGGER_JSON_DROP, MODULE_NAME, "drop-json-log",
        JsonDropLogInitCtx, JsonDropLogger, JsonDropLogCondition,
        JsonDropLogThreadInit, JsonDropLogThreadDeinit, NULL);
    OutputRegisterPacketSubModule(LOGGER_JSON_DROP, "eve-log", MODULE_NAME,
        "eve-log.drop", JsonDropLogInitCtxSub, JsonDropLogger,
        JsonDropLogCondition, JsonDropLogThreadInit, JsonDropLogThreadDeinit,
        NULL);
}
