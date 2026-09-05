#!/usr/bin/env python3
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

"""Serve the actor trace viewer and one local NDJSON trace."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    viewer = Path(__file__).with_name("viewer.html").read_bytes()
    trace = args.trace.resolve()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path in ("/", "/viewer.html"):
                body, content_type = viewer, "text/html; charset=utf-8"
            elif self.path == "/trace":
                try:
                    body = trace.read_bytes()
                except FileNotFoundError:
                    body = b""
                content_type = "application/x-ndjson; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *values: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Actor trace viewer: http://127.0.0.1:{args.port}")
    print(f"Trace: {trace}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
