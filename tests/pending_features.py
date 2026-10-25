#!/usr/bin/env python3
"""Pending acceptance contracts. XFAIL=77, XPASS=1, infrastructure error=2."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

MANIFEST = Path(__file__).with_name("pending") / "features.json"
STAGES = {"parser", "scanner", "debug", "semantic", "ownership"}
SAFETY_ERRORS = ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer",
                 "runtime error:", "Sanitizer:DEADLYSIGNAL")


class InfrastructureError(Exception):
    pass


def validate(document):
    if document.get("version") != 1 or not isinstance(document.get("features"), list):
        raise InfrastructureError("Invalid manifest version/features")
    cases, features = {}, set()
    for feature in document["features"]:
        name = feature.get("id", "")
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name) or name in features:
            raise InfrastructureError("Invalid or duplicate feature id: " + name)
        features.add(name)
        if feature.get("stage") not in STAGES or not feature.get("reason"):
            raise InfrastructureError("Missing feature stage/reason: " + name)
        if feature.get("syntax") not in {"existing", "proposed"} or not feature.get("cases"):
            raise InfrastructureError("Missing syntax status/cases: " + name)
        for case in feature["cases"]:
            short_name = case.get("id", "")
            if not re.fullmatch(r"[a-z][a-z0-9_]*", short_name):
                raise InfrastructureError("Invalid case id: " + short_name)
            key = name + "." + short_name
            if key in cases or not isinstance(case.get("source"), str) or not case["source"]:
                raise InfrastructureError("Duplicate case or empty source: " + key)
            if feature["stage"] in {"semantic", "ownership"}:
                expected = case.get("expected", {})
                if type(expected.get("ok")) is not bool:
                    raise InfrastructureError("Adapter case needs an expected boolean: " + key)
                if not expected["ok"] and not (expected.get("diagnostic") or feature["stage"] == "ownership"):
                    raise InfrastructureError("Rejection needs a diagnostic category: " + key)
            elif "check" not in case:
                raise InfrastructureError("Native case needs a probe check: " + key)
            for path in [case.get("entry", "main.oc"), case.get("working_directory", "."), *case.get("files", {})]:
                if Path(path).is_absolute() or ".." in Path(path).parts:
                    raise InfrastructureError("Fixture path escapes its temporary project: " + path)
            cases[key] = (feature, case)
    return cases


def execute(command, **kwargs):
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=15, **kwargs)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise InfrastructureError(str(error)) from error
    output = result.stdout + result.stderr
    if any(marker in output for marker in SAFETY_ERRORS):
        raise InfrastructureError("Sanitizer failure:\n" + output)
    return result, output


def native_contract(probe, feature, case, directory):
    if not probe or not Path(probe).is_file():
        raise InfrastructureError("A built --probe executable is required")
    source = directory / "fixture.oc"
    source.write_text(case["source"], encoding="utf-8")
    result, output = execute([str(Path(probe).resolve()), feature["stage"], str(source), case["check"]])
    if result.returncode == 10 and "CONTRACT_MISSING:" in output:
        return False, output.strip()
    if feature["stage"] == "debug" and result.returncode == -signal.SIGABRT and "Unknown expr type\n" in output:
        return False, "Debug printer does not support this AST node"
    if result.returncode == 1 and case.get("missing_diagnostic") and re.search(case["missing_diagnostic"], output):
        return False, output.strip()
    if feature["stage"] == "scanner" and case["check"] == "reject":
        if result.returncode == 1 and re.search(case["rejection_diagnostic"], output):
            return True, "Scanner rejected the invalid Unicode escape"
    if result.returncode != 0:
        raise InfrastructureError(f"Unexpected probe exit {result.returncode}:\n{output}")
    expected_output = case.get("stdout")
    if expected_output and expected_output not in output:
        return False, f"Missing expected printer output {expected_output!r}:\n{output}"
    return True, "Native acceptance contract satisfied"


def adapter_contract(driver, feature, case, directory):
    if not driver:
        return False, f"No {feature['stage']} test driver is configured; {feature['reason']}"
    root = directory / "project"
    root.mkdir()
    files = dict(case.get("files", {}))
    entry = case.get("entry", "main.oc")
    if entry not in files:
        files[entry] = case["source"]
    for name, text in files.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    working = root / case.get("working_directory", ".")
    working.mkdir(parents=True, exist_ok=True)
    # Do not send expected results to the driver: it must obtain them from the
    # compiler, not echo the acceptance contract back to the runner.
    request = {key: value for key, value in case.items() if key not in {"id", "expected"}}
    request.update(version=1, stage=feature["stage"], feature=feature["id"],
                   project_root=str(root), entry=entry, working_directory=str(working))
    result, output = execute([str(Path(driver).resolve())], input=json.dumps(request), cwd=working)
    if result.returncode != 0:
        raise InfrastructureError(f"Test driver exited {result.returncode}:\n{output}")
    try:
        response = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise InfrastructureError("Driver stdout must contain one JSON response: " + output) from error
    if not isinstance(response, dict) or type(response.get("ok")) is not bool:
        raise InfrastructureError("Driver response needs a boolean 'ok'")
    if not isinstance(response.get("facts", {}), dict) or not isinstance(response.get("diagnostics", []), list):
        raise InfrastructureError("Invalid driver facts/diagnostics schema")
    if not all(isinstance(code, str) for code in response.get("diagnostics", [])):
        raise InfrastructureError("Driver diagnostics must be canonical category strings")
    expected = case["expected"]
    if response["ok"] != expected["ok"]:
        return False, f"Expected ok={expected['ok']}, got {response['ok']}"
    if expected.get("diagnostic") and expected["diagnostic"] not in response.get("diagnostics", []):
        return False, "Missing diagnostic category: " + expected["diagnostic"]
    for key, value in expected.get("facts", {}).items():
        if key not in response.get("facts", {}) or response["facts"][key] != value:
            return False, f"Expected compiler fact {key}={value!r}, got {response.get('facts', {}).get(key)!r}"
    return True, "Compiler acceptance contract satisfied"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--case")
    parser.add_argument("--probe")
    parser.add_argument("--semantic-driver", default=os.environ.get("OCEAN_SEMANTIC_TEST_DRIVER"))
    parser.add_argument("--ownership-driver", default=os.environ.get("OCEAN_OWNERSHIP_TEST_DRIVER"))
    args = parser.parse_args(argv)
    try:
        cases = validate(json.loads(args.manifest.read_text(encoding="utf-8")))
        if args.list:
            print("\n".join(cases)); return 0
        if args.validate:
            print(f"Validated {len(cases)} pending contracts"); return 0
        if args.case not in cases:
            raise InfrastructureError("Unknown --case: " + str(args.case))
        feature, case = cases[args.case]
        with tempfile.TemporaryDirectory(prefix="ocean-pending-") as temporary:
            directory = Path(temporary)
            if feature["stage"] in {"semantic", "ownership"}:
                driver = args.semantic_driver if feature["stage"] == "semantic" else args.ownership_driver
                met, reason = adapter_contract(driver, feature, case, directory)
            else:
                met, reason = native_contract(args.probe, feature, case, directory)
        if met:
            print(f"XPASS {args.case}: {reason}. Promote this contract to the regular suite.")
            return 1
        print(f"XFAIL {args.case} [{feature['stage']}, {feature['syntax']} syntax]: {reason}")
        return 77
    except (InfrastructureError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
