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
 * \author Pierre Chifflier <chifflier@wzdftpd.net>
 */

#ifndef __APP_LAYER_REGISTER_H__
#define __APP_LAYER_REGISTER_H__

typedef struct AppLayerParser {
    const char *name;
    const char *default_port;
    int ip_proto;

    ProbingParserFPtr ProbeTS;
    ProbingParserFPtr ProbeTC;

    uint16_t min_depth;
    uint16_t max_depth;

    void *(*StateAlloc)(void);
    void (*StateFree)(void *);

    AppLayerParserFPtr ParseTS;
    AppLayerParserFPtr ParseTC;

    uint64_t (*StateGetTxCnt)(void *alstate);
    void *(*StateGetTx)(void *alstate, uint64_t tx_id);
    void (*StateTransactionFree)(void *, uint64_t);

    int (*StateGetProgressCompletionStatus)(uint8_t direction);
    int (*StateGetProgress)(void *alstate, uint8_t direction);

    uint32_t (*StateGetTxLogged)(void *alstate, void *tx);
    void (*StateSetTxLogged)(void *alstate, void *tx, uint32_t logger);

    DetectEngineState *(*GetTxDetectState)(void *tx);
    int (*SetTxDetectState)(void *tx, DetectEngineState *);

    AppLayerDecoderEvents *(*StateGetEvents)(void *);
    int (*StateGetEventInfo)(const char *event_name,
                             int *event_id, AppLayerEventType *event_type);
    int (*StateGetEventInfoById)(int event_id, const char **event_name,
                                  AppLayerEventType *event_type);

    void *(*LocalStorageAlloc)(void);
    void (*LocalStorageFree)(void *);

    uint64_t (*GetTxMpmIDs)(void *tx);
    int (*SetTxMpmIDs)(void *tx, uint64_t);

    FileContainer *(*StateGetFiles)(void *, uint8_t);

    AppLayerGetTxIterTuple (*GetTxIterator)(const uint8_t ipproto,
            const AppProto alproto, void *alstate, uint64_t min_tx_id,
            uint64_t max_tx_id, AppLayerGetTxIterState *istate);

    void (*SetTxDetectFlags)(void *, uint8_t, uint64_t);
    uint64_t (*GetTxDetectFlags)(void *, uint8_t);
} AppLayerParser;

/**
 * \brief App layer protocol detection function.
 *
 * \param parser The parser declaration structure.
 * \param enable_default A boolean to indicate if default port configuration should be used if none given
 *
 * \retval The AppProto constant if successful. On error, this function never returns.
 */
AppProto AppLayerRegisterProtocolDetection(const struct AppLayerParser *parser, int enable_default);

/**
 * \brief App layer protocol registration function.
 *
 * \param parser The parser declaration structure.
 * \param alproto The application layer protocol identifier.
 *
 * \retval 0 if successful. On error, this function never returns.
 */
int AppLayerRegisterParser(const struct AppLayerParser *p, AppProto alproto);

#endif /* __APP_LAYER_REGISTER_H__ */
