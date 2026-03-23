#ifndef GOPHERD_H
#define GOPHERD_H

#include <stdbool.h>
#include <stddef.h>

#define GOPHER_SELECTOR_MAX 255
#define GOPHER_IO_BUFSIZE 4096
#define GOPHER_LISTEN_BACKLOG 64
#define GOPHER_MAP_LINE_MAX 4096

enum request_status {
    REQUEST_IO_ERROR = -1,
    REQUEST_INVALID = -2,
    REQUEST_EOF = 0,
    REQUEST_OK = 1
};

struct server_config {
    char *docroot;
    char *bind_addr;
    char *hostname;
    char *user;
    char *group;
    unsigned port;
    unsigned max_children;
    bool allow_hidden;
};

int run_server(const struct server_config *config);

int read_selector_line(int fd, char **selector_out);
int normalize_selector(const char *raw, char **normalized_out);
int open_selector_node(int root_fd, const char *selector, bool allow_hidden,
                       int *node_fd_out, bool *is_dir_out);

int serve_selector(int client_fd, const struct server_config *config, int root_fd,
                   const char *selector);
int send_error_menu(int client_fd, const struct server_config *config,
                    const char *message);
int write_all(int fd, const void *buf, size_t len);

int create_listen_socket(const struct server_config *config);
int dup_cloexec(int fd);

char gopher_item_type_from_name(const char *name, bool is_dir);
const char *menu_host(const struct server_config *config);
bool is_hidden_name(const char *name);

void log_error(const char *fmt, ...);
void log_errno_msg(const char *context);

#endif
