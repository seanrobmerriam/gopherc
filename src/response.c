#include "gopherd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct text_encoder {
    int fd;
    unsigned char buf[GOPHER_IO_BUFSIZE];
    size_t used;
    bool at_line_start;
};

struct menu_entry {
    char *name;
    bool is_dir;
    char item_type;
};

int
write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t nwritten = write(fd, p, len);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)nwritten;
        len -= (size_t)nwritten;
    }

    return 0;
}

static int
text_flush(struct text_encoder *enc)
{
    if (enc->used == 0) {
        return 0;
    }
    if (write_all(enc->fd, enc->buf, enc->used) < 0) {
        return -1;
    }
    enc->used = 0;
    return 0;
}

static int
text_putc(struct text_encoder *enc, unsigned char ch)
{
    if (enc->used == sizeof(enc->buf) && text_flush(enc) < 0) {
        return -1;
    }
    enc->buf[enc->used++] = ch;
    return 0;
}

static int
text_put_crlf(struct text_encoder *enc)
{
    if (text_putc(enc, '\r') < 0 || text_putc(enc, '\n') < 0) {
        return -1;
    }
    enc->at_line_start = true;
    return 0;
}

static int
send_menu_item(int client_fd, const struct server_config *config, char item_type,
               const char *display, const char *selector)
{
    const char *host = menu_host(config);
    size_t host_len = strlen(host);
    int port_len = snprintf(NULL, 0, "%u", config->port);
    size_t line_len;
    char *line;

    if (port_len < 0) {
        errno = EINVAL;
        return -1;
    }

    line_len = 1U + strlen(display) + 1U + strlen(selector) + 1U +
               host_len + 1U + (size_t)port_len + 2U;
    line = malloc(line_len + 1U);
    if (line == NULL) {
        return -1;
    }

    snprintf(line, line_len + 1U, "%c%s\t%s\t%s\t%u\r\n",
             item_type, display, selector, host, config->port);

    if (write_all(client_fd, line, line_len) < 0) {
        free(line);
        return -1;
    }

    free(line);
    return 0;
}

static int
send_raw_menu_line(int client_fd, const char *line, size_t len)
{
    if (write_all(client_fd, line, len) < 0) {
        return -1;
    }
    return write_all(client_fd, "\r\n", 2);
}

int
send_error_menu(int client_fd, const struct server_config *config,
                const char *message)
{
    if (send_menu_item(client_fd, config, '3', message, "") < 0) {
        return -1;
    }
    return write_all(client_fd, ".\r\n", 3);
}

static bool
is_menu_safe_name(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    for (; *p != '\0'; p++) {
        if (*p == '\t' || *p == '\r' || *p == '\n' || *p == '\0') {
            return false;
        }
    }

    return true;
}

