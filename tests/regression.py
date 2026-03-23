#!/usr/bin/env python3
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def query(port: int, selector: str) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=2.0) as sock:
        sock.sendall(selector.encode("utf-8") + b"\r\n")
        chunks = []
        while True:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
        return b"".join(chunks)


def wait_for_server(proc: subprocess.Popen, port: int, deadline: float) -> None:
    while time.time() < deadline:
        if proc.poll() is not None:
            stderr = proc.stderr.read() if proc.stderr is not None else ""
            raise RuntimeError(f"server exited early: {stderr}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start in time")


def start_server(
    binary: str, root: Path, port: int, extra_args=None
) -> subprocess.Popen:
    args = [
        binary,
        "-r",
        str(root),
        "-a",
        "127.0.0.1",
        "-p",
        str(port),
        "-n",
        "testhost",
    ]
    if extra_args:
        args.extend(extra_args)
    proc = subprocess.Popen(
        args,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    return proc


def stop_server(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)


def make_tree(root: Path) -> None:
    (root / "dir").mkdir()
    (root / "a.txt").write_text("alpha\n.leading\n", encoding="utf-8")
    (root / "b.txt").write_text("bravo\n", encoding="utf-8")
    (root / "z.bin").write_bytes(b"\x00\x01\x02binary")
    (root / ".hidden.txt").write_text("hidden\n", encoding="utf-8")
    (root / "dir" / "readme.txt").write_text("nested\n", encoding="utf-8")
    (root / "dir" / "gophermap").write_text(
        "Directory banner\n0Custom item\t/custom\tother.example\t70\n",
        encoding="utf-8",
    )
    try:
        os.symlink("/etc/passwd", root / "passwd-link")
    except OSError:
        pass


def parse_menu_lines(payload: bytes):
    text = payload.decode("utf-8", "strict")
    lines = text.split("\r\n")
    assert lines[-2] == ".", f"menu did not terminate cleanly: {text!r}"
    return [line for line in lines[:-2] if line]


def assert_root_menu(payload: bytes) -> None:
    lines = parse_menu_lines(payload)
    displays = [line[1:].split("\t", 1)[0] for line in lines]
    assert displays == ["a.txt", "b.txt", "dir", "z.bin"], displays


def assert_text_response(payload: bytes) -> None:
    assert payload == b"alpha\r\n..leading\r\n.\r\n", payload


def assert_binary_response(payload: bytes) -> None:
    assert payload == b"\x00\x01\x02binary", payload


def assert_traversal_rejected(payload: bytes) -> None:
    lines = parse_menu_lines(payload)
    assert len(lines) == 1, lines
    assert lines[0].startswith("3"), lines[0]


def assert_gophermap_override(payload: bytes, port: int) -> None:
    lines = parse_menu_lines(payload)
    expected = [
        f"iDirectory banner\t\ttesthost\t{port}",
        "0Custom item\t/custom\tother.example\t70",
    ]
    assert lines == expected, lines


def assert_concurrent_service(port: int) -> None:
    blocker = socket.create_connection(("127.0.0.1", port), timeout=2.0)
    blocker.settimeout(2.0)
    try:
        time.sleep(0.2)
        assert_root_menu(query(port, ""))
    finally:
        blocker.close()


def assert_busy_limit(binary: str, root: Path) -> None:
    port = find_free_port()
    proc = start_server(binary, root, port, ["-m", "1"])
    try:
        wait_for_server(proc, port, time.time() + 5)
        time.sleep(0.3)
        blocker = socket.create_connection(("127.0.0.1", port), timeout=2.0)
        blocker.settimeout(2.0)
        try:
            time.sleep(0.2)
            try:
                payload = query(port, "")
            except ConnectionResetError:
                payload = b""
            assert payload in (b"",), payload
        finally:
            blocker.close()
    finally:
        stop_server(proc)


def maybe_assert_privilege_drop(binary: str, root: Path) -> None:
    if os.geteuid() != 0:
        return

    candidate = None
    for name in ("nobody", "_nobody", "daemon"):
        try:
            import pwd

            pwd.getpwnam(name)
            candidate = name
            break
        except KeyError:
            continue

    if candidate is None:
        return

    port = find_free_port()
    proc = start_server(binary, root, port, ["-u", candidate])
    try:
        deadline = time.time() + 5
        while time.time() < deadline:
            line = proc.stderr.readline()
            if not line:
                if proc.poll() is not None:
                    raise RuntimeError("privilege-drop server exited early")
                continue
            if "dropped privileges" in line:
                break
        else:
            raise RuntimeError("privilege drop log not observed")
        wait_for_server(proc, port, time.time() + 5)
        assert_root_menu(query(port, ""))
    finally:
        stop_server(proc)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: regression.py /path/to/gopherd", file=sys.stderr)
        sys.exit(2)

    binary = sys.argv[1]
    workdir = Path(tempfile.mkdtemp(prefix="gopherd-regression-"))
    PORT = find_free_port()

    try:
        make_tree(workdir)
        proc = start_server(binary, workdir, PORT)
        try:
            wait_for_server(proc, PORT, time.time() + 5)
            assert_root_menu(query(PORT, ""))
            assert_text_response(query(PORT, "/a.txt"))
            assert_binary_response(query(PORT, "/z.bin"))
            assert_traversal_rejected(query(PORT, "/../../etc/passwd"))
            assert_gophermap_override(query(PORT, "/dir"), PORT)
            assert_concurrent_service(PORT)
        finally:
            stop_server(proc)

        maybe_assert_privilege_drop(binary, workdir)
        assert_busy_limit(binary, workdir)
    finally:
        shutil.rmtree(workdir)
