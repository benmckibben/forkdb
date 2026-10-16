# Releasing

A release is made by the **Release build** workflow (`.github/workflows/release.yml`) from a tag, and finished by hand. Pushing a tag `vX.Y.Z` builds the extension for Linux, macOS and Windows, the CLI for Linux and macOS, the Python wheels and the npm tarball, tests each of them, writes `SHA256SUMS`, and creates a **draft** GitHub release with those ten files and the CHANGELOG entry as its notes. It publishes nothing: you read the draft and press Publish, and you upload the packages to PyPI and npm yourself. This is the checklist; it will be wrong where this repository has moved on, so fix it when it is.

The order matters:

1. **Before tagging**: everything that goes into the tagged commit.
2. **Making the repository public**: the settings first, then the exposure checks, then the flip. The tag rules must exist before the first tag does.
3. **Tagging**, then the draft release.
4. **Publishing the packages**, then **After publishing**.

A step marked *(owner)* changes a repository or registry setting, or publishes something, and is done by the repository owner, not by a script or an assistant.

## Before tagging

- [X] **Decide the version number.** The five places that carry it, which `scripts/check-versions.sh` compares and the workflow runs: the extension (`ext/forkdb_ext.c`, the `version` row of `forkdb_info`), the CLI (`FORKDB_VERSION` in `cli/forkdb.c`, shown by `forkdb --version`), `python/forkdb/__init__.py`, `python/pyproject.toml` and `node/package.json`. Not compared by the script, so check them by hand: the three tests that assert it (`test/ext_py_test.py`, `test/ext_node_test.js`, `test/ext_bsq_test.js`), and the heading and one sentence of `docs/KNOWN-LIMITS.md` ("v0.1.0"). *Ben's note: decided v0.1.0.*
- [X] **CHANGELOG.md:** the section of the release is headed `## [X.Y.Z] - YYYY-MM-DD` (above it, an `## Unreleased` for what comes next). The workflow refuses a tag without that heading, or with nothing under it, and the section is the text of the draft release.
- [ ] **Say what state the project is in, in the tagged commit.** `SECURITY.md` ("Status: pre-release. There are no released versions") and `docs/STABILITY.md` ("there is no tag and no release") both describe the time before the first release, and `README.md` still calls this checklist "the checklist for a release (there has not been one)". Is what each says still true on the day? STABILITY.md promises nothing today, and a release is where that could change.
- [ ] **CI is green on the exact commit** (every job listed in `.github/workflows/ci.yml`). The workflow checks this itself: it refuses a tag whose commit has no completed, successful CI run from the push to `main`; if the tag came too early, re-run it once CI is done.
- [ ] **Nothing is waiting on `main`.** Merge or close open pull requests (a dependency-update pull request counts), so that the commit you tag is the final one and the CI run the workflow looks for is on it. Delete merged branches (`git branch -r` should show `main` and nothing stale).
- [X] **Dry-run the release workflow:** Actions, Release build, Run workflow on `main`. Without a tag it builds, tests and verifies everything and makes no release. Read every job.
- [ ] **Run the full gates locally.** CI runs only the `quick` variants of the store, VFS and fkx suites. Run `scripts/test-store.sh`, `scripts/test-vfs.sh` and `scripts/test-fkx.sh` without `quick` (the store suite took about 33 minutes on one machine), and `scripts/test-threads.sh`.
- [ ] **README table and KNOWN-LIMITS.md match what CI ran** (the table is a claim about CI; check it against the job list).
- [ ] **Run `scripts/verify-windows.ps1` on a real Windows machine** (CI runs the same smoke tests on a Windows runner; this installs the packages the way a user would, on a desktop, and adds a compaction check, a hard kill and a second-process check).
- [ ] **Install the macOS wheel on a real Apple-silicon Mac** and run `FORKDB_SMOKE_INSTALLED=1 python test/smoke_py.py` with the venv's Python against it, if nobody has. macOS is the platform with the least hand testing, and CI runs it only on a runner. (There is no macOS counterpart of the Windows script; the smoke test is the check.)
- [ ] **A clean checkout builds:** `scripts/build-step4.sh test`, and `scripts/build-ext.sh` on each platform in the table.
- [ ] **No generated files are committed** (build output, vendored SQLite, `node_modules`); `git status` is clean.

## Making the repository public

Skip this section once the repository is public. The repository starts private, which is why most of what follows does not exist yet. Do it in this order, and do not merge or tag in the gap between the flip and the rules: some of these settings (rulesets on a private repository, private vulnerability reporting) may not be available before the flip on the owner's plan; that was not checked, so apply them straight after it if so.

### Settings *(owner)*

