import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile


MAX_PACKAGE_BYTES = 2 * 1024 * 1024 * 1024
DESKTOP_JOBS = {"windows / windows", "linux / linux", "linux / linux-arm64", "macos / macos-arm64"}
METADATA = {"SHA256SUMS", "SOURCE-PROVENANCE.json"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def packages(tag, android=True):
    require(re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?", tag),
            "Expected a release tag such as v1.2.1")
    names = [f"G-Diffuser-{tag}-{suffix}" for suffix in (
        "windows-x64.zip", "linux-x64.tar.gz", "linux-arm64.tar.gz", "macos-arm64.tar.gz")]
    if android:
        names.append(f"G-Diffuser-{tag}-android-arm64.apk")
    return names


def artifact_name(package):
    return package.removesuffix(".tar.gz").removesuffix(".zip").removesuffix(".apk")


def select_artifacts(rows, expected, run_id, sha, repo_id):
    require(len(rows) == len(expected), "Missing, duplicate, or unexpected artifacts")
    selected = {}
    for row in rows:
        name = row["name"]
        require(name in expected and name not in selected, f"Unexpected or duplicate artifact: {name}")
        require(isinstance(row.get("id"), int) and row["id"] > 0, "Invalid artifact ID")
        require(row.get("expired") is False, f"Expired artifact: {name}")
        require(re.fullmatch(r"sha256:[0-9a-f]{64}", row.get("digest") or ""),
                f"Missing SHA-256 archive digest: {name}")
        origin = row.get("workflow_run", {})
        require(origin.get("id") == run_id and origin.get("head_sha") == sha
                and origin.get("repository_id") == repo_id and origin.get("head_repository_id") == repo_id,
                f"Wrong source run, commit, or repository: {name}")
        selected[name] = {key: row[key] for key in ("id", "name", "digest")}
    return selected


def validate_source(run, jobs, rows, repo, tag, tag_sha):
    expected = {artifact_name(name) for name in packages(tag, android=False)}
    require(run["repository"]["full_name"] == repo
            and run["head_repository"]["full_name"] == repo
            and run["repository"]["id"] == run["head_repository"]["id"], "Source run is from another repository")
    require(run.get("path") == ".github/workflows/release.yml" and run.get("event") == "push"
            and run.get("status") == "completed" and run.get("conclusion") == "failure"
            and run.get("head_branch") == tag and run.get("head_sha") == tag_sha,
            "Source must be the completed failed Release run for the unchanged tag")
    for name in DESKTOP_JOBS:
        matches = [job for job in jobs if job["name"] == name]
        require(len(matches) == 1 and matches[0]["conclusion"] == "success", f"Desktop job not successful: {name}")
    matches = [job for job in jobs if job["name"] == "android / build-apk"]
    require(len(matches) == 1 and matches[0]["conclusion"] == "failure", "Source Android job did not fail")
    return select_artifacts(rows, expected, run["id"], tag_sha, run["repository"]["id"])


def validate_comparison(comparison, ancestor):
    require(comparison.get("status") in ("ahead", "identical")
            and comparison.get("merge_base_commit", {}).get("sha") == ancestor,
            "Repair must descend from the release tag and be contained in the controller commit")


def validate_release(release, tag):
    if release is None:
        return
    require(release.get("draft") is True and release.get("tag_name") == tag,
            "Refusing to modify a published release or another tag")
    names = [asset["name"] for asset in release.get("assets", [])]
    require(len(names) == len(set(names)) and set(names) <= set(packages(tag)) | METADATA,
            "Draft has unexpected assets; inspect it manually before recovery")


def gh_json(endpoint, paginate=False):
    command = ["gh", "api", endpoint]
    if paginate:
        command += ["--paginate", "--slurp"]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def release_for(repo, tag):
    pages = gh_json(f"repos/{repo}/releases?per_page=100", paginate=True)
    matches = [release for page in pages for release in page if release["tag_name"] == tag]
    require(len(matches) <= 1, "Multiple releases use the target tag")
    release = matches[0] if matches else None
    validate_release(release, tag)
    return release


def list_rows(repo, run_id, kind):
    pages = gh_json(f"repos/{repo}/actions/runs/{run_id}/{kind}?per_page=100", paginate=True)
    return [row for page in pages for row in page[kind]]


def tag_commit(repo, tag):
    obj = gh_json(f"repos/{repo}/git/ref/tags/{tag}")["object"]
    for _ in range(8):
        require(re.fullmatch(r"[0-9a-f]{40}", obj.get("sha", "")), "Invalid tag object SHA")
        if obj.get("type") == "commit":
            return obj["sha"]
        require(obj.get("type") == "tag", "Release tag does not resolve to a commit")
        obj = gh_json(f"repos/{repo}/git/tags/{obj['sha']}")["object"]
    raise ValueError("Release tag nesting is too deep")


def make_plan(repo, tag, source_run, android_commit, controller_commit):
    packages(tag)
    for commit in (android_commit, controller_commit):
        require(re.fullmatch(r"[0-9a-f]{40}", commit), "Use full lowercase 40-character commit SHAs")
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo), "Invalid repository")
    require(source_run > 0, "Invalid source run ID")
    tag_sha = tag_commit(repo, tag)
    validate_comparison(gh_json(f"repos/{repo}/compare/{tag_sha}...{android_commit}"), tag_sha)
    validate_comparison(gh_json(f"repos/{repo}/compare/{android_commit}...{controller_commit}"), android_commit)
    release_for(repo, tag)
    run = gh_json(f"repos/{repo}/actions/runs/{source_run}")
    require(run["id"] == source_run, "Source run ID mismatch")
    artifacts = validate_source(run, list_rows(repo, source_run, "jobs"),
                                list_rows(repo, source_run, "artifacts"), repo, tag, tag_sha)
    return {"schema": 1, "repository": repo, "repository_id": run["repository"]["id"],
            "tag": tag, "desktop_commit": tag_sha, "android_commit": android_commit,
            "controller_commit": controller_commit, "source_run": source_run, "artifacts": artifacts}


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def unpack(archive, digest, output):
    require("sha256:" + digest_file(archive) == digest, "Artifact archive SHA-256 mismatch")
    with zipfile.ZipFile(archive) as source:
        entries = source.infolist()
        require(len(entries) == 1 and entries[0].filename == output.name,
                f"Artifact must contain only {output.name}")
        entry = entries[0]
        mode = entry.external_attr >> 16
        require(not entry.is_dir() and not stat.S_ISLNK(mode)
                and 0 < entry.file_size <= MAX_PACKAGE_BYTES, "Invalid or oversized package entry")
        require(not output.exists(), f"Duplicate output: {output.name}")
        with source.open(entry) as incoming, output.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)


