#ifndef BS2_SSH_WORKER_H
#define BS2_SSH_WORKER_H

#include "output.h"

typedef struct {
    const char *host;
    const char *user;
    const char *password;
    int         port;
    int         timeout_s;   /* per-device total budget (connect+auth+action) */
    int         verbose;     /* log steps to STDERR */
    const char *known_hosts; /* path to known_hosts file (NULL = no verification) */
    const char *identity;    /* path to private key file (NULL = password auth) */
    const char *passphrase;  /* passphrase for private key (NULL = none) */
} bs2_ssh_opts;

/* Execute a shell command. If stdin_data != NULL the command is run via
 * `bash -s` and stdin_data is piped to its stdin (script mode, CRLF already
 * normalized by the caller). Fills r; never aborts the process. */
void bs2_ssh_exec(const bs2_ssh_opts *o, const char *cmd,
                  const char *stdin_data, size_t stdin_len, bs2_result *r);

/* Upload a local file or directory tree to remote path via SFTP. */
void bs2_ssh_upload(const bs2_ssh_opts *o, const char *local,
                    const char *remote, bs2_result *r);

/* Download a remote file or directory tree into local_dir/<host>/... */
void bs2_ssh_download(const bs2_ssh_opts *o, const char *remote,
                      const char *local_dir, bs2_result *r);

/* WSAStartup & friends; call once from main. */
int  bs2_net_init(void);
void bs2_net_cleanup(void);

#endif
