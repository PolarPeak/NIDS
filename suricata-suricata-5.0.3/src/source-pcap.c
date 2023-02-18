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
 *
 * Live pcap packet acquisition support
 */

#include "suricata-common.h"
#include "suricata.h"
#include "decode.h"
#include "packet-queue.h"
#include "threads.h"
#include "threadvars.h"
#include "tm-queuehandlers.h"
#include "tm-threads.h"
#include "source-pcap.h"
#include "conf.h"
#include "util-debug.h"
#include "util-error.h"
#include "util-privs.h"
#include "util-device.h"
#include "util-optimize.h"
#include "util-checksum.h"
#include "util-ioctl.h"
#include "tmqh-packetpool.h"

#define PCAP_STATE_DOWN 0
#define PCAP_STATE_UP 1

#define PCAP_RECONNECT_TIMEOUT 500000

/**
 * \brief Structure to hold thread specific variables.
 */
typedef struct PcapThreadVars_
{
    /* thread specific handle */
    pcap_t *pcap_handle;
    /* handle state */
    unsigned char pcap_state;
    /* thread specific bpf */
    struct bpf_program filter;
    /* ptr to string from config */
    const char *bpf_filter;

    time_t last_stats_dump;

    /* data link type for the thread */
    int datalink;

    /* counters */
    uint32_t pkts;
    uint64_t bytes;
    uint32_t errs;

    uint16_t capture_kernel_packets;
    uint16_t capture_kernel_drops;
    uint16_t capture_kernel_ifdrops;

    ThreadVars *tv;
    TmSlot *slot;

    /** callback result -- set if one of the thread module failed. */
    int cb_result;

    /* pcap buffer size */
    int pcap_buffer_size;
    int pcap_snaplen;

    ChecksumValidationMode checksum_mode;

    LiveDevice *livedev;
} PcapThreadVars;

TmEcode ReceivePcapThreadInit(ThreadVars *, const void *, void **);
void ReceivePcapThreadExitStats(ThreadVars *, void *);
TmEcode ReceivePcapThreadDeinit(ThreadVars *, void *);
TmEcode ReceivePcapLoop(ThreadVars *tv, void *data, void *slot);
TmEcode ReceivePcapBreakLoop(ThreadVars *tv, void *data);

TmEcode DecodePcapThreadInit(ThreadVars *, const void *, void **);
TmEcode DecodePcapThreadDeinit(ThreadVars *tv, void *data);
TmEcode DecodePcap(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);

/** protect pcap_compile and pcap_setfilter, as they are not thread safe:
 *  http://seclists.org/tcpdump/2009/q1/62 */
static SCMutex pcap_bpf_compile_lock = SCMUTEX_INITIALIZER;

/**
 * \brief Registration Function for RecievePcap.
 */
void TmModuleReceivePcapRegister (void)
{
    tmm_modules[TMM_RECEIVEPCAP].name = "ReceivePcap";
    tmm_modules[TMM_RECEIVEPCAP].ThreadInit = ReceivePcapThreadInit;
    tmm_modules[TMM_RECEIVEPCAP].PktAcqLoop = ReceivePcapLoop;
    tmm_modules[TMM_RECEIVEPCAP].PktAcqBreakLoop = ReceivePcapBreakLoop;
    tmm_modules[TMM_RECEIVEPCAP].ThreadExitPrintStats = ReceivePcapThreadExitStats;
    tmm_modules[TMM_RECEIVEPCAP].cap_flags = SC_CAP_NET_RAW;
    tmm_modules[TMM_RECEIVEPCAP].flags = TM_FLAG_RECEIVE_TM;
}

/**
 * \brief Registration Function for DecodePcap.
 */
void TmModuleDecodePcapRegister (void)
{
    tmm_modules[TMM_DECODEPCAP].name = "DecodePcap";
    tmm_modules[TMM_DECODEPCAP].ThreadInit = DecodePcapThreadInit;
    tmm_modules[TMM_DECODEPCAP].Func = DecodePcap;
    tmm_modules[TMM_DECODEPCAP].ThreadDeinit = DecodePcapThreadDeinit;
    tmm_modules[TMM_DECODEPCAP].flags = TM_FLAG_DECODE_TM;
}

