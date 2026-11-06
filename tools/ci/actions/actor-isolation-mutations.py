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

"""Run isolation guard mutations in a disposable copy of a built checkout."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, output):
    with output.open("w") as log:
        return subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                              timeout=180, check=False).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report-dir", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    reports = Path(args.report_dir).resolve()
    reports.mkdir(parents=True, exist_ok=True)
    mutations = [
        ("foreign-edge", "runtime/actor_heap.c",
         "result->error = CAML_ACTOR_HEAP_VERIFY_FOREIGN_EDGE;",
         "result->error = CAML_ACTOR_HEAP_VERIFY_OK;"),
        ("shared-allocation", "runtime/shared_heap.c",
         "&& domain_state->actor_heap != NULL)",
         "&& domain_state->actor_heap != NULL && 0)"),
        ("foreign-store", "runtime/actor_heap.c",
         "if (!target_is_field) return CAML_ACTOR_HEAP_STORE_INVALID;",
         "if (0 && !target_is_field) "
         "return CAML_ACTOR_HEAP_STORE_INVALID;"),
    ]
    with tempfile.TemporaryDirectory(prefix="actor-isolation-mutations-") as d:
        scratch = Path(d) / "ocaml"
        if scratch.is_relative_to(root):
            raise RuntimeError("TMPDIR must be outside the source checkout")
        shutil.copytree(root, scratch, symlinks=True,
                        ignore=shutil.ignore_patterns(".git", "_ocamltest"))
        test = ["env", "OCAMLSRCDIR=" + str(scratch), "make", "-C",
                str(scratch / "testsuite"), "one",
                "OCAMLTESTDIR=" + str(scratch / "testsuite/_ocamltest"),
                "TEST=tests/runtime-actors/isolated_arenas.ml",
                "USE_RUNTIME=", "KEEP=true"]
        if run(test, reports / "baseline.log") != 0:
            raise RuntimeError("unmodified isolation baseline failed")
        originals = {name: (scratch / name).read_text()
                     for _, name, _, _ in mutations}
        for label, name, old, new in mutations:
            for source, text in originals.items():
                (scratch / source).write_text(text)
            text = originals[name]
            if text.count(old) != 1:
                raise RuntimeError("mutation anchor is not unique: " + label)
            (scratch / name).write_text(text.replace(old, new))
            build = ["make", "-C", str(scratch), "-j2",
                     "runtime/ocamlrun", "runtime/libcamlrun.a",
                     "runtime/libcamlrun_non_shared.a"]
            if run(build, reports / (label + "-build.log")) != 0:
                raise RuntimeError("mutant did not compile: " + label)
            result = run(test, reports / (label + ".log"))
            details = list((scratch / "testsuite/_ocamltest").rglob("*.log"))
            evidence = "\n".join(p.read_text(errors="replace")
                                 for p in details)
            (reports / (label + "-details.log")).write_text(evidence)
            if result == 0 or "arena check" not in evidence:
                raise RuntimeError("guard mutation was not caught: " + label)
            print(label + ": caught by isolation assertions", flush=True)
        # Recover and rerun so the final evidence includes a clean baseline.
        for source, text in originals.items():
            (scratch / source).write_text(text)
        if run(build, reports / "restored-build.log") != 0:
            raise RuntimeError("restored runtime did not compile")
        if run(test, reports / "restored.log") != 0:
            raise RuntimeError("restored isolation gate failed")
        print("restored baseline: passed", flush=True)


if __name__ == "__main__":
    main()
