# Security

Status: **pre-release.** There are no released versions and so no supported ones; fixes go to `main`. forkdb is C code that has had no external audit. This note says what the code does and does not defend against, as far as it has been checked, and how to report a problem.

## Reporting a vulnerability

Please report it privately, not in a public issue: use GitHub's private vulnerability reporting (the repository's **Security** tab, then **Report a vulnerability**). Include the smallest input that shows it; for a crash on a crafted store file, the file itself. The maintainer is [Ben McKibben](https://github.com/benmckibben).

## What forkdb is, as a security boundary

forkdb is a library that runs **inside your process**, in memory-unsafe C, with that process's privileges. It is not a sandbox and not a server: it has no users, no authentication and no access control beyond the permissions of the files it opens.

- **It takes paths from its caller and does not check them.** `forkdb_init(path)`, `forkdb_open_db`, `forkdb_compact` and the CLI open, create and rewrite whatever file the process can write (compaction writes a temporary file beside the store and renames it over the store). There is no allow-list and no path validation in the extension or in `fkx`. **Do not let untrusted SQL call the `forkdb_*` functions**, and do not load the extension into a connection that runs untrusted SQL. (Opening a file that is not a store does not overwrite it: that was a bug until #51.)
- **No encryption, and no protection against tampering.** The checksums (CRC32C) detect accidental damage. They are not a defence against someone who can write the file: a record with altered content and a recomputed checksum is valid, and the recovery fuzzer builds exactly those.
- **The lock is advisory.** A store has one opener at a time (`flock` on Unix, the share mode on Windows), and it is held across a compaction (it was not on Linux and macOS until an independent review of 6f9e979 found that: see CHANGELOG.md). A process that does not use forkdb can still write the file, and a file system that cannot lock gives no protection. Compaction preserves the store's mode, so a store made private stays private; ownership is preserved only where the process may set it, and ACLs and extended attributes are not carried over.
- **A branch is an ordinary SQLite database.** Opening a store from an untrusted source puts SQLite in front of an untrusted database file, with SQLite's own caveats; forkdb does not add any.

## What has been checked, and what has not

- **Crafted and damaged store files, at the level of the log.** A deterministic fuzzer feeds `fk_open` damaged, truncated and deliberately malformed files under the address and undefined-behaviour sanitizers, 100000 inputs per CI run (`scripts/test-fuzz.sh`), and a mutant check shows it notices when one recovery check is removed. It found one bug (the small-file overwrite above). **It does not cover**: the file backends (`io_file.c`, `io_file_win.c`), the SQL functions, the VFS, the session driver on hostile content, or the durable-end marker (it cannot know what was once durable). Two of its mutants are known not to be caught; they are listed in the script.
- **Memory limits are constants, and the worst case is not measured.** A file can name up to 1024 branches (`FK_MAX_BRANCHES`), 2^28 pages per branch (`FK_MAX_PAGES`, 1 TiB at 4 KiB) and 512 bytes of metadata per branch (`FK_META_MAX`). The fuzzer's runs peaked at 21 MB; I have not measured what a file built to sit at those limits makes a process use.
- **Concurrency.** ThreadSanitizer runs the thread tests in CI, and they do not notice every missing lock: with the mutex removed from the VFS file's write wrapper (`fkf_write_m` in `vfs/fkvfs.c`) they still pass with no ThreadSanitizer warning (measured), so they do not show that path to be safe. The session driver is not tested under threads, and two accessors (`fkx_store_rc`, `fkx_sqlite_rc`) read their fields without the lock. See KNOWN-LIMITS.md.
- **Platforms.** The README table says what is tested where. A bug that exists only on a platform that is not tested will not have been seen.

## What to report

A crash, hang, out-of-bounds access or other memory error on a crafted store file, URI or SQL argument; silent data loss or corruption; anything that lets one branch's data appear in another; a path or file the library overwrites that it should refuse. Findings about the items under "has not been checked" are welcome and expected.