static inline void PcapDumpCounters(PcapThreadVars *ptv)
{
    struct pcap_stat pcap_s;
    if (likely((pcap_stats(ptv->pcap_handle, &pcap_s) >= 0))) {
        StatsSetUI64(ptv->tv, ptv->capture_kernel_packets, pcap_s.ps_recv);
        StatsSetUI64(ptv->tv, ptv->capture_kernel_drops, pcap_s.ps_drop);
        (void) SC_ATOMIC_SET(ptv->livedev->drop, pcap_s.ps_drop);
        StatsSetUI64(ptv->tv, ptv->capture_kernel_ifdrops, pcap_s.ps_ifdrop);
    }
}

static int PcapTryReopen(PcapThreadVars *ptv)
{
    ptv->pcap_state = PCAP_STATE_DOWN;

    int pcap_activate_r = pcap_activate(ptv->pcap_handle);
    if (pcap_activate_r != 0) {
        return pcap_activate_r;
    }

    /* set bpf filter if we have one */
    if (ptv->bpf_filter != NULL) {
        if (pcap_compile(ptv->pcap_handle, &ptv->filter,
                    (char *)ptv->bpf_filter, 1, 0) < 0)
        {
            SCLogError(SC_ERR_BPF, "bpf compilation error %s",
                    pcap_geterr(ptv->pcap_handle));
            return -1;
        }

        if (pcap_setfilter(ptv->pcap_handle, &ptv->filter) < 0) {
            SCLogError(SC_ERR_BPF, "could not set bpf filter %s",
                    pcap_geterr(ptv->pcap_handle));
            return -1;
        }
    }

    SCLogInfo("Recovering interface listening");
    ptv->pcap_state = PCAP_STATE_UP;
    return 0;
}

static void PcapCallbackLoop(char *user, struct pcap_pkthdr *h, u_char *pkt)
{
    SCEnter();

    PcapThreadVars *ptv = (PcapThreadVars *)user;
    Packet *p = PacketGetFromQueueOrAlloc();
    struct timeval current_time;

    if (unlikely(p == NULL)) {
        SCReturn;
    }

    PKT_SET_SRC(p, PKT_SRC_WIRE);
    p->ts.tv_sec = h->ts.tv_sec;
    p->ts.tv_usec = h->ts.tv_usec;
    SCLogDebug("p->ts.tv_sec %"PRIuMAX"", (uintmax_t)p->ts.tv_sec);
    p->datalink = ptv->datalink;

    ptv->pkts++;
    ptv->bytes += h->caplen;
    (void) SC_ATOMIC_ADD(ptv->livedev->pkts, 1);
    p->livedev = ptv->livedev;

    if (unlikely(PacketCopyData(p, pkt, h->caplen))) {
        TmqhOutputPacketpool(ptv->tv, p);
        SCReturn;
    }

    switch (ptv->checksum_mode) {
        case CHECKSUM_VALIDATION_AUTO:
            if (ptv->livedev->ignore_checksum) {
                p->flags |= PKT_IGNORE_CHECKSUM;
            } else if (ChecksumAutoModeCheck(ptv->pkts,
                        SC_ATOMIC_GET(ptv->livedev->pkts),
                        SC_ATOMIC_GET(ptv->livedev->invalid_checksums))) {
                ptv->livedev->ignore_checksum = 1;
                p->flags |= PKT_IGNORE_CHECKSUM;
            }
            break;
        case CHECKSUM_VALIDATION_DISABLE:
            p->flags |= PKT_IGNORE_CHECKSUM;
            break;
        default:
            break;
    }

    if (TmThreadsSlotProcessPkt(ptv->tv, ptv->slot, p) != TM_ECODE_OK) {
        pcap_breakloop(ptv->pcap_handle);
        ptv->cb_result = TM_ECODE_FAILED;
    }

    /* Trigger one dump of stats every second */
    TimeGet(&current_time);
    if (current_time.tv_sec != ptv->last_stats_dump) {
        PcapDumpCounters(ptv);
        ptv->last_stats_dump = current_time.tv_sec;
    }

    SCReturn;
}

#ifndef PCAP_ERROR_BREAK
#define PCAP_ERROR_BREAK -2
#endif

/**
 *  \brief Main PCAP reading Loop function
 */
