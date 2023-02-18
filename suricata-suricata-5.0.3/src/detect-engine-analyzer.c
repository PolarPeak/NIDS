/* Copyright (C) 2007-2018 Open Information Security Foundation
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
 * \author Eileen Donlon <emdonlo@gmail.com>
 * \author Victor Julien <victor@inliniac.net>
 *
 * Rule analyzers for the detection engine
 */

#include "suricata-common.h"
#include "suricata.h"
#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-analyzer.h"
#include "detect-engine-mpm.h"
#include "conf.h"
#include "detect-content.h"
#include "detect-flow.h"
#include "detect-tcp-flags.h"
#include "util-print.h"

static int rule_warnings_only = 0;
static FILE *rule_engine_analysis_FD = NULL;
static FILE *fp_engine_analysis_FD = NULL;
static pcre *percent_re = NULL;
static pcre_extra *percent_re_study = NULL;
static char log_path[PATH_MAX];

typedef struct FpPatternStats_ {
    uint16_t min;
    uint16_t max;
    uint32_t cnt;
    uint64_t tot;
} FpPatternStats;

/* Details for each buffer being tracked */
typedef struct DetectEngineAnalyzerItems {
    int16_t     item_id;
    bool        item_seen;
    bool        export_item_seen;
    bool        check_encoding_match;
    const char  *item_name;
    const char  *display_name;
} DetectEngineAnalyzerItems;

/* Track which items require the item_seen value to be exposed */
struct ExposedItemSeen {
    const char  *bufname;
    bool        *item_seen_ptr;
};

DetectEngineAnalyzerItems analyzer_items[] = {
    /* request keywords */
    { 0, false, false, true,  "http_uri",           "http uri" },
    { 0, false, false, false, "http_raw_uri",       "http raw uri" },
    { 0, false, true,  false, "http_method",        "http method" },
    { 0, false, false, false, "http_request_line",  "http request line" },
    { 0, false, false, false, "http_client_body",   "http client body" },
    { 0, false, false, true,  "http_header",        "http header" },
    { 0, false, false, false, "http_raw_header",    "http raw header" },
    { 0, false, false, true,  "http_cookie",        "http cookie" },
    { 0, false, false, false, "http_user_agent",    "http user agent" },
    { 0, false, false, false, "http_host",          "http host" },
    { 0, false, false, false, "http_raw_host",      "http raw host" },
    { 0, false, false, false, "http_accept_enc",    "http accept enc" },
    { 0, false, false, false, "http_referer",       "http referer" },
    { 0, false, false, false, "http_content_type",  "http content type" },
    { 0, false, false, false, "http_header_names",  "http header names" },

    /* response keywords not listed above */
    { 0, false, false, false, "http_stat_msg",      "http stat msg" },
    { 0, false, false, false, "http_stat_code",     "http stat code" },
    { 0, false, true,  false, "file_data",          "http server body"},

    /* missing request keywords */
    { 0, false, false, false, "http_request_line",  "http request line" },
    { 0, false, false, false, "http_accept",        "http accept" },
    { 0, false, false, false, "http_accept_lang",   "http accept lang" },
    { 0, false, false, false, "http_connection",    "http connection" },
    { 0, false, false, false, "http_content_len",   "http content len" },
    { 0, false, false, false, "http_protocol",      "http protocol" },
    { 0, false, false, false, "http_start",         "http start" },

    /* missing response keywords; some of the missing are listed above*/
    { 0, false, false, false, "http_response_line", "http response line" },
    { 0, false, false, false, "http.server",        "http server" },
    { 0, false, false, false, "http.location",      "http location" },
};

/*
 * This array contains the map between the `analyzer_items` array listed above and
 * the item ids returned by DetectBufferTypeGetByName. Iterating signature's sigmatch
 * array provides list_ids. The map converts those ids into elements of the
 * analyzer items array.
 *
 * Ultimately, the g_buffer_type_hash is searched for each buffer name. The size of that
 * hashlist is 256, so that's the value we use here.
 */
int16_t analyzer_item_map[256];

/*
 * Certain values must be directly accessible. This array contains items that are directly
 * accessed when checking if they've been seen or not.
 */
struct ExposedItemSeen exposed_item_seen_list[] = {
    { .bufname = "http_method"},
    { .bufname = "file_data"}
};

static FpPatternStats fp_pattern_stats[DETECT_SM_LIST_MAX];

static void FpPatternStatsAdd(int list, uint16_t patlen)
{
    if (list < 0 || list >= DETECT_SM_LIST_MAX)
        return;

    FpPatternStats *f = &fp_pattern_stats[list];

    if (f->min == 0)
        f->min = patlen;
    else if (patlen < f->min)
        f->min = patlen;

    if (patlen > f->max)
        f->max = patlen;

    f->cnt++;
    f->tot += patlen;
}

