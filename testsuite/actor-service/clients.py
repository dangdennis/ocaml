#**************************************************************************
#*                                                                        *
#*                                 OCaml                                  *
#*                                                                        *
#*                             Dennis Dang                                *
#*                                                                        *
#*   Copyright 2026 Dennis Dang                                           *
#*                                                                        *
#*   All rights reserved.  This file is distributed under the terms of    *
#*   the GNU Lesser General Public License version 2.1, with the          *
#*   special exception on linking described in the file LICENSE.          *
#*                                                                        *
#**************************************************************************

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import time


class Peer:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=20)
        self.socket.settimeout(20)  # Watchdog, not a progress assertion.
        self.input = self.socket.makefile("rb")

    def send(self, text):
        self.socket.sendall(
            text.encode("ascii") if isinstance(text, str) else text)

    def line(self, expected):
        actual = self.input.readline(1024)
        assert actual == expected.encode("ascii"), (actual, expected)

    def request(self, text, expected):
        self.send(text + "\n")
        self.line(expected + "\n")

    def eof(self):
        try:
            assert self.input.read(1) == b"", "expected closed connection"
        except ConnectionResetError:
            pass

    def close(self):
        self.input.close()
        self.socket.close()


class Server:
    def __init__(self, executable, mode):
        read_fd, self.fault_fd = os.pipe()
        environment = dict(os.environ, ACTOR_KV_FAULT_FD=str(read_fd))
        if mode == "restart":
            environment["ACTOR_KV_PIN_PORT"] = "1"
        self.process = subprocess.Popen(
            [executable, mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=environment, pass_fds=(read_fd,), bufsize=0,
        )
        os.close(read_fd)
        self.pending = b""
        self.finished = False
        try:
            self.port = self.ready()
        except BaseException:
            self.close()
            raise

    def line(self):
        deadline = time.monotonic() + 30
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "server progress watchdog"
            readable, _, _ = select.select(
                [self.process.stdout], [], [], remaining)
            assert readable, "server output watchdog"
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                error = self.process.stderr.read().decode(errors="replace")
                raise AssertionError(
                    "server exited before expected output: " + error)
            self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return line.decode()

    def ready(self):
        while True:
            line = self.line()
            if line.startswith("READY "):
                return int(line.split()[1])
            assert line == "BLOCKED", line

    def peer(self):
        return Peer(self.port)

    def fault(self, count=1):
        os.write(self.fault_fd, bytes([count]))
        # Accept readiness causes the real backend to observe the fault pipe.
        trigger = self.peer()
        return trigger

    def finish(self):
        clean = False
        while True:
            line = self.line()
            if line.startswith("CLEAN "):
                clean = True
            elif line == "PASS":
                break
            else:
                assert line.startswith("READY ") or line == "BLOCKED", line
        assert clean
        assert self.process.wait(timeout=20) == 0, (
            self.process.stderr.read().decode())
        self.finished = True

    def close(self):
        os.close(self.fault_fd)
        if self.process.poll() is None:
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
        _, error = self.process.communicate(timeout=10)
        if error:
            print(error.decode(errors="replace"), flush=True)


def shutdown(server, peer):
    peer.request("SHUTDOWN", "BYE")
    peer.eof()
    peer.close()
    server.finish()


def normal(server):
    a, b = server.peer(), server.peer()
    a.request("PING", "PONG")
    b.request("PING", "PONG")
    a.send("SE")
    a.send("T shared hello\nGET shared\n")
    a.line("STORED\n")
    a.line("VALUE hello\n")
    b.request("GET shared", "VALUE hello")
    for i in range(7):
        a.request(f"SET key{i} value{i}", "STORED")
    a.request("SET overflow x", "FULL")
    a.request("SET shared updated", "STORED")
    a.request("DEL key0", "DELETED")
    a.request("DEL key0", "NOT_FOUND")
    a.request("SET new value", "STORED")
    a.request("GET absent", "NOT_FOUND")
    a.request("SET shared ", "STORED")
    a.request("GET shared", "VALUE ")
    a.request("SET shared updated", "STORED")
    a.request("DEL key1", "DELETED")
    long_key, long_value = "k" * 64, "v" * 512
    a.send(f"SET {long_key} {long_value}\r\n")
    a.line("STORED\n")
    a.request("GET " + long_key, "VALUE " + long_value)
    a.request("", "ERROR")
    a.send(b"SET bad a\x00b\nPING\r\n")
    a.line("ERROR\n")
    a.line("PONG\n")
    crashed = server.peer()
    crashed.send("CRASH\n")
    crashed.eof()
    crashed.close()
    b.request("GET shared", "VALUE updated")

    def pipeline_client(_):
        peer = server.peer()
        try:
            for _ in range(20):
                peer.send("PING\nGET shared\n")
                peer.line("PONG\n")
                peer.line("VALUE updated\n")
            peer.request("QUIT", "BYE")
            peer.eof()
        finally:
            peer.close()
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        list(executor.map(pipeline_client, range(2)))

    long = server.peer()
    long.send("x" * 700)
    long.line("ERROR\n")
    long.eof()
    long.close()
    partial = server.peer()
    partial.send("SET partial unfinished")
    partial.socket.shutdown(socket.SHUT_WR)
    partial.eof()
    partial.close()
    a.request("GET partial", "NOT_FOUND")
    holds = [server.peer(), server.peer()]
    for peer in holds:
        peer.request("PING", "PONG")
    rejected = server.peer()
    rejected.eof()
    rejected.close()
    for peer in holds:
        peer.request("QUIT", "BYE")
        peer.eof()
        peer.close()

    a.request("SET shared " + "z" * 512, "STORED")
    slow = server.peer()
    slow.send("GET shared\n" * 4000)
    while server.line() != "BLOCKED":
        pass
    b.request("PING", "PONG")
    slow.close()
    shutdown(server, b)
    a.eof()
    a.close()


def restart(server):
    a, b = server.peer(), server.peer()
    a.request("SET persist value", "STORED")
    b.request("PING", "PONG")
    original_port = server.port
    trigger = server.fault()
    server.port = server.ready()
    assert server.port == original_port, "listener restart changed its endpoint"
    for peer in (a, b, trigger):
        peer.eof()
        peer.close()
    replacement = server.peer()
    replacement.request("GET persist", "VALUE value")
    shutdown(server, replacement)


def timeout(server):
    peer = server.peer()
    peer.eof()
    peer.close()
    shutdown(server, server.peer())


def quota(server):
    peer = server.peer()
    peer.request("PING", "PONG")
    rejected = server.peer()
    rejected.eof()
    rejected.close()
    shutdown(server, peer)


def state(server):
    peer = server.peer()
    peer.request("SET persist value", "STORED")
    peer.send("CRASH_STATE\n")
    peer.eof()
    peer.close()
    server.finish()


def storm(server):
    trigger = server.fault(99)
    trigger.close()
    server.finish()


def descriptors(server):
    for _ in range(3):
        peer = server.peer()
        peer.eof()
        peer.close()
        if _ < 2:
            server.port = server.ready()
    server.finish()


def resource_quota(server):
    peer = server.peer()
    peer.eof()
    peer.close()
    storm(server)


def soak(server, seconds):
    deadline = time.monotonic() + seconds
    latencies = []
    peak_rss = 0

    def client(index):
        count = 0
        samples = []
        while time.monotonic() < deadline:
            started = time.monotonic()
            peer = server.peer()
            try:
                key = f"soak{index}"
                peer.send(f"PING\nSET {key} value\nGET {key}\nQUIT\n")
                for response in (
                        "PONG\n", "STORED\n", "VALUE value\n", "BYE\n"):
                    peer.line(response)
                peer.eof()
            finally:
                peer.close()
            count += 1
            if len(samples) < 10000:
                samples.append((time.monotonic() - started) * 1000)
        return count, samples

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(client, i) for i in range(2)]
        while not all(f.done() for f in futures):
            status = Path(f"/proc/{server.process.pid}/status").read_text()
            for line in status.splitlines():
                if line.startswith("VmRSS:"):
                    peak_rss = max(peak_rss, int(line.split()[1]))
            concurrent.futures.wait(futures, timeout=0.1)
        results = [f.result() for f in futures]
    elapsed = time.monotonic() - started
    cycles = sum(result[0] for result in results)
    for _, samples in results:
        latencies.extend(samples)
    assert cycles >= 2
    latencies.sort()
    def percentile(fraction):
        return latencies[min(
            len(latencies) - 1, int(len(latencies) * fraction))]
    shutdown(server, server.peer())
    return {"seconds": round(elapsed, 3), "connections": cycles,
            "commands": cycles * 4,
            "commands_per_second": round(cycles * 4 / elapsed, 2),
            "connection_roundtrip_ms_p50": round(percentile(0.50), 3),
            "connection_roundtrip_ms_p95": round(percentile(0.95), 3),
            "connection_roundtrip_ms_p99": round(percentile(0.99), 3),
            "peak_process_rss_kib": peak_rss}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--report")
    parser.add_argument("--only", help="Comma-separated scenario names")
    args = parser.parse_args()
    assert 0 < args.seconds <= 86400
    scenarios = [("normal", normal), ("restart", restart), ("timeout", timeout),
                 ("quota", quota), ("state", state), ("storm", storm),
                 ("descriptors", descriptors), ("mailboxes", descriptors),
                 ("buffers", resource_quota),
                 ("operations", resource_quota),
                 ("soak", lambda s: soak(s, args.seconds))]
    if args.only:
        names = set(args.only.split(","))
        if not names <= {name for name, _ in scenarios}:
            parser.error("unknown scenario in --only")
        scenarios = [(name, run) for name, run in scenarios if name in names]
    # Failed startup must release the process pipes and control descriptor too.
    before = len(os.listdir("/proc/self/fd"))
    try:
        Server("/bin/false", "startup-failure")
    except AssertionError:
        pass
    else:
        raise AssertionError("startup-failure fixture unexpectedly succeeded")
    assert len(os.listdir("/proc/self/fd")) == before, (
        "startup fixture leaked descriptors")
    report = {}
    for name, run in scenarios:
        server = Server(args.executable, name)
        try:
            report[name] = run(server) or "passed"
            assert server.finished
            print(name + ": passed", flush=True)
        finally:
            server.close()
    output = json.dumps(report, indent=2)
    print(output, flush=True)
    if args.report:
        Path(args.report).write_text(output + "\n")


if __name__ == "__main__":
    main()
