/* Copyright (C) 2018 Open Information Security Foundation
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
 * \author Eric Leblond <eric@regit.org>
 *
 * LiveDevice wrapper around storage api
 */

#ifndef __DEVICE_STORAGE_H__
#define __DEVICE_STORAGE_H__

#include "util-storage.h"
#include "util-device.h"

unsigned int LiveDevStorageSize(void);

void *LiveDevGetStorageById(LiveDevice *d, int id);
int LiveDevSetStorageById(LiveDevice *d, int id, void *ptr);
void *LiveDevAllocStorageById(LiveDevice *d, int id);

void LiveDevFreeStorageById(LiveDevice *d, int id);
void LiveDevFreeStorage(LiveDevice *d);

void RegisterLiveDevStorageTests(void);

int LiveDevStorageRegister(const char *name, const unsigned int size, void *(*Alloc)(unsigned int), void (*Free)(void *));

#endif /* __DEVICE_STORAGE_H__ */
