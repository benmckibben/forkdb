# Releasing

A release is made by the **Release build** workflow (`.github/workflows/release.yml`) from a tag, and finished by hand. Pushing a tag `vX.Y.Z` builds the extension for Linux, macOS and Windows, the CLI for Linux and macOS, the Python wheels and the npm tarball, tests each of them, writes `SHA256SUMS`, and creates a **draft** GitHub release with those ten files and the CHANGELOG entry as its notes. It publishes nothing: you read the draft and press Publish, and you upload the packages to PyPI and npm yourself. This is the checklist; it will be wrong where this repository has moved on, so fix it when it is.

## Before tagging

- [ ] **Decide the version number.** The places that carry the current one: `ext/forkdb_ext.c` (the `version` row of `forkdb_info`), the three tests that assert it (`test/ext_py_test.py`, `test/ext_node_test.js`, `test/ext_bsq_test.js`), and the heading and one sentence of `docs/KNOWN-LIMITS.md` ("v0.1.0"). The CLI reports no version; decide whether it should.
- [ ] **CHANGELOG.md:** the section of the release is headed `## [X.Y.Z] - YYYY-MM-DD` (above it, an `## Unreleased` for what comes next). The workflow refuses a tag without that heading, or with nothing under it, and the section is the text of the draft release.
- [ ] **docs/STABILITY.md:** is what it says still true on the day? It promises nothing today, and a release is where that could change.
- [ ] **CI is green on the exact commit** (every job listed in `.github/workflows/ci.yml`). The workflow checks this itself: it refuses a tag whose commit has no completed, successful CI run from the push to `main`; if the tag came too early, re-run it once CI is done.
- [ ] **Dry-run the release workflow:** Actions, Release build, Run workflow on `main`. Without a tag it builds, tests and verifies everything and makes no release. Read every job.
- [ ] **Run the full gates locally.** CI runs only the `quick` variants of the store, VFS and fkx suites. Run `scripts/test-store.sh`, `scripts/test-vfs.sh` and `scripts/test-fkx.sh` without `quick` (the store suite took about 33 minutes on one machine), and `scripts/test-threads.sh`.
- [ ] **README table and KNOWN-LIMITS.md match what CI ran** (the table is a claim about CI; check it against the job list).
- [ ] **Run `scripts/verify-windows.ps1` on a real Windows machine** (CI runs the same smoke tests on a Windows runner; this installs the packages the way a user would, on a desktop, and adds a compaction check, a hard kill and a second-process check).
- [ ] **A clean checkout builds:** `scripts/build-step4.sh test`, and `scripts/build-ext.sh` on each platform in the table.
- [ ] **No generated files are committed** (build output, vendored SQLite, `node_modules`); `git status` is clean.

## Tagging

- [ ] `git tag -a vX.Y.Z -m "..."` on the merge commit that is on `main` (the version in the five places, as `vMAJOR.MINOR.PATCH`: no pre-release tags), and push the tag. The workflow starts.
- [ ] **Read the draft release** (Releases, Drafts): the notes are the CHANGELOG entry; ten files are attached (3 extension archives, 2 CLI archives, 3 wheels, the npm tarball, `SHA256SUMS`). Download them and run `sha256sum --check SHA256SUMS` (macOS: `shasum -a 256 -c SHA256SUMS`). Then Publish.
- [ ] **Publish the packages by hand**, from those downloaded files and after the release is published: `twine upload forkdb-X.Y.Z-*.whl` (PyPI project `forkdb`) and `npm publish sqlite-forkdb-X.Y.Z.tgz --access public` (npm package `sqlite-forkdb`). Neither command has been run for this project; the workflow never holds a registry token.
- [ ] **If the workflow fails:** a failure in the first job (tag, version, changelog, CI) made nothing: fix it and, if the tag was wrong, delete it (`git push --delete origin vX.Y.Z`, `git tag -d vX.Y.Z`) and tag again. A failure in a later job: read it; re-run the failed jobs if it was the runner, otherwise fix `main` and tag the next version (a tag that has been public should not move). A draft release that exists for a tag is never overwritten: delete the draft first.

## If the repository becomes public

It is private today. Before that:

- [ ] **Enable private vulnerability reporting** (SECURITY.md sends reporters there; I could not check whether it is on).
- [ ] **Know what the history contains about the benchmark host.** `infra/bench` (an OpenTofu definition of a throwaway AWS host, with an IAM user's name and cost figures in its README) was removed from the tree and is still in history; read it before making the repository public if that matters. The IAM policy there used an `ACCOUNT_ID` placeholder, not a real account number. `bench/fkbench.c` stays (`scripts/bench-compact.sh` builds it).
- [ ] **Read the history for anything that should not be public**; this checklist did not.
- [ ] **Licences:** the README offers Apache-2.0 or MIT at the user's option and both files are present; GitHub's licence detection shows only Apache-2.0.

