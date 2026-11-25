/*
 * mount_shm.c — mount the QNX shared-memory filesystem (v7 experiment).
 *
 * WHY THIS EXISTS: the stock image runs `mount -uo noexec /dev/shmem`
 * (quickstart ifs.build:62) before anything that needs shared memory.
 * The minimal image never did that, and the `mount` binary is NOT in
 * our assembled SDP tree — verified: no bin/mount in ~/qnx, and no
 * bin/mount in any of the 24 downloaded .qpkg files. So this helper
 * calls mount() directly (sys/mount.h, in-tree) with the parameters
 * the stock command implies: spec=/dev/shmem, dir=/dev/shmem, noexec.
 *
 * What is GROUNDED here (not guessed):
 *   - mount() prototype + _MOUNT_* flags: sys/mount.h (in-tree).
 *   - spec/dir "/dev/shmem" + "noexec": the stock command itself.
 *   - type NULL: NO shm type string exists anywhere — grep 'shmfs'
 *     across the SDP headers and procnto returns 0 hits, while
 *     procnto itself carries DEVSHMEM_PATH "/dev/shmem": procnto
 *     serves /dev/shmem, so no external filesystem type is involved.
 * What is NOT certain: the exact flag combination the mount utility
 * passes for its `-u`. This helper tries, in order, noexec alone,
 * noexec|IMPLIED, noexec|ENUMERATE — and PRINTS the rc/errno of
 * every attempt. It never claims success on its own exit code.
 *
 * VERDICT: the runtime fs probe (test_timer_timeout's "TT-TEST fs:
 * /dev/shm writable OK") plus slogger2/io-sock behaviour in the QEMU
 * log decide whether this worked. If all three attempts fail, the
 * real `mount` binary must come from the portal (File Lookup) and
 * this helper is dropped from the buildfile.
 *
 * RUN: foreground, FIRST in the startup script (before the test
 * program, so the fs probe tests the post-mount state):
 *     mount_shm
 *
 * BUILD (x86_64, SDP 8.0):
 *     export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
 *     export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
 *     export PATH=$QNX_HOST/usr/bin:$PATH
 *     qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -std=c11 -D_QNX_SOURCE \
 *         -o mount_shm mount_shm.c
 * Links libc only.
 */
#ifdef __QNX__

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int probe_shm_writable(void)
{
    /* KOREKSI (riset Gemini): path QNX yang benar adalah /dev/shmem,
     * BUKAN /dev/shm (itu path Linux). shm_open dan slogger2 memakai
     * /dev/shmem (strings: "/dev/shmem/slogger2",
     * "/dev/shmem/%016lx.bpf"). */
    FILE *f = fopen("/dev/shmem/mount-test", "w");
    if (!f) return -1;
    fputs("x", f);
    fclose(f);
    remove("/dev/shmem/mount-test");
    return 0;
}

static int try_mount(const char *label, int flags)
{
    errno = 0;
    int rc = mount("/dev/shmem", "/dev/shmem", flags, NULL, NULL, 0);
    int e = errno;
    printf("MOUNT-SHM %s: mount(\"/dev/shmem\", \"/dev/shmem\", "
           "flags=0x%x, NULL) rc=%d errno=%d (%s)\n",
           label, (unsigned)flags, rc, e, strerror(e));
    fflush(stdout);
    return rc;
}

int main(void)
{
    struct stat sb;

    printf("MOUNT-SHM START: mounting shared-memory fs "
           "(stock: mount -uo noexec /dev/shmem)\n");
    fflush(stdout);

    if (stat("/dev/shmem", &sb) == 0) {
        printf("MOUNT-SHM: /dev/shmem EXISTS mode=%o\n",
               (unsigned)(sb.st_mode & 0777));
    } else {
        printf("MOUNT-SHM: /dev/shmem MISSING errno=%d (%s) — mkdir it\n",
               errno, strerror(errno));
        if (mkdir("/dev/shmem", 0755) == -1) {
            printf("MOUNT-SHM: mkdir /dev/shmem FAIL errno=%d (%s)\n",
                   errno, strerror(errno));
        } else {
            printf("MOUNT-SHM: mkdir /dev/shmem OK\n");
        }
    }
    fflush(stdout);

    /* Attempt 1: plain noexec (the stock -o noexec). */
    if (try_mount("try1-noexec", _MOUNT_NOEXEC) == 0)
        goto mounted;
    /* Attempt 2: mountpoint implied by the client. */
    if (try_mount("try2-noexec-implied",
                  _MOUNT_NOEXEC | _MOUNT_IMPLIED) == 0)
        goto mounted;
    /* Attempt 3: auto-detect. */
    if (try_mount("try3-noexec-enumerate",
                  _MOUNT_NOEXEC | _MOUNT_ENUMERATE) == 0)
        goto mounted;

    printf("MOUNT-SHM FAIL: all mount() attempts failed — "
           "real `mount` binary needed (portal File Lookup)\n");
    fflush(stdout);
    return 1;

mounted:
    if (probe_shm_writable() == 0) {
        printf("MOUNT-SHM OK: /dev/shmem writable\n");
    } else {
        printf("MOUNT-SHM PARTIAL: mount rc=0 but /dev/shmem still NOT "
               "writable errno=%d (%s)\n", errno, strerror(errno));
    }
    fflush(stdout);
    return 0;
}

#else /* !__QNX__ */
#  error "mount_shm.c is a QNX-only helper: build it with qcc. It must never run on the host."
#endif /* __QNX__ */
