#include "gopherd.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -r DOCROOT [-a ADDRESS] [-p PORT] [-n HOSTNAME] [-u USER] [-g GROUP] [-m MAX_CHILDREN] [-A]\n"
            "  -r DOCROOT   document root to serve\n"
            "  -a ADDRESS   bind address (default: 0.0.0.0)\n"
            "  -p PORT      TCP port (default: 7070)\n"
            "  -n HOSTNAME  hostname advertised in generated menus\n"
            "  -u USER      drop privileges to this user after binding\n"
            "  -g GROUP     drop privileges to this group after binding\n"
            "  -m COUNT     max concurrent child workers, 0 means unlimited\n"
            "  -A           allow hidden files and directories\n"
            "  -h           show this help\n",
            prog);
}

static int
parse_port(const char *text, unsigned *port_out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 65535UL) {
        return -1;
    }

    *port_out = (unsigned)value;
    return 0;
}

static int
parse_uint(const char *text, unsigned *value_out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT_MAX) {
        return -1;
    }

    *value_out = (unsigned)value;
    return 0;
}

int
main(int argc, char **argv)
{
    struct server_config config = {
        .docroot = NULL,
        .bind_addr = NULL,
        .hostname = NULL,
        .user = NULL,
        .group = NULL,
        .port = 7070,
        .max_children = 0,
        .allow_hidden = false
    };
    int opt;
    int exit_code = EXIT_FAILURE;

    config.bind_addr = strdup("0.0.0.0");
    if (config.bind_addr == NULL) {
        perror("strdup");
        return EXIT_FAILURE;
    }

    while ((opt = getopt(argc, argv, "Ar:a:p:n:u:g:m:h")) != -1) {
        switch (opt) {
        case 'A':
            config.allow_hidden = true;
            break;
        case 'r':
            free(config.docroot);
            config.docroot = strdup(optarg);
            break;
        case 'a':
            free(config.bind_addr);
            config.bind_addr = strdup(optarg);
            break;
        case 'p':
            if (parse_port(optarg, &config.port) < 0) {
                fprintf(stderr, "invalid port: %s\n", optarg);
                goto out;
            }
            break;
        case 'n':
            free(config.hostname);
            config.hostname = strdup(optarg);
            break;
        case 'u':
            free(config.user);
            config.user = strdup(optarg);
            break;
        case 'g':
            free(config.group);
            config.group = strdup(optarg);
            break;
        case 'm':
            if (parse_uint(optarg, &config.max_children) < 0) {
                fprintf(stderr, "invalid max children: %s\n", optarg);
                goto out;
            }
            break;
        case 'h':
            usage(argv[0]);
            exit_code = EXIT_SUCCESS;
            goto out;
        default:
            usage(argv[0]);
            goto out;
        }

        if ((opt == 'r' && config.docroot == NULL) ||
            (opt == 'a' && config.bind_addr == NULL) ||
            (opt == 'n' && config.hostname == NULL) ||
            (opt == 'u' && config.user == NULL) ||
            (opt == 'g' && config.group == NULL)) {
            perror("strdup");
            goto out;
        }
    }

    if (config.docroot == NULL) {
        usage(argv[0]);
        goto out;
    }

    if (run_server(&config) == 0) {
        exit_code = EXIT_SUCCESS;
    }

out:
    free(config.docroot);
    free(config.bind_addr);
    free(config.hostname);
    free(config.user);
    free(config.group);
    return exit_code;
}
