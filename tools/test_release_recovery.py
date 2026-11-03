import copy
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import re
import warnings
import zipfile

import release_recovery as recovery


TAG = "v1.2.1"
SHA = "a" * 40
REPAIR = "b" * 40
REPO = "Zorkats/G-Diffuser"
RUN = 41581952351


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run = {
            "id": RUN, "repository": {"id": 7, "full_name": REPO},
            "head_repository": {"id": 7, "full_name": REPO},
            "path": ".github/workflows/release.yml", "event": "push",
            "status": "completed", "conclusion": "failure",
            "head_branch": TAG, "head_sha": SHA,
        }
        self.jobs = [{"name": name, "conclusion": "success"} for name in (
            "windows / windows", "linux / linux", "linux / linux-arm64",
            "macos / macos-arm64")]
        self.jobs.append({"name": "android / build-apk", "conclusion": "failure"})
        self.rows = []
        for index, name in enumerate(recovery.packages(TAG, android=False)):
            self.rows.append({
                "id": index + 1, "name": name.removesuffix(".tar.gz").removesuffix(".zip"),
                "digest": "sha256:" + "c" * 64, "expired": False,
                "workflow_run": {"id": RUN, "head_sha": SHA,
                                 "repository_id": 7, "head_repository_id": 7},
            })

    def validate(self):
        return recovery.validate_source(self.run, self.jobs, self.rows, REPO, TAG, SHA)

    def test_accepts_failed_release_with_four_successful_desktops(self):
        self.assertEqual(len(self.validate()), 4)

    def test_rejects_wrong_source_identity(self):
        for key, value in (("event", "pull_request"), ("path", "nightly.yml"),
                           ("status", "in_progress"), ("conclusion", "cancelled"),
                           ("head_sha", REPAIR), ("head_branch", "v1.2.0")):
            with self.subTest(key=key):
                original = self.run[key]
                self.run[key] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.run[key] = original
        self.run["head_repository"]["id"] = 8
        with self.assertRaises(ValueError):
            self.validate()

    def test_rejects_failed_or_missing_desktop_jobs(self):
        self.jobs[0]["conclusion"] = "failure"
        with self.assertRaises(ValueError):
            self.validate()
        self.jobs.pop(0)
        with self.assertRaises(ValueError):
            self.validate()

    def test_rejects_missing_duplicate_unexpected_artifacts(self):
        rows = copy.deepcopy(self.rows)
        for changed in (rows[:-1], rows + [rows[0]], rows + [{"name": "extra"}]):
            with self.subTest(rows=changed):
                self.rows = changed
                with self.assertRaises(ValueError):
                    self.validate()

    def test_rejects_expired_missing_digest_wrong_run_or_repository(self):
        for key, value in (("expired", True), ("digest", None), ("id", 0)):
            with self.subTest(key=key):
                original = self.rows[0][key]
                self.rows[0][key] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.rows[0][key] = original
        for key, value in (("id", RUN + 1), ("head_sha", REPAIR),
                           ("repository_id", 8), ("head_repository_id", 8)):
            with self.subTest(key=key):
                original = self.rows[0]["workflow_run"][key]
                self.rows[0]["workflow_run"][key] = value
                with self.assertRaises(ValueError):
                    self.validate()
                self.rows[0]["workflow_run"][key] = original

    def test_names_reject_shell_and_path_input(self):
        for tag in ("v1.2.1/escape", "v1.2.1\n", "v1.2.1;exit", "nightly", "v-1"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                recovery.packages(tag)

    def test_published_or_wrong_tag_release_is_refused(self):
        recovery.validate_release(None, TAG)
        recovery.validate_release({"draft": True, "tag_name": TAG, "assets": []}, TAG)
        for release in ({"draft": False, "tag_name": TAG},
                        {"draft": True, "tag_name": "v1.2.0"},
                        {"draft": True, "tag_name": TAG, "assets": [{"name": "extra"}]}):
            with self.subTest(release=release), self.assertRaises(ValueError):
                recovery.validate_release(release, TAG)

    def archive(self, entries):
        path = self.root / "archive.zip"
        with warnings.catch_warnings():
            warnings.filterwarnings("ignore", message="Duplicate name:", category=UserWarning)
            with zipfile.ZipFile(path, "w") as archive:
                for name, data in entries:
                    archive.writestr(name, data)
        return path, "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()

    def test_verified_archive_extracts_only_named_package(self):
        path, digest = self.archive([("package.zip", b"payload")])
        output = self.root / "package.zip"
        recovery.unpack(path, digest, output)
        self.assertEqual(output.read_bytes(), b"payload")

    def test_tampered_traversal_extra_duplicate_empty_and_symlink_fail(self):
        for entries in ([('../package.zip', b'x')], [('package.zip', b'x'), ('extra', b'x')],
                        [('package.zip', b'x'), ('package.zip', b'x')], [('package.zip', b'')]):
            with self.subTest(entries=entries):
                path, digest = self.archive(entries)
                with self.assertRaises(ValueError):
                    recovery.unpack(path, digest, self.root / "package.zip")
        path, digest = self.archive([('package.zip', b'x')])
        with self.assertRaises(ValueError):
            recovery.unpack(path, "sha256:" + "0" * 64, self.root / "package.zip")
        info = zipfile.ZipInfo("package.zip")
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        path, digest = self.archive([(info, b'target')])
        with self.assertRaises(ValueError):
            recovery.unpack(path, digest, self.root / "package.zip")
        with patch.object(recovery, "MAX_PACKAGE_BYTES", 1):
            path, digest = self.archive([('package.zip', b'xx')])
            with self.assertRaises(ValueError):
                recovery.unpack(path, digest, self.root / "package.zip")

    def test_seal_exact_five_packages_and_provenance(self):
        for name in recovery.packages(TAG):
            (self.root / name).write_bytes(name.encode())
        plan = {"tag": TAG, "desktop_commit": SHA, "android_commit": REPAIR}
        recovery.seal(self.root, plan, {"source_sha": REPAIR})
        recovery.validate_dist(self.root, TAG)
        sums = (self.root / "SHA256SUMS").read_text()
        self.assertEqual(len(sums.splitlines()), 6)
        provenance = json.loads((self.root / "SOURCE-PROVENANCE.json").read_text())
        self.assertEqual(provenance["desktop_commit"], SHA)
        self.assertEqual(provenance["android_commit"], REPAIR)
        (self.root / recovery.packages(TAG)[0]).write_bytes(b"changed")
        with self.assertRaises(ValueError):
            recovery.validate_dist(self.root, TAG)

    def test_seal_refuses_missing_or_extra_package(self):
        with self.assertRaises(ValueError):
            recovery.seal(self.root, {"tag": TAG}, {})
        for name in recovery.packages(TAG):
            (self.root / name).write_bytes(b"x")
        (self.root / "extra.apk").write_bytes(b"x")
        with self.assertRaises(ValueError):
            recovery.seal(self.root, {"tag": TAG}, {})

    def test_ancestry_rejects_diverged_or_wrong_commits(self):
        recovery.validate_comparison({"status": "ahead", "merge_base_commit": {"sha": SHA}}, SHA)
        recovery.validate_comparison({"status": "identical", "merge_base_commit": {"sha": SHA}}, SHA)
        for comparison in ({"status": "diverged", "merge_base_commit": {"sha": SHA}},
                           {"status": "ahead", "merge_base_commit": {"sha": REPAIR}}):
            with self.assertRaises(ValueError):
                recovery.validate_comparison(comparison, SHA)

    def test_assemble_and_draft_with_verified_synthetic_artifacts(self):
        plan = {"tag": TAG, "repository": REPO, "source_run": RUN,
                "desktop_commit": SHA, "android_commit": REPAIR,
                "controller_commit": REPAIR, "repository_id": 7,
                "artifacts": self.validate()}
        run_id = RUN + 1
        apk = recovery.packages(TAG)[-1]
        attestation = {"source_sha": REPAIR, "version": TAG[1:], "run_id": run_id}
        selected = []
        files = {}
        for index, name in enumerate((*recovery.packages(TAG), "android-source.json")):
            data = json.dumps(attestation).encode() if name.endswith(".json") else name.encode()
            archive = self.root / f"artifact-{index}.zip"
            with zipfile.ZipFile(archive, "w") as out:
                out.writestr(name, data)
            row = {"id": index + 1, "name": recovery.artifact_name(name),
                   "digest": "sha256:" + recovery.digest_file(archive), "expired": False,
                   "workflow_run": {"id": run_id, "head_sha": REPAIR,
                                    "repository_id": 7, "head_repository_id": 7}}
            if name == "android-source.json":
                row["name"] = f"android-source-{run_id}"
            if index < 4:
                plan["artifacts"][row["name"]] = {key: row[key] for key in ("id", "name", "digest")}
            else:
                selected.append(row)
            files[row["id"]] = archive

        def fetch(repo, row, output, scratch):
            recovery.unpack(files[row["id"]], row["digest"], output)

        run = dict(self.run, id=run_id, head_sha=REPAIR, event="workflow_dispatch")
        directory = self.root / "dist"
        with patch.object(recovery, "make_plan", return_value=copy.deepcopy(plan)), \
             patch.object(recovery, "gh_json", return_value=run), \
             patch.object(recovery, "list_rows", return_value=selected), \
             patch.object(recovery, "download", side_effect=fetch):
            recovery.assemble(plan, run_id, directory)
        provenance = json.loads((directory / "SOURCE-PROVENANCE.json").read_text())
        self.assertEqual(provenance["recovery_run"], run_id)
        self.assertEqual(provenance["android_build"], attestation)
        self.assertEqual(len(provenance["android_artifacts"]), 2)
        existing = {"id": 99, "draft": True, "tag_name": TAG,
                    "body": "Owner release notes.", "assets": []}
        uploaded = dict(existing, assets=[{"name": name} for name in recovery.packages(TAG)] +
                        [{"name": name} for name in recovery.METADATA])
        with patch.object(recovery, "tag_commit", return_value=SHA), \
             patch.object(recovery, "release_for", side_effect=[existing, existing, uploaded]), \
             patch.object(recovery.subprocess, "run") as command:
            def capture(args, **kwargs):
                if "--input" in args:
                    payload = json.loads(Path(args[args.index("--input") + 1]).read_text())
                    self.assertTrue(payload["draft"])
                    self.assertEqual(payload["tag_name"], TAG)
                    self.assertIn("Owner release notes.", payload["body"])
                    self.assertIn(SHA, payload["body"])
                    self.assertIn(REPAIR, payload["body"])
                    self.assertIn("do **not** share one source commit", payload["body"])
            command.side_effect = capture
            recovery.draft(plan, directory)
            self.assertEqual(command.call_count, 2)
            upload = command.call_args.args[0]
            self.assertEqual(len([arg for arg in upload if str(directory) in arg]), 7)
        with patch.object(recovery, "tag_commit", return_value=REPAIR), \
             patch.object(recovery.subprocess, "run") as command:
            with self.assertRaises(ValueError):
                recovery.draft(plan, directory)
            command.assert_not_called()

    def test_api_failure_is_not_treated_as_absent_release(self):
        with patch.object(recovery, "gh_json", side_effect=RuntimeError("API unavailable")):
            with self.assertRaises(RuntimeError):
                recovery.release_for(REPO, TAG)

    def test_lightweight_and_annotated_tag_resolution_use_only_tag_refs(self):
        commit = {"object": {"type": "commit", "sha": SHA}}
        annotated = {"object": {"type": "tag", "sha": REPAIR}}
        with patch.object(recovery, "gh_json", return_value=commit) as api:
            self.assertEqual(recovery.tag_commit(REPO, TAG), SHA)
            api.assert_called_once_with(f"repos/{REPO}/git/ref/tags/{TAG}")
        with patch.object(recovery, "gh_json", side_effect=[annotated, commit]):
            self.assertEqual(recovery.tag_commit(REPO, TAG), SHA)
        for response in ({"object": {"type": "tree", "sha": SHA}}, annotated):
            with patch.object(recovery, "gh_json", return_value=response), self.assertRaises(ValueError):
                recovery.tag_commit(REPO, TAG)


class WorkflowContractTests(unittest.TestCase):
    def setUp(self):
        self.workflows = Path(__file__).resolve().parents[1] / ".github" / "workflows"
        self.release = (self.workflows / "release.yml").read_text(encoding="utf-8")

    def test_only_release_and_nightly_have_launch_triggers(self):
        self.assertFalse((self.workflows / "android.yml").exists())
        for path in self.workflows.glob("*.yml"):
            text = path.read_text(encoding="utf-8")
            events = text.split("\non:\n", 1)[1].split("\npermissions:", 1)[0]
            triggers = set(re.findall(r"^  ([a-z_]+):", events, flags=re.M))
            if path.name in ("release.yml", "nightly.yml"):
                self.assertIn("workflow_dispatch", triggers)
            else:
                self.assertEqual(triggers, {"workflow_call"})
            for worker in re.findall(r"uses: \./\.github/workflows/(\S+)", text):
                self.assertTrue((self.workflows / worker).is_file(), worker)

    def test_recovery_skips_all_desktop_builds_and_pins_android(self):
        for name in ("windows", "linux", "macos"):
            job = re.split(r"\n  [a-z]+:\n", self.release.split(f"\n  {name}:\n")[1])[0]
            self.assertIn("if: needs.gate.outputs.recovery != 'true'", job)
        self.assertIn("source-commit: ${{ needs.gate.outputs.android_commit }}", self.release)
        worker = (self.workflows / "release-android.yml").read_text()
        self.assertIn("ref: ${{ inputs.source-commit || github.sha }}", worker)
        self.assertIn("apksigner\" verify --verbose", worker)
        self.assertNotIn("assembleDebug", worker)
        self.assertIn("android-source-${{ github.run_id }}", worker)

    def test_draft_condition_accepts_only_complete_normal_or_recovery_paths(self):
        job = self.release.split("\n  draft:\n")[1]
        expression = job.split("    if: >-\n", 1)[1].split("    runs-on:", 1)[0]
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = expression.replace("!cancelled()", "not cancelled()")
        expression = " ".join(expression.split())
        for recovery_mode in (True, False):
            desktop = "skipped" if recovery_mode else "success"
            states = {"gate": "success", "android": "success", "windows": desktop,
                      "linux": desktop, "macos": desktop}

            def evaluate(states, cancelled=False):
                needs = SimpleNamespace(**{name: SimpleNamespace(result=result) for name, result in states.items()})
                needs.gate.outputs = SimpleNamespace(recovery=str(recovery_mode).lower())
                return eval(expression, {"__builtins__": {}}, {
                    "needs": needs, "always": lambda: True, "cancelled": lambda: cancelled})

            self.assertTrue(evaluate(states))
            self.assertFalse(evaluate(states, cancelled=True))
            for name in states:
                for result in ("failure", "cancelled", "skipped" if states[name] == "success" else "success"):
                    with self.subTest(mode=recovery_mode, job=name, result=result):
                        self.assertFalse(evaluate(dict(states, **{name: result})))

    def test_plan_survives_draft_only_retry(self):
        self.assertNotIn("github.run_attempt", self.release)
        self.assertEqual(self.release.count("name: recovery-plan-${{ github.run_id }}"), 2)
        self.assertIn("actions: read", self.release)
        self.assertIn('"refs/heads/$DEFAULT_BRANCH"', self.release)


if __name__ == "__main__":
    unittest.main()
