#include "ssh_worker.h"
#include "util.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define BS2_BAD_SOCKET INVALID_SOCKET
#  define bs2_closesock(s) closesocket(s)
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <fcntl.h>
   typedef int sock_t;
#  define BS2_BAD_SOCKET (-1)
#  define bs2_closesock(s) close(s)
#endif

#define BS2_CHUNK 32768

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

int bs2_net_init(void) {
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void bs2_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static void wlog(const bs2_ssh_opts *o, const char *fmt, ...) {
    if (!o->verbose) return;
    fprintf(stderr, "[verbose][%s] ", o->host);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int sess_again(LIBSSH2_SESSION *s) {
    return libssh2_session_last_errno(s) == LIBSSH2_ERROR_EAGAIN;
}

static void sess_err(LIBSSH2_SESSION *s, char *err, size_t errsz, const char *ctx) {
    char *m = NULL;
    int l = 0;
    libssh2_session_last_error(s, &m, &l, 0);
    snprintf(err, errsz, "%s: %.*s", ctx, l, m ? m : "?");
}

/* ------------------------------------------------------------------ */
/* TCP connect with hard timeout (non-blocking connect + select)       */
/* ------------------------------------------------------------------ */

static int sock_set_nb(sock_t s, int nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static sock_t tcp_connect(const char *host, int port, int timeout_ms,
                          char *err, size_t errsz) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, errsz, "cannot resolve '%.240s'", host);
        return BS2_BAD_SOCKET;
    }

    sock_t best = BS2_BAD_SOCKET;
    for (struct addrinfo *ai = res; ai && best == BS2_BAD_SOCKET; ai = ai->ai_next) {
        sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BS2_BAD_SOCKET) continue;
        if (sock_set_nb(s, 1) != 0) { bs2_closesock(s); continue; }

        int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc != 0) {
#ifdef _WIN32
            int in_progress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            int in_progress = errno == EINPROGRESS;
#endif
            if (!in_progress) { bs2_closesock(s); continue; }

            fd_set wf, ef;
            FD_ZERO(&wf);
            FD_ZERO(&ef);
            FD_SET(s, &wf);
            FD_SET(s, &ef);
            struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            if (select((int)s + 1, NULL, &wf, &ef, &tv) <= 0) {
                bs2_closesock(s);
                continue; /* timeout or connect error */
            }
            int soerr = 0;
            socklen_t slen = (socklen_t)sizeof(soerr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen) != 0 ||
                soerr != 0) {
                bs2_closesock(s);
                continue;
            }
        }
        sock_set_nb(s, 0);
        best = s;
    }
    freeaddrinfo(res);

    if (best == BS2_BAD_SOCKET)
        snprintf(err, errsz, "connect to %s:%d failed (timeout after %d ms)",
                 host, port, timeout_ms);
    return best;
}

/* sleep until the session says its socket is readable/writable again */
static int wait_socket(LIBSSH2_SESSION *sess, sock_t sock, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int dir = libssh2_session_block_directions(sess);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)  FD_SET(sock, &rd);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(sock, &wr);
    if (!FD_ISSET(sock, &rd) && !FD_ISSET(sock, &wr)) FD_SET(sock, &rd);
    return select((int)sock + 1, &rd, &wr, NULL, &tv);
}

/* ------------------------------------------------------------------ */
/* connection: handshake + dual auth fallback                          */
/* ------------------------------------------------------------------ */

typedef struct {
    sock_t           sock;
    LIBSSH2_SESSION *sess;
    LIBSSH2_SFTP    *sftp;
} conn_t;

/* keyboard-interactive callback: answer every prompt with the password.
 * NOTE: response strings must be heap-allocated — libssh2 frees them
 * itself when the kbd-int state advances (double free otherwise). */
static void kbd_cb(const char *name, int name_len,
                   const char *instruction, int instruction_len,
                   int num_prompts,
                   const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                   LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                   void **abstract) {
    (void)name; (void)name_len;
    (void)instruction; (void)instruction_len;
    (void)prompts;

    const char *pwd = (const char *)(*abstract);
    size_t n = pwd ? strlen(pwd) : 0;
    for (int i = 0; i < num_prompts; i++) {
        char *a = (char *)malloc(n + 1);
        if (!a) { responses[i].text = NULL; responses[i].length = 0; continue; }
        memcpy(a, pwd, n + 1);
        responses[i].text = a;
        responses[i].length = (unsigned int)n;
    }
}

