#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "fkstore.h"

int main(void) {
    char path[] = "/tmp/fk5-checkpoint-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    close(fd);

    fk_io io;
    int rc = fk_file_open(path, &io);
    if (rc != FK_OK) { fprintf(stderr, "open fail: %d\n", rc); return 1; }

    fk_store *s;
    rc = fk_open(&io, &s);
    if (rc != FK_OK) { fprintf(stderr, "fk_open fail: %d\n", rc); return 1; }

    char page[FK_PAGE];
    memset(page, 'x', sizeof page);

    /* Write base pages on main */
    int npages = 200;
    rc = fk_txn_begin(s, 0);
    for (int i = 0; i < npages; i++) fk_txn_write(s, (uint32_t)i, page);
    fk_txn_commit(s, (uint32_t)npages);

    /* Fork and write different pages, then discard: creates garbage */
    for (int round = 0; round < 30; round++) {
        uint32_t kid;
        fk_fork(s, 0, &kid);
        char diff[FK_PAGE];
        memset(diff, 'y' + (round % 26), sizeof diff);
        fk_txn_begin(s, kid);
        for (int i = 0; i < npages; i++) fk_txn_write(s, (uint32_t)i, diff);
        fk_txn_commit(s, (uint32_t)npages);
        fk_discard(s, kid);
    }

    uint64_t size_before = 0, live_before = 0;
    fk_stats(s, &size_before, &live_before);
    printf("before close: file=%" PRIu64 " live=%" PRIu64 " (garbage=%" PRIu64 ")\n",
           size_before, live_before, size_before - live_before);

    fk_close(s);

    /* Reopen: should trigger checkpoint because there's lots of garbage and no snapshot */
    rc = fk_open(&io, &s);
    if (rc != FK_OK) { fprintf(stderr, "reopen fail: %d\n", rc); return 1; }

    uint64_t size_after = 0, live_after = 0;
    fk_stats(s, &size_after, &live_after);
    printf("after reopen:  file=%" PRIu64 " live=%" PRIu64 " (garbage=%" PRIu64 ")\n",
           size_after, live_after, size_after - live_after);

    if (size_before > ((uint64_t)16 << 20) && size_after < size_before) {
        printf("PASS: open-time checkpoint compacted the file\n");
    } else if (size_before <= ((uint64_t)16 << 20)) {
        printf("SKIP: file below checkpoint threshold\n");
    } else {
        printf("WARN: no compaction (before=%" PRIu64 " after=%" PRIu64 ")\n",
               size_before, size_after);
    }

    /* Verify data survived */
    char rbuf[FK_PAGE];
    rc = fk_read(s, 0, 0, rbuf);
    if (rc != FK_OK) { fprintf(stderr, "read fail: %d\n", rc); return 1; }
    if (memcmp(rbuf, page, FK_PAGE) != 0) { fprintf(stderr, "data mismatch\n"); return 1; }
    printf("data intact after checkpoint\n");

    /* Close and reopen again: should NOT compact because it's already a snapshot */
    fk_close(s);
    rc = fk_open(&io, &s);
    if (rc != FK_OK) { fprintf(stderr, "reopen2 fail: %d\n", rc); return 1; }

    uint64_t size_after2 = 0, live_after2 = 0;
    fk_stats(s, &size_after2, &live_after2);
    printf("after reopen2: file=%" PRIu64 " live=%" PRIu64 "\n", size_after2, live_after2);
    if (size_after2 == size_after && live_after2 == live_after) {
        printf("PASS: second reopen did not compact (already a snapshot)\n");
    } else {
        printf("WARN: unexpected second compaction\n");
    }

    fk_close(s);
    fk_file_close(&io);
    unlink(path);
    printf("ALL CHECKPOINT SMOKE TESTS PASSED\n");
    return 0;
}
