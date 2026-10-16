/* batchssh2 - batch SSH command execution & file transfer (libssh2)
 *
 * STDOUT carries pure business data (text/json/csv); all progress,
 * diagnostics and verbose logs go to STDERR.
 */
#include "util.h"
#include "output.h"
#include "pool.h"
#include "ssh_worker.h"

#include <libssh2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#define BS2_VERSION "0.1.0"

typedef enum { ACT_NONE = 0, ACT_EXEC, ACT_UPLOAD, ACT_DOWNLOAD } action_t;

typedef struct {
    /* resolved options */
    strlist    hosts;
    const char *user;
    char       *password;      /* malloc'd / points into argv or env */
    int        port;
    int        forks;
    int        timeout;
    int        verbose;
    int        quiet;
    int        dry_run;
    int        json;
    int        csv;
    action_t   action;
    const char *known_hosts;
    const char *identity;
    const char *passphrase;

    char       *exec_cmd;      /* direct command (may be NULL) */
    char       *script_data;   /* script content, CRLF-normalized (may be NULL) */
    size_t     script_len;

    char *up_local, *up_remote;
    char *dl_remote, *dl_local;

    /* runtime */
    bs2_result *results;
    int        total;
} app_t;

/* ------------------------------------------------------------------ */
/* usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(FILE *out) {
    fputs(
"batchssh2 " BS2_VERSION " - batch SSH operations tool (libssh2)\n"
"\n"
"USAGE:\n"
"  batchssh2 -H <hosts> [AUTH] <ACTION> [CONTROL] [OUTPUT]\n"
"\n"
"TARGET:\n"
"  -H, --hosts <targets>     comma separated IPs or @file ('#' comments,\n"
"                            blank lines ignored, auto deduplicated)\n"
"AUTH:\n"
"  -u, --user <name>         SSH username (default: root)\n"
"  -p, --password <pwd>      password directly (visible in ps, not recommended)\n"
"                            or @file to read from file\n"
"      --port <port>         SSH port (default: 22)\n"
"                            If -p not given: reads password from terminal\n"
"                            (like ssh), or env BATCHSSH2_PWD\n"
"ACTION (mutually exclusive):\n"
"  -c, --command <cmd>       run shell command; '@file' sends a local script\n"
"                            through 'bash -s' (CRLF auto-converted to LF)\n"
"      --upload <local:remote>    push local file/dir to remote path\n"
"      --download <remote:local>  pull remote file/dir into local dir\n"
"                            (anti-overwrite: saved as <local>/<ip>/...)\n"
"AUTH:\n"
"  -i, --identity <file>     SSH private key file for public key auth\n"
"      --passphrase <pwd>    passphrase for the private key\n"
"SECURITY:\n"
"      --known-hosts <file>  verify host key against known_hosts file\n"
"CONTROL:\n"
"  -F, --forks <num>         parallel workers (default: 10)\n"
"  -T, --timeout <seconds>   per-host timeout (default: 30)\n"
"      --dry-run             print actions without connecting\n"
"      --quiet               suppress all stderr output\n"
"OUTPUT:\n"
"      --json                JSON array to stdout (progress suppressed)\n"
"      --csv                 CSV report to stdout (RFC 4180, BOM on Windows)\n"
"  -v, --verbose             debug log to stderr\n"
"\n"
"EXIT CODE: 0 all ok | 1 partial failure | 2 all failed | 3 usage error\n"
"\n"
"EXAMPLES:\n"
"  batchssh2 -H @iplist.txt -c \"uptime\"\n"
"  batchssh2 -H 10.0.0.1,10.0.0.2 -p @pwd.txt -c @check.sh --csv > r.csv\n"
"  batchssh2 -H @iplist.txt --upload ./app.bin:/opt/app.bin\n"
"  batchssh2 -H @iplist.txt --download /var/log/syslog:./logs/\n",
    out);
}

/* ------------------------------------------------------------------ */
/* password input (terminal echo-off, like ssh)                        */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
#include <conio.h>
static char *read_password_terminal(const char *prompt) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    char buf[256] = {0};
    int i = 0;
    int ch;
    while ((ch = _getch()) != '\r' && ch != '\n') {
        if (ch == '\b' || ch == 127) { /* backspace */
            if (i > 0) { i--; fprintf(stderr, "\b \b"); }
        } else if (ch == 3) { /* Ctrl+C */
            fprintf(stderr, "\n");
            exit(130);
        } else if (i < (int)sizeof(buf) - 1 && ch >= 32 && ch < 127) {
            buf[i++] = (char)ch;
            fputc('*', stderr);
        }
    }
    fprintf(stderr, "\n");
    return strdup(buf);
}
#else
#include <termios.h>
#include <unistd.h>
static char *read_password_terminal(const char *prompt) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    struct termios old, new;
    tcgetattr(STDIN_FILENO, &old);
    new = old;
    new.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new);
    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), stdin)) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        fprintf(stderr, "\n");
        return NULL;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    fprintf(stderr, "\n");
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return strdup(buf);
}
#endif

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int opt_val(const char *arg, const char *name, char **argv, int argc,
                   int *i, const char **val) {
    size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0) return 0;
    if (arg[n] == '=') { *val = arg + n + 1; return 1; }
    if (arg[n] == '\0') {
        if (*i + 1 >= argc) {
            fprintf(stderr, "error: %s requires a value\n", name);
            exit(3);
        }
        *val = argv[++(*i)];
        return 1;
    }
    return 0;
}