def download(repo, row, output, scratch):
    archive = scratch / f"{row['id']}.zip"
    with archive.open("wb") as stream:
        subprocess.run(["gh", "api", f"repos/{repo}/actions/artifacts/{row['id']}/zip"],
                       check=True, stdout=stream)
    unpack(archive, row["digest"], output)
    archive.unlink()


def seal(directory, plan, android_source):
    names = set(packages(plan["tag"]))
    require({path.name for path in directory.iterdir()} == names, "Expected exactly five platform packages")
    provenance = dict(plan, android_build=android_source,
                      package_sha256={name: digest_file(directory / name) for name in sorted(names)})
    (directory / "SOURCE-PROVENANCE.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    names.add("SOURCE-PROVENANCE.json")
    (directory / "SHA256SUMS").write_text("".join(
        f"{digest_file(directory / name)}  {name}\n" for name in sorted(names)), encoding="utf-8")


def validate_dist(directory, tag):
    expected = set(packages(tag)) | METADATA
    require({path.name for path in directory.iterdir()} == expected, "Unexpected or missing release files")
    actual = (directory / "SHA256SUMS").read_text(encoding="utf-8")
    wanted = "".join(f"{digest_file(directory / name)}  {name}\n" for name in sorted(expected - {"SHA256SUMS"}))
    require(actual == wanted, "Release checksums do not match the exact package set")


def assemble(plan, run_id, directory):
    fresh = make_plan(plan["repository"], plan["tag"], plan["source_run"],
                      plan["android_commit"], plan["controller_commit"])
    require(fresh == plan, "Source metadata or tag changed after validation")
    repo = plan["repository"]
    run = gh_json(f"repos/{repo}/actions/runs/{run_id}")
    require(run_id != plan["source_run"] and run["head_sha"] == plan["controller_commit"]
            and run["repository"]["id"] == plan["repository_id"]
            and run["head_repository"]["id"] == plan["repository_id"]
            and run["path"] == ".github/workflows/release.yml" and run["event"] == "workflow_dispatch",
            "Android artifact run is not this recovery controller")
    apk = packages(plan["tag"])[-1]
    source_name = f"android-source-{run_id}"
    rows = list_rows(repo, run_id, "artifacts")
    selected = select_artifacts([row for row in rows if row["name"] in (artifact_name(apk), source_name)],
                                {artifact_name(apk), source_name}, run_id,
                                plan["controller_commit"], plan["repository_id"])
    directory.mkdir(parents=True, exist_ok=True)
    require(not any(directory.iterdir()), "Output directory must be empty")
    with tempfile.TemporaryDirectory() as temp:
        scratch = Path(temp)
        for package in packages(plan["tag"], android=False):
            download(repo, plan["artifacts"][artifact_name(package)], directory / package, scratch)
        download(repo, selected[artifact_name(apk)], directory / apk, scratch)
        source = scratch / "android-source.json"
        download(repo, selected[source_name], source, scratch)
        android_source = json.loads(source.read_text(encoding="utf-8"))
    require(android_source == {"source_sha": plan["android_commit"], "run_id": run_id,
                               "version": plan["tag"][1:]}, "APK source attestation does not match the approved repair")
    plan = dict(plan, recovery_run=run_id, android_artifacts=selected)
    seal(directory, plan, android_source)
    validate_dist(directory, plan["tag"])


def draft(plan, directory):
    repo, tag = plan["repository"], plan["tag"]
    require(tag_commit(repo, tag) == plan["desktop_commit"], "Release tag changed")
    validate_dist(directory, tag)
    provenance = json.loads((directory / "SOURCE-PROVENANCE.json").read_text(encoding="utf-8"))
    require(all(provenance.get(key) == value for key, value in plan.items()), "Provenance differs from validated plan")
    release = release_for(repo, tag)
    block = ("<!-- recovery-source:start -->\n## Android build recovery\n\n"
             f"Desktop packages are reused without rebuilding from run [{plan['source_run']}]"
             f"(https://github.com/{repo}/actions/runs/{plan['source_run']}), commit `{plan['desktop_commit']}`.\n\n"
             f"Android alone is built from repair commit `{plan['android_commit']}`. "
             f"The existing `{tag}` tag is unchanged. These packages do **not** share one source commit.\n\n"
             "See `SOURCE-PROVENANCE.json` for artifact IDs and archive/package SHA-256 values. "
             "`SHA256SUMS` covers all five packages and that provenance file.\n<!-- recovery-source:end -->")
    body = (release.get("body") or "") if release else ""
    body = re.sub(r"<!-- recovery-source:start -->.*?<!-- recovery-source:end -->", "", body, flags=re.S).strip()
    body = (body + "\n\n" + block).strip() + "\n"
    payload = {"tag_name": tag, "name": f"G-Diffuser {tag[1:]}", "body": body,
               "draft": True, "prerelease": "-" in tag}
    endpoint = f"repos/{repo}/releases" + (f"/{release['id']}" if release else "")
    with tempfile.TemporaryDirectory() as temp:
        payload_path = Path(temp) / "release.json"
        payload_path.write_text(json.dumps(payload), encoding="utf-8")
        subprocess.run(["gh", "api", "--method", "PATCH" if release else "POST", endpoint,
                        "--input", str(payload_path)], check=True, stdout=subprocess.DEVNULL)
    release_for(repo, tag)
    subprocess.run(["gh", "release", "upload", tag, "--repo", repo, "--clobber",
                    *[str(directory / name) for name in sorted(set(packages(tag)) | METADATA)]], check=True)
    uploaded = release_for(repo, tag)
    require(uploaded is not None and {asset["name"] for asset in uploaded["assets"]}
            == set(packages(tag)) | METADATA, "Draft upload is incomplete")


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    plan_parser = commands.add_parser("plan")
    plan_parser.add_argument("--repo", required=True)
    plan_parser.add_argument("--tag", required=True)
    plan_parser.add_argument("--source-run", required=True, type=int)
    plan_parser.add_argument("--android-commit", required=True)
    plan_parser.add_argument("--controller-commit", required=True)
    plan_parser.add_argument("--output", required=True, type=Path)
    for command in ("assemble", "draft"):
        child = commands.add_parser(command)
        child.add_argument("--plan", required=True, type=Path)
        child.add_argument("--directory", required=True, type=Path)
        if command == "assemble":
            child.add_argument("--run-id", required=True, type=int)
    args = parser.parse_args()
    if args.command == "plan":
        plan = make_plan(args.repo, args.tag, args.source_run, args.android_commit, args.controller_commit)
        args.output.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        if os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                output.write(f"version={args.tag[1:]}\nandroid_commit={args.android_commit}\n")
        print(json.dumps(plan, indent=2))
    else:
        plan = json.loads(args.plan.read_text(encoding="utf-8"))
        if args.command == "assemble":
            assemble(plan, args.run_id, args.directory)
        else:
            draft(plan, args.directory)


if __name__ == "__main__":
    main()
