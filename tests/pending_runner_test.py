#!/usr/bin/env python3
"""The pending harness must never turn a crash or broken fixture into XFAIL."""
import copy
import io
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import sys
import unittest
from contextlib import redirect_stdout, redirect_stderr
from unittest.mock import patch

sys.dont_write_bytecode = True
import pending_features as runner


class PendingRunnerTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(runner.MANIFEST.read_text())
        self.cases = runner.validate(self.manifest)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.probe = self.root / "probe"
        self.probe.touch()
        self.feature, self.case = self.cases["debug_printer.index"]

    def process(self, code=0, output="", error=""):
        return subprocess.CompletedProcess([], code, output, error)

    def native(self, process):
        with patch.object(runner.subprocess, "run", return_value=process):
            return runner.native_contract(str(self.probe), self.feature, self.case, self.root)

    def test_inventory_has_every_documented_area(self):
        expected = {"debug_printer",
                    "imports", "module_discovery", "symbol_binding", "type_checking",
                    "generic_instantiation", "ambiguous_calls", "generic_function_values",
                    "type_dimension_resolution", "constant_evaluation", "enum_values",
                    "return_checking", "label_resolution", "extern_linkage", "struct_value_application",
                    "operator_resolution", "custom_constraints", "index_delegation", "cleanup"}
        self.assertEqual({feature["id"] for feature in self.manifest["features"]}, expected)

    def test_duplicate_ids_rejected(self):
        document = copy.deepcopy(self.manifest)
        document["features"].append(document["features"][0])
        with self.assertRaises(runner.InfrastructureError): runner.validate(document)

    def test_empty_source_rejected(self):
        self.manifest["features"][0]["cases"][0]["source"] = ""
        with self.assertRaises(runner.InfrastructureError): runner.validate(self.manifest)

    def test_path_escape_rejected(self):
        self.manifest["features"][0]["cases"][0]["files"] = {"../outside": "bad"}
        with self.assertRaises(runner.InfrastructureError): runner.validate(self.manifest)

    def test_missing_rejection_category_rejected(self):
        feature = next(f for f in self.manifest["features"] if f["id"] == "symbol_binding")
        del feature["cases"][0]["expected"]["diagnostic"]
        with self.assertRaises(runner.InfrastructureError): runner.validate(self.manifest)

    def test_checked_contract_mismatch_is_pending(self):
        self.assertFalse(self.native(self.process(10, "CONTRACT_MISSING: index AST"))[0])

    def test_success_is_detected(self):
        self.assertTrue(self.native(self.process(output=self.case["stdout"]))[0])

    def test_arbitrary_nonzero_exit_is_error(self):
        with self.assertRaises(runner.InfrastructureError): self.native(self.process(1, "broken fixture"))
        with self.assertRaises(runner.InfrastructureError): self.native(self.process(10, "no contract marker"))

    def test_crashes_are_errors(self):
        with self.assertRaises(runner.InfrastructureError): self.native(self.process(-signal.SIGSEGV))

    def test_sanitizer_report_is_error_even_with_missing_marker(self):
        with self.assertRaises(runner.InfrastructureError):
            self.native(self.process(10, "CONTRACT_MISSING: x", "AddressSanitizer: invalid read"))

    def test_only_known_debug_abort_is_pending(self):
        feature, case = self.cases["debug_printer.member"]
        with patch.object(runner.subprocess, "run", return_value=self.process(-signal.SIGABRT, "Unknown expr type\n")):
            self.assertFalse(runner.native_contract(str(self.probe), feature, case, self.root)[0])
        with patch.object(runner.subprocess, "run", return_value=self.process(-signal.SIGABRT, "different assertion")):
            with self.assertRaises(runner.InfrastructureError):
                runner.native_contract(str(self.probe), feature, case, self.root)

    def test_timeout_is_error(self):
        with patch.object(runner.subprocess, "run", side_effect=subprocess.TimeoutExpired([], 15)):
            with self.assertRaises(runner.InfrastructureError): self.native_call()

    def native_call(self):
        return runner.native_contract(str(self.probe), self.feature, self.case, self.root)

    def test_missing_adapter_is_explicitly_pending(self):
        feature, case = self.cases["symbol_binding.undeclared"]
        met, reason = runner.adapter_contract(None, feature, case, self.root)
        self.assertFalse(met); self.assertIn("No semantic test driver", reason)

    def test_adapter_uses_real_facts_not_echoed_expectations(self):
        feature, case = self.cases["generic_instantiation.substitution"]
        response = self.process(output=json.dumps({"ok": True, "facts": {"x_type": "int"}}))
        with patch.object(runner.subprocess, "run", return_value=response) as call:
            self.assertTrue(runner.adapter_contract(str(self.probe), feature, case, self.root)[0])
        request = json.loads(call.call_args.kwargs["input"])
        self.assertNotIn("expected", request)
        self.assertEqual((Path(request["project_root"]) / request["entry"]).read_text(), case["source"])

    def test_wrong_adapter_diagnostic_is_pending(self):
        feature, case = self.cases["symbol_binding.undeclared"]
        response = self.process(output=json.dumps({"ok": False, "diagnostics": ["unrelated"]}))
        with patch.object(runner.subprocess, "run", return_value=response):
            self.assertFalse(runner.adapter_contract(str(self.probe), feature, case, self.root)[0])

    def test_malformed_adapter_output_is_error(self):
        feature, case = self.cases["symbol_binding.undeclared"]
        with patch.object(runner.subprocess, "run", return_value=self.process(output="not JSON")):
            with self.assertRaises(runner.InfrastructureError):
                runner.adapter_contract(str(self.probe), feature, case, self.root)

    def test_missing_probe_is_error(self):
        with self.assertRaises(runner.InfrastructureError):
            runner.native_contract(None, self.feature, self.case, self.root)

    def test_wrong_facts_are_pending(self):
        feature, case = self.cases["generic_instantiation.substitution"]
        response = self.process(output=json.dumps({"ok": True, "facts": {"x_type": "string"}}))
        with patch.object(runner.subprocess, "run", return_value=response):
            self.assertFalse(runner.adapter_contract(str(self.probe), feature, case, self.root)[0])

    def test_wrong_acceptance_status_is_pending(self):
        feature, case = self.cases["symbol_binding.undeclared"]
        response = self.process(output=json.dumps({"ok": True}))
        with patch.object(runner.subprocess, "run", return_value=response):
            self.assertFalse(runner.adapter_contract(str(self.probe), feature, case, self.root)[0])

    def test_adapter_crash_is_error(self):
        feature, case = self.cases["symbol_binding.undeclared"]
        with patch.object(runner.subprocess, "run", return_value=self.process(-signal.SIGSEGV)):
            with self.assertRaises(runner.InfrastructureError):
                runner.adapter_contract(str(self.probe), feature, case, self.root)

    def test_invalid_response_schema_is_error(self):
        feature, case = self.cases["symbol_binding.undeclared"]
        with patch.object(runner.subprocess, "run", return_value=self.process(output='{"ok": "false"}')):
            with self.assertRaises(runner.InfrastructureError):
                runner.adapter_contract(str(self.probe), feature, case, self.root)

    def test_invalid_utf8_diagnostic_does_not_crash_runner(self):
        # The scanner currently prints individual UTF-8 bytes in diagnostics.
        with patch.object(runner.subprocess, "run", return_value=self.process(1, "invalid")) as call:
            runner.execute(["probe"])
        self.assertEqual(call.call_args.kwargs["errors"], "replace")

    def test_xpass_fails_instead_of_silently_skipping(self):
        with patch.object(runner, "native_contract", return_value=(True, "contract met")):
            with redirect_stdout(io.StringIO()) as output:
                status = runner.main(["--case", "debug_printer.index", "--probe", str(self.probe)])
        self.assertEqual(status, 1); self.assertIn("XPASS", output.getvalue())

    def test_missing_contract_has_skip_exit_code(self):
        with patch.object(runner, "native_contract", return_value=(False, "not implemented")):
            with redirect_stdout(io.StringIO()) as output:
                status = runner.main(["--case", "debug_printer.index", "--probe", str(self.probe)])
        self.assertEqual(status, 77); self.assertIn("XFAIL", output.getvalue())

    def test_unknown_case_is_error(self):
        with redirect_stderr(io.StringIO()): status = runner.main(["--case", "typo"])
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
