# Releasing

There has been no release, and nothing automates one: no release workflow, no packaging, no binaries. This is the checklist for doing it by hand, written from what exists. It will be wrong where this repository has moved on; fix it when it is.

## Before tagging

- [ ] **Decide the version number.** The places that carry the current one: `ext/forkdb_ext.c` (the `version` row of `forkdb_info`), the three tests that assert it (`test/ext_py_test.py`, `test/ext_node_test.js`, `test/ext_bsq_test.js`), and the heading and one sentence of `docs/KNOWN-LIMITS.md` ("v0.1.0"). The CLI reports no version; decide whether it should.
- [ ] **CHANGELOG.md:** turn the Unreleased section into the version, with the date, and open a new Unreleased.
- [ ] **docs/STABILITY.md:** is what it says still true on the day? It promises nothing today, and a release is where that could change.
- [ ] **CI is green on the exact commit** (every job listed in `.github/workflows/ci.yml`).
- [ ] **Run the full gates locally.** CI runs only the `quick` variants of the store, VFS and fkx suites. Run `scripts/test-store.sh`, `scripts/test-vfs.sh` and `scripts/test-fkx.sh` without `quick` (the store suite took about 33 minutes on one machine), and `scripts/test-threads.sh`.
- [ ] **README table and KNOWN-LIMITS.md match what CI ran** (the table is a claim about CI; check it against the job list).
- [ ] **Run `scripts/verify-windows.ps1` on a real Windows machine** (CI runs the same smoke tests on a Windows runner; this installs the packages the way a user would, on a desktop, and adds a compaction check, a hard kill and a second-process check).
- [ ] **A clean checkout builds:** `scripts/build-step4.sh test`, and `scripts/build-ext.sh` on each platform in the table.
- [ ] **No generated files are committed** (build output, vendored SQLite, `node_modules`); `git status` is clean.

## Tagging

- [ ] `git tag -a vX.Y.Z -m "..."` on the merge commit, and push the tag.
- [ ] Create the GitHub release from the CHANGELOG entry. **There are no artifacts to attach**: users build the extension and CLI themselves (`scripts/build-ext.sh`, `scripts/build-step4.sh cli`). If binaries are ever distributed, someone has to decide how they are built, signed and checked; that work does not exist.

## If the repository becomes public

It is private today. Before that:

- [ ] **Enable private vulnerability reporting** (SECURITY.md sends reporters there; I could not check whether it is on).
- [ ] **Know what the history contains about the benchmark host.** `infra/bench` (an OpenTofu definition of a throwaway AWS host, with an IAM user's name and cost figures in its README) was removed from the tree and is still in history; read it before making the repository public if that matters. The IAM policy there used an `ACCOUNT_ID` placeholder, not a real account number. `bench/fkbench.c` stays (`scripts/bench-compact.sh` builds it).
- [ ] **Read the history for anything that should not be public**; this checklist did not.
- [ ] **Licences:** the README offers Apache-2.0 or MIT at the user's option and both files are present; GitHub's licence detection shows only Apache-2.0.

