/* Copyright (C) 2007-2017 Open Information Security Foundation
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
 * \author Pierre Chifflier <chifflier@edenwall.com>
 * \author Yoann Vandoorselaere <yoann.v@prelude-ids.com>
 *
 * Logs alerts to the Prelude system, using IDMEF (RFC 4765) messages.
 *
 * Each message contains the alert description and reference (using
 * the SID/GID), and a normalized description (assessment, impact,
 * sources etc.)
 *
 * libprelude handles the connection with the manager (collecting component),
 * spooling and sending the event asynchronously. It also offers transport
 * security (using TLS and trusted certificates) and reliability (events
 * are retransmitted if not sent successfully).
 *
 * This modules requires a Prelude profile to work (see man prelude-admin
 * and the Prelude Handbook for help).
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-unittest.h"
#include "util-time.h"
#include "util-debug.h"
#include "util-error.h"
#include "util-print.h"

#include "output.h"
#include "output-json.h"
#include "output-json-http.h"
#include "output-json-tls.h"
#include "output-json-ssh.h"
#include "output-json-smtp.h"
#include "output-json-email-common.h"

#include "util-privs.h"
#include "util-optimize.h"

#include "stream.h"

#include "alert-prelude.h"

#ifndef PRELUDE

/* Handle the case where no PRELUDE support is compiled in. */

void AlertPreludeRegister(void)
{
    SCLogDebug("Can't register Prelude output thread - Prelude support was disabled during build.");
}

#else /* implied we do have PRELUDE support */

#include <libprelude/prelude.h>

#define ANALYZER_CLASS "NIDS"
#define ANALYZER_MODEL "Suricata"
#define ANALYZER_MANUFACTURER "http://www.openinfosecfoundation.org/"
#define ANALYZER_SID_URL "http://www.snort.org/search/sid/"

#define SNORT_MAX_OWNED_SID 1000000
#define DEFAULT_ANALYZER_NAME "suricata"

#define DEFAULT_PRELUDE_PROFILE "suricata"

static unsigned int info_priority = 4;
static unsigned int low_priority  = 3;
static unsigned int mid_priority  = 2;

/**
 * This holds global structures and variables. Since libprelude is thread-safe,
 * there is no need to store a mutex.
 */
typedef struct AlertPreludeCtx_ {
    /** The client (which has the send function) */
    prelude_client_t *client;
    int log_packet_content;
    int log_packet_header;
} AlertPreludeCtx;

/**
 * This holds per-thread specific structures and variables.
 */
typedef struct AlertPreludeThread_ {
    /** Pointer to the global context */
    AlertPreludeCtx *ctx;
    idmef_analyzer_t *analyzer;
} AlertPreludeThread;


/**
 * \brief Initialize analyzer description
 *
 * \return 0 if ok
 */
static int SetupAnalyzer(idmef_analyzer_t *analyzer)
{
    int ret;
    prelude_string_t *string;

    SCEnter();

    ret = idmef_analyzer_new_model(analyzer, &string);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating analyzer model: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }
    ret = prelude_string_set_constant(string, ANALYZER_MODEL);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting analyzer model: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }

    ret = idmef_analyzer_new_class(analyzer, &string);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating analyzer class: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }
    ret = prelude_string_set_constant(string, ANALYZER_CLASS);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting analyzer class: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }

    ret = idmef_analyzer_new_manufacturer(analyzer, &string);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating analyzer manufacturer: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }
    ret = prelude_string_set_constant(string, ANALYZER_MANUFACTURER);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting analyzer manufacturer: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }

    ret = idmef_analyzer_new_version(analyzer, &string);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating analyzer version: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }
    ret = prelude_string_set_constant(string, VERSION);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting analyzer version: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }

    SCReturnInt(0);
}

/**
 * \brief Create event impact description (see section
 * 4.2.6.1 of RFC 4765).
 * The impact contains the severity, completion (succeeded or failed)
 * and basic classification of the attack type.
 * Here, we don't set the completion since we don't know it (default
 * is unknown).
 *
 * \return 0 if ok
 */
