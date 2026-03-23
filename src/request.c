#include "gopherd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int
drain_request_line(int fd)
{
    unsigned char ch;
    ssize_t nread;

    for (;;) {
        nread = read(fd, &ch, 1);
        if (nread == 0) {
            return 0;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ch == '\n') {
            return 0;
        }
    }
}

int
read_selector_line(int fd, char **selector_out)
{
    char *buf;
    size_t len = 0;
    bool saw_newline = false;

    *selector_out = NULL;
    buf = calloc(GOPHER_SELECTOR_MAX + 1U, sizeof(*buf));
    if (buf == NULL) {
        return REQUEST_IO_ERROR;
    }

    for (;;) {
        unsigned char ch;
        ssize_t nread;

        nread = read(fd, &ch, 1);
        if (nread == 0) {
            break;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return REQUEST_IO_ERROR;
        }
        if (ch == '\0') {
            free(buf);
            if (drain_request_line(fd) < 0) {
                return REQUEST_IO_ERROR;
            }
            return REQUEST_INVALID;
        }
        if (ch == '\n') {
            saw_newline = true;
            break;
        }
        if (len >= GOPHER_SELECTOR_MAX) {
            free(buf);
            if (drain_request_line(fd) < 0) {
                return REQUEST_IO_ERROR;
            }
            return REQUEST_INVALID;
        }
        buf[len++] = (char)ch;
    }

    if (len == 0 && !saw_newline) {
        free(buf);
        return REQUEST_EOF;
    }

    if (len > 0 && buf[len - 1] == '\r') {
        len--;
    }
    buf[len] = '\0';
    *selector_out = buf;
    return REQUEST_OK;
}

int
normalize_selector(const char *raw, char **normalized_out)
{
    const char *start;
    const char *end;
    char *normalized;
    size_t out_len;
    size_t i;

    *normalized_out = NULL;
    start = raw;
    while (*start == '/') {
        start++;
    }

    end = raw + strlen(raw);
    while (end > start && end[-1] == '/') {
        end--;
    }

    out_len = (size_t)(end - start);
    normalized = malloc(out_len + 1U);
    if (normalized == NULL) {
        return -1;
    }

    for (i = 0; i < out_len; i++) {
        unsigned char ch = (unsigned char)start[i];

        if (ch == '\t' || ch < 0x20U || ch == 0x7fU) {
            free(normalized);
            return -1;
        }
        normalized[i] = (char)ch;
    }
    normalized[out_len] = '\0';

    if (out_len > 0) {
        size_t component_start = 0;

        for (i = 0; i <= out_len; i++) {
            if (normalized[i] != '/' && normalized[i] != '\0') {
                continue;
            }

            if (i == component_start) {
                free(normalized);
                return -1;
            }
            if ((i - component_start) == 1 &&
                normalized[component_start] == '.') {
                free(normalized);
                return -1;
            }
            if ((i - component_start) == 2 &&
                normalized[component_start] == '.' &&
                normalized[component_start + 1] == '.') {
                free(normalized);
                return -1;
            }

            component_start = i + 1;
        }
    }

    *normalized_out = normalized;
    return 0;
}

int
open_selector_node(int root_fd, const char *selector, bool allow_hidden,
                   int *node_fd_out, bool *is_dir_out)
{
    int current_fd;
    char *path_copy = NULL;
    char *saveptr = NULL;
    char *component;

    *node_fd_out = -1;
    *is_dir_out = false;

    current_fd = openat(root_fd, ".", O_RDONLY | O_CLOEXEC);
    if (current_fd < 0) {
        return -1;
    }

    if (selector[0] == '\0') {
        struct stat st;

        if (fstat(current_fd, &st) < 0) {
            goto fail;
        }
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            goto fail;
        }
        *node_fd_out = current_fd;
        *is_dir_out = true;
        return 0;
    }

    path_copy = strdup(selector);
    if (path_copy == NULL) {
        goto fail;
    }

    component = strtok_r(path_copy, "/", &saveptr);
    while (component != NULL) {
        struct stat st;
        char *next_component = strtok_r(NULL, "/", &saveptr);
        int next_fd;

        if (!allow_hidden && is_hidden_name(component)) {
            errno = ENOENT;
            goto fail;
        }

        next_fd = openat(current_fd, component, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (next_fd < 0) {
            goto fail;
        }
        if (fstat(next_fd, &st) < 0) {
            int saved_errno = errno;
            close(next_fd);
            errno = saved_errno;
            goto fail;
        }

        if (next_component != NULL) {
            if (!S_ISDIR(st.st_mode)) {
                close(next_fd);
                errno = ENOTDIR;
                goto fail;
            }
        } else if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            close(next_fd);
            errno = EACCES;
            goto fail;
        }

        close(current_fd);
        current_fd = next_fd;
        component = next_component;

        if (component == NULL) {
            *node_fd_out = current_fd;
            *is_dir_out = S_ISDIR(st.st_mode);
            free(path_copy);
            return 0;
        }
    }

    errno = ENOENT;

fail:
    {
        int saved_errno = errno;
        free(path_copy);
        close(current_fd);
        errno = saved_errno;
    }
    return -1;
}
