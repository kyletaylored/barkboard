#!/usr/bin/env python3
"""Inspect what the device would actually fetch from Datadog, per screen —
same endpoints, same query construction (team scope, teams scope, tags_query)
as src/datadog.cpp — without needing to flash first. Pulls DD_API_KEY/
DD_APP_KEY from .env at the repo root (or the environment); never prints
their values.

Subcommands mirror the data screens:
  monitors        /api/v1/monitor/search — grouped by type, raw + extracted
                  chart query (see Monitor Detail's dd::fetchMonitorChartSeries())
  incidents       /api/v2/incidents/search?query=state:active[ teams:<team>]
  oncall          /api/v2/team[?filter[keyword]=<team>], then
                  /api/v2/on-call/teams/{id}/on-call per team
  slos            /api/v1/slo/search[?query=team:<team>] — status/state
                  included inline (see fetchSlos() in src/datadog.cpp)
  event           /api/v2/events/search?query=source:alert @monitor.id:<id> —
                  a monitor's most recent alert event, full raw JSON. This is
                  the ONLY thing the firmware's Events API call (inside
                  triggerBitsInvestigation()) actually looks up today, and it
                  only reads two fields out of it (evt.id, timestamp) — this
                  subcommand shows you everything else in there too (whether
                  there's chart/snapshot data worth using, etc).
  investigations  GET /api/unstable/bits-ai/investigation/search?query=team:<team>
                  — an undocumented endpoint (the web UI's own investigations
                  page uses it) that happens to accept the same DD-API-KEY/
                  DD-APPLICATION-KEY auth as everything else here; see
                  cmd_investigations()'s doc comment for the "can change
                  without notice" caveat.
  investigation   GET /api/v2/bits-ai/investigations/<uuid> — the documented,
                  stable one — full detail on a single investigation by id.

Examples, run from repo root:
  python3 tools/fetch_dd.py monitors
  python3 tools/fetch_dd.py monitors --team my-team --raw   # full JSON per monitor
  python3 tools/fetch_dd.py monitors --verify --type "log alert"
  python3 tools/fetch_dd.py incidents --team my-team
  python3 tools/fetch_dd.py oncall
  python3 tools/fetch_dd.py oncall --team-id 12345   # test the on-call
                                                       # endpoint shape even
                                                       # with zero teams
  python3 tools/fetch_dd.py slos --team my-team
  python3 tools/fetch_dd.py event --monitor-id 311012240
  python3 tools/fetch_dd.py investigations --team my-team
  python3 tools/fetch_dd.py investigation --id 1dca0bec-e879-4bf7-a357-6bf29a89a286
  python3 tools/fetch_dd.py investigation --id 1dca0bec-e879-4bf7-a357-6bf29a89a286 --summary-only
  python3 tools/fetch_dd.py monitors --site datadoghq.eu

Via the Makefile, extra flags go after `--`:
  make fetch-monitors -- --team my-team --raw
  make fetch-event -- --monitor-id 311012240
  make fetch-investigations -- --team my-team
  make fetch-investigation -- --id 1dca0bec-e879-4bf7-a357-6bf29a89a286 --summary-only
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV_FILE = ROOT / ".env"

# Monitor types with no metrics query at all — kept in sync by hand with
# isChartableMonitorType() in src/datadog.cpp.
NOT_CHARTABLE_TYPES = {
    "log alert", "synthetics alert", "event alert", "event-v2 alert",
    "process alert", "service check", "trace-analytics alert",
    "rum alert", "audit alert", "error-tracking alert", "composite",
    "ci-pipelines alert", "ci-tests alert",
}

# Detection-function wrappers whose first argument is the real metric query
# — kept in sync by hand with extractChartableQuery() in src/datadog.cpp.
WRAPPERS = ("anomalies(", "outliers(", "forecast(", "raw_forecast(", "change(", "pct_change(")


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


def bare_team(team: str) -> str:
    """Same normalization as bareTeamScope() in src/datadog.cpp — strips a
    leading team:/teams: someone might paste in out of habit."""
    if not team:
        return ""
    t = team.strip()
    low = t.lower()
    if low.startswith("teams:"):
        t = t[6:]
    elif low.startswith("team:"):
        t = t[5:]
    return t.strip()


def extract_chartable_query(raw: str) -> str:
    """Python port of extractChartableQuery() in src/datadog.cpp — keep the
    two in sync by hand if you change the extraction logic on-device."""
    q = raw.strip()

    # Drop leading "<time_aggr>(<window>):" prefix, e.g. "avg(last_5m):".
    first_paren = q.find("(")
    close_colon = q.find("):")
    if first_paren >= 0 and close_colon > first_paren:
        q = q[close_colon + 2:].strip()

    # Drop trailing " <comparison> <threshold>" at depth 0.
    depth = 0
    cmp_at = -1
    for i, c in enumerate(q):
        if c in "({":
            depth += 1
        elif c in ")}":
            depth -= 1
        elif depth == 0 and c in "><":
            cmp_at = i
        elif depth == 0 and c == "=" and i > 0 and q[i - 1] in "=!":
            cmp_at = i - 1
    if cmp_at > 0:
        q = q[:cmp_at].strip()

    # Detection-function wrapper: pull the first argument.
    for w in WRAPPERS:
        if q.startswith(w):
            d = 0
            arg_end = -1
            for i in range(len(w), len(q)):
                c = q[i]
                if c in "({":
                    d += 1
                elif c in ")}":
                    if d == 0:
                        arg_end = i
                        break
                    d -= 1
                elif c == "," and d == 0:
                    arg_end = i
                    break
            if arg_end > len(w):
                q = q[len(w):arg_end].strip()
            break

    return q


def dd_get(site, api_key, app_key, path, params=None):
    url = f"https://api.{site}{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={
        "DD-API-KEY": api_key,
        "DD-APPLICATION-KEY": app_key,
        "Accept": "application/json",
    })
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode("utf-8"))


def cmd_monitors(dd, args):
    team = bare_team(args.team)
    query = f"team:{team}" if team else ""
    print(f"query: {query!r}")

    try:
        # monitor/search, not the plain /api/v1/monitor list — matches how
        # the firmware actually fetches (server-side team scoping), and is
        # what --team needs to filter on at all.
        resp = dd_get(*dd, "/api/v1/monitor/search", {"query": query, "per_page": args.limit})
    except urllib.error.HTTPError as e:
        print(f"error: GET /api/v1/monitor/search -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    monitors = resp.get("monitors", [])
    if args.type:
        monitors = [m for m in monitors if args.type.lower() in (m.get("type") or "").lower()]

    if args.raw:
        for m in monitors:
            print(f"\n=== #{m.get('id')}  {m.get('name', '')!r} — raw JSON ===")
            print(json.dumps(m, indent=2))
        print(f"\n{len(monitors)} monitor(s) total.")
        return

    by_type = {}
    for m in monitors:
        by_type.setdefault(m.get("type", "?"), []).append(m)

    now = int(time.time())
    frm = now - 3600

    for mtype in sorted(by_type):
        group = by_type[mtype]
        chartable = mtype not in NOT_CHARTABLE_TYPES
        print(f"\n=== {mtype} ({len(group)}) — {'chartable type' if chartable else 'NOT a chartable type (no metrics query)'} ===")
        for m in group:
            raw_query = m.get("query", "")
            print(f"\n  #{m['id']}  {m['name'][:70]!r}")
            print(f"    raw query:       {raw_query}")
            if not chartable:
                continue
            extracted = extract_chartable_query(raw_query)
            print(f"    extracted query: {extracted}")
            if args.verify and extracted:
                try:
                    vresp = dd_get(*dd, "/api/v1/query", {"query": extracted, "from": frm, "to": now})
                    if vresp.get("status") == "ok" and vresp.get("series"):
                        n = len(vresp["series"][0].get("pointlist", []))
                        print(f"    /api/v1/query:   ok, {n} points")
                    else:
                        print(f"    /api/v1/query:   status={vresp.get('status')} error={vresp.get('error')!r}")
                except urllib.error.HTTPError as e:
                    print(f"    /api/v1/query:   HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:200]}")

    print(f"\n{len(monitors)} monitor(s) total across {len(by_type)} type(s). Pass --raw to see the full JSON per monitor.")


def cmd_incidents(dd, args):
    team = bare_team(args.team)
    query = "state:active"
    if team:
        query += f" teams:{team}"
    print(f"query: {query!r}")

    try:
        resp = dd_get(*dd, "/api/v2/incidents/search", {"query": query, "page[size]": args.limit})
    except urllib.error.HTTPError as e:
        print(f"error: GET /api/v2/incidents/search -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    incidents = resp.get("data", {}).get("attributes", {}).get("incidents", [])
    for wrapper in incidents:
        item = wrapper.get("data", {})
        a = item.get("attributes", {})
        commander = a.get("commander", {}).get("data", {}).get("attributes", {}).get("name")
        services = a.get("fields", {}).get("services", {}).get("value", [])
        print(f"\n  #{item.get('id')}  {a.get('title', '')!r}")
        print(f"    severity: {a.get('severity')}  state: {a.get('state')}  commander: {commander}")
        print(f"    services: {services}")

    total = resp.get("data", {}).get("attributes", {}).get("total")
    print(f"\n{len(incidents)} incident(s) returned (total matching: {total}).")


def cmd_oncall(dd, args):
    if args.team_id:
        print(f"Testing /api/v2/on-call/teams/{args.team_id}/on-call directly (skipping team lookup)...")
        try:
            resp = dd_get(*dd, f"/api/v2/on-call/teams/{args.team_id}/on-call")
            print(json.dumps(resp, indent=2)[:4000])
        except urllib.error.HTTPError as e:
            print(f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')}")
        return

    team = bare_team(args.team)
    params = {"page[size]": args.limit}
    if team:
        params["filter[keyword]"] = team
    try:
        resp = dd_get(*dd, "/api/v2/team", params)
    except urllib.error.HTTPError as e:
        print(f"error: GET /api/v2/team -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    teams = resp.get("data", [])
    print(f"{len(teams)} team(s) found" + (f" matching '{team}'" if team else "") + ".")
    if not teams:
        print("No teams — nothing to fan out to. Pass --team-id <id> to test the "
              "on-call endpoint's shape directly against a known team id instead.")
        return

    for t in teams:
        tid = t.get("id")
        tname = t.get("attributes", {}).get("name")
        print(f"\n  team {tid}  {tname!r}")
        try:
            oc = dd_get(*dd, f"/api/v2/on-call/teams/{tid}/on-call")
            for item in oc.get("data", []):
                a = item.get("attributes", {})
                user = (a.get("user") or {}).get("name") or (a.get("user") or {}).get("email")
                schedule = (a.get("schedule") or {}).get("name", "(direct)")
                print(f"    on-call: {user}  schedule={schedule}  level={a.get('escalation_level')}")
            if not oc.get("data"):
                print("    (no one on call for this team right now)")
        except urllib.error.HTTPError as e:
            print(f"    HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:200]}")


def cmd_slos(dd, args):
    # /api/v1/slo/search, not /api/v1/slo — matches fetchSlos() in
    # src/datadog.cpp, which switched to the search endpoint specifically
    # because it returns each SLO's live status (state/sli) inline, letting
    # the device color a status dot without an extra per-SLO call.
    team = bare_team(args.team)
    params = {"page[size]": args.limit}
    if team:
        params["query"] = f"team:{team}"
    print(f"params: {params}")

    try:
        resp = dd_get(*dd, "/api/v1/slo/search", params)
    except urllib.error.HTTPError as e:
        print(f"error: GET /api/v1/slo/search -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    slos = resp.get("data", {}).get("attributes", {}).get("slos", [])
    for item in slos:
        s = item.get("data", {})
        a = s.get("attributes", {})
        status = a.get("status", {})
        print(f"\n  #{s.get('id')}  {a.get('name')!r}")
        print(f"    type: {a.get('slo_type')}  target: {a.get('target_threshold')}  timeframe: {a.get('timeframe')}")
        print(f"    state: {status.get('state')}  sli: {status.get('sli')}")

    print(f"\n{len(slos)} SLO(s) returned.")


def cmd_event(dd, args):
    """Mirrors fetchLatestMonitorAlertEvent() in src/datadog.cpp exactly —
    same query, same endpoint — but dumps the FULL raw event instead of
    just pulling evt.id/timestamp out of it. That's the only place the
    firmware touches the Events API today (it's what triggerBitsInvestigation()
    needs event_id/event_ts for); the rest of the event body — anything
    chart/snapshot-related — has never actually been looked at until now."""
    query = f"source:alert @monitor.id:{args.monitor_id}"
    print(f"query: {query!r}")

    body = {
        "filter": {"query": query, "from": f"now-{args.days}d", "to": "now"},
        "sort": "-timestamp",
        "page": {"limit": args.limit},
    }
    req = urllib.request.Request(
        f"https://api.{dd[0]}/api/v2/events/search",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "DD-API-KEY": dd[1],
            "DD-APPLICATION-KEY": dd[2],
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            result = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        print(f"error: POST /api/v2/events/search -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    events = result.get("data", [])
    if not events:
        print(f"No alert events found for monitor {args.monitor_id} in the last {args.days} day(s). "
              f"Try --days with a bigger window, or double-check the monitor id.")
        return

    for i, e in enumerate(events):
        a = e.get("attributes", {})
        nested = a.get("attributes", {})
        print(f"\n=== event {i+1}/{len(events)}  id={e.get('id')}  timestamp={a.get('timestamp')} ===")
        print(f"    evt.id (what Bits AI needs):        {nested.get('evt', {}).get('id')}")
        print(f"    attributes.timestamp (epoch ms, what Bits AI needs as event_ts): {nested.get('timestamp')}")
        print(json.dumps(e, indent=2))


def cmd_investigations(dd, args):
    """Mirrors dd::fetchBitsInvestigations() in src/datadog.cpp exactly —
    same endpoint, same query. This is an /api/unstable/ endpoint (the web
    UI's own investigations page uses it) — confirmed live it accepts plain
    DD-API-KEY/DD-APPLICATION-KEY auth, but it isn't covered by Datadog's
    /api/v2/ compatibility guarantees and can change or disappear without
    notice. If this subcommand starts failing where it used to work, that's
    the first thing to suspect, not a bug in the device's request."""
    team = bare_team(args.team)
    params = {"page_size": args.limit}
    if team:
        params["query"] = f"team:{team}"
    print(f"params: {params}")

    try:
        resp = dd_get(*dd, "/api/unstable/bits-ai/investigation/search", params)
    except urllib.error.HTTPError as e:
        print(f"error: GET /api/unstable/bits-ai/investigation/search -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    investigations = resp.get("data", {}).get("attributes", {}).get("response", {}).get("investigations", [])

    if args.raw:
        for inv in investigations:
            print(f"\n=== {inv.get('uuid')}  {inv.get('title', '')!r} — raw JSON ===")
            print(json.dumps(inv, indent=2))
        print(f"\n{len(investigations)} investigation(s) total.")
        return

    for inv in investigations:
        print(f"\n  {inv.get('uuid')}  {inv.get('title', '')!r}")
        print(f"    status: {inv.get('status')}  entity: {inv.get('entity', {}).get('source')}  modified: {inv.get('modified_timestamp')}")

    total = resp.get("data", {}).get("attributes", {}).get("response", {}).get("metadata", {}).get("total_results")
    print(f"\n{len(investigations)} investigation(s) returned (total matching: {total}). "
          f"Pass --raw to see the full JSON per item (this script applies no field filtering — unlike the device, "
          f"which uses ArduinoJson's Filter to cut parse cost — so --raw shows everything the API actually returns, "
          f"including facets/hypotheses/narrative/timings the device never looks at), "
          f"or a uuid to `fetch_dd.py investigation --id <uuid>` for the documented single-investigation endpoint.")


def cmd_investigation(dd, args):
    """Mirrors dd::fetchBitsInvestigationDetail() in src/datadog.cpp — same
    endpoint (the documented, stable GET /api/v2/bits-ai/investigations/{id}),
    but dumps the full raw JSON by default, including conclusions[].description
    (a multi-KB markdown wall of text per conclusion — the device deliberately
    doesn't fetch or show this, only title/summary; --summary-only mimics
    exactly what the device actually pulls out of this response)."""
    try:
        resp = dd_get(*dd, f"/api/v2/bits-ai/investigations/{args.id}")
    except urllib.error.HTTPError as e:
        print(f"error: GET /api/v2/bits-ai/investigations/{args.id} -> HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)

    if args.summary_only:
        a = resp.get("data", {}).get("attributes", {})
        print(f"title:  {a.get('title')!r}")
        print(f"status: {a.get('status')}")
        conclusions = a.get("conclusions", [])
        if conclusions:
            c = conclusions[0]
            print(f"conclusion title:   {c.get('title')!r}")
            print(f"conclusion summary: {c.get('summary')!r}")
        else:
            print("(no conclusions yet)")
        return

    print(json.dumps(resp, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", default=os.environ.get("DD_SITE", "datadoghq.com"),
                     help="Datadog site host (default: datadoghq.com, or $DD_SITE)")
    ap.add_argument("--limit", type=int, default=100, help="max items to fetch (default 100)")
    sub = ap.add_subparsers(dest="kind", required=True)

    p_mon = sub.add_parser("monitors", help="fetch + inspect monitors (Monitor Detail chart debugging)")
    p_mon.add_argument("--team", default="", help="team value — applied as 'team:<value>' like the device does")
    p_mon.add_argument("--type", help="only show monitors whose type contains this substring")
    p_mon.add_argument("--verify", action="store_true",
                        help="also call /api/v1/query with the extracted query and report ok/error")
    p_mon.add_argument("--raw", action="store_true",
                        help="dump the full raw JSON per monitor instead of the type-grouped summary")
    p_mon.set_defaults(func=cmd_monitors)

    p_inc = sub.add_parser("incidents", help="fetch active incidents")
    p_inc.add_argument("--team", default="", help="team value — applied as 'teams:<value>' like the device does")
    p_inc.set_defaults(func=cmd_incidents)

    p_oc = sub.add_parser("oncall", help="fetch teams, then who's on-call per team")
    p_oc.add_argument("--team", default="", help="team value — applied as filter[keyword]=<value> on the team lookup")
    p_oc.add_argument("--team-id", help="skip team lookup, hit /api/v2/on-call/teams/<id>/on-call directly")
    p_oc.set_defaults(func=cmd_oncall)

    p_slo = sub.add_parser("slos", help="fetch SLOs")
    p_slo.add_argument("--team", default="", help="team value — applied as tags_query=team:<value> like the device does")
    p_slo.set_defaults(func=cmd_slos)

    p_evt = sub.add_parser("event", help="fetch a monitor's most recent alert event (full raw JSON) — "
                                          "what triggerBitsInvestigation() looks up, but with everything shown")
    p_evt.add_argument("--monitor-id", required=True, help="monitor id, e.g. from `fetch_dd.py monitors`")
    p_evt.add_argument("--days", type=int, default=7, help="how far back to search (default 7 days)")
    p_evt.add_argument("--limit", type=int, default=1, help="how many recent events to show (default 1 — just the latest)")
    p_evt.set_defaults(func=cmd_event)

    p_invs = sub.add_parser("investigations", help="search Bits AI investigations (Bits screen debugging) — "
                                                     "hits an /api/unstable/ endpoint, see cmd_investigations' docstring")
    p_invs.add_argument("--team", default="", help="team value — applied as 'team:<value>' like the device does")
    p_invs.add_argument("--raw", action="store_true",
                        help="dump the full raw JSON per investigation instead of the one-line summary")
    p_invs.set_defaults(func=cmd_investigations)

    p_inv = sub.add_parser("investigation", help="fetch one Bits AI investigation's full detail by id")
    p_inv.add_argument("--id", required=True, help="investigation uuid, e.g. from `fetch_dd.py investigations`")
    p_inv.add_argument("--summary-only", action="store_true",
                        help="print just title/status/top-conclusion — exactly what the device actually shows, "
                             "instead of the full raw JSON (which includes each conclusion's full description)")
    p_inv.set_defaults(func=cmd_investigation)

    args = ap.parse_args()

    env = load_env(ENV_FILE)
    api_key = os.environ.get("DD_API_KEY") or env.get("DD_API_KEY")
    app_key = os.environ.get("DD_APP_KEY") or env.get("DD_APP_KEY")
    if not api_key or not app_key:
        print(f"error: DD_API_KEY / DD_APP_KEY not found in {ENV_FILE} or the environment", file=sys.stderr)
        sys.exit(1)

    args.func((args.site, api_key, app_key), args)


if __name__ == "__main__":
    main()
