/*
 * mark.c — path-existence marker for the minimal-IFS startup script (v8).
 *
 * WHY THIS EXISTS: the mkifs startup-script interpreter does NOT
 * understand shell syntax. The v7 image proved it: every multi-line
 * `if test ... / then display_msg ... / else display_msg ... / fi`
 * block failed with 3x `Unable to start "if"/"then"/"else"/"fi"`.
 * So markers must be plain commands with arguments — this helper
 * stats a path and prints exactly one line:
 *     MARKER <label> OK <path>
 * or
 *     MARKER <label> FAIL <path> errno=<n> (<strerror>)
 * Usage: mark <path> <label>
 *
 * BUILD (x86_64, SDP 8.0) — same recipe as mount_shm.c:
 *     export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
 *     export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
 *     export PATH=$QNX_HOST/usr/bin:$PATH
 *     qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -std=c11 -D_QNX_SOURCE \
 *         -o mark mark.c
 * Links libc only.
 */
#ifdef __QNX__

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    struct stat sb;

    if (argc != 3) {
        printf("MARKER usage: mark <path> <label>\n");
        fflush(stdout);
        return 2;
    }
    if (stat(argv[1], &sb) == 0) {
        printf("MARKER %s OK %s\n", argv[2], argv[1]);
    } else {
        printf("MARKER %s FAIL %s errno=%d (%s)\n",
               argv[2], argv[1], errno, strerror(errno));
    }
    fflush(stdout);
    return 0;
}

#else /* !__QNX__ */
#  error "mark.c is a QNX-only helper: build it with qcc. It must never run on the host."
#endif /* __QNX__ */
