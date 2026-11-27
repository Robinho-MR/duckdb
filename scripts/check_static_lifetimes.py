#!/usr/bin/env python3
"""Fails if first-party code needs a global constructor or an exit-time destructor.

Queries can still be running on other threads while the process exits and static destructors run, so a static that has
a destructor can be used after it was destroyed. Global constructors run before main in an unspecified order across
translation units. clang finds both with -Wexit-time-destructors and -Wglobal-constructors.

How to fix a report:
  - prefer a trivially destructible type (constexpr data, a plain array, an enum, ...)
  - otherwise make it a function-local static wrapped in NoDestructor<T> (duckdb/common/no_destructor.hpp)
  - if a destructor really must run (thread_local cleanup) or a constructor must run before main (registration),
    wrap the declaration in DUCKDB_ALLOW_STATIC_LIFETIME_BEGIN / DUCKDB_ALLOW_STATIC_LIFETIME_END and say why

Usage: check_static_lifetimes.py <build_dir> [--clang clang++] [--jobs N]
<build_dir> has to be configured with clang and CMAKE_EXPORT_COMPILE_COMMANDS=ON (make static-lifetime-check does this).
"""

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

WARNINGS = ["-Wexit-time-destructors", "-Wglobal-constructors"]
DIAGNOSTIC = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): warning: (?P<message>.*) \[-W(?:exit-time-destructors|global-constructors)\]$"
)
# vendored code is not ours to change
IGNORED_PATH_PARTS = ("/third_party/",)
DROPPED_FLAGS = {"-c", "-MD", "-MMD"}
DROPPED_FLAGS_WITH_ARGUMENT = {"-o", "-MF", "-MT", "-MQ"}


def find_clang(requested):
    if requested:
        return requested
    for name in ("clang++-20", "clang++-19", "clang++-18", "clang++"):
        if shutil.which(name):
            return name
    sys.exit("no clang++ found, pass --clang")


def check_file(entry, clang):
    args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    command = [clang]
    skip_next = False
    for arg in args[1:]:
        if skip_next:
            skip_next = False
        elif arg in DROPPED_FLAGS_WITH_ARGUMENT:
            skip_next = True
        elif arg not in DROPPED_FLAGS:
            command.append(arg)
    command += WARNINGS + ["-fsyntax-only", "-fno-color-diagnostics"]
    result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
    diagnostics = set()
    for line in result.stderr.splitlines():
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        path = os.path.normpath(os.path.join(entry["directory"], match.group("file")))
        if any(part in path for part in IGNORED_PATH_PARTS):
            continue
        diagnostics.add("%s:%s:%s: %s" % (path, match.group("line"), match.group("col"), match.group("message")))
    errors = [line for line in result.stderr.splitlines() if ": error:" in line]
    return diagnostics, errors, entry["file"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir")
    parser.add_argument("--clang")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()

    clang = find_clang(args.clang)
    with open(os.path.join(args.build_dir, "compile_commands.json")) as handle:
        entries = json.load(handle)
    entries = [
        e
        for e in entries
        if e["file"].endswith((".cpp", ".cc")) and not any(p in e["file"] for p in IGNORED_PATH_PARTS)
    ]
    if not entries:
        sys.exit("no translation units found in compile_commands.json")

    diagnostics = set()
    failed = []
    with ThreadPoolExecutor(args.jobs) as pool:
        for found, errors, path in pool.map(lambda e: check_file(e, clang), entries):
            diagnostics |= found
            if errors:
                failed.append((path, errors[0]))

    for path, error in failed:
        print("could not check %s: %s" % (path, error), file=sys.stderr)
    root = os.getcwd() + os.sep
    for diagnostic in sorted(diagnostics):
        print(diagnostic.replace(root, ""))
    print("%d translation units checked, %d static lifetime problems" % (len(entries), len(diagnostics)))
    if diagnostics:
        print(__doc__.split("How to fix a report:")[1].split("Usage:")[0].rstrip(), file=sys.stderr)
    return 1 if diagnostics or failed else 0


if __name__ == "__main__":
    sys.exit(main())
