#define _CRT_NONSTDC_NO_DEPRECATE 1
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <dirent.h>
#endif

/* ---------------- sbuf ---------------- */

void sbuf_init(sbuf *s) { s->data = NULL; s->len = 0; s->cap = 0; }

void sbuf_free(sbuf *s) { free(s->data); s->data = NULL; s->len = s->cap = 0; }

static int sbuf_reserve(sbuf *s, size_t need) {
    if (s->len + need + 1 <= s->cap) return 0;
    size_t ncap = s->cap ? s->cap : 256;
    while (ncap < s->len + need + 1) ncap *= 2;
    char *nd = (char *)realloc(s->data, ncap);
    if (!nd) return -1;
    s->data = nd;
    s->cap = ncap;
    return 0;
}

void sbuf_append(sbuf *s, const void *p, size_t n) {
    if (n == 0) return;
    if (sbuf_reserve(s, n) != 0) {
        fprintf(stderr, "warning: out of memory, output may be truncated\n");
        return;
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void sbuf_appendf(sbuf *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need <= 0) { va_end(ap2); return; }
    if (sbuf_reserve(s, (size_t)need) != 0) { va_end(ap2); return; }
    vsnprintf(s->data + s->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)need;
}

/* ---------------- strlist ---------------- */

/* binary search: returns index if found, or insertion point + 1 if not */
static int strlist_bsearch(const strlist *l, const char *s) {
    int lo = 0, hi = (int)l->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(l->items[mid], s);
        if (cmp == 0) return mid;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -(lo + 1);
}

void strlist_push(strlist *l, const char *s) {
    /* find insertion point */
    int pos = strlist_bsearch(l, s);
    if (pos >= 0) return; /* already exists, skip */

    pos = -pos - 1; /* convert to insertion index */

    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 8;
        char **ni = (char **)realloc(l->items, nc * sizeof(char *));
        if (!ni) return;
        l->items = ni;
        l->cap = nc;
    }
    char *dup = strdup(s);
    if (!dup) return;

    /* shift elements to make room */
    memmove(&l->items[pos + 1], &l->items[pos],
            (l->count - (size_t)pos) * sizeof(char *));
    l->items[pos] = dup;
    l->count++;
}

int strlist_contains(const strlist *l, const char *s) {
    return strlist_bsearch(l, s) >= 0;
}

void strlist_free(strlist *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

/* ---------------- files ---------------- */

char *bs2_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

char *bs2_crlf_to_lf(char *s) {
    if (!s) return s;
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\r' && r[1] == '\n') {
            r++;            /* skip CR, keep LF */
        }
        *w++ = *r++;
    }
    *w = '\0';
    return s;
}

char *bs2_trim(char *s) {
    if (!s) return s;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = strlen(p);
    while (n > 0 && isspace((unsigned char)p[n - 1])) n--;
    p[n] = '\0';
    return p;
}

int bs2_mkdirs(const char *path) {
    if (!path || !*path) return -1;
    size_t n = strlen(path);
    char *tmp = (char *)malloc(n + 1);
    if (!tmp) return -1;
    memcpy(tmp, path, n + 1);

    int rc = 0;
    for (size_t i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0') {
#if defined(_WIN32)
                size_t seglen = strlen(tmp);
                if (seglen > 0 && strcmp(tmp, ".") != 0 &&
                    tmp[seglen - 1] != ':') {
                    _mkdir(tmp);   /* ignore EEXIST; verified below */
                }
#else
                mkdir(tmp, 0755);
#endif
            }
            tmp[i] = c;
        }
    }
    free(tmp);
    /* final check */
    struct stat st;
    if (stat(path, &st) == 0 && BS2_ISDIR(st.st_mode)) rc = 0;
    else rc = -1;
    return rc;
}

int64_t bs2_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    if (QueryPerformanceFrequency(&f) && QueryPerformanceCounter(&c))
        return (int64_t)(c.QuadPart * 1000 / f.QuadPart);
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

const char *bs2_basename(const char *path) {
    if (!path) return "";
    const char *p = path + strlen(path);
    while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
    return p;
}

char *bs2_dirname(const char *path) {
    const char *p = path + strlen(path);
    /* skip trailing separators */
    while (p > path && (p[-1] == '/' || p[-1] == '\\')) p--;
    /* drop last component */
    while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
    /* strip trailing separators again, keep root */
    while (p > path + 1 && (p[-1] == '/' || p[-1] == '\\')) p--;
    size_t n = (size_t)(p - path);
    if (n == 0) n = 1; /* "." */
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    if (n == 1 && p == path) out[0] = '.';
    else      memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

char *bs2_path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    int need_sep = dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\';
    char *out = (char *)malloc(dl + need_sep + strlen(name) + 1);
    if (!out) return NULL;
    if (need_sep) sprintf(out, "%s/%s", dir, name);
    else         sprintf(out, "%s%s", dir, name);
    return out;
}

int bs2_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && BS2_ISDIR(st.st_mode);
}

int bs2_list_dir(const char *dir, strlist *out) {
#if defined(_WIN32)
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        strlist_push(out, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        strlist_push(out, e->d_name);
    }
    closedir(d);
    return 0;
#endif
}