Match the other repositories that were released this way (nullroute, cloakenv). Read the result back afterwards: `gh api repos/OWNER/forkdb/rulesets` and `gh api repos/OWNER/forkdb`.

- [x] **A branch ruleset on the default branch** (target: the default branch): block deletion; block force pushes (non-fast-forward); require a pull request (1 approval, dismiss stale approvals on a new push, merge commits only); require the status checks below. Bypass: the repository admin role, always, so that the owner can merge with admin rights.
- [x] **Required status checks.** Use the job names exactly as a real run reports them: `gh api repos/OWNER/forkdb/actions/runs/RUN_ID/jobs --jq '.jobs[].name' | sort -u` on a green `main` run. Matrix jobs report one name per combination (for example "Extension tests (…, ubuntu-latest)"), so list each. Decide each job on purpose: the test, sanitizer and CLI jobs yes; a benchmark job ("What nosync saves") probably not. **Do not require anything from `release.yml`**: it runs only for some paths and for tags, and a required check that never runs blocks the pull request for good. Whether the branch must be up to date before merging ("strict"): nullroute does, cloakenv does not; with this many jobs on three operating systems, strict means re-running all of them after every other merge, so start without it.
- [x] **A tag ruleset on `refs/tags/v*`**: block deletion, update, force pushes and creation; bypass for the repository admin role. Without it a published `v0.1.0` can be moved or deleted by anyone with write access. Creating the release tag then needs that bypass; that is the intended gate.
- [x] **Merge methods:** merge commits only (turn squash and rebase off), and **delete head branches on merge** on. (Both are currently the opposite: all three methods are on, and merged branches stay.)
- [x] **Wiki off** (it is on today). Add a **description**, a **homepage** (if there is one) and **topics**: they are what a visitor sees first, and the other two repositories have them.
- [ ] **Private vulnerability reporting on.** `SECURITY.md` sends reporters to it. Check it: `gh api repos/OWNER/forkdb/private-vulnerability-reporting` must say `{"enabled":true}`. On the private repository it answers 404, so it is off or unavailable.
- [ ] **Actions settings** (Settings, Actions, General; not readable with the token an assistant has, so look at them): default workflow permissions **read-only**; fork pull requests need approval before their workflows run (**all outside collaborators**). CI here is heavy (fuzzers, sanitizers, macOS and Windows runners), which is what a drive-by pull request would waste. `ci.yml` declares no `permissions:` block and so depends on that default.
- [ ] **Security features** (Settings, Code security): secret scanning with push protection, and Dependabot alerts, on. Dependabot version updates for the Actions are already configured in the repository.
- [X] **Immutable releases**, if the setting exists on this plan: consider it before the first publish. It stops the assets and the tag of a release published after it is turned on from being changed. Not checked that it is available, or what exactly it covers.

### Files

- [ ] **CONTRIBUTING.md** (nullroute and cloakenv have one, this repository does not). Say how to report a bug, how to run the tests, and what a pull request has to pass. Add an issue template and a pull request template only if you want them.

### What the flip publishes

Everything below is visible to anyone once the repository is public, and cannot be taken back (a copy survives in any clone or fork).

