#ifndef BS2_UTIL_H
#define BS2_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ---- common size constants ---- */
#define BS2_MAX_HOST_LEN     256
#define BS2_MAX_PATH_LEN     1024
#define BS2_MAX_REMOTE_PATH  1536
#define BS2_ERRMSG_SZ        512
#define BS2_MAX_IDENTITY     1024

/* stat mode checks: MSVC ships S_ISDIR/S_ISREG as CRT functions that may not
 * link, while strict POSIX (_POSIX_C_SOURCE) hides the S_IF* masks — use
 * whatever each platform guarantees. */
#if defined(_WIN32)
#  define BS2_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#  define BS2_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#else
#  define BS2_ISDIR(m) S_ISDIR(m)
#  define BS2_ISREG(m) S_ISREG(m)
#endif

/* ---- growable byte buffer (also used as a string) ---- */
typedef struct {
    char  *data;   /* always NUL-terminated for convenience */
    size_t len;
    size_t cap;
} sbuf;

void sbuf_init(sbuf *s);
void sbuf_free(sbuf *s);
void sbuf_append(sbuf *s, const void *p, size_t n);
void sbuf_appendf(sbuf *s, const char *fmt, ...);

/* ---- list of malloc'd strings ---- */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strlist;

void strlist_push(strlist *l, const char *s);
int  strlist_contains(const strlist *l, const char *s);
void strlist_free(strlist *l);

/* read whole file; returns malloc'd NUL-terminated buffer, *out_len excludes
 * the NUL. returns NULL on failure. */
char *bs2_read_file(const char *path, size_t *out_len);

/* normalize CRLF -> LF in place, returns same pointer */
char *bs2_crlf_to_lf(char *s);

/* trim leading/trailing whitespace in place, returns pointer into s */
char *bs2_trim(char *s);

/* recursively create a directory (accepts '/' or '\' separators); 0 = ok */
int bs2_mkdirs(const char *path);

int64_t bs2_now_ms(void);

const char *bs2_basename(const char *path);
char *bs2_dirname(const char *path);                    /* malloc'd */
char *bs2_path_join(const char *dir, const char *name); /* malloc'd */

int bs2_is_dir(const char *path);
/* list immediate child entry names of a directory; 0 = ok */
int bs2_list_dir(const char *dir, strlist *out);

#endif
