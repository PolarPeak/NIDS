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
 * \author Victor Julien <victor@inliniac.net>
 */

#ifndef __DETECT_HTTP_HEADER_COMMON_H__
#define __DETECT_HTTP_HEADER_COMMON_H__

typedef struct HttpHeaderBuffer_ {
    uint8_t *buffer;
    uint32_t size;      /**< buffer size */
    uint32_t len;       /**< part of buffer in use */
} HttpHeaderBuffer;

typedef struct HttpHeaderThreadConfig_ {
    uint16_t tx_step;
    uint16_t size_step;
} HttpHeaderThreadDataConfig;

typedef struct HttpHeaderThreadData_ {
    HttpHeaderBuffer *buffers;  /**< array of buffers */
    uint16_t buffers_size;      /**< number of buffers */
    uint16_t buffers_list_len;
    uint16_t size_step;         /**< increase size of HttpHeaderBuffer::buffer with this */
    uint16_t tx_step;           /**< increase number of txs with this */
    uint64_t start_tx_id;
    uint64_t tick;
} HttpHeaderThreadData;

void *HttpHeaderThreadDataInit(void *data);
void HttpHeaderThreadDataFree(void *data);

HttpHeaderBuffer *HttpHeaderGetBufferSpaceForTXID(DetectEngineThreadCtx *det_ctx,
        Flow *f, uint8_t flags, uint64_t tx_id, const int keyword_id,
        HttpHeaderThreadData **ret_hdr_td);

int HttpHeaderExpandBuffer(HttpHeaderThreadData *td,
        HttpHeaderBuffer *buf, uint32_t size);

#endif /* __DETECT_HTTP_HEADER_COMMON_H__ */