static char *
build_child_selector(const char *base_selector, const char *name)
{
    size_t base_len = strlen(base_selector);
    size_t name_len = strlen(name);
    size_t total_len;
    char *selector;

    total_len = 1U + base_len + (base_len > 0 ? 1U : 0U) + name_len;
    if (total_len > GOPHER_SELECTOR_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    selector = malloc(total_len + 1U);
    if (selector == NULL) {
        return NULL;
    }

    if (base_len == 0) {
        selector[0] = '/';
        memcpy(selector + 1, name, name_len);
        selector[1 + name_len] = '\0';
    } else {
        selector[0] = '/';
        memcpy(selector + 1, base_selector, base_len);
        selector[1 + base_len] = '/';
        memcpy(selector + 2 + base_len, name, name_len);
        selector[total_len] = '\0';
    }

    return selector;
}

static void
free_menu_entries(struct menu_entry *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

static int
append_menu_entry(struct menu_entry **entries, size_t *count, size_t *capacity,
                  const char *name, bool is_dir)
{
    struct menu_entry *new_entries;

    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 16U : (*capacity * 2U);

        new_entries = realloc(*entries, new_capacity * sizeof(*new_entries));
        if (new_entries == NULL) {
            return -1;
        }
        *entries = new_entries;
        *capacity = new_capacity;
    }

    (*entries)[*count].name = strdup(name);
    if ((*entries)[*count].name == NULL) {
        return -1;
    }
    (*entries)[*count].is_dir = is_dir;
    (*entries)[*count].item_type = gopher_item_type_from_name(name, is_dir);
    (*count)++;
    return 0;
}

static int
menu_entry_cmp(const void *lhs, const void *rhs)
{
    const struct menu_entry *left = lhs;
    const struct menu_entry *right = rhs;

    return strcmp(left->name, right->name);
}

static size_t
count_tabs(const char *line)
{
    size_t count = 0;

    for (; *line != '\0'; line++) {
        if (*line == '\t') {
            count++;
        }
    }
    return count;
}

static int
serve_gophermap(int client_fd, const struct server_config *config, int map_fd)
{
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    int rc = -1;

    fp = fdopen(map_fd, "r");
    if (fp == NULL) {
        close(map_fd);
        return -1;
    }

    for (;;) {
        ssize_t nread = getline(&line, &cap, fp);
        size_t line_len;
        size_t tabs;

        if (nread < 0) {
            if (feof(fp)) {
                break;
            }
            goto out;
        }
        if ((size_t)nread > GOPHER_MAP_LINE_MAX) {
            errno = EOVERFLOW;
            goto out;
        }
        while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            nread--;
        }
        if (nread == 0) {
            continue;
        }
        if (memchr(line, '\0', (size_t)nread) != NULL) {
            errno = EINVAL;
            goto out;
        }

        line_len = (size_t)nread;
        line[line_len] = '\0';
        tabs = count_tabs(line);

        if (tabs >= 3) {
            if (send_raw_menu_line(client_fd, line, line_len) < 0) {
                goto out;
            }
            continue;
        }
        if (tabs == 0) {
            if (send_menu_item(client_fd, config, 'i', line, "") < 0) {
                goto out;
            }
            continue;
        }

        errno = EINVAL;
        goto out;
    }

    rc = write_all(client_fd, ".\r\n", 3);

out:
    free(line);
    if (fclose(fp) < 0 && rc == 0) {
        rc = -1;
    }
    return rc;
}

static int
serve_directory_menu(int client_fd, const struct server_config *config,
                     const char *selector, int dir_fd)
{
    DIR *dir;
    struct dirent *entry;
    char *child_selector = NULL;
    char item_type;
    struct menu_entry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    int map_fd;
    int rc = -1;

    map_fd = openat(dir_fd, "gophermap", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (map_fd >= 0) {
        if (serve_gophermap(client_fd, config, map_fd) < 0) {
            log_errno_msg("serve gophermap");
            close(dir_fd);
            return -1;
        }
        if (close(dir_fd) < 0) {
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        log_errno_msg("open gophermap");
    }

    dir = fdopendir(dir_fd);
    if (dir == NULL) {
        return -1;
    }

    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        int child_fd;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!config->allow_hidden && is_hidden_name(entry->d_name)) {
            continue;
        }
        if (!is_menu_safe_name(entry->d_name)) {
            continue;
        }

        child_fd = openat(dirfd(dir), entry->d_name,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (child_fd < 0) {
            continue;
        }
        if (fstat(child_fd, &st) < 0) {
            close(child_fd);
            continue;
        }
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            close(child_fd);
            continue;
        }

        if (append_menu_entry(&entries, &entry_count, &entry_capacity,
                              entry->d_name, S_ISDIR(st.st_mode)) < 0) {
            close(child_fd);
            goto out;
        }
        close(child_fd);
        errno = 0;
    }

    if (entry == NULL && errno != 0) {
        log_errno_msg("readdir");
    }

    qsort(entries, entry_count, sizeof(*entries), menu_entry_cmp);

    for (size_t i = 0; i < entry_count; i++) {
        child_selector = build_child_selector(selector, entries[i].name);
        if (child_selector == NULL) {
            goto out;
        }

        item_type = entries[i].item_type;
        if (send_menu_item(client_fd, config, item_type, entries[i].name,
                           child_selector) < 0) {
            free(child_selector);
            child_selector = NULL;
            goto out;
        }
        free(child_selector);
        child_selector = NULL;
    }

    rc = write_all(client_fd, ".\r\n", 3);

out:
    free(child_selector);
    free_menu_entries(entries, entry_count);
    if (closedir(dir) < 0 && rc == 0) {
        rc = -1;
    }
    return rc;
}

