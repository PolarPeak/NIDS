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
 * \author Jeff Lucovsky <jeff@lucovsky.org>
 *
 * Logs anomalies in JSON format.
 *
 */

#ifndef __OUTPUT_JSON_ANOMALY_H__
#define __OUTPUT_JSON_ANOMALY_H__

void JsonAnomalyLogRegister(void);
void AnomalyJsonHeader(void *ctx, const Packet *p, const PacketAlert *pa, json_t *js,
                     uint16_t flags);

#endif /* __OUTPUT_JSON_ALERT_H__ */

