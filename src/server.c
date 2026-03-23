#include "gopherd.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
int initgroups(const char *, gid_t);
#endif

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t child_exit_pending = 0;

static int
resolve_user_spec(const char *spec, uid_t *uid_out, gid_t *gid_out, const char **name_out)
{
    struct passwd *pw = NULL;
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(spec, &end, 10);
    if (errno == 0 && end != spec && *end == '\0') {
        if ((unsigned long)((uid_t)value) != value) {
            errno = ERANGE;
            return -1;
        }
        pw = getpwuid((uid_t)value);
    } else {
        pw = getpwnam(spec);
    }
    if (pw == NULL) {
        errno = EINVAL;
        return -1;
    }

    *uid_out = pw->pw_uid;
    *gid_out = pw->pw_gid;
    *name_out = pw->pw_name;
    return 0;
}

static int
resolve_group_spec(const char *spec, gid_t *gid_out)
{
    struct group *gr = NULL;
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(spec, &end, 10);
    if (errno == 0 && end != spec && *end == '\0') {
        if ((unsigned long)((gid_t)value) != value) {
            errno = ERANGE;
            return -1;
        }
        *gid_out = (gid_t)value;
        return 0;
    }

    gr = getgrnam(spec);
    if (gr == NULL) {
        errno = EINVAL;
        return -1;
    }

    *gid_out = gr->gr_gid;
    return 0;
}

static int
drop_privileges(const struct server_config *config)
{
    uid_t target_uid = (uid_t)-1;
    gid_t target_gid = (gid_t)-1;
    gid_t user_gid = (gid_t)-1;
    const char *user_name = NULL;
    bool have_user = false;
    bool have_group = false;

    if (config->user == NULL && config->group == NULL) {
        return 0;
    }

    if (config->user != NULL) {
        if (resolve_user_spec(config->user, &target_uid, &user_gid, &user_name) < 0) {
            log_error("unable to resolve user: %s", config->user);
            return -1;
        }
        have_user = true;
        target_gid = user_gid;
    }

    if (config->group != NULL) {
        if (resolve_group_spec(config->group, &target_gid) < 0) {
            log_error("unable to resolve group: %s", config->group);
            return -1;
        }
        have_group = true;
    }

    if (have_user) {
        if (initgroups(user_name, target_gid) < 0) {
            log_errno_msg("initgroups");
            return -1;
        }
    }
    if (have_user || have_group) {
        if (setgid(target_gid) < 0) {
            log_errno_msg("setgid");
            return -1;
        }
    }
    if (have_user) {
        if (setuid(target_uid) < 0) {
            log_errno_msg("setuid");
            return -1;
        }
        log_error("dropped privileges to uid=%lu gid=%lu",
                  (unsigned long)target_uid, (unsigned long)target_gid);
    } else {
        log_error("dropped privileges to gid=%lu", (unsigned long)target_gid);
    }

    return 0;
}

static void
handle_shutdown_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int
reap_children(void)
{
    int reaped = 0;

    for (;;) {
        pid_t pid = waitpid(-1, NULL, WNOHANG);

        if (pid > 0) {
            reaped++;
            continue;
        }
        if (pid == 0) {
            return reaped;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ECHILD) {
            return reaped;
        }
        return -1;
    }
}

static void
handle_sigchld(int signo)
{
    (void)signo;
    child_exit_pending = 1;
}

static int
install_signal_handlers(void)
{
    struct sigaction sa;
    struct sigaction chld_sa;
    struct sigaction ignore_sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        return -1;
    }

    memset(&chld_sa, 0, sizeof(chld_sa));
    chld_sa.sa_handler = handle_sigchld;
    chld_sa.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&chld_sa.sa_mask);
    if (sigaction(SIGCHLD, &chld_sa, NULL) < 0) {
        return -1;
    }

    memset(&ignore_sa, 0, sizeof(ignore_sa));
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    if (sigaction(SIGPIPE, &ignore_sa, NULL) < 0) {
        return -1;
    }

    return 0;
}