static int ssh_auth(LIBSSH2_SESSION *sess, const bs2_ssh_opts *o,
                    char *err, size_t errsz) {
    /* 1) public key auth (if identity file is provided) */
    if (o->identity) {
        int rc = libssh2_userauth_publickey_fromfile(sess, o->user,
                                                       NULL, o->identity, o->passphrase);
        if (rc == 0) return 0;
    }

    /* 2) plain password */
    if (libssh2_userauth_password(sess, o->user, o->password) == 0)
        return 0;

    /* 3) PAM-style servers often disable "password": retry the same
     *    secret through keyboard-interactive */
    *libssh2_session_abstract(sess) = (void *)o->password;
    int rc = libssh2_userauth_keyboard_interactive(sess, o->user, kbd_cb);
    *libssh2_session_abstract(sess) = NULL;
    if (rc == 0) return 0;

    sess_err(sess, err, errsz, "authentication failed (password & keyboard-interactive)");
    return -1;
}

static void conn_close(conn_t *c) {
    if (c->sftp) { libssh2_sftp_shutdown(c->sftp); c->sftp = NULL; }
    if (c->sess) {
        libssh2_session_disconnect(c->sess, "batchssh2 done");
        libssh2_session_free(c->sess);
        c->sess = NULL;
    }
    if (c->sock != BS2_BAD_SOCKET) { bs2_closesock(c->sock); c->sock = BS2_BAD_SOCKET; }
}

/* note: host keys are NOT verified (managed fleets usually share one key) */
static int conn_open(conn_t *c, const bs2_ssh_opts *o, int64_t deadline,
                     char *err, size_t errsz) {
    c->sock = BS2_BAD_SOCKET;
    c->sess = NULL;
    c->sftp = NULL;

    sock_t s = tcp_connect(o->host, o->port, o->timeout_s * 1000, err, errsz);
    if (s == BS2_BAD_SOCKET) return -1;
    c->sock = s;

    c->sess = libssh2_session_init();
    if (!c->sess) {
        snprintf(err, errsz, "libssh2_session_init failed (out of memory)");
        conn_close(c);
        return -1;
    }
    /* hard ceiling for every blocking libssh2 call on this session */
    libssh2_session_set_timeout(c->sess, o->timeout_s * 1000);
    libssh2_session_set_blocking(c->sess, 1);

    for (;;) {
        int rc = libssh2_session_handshake(c->sess, s);
        if (rc == 0) break;
        if (!sess_again(c->sess)) { sess_err(c->sess, err, errsz, "ssh handshake failed"); goto fail; }
        if (bs2_now_ms() > deadline) { snprintf(err, errsz, "handshake timed out"); goto fail; }
        wait_socket(c->sess, c->sock, 1000);
    }
    wlog(o, "handshake ok");

    /* verify host key against known_hosts file if provided */
    if (o->known_hosts) {
        LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(c->sess);
        if (!hosts) {
            snprintf(err, errsz, "known_hosts: init failed");
            goto fail;
        }
        int kh_rc = libssh2_knownhost_readfile(hosts, o->known_hosts,
                                                LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        if (kh_rc < 0) {
            snprintf(err, errsz, "known_hosts: cannot read '%s'", o->known_hosts);
            libssh2_knownhost_free(hosts);
            goto fail;
        }
        const char *key = NULL;
        size_t keylen = 0;
        int keytype = 0;
        libssh2_session_hostkey(c->sess, &keylen, &keytype);
        (void)key;
        int check = libssh2_knownhost_check(hosts, o->host, key, (size_t)keylen,
                                             LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                                             LIBSSH2_KNOWNHOST_KEYENC_RAW,
                                             NULL);
        libssh2_knownhost_free(hosts);
        if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            snprintf(err, errsz, "known_hosts: host key verification failed for %s", o->host);
            goto fail;
        }
        wlog(o, "host key verified against %s", o->known_hosts);
    }

    if (ssh_auth(c->sess, o, err, errsz) != 0) goto fail;
    wlog(o, "authenticated as '%.240s'", o->user);
    return 0;

fail:
    conn_close(c);
    return -1;
}

/* ------------------------------------------------------------------ */
/* remote command execution                                            */
/* ------------------------------------------------------------------ */

