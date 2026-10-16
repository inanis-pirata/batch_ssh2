#ifndef BS2_OUTPUT_H
#define BS2_OUTPUT_H

#include "util.h"

typedef enum {
    BS2_OK      = 0,
    BS2_FAIL    = 1,
    BS2_TIMEOUT = 2
} bs2_status;

typedef struct {
    const char *host;      /* borrowed pointer into the host list */
    bs2_status  status;
    int         exit_code; /* -1 = unknown/not applicable */
    int64_t     time_ms;
    sbuf        out;       /* remote stdout */
    sbuf        err;       /* remote stderr */
    char        error_msg[512];
    /* transfer stats (action-specific) */
    int         files_transferred;
    uint64_t    bytes_transferred;
} bs2_result;

void        bs2_result_reset(bs2_result *r, const char *host);
void        bs2_result_free(bs2_result *r);
const char *bs2_status_str(bs2_status s);

/* all printers write pure data to STDOUT; progress/errors go to STDERR */
void out_emit_text(const bs2_result *rs, size_t n);
void out_emit_csv(const bs2_result *rs, size_t n);
void out_emit_json(const bs2_result *rs, size_t n);

#endif
