/* Unit tests for util.c pure functions.
 * Build: cl /I..\..\src test_util.c ..\..\src\util.c /Fe:test_util.exe
 * Run:   test_util.exe
 */
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int pass = 0, fail = 0;

#define CK(name, cond) do { \
    if (cond) { pass++; printf("PASS: %s\n", name); } \
    else      { fail++; printf("FAIL: %s\n", name); } \
} while (0)

static void test_trim(void) {
    char s1[] = "  hello  ";
    CK("trim basic", strcmp(bs2_trim(s1), "hello") == 0);

    char s2[] = "\t\ttabbed\r\n";
    CK("trim whitespace", strcmp(bs2_trim(s2), "tabbed") == 0);

    char s3[] = "";
    CK("trim empty", strcmp(bs2_trim(s3), "") == 0);

    char s4[] = "   ";
    CK("trim all-space", strcmp(bs2_trim(s4), "") == 0);

    char s5[] = "nospace";
    CK("trim no-op", strcmp(bs2_trim(s5), "nospace") == 0);
}

static void test_crlf(void) {
    char s1[] = "line1\r\nline2\r\n";
    bs2_crlf_to_lf(s1);
    CK("crlf basic", strcmp(s1, "line1\nline2\n") == 0);

    char s2[] = "no crlf";
    bs2_crlf_to_lf(s2);
    CK("crlf no-op", strcmp(s2, "no crlf") == 0);

    char s3[] = "\r\n\r\n";
    bs2_crlf_to_lf(s3);
    CK("crlf all", strcmp(s3, "\n\n") == 0);

    char s4[] = "mixed\r\nlf\n";
    bs2_crlf_to_lf(s4);
    CK("crlf mixed", strcmp(s4, "mixed\nlf\n") == 0);
}

static void test_sbuf(void) {
    sbuf s;
    sbuf_init(&s);
    CK("sbuf init", s.len == 0 && s.data == NULL);

    sbuf_append(&s, "hello", 5);
    CK("sbuf append", s.len == 5 && strcmp(s.data, "hello") == 0);

    sbuf_append(&s, " world", 6);
    CK("sbuf append more", s.len == 11 && strcmp(s.data, "hello world") == 0);

    sbuf_appendf(&s, " %d", 42);
    CK("sbuf appendf", s.len == 14 && strcmp(s.data, "hello world 42") == 0);

    sbuf_free(&s);
    CK("sbuf free", s.data == NULL && s.cap == 0);
}

static void test_strlist(void) {
    strlist l;
    memset(&l, 0, sizeof(l));

    strlist_push(&l, "charlie");
    strlist_push(&l, "alpha");
    strlist_push(&l, "bravo");
    CK("strlist count", l.count == 3);

    /* sorted order: alpha, bravo, charlie */
    CK("strlist sorted", strcmp(l.items[0], "alpha") == 0 &&
                        strcmp(l.items[1], "bravo") == 0 &&
                        strcmp(l.items[2], "charlie") == 0);

    CK("strlist contains hit", strlist_contains(&l, "bravo"));
    CK("strlist contains miss", !strlist_contains(&l, "delta"));

    strlist_push(&l, "alpha"); /* duplicate */
    CK("strlist dedup", l.count == 3);

    strlist_free(&l);
    CK("strlist free", l.items == NULL && l.count == 0);
}

static void test_path_join(void) {
    char *p1 = bs2_path_join("/tmp", "file.txt");
    CK("path_join basic", strcmp(p1, "/tmp/file.txt") == 0);
    free(p1);

    char *p2 = bs2_path_join("/tmp/", "file.txt");
    CK("path_join trailing-slash", strcmp(p2, "/tmp/file.txt") == 0);
    free(p2);

    char *p3 = bs2_path_join("", "file.txt");
    CK("path_join empty-dir", strcmp(p3, "file.txt") == 0);
    free(p3);
}

static void test_basename(void) {
    CK("basename unix", strcmp(bs2_basename("/var/log/syslog"), "syslog") == 0);
    CK("basename win", strcmp(bs2_basename("C:\\Users\\test\\file.txt"), "file.txt") == 0);
    CK("basename no-path", strcmp(bs2_basename("file.txt"), "file.txt") == 0);
    CK("basename trailing-slash", strcmp(bs2_basename("/var/log/"), "") == 0);
}

static void test_dirname(void) {
    char *d1 = bs2_dirname("/var/log/syslog");
    CK("dirname basic", strcmp(d1, "/var/log") == 0);
    free(d1);

    char *d2 = bs2_dirname("file.txt");
    CK("dirname no-path", strcmp(d2, ".") == 0);
    free(d2);

    char *d3 = bs2_dirname("/file.txt");
    CK("dirname root", strcmp(d3, "/") == 0);
    free(d3);
}

static void test_read_file(void) {
    /* write a temp file then read it back */
    const char *tmp = "test_util_tmp.txt";
    FILE *f = fopen(tmp, "wb");
    CK("read_file create", f != NULL);
    if (f) {
        fwrite("hello\nworld\n", 1, 12, f);
        fclose(f);
    }

    size_t len;
    char *data = bs2_read_file(tmp, &len);
    CK("read_file len", len == 12);
    CK("read_file content", data && memcmp(data, "hello\nworld\n", 12) == 0);
    free(data);

    remove(tmp);
}

int main(void) {
    printf("== util unit tests ==\n");
    test_trim();
    test_crlf();
    test_sbuf();
    test_strlist();
    test_path_join();
    test_basename();
    test_dirname();
    test_read_file();
    printf("\nRESULT: %d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