static int drain(LIBSSH2_CHANNEL *ch, sbuf *out, sbuf *errb) {
    char buf[BS2_CHUNK];
    int got = 0;
    for (;;) {
        ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n <= 0) break;
        sbuf_append(out, buf, (size_t)n);
        got += (int)n;
    }
    for (;;) {
        ssize_t n = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
        if (n <= 0) break;
        sbuf_append(errb, buf, (size_t)n);
        got += (int)n;
    }
    return got;
}

void bs2_ssh_exec(const bs2_ssh_opts *o, const char *cmd,
                  const char *stdin_data, size_t stdin_len, bs2_result *r) {
    int64_t t0 = bs2_now_ms();
    int64_t deadline = t0 + (int64_t)o->timeout_s * 1000;
    char err[512] = "";
    conn_t c;
    LIBSSH2_CHANNEL *ch = NULL;

    if (conn_open(&c, o, deadline, err, sizeof(err)) != 0)
        goto finish_fail;

    for (;;) {
        ch = libssh2_channel_open_session(c.sess);
        if (ch) break;
        if (!sess_again(c.sess)) { sess_err(c.sess, err, sizeof(err), "open channel failed"); goto finish_fail; }
        if (bs2_now_ms() > deadline) { snprintf(err, sizeof(err), "timed out opening channel"); goto finish_fail; }
        wait_socket(c.sess, c.sock, 1000);
    }

    {
        const char *exe = stdin_data ? "bash -s" : cmd;
        wlog(o, "exec: %s", stdin_data ? "bash -s (script via stdin)" : exe);
        for (;;) {
            int rc = libssh2_channel_exec(ch, exe);
            if (rc == 0) break;
            if (rc != LIBSSH2_ERROR_EAGAIN) { sess_err(c.sess, err, sizeof(err), "exec failed"); goto finish_fail; }
            if (bs2_now_ms() > deadline) { snprintf(err, sizeof(err), "timed out starting command"); goto finish_fail; }
            wait_socket(c.sess, c.sock, 1000);
        }
    }

    /* non-blocking pump: interleave stdin feeding with output draining so a
     * chatty script can never deadlock against a large stdin payload */
    libssh2_channel_set_blocking(ch, 0);
    {
        size_t sent = 0;
        int eof_sent = 0;
        int remote_eof = 0;

        for (;;) {
            int got = drain(ch, &r->out, &r->err);
            if (!remote_eof && libssh2_channel_eof(ch)) remote_eof = 1;
            if (remote_eof && got == 0) break;

            if (stdin_data) {
                if (sent < stdin_len) {
                    size_t want = stdin_len - sent;
                    if (want > BS2_CHUNK) want = BS2_CHUNK;
                    ssize_t n = libssh2_channel_write(ch, stdin_data + sent, want);
                    if (n > 0) sent += (size_t)n;
                } else if (!eof_sent) {
                    if (libssh2_channel_send_eof(ch) == 0) eof_sent = 1;
                }
            }

            if (bs2_now_ms() > deadline) {
                snprintf(err, sizeof(err), "timed out after %ds", o->timeout_s);
                libssh2_channel_free(ch);
                conn_close(&c);
                r->status = BS2_TIMEOUT;
                snprintf(r->error_msg, sizeof(r->error_msg), "%s", err);
                r->time_ms = bs2_now_ms() - t0;
                return;
            }
            wait_socket(c.sess, c.sock, 1000);
        }
    }

    libssh2_channel_set_blocking(ch, 1);
    r->exit_code = libssh2_channel_get_exit_status(ch);
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    conn_close(&c);

    r->status = BS2_OK;
    r->time_ms = bs2_now_ms() - t0;
    return;

finish_fail:
    if (ch) libssh2_channel_free(ch);
    conn_close(&c);
    r->status = (bs2_now_ms() > deadline) ? BS2_TIMEOUT : BS2_FAIL;
    snprintf(r->error_msg, sizeof(r->error_msg), "%s", err);
    r->time_ms = bs2_now_ms() - t0;
}

/* ------------------------------------------------------------------ */
/* SFTP transfer                                                       */
/* ------------------------------------------------------------------ */