static void die_usage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n\n");
    va_end(ap);
    usage(stderr);
    exit(3);
}

/* add one token ("ip" or "@file") to the host list, deduplicated */
static int add_host_token(app_t *a, char *tok) {
    tok = bs2_trim(tok);
    if (!*tok || *tok == '#') return 0;

    if (*tok == '@') {
        size_t len;
        char *data = bs2_read_file(tok + 1, &len);
        if (!data) {
            fprintf(stderr, "error: cannot read host file '%s'\n", tok + 1);
            return -1;
        }
        char *p = data;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            char *t = bs2_trim(p);
            /* allow "ip # comment" and plain "# comment" lines */
            char *hash = strchr(t, '#');
            if (hash) *hash = '\0';
            t = bs2_trim(t);
            if (*t && !strlist_contains(&a->hosts, t)) strlist_push(&a->hosts, t);
            p = nl ? nl + 1 : NULL;
        }
        free(data);
        return 0;
    }

    if (!strlist_contains(&a->hosts, tok)) strlist_push(&a->hosts, tok);
    return 0;
}

static void add_hosts_spec(app_t *a, const char *spec) {
    char *buf = strdup(spec);
    if (!buf) die_usage("out of memory");
    char *p = buf;
    while (p && *p) {
        char *end = p;
        while (*end && *end != ',' && *end != ';' && *end != ' ' && *end != '\t') end++;
        char c = *end;
        *end = '\0';
        if (add_host_token(a, p) != 0) exit(3);
        p = c ? end + 1 : NULL;
    }
    free(buf);
}

static char *read_trimmed_file(const char *path, const char *what) {
    size_t len;
    char *data = bs2_read_file(path, &len);
    if (!data) die_usage("cannot read %s file '%s'", what, path);
    char *t = bs2_trim(data); /* trims trailing newline(s) of the secret */
    return t;
}

static int atomic_inc(int *p) {
#if defined(_WIN32)
    return InterlockedIncrement((volatile LONG *)p);
#else
    return __sync_add_and_fetch(p, 1);
#endif
}

/* ------------------------------------------------------------------ */
/* worker job                                                          */
/* ------------------------------------------------------------------ */

static int g_done = 0;
static int64_t g_start_ms = 0;
static volatile sig_atomic_t g_stop = 0;

