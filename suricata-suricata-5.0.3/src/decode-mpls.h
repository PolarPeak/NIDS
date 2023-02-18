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
 * \author Jason Ish <jason.ish@emulex.com>
 *
 * MPLS decoder.
 */

#ifndef __DECODE_MPLS_H__
#define __DECODE_MPLS_H__

#define ETHERNET_TYPE_MPLS_UNICAST   0x8847
#define ETHERNET_TYPE_MPLS_MULTICAST 0x8848

void DecodeMPLSRegisterTests(void);

#endif /* !__DECODE_MPLS_H__ */