static int ensure_sftp(conn_t *c, int64_t deadline, char *err, size_t errsz) {
    if (c->sftp) return 0;
    for (;;) {
        c->sftp = libssh2_sftp_init(c->sess);
        if (c->sftp) return 0;
        if (!sess_again(c->sess)) { sess_err(c->sess, err, errsz, "sftp init failed"); return -1; }
        if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out starting sftp"); return -1; }
        wait_socket(c->sess, c->sock, 1000);
    }
}

static int sftp_is_dir(conn_t *c, const char *path, int64_t deadline) {
    LIBSSH2_SFTP_ATTRIBUTES st;
    for (;;) {
        int rc = libssh2_sftp_stat(c->sftp, path, &st);
        if (rc == 0)
            return (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && BS2_ISDIR(st.permissions);
        if (!sess_again(c->sess)) return 0;
        if (bs2_now_ms() > deadline) return 0;
        wait_socket(c->sess, c->sock, 1000);
    }
}

static int sftp_mkdir(conn_t *c, const char *path, int64_t deadline) {
    for (;;) {
        int rc = libssh2_sftp_mkdir(c->sftp, path, 0755);
        if (rc == 0) return 0;
        if (sess_again(c->sess)) {
            if (bs2_now_ms() <= deadline) { wait_socket(c->sess, c->sock, 1000); continue; }
            return -1;
        }
        /* exists already (some servers answer FAILURE instead of EXISTS) */
        if (sftp_is_dir(c, path, deadline)) return 0;
        return -1;
    }
}

static int sftp_mkdirs(conn_t *c, const char *path, int64_t deadline) {
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (p != tmp + 1) sftp_mkdir(c, tmp, deadline); /* parents may exist */
            *p = '/';
        }
    }
    return sftp_mkdir(c, tmp, deadline);
}

