/* Copyright (C) 2017 Open Information Security Foundation
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

#ifndef __OUTPUT_JSON_NFS_H__
#define __OUTPUT_JSON_NFS_H__

void JsonNFSLogRegister(void);
json_t *JsonNFSAddMetadataRPC(const Flow *f, uint64_t tx_id);
json_t *JsonNFSAddMetadata(const Flow *f, uint64_t tx_id);
#endif /* __OUTPUT_JSON_NFS_H__ */