void EngineAnalysisFP(const DetectEngineCtx *de_ctx, const Signature *s, char *line)
{
    int fast_pattern_set = 0;
    int fast_pattern_only_set = 0;
    int fast_pattern_chop_set = 0;
    DetectContentData *fp_cd = NULL;
    SigMatch *mpm_sm = s->init_data->mpm_sm;

    if (mpm_sm != NULL) {
        fp_cd = (DetectContentData *)mpm_sm->ctx;
        if (fp_cd->flags & DETECT_CONTENT_FAST_PATTERN) {
            fast_pattern_set = 1;
            if (fp_cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) {
                fast_pattern_only_set = 1;
            } else if (fp_cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) {
                fast_pattern_chop_set = 1;
            }
        }
    }

    fprintf(fp_engine_analysis_FD, "== Sid: %u ==\n", s->id);
    fprintf(fp_engine_analysis_FD, "%s\n", line);

    fprintf(fp_engine_analysis_FD, "    Fast Pattern analysis:\n");
    if (s->init_data->prefilter_sm != NULL) {
        fprintf(fp_engine_analysis_FD, "        Prefilter on: %s\n",
                sigmatch_table[s->init_data->prefilter_sm->type].name);
        fprintf(fp_engine_analysis_FD, "\n");
        return;
    }

    if (fp_cd == NULL) {
        fprintf(fp_engine_analysis_FD, "        No content present\n");
        fprintf(fp_engine_analysis_FD, "\n");
        return;
    }

    fprintf(fp_engine_analysis_FD, "        Fast pattern matcher: ");
    int list_type = SigMatchListSMBelongsTo(s, mpm_sm);
    if (list_type == DETECT_SM_LIST_PMATCH)
        fprintf(fp_engine_analysis_FD, "content\n");
    else {
        const char *desc = DetectBufferTypeGetDescriptionById(de_ctx, list_type);
        const char *name = DetectBufferTypeGetNameById(de_ctx, list_type);
        if (desc && name) {
            fprintf(fp_engine_analysis_FD, "%s (%s)\n", desc, name);
        }
    }

    int flags_set = 0;
    fprintf(fp_engine_analysis_FD, "        Flags:");
    if (fp_cd->flags & DETECT_CONTENT_OFFSET) {
        fprintf(fp_engine_analysis_FD, " Offset");
        flags_set = 1;
    } if (fp_cd->flags & DETECT_CONTENT_DEPTH) {
        fprintf(fp_engine_analysis_FD, " Depth");
        flags_set = 1;
    }
    if (fp_cd->flags & DETECT_CONTENT_WITHIN) {
        fprintf(fp_engine_analysis_FD, " Within");
        flags_set = 1;
    }
    if (fp_cd->flags & DETECT_CONTENT_DISTANCE) {
        fprintf(fp_engine_analysis_FD, " Distance");
        flags_set = 1;
    }
    if (fp_cd->flags & DETECT_CONTENT_NOCASE) {
        fprintf(fp_engine_analysis_FD, " Nocase");
        flags_set = 1;
    }
    if (fp_cd->flags & DETECT_CONTENT_NEGATED) {
        fprintf(fp_engine_analysis_FD, " Negated");
        flags_set = 1;
    }
    if (flags_set == 0)
        fprintf(fp_engine_analysis_FD, " None");
    fprintf(fp_engine_analysis_FD, "\n");

    fprintf(fp_engine_analysis_FD, "        Fast pattern set: %s\n", fast_pattern_set ? "yes" : "no");
    fprintf(fp_engine_analysis_FD, "        Fast pattern only set: %s\n",
            fast_pattern_only_set ? "yes" : "no");
    fprintf(fp_engine_analysis_FD, "        Fast pattern chop set: %s\n",
            fast_pattern_chop_set ? "yes" : "no");
    if (fast_pattern_chop_set) {
        fprintf(fp_engine_analysis_FD, "        Fast pattern offset, length: %u, %u\n",
                fp_cd->fp_chop_offset, fp_cd->fp_chop_len);
    }

    uint16_t patlen = fp_cd->content_len;
    uint8_t *pat = SCMalloc(fp_cd->content_len + 1);
    if (unlikely(pat == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memcpy(pat, fp_cd->content, fp_cd->content_len);
    pat[fp_cd->content_len] = '\0';
    fprintf(fp_engine_analysis_FD, "        Original content: ");
    PrintRawUriFp(fp_engine_analysis_FD, pat, patlen);
    fprintf(fp_engine_analysis_FD, "\n");

    if (fast_pattern_chop_set) {
        SCFree(pat);
        patlen = fp_cd->fp_chop_len;
        pat = SCMalloc(fp_cd->fp_chop_len + 1);
        if (unlikely(pat == NULL)) {
            exit(EXIT_FAILURE);
        }
        memcpy(pat, fp_cd->content + fp_cd->fp_chop_offset, fp_cd->fp_chop_len);
        pat[fp_cd->fp_chop_len] = '\0';
        fprintf(fp_engine_analysis_FD, "        Final content: ");
        PrintRawUriFp(fp_engine_analysis_FD, pat, patlen);
        fprintf(fp_engine_analysis_FD, "\n");

        FpPatternStatsAdd(list_type, patlen);
    } else {
        fprintf(fp_engine_analysis_FD, "        Final content: ");
        PrintRawUriFp(fp_engine_analysis_FD, pat, patlen);
        fprintf(fp_engine_analysis_FD, "\n");

        FpPatternStatsAdd(list_type, patlen);
    }
    SCFree(pat);

    fprintf(fp_engine_analysis_FD, "\n");
    return;
}

/**
 * \brief Sets up the fast pattern analyzer according to the config.
 *
 * \retval 1 If rule analyzer successfully enabled.
 * \retval 0 If not enabled.
 */
int SetupFPAnalyzer(void)
{
    int fp_engine_analysis_set = 0;

    if ((ConfGetBool("engine-analysis.rules-fast-pattern",
                     &fp_engine_analysis_set)) == 0) {
        return 0;
    }

    if (fp_engine_analysis_set == 0)
        return 0;

    const char *log_dir;
    log_dir = ConfigGetLogDirectory();
    snprintf(log_path, sizeof(log_path), "%s/%s", log_dir,
             "rules_fast_pattern.txt");

    fp_engine_analysis_FD = fopen(log_path, "w");
    if (fp_engine_analysis_FD == NULL) {
        SCLogError(SC_ERR_FOPEN, "failed to open %s: %s", log_path,
                   strerror(errno));
        return 0;
    }

    SCLogInfo("Engine-Analysis for fast_pattern printed to file - %s",
              log_path);

    struct timeval tval;
    struct tm *tms;
    gettimeofday(&tval, NULL);
    struct tm local_tm;
    tms = SCLocalTime(tval.tv_sec, &local_tm);
    fprintf(fp_engine_analysis_FD, "----------------------------------------------"
            "---------------------\n");
    fprintf(fp_engine_analysis_FD, "Date: %" PRId32 "/%" PRId32 "/%04d -- "
            "%02d:%02d:%02d\n",
            tms->tm_mday, tms->tm_mon + 1, tms->tm_year + 1900, tms->tm_hour,
            tms->tm_min, tms->tm_sec);
    fprintf(fp_engine_analysis_FD, "----------------------------------------------"
            "---------------------\n");

    memset(&fp_pattern_stats, 0, sizeof(fp_pattern_stats));
    return 1;
}

/**
 * \brief Sets up the rule analyzer according to the config
 * \retval 1 if rule analyzer successfully enabled
 * \retval 0 if not enabled
 */
int SetupRuleAnalyzer(void)
{
    ConfNode *conf = ConfGetNode("engine-analysis");
    int enabled = 0;
    if (conf != NULL) {
        const char *value = ConfNodeLookupChildValue(conf, "rules");
        if (value && ConfValIsTrue(value)) {
            enabled = 1;
        } else if (value && strcasecmp(value, "warnings-only") == 0) {
            enabled = 1;
            rule_warnings_only = 1;
        }
        if (enabled) {
            const char *log_dir;
            log_dir = ConfigGetLogDirectory();
            snprintf(log_path, sizeof(log_path), "%s/%s", log_dir, "rules_analysis.txt");
            rule_engine_analysis_FD = fopen(log_path, "w");
            if (rule_engine_analysis_FD == NULL) {
                SCLogError(SC_ERR_FOPEN, "failed to open %s: %s", log_path, strerror(errno));
                return 0;
            }

            SCLogInfo("Engine-Analysis for rules printed to file - %s",
                      log_path);

            struct timeval tval;
            struct tm *tms;
            gettimeofday(&tval, NULL);
            struct tm local_tm;
            tms = SCLocalTime(tval.tv_sec, &local_tm);
            fprintf(rule_engine_analysis_FD, "----------------------------------------------"
                    "---------------------\n");
            fprintf(rule_engine_analysis_FD, "Date: %" PRId32 "/%" PRId32 "/%04d -- "
                    "%02d:%02d:%02d\n",
                    tms->tm_mday, tms->tm_mon + 1, tms->tm_year + 1900, tms->tm_hour,
                    tms->tm_min, tms->tm_sec);
            fprintf(rule_engine_analysis_FD, "----------------------------------------------"
                    "---------------------\n");

            /*compile regex's for rule analysis*/
            if (PerCentEncodingSetup()== 0) {
                fprintf(rule_engine_analysis_FD, "Error compiling regex; can't check for percent encoding in normalized http content.\n");
            }
        }
    }
    else {
        SCLogInfo("Conf parameter \"engine-analysis.rules\" not found. "
                                      "Defaulting to not printing the rules analysis report.");
    }
    if (!enabled) {
        SCLogInfo("Engine-Analysis for rules disabled in conf file.");
        return 0;
    }
    return 1;
}

void CleanupFPAnalyzer(void)
{
    fprintf(fp_engine_analysis_FD, "============\n"
        "Summary:\n============\n");
    int i;
    for (i = 0; i < DETECT_SM_LIST_MAX; i++) {
        FpPatternStats *f = &fp_pattern_stats[i];
        if (f->cnt == 0)
            continue;

        fprintf(fp_engine_analysis_FD,
            "%s, smallest pattern %u byte(s), longest pattern %u byte(s), number of patterns %u, avg pattern len %.2f byte(s)\n",
            DetectSigmatchListEnumToString(i), f->min, f->max, f->cnt, (float)((double)f->tot/(float)f->cnt));
    }

    if (fp_engine_analysis_FD != NULL) {
        fclose(fp_engine_analysis_FD);
        fp_engine_analysis_FD = NULL;
    }

    return;
}


void CleanupRuleAnalyzer(void)
{
    if (rule_engine_analysis_FD != NULL) {
         SCLogInfo("Engine-Analyis for rules printed to file - %s", log_path);
        fclose(rule_engine_analysis_FD);
        rule_engine_analysis_FD = NULL;
    }
}

/**
 * \brief Compiles regex for rule analysis
 * \retval 1 if successful
 * \retval 0 if on error
 */
int PerCentEncodingSetup ()
{
#define DETECT_PERCENT_ENCODING_REGEX "%[0-9|a-f|A-F]{2}"
    const char *eb = NULL;
    int eo = 0;
    int opts = 0;    //PCRE_NEWLINE_ANY??

    percent_re = pcre_compile(DETECT_PERCENT_ENCODING_REGEX, opts, &eb, &eo, NULL);
    if (percent_re == NULL) {
        SCLogError(SC_ERR_PCRE_COMPILE, "Compile of \"%s\" failed at offset %" PRId32 ": %s",
                   DETECT_PERCENT_ENCODING_REGEX, eo, eb);
        return 0;
    }

    percent_re_study = pcre_study(percent_re, 0, &eb);
    if (eb != NULL) {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        return 0;
    }
    return 1;
}

/**
 * \brief Checks for % encoding in content.
 * \param Pointer to content
 * \retval number of matches if content has % encoding
 * \retval 0 if it doesn't have % encoding
 * \retval -1 on error
 */
int PerCentEncodingMatch (uint8_t *content, uint8_t content_len)
{
#define MAX_ENCODED_CHARS 240
    int ret = 0;
    int ov[MAX_ENCODED_CHARS];

    ret = pcre_exec(percent_re, percent_re_study, (char *)content, content_len, 0, 0, ov, MAX_ENCODED_CHARS);
    if (ret == -1) {
        return 0;
    }
    else if (ret < -1) {
        SCLogError(SC_ERR_PCRE_MATCH, "Error parsing content - %s; error code is %d", content, ret);
        return -1;
    }
    return ret;
}

static void EngineAnalysisRulesPrintFP(const DetectEngineCtx *de_ctx, const Signature *s)
{
    DetectContentData *fp_cd = NULL;
    SigMatch *mpm_sm = s->init_data->mpm_sm;

    if (mpm_sm != NULL) {
        fp_cd = (DetectContentData *)mpm_sm->ctx;
    }

    if (fp_cd == NULL) {
        return;
    }

    uint16_t patlen = fp_cd->content_len;
    uint8_t *pat = SCMalloc(fp_cd->content_len + 1);
    if (unlikely(pat == NULL)) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }
    memcpy(pat, fp_cd->content, fp_cd->content_len);
    pat[fp_cd->content_len] = '\0';

    if (fp_cd->flags & DETECT_CONTENT_FAST_PATTERN_CHOP) {
        SCFree(pat);
        patlen = fp_cd->fp_chop_len;
        pat = SCMalloc(fp_cd->fp_chop_len + 1);
        if (unlikely(pat == NULL)) {
            exit(EXIT_FAILURE);
        }
        memcpy(pat, fp_cd->content + fp_cd->fp_chop_offset, fp_cd->fp_chop_len);
        pat[fp_cd->fp_chop_len] = '\0';
        fprintf(rule_engine_analysis_FD, "    Fast Pattern \"");
        PrintRawUriFp(rule_engine_analysis_FD, pat, patlen);
    } else {
        fprintf(rule_engine_analysis_FD, "    Fast Pattern \"");
        PrintRawUriFp(rule_engine_analysis_FD, pat, patlen);
    }
    SCFree(pat);

    fprintf(rule_engine_analysis_FD, "\" on \"");

    int list_type = SigMatchListSMBelongsTo(s, mpm_sm);
    if (list_type == DETECT_SM_LIST_PMATCH) {
        int payload = 0;
        int stream = 0;
        if (SignatureHasPacketContent(s))
            payload = 1;
        if (SignatureHasStreamContent(s))
            stream = 1;
        fprintf(rule_engine_analysis_FD, "%s",
                payload ? (stream ? "payload and reassembled stream" : "payload") : "reassembled stream");
    }
    else {
        const char *desc = DetectBufferTypeGetDescriptionById(de_ctx, list_type);
        const char *name = DetectBufferTypeGetNameById(de_ctx, list_type);
        if (desc && name) {
            fprintf(rule_engine_analysis_FD, "%s (%s)", desc, name);
        } else if (desc || name) {
            fprintf(rule_engine_analysis_FD, "%s", desc ? desc : name);
        }

    }

    fprintf(rule_engine_analysis_FD, "\" ");
    if (de_ctx->buffer_type_map[list_type] && de_ctx->buffer_type_map[list_type]->transforms.cnt) {
        fprintf(rule_engine_analysis_FD, "(with %d transform(s)) ",
                de_ctx->buffer_type_map[list_type]->transforms.cnt);
    }
    fprintf(rule_engine_analysis_FD, "buffer.\n");

    return;
}


void EngineAnalysisRulesFailure(char *line, char *file, int lineno)
{
    fprintf(rule_engine_analysis_FD, "== Sid: UNKNOWN ==\n");
    fprintf(rule_engine_analysis_FD, "%s\n", line);
    fprintf(rule_engine_analysis_FD, "    FAILURE: invalid rule.\n");
    fprintf(rule_engine_analysis_FD, "    File: %s.\n", file);
    fprintf(rule_engine_analysis_FD, "    Line: %d.\n", lineno);
    fprintf(rule_engine_analysis_FD, "\n");
}

#include "util-buffer.h"
#include "output-json.h"

typedef struct RuleAnalyzer {
    json_t *js; /* document root */

    json_t *js_warnings;
    json_t *js_notes;
} RuleAnalyzer;

static void __attribute__ ((format (printf, 2, 3)))
AnalyzerNote(RuleAnalyzer *ctx, char *fmt, ...)
{
    va_list ap;
    char str[1024];

    va_start(ap, fmt);
    vsnprintf(str, sizeof(str), fmt, ap);
    va_end(ap);

    if (!ctx->js_notes)
        ctx->js_notes = json_array();
    if (ctx->js_notes)
        json_array_append_new(ctx->js_notes, json_string(str));
}

static void __attribute__ ((format (printf, 2, 3)))
AnalyzerWarning(RuleAnalyzer *ctx, char *fmt, ...)
{
    va_list ap;
    char str[1024];

    va_start(ap, fmt);
    vsnprintf(str, sizeof(str), fmt, ap);
    va_end(ap);

    if (!ctx->js_warnings)
        ctx->js_warnings = json_array();
    if (ctx->js_warnings)
        json_array_append_new(ctx->js_warnings, json_string(str));
}

#define CHECK(pat) if (strlen((pat)) <= len && memcmp((pat), buf, MIN(len, strlen((pat)))) == 0) return true;

static bool LooksLikeHTTPMethod(const uint8_t *buf, uint16_t len)
{
    CHECK("GET /");
    CHECK("POST /");
    CHECK("HEAD /");
    CHECK("PUT /");
    return false;
}

static bool LooksLikeHTTPUA(const uint8_t *buf, uint16_t len)
{
    CHECK("User-Agent: ");
    CHECK("\nUser-Agent: ");
    return false;
}

static void DumpMatches(RuleAnalyzer *ctx, json_t *js, const SigMatchData *smd)
{
    if (smd == NULL)
        return;

    json_t *js_matches = json_array();
    if (js_matches == NULL) {
        return;
    }
    do {
        json_t *js_match = json_object();
        if (js_match != NULL) {
            const char *mname = sigmatch_table[smd->type].name;
            json_object_set_new(js_match, "name", json_string(mname));

            switch (smd->type) {
                case DETECT_CONTENT: {
                    const DetectContentData *cd = (const DetectContentData *)smd->ctx;
                    uint8_t *pat = SCMalloc(cd->content_len + 1);
                    if (unlikely(pat == NULL)) {
                        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
                        exit(EXIT_FAILURE);
                    }
                    memcpy(pat, cd->content, cd->content_len);
                    pat[cd->content_len] = '\0';

                    json_t *js_match_content = json_object();
                    if (js_match_content != NULL) {
                        json_object_set_new(js_match_content, "pattern", SCJsonString((const char *)pat));
                        json_object_set_new(js_match_content, "nocase", json_boolean(cd->flags & DETECT_CONTENT_NOCASE));
                        json_object_set_new(js_match_content, "negated", json_boolean(cd->flags & DETECT_CONTENT_NEGATED));
                        json_object_set_new(js_match_content, "starts_with", json_boolean(cd->flags & DETECT_CONTENT_STARTS_WITH));
                        json_object_set_new(js_match_content, "ends_with", json_boolean(cd->flags & DETECT_CONTENT_ENDS_WITH));
                        json_object_set_new(js_match_content, "is_mpm", json_boolean(cd->flags & DETECT_CONTENT_MPM));
                        if (cd->flags & DETECT_CONTENT_OFFSET) {
                            json_object_set_new(js_match_content, "offset", json_integer(cd->offset));
                        }
                        if (cd->flags & DETECT_CONTENT_DEPTH) {
                            json_object_set_new(js_match_content, "depth", json_integer(cd->depth));
                        }
                        if (cd->flags & DETECT_CONTENT_DISTANCE) {
                            json_object_set_new(js_match_content, "distance", json_integer(cd->distance));
                        }
                        if (cd->flags & DETECT_CONTENT_WITHIN) {
                            json_object_set_new(js_match_content, "within", json_integer(cd->within));
                        }

                        json_object_set_new(js_match_content, "fast_pattern", json_boolean(cd->flags & DETECT_CONTENT_FAST_PATTERN));
                        if (cd->flags & DETECT_CONTENT_FAST_PATTERN_ONLY) {
                            AnalyzerNote(ctx, (char *)"'fast_pattern:only' option is silently ignored and is interpreted as regular 'fast_pattern'");
                        }

                        json_object_set_new(js_match, "content", js_match_content);
                    }

                    if (LooksLikeHTTPMethod(cd->content, cd->content_len)) {
                        AnalyzerWarning(ctx, (char *)"pattern looks like it inspects HTTP, use http_request_line or http_method and http_uri instead for improved performance");
                    }
                    if (LooksLikeHTTPUA(cd->content, cd->content_len)) {
                        AnalyzerWarning(ctx, (char *)"pattern looks like it inspects HTTP, use http_user_agent or http_header for improved performance");
                    }

                    SCFree(pat);
                    break;
                }
            }
        }
        json_array_append_new(js_matches, js_match);

        if (smd->is_last)
            break;
        smd++;
    } while (1);
    json_object_set_new(js, "matches", js_matches);
}

SCMutex g_rules_analyzer_write_m = SCMUTEX_INITIALIZER;
void EngineAnalysisRules2(const DetectEngineCtx *de_ctx, const Signature *s)
{
    SCEnter();

    RuleAnalyzer ctx = { NULL, NULL, NULL };

    ctx.js = json_object();
    if (ctx.js == NULL)
        SCReturn;

    json_object_set_new(ctx.js, "raw", json_string(s->sig_str));
    json_object_set_new(ctx.js, "id", json_integer(s->id));
    json_object_set_new(ctx.js, "gid", json_integer(s->gid));
    json_object_set_new(ctx.js, "rev", json_integer(s->rev));
    json_object_set_new(ctx.js, "msg", json_string(s->msg));

    const char *alproto = AppProtoToString(s->alproto);
    json_object_set_new(ctx.js, "app_proto", json_string(alproto));

    json_t *js_flags = json_array();
    if (js_flags != NULL) {
        if (s->mask & SIG_MASK_REQUIRE_PAYLOAD) {
            json_array_append_new(js_flags, json_string("payload"));
        }
        if (s->mask & SIG_MASK_REQUIRE_NO_PAYLOAD) {
            json_array_append_new(js_flags, json_string("no_payload"));
        }
        if (s->mask & SIG_MASK_REQUIRE_FLOW) {
            json_array_append_new(js_flags, json_string("flow"));
        }
        if (s->mask & SIG_MASK_REQUIRE_FLAGS_INITDEINIT) {
            json_array_append_new(js_flags, json_string("tcp_flags_init_deinit"));
        }
        if (s->mask & SIG_MASK_REQUIRE_FLAGS_UNUSUAL) {
            json_array_append_new(js_flags, json_string("tcp_flags_unusual"));
        }
        if (s->mask & SIG_MASK_REQUIRE_DCERPC) {
            json_array_append_new(js_flags, json_string("dcerpc"));
        }
        if (s->mask & SIG_MASK_REQUIRE_ENGINE_EVENT) {
            json_array_append_new(js_flags, json_string("engine_event"));
        }
        json_object_set_new(ctx.js, "requirements", js_flags);
    }

    js_flags = json_array();
    if (js_flags != NULL) {
        if (s->flags & SIG_FLAG_SRC_ANY) {
            json_array_append_new(js_flags, json_string("src_any"));
        }
        if (s->flags & SIG_FLAG_DST_ANY) {
            json_array_append_new(js_flags, json_string("dst_any"));
        }
        if (s->flags & SIG_FLAG_SP_ANY) {
            json_array_append_new(js_flags, json_string("sp_any"));
        }
        if (s->flags & SIG_FLAG_DP_ANY) {
            json_array_append_new(js_flags, json_string("dp_any"));
        }
        if (s->flags & SIG_FLAG_NOALERT) {
            json_array_append_new(js_flags, json_string("noalert"));
        }
        if (s->flags & SIG_FLAG_DSIZE) {
            json_array_append_new(js_flags, json_string("dsize"));
        }
        if (s->flags & SIG_FLAG_APPLAYER) {
            json_array_append_new(js_flags, json_string("applayer"));
        }
        if (s->flags & SIG_FLAG_IPONLY) {
            json_array_append_new(js_flags, json_string("ip_only"));
        }
        if (s->flags & SIG_FLAG_REQUIRE_PACKET) {
            json_array_append_new(js_flags, json_string("need_packet"));
        }
        if (s->flags & SIG_FLAG_REQUIRE_STREAM) {
            json_array_append_new(js_flags, json_string("need_stream"));
        }
        if (s->flags & SIG_FLAG_MPM_NEG) {
            json_array_append_new(js_flags, json_string("negated_mpm"));
        }
        if (s->flags & SIG_FLAG_FLUSH) {
            json_array_append_new(js_flags, json_string("flush"));
        }
        if (s->flags & SIG_FLAG_REQUIRE_FLOWVAR) {
            json_array_append_new(js_flags, json_string("need_flowvar"));
        }
        if (s->flags & SIG_FLAG_FILESTORE) {
            json_array_append_new(js_flags, json_string("filestore"));
        }
        if (s->flags & SIG_FLAG_TOSERVER) {
            json_array_append_new(js_flags, json_string("toserver"));
        }
        if (s->flags & SIG_FLAG_TOCLIENT) {
            json_array_append_new(js_flags, json_string("toclient"));
        }
        if (s->flags & SIG_FLAG_TLSSTORE) {
            json_array_append_new(js_flags, json_string("tlsstore"));
        }
        if (s->flags & SIG_FLAG_BYPASS) {
            json_array_append_new(js_flags, json_string("bypass"));
        }
        if (s->flags & SIG_FLAG_PREFILTER) {
            json_array_append_new(js_flags, json_string("prefilter"));
        }
        if (s->flags & SIG_FLAG_PDONLY) {
            json_array_append_new(js_flags, json_string("proto_detect_only"));
        }
        if (s->flags & SIG_FLAG_SRC_IS_TARGET) {
            json_array_append_new(js_flags, json_string("src_is_target"));
        }
        if (s->flags & SIG_FLAG_DEST_IS_TARGET) {
            json_array_append_new(js_flags, json_string("dst_is_target"));
        }
        json_object_set_new(ctx.js, "flags", js_flags);
    }

    json_t *js_pkt_array = json_array();
    const DetectEnginePktInspectionEngine *pkt = s->pkt_inspect;
    for ( ; pkt != NULL; pkt = pkt->next) {
        const char *name = DetectBufferTypeGetNameById(de_ctx, pkt->sm_list);
        if (name == NULL) {
            switch (pkt->sm_list) {
                case DETECT_SM_LIST_PMATCH:
                    name = "payload";
                    break;
                case DETECT_SM_LIST_MATCH:
                    name = "packet";
                    break;
                default:
                    name = "unknown";
                    break;
            }
        }
        json_t *js_engine = json_object();
        if (js_engine != NULL) {
            json_object_set_new(js_engine, "name", json_string(name));

            json_object_set_new(js_engine, "is_mpm", json_boolean(pkt->mpm));

            DumpMatches(&ctx, js_engine, pkt->smd);

            json_array_append_new(js_pkt_array, js_engine);
        }
    }
    json_object_set_new(ctx.js, "pkt_engines", js_pkt_array);

    if (s->init_data->init_flags & SIG_FLAG_INIT_STATE_MATCH) {
        bool has_stream = false;
        bool has_client_body_mpm = false;
        bool has_file_data_mpm = false;

        json_t *js_array = json_array();
        const DetectEngineAppInspectionEngine *app = s->app_inspect;
        for ( ; app != NULL; app = app->next) {
            const char *name = DetectBufferTypeGetNameById(de_ctx, app->sm_list);
            if (name == NULL) {
                switch (app->sm_list) {
                    case DETECT_SM_LIST_PMATCH:
                        name = "stream";
                        break;
                    default:
                        name = "unknown";
                        break;
                }
            }

            if (app->sm_list == DETECT_SM_LIST_PMATCH && !app->mpm) {
                has_stream = true;
            } else if (app->mpm && strcmp(name, "http_client_body") == 0) {
                has_client_body_mpm = true;
            } else if (app->mpm && strcmp(name, "file_data") == 0) {
                has_file_data_mpm = true;
            }

            json_t *js_engine = json_object();
            if (js_engine != NULL) {
                json_object_set_new(js_engine, "name", json_string(name));

                const char *direction = app->dir == 0 ? "toserver" : "toclient";
                json_object_set_new(js_engine, "direction", json_string(direction));
                json_object_set_new(js_engine, "is_mpm", json_boolean(app->mpm));
                json_object_set_new(js_engine, "app_proto", json_string(AppProtoToString(app->alproto)));
                json_object_set_new(js_engine, "progress", json_integer(app->progress));

                DumpMatches(&ctx, js_engine, app->smd);

                json_array_append_new(js_array, js_engine);
            }
        }
        json_object_set_new(ctx.js, "engines", js_array);

        if (has_stream && has_client_body_mpm)
            AnalyzerNote(&ctx, (char *)"mpm in http_client_body combined with stream match leads to stream buffering");
        if (has_stream && has_file_data_mpm)
            AnalyzerNote(&ctx, (char *)"mpm in file_data combined with stream match leads to stream buffering");
    }

    json_t *js_lists = json_object();
    for (int i = 0; i < DETECT_SM_LIST_MAX; i++) {
        if (s->sm_arrays[i] != NULL) {
            json_t *js_list = json_object();
            if (js_list != NULL) {
                DumpMatches(&ctx, js_list, s->sm_arrays[i]);
                json_object_set_new(js_lists, DetectSigmatchListEnumToString(i), js_list);
            }
        }
    }
    json_object_set_new(ctx.js, "lists", js_lists);

    if (ctx.js_warnings) {
        json_object_set_new(ctx.js, "warnings", ctx.js_warnings);
    }
    if (ctx.js_notes) {
        json_object_set_new(ctx.js, "notes", ctx.js_notes);
    }

    const char *filename = "rules.json";
    const char *log_dir = ConfigGetLogDirectory();
    char json_path[PATH_MAX] = "";
    snprintf(json_path, sizeof(json_path), "%s/%s", log_dir, filename);

    MemBuffer *mbuf = MemBufferCreateNew(4096);
    if (mbuf != NULL) {
        OutputJSONMemBufferWrapper wrapper = {
            .buffer = &mbuf,
            .expand_by = 4096,
        };

        int r = json_dump_callback(ctx.js, OutputJSONMemBufferCallback, &wrapper,
                JSON_PRESERVE_ORDER|JSON_COMPACT|JSON_ENSURE_ASCII|
                JSON_ESCAPE_SLASH);
        if (r != 0) {
            SCLogWarning(SC_ERR_SOCKET, "unable to serialize JSON object");
        } else {
            MemBufferWriteString(mbuf, "\n");
            SCMutexLock(&g_rules_analyzer_write_m);
            FILE *fp = fopen(json_path, "a");
            if (fp != NULL) {
                MemBufferPrintToFPAsString(mbuf, fp);
                fclose(fp);
            }
            SCMutexUnlock(&g_rules_analyzer_write_m);
        }

        MemBufferFree(mbuf);
    }
    json_object_clear(ctx.js);
    json_decref(ctx.js);
    SCReturn;
}

static void EngineAnalysisItemsReset(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(analyzer_items); i++) {
        analyzer_items[i].item_seen = false;
    }
}

