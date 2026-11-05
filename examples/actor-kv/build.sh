#!/bin/sh

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

set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/_actor-kv"}
mkdir -p "$build_dir"
for unit in kv_protocol kv_state kv_connection kv_listener kv_service main; do
  cp "$repo_root/examples/actor-kv/$unit.ml" "$build_dir/"
done
cd "$build_dir"
"$repo_root/runtime/ocamlrun" "$repo_root/ocamlc" -g -I "$repo_root/stdlib" \
  -o actor-kv.byte kv_protocol.ml kv_state.ml kv_connection.ml kv_listener.ml \
  kv_service.ml main.ml
