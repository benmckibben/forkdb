# Contributing

Thanks for looking at forkdb. Bug reports and pull requests are welcome. forkdb is unstable (see [docs/STABILITY.md](docs/STABILITY.md)): the on-disk format, the SQL functions, the CLI and the C interfaces can all change. For a security problem, please follow [SECURITY.md](SECURITY.md) instead of opening a public issue.

Please read the [README](README.md) before changing behaviour: it says what is tested, against what, and what is not, and every claim in it should stay true. For the store and the branch layer, read [docs/DURABLE-STORE.md](docs/DURABLE-STORE.md) and [docs/FKX-DESIGN.md](docs/FKX-DESIGN.md) first. This is memory-unsafe C that runs inside your process, and what it promises about durability is the point of it.

## Reporting a bug

Open an issue with:

- the forkdb version (`forkdb --version`, or `select * from forkdb_info` from SQL), your operating system and architecture, and how you use it: the CLI, the Python wheel, the npm package, or the library in your own program;
- for the extension, the host: Python, Node or other, and `select sqlite_version()`;
- the smallest sequence of statements or commands that shows the problem, what you expected and what happened;
- for a crash or a store that will not open, the store file if you can share it (it is a copy of your data: do not, if it holds anything private).

## Building and testing

Linux needs gcc, macOS an Apple clang that takes gcc-style flags, and Windows mingw-w64 gcc (`scripts/build-ext.sh` does not drive MSVC). Most scripts also need curl, sha256sum and python3, and the Node tests need Node 24 (`node:sqlite`). SQLite is not a dependency you install: the scripts fetch a pinned amalgamation (3.50.0) into `spike/vendor/`, which is ignored by git, and check its hash.

```sh
./vfs/build.sh sqlite                 # once per checkout: compile the pinned SQLite (under a minute)
./scripts/build-step4.sh test         # the CLI and the extension, and a smoke test of the CLI

scripts/test-store.sh quick           # the durable store: unit, file and power-loss tests
scripts/test-vfs.sh quick             # SQLite on the store
scripts/test-fkx.sh quick             # the branch layer
scripts/test-threads.sh               # ThreadSanitizer
scripts/test-fuzz.sh                  # the recovery fuzzer
scripts/test-cli.sh                   # the CLI against bad input
scripts/test-ext.sh                   # the loadable extension from Python's sqlite3
scripts/test-ext-node.sh              # ... from Node's node:sqlite
scripts/test-ext-bsq.sh               # ... from better-sqlite3 (installs it with npm, into build/)
scripts/test-oldest-sqlite.sh         # ... in SQLite 3.24.0, the oldest supported
scripts/test-pytest-example.sh        # the pytest plugin in examples/
scripts/test-release.sh               # the release scripts
```

CI runs the `quick` variants of the store, VFS and fkx suites. If you change `store/`, `vfs/` or `fkx/`, run them without `quick` as well (the store suite takes about half an hour) and say that you did.

## What CI checks

Every pull request must pass every job of [`.github/workflows/ci.yml`](.github/workflows/ci.yml): the store, VFS, fkx and CLI tests on Linux, the store tests on macOS and Windows, the recovery fuzzer under the address and undefined-behaviour sanitizers, the thread tests under ThreadSanitizer, the extension from Python, from Node's `node:sqlite` and from better-sqlite3 (the last two also on macOS and Windows), the extension in SQLite 3.24.0, the pytest example, the `nosync` benchmark, and the tests of the release scripts. The branch rules require all of them, one approval, and a merge commit.

A change to the build or to the packages also runs [`.github/workflows/release.yml`](.github/workflows/release.yml), which builds the library, the CLI, the wheels and the npm tarball on all three platforms and tests them as installed.

## Pull requests

- Branch from `main`, one change per pull request. Start the commit messages and the title with the kind of change and the area, as the history does (`fix:`, `test:`, `docs:`, `ci:`, `build:`, `store:`, `ext:`...).
- Say what you ran and what you did not. A passing unit test is not an end-to-end check, and a claim in a description that was not measured should say so.
- A fix comes with a test that fails without it. If you change a safety check, check that the tests notice when it is removed (the repository does this with mutants for the recovery code and the tests of the release scripts).
- If a change touches the on-disk format, the SQL functions, the CLI or the C interfaces, say so in the description and add a line under `## Unreleased` in [CHANGELOG.md](CHANGELOG.md). Update the README and the docs in the same pull request when the claim they make changes.
- Do not commit build output, the vendored SQLite or `node_modules` (they are in `.gitignore`).
- Third-party GitHub Actions are pinned to a full commit SHA with the version in a trailing comment; Dependabot proposes the updates.

## Where things are

`store/` the durable log and its file backends; `vfs/` the SQLite VFS; `fkx/` the branch layer and the session driver; `ext/` the loadable extension; `cli/` the CLI; `python/` and `node/` the packages; `test/` and `scripts/` the tests and the scripts that run them; `examples/` the pytest plugin; `docs/` the design and the limits.