static void job(void *ctx, int idx) {
    app_t *a = (app_t *)ctx;
    if (g_stop) {
        a->results[idx].status = BS2_FAIL;
        a->results[idx].time_ms = 0;
        snprintf(a->results[idx].error_msg, sizeof(a->results[idx].error_msg), "interrupted by signal");
        return;
    }
    bs2_ssh_opts o;
    memset(&o, 0, sizeof(o));
    o.host     = a->hosts.items[idx];
    o.user     = a->user;
    o.password = a->password;
    o.port     = a->port;
    o.timeout_s = a->timeout;
    o.verbose  = a->verbose;
    o.known_hosts = a->known_hosts;
    o.identity    = a->identity;
    o.passphrase  = a->passphrase;

    bs2_result *r = &a->results[idx];
    switch (a->action) {
    case ACT_EXEC:     bs2_ssh_exec(&o, a->exec_cmd, a->script_data, a->script_len, r); break;
    case ACT_UPLOAD:   bs2_ssh_upload(&o, a->up_local, a->up_remote, r);               break;
    case ACT_DOWNLOAD: bs2_ssh_download(&o, a->dl_remote, a->dl_local, r);             break;
    default: break;
    }

    if (!a->json && !a->quiet) { /* progress on STDERR only; never pollute stdout */
        int n = atomic_inc(&g_done);
        int64_t elapsed = bs2_now_ms() - g_start_ms;
        double avg_ms = n > 0 ? (double)elapsed / n : 0;
        int remaining = a->total - n;
        int eta_s = (int)(avg_ms * remaining / 1000.0);
        fprintf(stderr, "[%d/%d] %s: %s (exit=%d, %lld ms) eta=%ds%s%s\n",
                n, a->total, o.host, bs2_status_str(r->status), r->exit_code,
                (long long)r->time_ms, eta_s,
                r->error_msg[0] ? " | " : "",
                r->error_msg[0] ? r->error_msg : "");
        fflush(stderr);
    }
}