static void EngineAnalysisItemsInit(void)
{
    static bool analyzer_init = false;

    if (analyzer_init) {
        EngineAnalysisItemsReset();
        return;
    }

    memset(analyzer_item_map, -1, sizeof(analyzer_item_map));

    for (size_t i = 0; i < ARRAY_SIZE(analyzer_items); i++) {
        DetectEngineAnalyzerItems *analyzer_item = &analyzer_items[i];

        analyzer_item->item_id = DetectBufferTypeGetByName(analyzer_item->item_name);
        if (analyzer_item->item_id == -1) {
            /* Mismatch between the analyzer_items array and what's supported */
            FatalError(SC_ERR_INITIALIZATION,
                       "unable to initialize engine-analysis table: detect buffer \"%s\" not recognized.",
                       analyzer_item->item_name);
        }
        analyzer_item->item_seen = false;

        if (analyzer_item->export_item_seen) {
            for (size_t k = 0; k < ARRAY_SIZE(exposed_item_seen_list); k++) {
                if (0 == strcmp(exposed_item_seen_list[k].bufname, analyzer_item->item_name))
                    exposed_item_seen_list[k].item_seen_ptr = &analyzer_item->item_seen;
            }

        }
        analyzer_item_map[analyzer_item->item_id] = (int16_t) i;
    }

    analyzer_init = true;
}

