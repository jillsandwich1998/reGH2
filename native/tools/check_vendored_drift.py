#!/usr/bin/env python3
"""Do the vendored headers (native/rt/abi/shim/rex) still match the SDK source they were copied from?

  check_vendored_drift.py [SDK_INCLUDE_DIR]     compare (default: $GH2_SDK_INCLUDE, work/sdk-src/include, work/sdk-built/linux-amd64/include)
  check_vendored_drift.py --update [DIR]        rewrite func.h.expected.diff from the current copy of func.h

Verbatim copies must be byte-identical; func.h must differ from the SDK's by exactly native/rt/abi/func.h.expected.diff.
Exit 0 = no drift (or no SDK source to compare with: reported, not an error), 1 = drift.
"""
import difflib
import os
import pathlib
import sys

NATIVE = pathlib.Path(__file__).resolve().parents[1]
SHIM = NATIVE / "rt/abi/shim"
EXPECTED = NATIVE / "rt/abi/func.h.expected.diff"
VERBATIM = ["rex/ppc/context.h", "rex/ppc/intrinsics.h", "rex/types.h", "rex/platform/fpscr.h", "rex/image_info.h"]
MODIFIED = "rex/ppc/func.h"


def sdk_dir(args):
    cands = [a for a in args if not a.startswith("--")] + [os.environ.get("GH2_SDK_INCLUDE", "")]
    work = NATIVE.parents[1]
    cands += [str(work / "sdk-src/include"), str(work / "sdk-built/linux-amd64/include")]
    for c in cands:
        if c and (pathlib.Path(c) / MODIFIED).exists():
            return pathlib.Path(c)
    return None


def diff_text(sdk, mine):
    a = (sdk / MODIFIED).read_text().splitlines(keepends=True)
    b = (SHIM / MODIFIED).read_text().splitlines(keepends=True)
    return "".join(difflib.unified_diff(a, b, "sdk/" + MODIFIED, "shim/" + MODIFIED, n=2))


def main():
    sdk = sdk_dir(sys.argv[1:])
    if sdk is None:
        print("no SDK source found to compare with (set GH2_SDK_INCLUDE): nothing checked")
        return 0
    if "--update" in sys.argv:
        EXPECTED.write_text(diff_text(sdk, None))
        print("wrote %s (%d lines)" % (EXPECTED.name, len(EXPECTED.read_text().splitlines())))
        return 0
    bad = []
    for rel in VERBATIM:
        if (sdk / rel).read_bytes() != (SHIM / rel).read_bytes():
            bad.append("%s differs from the SDK's copy" % rel)
    if not EXPECTED.exists() or diff_text(sdk, None) != EXPECTED.read_text():
        bad.append("%s differs from the SDK's in more (or other) ways than %s" % (MODIFIED, EXPECTED.name))
    print("compared against %s: %s" % (sdk, "DRIFT" if bad else "no drift (%d verbatim files + func.h with its expected diff)" % len(VERBATIM)))
    for b in bad:
        print("  - " + b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
