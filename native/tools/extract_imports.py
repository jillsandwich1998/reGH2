#!/usr/bin/env python3
"""Kernel imports used by the generated code = the `{ addr, __imp__Name }` thunk table of generated/default/gh2_init.cpp.

  extract_imports.py GENERATED_DIR            print the sorted names
  extract_imports.py GENERATED_DIR --write F  write them to F (native/rt/abi/contract/imports.txt)
  extract_imports.py GENERATED_DIR --check F  exit 1 if F differs (the contract must follow the generated code)
"""
import pathlib
import re
import sys


def imports(gen_dir):
    text = (pathlib.Path(gen_dir) / "gh2_init.cpp").read_text(errors="replace")
    names = set(re.findall(r"\{\s*0x[0-9A-Fa-f]{8},\s*__imp__([A-Za-z0-9_]+)\s*\}", text))
    return sorted(names)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    names = imports(sys.argv[1])
    body = "\n".join(names) + "\n"
    if "--write" in sys.argv:
        pathlib.Path(sys.argv[sys.argv.index("--write") + 1]).write_text(body)
        print("wrote %d imports" % len(names))
    elif "--check" in sys.argv:
        want = pathlib.Path(sys.argv[sys.argv.index("--check") + 1]).read_text()
        if want != body:
            have, now = set(want.split()), set(names)
            print("imports.txt is out of date: only in generated code: %s | only in contract: %s" %
                  (sorted(now - have), sorted(have - now)))
            sys.exit(1)
        print("imports.txt matches the generated code (%d imports)" % len(names))
    else:
        sys.stdout.write(body)


if __name__ == "__main__":
    main()
