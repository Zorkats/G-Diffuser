# Release builds and Android recovery

**Release** and **Nightly** are the two CI launch points. The platform workflows are reusable
workers, not independent builds. A tag push builds all platforms and prepares a draft. Manual
Release recovery builds only Android and reuses verified desktop packages from a failed release.
Publishing a stable release remains a maintainer action.

## Recover only Android

Use this path when a tagged Release run has four successful desktop builds and a failed Android
build. You need repository write access, an authenticated GitHub CLI (`gh auth status`), and the
four existing `ANDROID_KEYSTORE_*` / `ANDROID_KEY_*` repository secrets used by the Android worker.
The recovery workflow must already be committed and pushed to the default branch.

For **v1.2.1**, run this PowerShell command from the repository after pushing the workflow changes:

```powershell
gh workflow run release.yml --repo Zorkats/G-Diffuser --ref main `
  -f recovery_tag=v1.2.1 `
  -f source_run=41581952351 `
  -f android_commit=21f74387e203d1420d928d39981d9845000cf128
if ($LASTEXITCODE -ne 0) { throw 'Recovery dispatch failed' }

gh run list --repo Zorkats/G-Diffuser --workflow release.yml --event workflow_dispatch --limit 5
```

Open the returned run with `gh run view RUN_ID --repo Zorkats/G-Diffuser --web`, replacing
`RUN_ID` with the recovery run's numeric ID. Expect the gate and Android build to succeed,
Windows/Linux/macOS jobs to be **skipped**, and the draft job to succeed. Neither desktop
packages nor the existing `v1.2.1` tag are rebuilt or moved.

The repair commit fixes Android configuration by excluding desktop-only test targets. Re-running
the original failed run would still use its original commit, without that repair. Do not delete,
recreate, or force-push the release tag to work around this.

### What gets checked

| Check | Required result |
| --- | --- |
| Controller | Manual dispatch from the default branch; the repair is an ancestor of the controller commit. |
| Source run | Same repository, Release tag-push run, completed with failure, unchanged tag commit. |
| Desktop builds | Windows x64, Linux x64, Linux arm64, and macOS arm64 all succeeded. |
| Retained artifacts | Exactly those four, unexpired, with matching run/repository/commit and SHA-256 archive digests. |
| Android | Signed release APK from the requested full repair SHA; recorded checkout SHA and run ID match. |
| Package archives | Each artifact contains only the expected package file; its downloaded archive digest matches GitHub metadata. |
| Draft | Exactly five packages, `SHA256SUMS`, and `SOURCE-PROVENANCE.json`; an already published release is refused. |

The gate validates metadata before spending time on Android. The draft job checks source metadata
again and verifies every downloaded artifact. It never runs the retained package contents.

### Inspect the draft before publishing

```powershell
gh release view v1.2.1 --repo Zorkats/G-Diffuser --json isDraft,tagName,assets,body
gh release view v1.2.1 --repo Zorkats/G-Diffuser --web
```

- Confirm `isDraft` is `true` and all five platform packages are attached.
- Confirm the notes and `SOURCE-PROVENANCE.json` identify desktop commit
  `cc760ddb405bd62a223766dd2afb903f681438b1` and Android repair commit
  `21f74387e203d1420d928d39981d9845000cf128` separately. GitHub's automatic source archives
  still represent the original tag, **not** the later Android repair.
- Add or review the player-facing changelog. Recovery preserves existing draft notes and adds
  a source disclosure; it does not invent a changelog for a draft that did not exist.
- Download the attached files and verify `SHA256SUMS`. On a system with `sha256sum`, run
  `sha256sum --check SHA256SUMS` in the download directory. It covers all five packages and
  `SOURCE-PROVENANCE.json`, not itself.
- Install the APK on an arm64 Android device. Check startup, first-time asset setup, menus,
  touch controls, and a race. If an earlier project-signed APK is installed, verify this APK
  upgrades it without uninstalling or losing saves.

Do not publish while a recovery run or retry is active. Once the run succeeds and you accept
the assets, publish the draft through GitHub. Recovery never publishes automatically.

### If recovery fails

| Failure | Next action |
| --- | --- |
| Gate rejects source/version/ancestry | Check the tag, source run ID, full repair SHA, and selected branch. Do not bypass the check. |
| Artifact expired, missing, or corrupted | Stop. This path cannot recover absent packages; decide on a separate rebuild rather than silently mixing artifacts. |
| Android fails | Inspect its log. A failed-jobs retry on this recovery run keeps the desktop jobs skipped. If the fix needs another source commit, push it and dispatch a fresh recovery with that SHA. |
| Draft/upload fails after Android succeeds | Retry **failed jobs** on the recovery run. It reuses that run's APK and retained plan, without rebuilding Android or desktops. |
| Draft contains unexpected assets | Inspect the draft manually and remove only unintended draft assets before retrying. Published releases are never repaired in place. |

To retry failed jobs, substitute the **recovery** run ID, not `41581952351`:

```powershell
gh run rerun RUN_ID --repo Zorkats/G-Diffuser --failed
```

## Other release paths

- A new `v*` tag push starts **Release**, builds all platforms, and prepares a draft only when
  all builds succeed. Use a numeric three-part version such as `v1.2.2` or `v1.2.2-rc1`.
- **Nightly** keeps its scheduled and manual default-branch builds. It calls the same platform
  workers, including signed Android, and retains its existing rolling prerelease behavior.
- Local Android debug builds remain available through the [Android build guide](ANDROID.md#building).

## Local verification

```powershell
python -m unittest discover -s tools -p test_release_recovery.py -v
```

These tests cover validation, safe artifact extraction, package checksums, synthetic assembly/draft
requests, workflow launch points, desktop skipping, and draft prerequisites. They do not run
GitHub Actions, sign an APK, or prove Android device behavior.
