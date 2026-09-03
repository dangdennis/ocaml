#!/bin/sh

set -eu

if test "$#" -ne 1; then
  echo "usage: gen_actor_primitive_policy.sh POLICY.def" >&2
  exit 2
fi

awk '
function trim(value) {
  sub(/^[[:space:]]+/, "", value)
  sub(/[[:space:]]+$/, "", value)
  return value
}
function unquote(value) {
  value = trim(value)
  if (value !~ /^"[^"]+"$/) fail("expected a non-empty quoted string")
  return substr(value, 2, length(value) - 2)
}
function fail(message) {
  printf "%s:%d: %s\n", FILENAME, FNR, message > "/dev/stderr"
  exit 2
}
BEGIN {
  print "(**************************************************************************)"
  print "(* Generated from runtime/actor_primitive_policy.def.  Do not edit.       *)"
  print "(**************************************************************************)"
  print ""
  print "type capability = Pure | Actor_local | Scheduler_aware | Forbidden"
  print "type entry = {"
  print "  name : string;"
  print "  arity : int;"
  print "  capability : capability;"
  print "  family : string;"
  print "  audit : string;"
  print "}"
  print "type classification ="
  print "  | Allowed of entry"
  print "  | Denied of entry"
  print "  | Arity_mismatch of entry"
  print "  | Unknown"
  print ""
  print "let entries = [|"
}
/^[[:space:]]*ACTOR_PRIMITIVE\(/ {
  line = $0
  sub(/^[[:space:]]*ACTOR_PRIMITIVE\(/, "", line)
  sub(/\)[[:space:]]*$/, "", line)
  if (split(line, field, /,[[:space:]]*/) != 6)
    fail("primitive entry must contain exactly six comma-free fields")
  name = unquote(field[1])
  arity = trim(field[3])
  capability = trim(field[4])
  family = trim(field[5])
  audit = unquote(field[6])
  if (arity !~ /^[1-5]$/) fail("arity must be between one and five")
  if (capability == "PURE") ocaml_capability = "Pure"
  else if (capability == "ACTOR_LOCAL") ocaml_capability = "Actor_local"
  else if (capability == "SCHEDULER_AWARE") ocaml_capability = "Scheduler_aware"
  else if (capability == "FORBIDDEN") ocaml_capability = "Forbidden"
  else fail("unknown capability " capability)
  key = name "/" arity
  if (seen[key]++) fail("duplicate primitive policy entry " key)
  if (seen_name[name]++) fail("duplicate primitive policy name " name)
  if (family !~ /^[a-z][a-z0-9_]*$/) fail("invalid primitive family")
  printf "  { name = %c%s%c; arity = %s; capability = %s;\n", 34, name, 34, arity, ocaml_capability
  printf "    family = %c%s%c; audit = %c%s%c };\n", 34, family, 34, 34, audit, 34
  count++
  next
}
/^[[:space:]]*(\/\*|\*|\*\/|$)/ { next }
{ fail("unrecognized policy syntax") }
END {
  if (count == 0) fail("policy contains no entries")
  print "|]"
  print ""
  print "let find name ="
  print "  let rec loop index ="
  print "    if index = Array.length entries then None"
  print "    else if entries.(index).name = name then Some entries.(index)"
  print "    else loop (index + 1)"
  print "  in"
  print "  loop 0"
  print ""
  print "let classify name arity ="
  print "  match find name with"
  print "  | None -> Unknown"
  print "  | Some entry ->"
  print "      if entry.arity <> arity then Arity_mismatch entry"
  print "      else if entry.capability = Forbidden then Denied entry"
  print "      else Allowed entry"
  print ""
  print "let capability_name = function"
  print "  | Pure -> \"pure\""
  print "  | Actor_local -> \"actor-local\""
  print "  | Scheduler_aware -> \"scheduler-aware\""
  print "  | Forbidden -> \"forbidden\""
}
' "$1"
