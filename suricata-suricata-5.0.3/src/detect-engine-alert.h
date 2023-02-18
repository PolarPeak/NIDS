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
 */

#ifndef __DETECT_ENGINE_ALERT_H__
#define __DETECT_ENGINE_ALERT_H__

#include "suricata-common.h"
#include "decode.h"
#include "detect.h"

void PacketAlertFinalize(DetectEngineCtx *, DetectEngineThreadCtx *, Packet *);
int PacketAlertAppend(DetectEngineThreadCtx *, const Signature *,
        Packet *, uint64_t tx_id, uint8_t);
int PacketAlertCheck(Packet *, uint32_t);
int PacketAlertRemove(Packet *, uint16_t);
void PacketAlertTagInit(void);
PacketAlert *PacketAlertGetTag(void);

#endif /* __DETECT_ENGINE_ALERT_H__ */