- [X] **Know what the history contains about the benchmark host.** `infra/bench` (an OpenTofu definition of a throwaway AWS host, with an IAM user's name and cost figures in its README) was removed from the tree and is still in history; read it before making the repository public if that matters. The IAM policy there used an `ACCOUNT_ID` placeholder, not a real account number. It names a storage bucket and a state key for that host. `bench/fkbench.c` stays (`scripts/bench-compact.sh` builds it). **Decide, and write the decision down:** accept it, or rewrite the history before the flip (every commit hash changes, and so does every reference to one: pull requests, the changelog, the tag you have not made yet). Do not tag first. *Ben's note: accepted.*
- [X] **Read the history for anything that should not be public.** A scan of the history and the tree for key and token patterns found no credentials; that was a scan, not a reading, and it needs repeating on the commit you will tag. Also look for private names and paths, and for credits to the people or assistants who helped (`git grep` the tree, `git log --format=%B | grep` the messages). Some test file headers credit the author and reviewer by name; remove or keep them on purpose. *Ben's note: done by agents 2026-10-15.*
- [X] **Pull requests, issues and comments become public too.** There are dozens of pull requests; some of their descriptions credit the assistants that wrote the code, and one quotes an IAM user's name. Read them (`gh pr list --state all --json number,title,body`), and edit what should not stay.
- [X] **Past Actions runs and artifacts become public.** There are over a hundred runs and dozens of artifacts, with their logs. Skim a few logs for paths, hostnames and tokens; delete old runs and artifacts that should not stay.
- [X] **Branches:** every branch on the remote becomes public. Keep `main` and delete the rest first.
- [X] **Licences:** the README offers Apache-2.0 or MIT at the user's option and both files are present; GitHub's licence detection shows only Apache-2.0.

## Tagging

- [ ] `git tag -a vX.Y.Z -m "..."` on the merge commit that is on `main` (the version in the five places, as `vMAJOR.MINOR.PATCH`: no pre-release tags), and push the tag. The workflow starts. With the tag ruleset in place, creating the tag needs the owner's bypass.
- [ ] **Read the draft release** (Releases, Drafts): the notes are the CHANGELOG entry; ten files are attached (3 extension archives, 2 CLI archives, 3 wheels, the npm tarball, `SHA256SUMS`). Download them and run `sha256sum --check SHA256SUMS` (macOS: `shasum -a 256 -c SHA256SUMS`). Then Publish.
- [ ] **If the workflow fails:** a failure in the first job (tag, version, changelog, CI) made nothing: fix it and, if the tag was wrong, delete it (`git push --delete origin vX.Y.Z`, `git tag -d vX.Y.Z`) and tag again. A failure in a later job: read it; re-run the failed jobs if it was the runner, otherwise fix `main` and tag the next version (a tag that has been public should not move). A draft release that exists for a tag is never overwritten: delete the draft first. (With the tag ruleset on, deleting a tag also needs the owner's bypass.)

## Publishing the packages *(owner)*

The workflow never holds a registry token, so this is done by hand, from the files downloaded from the **published** release, with `twine` and `npm`. Neither command has been run for this project. A published version cannot be taken back: neither registry lets you reuse a version number, and PyPI does not let you re-upload a file name even after deleting the release.

- [ ] **Check the names again the same day.** `forkdb` on PyPI and `sqlite-forkdb` on npm were both unclaimed when last looked at; a 404 is not a reservation. (The plain npm name `forkdb` belongs to an unrelated package, which is why this one is `sqlite-forkdb`.)
- [ ] **Look at what will be uploaded.** `twine check forkdb-X.Y.Z-*.whl`; `npm publish sqlite-forkdb-X.Y.Z.tgz --dry-run` and read the file list (the library for each platform, `index.js`, `index.d.ts`, `README.md`, both licence files, and nothing else). Optional rehearsal: upload the wheels to TestPyPI first (a separate account and token, and a separate namespace, so it costs nothing on the real index).
- [ ] **Use a token that can do only this.** Short expiry, created for this release only. A token limited to one project cannot exist before the project does, so the first upload to PyPI is likely to need a wider token than later ones; npm has the same shape for a new package. Check what each registry offers at the time, take the narrowest option, and do not leave it behind: after the last upload, **revoke it at the registry and delete the local copy** (`~/.pypirc`, `~/.npmrc`, and any environment variable or password-manager entry it passed through). Two-factor authentication stays on for both accounts.
- [ ] **Upload:** `twine upload forkdb-X.Y.Z-*.whl` (PyPI project `forkdb`) and `npm publish sqlite-forkdb-X.Y.Z.tgz --access public` (npm package `sqlite-forkdb`). Upload the files that are in the release, not a rebuild.

## After publishing

- [ ] **Install it the way a user would,** in a clean environment, from the registry: `pip install forkdb==X.Y.Z` in a fresh virtual environment and `npm install sqlite-forkdb@X.Y.Z` in an empty directory, then run `FORKDB_SMOKE_INSTALLED=1 python test/smoke_py.py` (the venv's Python) and `FORKDB_SMOKE_INSTALLED=1 node --no-warnings test/smoke_node.js` (from the empty directory) against them, on each OS you can reach. This is the check the release workflow cannot make, because it tests the files before they are on the registry.
- [ ] **Compare what the registry serves with the release:** `pip download forkdb==X.Y.Z --no-deps` and `npm pack sqlite-forkdb@X.Y.Z`, then `sha256sum` against `SHA256SUMS`; they should match.
- [ ] **Revoke the tokens and delete the credentials files** (see above), and check that nothing is left behind in shell history or environment.
- [ ] **Put install instructions in the README,** now that they are true (there are none today), and say which platforms have a prebuilt package (Linux x86_64, macOS arm64, Windows x86_64: anything else builds from source).
- [ ] **Update the status lines** in `SECURITY.md` and `docs/STABILITY.md` if they were left for this point, and check that the README's link to this file no longer says there has not been a release.
- [ ] **Check the public page once** as a stranger would see it: the repository front page, the Releases page, the Security tab (is "Report a vulnerability" there?), the PyPI and npm pages (licence, README, links).
