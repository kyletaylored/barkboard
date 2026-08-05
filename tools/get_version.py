# PlatformIO pre-build extra script (see platformio.ini's extra_scripts) —
# stamps the firmware with a build-time version string derived from git, so
# it's visible on-device (the Bits idle screen) without hand-editing a
# version file before every release. Uses `git describe`, not a static
# VERSION file: it's already the source of truth for "how far past the last
# tag is this build" and needs no separate bump step to stay in sync with
# the tags CI actually cuts.
#
# Output shape:
#   v1.2.0            - exactly on a tag, clean tree
#   v1.2.0-3-gabc1234 - 3 commits past tag v1.2.0, at commit abc1234
#   v1.2.0-dirty       / v1.2.0-3-gabc1234-dirty - uncommitted changes present
#   abc1234           - no tags exist in the repo at all yet (--always fallback)
#   unknown           - git itself isn't available (shouldn't happen in CI
#                       or on a real dev machine, but build must not fail)
#
# CI note: actions/checkout defaults to a shallow clone (fetch-depth: 1),
# which has no tag history at all — `git describe --tags` would silently
# fall through to the --always short-SHA case even right after tagging a
# release. Both .github/workflows/build.yml and pages.yml set
# `fetch-depth: 0` on their checkout step specifically so this script sees
# real tags in CI, not just on a full local clone.
Import("env")
import subprocess


def get_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env["PROJECT_DIR"],
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip()
    except Exception:
        return "unknown"


version = get_version()
print("[get_version] BARKBOARD_VERSION = %s" % version)
env.Append(CPPDEFINES=[("BARKBOARD_VERSION", '\\"%s\\"' % version)])
