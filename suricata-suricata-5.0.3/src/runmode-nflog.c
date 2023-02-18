/* Copyright (C) 2014 Open Information Security Foundation
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
 * \author Giuseppe Longo <giuseppelng@gmail.com>
 */
#include "suricata-common.h"
#include "config.h"
#include "tm-threads.h"
#include "conf.h"
#include "runmodes.h"
#include "runmode-nflog.h"

#include "util-debug.h"
#include "util-device.h"
#include "util-runmodes.h"
#include "util-misc.h"

#include "source-nflog.h"

const char *RunModeIdsNflogGetDefaultMode(void)
{
    return "autofp";
}

void RunModeIdsNflogRegister(void)
{
    RunModeRegisterNewRunMode(RUNMODE_NFLOG, "autofp",
                              "Multi threaded nflog mode",
                              RunModeIdsNflogAutoFp);
    RunModeRegisterNewRunMode(RUNMODE_NFLOG, "single",
                              "Single threaded nflog mode",
                              RunModeIdsNflogSingle);
    RunModeRegisterNewRunMode(RUNMODE_NFLOG, "workers",
                              "Workers nflog mode",
                              RunModeIdsNflogWorkers);
    return;
}

#ifdef HAVE_NFLOG
static void NflogDerefConfig(void *data)
{
    NflogGroupConfig *nflogconf = (NflogGroupConfig *)data;
    SCFree(nflogconf);
}

static void *ParseNflogConfig(const char *group)
{
    ConfNode *group_root;
    ConfNode *group_default = NULL;
    ConfNode *nflog_node;
    NflogGroupConfig *nflogconf = SCMalloc(sizeof(*nflogconf));
    intmax_t bufsize;
    intmax_t bufsize_max;
    intmax_t qthreshold;
    intmax_t qtimeout;
    int boolval;

    if (unlikely(nflogconf == NULL))
        return NULL;

    if (group == NULL) {
        SCFree(nflogconf);
        return NULL;
    }

    nflogconf->DerefFunc = NflogDerefConfig;
    nflog_node = ConfGetNode("nflog");

    if (nflog_node == NULL) {
        SCLogInfo("Unable to find nflog config using default value");
        return nflogconf;
    }

    group_root = ConfNodeLookupKeyValue(nflog_node, "group", group);

    group_default = ConfNodeLookupKeyValue(nflog_node, "group", "default");

    if (group_root == NULL && group_default == NULL) {
        SCLogInfo("Unable to find nflog config for "
                  "group \"%s\" or \"default\", using default value",
                  group);
        return nflogconf;
    }

    nflogconf->nful_overrun_warned = 0;
    strlcpy(nflogconf->numgroup, group, sizeof(nflogconf->numgroup));

    if (ParseSizeStringU16(group, &nflogconf->group) < 0) {
        SCLogError(SC_ERR_NFLOG_GROUP, "NFLOG's group number invalid.");
        exit(EXIT_FAILURE);
    }

    boolval = ConfGetChildValueIntWithDefault(group_root, group_default,
                                              "buffer-size", &bufsize);

    if (boolval)
        nflogconf->nlbufsiz = bufsize;
    else {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid buffer-size value");
        SCFree(nflogconf);
        return NULL;
    }

    boolval = ConfGetChildValueIntWithDefault(group_root, group_default,
                                              "max-size", &bufsize_max);

    if (boolval)
        nflogconf->nlbufsiz_max = bufsize_max;
    else {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid max-size value");
        SCFree(nflogconf);
        return NULL;
    }

    if (nflogconf->nlbufsiz > nflogconf->nlbufsiz_max) {
        SCLogWarning(SC_ERR_INVALID_ARGUMENT, "buffer-size value larger "
                "than max-size value, adjusting buffer-size");
        nflogconf->nlbufsiz = nflogconf->nlbufsiz_max;
    }

    boolval = ConfGetChildValueIntWithDefault(group_root, group_default,
                                              "qthreshold", &qthreshold);

    if (boolval)
        nflogconf->qthreshold = qthreshold;
    else {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid qthreshold value");
        SCFree(nflogconf);
        return NULL;
    }

    boolval = ConfGetChildValueIntWithDefault(group_root, group_default,
                                              "qtimeout", &qtimeout);

    if (boolval)
        nflogconf->qtimeout = qtimeout;
    else {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid qtimeout value");
        SCFree(nflogconf);
        return NULL;
    }

    return nflogconf;
}

static int NflogConfigGeThreadsCount(void *conf)
{
    /* for each nflog group there is no reason to use more than 1 thread */
    return 1;
}
#endif

int RunModeIdsNflogAutoFp(void)
{
    SCEnter();

#ifdef HAVE_NFLOG
    int ret = 0;
    char *live_dev = NULL;

    RunModeInitialize();
    TimeModeSetLive();

    ret = RunModeSetLiveCaptureAutoFp(ParseNflogConfig,
                                      NflogConfigGeThreadsCount,
                                      "ReceiveNFLOG",
                                      "DecodeNFLOG",
                                      thread_name_autofp,
                                      live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsNflogAutoFp initialised");
#endif /* HAVE_NFLOG */

    SCReturnInt(0);
}

int RunModeIdsNflogSingle(void)
{
    SCEnter();

#ifdef HAVE_NFLOG
    int ret = 0;
    char *live_dev = NULL;

    RunModeInitialize();
    TimeModeSetLive();

    ret = RunModeSetLiveCaptureSingle(ParseNflogConfig,
                                      NflogConfigGeThreadsCount,
                                      "ReceiveNFLOG",
                                      "DecodeNFLOG",
                                      thread_name_single,
                                      live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsNflogSingle initialised");
#endif /* HAVE_NFLOG */

    SCReturnInt(0);
}

int RunModeIdsNflogWorkers(void)
{
    SCEnter();

#ifdef HAVE_NFLOG
    int ret = 0;
    char *live_dev = NULL;

    RunModeInitialize();
    TimeModeSetLive();

    ret = RunModeSetLiveCaptureWorkers(ParseNflogConfig,
                                       NflogConfigGeThreadsCount,
                                       "ReceiveNFLOG",
                                       "DecodeNFLOG",
                                       thread_name_workers,
                                       live_dev);
    if (ret != 0) {
        SCLogError(SC_ERR_RUNMODE, "Unable to start runmode");
        exit(EXIT_FAILURE);
    }

    SCLogInfo("RunModeIdsNflogWorkers initialised");
#endif /* HAVE_NFLOG */

    SCReturnInt(0);
}
