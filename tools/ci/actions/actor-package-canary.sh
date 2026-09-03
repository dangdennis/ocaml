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

astring_version=0.8.5
astring_sha256=865692630c07c3ab87c66cdfc2734c0fdfc9c34a57f8e89ffec7c7d15e7a70fa
astring_url=https://erratique.ch/software/astring/releases/astring-0.8.5.tbz

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/ocaml-actor-package-canary.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

archive=${ASTRING_ARCHIVE:-"$work_dir/astring-$astring_version.tbz"}
if test -z "${ASTRING_ARCHIVE:-}"; then
  curl --fail --location --silent --show-error \
    "$astring_url" --output "$archive"
fi

if command -v sha256sum >/dev/null 2>&1; then
  echo "$astring_sha256  $archive" | sha256sum --check --status
else
  actual_sha256=$(shasum -a 256 "$archive" | awk '{ print $1 }')
  test "$actual_sha256" = "$astring_sha256"
fi

tar xjf "$archive" -C "$work_dir"
source_dir="$work_dir/astring-$astring_version/src"
runtime="$repo_root/runtime/ocamlrun"
compiler="$repo_root/ocamlc"

cd "$source_dir"
for unit in \
  astring_unsafe.ml \
  astring_base.ml \
  astring_escape.ml \
  astring_char.ml \
  astring_sub.ml \
  astring_string.ml
do
  "$runtime" "$compiler" -I "$repo_root/stdlib" -c "$unit"
done
"$runtime" "$compiler" -I "$repo_root/stdlib" -I . -c astring.mli
"$runtime" "$compiler" -I "$repo_root/stdlib" -I . -c astring.ml

"$runtime" "$compiler" -I "$repo_root/stdlib" -I . \
  -o "$work_dir/astring_actor.byte" \
  astring_unsafe.cmo \
  astring_base.cmo \
  astring_escape.cmo \
  astring_char.cmo \
  astring_sub.cmo \
  astring_string.cmo \
  astring.cmo \
  "$repo_root/testsuite/package-canaries/astring_actor.ml"

"$runtime" "$work_dir/astring_actor.byte"
