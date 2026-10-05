import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("qualify", ROOT / "scripts/qualify_asset_free.py")
qualify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qualify)


class AssetFreeQualificationTests(unittest.TestCase):
    def test_manifest_has_unique_required_and_classified_names(self):
        manifest = json.loads((ROOT / "scripts/asset_free_suite.json").read_text())
        required = [entry["name"] for entry in manifest["required"]]
        dependent = [entry["name"] for entry in manifest["input_dependent"]]
        native = [entry["name"] for entry in manifest["platform_native"]]
        # The public export leaves the 22 harness tests (cmake/RrvHarnessTests.cmake) and their manifest entries out.
        self.assertEqual(len(required), 56 if (ROOT / 'cmake/RrvHarnessTests.cmake').exists() else 34)
        self.assertEqual(len(required), len(set(required)))
        self.assertTrue(set(required).isdisjoint(dependent))
        self.assertTrue(set(required + dependent).isdisjoint(native))

    def test_registration_rejects_unexpected_and_missing(self):
        manifest = {"label": "portable-asset-free", "required": [{"name": "ok"}], "input_dependent": []}
        with self.assertRaises(RuntimeError):
            qualify.validate_registration([{"name": "unexpected"}], manifest)

    def test_platform_native_is_optional_but_label_checked(self):
        native = {"name": "native", "label": "g1-h1e-native"}
        manifest = {"label": "portable-asset-free", "required": [{"name": "ok"}],
                    "input_dependent": [], "platform_native": [native]}
        ok = {"name": "ok", "properties": [{"name": "LABELS", "value": ["portable-asset-free"]}]}
        labelled = {"name": "native", "properties": [{"name": "LABELS", "value": ["g1-h1e-native"]}]}
        self.assertEqual(qualify.validate_registration([ok], manifest), ["ok"])
        self.assertEqual(qualify.validate_registration([ok, labelled], manifest), ["ok"])
        wrong = {"name": "native", "properties": [{"name": "LABELS", "value": ["portable-asset-free"]}]}
        with self.assertRaises(RuntimeError):
            qualify.validate_registration([ok, wrong], manifest)
        overlap = dict(manifest, platform_native=[{"name": "ok", "label": "g1-h1e-native"}])
        with self.assertRaises(RuntimeError):
            qualify.validate_registration([ok], overlap)
        portable = dict(manifest, platform_native=[{"name": "native", "label": "portable-asset-free"}])
        with self.assertRaises(RuntimeError):
            qualify.validate_registration([ok], portable)

    def test_junit_rejects_skip(self):
        manifest = {"required": [{"name": "ok"}], "input_dependent": []}
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "result.xml"
            result.write_text('<testsuite><testcase name="ok"><skipped /></testcase></testsuite>')
            with self.assertRaises(RuntimeError):
                qualify.validate_junit(result, ["ok"])

    def test_manifest_rejects_duplicate_names_and_inner_skip(self):
        manifest = {"label": "portable-asset-free", "required": [{"name": "ok"}, {"name": "ok"}], "input_dependent": []}
        with self.assertRaises(RuntimeError):
            qualify.validate_registration([], manifest)
        with self.assertRaises(RuntimeError):
            qualify.validate_unittest_output("Ran 1 test\nOK (skipped=1)")


if __name__ == "__main__":
    unittest.main()