static int EventToImpact(const PacketAlert *pa, const Packet *p, idmef_alert_t *alert)
{
    int ret;
    prelude_string_t *str;
    idmef_impact_t *impact;
    idmef_assessment_t *assessment;
    idmef_impact_severity_t severity;

    SCEnter();

    ret = idmef_alert_new_assessment(alert, &assessment);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating assessment: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }

    ret = idmef_assessment_new_impact(assessment, &impact);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating assessment impact: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(ret);
    }

    if ( (unsigned int)pa->s->prio < mid_priority )
        severity = IDMEF_IMPACT_SEVERITY_HIGH;

    else if ( (unsigned int)pa->s->prio < low_priority )
        severity = IDMEF_IMPACT_SEVERITY_MEDIUM;

    else if ( (unsigned int)pa->s->prio < info_priority )
        severity = IDMEF_IMPACT_SEVERITY_LOW;

    else
        severity = IDMEF_IMPACT_SEVERITY_INFO;

    idmef_impact_set_severity(impact, severity);

    if (PACKET_TEST_ACTION(p, ACTION_DROP) ||
        PACKET_TEST_ACTION(p, ACTION_REJECT) ||
        PACKET_TEST_ACTION(p, ACTION_REJECT_DST) ||
        PACKET_TEST_ACTION(p, ACTION_REJECT_BOTH) ) {
        idmef_action_t *action;

        ret = idmef_action_new(&action);
        if (unlikely(ret < 0))
            SCReturnInt(ret);

        idmef_action_set_category(action, IDMEF_ACTION_CATEGORY_BLOCK_INSTALLED);
        idmef_assessment_set_action(assessment, action, 0);
    }

    if (pa->s->msg) {
        ret = idmef_impact_new_description(impact, &str);
        if (unlikely(ret < 0))
            SCReturnInt(ret);

        prelude_string_set_ref(str, pa->s->msg);
    }

    SCReturnInt(0);
}

/**
 * \brief Add Source and Target fields to the IDMEF alert.
 * These objects contains IP addresses, source and destination
 * ports (see sections 4.2.4.3 and 4.2.4.4 of RFC 4765).
 *
 * \return 0 if ok
 */
