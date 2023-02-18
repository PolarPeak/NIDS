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
 * \ingroup httplayer
 *
 * @{
 */


/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Implements support http_referer sticky buffer
 */

#define KEYWORD_NAME_LEGACY "http_referer"
#define KEYWORD_NAME "http.referer"
#define KEYWORD_DOC "http-keywords.html#http-referer"
#define BUFFER_NAME "http_referer"
#define BUFFER_DESC "http referer header"
#define HEADER_NAME "Referer"
#define KEYWORD_ID DETECT_AL_HTTP_HEADER_REFERER
#define KEYWORD_TOSERVER 1

#include "detect-http-headers-stub.h"
#include "detect-http-referer.h"

void RegisterHttpHeadersReferer(void)
{
    DetectHttpHeadersRegisterStub();
}
