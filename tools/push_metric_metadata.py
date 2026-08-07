#!/usr/bin/env python3
"""Push short_name/unit/description metadata for every barkboard.* metric
(docs/datadog/metrics.json) so Datadog's own UI — dashboards, the metrics
explorer, monitor creation — shows a real label and unit instead of a bare
gauge value.

Firmware running dd::pushMetricMetadataIfNeeded() (src/datadog.cpp) already
does this itself, once per firmware version, whenever device metrics are
enabled in Settings — so most users never need this script at all. It's kept
around for: a device still running an older firmware build from before that
existed, re-pushing after hand-editing metrics.json without bumping
BARKBOARD_VERSION (the on-device version-gate wouldn't notice a text-only
change), or pushing against an org without flashing/enabling anything on a
real device at all. Keep docs/datadog/metrics.json and the METRIC_METADATA
array in src/datadog.cpp in sync by hand if you change either one.

`unit` values are validated against Datadog's own metric-unit list — a typo
here fails loudly (HTTP 404 "unit not found") rather than silently applying
nothing, and every unit id currently in metrics.json (byte, percent, second,
request, decibel-milliwatt) has already been confirmed live against that
list.

Usage, run from repo root (reads DD_API_KEY/DD_APP_KEY from .env or the
environment, same convention as tools/fetch_dd.py):
  python3 tools/push_metric_metadata.py
  python3 tools/push_metric_metadata.py --site datadoghq.eu
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV_FILE = ROOT / ".env"
METRICS_FILE = ROOT / "docs" / "datadog" / "metrics.json"


def load_env(path: Path) -> dict:
    env = {}
    if not path.exists():
        return env
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def push_one(site: str, api_key: str, app_key: str, entry: dict) -> None:
    metric = entry["metric"]
    body = {k: v for k, v in entry.items() if k != "metric"}
    req = urllib.request.Request(
        f"https://api.{site}/api/v1/metrics/{metric}",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "DD-API-KEY": api_key,
            "DD-APPLICATION-KEY": app_key,
            "Content-Type": "application/json",
        },
        method="PUT",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            resp.read()
        print(f"ok    {metric}")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        print(f"FAIL  {metric} -> HTTP {e.code}: {detail}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", default=os.environ.get("DD_SITE", "datadoghq.com"),
                     help="Datadog site host (default: datadoghq.com, or $DD_SITE)")
    ap.add_argument("--metrics-file", default=str(METRICS_FILE),
                     help=f"path to the metadata definitions (default: {METRICS_FILE})")
    args = ap.parse_args()

    env = load_env(ENV_FILE)
    api_key = os.environ.get("DD_API_KEY") or env.get("DD_API_KEY")
    app_key = os.environ.get("DD_APP_KEY") or env.get("DD_APP_KEY")
    if not api_key or not app_key:
        print(f"error: DD_API_KEY / DD_APP_KEY not found in {ENV_FILE} or the environment", file=sys.stderr)
        sys.exit(1)

    entries = json.loads(Path(args.metrics_file).read_text())
    for entry in entries:
        push_one(args.site, api_key, app_key, entry)


if __name__ == "__main__":
    main()