TmEcode ReceivePcapLoop(ThreadVars *tv, void *data, void *slot)
{
    SCEnter();

    int packet_q_len = 64;
    PcapThreadVars *ptv = (PcapThreadVars *)data;
    TmSlot *s = (TmSlot *)slot;

    ptv->slot = s->slot_next;
    ptv->cb_result = TM_ECODE_OK;

    while (1) {
        if (suricata_ctl_flags & SURICATA_STOP) {
            SCReturnInt(TM_ECODE_OK);
        }

        /* make sure we have at least one packet in the packet pool, to prevent
         * us from alloc'ing packets at line rate */
        PacketPoolWait();

        int r = pcap_dispatch(ptv->pcap_handle, packet_q_len,
                          (pcap_handler)PcapCallbackLoop, (u_char *)ptv);
        if (unlikely(r == 0 || r == PCAP_ERROR_BREAK)) {
            if (r == PCAP_ERROR_BREAK && ptv->cb_result == TM_ECODE_FAILED) {
                SCReturnInt(TM_ECODE_FAILED);
            }
            TmThreadsCaptureHandleTimeout(tv, ptv->slot, NULL);
        } else if (unlikely(r < 0)) {
            int dbreak = 0;
            SCLogError(SC_ERR_PCAP_DISPATCH, "error code %" PRId32 " %s",
                       r, pcap_geterr(ptv->pcap_handle));
            do {
                usleep(PCAP_RECONNECT_TIMEOUT);
                if (suricata_ctl_flags != 0) {
                    dbreak = 1;
                    break;
                }
                r = PcapTryReopen(ptv);
            } while (r < 0);
            if (dbreak) {
                break;
            }
        } else if (ptv->cb_result == TM_ECODE_FAILED) {
            SCLogError(SC_ERR_PCAP_DISPATCH, "Pcap callback PcapCallbackLoop failed");
            SCReturnInt(TM_ECODE_FAILED);
        }

        StatsSyncCountersIfSignalled(tv);
    }

    PcapDumpCounters(ptv);
    StatsSyncCountersIfSignalled(tv);
    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief PCAP Break Loop function.
 */
TmEcode ReceivePcapBreakLoop(ThreadVars *tv, void *data)
{
    SCEnter();
    PcapThreadVars *ptv = (PcapThreadVars *)data;
    if (ptv->pcap_handle == NULL) {
        SCReturnInt(TM_ECODE_FAILED);
    }
    pcap_breakloop(ptv->pcap_handle);
    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Init function for ReceivePcap.
 *
 * This is a setup function for recieving packets
 * via libpcap. There are two versions of this function
 * depending on the major version of libpcap used.
 * For versions prior to 1.x we use open_pcap_live,
 * for versions 1.x and greater we use pcap_create + pcap_activate.
 *
 * \param tv pointer to ThreadVars
 * \param initdata pointer to the interface passed from the user
 * \param data pointer gets populated with PcapThreadVars
 *
 * \todo Create a general pcap setup function.
 */
TmEcode ReceivePcapThreadInit(ThreadVars *tv, const void *initdata, void **data)
{
    SCEnter();
    PcapIfaceConfig *pcapconfig = (PcapIfaceConfig *)initdata;

    if (initdata == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "initdata == NULL");
        SCReturnInt(TM_ECODE_FAILED);
    }

    PcapThreadVars *ptv = SCCalloc(1, sizeof(PcapThreadVars));
    if (unlikely(ptv == NULL)) {
        pcapconfig->DerefFunc(pcapconfig);
        SCReturnInt(TM_ECODE_FAILED);
    }

    ptv->tv = tv;

    ptv->livedev = LiveGetDevice(pcapconfig->iface);
    if (ptv->livedev == NULL) {
        SCLogError(SC_ERR_INVALID_VALUE, "unable to find Live device");
        SCFree(ptv);
        SCReturnInt(TM_ECODE_FAILED);
    }
    SCLogInfo("using interface %s", (char *)pcapconfig->iface);

    if (LiveGetOffload() == 0) {
        (void)GetIfaceOffloading((char *)pcapconfig->iface, 1, 1);
    } else {
        DisableIfaceOffloading(ptv->livedev, 1, 1);
    }

    ptv->checksum_mode = pcapconfig->checksum_mode;
    if (ptv->checksum_mode == CHECKSUM_VALIDATION_AUTO) {
        SCLogInfo("running in 'auto' checksum mode. Detection of interface "
                "state will require " xstr(CHECKSUM_SAMPLE_COUNT) " packets");
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    ptv->pcap_handle = pcap_create((char *)pcapconfig->iface, errbuf);
    if (ptv->pcap_handle == NULL) {
        if (strlen(errbuf)) {
            SCLogError(SC_ERR_PCAP_CREATE, "could not create a new "
                    "pcap handler for %s, error %s",
                    (char *)pcapconfig->iface, errbuf);
        } else {
            SCLogError(SC_ERR_PCAP_CREATE, "could not create a new "
                    "pcap handler for %s",
                    (char *)pcapconfig->iface);
        }
        SCFree(ptv);
        pcapconfig->DerefFunc(pcapconfig);
        SCReturnInt(TM_ECODE_FAILED);
    }

    if (pcapconfig->snaplen == 0) {
        /* We set snaplen if we can get the MTU */
        ptv->pcap_snaplen = GetIfaceMaxPacketSize(pcapconfig->iface);
    } else {
        ptv->pcap_snaplen = pcapconfig->snaplen;
    }
    if (ptv->pcap_snaplen > 0) {
        /* set Snaplen. Must be called before pcap_activate */
        int pcap_set_snaplen_r = pcap_set_snaplen(ptv->pcap_handle, ptv->pcap_snaplen);
        if (pcap_set_snaplen_r != 0) {
            SCLogError(SC_ERR_PCAP_SET_SNAPLEN, "could not set snaplen, "
                    "error: %s", pcap_geterr(ptv->pcap_handle));
            SCFree(ptv);
            pcapconfig->DerefFunc(pcapconfig);
            SCReturnInt(TM_ECODE_FAILED);
        }
        SCLogInfo("Set snaplen to %d for '%s'", ptv->pcap_snaplen,
                  pcapconfig->iface);
    }

    /* set Promisc, and Timeout. Must be called before pcap_activate */
    int pcap_set_promisc_r = pcap_set_promisc(ptv->pcap_handle, pcapconfig->promisc);
    if (pcap_set_promisc_r != 0) {
        SCLogError(SC_ERR_PCAP_SET_PROMISC, "could not set promisc mode, "
                "error %s", pcap_geterr(ptv->pcap_handle));
        SCFree(ptv);
        pcapconfig->DerefFunc(pcapconfig);
        SCReturnInt(TM_ECODE_FAILED);
    }

    int pcap_set_timeout_r = pcap_set_timeout(ptv->pcap_handle, LIBPCAP_COPYWAIT);
    if (pcap_set_timeout_r != 0) {
        SCLogError(SC_ERR_PCAP_SET_TIMEOUT, "could not set timeout, "
                "error %s", pcap_geterr(ptv->pcap_handle));
        SCFree(ptv);
        pcapconfig->DerefFunc(pcapconfig);
        SCReturnInt(TM_ECODE_FAILED);
    }
#ifdef HAVE_PCAP_SET_BUFF
    ptv->pcap_buffer_size = pcapconfig->buffer_size;
    if (ptv->pcap_buffer_size > 0) {
        SCLogInfo("going to use pcap buffer size of %" PRId32,
                ptv->pcap_buffer_size);

        int pcap_set_buffer_size_r = pcap_set_buffer_size(ptv->pcap_handle,
                ptv->pcap_buffer_size);
        if (pcap_set_buffer_size_r != 0) {
            SCLogError(SC_ERR_PCAP_SET_BUFF_SIZE, "could not set "
                    "pcap buffer size, error %s", pcap_geterr(ptv->pcap_handle));
            SCFree(ptv);
            pcapconfig->DerefFunc(pcapconfig);
            SCReturnInt(TM_ECODE_FAILED);
        }
    }
#endif /* HAVE_PCAP_SET_BUFF */

    /* activate the handle */
    int pcap_activate_r = pcap_activate(ptv->pcap_handle);
    if (pcap_activate_r != 0) {
        SCLogError(SC_ERR_PCAP_ACTIVATE_HANDLE, "could not activate the "
                "pcap handler, error %s", pcap_geterr(ptv->pcap_handle));
        SCFree(ptv);
        pcapconfig->DerefFunc(pcapconfig);
        SCReturnInt(TM_ECODE_FAILED);
    }
    ptv->pcap_state = PCAP_STATE_UP;

    /* set bpf filter if we have one */
    if (pcapconfig->bpf_filter) {
        SCMutexLock(&pcap_bpf_compile_lock);

        ptv->bpf_filter = pcapconfig->bpf_filter;

        if (pcap_compile(ptv->pcap_handle, &ptv->filter,
                    (char *)ptv->bpf_filter, 1, 0) < 0)
        {
            SCLogError(SC_ERR_BPF, "bpf compilation error %s",
                    pcap_geterr(ptv->pcap_handle));

            SCMutexUnlock(&pcap_bpf_compile_lock);
            SCFree(ptv);
            pcapconfig->DerefFunc(pcapconfig);
            return TM_ECODE_FAILED;
        }

        if (pcap_setfilter(ptv->pcap_handle, &ptv->filter) < 0) {
            SCLogError(SC_ERR_BPF, "could not set bpf filter %s",
                    pcap_geterr(ptv->pcap_handle));

            SCMutexUnlock(&pcap_bpf_compile_lock);
            SCFree(ptv);
            pcapconfig->DerefFunc(pcapconfig);
            return TM_ECODE_FAILED;
        }

        SCMutexUnlock(&pcap_bpf_compile_lock);
    }

    /* no offloading supported at all */
    (void)GetIfaceOffloading(pcapconfig->iface, 1, 1);

    ptv->datalink = pcap_datalink(ptv->pcap_handle);

    pcapconfig->DerefFunc(pcapconfig);

    ptv->capture_kernel_packets = StatsRegisterCounter("capture.kernel_packets",
            ptv->tv);
    ptv->capture_kernel_drops = StatsRegisterCounter("capture.kernel_drops",
            ptv->tv);
    ptv->capture_kernel_ifdrops = StatsRegisterCounter("capture.kernel_ifdrops",
            ptv->tv);

    *data = (void *)ptv;
    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief This function prints stats to the screen at exit.
 * \param tv pointer to ThreadVars
 * \param data pointer that gets cast into PcapThreadVars for ptv
 */
void ReceivePcapThreadExitStats(ThreadVars *tv, void *data)
{
    SCEnter();
    PcapThreadVars *ptv = (PcapThreadVars *)data;
    struct pcap_stat pcap_s;

    if (pcap_stats(ptv->pcap_handle, &pcap_s) < 0) {
        SCLogError(SC_ERR_STAT,"(%s) Failed to get pcap_stats: %s",
                tv->name, pcap_geterr(ptv->pcap_handle));
        SCLogInfo("(%s) Packets %" PRIu32 ", bytes %" PRIu64 "",
                tv->name, ptv->pkts, ptv->bytes);
    } else {
        SCLogInfo("(%s) Packets %" PRIu32 ", bytes %" PRIu64 "",
                tv->name, ptv->pkts, ptv->bytes);

        /* these numbers are not entirely accurate as ps_recv contains packets
         * that are still waiting to be processed at exit. ps_drop only contains
         * packets dropped by the driver and not any packets dropped by the interface.
         * Additionally see http://tracker.icir.org/bro/ticket/18
         *
         * Note: ps_recv includes dropped packets and should be considered total.
         * Unless we start to look at ps_ifdrop which isn't supported everywhere.
         */
        SCLogInfo("(%s) Pcap Total:%" PRIu64 " Recv:%" PRIu64 " Drop:%" PRIu64 " (%02.1f%%).",
                tv->name, (uint64_t)pcap_s.ps_recv,
                (uint64_t)pcap_s.ps_recv - (uint64_t)pcap_s.ps_drop,
                (uint64_t)pcap_s.ps_drop,
                (((float)(uint64_t)pcap_s.ps_drop)/(float)(uint64_t)pcap_s.ps_recv)*100);
    }
}

/**
 * \brief DeInit function closes pcap_handle at exit.
 * \param tv pointer to ThreadVars
 * \param data pointer that gets cast into PcapThreadVars for ptv
 */
TmEcode ReceivePcapThreadDeinit(ThreadVars *tv, void *data)
{
    PcapThreadVars *ptv = (PcapThreadVars *)data;

    pcap_close(ptv->pcap_handle);
    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief This function passes off to link type decoders.
 *
 * DecodePcap reads packets from the PacketQueue and passes
 * them off to the proper link type decoder.
 *
 * \param t pointer to ThreadVars
 * \param p pointer to the current packet
 * \param data pointer that gets cast into PcapThreadVars for ptv
 * \param pq pointer to the current PacketQueue
 */
TmEcode DecodePcap(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();
    DecodeThreadVars *dtv = (DecodeThreadVars *)data;

    /* XXX HACK: flow timeout can call us for injected pseudo packets
     *           see bug: https://redmine.openinfosecfoundation.org/issues/1107 */
    if (p->flags & PKT_PSEUDO_STREAM_END)
        return TM_ECODE_OK;

    /* update counters */
    DecodeUpdatePacketCounters(tv, dtv, p);

    /* call the decoder */
    switch(p->datalink) {
        case LINKTYPE_LINUX_SLL:
            DecodeSll(tv, dtv, p, GET_PKT_DATA(p), GET_PKT_LEN(p), pq);
            break;
        case LINKTYPE_ETHERNET:
            DecodeEthernet(tv, dtv, p,GET_PKT_DATA(p), GET_PKT_LEN(p), pq);
            break;
        case LINKTYPE_PPP:
            DecodePPP(tv, dtv, p, GET_PKT_DATA(p), GET_PKT_LEN(p), pq);
            break;
        case LINKTYPE_RAW:
        case LINKTYPE_GRE_OVER_IP:
            DecodeRaw(tv, dtv, p, GET_PKT_DATA(p), GET_PKT_LEN(p), pq);
            break;
        case LINKTYPE_NULL:
            DecodeNull(tv, dtv, p, GET_PKT_DATA(p), GET_PKT_LEN(p), pq);
            break;
        default:
            SCLogError(SC_ERR_DATALINK_UNIMPLEMENTED, "Error: datalink "
                    "type %" PRId32 " not yet supported in module "
                    "DecodePcap", p->datalink);
            break;
    }

    PacketDecodeFinalize(tv, dtv, p);

    SCReturnInt(TM_ECODE_OK);
}

TmEcode DecodePcapThreadInit(ThreadVars *tv, const void *initdata, void **data)
{
    SCEnter();

    DecodeThreadVars *dtv = DecodeThreadVarsAlloc(tv);
    if (dtv == NULL)
        SCReturnInt(TM_ECODE_FAILED);

    DecodeRegisterPerfCounters(dtv, tv);

    *data = (void *)dtv;

    SCReturnInt(TM_ECODE_OK);
}

TmEcode DecodePcapThreadDeinit(ThreadVars *tv, void *data)
{
    if (data != NULL)
        DecodeThreadVarsFree(tv, data);
    SCReturnInt(TM_ECODE_OK);
}

void PcapTranslateIPToDevice(char *pcap_dev, size_t len)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_if_t *alldevsp = NULL;

    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    int ret = 0;

    memset(&ai_hints, 0, sizeof(ai_hints));
    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_flags = AI_NUMERICHOST;

    /* try to translate IP */
    if ((ret = getaddrinfo(pcap_dev, NULL, &ai_hints, &ai_list)) != 0) {
        return;
    }

    if (pcap_findalldevs(&alldevsp, errbuf)) {
        freeaddrinfo(ai_list);
        return;
    }

    for (pcap_if_t *devsp = alldevsp; devsp ; devsp = devsp->next) {
        for (pcap_addr_t *ip = devsp->addresses; ip ; ip = ip->next) {

            if (ai_list->ai_family != ip->addr->sa_family) {
                continue;
            }

            if (ip->addr->sa_family == AF_INET) {
                if (memcmp(&((struct sockaddr_in*)ai_list->ai_addr)->sin_addr,
                            &((struct sockaddr_in*)ip->addr)->sin_addr,
                            sizeof(struct in_addr)))
                {
                    continue;
                }
            } else if (ip->addr->sa_family == AF_INET6) {
                if (memcmp(&((struct sockaddr_in6*)ai_list->ai_addr)->sin6_addr,
                            &((struct sockaddr_in6*)ip->addr)->sin6_addr,
                            sizeof(struct in6_addr)))
                {
                    continue;
                }
            } else {
                continue;
            }

            freeaddrinfo(ai_list);

            memset(pcap_dev, 0, len);
            strlcpy(pcap_dev, devsp->name, len);

            pcap_freealldevs(alldevsp);
            return;
        }
    }

    freeaddrinfo(ai_list);

    pcap_freealldevs(alldevsp);
}