/**
 * \brief Prints analysis of loaded rules.
 *
 *        Warns if potential rule issues are detected. For example,
 *        warns if a rule uses a construct that may perform poorly,
 *        e.g. pcre without content or with http_method content only;
 *        warns if a rule uses a construct that may not be consistent with intent,
 *        e.g. client side ports only, http and content without any http_* modifiers, etc.
 *
 * \param s Pointer to the signature.
 */
void EngineAnalysisRules(const DetectEngineCtx *de_ctx,
        const Signature *s, const char *line)
{
    uint32_t rule_bidirectional = 0;
    uint32_t rule_pcre = 0;
    uint32_t rule_pcre_http = 0;
    uint32_t rule_content = 0;
    uint32_t rule_flow = 0;
    uint32_t rule_flags = 0;
    uint32_t rule_flow_toserver = 0;
    uint32_t rule_flow_toclient = 0;
    uint32_t rule_flow_nostream = 0;
    uint32_t rule_ipv4_only = 0;
    uint32_t rule_ipv6_only = 0;
    uint32_t rule_flowbits = 0;
    uint32_t rule_flowint = 0;
    uint32_t rule_content_http = 0;
    uint32_t rule_content_offset_depth = 0;
    int32_t list_id = 0;
    uint32_t rule_warning = 0;
    uint32_t stream_buf = 0;
    uint32_t packet_buf = 0;
    uint32_t warn_pcre_no_content = 0;
    uint32_t warn_pcre_http_content = 0;
    uint32_t warn_pcre_http = 0;
    uint32_t warn_content_http_content = 0;
    uint32_t warn_content_http = 0;
    uint32_t warn_tcp_no_flow = 0;
    uint32_t warn_client_ports = 0;
    uint32_t warn_direction = 0;
    uint32_t warn_method_toclient = 0;
    uint32_t warn_method_serverbody = 0;
    uint32_t warn_pcre_method = 0;
    uint32_t warn_encoding_norm_http_buf = 0;
    uint32_t warn_offset_depth_pkt_stream = 0;
    uint32_t warn_offset_depth_alproto = 0;
    uint32_t warn_non_alproto_fp_for_alproto_sig = 0;
    uint32_t warn_no_direction = 0;
    uint32_t warn_both_direction = 0;

    EngineAnalysisItemsInit();

    bool *http_method_item_seen_ptr = exposed_item_seen_list[0].item_seen_ptr;
    bool *http_server_body_item_seen_ptr = exposed_item_seen_list[1].item_seen_ptr;

    if (s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
        rule_bidirectional = 1;
    }

    if (s->flags & SIG_FLAG_REQUIRE_PACKET) {
        packet_buf += 1;
    }
    if (s->flags & SIG_FLAG_REQUIRE_STREAM) {
        stream_buf += 1;
    }

    if (s->proto.flags & DETECT_PROTO_IPV4) {
        rule_ipv4_only += 1;
    }
    if (s->proto.flags & DETECT_PROTO_IPV6) {
        rule_ipv6_only += 1;
    }

    for (list_id = 0; list_id < (int)s->init_data->smlists_array_size; list_id++) {
        SigMatch *sm = NULL;
        for (sm = s->init_data->smlists[list_id]; sm != NULL; sm = sm->next) {
            int16_t item_slot = analyzer_item_map[list_id];
            if (sm->type == DETECT_PCRE) {
                if (item_slot == -1) {
                    rule_pcre++;
                    continue;
                }

                rule_pcre_http++;
                analyzer_items[item_slot].item_seen = true;
            } else if (sm->type == DETECT_CONTENT) {
                if (item_slot == -1) {
                    rule_content++;
                    if (list_id == DETECT_SM_LIST_PMATCH) {
                        DetectContentData *cd = (DetectContentData *)sm->ctx;
                        if (cd->flags & (DETECT_CONTENT_OFFSET | DETECT_CONTENT_DEPTH)) {
                            rule_content_offset_depth++;
                        }
                    }
                    continue;
                }

                rule_content_http++;
                analyzer_items[item_slot].item_seen = true;

                if (analyzer_items[item_slot].check_encoding_match) {
                    DetectContentData *cd = (DetectContentData *)sm->ctx;
                    if (cd != NULL && PerCentEncodingMatch(cd->content, cd->content_len) > 0) {
                        warn_encoding_norm_http_buf += 1;
                    }
                }
            }
            else if (sm->type == DETECT_FLOW) {
                rule_flow += 1;
                if ((s->flags & SIG_FLAG_TOSERVER) && !(s->flags & SIG_FLAG_TOCLIENT)) {
                    rule_flow_toserver = 1;
                }
                else if ((s->flags & SIG_FLAG_TOCLIENT) && !(s->flags & SIG_FLAG_TOSERVER)) {
                    rule_flow_toclient = 1;
                }
                DetectFlowData *fd = (DetectFlowData *)sm->ctx;
                if (fd != NULL) {
                    if (fd->flags & DETECT_FLOW_FLAG_NOSTREAM)
                        rule_flow_nostream = 1;
                }
            }
            else if (sm->type == DETECT_FLOWBITS) {
                if (list_id == DETECT_SM_LIST_MATCH) {
                    rule_flowbits += 1;
                }
            }
            else if (sm->type == DETECT_FLOWINT) {
                if (list_id == DETECT_SM_LIST_MATCH) {
                    rule_flowint += 1;
                }
            }
            else if (sm->type == DETECT_FLAGS) {
                DetectFlagsData *fd = (DetectFlagsData *)sm->ctx;
                if (fd != NULL) {
                    rule_flags = 1;
                }
            }
        } /* for (sm = s->sm_lists[list_id]; sm != NULL; sm = sm->next) */

    } /* for ( ; list_id < DETECT_SM_LIST_MAX; list_id++) */


    if (rule_pcre > 0 && rule_content == 0 && rule_content_http == 0) {
        rule_warning += 1;
        warn_pcre_no_content = 1;
    }

    if (rule_content_http > 0 && rule_pcre > 0 && rule_pcre_http == 0) {
        rule_warning += 1;
        warn_pcre_http_content = 1;
    }
    else if (s->alproto == ALPROTO_HTTP && rule_pcre > 0 && rule_pcre_http == 0) {
        rule_warning += 1;
        warn_pcre_http = 1;
    }

    if (rule_content > 0 && rule_content_http > 0) {
        rule_warning += 1;
        warn_content_http_content = 1;
    }
    if (s->alproto == ALPROTO_HTTP && rule_content > 0 && rule_content_http == 0) {
        rule_warning += 1;
        warn_content_http = 1;
    }
    if (rule_content == 1) {
         //todo: warning if content is weak, separate warning for pcre + weak content
    }
    if (rule_flow == 0 && rule_flags == 0
        && !(s->proto.flags & DETECT_PROTO_ANY) && DetectProtoContainsProto(&s->proto, IPPROTO_TCP)
        && (rule_content || rule_content_http || rule_pcre || rule_pcre_http || rule_flowbits)) {
        rule_warning += 1;
        warn_tcp_no_flow = 1;
    }
    if (rule_flow && !rule_bidirectional && (rule_flow_toserver || rule_flow_toclient)
                  && !((s->flags & SIG_FLAG_SP_ANY) && (s->flags & SIG_FLAG_DP_ANY))) {
        if (((s->flags & SIG_FLAG_TOSERVER) && !(s->flags & SIG_FLAG_SP_ANY) && (s->flags & SIG_FLAG_DP_ANY))
          || ((s->flags & SIG_FLAG_TOCLIENT) && !(s->flags & SIG_FLAG_DP_ANY) && (s->flags & SIG_FLAG_SP_ANY))) {
            rule_warning += 1;
            warn_client_ports = 1;
        }
    }
    if (rule_flow && rule_bidirectional && (rule_flow_toserver || rule_flow_toclient)) {
        rule_warning += 1;
        warn_direction = 1;
    }

    if (*http_method_item_seen_ptr) {
        if (rule_flow && rule_flow_toclient) {
            rule_warning += 1;
            warn_method_toclient = 1;
        }
        if (*http_server_body_item_seen_ptr) {
            rule_warning += 1;
            warn_method_serverbody = 1;
        }
        if (rule_content == 0 && rule_content_http == 0 && (rule_pcre > 0 || rule_pcre_http > 0)) {
            rule_warning += 1;
            warn_pcre_method = 1;
        }
    }
    if (rule_content_offset_depth > 0 && stream_buf && packet_buf) {
        rule_warning += 1;
        warn_offset_depth_pkt_stream = 1;
    }
    if (rule_content_offset_depth > 0 && !stream_buf && packet_buf && s->alproto != ALPROTO_UNKNOWN) {
        rule_warning += 1;
        warn_offset_depth_alproto = 1;
    }
    if (s->init_data->mpm_sm != NULL && s->alproto == ALPROTO_HTTP &&
        SigMatchListSMBelongsTo(s, s->init_data->mpm_sm) == DETECT_SM_LIST_PMATCH) {
        rule_warning += 1;
        warn_non_alproto_fp_for_alproto_sig = 1;
    }

    if ((s->flags & (SIG_FLAG_TOSERVER|SIG_FLAG_TOCLIENT)) == 0) {
        warn_no_direction += 1;
        rule_warning += 1;
    }

    /* No warning about direction for ICMP protos */
    if (!(DetectProtoContainsProto(&s->proto, IPPROTO_ICMPV6) && DetectProtoContainsProto(&s->proto, IPPROTO_ICMP))) {
        if ((s->flags & (SIG_FLAG_TOSERVER|SIG_FLAG_TOCLIENT)) == (SIG_FLAG_TOSERVER|SIG_FLAG_TOCLIENT)) {
            warn_both_direction += 1;
            rule_warning += 1;
        }
    }

    if (!rule_warnings_only || (rule_warnings_only && rule_warning > 0)) {
        fprintf(rule_engine_analysis_FD, "== Sid: %u ==\n", s->id);
        fprintf(rule_engine_analysis_FD, "%s\n", line);

        if (s->flags & SIG_FLAG_IPONLY) fprintf(rule_engine_analysis_FD, "    Rule is ip only.\n");
        if (s->flags & SIG_FLAG_PDONLY) fprintf(rule_engine_analysis_FD, "    Rule is PD only.\n");
        if (rule_ipv6_only) fprintf(rule_engine_analysis_FD, "    Rule is IPv6 only.\n");
        if (rule_ipv4_only) fprintf(rule_engine_analysis_FD, "    Rule is IPv4 only.\n");
        if (packet_buf) fprintf(rule_engine_analysis_FD, "    Rule matches on packets.\n");
        if (!rule_flow_nostream && stream_buf && (rule_flow || rule_flowbits || rule_content || rule_pcre)) {
            fprintf(rule_engine_analysis_FD, "    Rule matches on reassembled stream.\n");
        }
        for(size_t i = 0; i < ARRAY_SIZE(analyzer_items); i++) {
            DetectEngineAnalyzerItems *ai = &analyzer_items[i];
            if (ai->item_seen) {
                 fprintf(rule_engine_analysis_FD, "    Rule matches on %s buffer.\n", ai->display_name);
            }
        }
        if (s->alproto != ALPROTO_UNKNOWN) {
            fprintf(rule_engine_analysis_FD, "    App layer protocol is %s.\n", AppProtoToString(s->alproto));
        }
        if (rule_content || rule_content_http || rule_pcre || rule_pcre_http) {
            fprintf(rule_engine_analysis_FD, "    Rule contains %d content options, %d http content options, %d pcre options, and %d pcre options with http modifiers.\n", rule_content, rule_content_http, rule_pcre, rule_pcre_http);
        }

        /* print fast pattern info */
        if (s->init_data->prefilter_sm) {
            fprintf(rule_engine_analysis_FD, "    Prefilter on: %s.\n",
                    sigmatch_table[s->init_data->prefilter_sm->type].name);
        } else {
            EngineAnalysisRulesPrintFP(de_ctx, s);
        }

        /* this is where the warnings start */
        if (warn_pcre_no_content /*rule_pcre > 0 && rule_content == 0 && rule_content_http == 0*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule uses pcre without a content option present.\n"
                                             "             -Consider adding a content to improve performance of this rule.\n");
        }
        if (warn_pcre_http_content /*rule_content_http > 0 && rule_pcre > 0 && rule_pcre_http == 0*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule uses content options with http_* and pcre options without http modifiers.\n"
                                             "             -Consider adding http pcre modifier.\n");
        }
        else if (warn_pcre_http /*s->alproto == ALPROTO_HTTP && rule_pcre > 0 && rule_pcre_http == 0*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule app layer protocol is http, but pcre options do not have http modifiers.\n"
                                             "             -Consider adding http pcre modifiers.\n");
        }
        if (warn_content_http_content /*rule_content > 0 && rule_content_http > 0*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule contains content with http_* and content without http_*.\n"
                                         "             -Consider adding http content modifiers.\n");
        }
        if (warn_content_http /*s->alproto == ALPROTO_HTTP && rule_content > 0 && rule_content_http == 0*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule app layer protocol is http, but content options do not have http_* modifiers.\n"
                                             "             -Consider adding http content modifiers.\n");
        }
        if (rule_content == 1) {
             //todo: warning if content is weak, separate warning for pcre + weak content
        }
        if (warn_encoding_norm_http_buf) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule may contain percent encoded content for a normalized http buffer match.\n");
        }
        if (warn_tcp_no_flow /*rule_flow == 0 && rule_flags == 0
                && !(s->proto.flags & DETECT_PROTO_ANY) && DetectProtoContainsProto(&s->proto, IPPROTO_TCP)*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: TCP rule without a flow or flags option.\n"
                                             "             -Consider adding flow or flags to improve performance of this rule.\n");
        }
        if (warn_client_ports /*rule_flow && !rule_bidirectional && (rule_flow_toserver || rule_flow_toclient)
                      && !((s->flags & SIG_FLAG_SP_ANY) && (s->flags & SIG_FLAG_DP_ANY)))
            if (((s->flags & SIG_FLAG_TOSERVER) && !(s->flags & SIG_FLAG_SP_ANY) && (s->flags & SIG_FLAG_DP_ANY))
                || ((s->flags & SIG_FLAG_TOCLIENT) && !(s->flags & SIG_FLAG_DP_ANY) && (s->flags & SIG_FLAG_SP_ANY))*/) {
                fprintf(rule_engine_analysis_FD, "    Warning: Rule contains ports or port variables only on the client side.\n"
                                                 "             -Flow direction possibly inconsistent with rule.\n");
        }
        if (warn_direction /*rule_flow && rule_bidirectional && (rule_flow_toserver || rule_flow_toclient)*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule is bidirectional and has a flow option with a specific direction.\n");
        }
        if (warn_method_toclient /*http_method_buf && rule_flow && rule_flow_toclient*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule uses content or pcre for http_method with flow:to_client or from_server\n");
        }
        if (warn_method_serverbody /*http_method_buf && http_server_body_buf*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule uses content or pcre for http_method with content or pcre for http_server_body.\n");
        }
        if (warn_pcre_method /*http_method_buf && rule_content == 0 && rule_content_http == 0
                               && (rule_pcre > 0 || rule_pcre_http > 0)*/) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule uses pcre with only a http_method content; possible performance issue.\n");
        }
        if (warn_offset_depth_pkt_stream) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule has depth"
                    "/offset with raw content keywords.  Please note the "
                    "offset/depth will be checked against both packet "
                    "payloads and stream.  If you meant to have the offset/"
                    "depth checked against just the payload, you can update "
                    "the signature as \"alert tcp-pkt...\"\n");
        }
        if (warn_offset_depth_alproto) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule has "
                    "offset/depth set along with a match on a specific "
                    "app layer protocol - %d.  This can lead to FNs if we "
                    "have a offset/depth content match on a packet payload "
                    "before we can detect the app layer protocol for the "
                    "flow.\n", s->alproto);
        }
        if (warn_non_alproto_fp_for_alproto_sig) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule app layer "
                    "protocol is http, but the fast_pattern is set on the raw "
                    "stream.  Consider adding fast_pattern over a http "
                    "buffer for increased performance.");
        }
        if (warn_no_direction) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule has no direction indicator.\n");
        }
        if (warn_both_direction) {
            fprintf(rule_engine_analysis_FD, "    Warning: Rule is inspecting both the request and the response.\n");
        }
        if (rule_warning == 0) {
            fprintf(rule_engine_analysis_FD, "    No warnings for this rule.\n");
        }
        fprintf(rule_engine_analysis_FD, "\n");
    }
    return;
}