static int
serve_binary_file(int client_fd, int file_fd)
{
    unsigned char buf[GOPHER_IO_BUFSIZE];

    for (;;) {
        ssize_t nread = read(file_fd, buf, sizeof(buf));
        if (nread == 0) {
            return 0;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (write_all(client_fd, buf, (size_t)nread) < 0) {
            return -1;
        }
    }
}

static int
serve_text_file(int client_fd, int file_fd)
{
    unsigned char inbuf[GOPHER_IO_BUFSIZE];
    struct text_encoder enc = {
        .fd = client_fd,
        .used = 0,
        .at_line_start = true
    };
    bool pending_cr = false;

    for (;;) {
        ssize_t nread = read(file_fd, inbuf, sizeof(inbuf));
        size_t i;

        if (nread == 0) {
            break;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        for (i = 0; i < (size_t)nread; i++) {
            unsigned char ch = inbuf[i];

            if (pending_cr) {
                pending_cr = false;
                if (ch == '\n') {
                    if (text_put_crlf(&enc) < 0) {
                        return -1;
                    }
                    continue;
                }
                if (text_put_crlf(&enc) < 0) {
                    return -1;
                }
            }

            if (ch == '\r') {
                pending_cr = true;
                continue;
            }
            if (ch == '\n') {
                if (text_put_crlf(&enc) < 0) {
                    return -1;
                }
                continue;
            }
            if (enc.at_line_start && ch == '.') {
                if (text_putc(&enc, '.') < 0) {
                    return -1;
                }
            }
            if (text_putc(&enc, ch) < 0) {
                return -1;
            }
            enc.at_line_start = false;
        }
    }

    if (pending_cr && text_put_crlf(&enc) < 0) {
        return -1;
    }
    if (!enc.at_line_start && text_put_crlf(&enc) < 0) {
        return -1;
    }
    if (text_putc(&enc, '.') < 0 ||
        text_putc(&enc, '\r') < 0 ||
        text_putc(&enc, '\n') < 0) {
        return -1;
    }

    return text_flush(&enc);
}

int
serve_selector(int client_fd, const struct server_config *config, int root_fd,
               const char *selector)
{
    int node_fd = -1;
    bool is_dir = false;
    int rc = -1;

    if (open_selector_node(root_fd, selector, config->allow_hidden,
                           &node_fd, &is_dir) < 0) {
        switch (errno) {
        case ENOENT:
        case ENOTDIR:
        case EACCES:
        case EPERM:
        case ELOOP:
        case ENAMETOOLONG:
            return send_error_menu(client_fd, config, "Not found");
        default:
            log_errno_msg("resolve selector");
            return send_error_menu(client_fd, config, "Internal server error");
        }
    }

    if (is_dir) {
        rc = serve_directory_menu(client_fd, config, selector, node_fd);
        if (rc < 0) {
            log_errno_msg("serve directory");
        }
        return rc;
    }

    {
        const char *name = strrchr(selector, '/');
        char item_type;

        if (name == NULL) {
            name = selector;
        } else {
            name++;
        }
        item_type = gopher_item_type_from_name(name, false);
        if (item_type == '0') {
            rc = serve_text_file(client_fd, node_fd);
        } else {
            rc = serve_binary_file(client_fd, node_fd);
        }
    }

    if (close(node_fd) < 0 && rc == 0) {
        rc = -1;
    }
    if (rc < 0) {
        log_errno_msg("serve file");
    }
    return rc;
}
