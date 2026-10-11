# Test data

`v2-compacted.fk` is a store in **format version 2** (before branch ids could be reused), with a snapshot from a compaction, written by the
store as it was at commit `7008c1c`. [file_test.c](../file_test.c) opens a copy of it with the current code, to show that files written by the
previous release still open, read back correctly, and keep working. It must never be regenerated with the current code: the point is that
the bytes come from the old one.

To regenerate it (only needed if the test is changed):

```sh
d=$(mktemp -d)
for f in fkstore.c fkstore.h io_file.c; do git show 7008c1c:store/$f > "$d/$f"; done
cp test/data/gen_v2.c "$d/"
cc -O2 -std=gnu11 -I"$d" "$d/gen_v2.c" "$d/fkstore.c" "$d/io_file.c" -o "$d/gen_v2" && "$d/gen_v2" test/data/v2-compacted.fk
```

The history it contains is described at the top of [gen_v2.c](gen_v2.c).
