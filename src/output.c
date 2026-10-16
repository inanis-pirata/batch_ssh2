#include "output.h"

#include <stdio.h>
#include <string.h>

void bs2_result_reset(bs2_result *r, const char *host) {
    memset(r, 0, sizeof(*r));
    r->host = host;
    r->exit_code = -1;
    sbuf_init(&r->out);
    sbuf_init(&r->err);
}

void bs2_result_free(bs2_result *r) {
    sbuf_free(&r->out);
    sbuf_free(&r->err);
}

const char *bs2_status_str(bs2_status s) {
    switch (s) {
    case BS2_OK:      return "success";
    case BS2_FAIL:    return "failed";
    case BS2_TIMEOUT: return "timeout";
    }
    return "unknown";
}

/* ---------------- text ---------------- */

void out_emit_text(const bs2_result *rs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const bs2_result *r = &rs[i];
        printf("===== %s [%s] exit=%d %lldms =====\n",
               r->host, bs2_status_str(r->status),
               r->exit_code, (long long)r->time_ms);
        if (r->out.len > 0) {
            fwrite(r->out.data, 1, r->out.len, stdout);
            if (r->out.data[r->out.len - 1] != '\n') printf("\n");
        }
        if (r->err.len > 0) {
            printf("--- stderr ---\n");
            fwrite(r->err.data, 1, r->err.len, stdout);
            if (r->err.data[r->err.len - 1] != '\n') printf("\n");
        }
        if (r->error_msg[0])
            printf("--- error: %s\n", r->error_msg);
        printf("\n");
    }
    fflush(stdout);
}

/* ---------------- csv (RFC 4180) ---------------- */

static void csv_field(const char *s, size_t len) {
    int need_quote = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ',' || c == '"' || c == '\r' || c == '\n') { need_quote = 1; break; }
    }
    if (!need_quote) {
        fwrite(s, 1, len, stdout);
        return;
    }
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') putchar('"');
        putchar(s[i]);
    }
    putchar('"');
}

void out_emit_csv(const bs2_result *rs, size_t n) {
#if defined(_WIN32)
    /* UTF-8 BOM so Excel opens Chinese text correctly on double click */
    fwrite("\xEF\xBB\xBF", 1, 3, stdout);
#endif
    printf("Host,Status,ExitCode,TimeCost(ms),Stdout,Stderr,ErrorMsg\n");
    for (size_t i = 0; i < n; i++) {
        const bs2_result *r = &rs[i];
        csv_field(r->host, strlen(r->host));
        putchar(',');
        csv_field(bs2_status_str(r->status), strlen(bs2_status_str(r->status)));
        printf(",%d,%lld,", r->exit_code, (long long)r->time_ms);
        csv_field(r->out.data ? r->out.data : "", r->out.len);
        putchar(',');
        csv_field(r->err.data ? r->err.data : "", r->err.len);
        putchar(',');
        csv_field(r->error_msg, strlen(r->error_msg));
        putchar('\n');
    }
    fflush(stdout);
}

/* ---------------- json ---------------- */

static void json_escape(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout);  break;
        case '\f': fputs("\\f", stdout);  break;
        case '\n': fputs("\\n", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        default:
            if (c < 0x20) printf("\\u%04x", c);
            else          putchar(c); /* pass UTF-8 (and raw bytes) through */
        }
    }
}

void out_emit_json(const bs2_result *rs, size_t n) {
    printf("[\n");
    for (size_t i = 0; i < n; i++) {
        const bs2_result *r = &rs[i];
        printf("  {\n    \"host\": \"");
        json_escape(r->host, strlen(r->host));
        printf("\",\n    \"status\": \"%s\",\n", bs2_status_str(r->status));
        printf("    \"exit_code\": %d,\n", r->exit_code);
        printf("    \"time_cost_ms\": %lld,\n", (long long)r->time_ms);
        printf("    \"files_transferred\": %d,\n", r->files_transferred);
        printf("    \"bytes_transferred\": %llu,\n", (unsigned long long)r->bytes_transferred);
        printf("    \"stdout\": \"");
        json_escape(r->out.data ? r->out.data : "", r->out.len);
        printf("\",\n    \"stderr\": \"");
        json_escape(r->err.data ? r->err.data : "", r->err.len);
        printf("\",\n    \"error_msg\": \"");
        json_escape(r->error_msg, strlen(r->error_msg));
        printf("\"\n  }%s\n", (i + 1 < n) ? "," : "");
    }
    printf("]\n");
    fflush(stdout);
}
