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
variant=${ACTOR_ISOLATION_RUNTIME:-}
case "$variant" in
  ""|d|i) ;;
  *) echo "invalid ACTOR_ISOLATION_RUNTIME" >&2; exit 2;;
esac
for test in isolated_arenas private_copying_gc frozen_global_mutation_fence \
  wire_codec actor_heap_isolation actor_service_fuzz; do
  make -C "$repo_root/testsuite" one \
    TEST="tests/runtime-actors/$test.ml" USE_RUNTIME="$variant"
done
