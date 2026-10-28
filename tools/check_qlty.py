#!/usr/bin/env python3
"""Fail closed on incomplete native SARIF; validate native diff results.

Raw reports stay private: stdout contains only rule/path/level counts. A zero
new-diff-issue result does not resolve the explicitly reported baseline backlog.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys
from urllib.parse import unquote


class IncompleteScan(ValueError):
    pass


def read_report(path, exit_code):
    if exit_code not in (0, 1):
        raise IncompleteScan("native analysis did not complete successfully")
    document = json.loads(Path(path).read_text())
    if document.get("version") != "2.1.0" or len(document.get("runs", [])) != 1:
        raise IncompleteScan("missing or unsupported SARIF run")
    run = document["runs"][0]
    driver = run["tool"]["driver"]
    if driver["name"] != "qlty" or driver["semanticVersion"] != "0.662.0":
        raise IncompleteScan("unexpected analyzer identity")
    invocations = run.get("invocations", [])
    if len(invocations) != 1 or invocations[0].get("executionSuccessful") is not True:
        raise IncompleteScan("analyzer operation failed or completion is missing")
    # Unlocated findings are notifications too: fail rather than silently lose them.
    for notification in invocations[0].get("toolExecutionNotifications", []):
        if (notification.get("level") == "error" or
                not notification.get("descriptor", {}).get("id", "").startswith("qlty:message:")):
            raise IncompleteScan("operational error or unlocated finding requires review")
    results = run["results"]
    if not isinstance(results, list) or bool(results) != bool(exit_code):
        raise IncompleteScan("result structure and strict native issue exit disagree")
    return results


def issue_counts(results):
    groups = Counter()
    for result in results:
        rule, level = result["ruleId"], result["level"]
        locations = result["locations"]
        if len(locations) != 1:
            raise IncompleteScan("ambiguous finding location")
        uri = unquote(locations[0]["physicalLocation"]["artifactLocation"]["uri"])
        if Path(uri).is_absolute() or ".." in Path(uri).parts:
            raise IncompleteScan("finding outside the public export")
        if not isinstance(rule, str) or level not in ("error", "warning", "note", "none"):
            raise IncompleteScan("invalid finding identity")
        groups[(rule, level, uri)] += 1
    return groups


def compare(base_path, base_exit, candidate_path, candidate_exit):
    baseline = issue_counts(read_report(base_path, base_exit))
    # Candidate SARIF is the native --upstream diff, never a full-scan count waiver.
    new = issue_counts(read_report(candidate_path, candidate_exit))
    print(json.dumps({
        "status": "NEW_FINDINGS" if new else "NO_NEW_DIFF_FINDINGS_BACKLOG_REMAINS_OPEN",
        "baseline_findings": sum(baseline.values()), "new_findings": sum(new.values()),
        "counts": [{"rule": key[0], "level": key[1], "path": key[2], "count": count}
                   for key, count in sorted(new.items())],
    }, sort_keys=True))
    return int(bool(new))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("baseline_exit", type=int)
    parser.add_argument("candidate")
    parser.add_argument("candidate_exit", type=int)
    args = parser.parse_args()
    try:
        return compare(args.baseline, args.baseline_exit, args.candidate, args.candidate_exit)
    except (OSError, ValueError, KeyError, TypeError):
        # Never print tool messages, raw reports, snippets, tokens or fingerprints.
        print("Qlty incomplete/invalid analysis: gate closed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