int
create_listen_socket(const struct server_config *config)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char port_str[6];
    int listen_fd = -1;
    int rc;

    snprintf(port_str, sizeof(port_str), "%u", config->port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(config->bind_addr, port_str, &hints, &res);
    if (rc != 0) {
        log_error("getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int one = 1;

        listen_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
            log_errno_msg("setsockopt(SO_REUSEADDR)");
            close(listen_fd);
            listen_fd = -1;
            continue;
        }
        if (bind(listen_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            close(listen_fd);
            listen_fd = -1;
            continue;
        }
        if (listen(listen_fd, GOPHER_LISTEN_BACKLOG) < 0) {
            log_errno_msg("listen");
            close(listen_fd);
            listen_fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(res);
    if (listen_fd < 0) {
        log_error("unable to bind %s:%u", config->bind_addr, config->port);
    }
    return listen_fd;
}

static int
handle_client(int client_fd, const struct server_config *config, int root_fd)
{
    char *raw_selector = NULL;
    char *selector = NULL;
    int status;
    int rc = -1;

    status = read_selector_line(client_fd, &raw_selector);
    if (status == REQUEST_EOF) {
        return 0;
    }
    if (status == REQUEST_IO_ERROR) {
        log_errno_msg("read request");
        return -1;
    }
    if (status == REQUEST_INVALID) {
        return send_error_menu(client_fd, config, "Malformed request");
    }

    if (normalize_selector(raw_selector, &selector) < 0) {
        free(raw_selector);
        return send_error_menu(client_fd, config, "Unsupported selector");
    }
    free(raw_selector);

    rc = serve_selector(client_fd, config, root_fd, selector);
    free(selector);
    return rc;
}

int
run_server(const struct server_config *config)
{
    char *docroot_real = NULL;
    int root_fd = -1;
    int listen_fd = -1;
    int rc = -1;
    unsigned active_children = 0;
    struct stat st;

    if (install_signal_handlers() < 0) {
        log_errno_msg("sigaction");
        return -1;
    }

    docroot_real = realpath(config->docroot, NULL);
    if (docroot_real == NULL) {
        log_errno_msg("realpath(docroot)");
        goto out;
    }

    root_fd = open(docroot_real, O_RDONLY | O_CLOEXEC);
    if (root_fd < 0) {
        log_errno_msg("open(docroot)");
        goto out;
    }
    if (fstat(root_fd, &st) < 0) {
        log_errno_msg("fstat(docroot)");
        goto out;
    }
    if (!S_ISDIR(st.st_mode)) {
        log_error("%s is not a directory", docroot_real);
        goto out;
    }

    listen_fd = create_listen_socket(config);
    if (listen_fd < 0) {
        goto out;
    }

    if (drop_privileges(config) < 0) {
        goto out;
    }

    log_error("listening on %s:%u, root=%s",
              config->bind_addr, config->port, docroot_real);

    for (;;) {
        int client_fd;
        pid_t pid;

        if (stop_requested) {
            rc = 0;
            break;
        }
        if (child_exit_pending) {
            int reaped;

            child_exit_pending = 0;
            reaped = reap_children();
            if (reaped < 0) {
                log_errno_msg("waitpid");
            } else if ((unsigned)reaped >= active_children) {
                active_children = 0;
            } else {
                active_children -= (unsigned)reaped;
            }
        }

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                if (child_exit_pending) {
                    int reaped;

                    child_exit_pending = 0;
                    reaped = reap_children();
                    if (reaped < 0) {
                        log_errno_msg("waitpid");
                    } else if ((unsigned)reaped >= active_children) {
                        active_children = 0;
                    } else {
                        active_children -= (unsigned)reaped;
                    }
                }
                if (stop_requested) {
                    rc = 0;
                    break;
                }
                continue;
            }
            log_errno_msg("accept");
            continue;
        }

        if (config->max_children != 0 && active_children >= config->max_children) {
            log_error("connection refused: server busy");
            if (close(client_fd) < 0) {
                log_errno_msg("close(client)");
            }
            continue;
        }

        pid = fork();
        if (pid < 0) {
            log_errno_msg("fork");
            if (handle_client(client_fd, config, root_fd) < 0) {
                /* response path already logged where useful */
            }
            if (close(client_fd) < 0) {
                log_errno_msg("close(client)");
            }
            continue;
        }

        if (pid == 0) {
            int child_status = 0;

            if (close(listen_fd) < 0) {
                log_errno_msg("close(listen in child)");
                child_status = 1;
            } else if (handle_client(client_fd, config, root_fd) < 0) {
                child_status = 1;
            }
            if (close(client_fd) < 0) {
                log_errno_msg("close(client in child)");
                child_status = 1;
            }
            if (close(root_fd) < 0) {
                log_errno_msg("close(docroot in child)");
                child_status = 1;
            }
            _exit(child_status);
        }

        active_children++;
        if (close(client_fd) < 0) {
            log_errno_msg("close(client)");
        }
    }

out:
    if (listen_fd >= 0 && close(listen_fd) < 0) {
        log_errno_msg("close(listen)");
    }
    if (root_fd >= 0 && close(root_fd) < 0) {
        log_errno_msg("close(docroot)");
    }
    free(docroot_real);
    return rc;
}