static int sftp_push_file(conn_t *c, const char *local, const char *remote,
                          int64_t deadline, unsigned long long *total,
                          char *err, size_t errsz) {
    /* create the remote parent directory chain if it does not exist yet */
    const char *slash = strrchr(remote, '/');
    if (slash && slash != remote) {
        size_t plen = (size_t)(slash - remote);
        char parent[1024];
        if (plen >= sizeof(parent)) { snprintf(err, errsz, "remote path too long"); return -1; }
        memcpy(parent, remote, plen);
        parent[plen] = '\0';
        if (sftp_mkdirs(c, parent, deadline) != 0) {
            snprintf(err, errsz, "cannot create remote directory '%.240s'", parent);
            return -1;
        }
    }

    FILE *lf = fopen(local, "rb");
    if (!lf) { snprintf(err, errsz, "cannot open local file '%.240s'", local); return -1; }

    LIBSSH2_SFTP_HANDLE *h = NULL;
    for (;;) {
        h = libssh2_sftp_open(c->sftp, remote,
                              LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                              LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                              LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
        if (h) break;
        if (!sess_again(c->sess)) { sess_err(c->sess, err, errsz, "sftp open (write) failed"); fclose(lf); return -1; }
        if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out opening remote '%.240s'", remote); fclose(lf); return -1; }
        wait_socket(c->sess, c->sock, 1000);
    }

    char buf[BS2_CHUNK];
    unsigned long long bytes = 0;
    size_t nread;
    int failed = 0;
    while (!failed && (nread = fread(buf, 1, sizeof(buf), lf)) > 0) {
        size_t off = 0;
        while (off < nread) {
            ssize_t n = libssh2_sftp_write(h, buf + off, nread - off);
            if (n > 0) { off += (size_t)n; bytes += (unsigned long long)n; continue; }
            if (n == LIBSSH2_ERROR_EAGAIN) {
                if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out writing '%.240s'", remote); failed = 1; break; }
                wait_socket(c->sess, c->sock, 1000);
                continue;
            }
            sess_err(c->sess, err, errsz, "sftp write failed");
            failed = 1;
            break;
        }
    }
    fclose(lf);
    while (libssh2_sftp_close(h) == LIBSSH2_ERROR_EAGAIN)
        if (bs2_now_ms() > deadline) break;
        else wait_socket(c->sess, c->sock, 1000);

    if (!failed && total) *total += bytes;
    return failed ? -1 : 0;
}

static int sftp_push_tree(conn_t *c, const char *local, const char *remote,
                          int64_t deadline, unsigned long long *total, int *nfiles,
                          char *err, size_t errsz) {
    if (!bs2_is_dir(local)) {
        if (sftp_push_file(c, local, remote, deadline, total, err, errsz) != 0) return -1;
        (*nfiles)++;
        return 0;
    }

    if (sftp_mkdirs(c, remote, deadline) != 0) {
        snprintf(err, errsz, "cannot create remote directory '%.240s'", remote);
        return -1;
    }
    strlist names;
    memset(&names, 0, sizeof(names));
    if (bs2_list_dir(local, &names) != 0) {
        snprintf(err, errsz, "cannot list local directory '%.240s'", local);
        strlist_free(&names);
        return -1;
    }
    for (size_t i = 0; i < names.count; i++) {
        char *sub_local = bs2_path_join(local, names.items[i]);
        char *sub_remote = bs2_path_join(remote, names.items[i]);
        int rc = sftp_push_tree(c, sub_local, sub_remote, deadline, total, nfiles, err, errsz);
        free(sub_local);
        free(sub_remote);
        if (rc != 0) { strlist_free(&names); return -1; }
    }
    strlist_free(&names);
    return 0;
}

static int sftp_pull_file(conn_t *c, const char *remote, const char *local,
                          int64_t deadline, unsigned long long *total,
                          char *err, size_t errsz) {
    /* make sure <local_dir>/<host>/ exists before creating the file in it */
    char *parent = bs2_dirname(local);
    if (parent && bs2_mkdirs(parent) != 0) {
        snprintf(err, errsz, "cannot create directory '%.240s'", parent);
        free(parent);
        return -1;
    }
    free(parent);

    LIBSSH2_SFTP_HANDLE *h = NULL;
    for (;;) {
        h = libssh2_sftp_open(c->sftp, remote, LIBSSH2_FXF_READ, 0);
        if (h) break;
        if (!sess_again(c->sess)) { sess_err(c->sess, err, errsz, "sftp open (read) failed"); return -1; }
        if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out opening remote '%.240s'", remote); return -1; }
        wait_socket(c->sess, c->sock, 1000);
    }

    FILE *lf = fopen(local, "wb");
    if (!lf) {
        snprintf(err, errsz, "cannot create local file '%.240s'", local);
        libssh2_sftp_close(h);
        return -1;
    }

    char buf[BS2_CHUNK];
    unsigned long long bytes = 0;
    int failed = 0;
    for (;;) {
        ssize_t n = libssh2_sftp_read(h, buf, sizeof(buf));
        if (n > 0) {
            if (fwrite(buf, 1, (size_t)n, lf) != (size_t)n) {
                snprintf(err, errsz, "local write error on '%.240s'", local);
                failed = 1;
                break;
            }
            bytes += (unsigned long long)n;
            continue;
        }
        if (n == 0) break; /* EOF */
        if (n == LIBSSH2_ERROR_EAGAIN) {
            if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out reading '%.240s'", remote); failed = 1; break; }
            wait_socket(c->sess, c->sock, 1000);
            continue;
        }
        sess_err(c->sess, err, errsz, "sftp read failed");
        failed = 1;
        break;
    }
    fclose(lf);
    while (libssh2_sftp_close(h) == LIBSSH2_ERROR_EAGAIN)
        if (bs2_now_ms() > deadline) break;
        else wait_socket(c->sess, c->sock, 1000);

    if (failed) remove(local);
    else if (total) *total += bytes;
    return failed ? -1 : 0;
}

static int sftp_pull_tree(conn_t *c, const char *remote, const char *local,
                          int64_t deadline, unsigned long long *total, int *nfiles,
                          char *err, size_t errsz) {
    if (!sftp_is_dir(c, remote, deadline))
        return sftp_pull_file(c, remote, local, deadline, total, err, errsz);


    if (bs2_mkdirs(local) != 0) {
        snprintf(err, errsz, "cannot create local directory '%.240s'", local);
        return -1;
    }

    LIBSSH2_SFTP_HANDLE *dh = NULL;
    for (;;) {
        dh = libssh2_sftp_opendir(c->sftp, remote);
        if (dh) break;
        if (!sess_again(c->sess)) { sess_err(c->sess, err, errsz, "sftp opendir failed"); return -1; }
        if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out opening remote dir '%.240s'", remote); return -1; }
        wait_socket(c->sess, c->sock, 1000);
    }

    int rc, failed = 0;
    do {
        char fname[512];
        LIBSSH2_SFTP_ATTRIBUTES fst;
        rc = libssh2_sftp_readdir(dh, fname, sizeof(fname), &fst);
        if (rc > 0) {
            if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) continue;
            char *sub_remote = bs2_path_join(remote, fname);
            char *sub_local = bs2_path_join(local, fname);
            failed = sftp_pull_tree(c, sub_remote, sub_local, deadline, total, nfiles, err, errsz) != 0;
            free(sub_remote);
            free(sub_local);
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (bs2_now_ms() > deadline) { snprintf(err, errsz, "timed out listing '%.240s'", remote); failed = 1; break; }
            wait_socket(c->sess, c->sock, 1000);
        }
    } while (rc > 0 && !failed);
    while (libssh2_sftp_closedir(dh) == LIBSSH2_ERROR_EAGAIN)
        if (bs2_now_ms() > deadline) break;
        else wait_socket(c->sess, c->sock, 1000);
    return failed ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* top-level actions                                                   */
/* ------------------------------------------------------------------ */

void bs2_ssh_upload(const bs2_ssh_opts *o, const char *local,
                    const char *remote, bs2_result *r) {
    int64_t t0 = bs2_now_ms();
    int64_t deadline = t0 + (int64_t)o->timeout_s * 1000;
    char err[512] = "";
    conn_t c;

    if (conn_open(&c, o, deadline, err, sizeof(err)) != 0)
        goto finish;
    if (ensure_sftp(&c, deadline, err, sizeof(err)) != 0)
        goto finish;

    wlog(o, "upload %s -> %s", local, remote);
    {
        unsigned long long total = 0;
        int nfiles = 0;
        int rc = sftp_push_tree(&c, local, remote, deadline, &total, &nfiles, err, sizeof(err));
        conn_close(&c);
        if (rc == 0) {
            sbuf_appendf(&r->out, "uploaded %d file(s), %llu bytes to %s\n",
                         nfiles, total, remote);
            r->status = BS2_OK;
        } else {
            goto finish_n;
        }
    }
    r->time_ms = bs2_now_ms() - t0;
    return;

finish:
    conn_close(&c);
finish_n:
    r->status = (bs2_now_ms() > deadline) ? BS2_TIMEOUT : BS2_FAIL;
    snprintf(r->error_msg, sizeof(r->error_msg), "%s", err);
    r->time_ms = bs2_now_ms() - t0;
}

void bs2_ssh_download(const bs2_ssh_opts *o, const char *remote,
                      const char *local_dir, bs2_result *r) {
    int64_t t0 = bs2_now_ms();
    int64_t deadline = t0 + (int64_t)o->timeout_s * 1000;
    char err[512] = "";
    conn_t c;

    /* normalize remote: strip trailing slashes so basename() works */
    char rpath[1024];
    snprintf(rpath, sizeof(rpath), "%s", remote);
    size_t rl = strlen(rpath);
    while (rl > 1 && rpath[rl - 1] == '/') rpath[--rl] = '\0';

    if (conn_open(&c, o, deadline, err, sizeof(err)) != 0)
        goto finish;
    if (ensure_sftp(&c, deadline, err, sizeof(err)) != 0)
        goto finish;

    wlog(o, "download %s -> %s", rpath, local_dir);
    {
        /* anti-overwrite layout: <local_dir>/<host>/<basename> */
        char target[1536];
        snprintf(target, sizeof(target), "%s/%s/%s",
                 local_dir, o->host, bs2_basename(rpath));

        unsigned long long total = 0;
        int nfiles = 0;
        int rc = sftp_pull_tree(&c, rpath, target, deadline, &total, &nfiles, err, sizeof(err));
        conn_close(&c);
        if (rc == 0) {
            sbuf_appendf(&r->out, "downloaded %d file(s), %llu bytes to %s\n",
                         nfiles, total, target);
            r->status = BS2_OK;
        } else {
            goto finish_n;
        }
    }
    r->time_ms = bs2_now_ms() - t0;
    return;

finish:
    conn_close(&c);
finish_n:
    r->status = (bs2_now_ms() > deadline) ? BS2_TIMEOUT : BS2_FAIL;
    snprintf(r->error_msg, sizeof(r->error_msg), "%s", err);
    r->time_ms = bs2_now_ms() - t0;
}
