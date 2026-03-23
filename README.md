# gopherc

`gopherc` is a small Gopher server for Unix-like systems written in C11 with POSIX sockets and no external runtime dependencies.

It serves content from a document root, supports directories, text files, and binary files, and is intentionally conservative about filesystem access and protocol handling.

## Features

- Gopher menus for directories
- Text file serving with CRLF normalization and dot-stuffing
- Binary file serving as raw bytes until close
- Empty selector maps to the document root
- Hidden files denied by default
- Path traversal rejected
- Symlink traversal refused with `O_NOFOLLOW`
- Optional `gophermap` override for directory menus
- Fork-per-connection concurrency
- Optional privilege drop after bind/listen
- Optional child-process limit for basic backpressure

## Build

Requirements:

- `cc`, `gcc`, or `clang`
- `make`
- Python 3 for the regression test script

Build the server:

```sh
make
```

Run the regression tests:

```sh
make test
```

Clean build outputs:

```sh
make clean
```

## Run

Serve the bundled example tree on localhost port `7070`:

```sh
./gopherd -r example-root -a 127.0.0.1 -p 7070 -n localhost
```

Fetch the root menu with a simple client:

```sh
python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 7070))
s.sendall(b"\r\n")
print(s.recv(4096).decode("utf-8", "replace"))
PY
```

## Command-Line Options

```text
-r DOCROOT        document root to serve
-a ADDRESS        bind address, default: 0.0.0.0
-p PORT           TCP port, default: 7070
-n HOSTNAME       hostname advertised in generated menus
-u USER           drop privileges to this user after binding
-g GROUP          drop privileges to this group after binding
-m COUNT          max concurrent child workers, 0 means unlimited
-A                allow hidden files and directories
-h                show help
```

Notes:

- `-r` is required.
- If `-n` is omitted, generated menus use the bind address when possible, otherwise `localhost`.
- If `-u` is set, supplementary groups are dropped with `initgroups()` before `setgid()` and `setuid()`.
- If `-m` is reached, new connections are refused by the parent instead of spawning more workers.

## Document Root Behavior

Selectors map to paths under the configured root.

By default:

- names beginning with `.` are ignored in menus and cannot be requested
- `.` and `..` path components are rejected
- symlinks are refused during traversal
- only regular files and directories are served

Item type inference is intentionally simple:

- directories are type `1`
- known text-like extensions are type `0`
- everything else defaults to type `9`

## gophermap

If a directory contains a file named `gophermap`, it overrides the generated listing for that directory.

Supported line forms:

- no tabs: emitted as an informational `i` item using the server hostname and port
- three or more tabs: emitted verbatim as a menu line

Any other line form is treated as invalid and the request fails.

Example:

```text
Subdirectory banner
0Read the nested file	/subdir/readme.md	localhost	7070
```

## Example Content

The repository includes a sample tree under [`example-root/`](/home/sean/workspace/projects/gopherc/example-root):

```text
example-root/
├── docs/
│   └── welcome.txt
├── files/
│   └── blob.bin
├── index.txt
└── subdir/
    ├── gophermap
    └── readme.md
```

## Architecture

Source layout:

- [`src/main.c`](/home/sean/workspace/projects/gopherc/src/main.c): CLI parsing
- [`src/server.c`](/home/sean/workspace/projects/gopherc/src/server.c): socket setup, signals, fork-per-connection loop, privilege drop
- [`src/request.c`](/home/sean/workspace/projects/gopherc/src/request.c): bounded selector parsing and secure path resolution
- [`src/response.c`](/home/sean/workspace/projects/gopherc/src/response.c): menus, `gophermap`, text and binary responses
- [`src/util.c`](/home/sean/workspace/projects/gopherc/src/util.c): logging and small helpers
- [`tests/regression.py`](/home/sean/workspace/projects/gopherc/tests/regression.py): end-to-end smoke tests

## Security Properties

This server is designed to preserve a small set of explicit invariants:

- selector input is bounded and treated as untrusted
- accesses stay inside the configured document root
- partial writes are handled correctly
- file descriptors are closed on all main paths
- text responses use valid Gopher line termination
- menus terminate with `.\r\n`

## Operational Notes

- The server uses `fork()` per connection.
- Children are reaped with `SIGCHLD` and `waitpid(..., WNOHANG)`.
- `SIGINT` and `SIGTERM` trigger a graceful stop of the accept loop.
- `SIGPIPE` is ignored.
- `SO_REUSEADDR` is enabled on the listening socket.

## Limitations

- No TLS
- No authentication or authorization
- No chroot, seccomp, or pledge-style sandbox
- No access log or structured request log
- No sorted order customization beyond lexical filename sort
- No MIME detection beyond filename-extension heuristics
- No advanced Gopher+ features

## Development

Typical workflow:

```sh
make
make test
./gopherd -r example-root -a 127.0.0.1 -p 7070 -n localhost
```
