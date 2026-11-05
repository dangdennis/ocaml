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
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/ocaml-actor-service.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM
for unit in kv_protocol kv_state kv_connection kv_listener kv_service; do
  cp "$repo_root/examples/actor-kv/$unit.ml" "$work_dir/"
done
for source in protocol_fuzz.ml service_unit.ml service_harness.ml \
  service_stubs.c; do
  cp "$repo_root/testsuite/actor-service/$source" "$work_dir/"
done
cd "$work_dir"
runtime="$repo_root/runtime/ocamlrun"
variant=${ACTOR_SERVICE_RUNTIME:-}
case "$variant" in
  ""|d|i) ;;
  *) echo "invalid ACTOR_SERVICE_RUNTIME" >&2; exit 2;;
esac
actor_runtime="$repo_root/runtime/ocamlrun$variant"
compiler="$repo_root/ocamlc"
"$runtime" "$compiler" -g -I "$repo_root/stdlib" -I "$repo_root/runtime" \
  -o protocol_fuzz.byte kv_protocol.ml protocol_fuzz.ml
"$actor_runtime" protocol_fuzz.byte
"$runtime" "$compiler" -g -I "$repo_root/stdlib" \
  -o service_unit.byte kv_protocol.ml kv_state.ml kv_connection.ml \
  kv_listener.ml kv_service.ml service_unit.ml
timeout 20 "$actor_runtime" service_unit.byte
"$runtime" "$compiler" -g -I "$repo_root/stdlib" -I "$repo_root/runtime" \
  -runtime-variant "$variant" -custom -o service_harness.byte \
  kv_protocol.ml kv_state.ml kv_connection.ml \
  kv_listener.ml kv_service.ml service_stubs.c service_harness.ml
python3 "$repo_root/testsuite/actor-service/clients.py" \
  "$work_dir/service_harness.byte" "$@"
