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

#ifndef __STREAM_TCP_UTIL_H__
#define __STREAM_TCP_UTIL_H__

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"

void StreamTcpUTInit(TcpReassemblyThreadCtx **);
void StreamTcpUTDeinit(TcpReassemblyThreadCtx *);

void StreamTcpUTInitInline(void);

void StreamTcpUTSetupSession(TcpSession *);
void StreamTcpUTClearSession(TcpSession *);

void StreamTcpUTSetupStream(TcpStream *, uint32_t isn);
void StreamTcpUTClearStream(TcpStream *);

int StreamTcpUTAddSegmentWithByte(ThreadVars *, TcpReassemblyThreadCtx *, TcpStream *, uint32_t, uint8_t, uint16_t);
int StreamTcpUTAddSegmentWithPayload(ThreadVars *, TcpReassemblyThreadCtx *, TcpStream *, uint32_t, uint8_t *, uint16_t);
int StreamTcpUTAddPayload(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx, TcpSession *ssn, TcpStream *stream, uint32_t seq, uint8_t *payload, uint16_t len);

void StreamTcpUtilRegisterTests(void);

#endif /* __STREAM_TCP_UTIL_H__ */

