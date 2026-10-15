# Stability

Status: **unstable.** forkdb has not been released: there is no tag and no release, and the version string that `forkdb_info` reports (`0.1.0`) is a placeholder for a first release. Nothing in this repository is a promise about what stays the same, and **no version number is promised for when any of it becomes stable.**

This note says what that means for each surface, what has already changed, and what is true today without being promised.

## The surfaces

| Surface | What may change | What has already changed |
|---|---|---|
| **On-disk format** (a store file) | A new format version can appear at any time, and a later build may refuse a file this build wrote. | Format versions 1 to 4 were introduced between 9 and 13 October 2026: 2 added snapshots (compaction), 3 let a fork reuse the ids of discarded branches, 4 added branch metadata, reset and state ids. The current build writes version 4. |
| **SQL functions** (`forkdb_init`, `forkdb_open_db`, `forkdb_fork`, `forkdb_snapshot`, `forkdb_discard`, `forkdb_reset`, `forkdb_diff`, `forkdb_promote`, `forkdb_undo`, `forkdb_compact`, `forkdb_close`, `forkdb_vfs_name`, `forkdb_sync_count`, and the `forkdb_info` table) | Names, arguments, results, option words and error messages. | #44 added option words to `forkdb_init` (`'nosync'`); #47 changed the string `forkdb_open_db` returns (`file:/forkdb/bN?vfs=forkdb`, from `file:bN?vfs=forkdb`); #46 made branch ids fit 53 bits; #54 added `forkdb_sync_count`. |
| **The CLI** (`forkdb`, its commands, flags and `--json` output) | Anything. | #40 changed what it does with a store it cannot open (an error message, where it used to segfault). |
| **The C interfaces** (`store/fkstore.h`, `fkx/fkx.h`, `vfs/fkvfs.h`) | Anything: names, signatures, error codes. | #41 added `FK_ELOCKED` and `FKX_ELOCKED`; #44 added `fk_file_open_ex` and an options field; #54 added `fk_file_sync_count`; #42 added `fkvfs_lock` and `fkvfs_unlock`. |
| **Opaque values**: the URI `forkdb_open_db` returns, branch uids, state ids | Their shape and size. Treat them as opaque strings and numbers: do not parse them, and do not store a URI. | See the rows above. |

## Older files

What is true today, not a commitment:

- The C library (`fk_open`) still reads files of versions 1 to 3. Calls that need version 4 (reset, metadata, state ids) return `FK_EVERSION` on such a file until `fk_compact` rewrites it in the current format. This is tested with a version 2 and a version 3 file in `test/data`.
- **The CLI and the extension do not open a file older than version 4**: they report that it is older than format version 4. They are not offered a way to upgrade it, and nothing needs one: `fkx`, the CLI and the extension were written after version 4 (the commits are in that order, on 13 October), so no store they made has ever been older. Only a file made by the raw store library can be. (`fkx_options.upgrade` exists in the library; neither the CLI nor the extension sets it, and a store made by the raw library is refused by `fkx` in any case as not made by it. `test/fkx_test.c` checks both.)
- A file written by this build may be unreadable by a later one. There is no migration tool, and none is promised.

## What we do instead of promising

Every change is listed in [CHANGELOG.md](../CHANGELOG.md) with its pull request, including changes to the format, the SQL functions, the CLI and the C interfaces. Known limitations are in [KNOWN-LIMITS.md](KNOWN-LIMITS.md); what has and has not been tested is in the README.