/* ------------------------------------------------------------------ */
/* config file                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *key;
    const char *val;
} cfg_entry;

static char *cfg_get(cfg_entry *entries, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (strcmp(entries[i].key, key) == 0) return (char *)entries[i].val;
    return NULL;
}

static int parse_config(const char *path, cfg_entry *entries, int max_entries) {
    size_t len;
    char *data = bs2_read_file(path, &len);
    if (!data) return -1;
    int n = 0;
    char *p = data;
    while (p && *p && n < max_entries) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char *line = bs2_trim(p);
        if (*line && *line != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                entries[n].key = bs2_trim(line);
                entries[n].val = bs2_trim(eq + 1);
                n++;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    free(data);
    return n;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    app_t a;
    memset(&a, 0, sizeof(a));
    a.user = "root";
    a.port = 22;
    a.forks = 10;
    a.timeout = 30;

    const char *user_arg = NULL, *pass_arg = NULL, *cmd_arg = NULL;
    const char *upload_arg = NULL, *download_arg = NULL;
    const char *forks_arg = NULL, *timeout_arg = NULL, *port_arg = NULL;
    const char *identity_arg = NULL, *passphrase_arg = NULL, *known_hosts_arg = NULL;
    const char *config_arg = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "--version") == 0) {
            printf("batchssh2 %s (libssh2 %s)\n", BS2_VERSION, libssh2_version(0));
            return 0;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            a.verbose = 1;
        } else if (strcmp(arg, "--json") == 0) {
            a.json = 1;
        } else if (strcmp(arg, "--csv") == 0) {
            a.csv = 1;
        } else if (opt_val(arg, "-H", argv, argc, &i, &val) ||
                   opt_val(arg, "--hosts", argv, argc, &i, &val)) {
            add_hosts_spec(&a, val); /* repeated -H accumulate */
        } else if (opt_val(arg, "-u", argv, argc, &i, &val) ||
                   opt_val(arg, "--user", argv, argc, &i, &val)) {
            user_arg = val;
        } else if (opt_val(arg, "-p", argv, argc, &i, &val) ||
                   opt_val(arg, "--password", argv, argc, &i, &val)) {
            pass_arg = val;
        } else if (opt_val(arg, "--port", argv, argc, &i, &val)) {
            port_arg = val;
        } else if (opt_val(arg, "-c", argv, argc, &i, &val) ||
                   opt_val(arg, "--command", argv, argc, &i, &val)) {
            cmd_arg = val;
        } else if (opt_val(arg, "--upload", argv, argc, &i, &val)) {
            upload_arg = val;
        } else if (opt_val(arg, "--download", argv, argc, &i, &val)) {
            download_arg = val;
        } else if (opt_val(arg, "-i", argv, argc, &i, &val) ||
                   opt_val(arg, "--identity", argv, argc, &i, &val)) {
            identity_arg = val;
        } else if (opt_val(arg, "--passphrase", argv, argc, &i, &val)) {
            passphrase_arg = val;
        } else if (opt_val(arg, "--known-hosts", argv, argc, &i, &val)) {
            known_hosts_arg = val;
        } else if (opt_val(arg, "--config", argv, argc, &i, &val)) {
            config_arg = val;
        } else if (strcmp(arg, "--dry-run") == 0) {
            a.dry_run = 1;
        } else if (strcmp(arg, "--quiet") == 0) {
            a.quiet = 1;
        } else if (opt_val(arg, "-F", argv, argc, &i, &val) ||
                   opt_val(arg, "--forks", argv, argc, &i, &val)) {
            forks_arg = val;
        } else if (opt_val(arg, "-T", argv, argc, &i, &val) ||
                   opt_val(arg, "--timeout", argv, argc, &i, &val)) {
            timeout_arg = val;
        } else {
            die_usage("unknown argument '%s'", arg);
        }
    }

    /* ---- config file (connection/run defaults only; CLI overrides) ----
     * Deliberately NOT configurable here: the targets (-H) and the action
     * (-c / --upload / --download). Those define *this run's task*, so a
     * default for them would be meaningless - they must be given on the
     * command line every time. */
    if (config_arg) {
        cfg_entry entries[32];
        int n = parse_config(config_arg, entries, 32);
        if (n < 0) die_usage("cannot read config file '%s'", config_arg);
        if (!user_arg && cfg_get(entries, n, "user"))     user_arg = cfg_get(entries, n, "user");
        if (!pass_arg && cfg_get(entries, n, "password"))  pass_arg = cfg_get(entries, n, "password");
        if (!port_arg && cfg_get(entries, n, "port"))     port_arg = cfg_get(entries, n, "port");
        if (!forks_arg && cfg_get(entries, n, "forks"))   forks_arg = cfg_get(entries, n, "forks");
        if (!timeout_arg && cfg_get(entries, n, "timeout")) timeout_arg = cfg_get(entries, n, "timeout");
        if (!identity_arg && cfg_get(entries, n, "identity")) identity_arg = cfg_get(entries, n, "identity");
        if (!known_hosts_arg && cfg_get(entries, n, "known_hosts")) known_hosts_arg = cfg_get(entries, n, "known_hosts");
    }

    /* ---- resolve options ---- */
    if (user_arg) a.user = user_arg;
    if (identity_arg) a.identity = identity_arg;
    if (passphrase_arg) a.passphrase = passphrase_arg;
    if (known_hosts_arg) a.known_hosts = known_hosts_arg;
    if (port_arg) {
        a.port = atoi(port_arg);
        if (a.port <= 0 || a.port > 65535) die_usage("invalid port '%s'", port_arg);
    }
    if (forks_arg) {
        a.forks = atoi(forks_arg);
        if (a.forks < 1 || a.forks > 512) die_usage("invalid forks '%s'", forks_arg);
    }
    if (timeout_arg) {
        a.timeout = atoi(timeout_arg);
        if (a.timeout < 1 || a.timeout > 86400) die_usage("invalid timeout '%s'", timeout_arg);
    }

    /* password: -p value | -p @file | env BATCHSSH2_PWD | interactive terminal */
    if (pass_arg) {
        if (pass_arg[0] == '@')
            a.password = read_trimmed_file(pass_arg + 1, "password");
        else
            a.password = strdup(pass_arg);
    } else {
        const char *env = getenv("BATCHSSH2_PWD");
        if (env && *env) {
            a.password = strdup(env);
        } else {
            /* interactive terminal input (like ssh) */
            a.password = read_password_terminal("password: ");
            if (!a.password || !a.password[0])
                die_usage("no password entered");
        }
    }

    /* ---- action (mutually exclusive) ---- */
    {
        int nact = (cmd_arg ? 1 : 0) + (upload_arg ? 1 : 0) + (download_arg ? 1 : 0);
        if (nact == 0) die_usage("no action given (need -c, --upload or --download)");
        if (nact > 1)  die_usage("-c, --upload and --download are mutually exclusive");
    }
    if (a.json && a.csv) die_usage("--json and --csv are mutually exclusive");

    if (cmd_arg) {
        a.action = ACT_EXEC;
        if (cmd_arg[0] == '@') {
            size_t len;
            char *data = bs2_read_file(cmd_arg + 1, &len);
            if (!data) die_usage("cannot read script file '%s'", cmd_arg + 1);
            bs2_crlf_to_lf(data); /* Windows CRLF would break remote bash */
            a.script_data = data;
            a.script_len = strlen(data);
            if (a.script_len == 0) die_usage("script file '%s' is empty", cmd_arg + 1);
        } else {
            a.exec_cmd = strdup(cmd_arg);
        }
    } else if (upload_arg) {
        a.action = ACT_UPLOAD;
        const char *colon = strrchr(upload_arg, ':'); /* LAST ':' (local may be D:\...) */
        if (!colon || colon == upload_arg || !colon[1])
            die_usage("--upload expects <local:remote>, got '%s'", upload_arg);
        size_t llen = (size_t)(colon - upload_arg);
        a.up_local = (char *)malloc(llen + 1);
        a.up_remote = strdup(colon + 1);
        if (!a.up_local || !a.up_remote) die_usage("out of memory");
        memcpy(a.up_local, upload_arg, llen);
        a.up_local[llen] = '\0';
        struct stat st;
        if (stat(a.up_local, &st) != 0)
            die_usage("local path '%s' does not exist", a.up_local);
    } else if (download_arg) {
        a.action = ACT_DOWNLOAD;
        const char *colon = strchr(download_arg, ':'); /* FIRST ':' (remote is POSIX) */
        if (!colon || colon == download_arg || !colon[1])
            die_usage("--download expects <remote:local_dir>, got '%s'", download_arg);
        size_t rlen = (size_t)(colon - download_arg);
        a.dl_remote = (char *)malloc(rlen + 1);
        a.dl_local = strdup(colon + 1);
        if (!a.dl_remote || !a.dl_local) die_usage("out of memory");
        memcpy(a.dl_remote, download_arg, rlen);
        a.dl_remote[rlen] = '\0';
    }

    if (a.hosts.count == 0) die_usage("no target hosts (use -H <ips> or -H @file)");
    a.total = (int)a.hosts.count;

    /* ---- dry-run mode ---- */
    if (a.dry_run) {
        const char *act = a.action == ACT_EXEC ? "exec"
                         : a.action == ACT_UPLOAD ? "upload" : "download";
        fprintf(stderr, "dry-run: %d host(s), action=%s, forks=%d, timeout=%ds\n",
                a.total, act, a.forks, a.timeout);
        for (int i = 0; i < a.total; i++)
            fprintf(stderr, "  would %s on %s\n", act, a.hosts.items[i]);
        return 0;
    }

    /* ---- run ---- */
    g_start_ms = bs2_now_ms();
    if (bs2_net_init() != 0) { fprintf(stderr, "error: WSAStartup failed\n"); return 3; }
    if (libssh2_init(0) != 0) { fprintf(stderr, "error: libssh2_init failed\n"); return 3; }

    a.results = (bs2_result *)calloc(a.total, sizeof(bs2_result));
    if (!a.results) { fprintf(stderr, "error: out of memory\n"); return 3; }
    for (int i = 0; i < a.total; i++)
        bs2_result_reset(&a.results[i], a.hosts.items[i]);

    if (!a.json && !a.quiet) {
        const char *act = a.action == ACT_EXEC ? "exec"
                        : a.action == ACT_UPLOAD ? "upload" : "download";
        fprintf(stderr, "batchssh2: %d host(s), action=%s, forks=%d, timeout=%ds\n",
                a.total, act, a.forks, a.timeout);
        fflush(stderr);
    }

    bs2_pool_run(job, &a, a.total, a.forks);

    /* ---- report ---- */
    if (a.json)      out_emit_json(a.results, (size_t)a.total);
    else if (a.csv)  out_emit_csv(a.results, (size_t)a.total);
    else             out_emit_text(a.results, (size_t)a.total);

    int ok = 0, bad = 0;
    for (int i = 0; i < a.total; i++) {
        if (a.results[i].status == BS2_OK) ok++;
        else bad++;
        bs2_result_free(&a.results[i]);
    }
    free(a.results);

    if (!a.json && !a.quiet)
        fprintf(stderr, "batchssh2: %d ok, %d failed\n", ok, bad);

    libssh2_exit();
    bs2_net_cleanup();
    return bad == 0 ? 0 : (ok == 0 ? 2 : 1);
}