static int EventToSourceTarget(const PacketAlert *pa, const Packet *p,
                               idmef_alert_t *alert)
{
    int ret;
    idmef_node_t *node;
    idmef_source_t *source;
    idmef_target_t *target;
    idmef_address_t *address;
    idmef_service_t *service;
    prelude_string_t *string;
    static char saddr[128], daddr[128];
    uint8_t ip_vers;
    uint8_t ip_proto;
    uint16_t sp, dp;
    uint8_t invert = 0;

    SCEnter();

    if ( !p )
        SCReturnInt(0);

    if ( ! IPH_IS_VALID(p) )
        SCReturnInt(0);

    if (pa->s->flags & SIG_FLAG_HAS_TARGET) {
        if (pa->s->flags & SIG_FLAG_SRC_IS_TARGET) {
            invert = 1;
        } else {
            invert = 0;
        }
    } else {
        invert = 0;
    }

    if (PKT_IS_IPV4(p)) {
        ip_vers = 4;
        ip_proto = IPV4_GET_RAW_IPPROTO(p->ip4h);
        if (invert) {
            PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), saddr, sizeof(saddr));
            PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), daddr, sizeof(daddr));
            sp = p->dp;
            dp = p->sp;
        } else {
            PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), saddr, sizeof(saddr));
            PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), daddr, sizeof(daddr));
            sp = p->sp;
            dp = p->dp;

        }
    } else if (PKT_IS_IPV6(p)) {
        ip_vers = 6;
        ip_proto = IPV6_GET_L4PROTO(p);
        if (invert) {
            PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), saddr, sizeof(saddr));
            PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), daddr, sizeof(daddr));
            sp = p->dp;
            dp = p->sp;
        } else {
            PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), saddr, sizeof(saddr));
            PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), daddr, sizeof(daddr));
            sp = p->sp;
            dp = p->dp;
        }
    } else
        SCReturnInt(0);

    ret = idmef_alert_new_source(alert, &source, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_source_new_service(source, &service);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    if ( p->tcph || p->udph )
        idmef_service_set_port(service, sp);

    idmef_service_set_ip_version(service, ip_vers);
    idmef_service_set_iana_protocol_number(service, ip_proto);

    ret = idmef_source_new_node(source, &node);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_node_new_address(node, &address, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_address_new_address(address, &string);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    prelude_string_set_ref(string, saddr);

    ret = idmef_alert_new_target(alert, &target, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_target_new_service(target, &service);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    if ( p->tcph || p->udph )
        idmef_service_set_port(service, dp);

    idmef_service_set_ip_version(service, ip_vers);
    idmef_service_set_iana_protocol_number(service, ip_proto);

    ret = idmef_target_new_node(target, &node);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_node_new_address(node, &address, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_address_new_address(address, &string);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    prelude_string_set_ref(string, daddr);

    SCReturnInt(0);
}

/**
 * \brief Add binary data, to be stored in the Additional Data
 * field of the IDMEF alert (see section 4.2.4.6 of RFC 4765).
 *
 * \return 0 if ok
 */
static int AddByteData(idmef_alert_t *alert, const char *meaning, const unsigned char *data, size_t size)
{
    int ret;
    prelude_string_t *str;
    idmef_additional_data_t *ad;

    SCEnter();

    if ( ! data || ! size )
        SCReturnInt(0);

    ret = idmef_alert_new_additional_data(alert, &ad, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(0);

    ret = idmef_additional_data_set_byte_string_ref(ad, data, size);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting byte string data: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    ret = idmef_additional_data_new_meaning(ad, &str);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating additional-data meaning: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    ret = prelude_string_set_ref(str, meaning);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting byte string data meaning: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    SCReturnInt(0);
}

/**
 * \brief Add integer data, to be stored in the Additional Data
 * field of the IDMEF alert (see section 4.2.4.6 of RFC 4765).
 *
 * \return 0 if ok
 */
static int AddIntData(idmef_alert_t *alert, const char *meaning, uint32_t data)
{
    int ret;
    prelude_string_t *str;
    idmef_additional_data_t *ad;

    SCEnter();

    ret = idmef_alert_new_additional_data(alert, &ad, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    idmef_additional_data_set_integer(ad, data);

    ret = idmef_additional_data_new_meaning(ad, &str);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating additional-data meaning: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    ret = prelude_string_set_ref(str, meaning);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting integer data meaning: %s.",
                prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    SCReturnInt(0);
}

/**
 * \brief Add string data, to be stored in the Additional Data
 * field of the IDMEF alert (see section 4.2.4.6 of RFC 4765).
 * \param alert IDMEF alert where to add additional data
 * \param meaning Name of the value to add to IDMEF alert
 * \param data String to add to IDMEF alert
 * \return 0 if ok, else < 0
 */
static int AddStringData(idmef_alert_t *alert, const char *meaning, const char *data)
{
    int ret;
    idmef_additional_data_t *ad;
    prelude_string_t * p_str;

    SCEnter();

    ret = idmef_alert_new_additional_data(alert, &ad, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_additional_data_new_meaning(ad, &p_str);
    if (ret < 0) {
        idmef_additional_data_destroy(ad);
        SCReturnInt(ret);
    }

    ret = prelude_string_ncat(p_str, meaning, strlen(meaning));
    if (ret < 0) {
        idmef_additional_data_destroy(ad);
        SCReturnInt(ret);
    }

    ret = prelude_string_new(&p_str);
    if (ret < 0) {
        idmef_additional_data_destroy(ad);
        SCReturnInt(ret);
    }

    ret = prelude_string_ncat(p_str, data, strlen(data));
    if (ret < 0) {
        prelude_string_destroy(p_str);
        idmef_additional_data_destroy(ad);
        SCReturnInt(ret);
    }

    ret = idmef_additional_data_set_string_dup_fast(ad, prelude_string_get_string(p_str), prelude_string_get_len(p_str));

    prelude_string_destroy(p_str);

    if (ret < 0) {
        idmef_additional_data_destroy(ad);
        SCReturnInt(ret);
    }
    SCReturnInt(0);
}

/**
 * \brief Add real data, to be stored in the Additional Data
 * field of the IDMEF alert (see section 4.2.4.6 of RFC 4765).
 * \param alert IDMEF alert where to add additional data
 * \param meaning Name of the value to add to IDMEF alert
 * \param data Real to add to IDMEF alert
 * \return 0 if ok
 */
static int AddRealData(idmef_alert_t *alert, const char *meaning, uint32_t data)
{
    int ret;
    prelude_string_t *str;
    idmef_additional_data_t *ad;

    SCEnter();

    ret = idmef_alert_new_additional_data(alert, &ad, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    idmef_additional_data_set_real(ad, data);

    ret = idmef_additional_data_new_meaning(ad, &str);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error creating additional-data meaning: %s.",
                        prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    ret = prelude_string_set_ref(str, meaning);
    if (unlikely(ret < 0)) {
        SCLogDebug("%s: error setting integer data meaning: %s.",
                        prelude_strsource(ret), prelude_strerror(ret));
        SCReturnInt(-1);
    }

    SCReturnInt(0);
}

/**
 * \brief Add IPv4 header data, to be stored in the Additional Data
 * field of the IDMEF alert (see section 4.2.4.6 of RFC 4765).
 *
 * \return 0 if ok
 */
static int PacketToDataV4(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    SCEnter();

    AddIntData(alert, "ip_ver", IPV4_GET_RAW_VER(p->ip4h));
    AddIntData(alert, "ip_hlen", IPV4_GET_RAW_HLEN(p->ip4h));
    AddIntData(alert, "ip_tos", IPV4_GET_RAW_IPTOS(p->ip4h));
    AddIntData(alert, "ip_len", SCNtohs(IPV4_GET_RAW_IPLEN(p->ip4h)));

    AddIntData(alert, "ip_id", SCNtohs(IPV4_GET_RAW_IPID(p->ip4h)));

    AddIntData(alert, "ip_off", SCNtohs(IPV4_GET_RAW_IPOFFSET(p->ip4h)));

    AddIntData(alert, "ip_ttl", IPV4_GET_RAW_IPTTL(p->ip4h));
    AddIntData(alert, "ip_proto", IPV4_GET_RAW_IPPROTO(p->ip4h));

    AddIntData(alert, "ip_sum", SCNtohs(p->ip4h->ip_csum));

    SCReturnInt(0);
}

/**
 * \brief Add IPv6 header data, to be stored in the Additional Data
 * field of the IDMEF alert (see section 4.2.4.6 of RFC 4765).
 *
 * \return 0 if ok
 */
static int PacketToDataV6(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    SCEnter();

    AddIntData(alert, "ip_ver", IPV6_GET_VER(p));
    AddIntData(alert, "ip_class", IPV6_GET_CLASS(p));
    AddIntData(alert, "ip_flow", IPV6_GET_FLOW(p));
    AddIntData(alert, "ip_nh", IPV6_GET_NH(p));
    AddIntData(alert, "ip_plen", IPV6_GET_PLEN(p));
    AddIntData(alert, "ip_hlim", IPV6_GET_HLIM(p));
    AddIntData(alert, "ip_proto", IPV6_GET_L4PROTO(p));

    SCReturnInt(0);
}

/**
 * \brief Convert JSON object to Prelude additional data with
 * the right type of data. Browse the JSON object to get
 * the key=value information.
 * \param key Name of the JSON value
 * \param value JSON object to add to the IDMEF alert
 * \param alert IDMEF alert
 * \return 0 if ok
 */
static int JsonToAdditionalData(const char * key, json_t * value, idmef_alert_t *alert)
{
    SCEnter();

    int ret = 0;
    const char *key_js;
    char local_key[128];
    json_t *value_js;
    size_t index;

    if (!json_is_object(value) && key == NULL)
        SCReturnInt(-1);

    if (json_is_object(value)) {
        json_object_foreach(value, key_js, value_js) {
            if (key != NULL) {
                snprintf(local_key, sizeof(local_key), "%s_%s", key, key_js);
            } else {
                snprintf(local_key, sizeof(local_key), "%s", key_js);
            }
            ret = JsonToAdditionalData(local_key, value_js, alert);
        }
    } else if (json_is_array(value)) {
        json_array_foreach(value, index, value_js) {
            ret = snprintf(local_key, sizeof(local_key), "%s_%ju", key, (uintmax_t)index);
            if (ret < 0 || (size_t)ret >= sizeof(local_key)) {
                SCLogError(SC_ERR_SPRINTF,"failed to construct key");
                continue;
            }
            ret = JsonToAdditionalData(local_key, value_js, alert);
        }
    } else if (json_is_integer(value)) {
        ret = AddIntData(alert, key, json_integer_value(value));
    } else if (json_is_real(value)) {
        ret = AddRealData(alert, key, json_real_value(value));
    } else if (json_is_boolean(value)) {
        ret = AddIntData(alert, key, json_is_true(value));
    } else if (json_is_string(value)) {
        ret = AddStringData(alert, key, json_string_value(value));
    } else {
        ret = AddStringData(alert, key, json_dumps(value, 0));
    }
    SCReturnInt(ret);
}

/**
 * \brief Handle ALPROTO_HTTP JSON information
 * \param p Packet where to extract data
 * \param pa Packet alert information
 * \param alert IDMEF alert
 * \return void
 */
static void PacketToDataProtoHTTP(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    json_t *js;

    js = JsonHttpAddMetadata(p->flow, pa->tx_id);
    if (js == NULL)
        return;

    JsonToAdditionalData(NULL, js, alert);

    json_decref(js);

}

/**
 * \brief Handle ALPROTO_TLS JSON information
 * \param p Packet where to extract data
 * \param pa Packet alert information
 * \param alert IDMEF alert
 * \return void
 */
static void PacketToDataProtoTLS(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    json_t *js;
    SSLState *ssl_state = (SSLState *)FlowGetAppState(p->flow);

    if (ssl_state == NULL)
        return;

    js = json_object();
    if (js == NULL)
        return;

    JsonTlsLogJSONBasic(js, ssl_state);
    JsonTlsLogJSONExtended(js, ssl_state);
    JsonToAdditionalData(NULL, js, alert);

    json_decref(js);

}

/**
 * \brief Handle ALPROTO_SSH JSON information
 * \param p Packet where to extract data
 * \param pa Packet alert information
 * \param alert IDMEF alert
 * \return void
 */
static void PacketToDataProtoSSH(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    json_t *js, *s_js;
    SshState *ssh_state = (SshState *)FlowGetAppState(p->flow);

    if (ssh_state == NULL)
        return;

    js = json_object();
    if (js == NULL)
        return;

    JsonSshLogJSON(js, ssh_state);

    s_js = json_object_get(js, "server");
    if (s_js != NULL) {
        JsonToAdditionalData(NULL, s_js, alert);
    }

    s_js = json_object_get(js, "client");
    if (s_js != NULL) {
        JsonToAdditionalData(NULL, s_js, alert);
    }

    json_decref(js);

}

/**
 * \brief Handle ALPROTO_SMTP JSON information
 * \param p Packet where to extract data
 * \param pa Packet alert information
 * \param alert IDMEF alert
 * \return void
 */
static void PacketToDataProtoSMTP(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    json_t *js;

    js = JsonSMTPAddMetadata(p->flow, pa->tx_id);
    if (js == NULL)
        return;

    JsonToAdditionalData(NULL, js, alert);

    json_decref(js);

}

/**
 * \brief Handle ALPROTO_(SMTP|IMAP) Email JSON information
 * \param p Packet where to extract data
 * \param pa Packet alert information
 * \param alert IDMEF alert
 * \return void
 */
static void PacketToDataProtoEmail(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert)
{
    json_t *js;

    js = JsonEmailAddMetadata(p->flow, pa->tx_id);
    if (js == NULL)
        return;

    JsonToAdditionalData(NULL, js, alert);

    json_decref(js);

}

/**
 * \brief Convert IP packet to an IDMEF alert (RFC 4765).
 * This function stores the alert SID (description and reference),
 * the payload of the packet, and pre-processed data.
 *
 * \return 0 if ok
 */
static int PacketToData(const Packet *p, const PacketAlert *pa, idmef_alert_t *alert, AlertPreludeCtx *ctx)
{
    SCEnter();

    if (unlikely(p == NULL))
        SCReturnInt(0);

    if (p->flow != NULL) {
        uint16_t proto = FlowGetAppProtocol(p->flow);
        switch (proto) {
            case ALPROTO_HTTP:
                PacketToDataProtoHTTP(p, pa, alert);
                break;
            case ALPROTO_TLS:
                PacketToDataProtoTLS(p, pa, alert);
                break;
            case ALPROTO_SSH:
                PacketToDataProtoSSH(p, pa, alert);
                break;
            case ALPROTO_SMTP:
            case ALPROTO_IMAP:
                PacketToDataProtoSMTP(p, pa, alert);
                PacketToDataProtoEmail(p, pa, alert);
                break;
        }
    }

    AddIntData(alert, "snort_rule_sid", pa->s->id);
    AddIntData(alert, "snort_rule_rev", pa->s->rev);

    if (ctx->log_packet_header) {
        if ( PKT_IS_IPV4(p) )
            PacketToDataV4(p, pa, alert);

        else if ( PKT_IS_IPV6(p) )
            PacketToDataV6(p, pa, alert);

        if ( PKT_IS_TCP(p) ) {
            AddIntData(alert, "tcp_seq", TCP_GET_SEQ(p));
            AddIntData(alert, "tcp_ack", TCP_GET_ACK(p));

            AddIntData(alert, "tcp_off", TCP_GET_OFFSET(p));
            AddIntData(alert, "tcp_res", TCP_GET_X2(p));
            AddIntData(alert, "tcp_flags", TCP_GET_FLAGS(p));

            AddIntData(alert, "tcp_win", TCP_GET_WINDOW(p));
            AddIntData(alert, "tcp_sum", TCP_GET_SUM(p));
            AddIntData(alert, "tcp_urp", TCP_GET_URG_POINTER(p));
            if (p->tcpvars.ts_val != 0) {
                AddIntData(alert, "tcp_tsval", TCP_GET_TSVAL(p));
            }
            if (p->tcpvars.ts_ecr != 0) {
                AddIntData(alert, "tcp_tsecr", TCP_GET_TSECR(p));
            }
            if (p->tcph != NULL) {
                AddIntData(alert, "tcp_wscale", TCP_GET_WSCALE(p));
            }
            if (TCP_HAS_SACKOK(p)) {
                AddIntData(alert, "tcp_sackok", TCP_GET_SACKOK(p));
            }
            if (TCP_HAS_SACK(p)) {
                AddIntData(alert, "tcp_sack_cnt", TCP_GET_SACK_CNT(p));
            }
            AddIntData(alert, "tcp_hlen", TCP_GET_HLEN(p));
        }

        else if ( PKT_IS_UDP(p) ) {
            AddIntData(alert, "udp_len", UDP_GET_LEN(p));
            AddIntData(alert, "udp_sum", UDP_GET_SUM(p));
        }

        else if ( PKT_IS_ICMPV4(p) ) {
            AddIntData(alert, "icmp_type", ICMPV4_GET_TYPE(p));
            AddIntData(alert, "icmp_code", ICMPV4_GET_CODE(p));
            AddIntData(alert, "icmp_sum", ICMPV4_GET_RAW_CSUM(p));

        }

        else if ( PKT_IS_ICMPV6(p) ) {
            AddIntData(alert, "icmp_type", ICMPV6_GET_TYPE(p));
            AddIntData(alert, "icmp_code", ICMPV6_GET_CODE(p));
            AddIntData(alert, "icmp_csum", ICMPV6_GET_RAW_CSUM(p));
        }
    }

    if (ctx->log_packet_content)
        AddByteData(alert, "payload", p->payload, p->payload_len);

    SCReturnInt(0);
}

/**
 * \brief Store reference on rule (SID and GID) in the IDMEF alert,
 * and embed an URL pointing to the rule description.
 *
 * \return 0 if ok
 */
static int AddSnortReference(idmef_classification_t *class, int gen_id, int sig_id)
{
    int ret;
    prelude_string_t *str;
    idmef_reference_t *ref;

    SCEnter();

    if ( sig_id >= SNORT_MAX_OWNED_SID )
        SCReturnInt(0);

    ret = idmef_classification_new_reference(class, &ref, IDMEF_LIST_APPEND);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_reference_new_name(ref, &str);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    idmef_reference_set_origin(ref, IDMEF_REFERENCE_ORIGIN_VENDOR_SPECIFIC);

    if ( gen_id == 0 )
        ret = prelude_string_sprintf(str, "%u", sig_id);
    else
        ret = prelude_string_sprintf(str, "%u:%u", gen_id, sig_id);

    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_reference_new_meaning(ref, &str);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = prelude_string_sprintf(str, "Snort Signature ID");
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = idmef_reference_new_url(ref, &str);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    if ( gen_id == 0 )
        ret = prelude_string_sprintf(str, ANALYZER_SID_URL "%u", sig_id);
    else
        ret = prelude_string_sprintf(str, ANALYZER_SID_URL "%u-%u", gen_id, sig_id);

    SCReturnInt(ret);
}

/**
 * \brief Create event classification description (see section
 * 4.2.4.2 of RFC 4765).
 * The classification is the "name" of the alert, identification of the
 * rule signature, and additional information on the rule.
 *
 * \return 0 if ok
 */
static int EventToReference(const PacketAlert *pa, const Packet *p, idmef_classification_t *class)
{
    int ret;
    prelude_string_t *str;

    SCEnter();

    ret = idmef_classification_new_ident(class, &str);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    if ( pa->s->gid == 0 )
        ret = prelude_string_sprintf(str, "%u", pa->s->id);
    else
        ret = prelude_string_sprintf(str, "%u:%u", pa->s->gid, pa->s->id);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    ret = AddSnortReference(class, pa->s->gid, pa->s->id);
    if (unlikely(ret < 0))
        SCReturnInt(ret);

    SCReturnInt(0);
}

static int PreludePrintStreamSegmentCallback(const Packet *p, void *data, const uint8_t *buf, uint32_t buflen)
{
    int ret;

    ret = AddByteData((idmef_alert_t *)data, "stream-segment", buf, buflen);
    if (ret == 0)
        return 1;
    else
        return -1;
}

/**
 * \brief Initialize thread-specific data. Each thread structure contains
 * a pointer to the \a AlertPreludeCtx context.
 *
 * \return TM_ECODE_OK if ok, else TM_ECODE_FAILED
 */
static TmEcode AlertPreludeThreadInit(ThreadVars *t, const void *initdata, void **data)
{
    int ret;
    AlertPreludeThread *aun;

    SCEnter();

    if (unlikely(initdata == NULL)) {
        SCLogError(SC_ERR_INITIALIZATION,
                   "Error getting context for Prelude.  \"initdata\" argument NULL");
        SCReturnInt(TM_ECODE_FAILED);
    }

    aun = SCMalloc(sizeof(AlertPreludeThread));
    if (unlikely(aun == NULL))
        SCReturnInt(TM_ECODE_FAILED);
    memset(aun, 0, sizeof(AlertPreludeThread));

    /* Use the Ouput Context */
    aun->ctx = ((OutputCtx *)initdata)->data;

    /* Create a per-thread idmef analyzer */
    ret = idmef_analyzer_clone(prelude_client_get_analyzer(aun->ctx->client), &aun->analyzer);
    if (unlikely(ret < 0)) {
        SCLogError(SC_ERR_INITIALIZATION,
                   "Error creating idmef analyzer for Prelude.");

        SCFree(aun);
        SCReturnInt(TM_ECODE_FAILED);
    }

    *data = (void *)aun;
    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Free thread-specific data.
 *
 * \return TM_ECODE_OK if ok, else TM_ECODE_FAILED
 */
static TmEcode AlertPreludeThreadDeinit(ThreadVars *t, void *data)
{
    AlertPreludeThread *aun = (AlertPreludeThread *)data;

    SCEnter();

    if (unlikely(aun == NULL)) {
        SCLogDebug("AlertPreludeThreadDeinit done (error)");
        SCReturnInt(TM_ECODE_FAILED);
    }

    /* clear memory */
    idmef_analyzer_destroy(aun->analyzer);
    memset(aun, 0, sizeof(AlertPreludeThread));
    SCFree(aun);

    SCReturnInt(TM_ECODE_OK);
}

static void AlertPreludeDeinitCtx(OutputCtx *output_ctx)
{
    AlertPreludeCtx *ctx = (AlertPreludeCtx *)output_ctx->data;

    prelude_client_destroy(ctx->client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
    SCFree(output_ctx);
}

/** \brief Initialize the Prelude logging module: initialize
 * library, create the client and try to establish the connection
 * to the Prelude Manager.
 * Client flags are set to force asynchronous (non-blocking) mode for
 * both alerts and heartbeats.
 * This function requires an existing Prelude profile to work.
 *
 * \return A newly allocated AlertPreludeCtx structure, or NULL
 */
static OutputInitResult AlertPreludeInitCtx(ConfNode *conf)
{
    int ret;
    prelude_client_t *client;
    AlertPreludeCtx *ctx;
    const char *prelude_profile_name;
    const char *log_packet_content;
    const char *log_packet_header;
    OutputInitResult result = { NULL, false };
    OutputCtx *output_ctx;

    SCEnter();

    ret = prelude_init(0, NULL);
    if (unlikely(ret < 0)) {
        prelude_perror(ret, "unable to initialize the prelude library");
        SCReturnCT(result, "OutputInitResult");
    }

    prelude_profile_name = ConfNodeLookupChildValue(conf, "profile");
    if (prelude_profile_name == NULL)
        prelude_profile_name = DEFAULT_PRELUDE_PROFILE;

    log_packet_content = ConfNodeLookupChildValue(conf, "log-packet-content");
    log_packet_header = ConfNodeLookupChildValue(conf, "log-packet-header");

    ret = prelude_client_new(&client, prelude_profile_name);
    if ( unlikely(ret < 0 || client == NULL )) {
        prelude_perror(ret, "Unable to create a prelude client object");
        prelude_client_destroy(client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
        SCReturnCT(result, "OutputInitResult");
    }

    ret = prelude_client_set_flags(client, prelude_client_get_flags(client) | PRELUDE_CLIENT_FLAGS_ASYNC_TIMER|PRELUDE_CLIENT_FLAGS_ASYNC_SEND);
    if (unlikely(ret < 0)) {
        SCLogDebug("Unable to set asynchronous send and timer.");
        prelude_client_destroy(client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
        SCReturnCT(result, "OutputInitResult");
    }

    ret = SetupAnalyzer(prelude_client_get_analyzer(client));
    if (ret < 0) {
        SCLogDebug("Unable to setup prelude client analyzer.");
        prelude_client_destroy(client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
        SCReturnCT(result, "OutputInitResult");
    }

    ret = prelude_client_start(client);
    if (unlikely(ret < 0)) {
        prelude_perror(ret, "Unable to start prelude client");
        prelude_client_destroy(client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
        SCReturnCT(result, "OutputInitResult");
    }

    ctx = SCMalloc(sizeof(AlertPreludeCtx));
    if (unlikely(ctx == NULL)) {
        prelude_perror(ret, "Unable to allocate memory");
        prelude_client_destroy(client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
        SCReturnCT(result, "OutputInitResult");
    }

    ctx->client = client;
    ctx->log_packet_content = 0;
    ctx->log_packet_header = 1;
    if (log_packet_content && ConfValIsTrue(log_packet_content))
        ctx->log_packet_content = 1;
    if (log_packet_header && ConfValIsFalse(log_packet_header))
        ctx->log_packet_header = 0;

    output_ctx = SCMalloc(sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        SCFree(ctx);
        prelude_perror(ret, "Unable to allocate memory");
        prelude_client_destroy(client, PRELUDE_CLIENT_EXIT_STATUS_SUCCESS);
        SCReturnCT(result, "OutputInitResult");
    }

    output_ctx->data = ctx;
    output_ctx->DeInit = AlertPreludeDeinitCtx;

    result.ctx = output_ctx;
    result.ok = true;
    SCReturnCT(result, "OutputInitResult");
}

static int AlertPreludeCondition(ThreadVars *tv, const Packet *p)
{
    if (p->alerts.cnt == 0)
        return FALSE;
    if (!IPH_IS_VALID(p))
        return FALSE;
    return TRUE;
}

/**
 * \brief Handle Suricata alert: convert it to and IDMEF alert (see RFC 4765)
 * and send it asynchronously (so, this function does not block and returns
 * immediately).
 * If the destination Prelude Manager is not available, the alert is spooled
 * (and the function also returns immediately).
 * An IDMEF object is created, and all available information is added: IP packet
 * header and data, rule signature ID, additional data like URL pointing to
 * rule description, CVE, etc.
 * The IDMEF alert has a reference to all created objects, so freeing it will
 * automatically free all allocated memory.
 *
 * \note This function is thread safe.
 *
 * \return TM_ECODE_OK if ok, else TM_ECODE_FAILED
 */
static int AlertPreludeLogger(ThreadVars *tv, void *thread_data, const Packet *p)
{
    AlertPreludeThread *apn = (AlertPreludeThread *)thread_data;
    int ret;
    idmef_time_t *time;
    idmef_alert_t *alert;
    prelude_string_t *str;
    idmef_message_t *idmef = NULL;
    idmef_classification_t *class;
    const PacketAlert *pa;

    SCEnter();

    if (unlikely(apn == NULL || apn->ctx == NULL)) {
        SCReturnInt(TM_ECODE_FAILED);
    }

    if (p->alerts.cnt == 0)
        SCReturnInt(TM_ECODE_OK);

    if ( !IPH_IS_VALID(p) )
        SCReturnInt(TM_ECODE_OK);

    /* XXX which one to add to this alert? Lets see how Snort solves this.
     * For now just take last alert. */
    pa = &p->alerts.alerts[p->alerts.cnt-1];
    if (unlikely(pa->s == NULL))
        goto err;

    ret = idmef_message_new(&idmef);
    if (unlikely(ret < 0))
        SCReturnInt(TM_ECODE_FAILED);

    ret = idmef_message_new_alert(idmef, &alert);
    if (unlikely(ret < 0))
        goto err;

    ret = idmef_alert_new_classification(alert, &class);
    if (unlikely(ret < 0))
        goto err;

    if (pa->s->class_msg) {
        ret = idmef_classification_new_text(class, &str);
        if (unlikely(ret < 0))
            goto err;

        prelude_string_set_ref(str, pa->s->class_msg);
    }

    ret = EventToImpact(pa, p, alert);
    if (unlikely(ret < 0))
        goto err;

    ret = EventToReference(pa, p, class);
    if (unlikely(ret < 0))
        goto err;

    ret = EventToSourceTarget(pa, p, alert);
    if (unlikely(ret < 0))
        goto err;

    ret = PacketToData(p, pa, alert, apn->ctx);
    if (unlikely(ret < 0))
        goto err;

    if (PKT_IS_TCP(p) && (pa->flags & PACKET_ALERT_FLAG_STATE_MATCH)) {
        uint8_t flag;
        if (p->flowflags & FLOW_PKT_TOSERVER) {
            flag = FLOW_PKT_TOCLIENT;
        } else {
            flag = FLOW_PKT_TOSERVER;
        }
        ret = StreamSegmentForEach(p, flag,
                                   PreludePrintStreamSegmentCallback,
                                   (void *)alert);
    }
    if (unlikely(ret < 0))
        goto err;

    ret = idmef_alert_new_detect_time(alert, &time);
    if (unlikely(ret < 0))
        goto err;
    idmef_time_set_from_timeval(time, &p->ts);

    ret = idmef_time_new_from_gettimeofday(&time);
    if (unlikely(ret < 0))
        goto err;
    idmef_alert_set_create_time(alert, time);

    idmef_alert_set_analyzer(alert, idmef_analyzer_ref(apn->analyzer), IDMEF_LIST_PREPEND);

    /* finally, send event */
    prelude_client_send_idmef(apn->ctx->client, idmef);
    idmef_message_destroy(idmef);

    SCReturnInt(TM_ECODE_OK);

err:
    if (idmef != NULL)
        idmef_message_destroy(idmef);
    SCReturnInt(TM_ECODE_FAILED);
}

void AlertPreludeRegister (void)
{
    OutputRegisterPacketModule(LOGGER_PRELUDE, "AlertPrelude", "alert-prelude",
        AlertPreludeInitCtx, AlertPreludeLogger, AlertPreludeCondition,
        AlertPreludeThreadInit, AlertPreludeThreadDeinit, NULL);
}
#endif /* PRELUDE */
