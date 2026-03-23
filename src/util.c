#include "gopherd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

void
log_error(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "gopherd: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void
log_errno_msg(const char *context)
{
    int saved_errno = errno;

    fprintf(stderr, "gopherd: %s: %s\n", context, strerror(saved_errno));
}

int
dup_cloexec(int fd)
{
    int new_fd = dup(fd);
    int flags;

    if (new_fd < 0) {
        return -1;
    }

    flags = fcntl(new_fd, F_GETFD);
    if (flags < 0) {
        goto fail;
    }
    if (fcntl(new_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        goto fail;
    }

    return new_fd;

fail:
    {
        int saved_errno = errno;
        close(new_fd);
        errno = saved_errno;
    }
    return -1;
}

bool
is_hidden_name(const char *name)
{
    return name[0] == '.';
}

const char *
menu_host(const struct server_config *config)
{
    if (config->hostname != NULL && config->hostname[0] != '\0') {
        return config->hostname;
    }

    if (config->bind_addr == NULL || config->bind_addr[0] == '\0') {
        return "localhost";
    }

    if (strcmp(config->bind_addr, "0.0.0.0") == 0 ||
        strcmp(config->bind_addr, "::") == 0 ||
        strcmp(config->bind_addr, "*") == 0) {
        return "localhost";
    }

    return config->bind_addr;
}

static bool
has_text_extension(const char *name)
{
    static const char *const text_exts[] = {
        ".txt",  ".text", ".asc",  ".md",   ".html", ".htm",  ".csv",
        ".tsv",  ".log",  ".conf", ".cfg",  ".ini",  ".json", ".xml",
        ".yaml", ".yml",  ".toml", ".c",    ".h",    ".cc",   ".cpp",
        ".hpp",  ".go",   ".rs",   ".py",   ".pl",   ".sh",   ".bash",
        ".zsh",  ".mk"
    };
    const char *dot;
    size_t i;

    dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }

    for (i = 0; i < sizeof(text_exts) / sizeof(text_exts[0]); i++) {
        if (strcasecmp(dot, text_exts[i]) == 0) {
            return true;
        }
    }

    return false;
}

char
gopher_item_type_from_name(const char *name, bool is_dir)
{
    if (is_dir) {
        return '1';
    }

    if (has_text_extension(name)) {
        return '0';
    }

    return '9';
}
