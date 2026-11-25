/*
 * test_timer_timeout.c — minimal TimerTimeout reproducer + discriminator.
 *
 * CONTEXT (QNX hackathon, 25 Nov 2026): in the QEMU v5 gateway test the
 * broker plugin's worker called exactly
 *
 *     TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE,
 *                  &pulse_event, &ntime_ns (100 ms), NULL)
 *
 * and got -1 / EINVAL ("Invalid argument") on its very first call
 * (qnx-boot-v5.log: "qnx_ipc: TimerTimeout: Invalid argument").
 * Static analysis could NOT explain it:
 *   - the call matches the SDP 8.0 prototype (sys/neutrino.h:908):
 *     (clockid_t, _Uint32t, const struct sigevent*, const _Uint64t*,
 *     _Uint64t*), nanoseconds;
 *   - disassembly of the shipped qnx_bridge.so shows the real args:
 *     clock=0 (CLOCK_REALTIME), flags=0x20 (_NTO_TIMEOUT_RECEIVE),
 *     event: notify=SIGEV_PULSE(4), coid=pulse_coid, value=0,
 *     code=0 (_PULSE_CODE_MINAVAIL), priority=-1 (INHERIT);
 *   - the official QNX 8.0 docs list only TWO EINVAL causes, both ruled
 *     out here: (1) invalid clock ID — CLOCK_REALTIME is valid;
 *     (2) TIMER_TOLERANCE misused — flags carry no TIMER_TOLERANCE bit.
 * The broker then died for an UNRELATED reason (listener socket failed:
 * io-sock was down, so mosquitto exited silently before its main loop),
 * which is why the worker only ever logged ONE error pair.
 *
 * V6 RESULT (qnx-boot-v6.log): exact FAIL EINVAL, mono FAIL EINVAL,
 * unblock TimerTimeout OK — then the program HUNG ~350 s (no
 * plain-coid / msg-arrives / DONE lines). Two v6 flaws explain the
 * blind spot, both fixed in this v7 version:
 *   (a) In v6, tt_one() printed + fflush'ed BETWEEN TimerTimeout and
 *       MsgReceive. The intervening write() kernel call may have
 *       consumed the armed timeout, so the unblock receive blocked
 *       forever. Here MsgReceive is called IMMEDIATELY after
 *       TimerTimeout (no call in between) — exactly like
 *       qnx_ipc_receive() does — so this run tests the true adjacent
 *       form.
 *   (b) v6 prints were stdio-buffered (no fflush after most lines),
 *       so a hang left no trace of where it stopped. Here stdout is
 *       unbuffered (setvbuf _IONBF) AND every printf is followed by
 *       fflush(stdout).
 *   (c) NEW: a 30 s watchdog thread aborts the program with a
 *       TT-TEST GUARD line, so no test can hang the boot.
 *   (d) NEW test 6 "pulse-wake": plain blocking MsgReceive woken by
 *       a MsgSendPulse at +50 ms, no TimerTimeout at all. This is THE
 *       mechanism the v7 worker ticker relies on — if this test fails,
 *       the ticker design is dead too.
 *
 * RUN: add to the v7 minimal image and run EARLY in the startup script,
 * in the background, so a hang can never stall the boot:
 *     test_timer_timeout &
 *     waitfor /dev/delay 20
 * (each test's MsgReceive is bounded: by the 100 ms timeout, by
 * ETIMEDOUT, by a sender thread, or by the 30 s watchdog.)
 *
 * BUILD (x86_64, SDP 8.0):
 *     export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
 *     export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
 *     export PATH=$QNX_HOST/usr/bin:$PATH
 *     qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -std=c11 -D_QNX_SOURCE \
 *         -o test_timer_timeout test_timer_timeout.c
 * Links libc only (pthread symbols resolve from libc on QNX).
 */
#ifdef __QNX__

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/stat.h>

#define TT_TIMEOUT_MS   100
#define TT_PULSE_CODE   0   /* == _PULSE_CODE_MINAVAIL, == QNX_IPC_TIMEOUT_CODE */
#define TT_SENDER_CODE  0x42
#define TT_WAKE_CODE    0x43 /* pulse code for the v7 pulse-wake test */
#define TT_GUARD_SECS   30  /* watchdog: abort the whole program after this */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Watchdog (v7): if any test blocks forever, print a guard line and
 * abort the process so the boot can never hang on this program. */
static void *tt_watchdog_main(void *arg)
{
    (void)arg;
    sleep(TT_GUARD_SECS);
    printf("TT-TEST GUARD: TIMEOUT after %ds — a test hung, aborting\n",
           TT_GUARD_SECS);
    fflush(stdout);
    _exit(2);
}

/* One test channel: name_attach + self-connection for pulses. */
struct tt_chan {
    name_attach_t *attach;
    int chid;
    int pulse_coid;
};

static int tt_create(const char *name, int side_channel, struct tt_chan *c)
{
    memset(c, 0, sizeof(*c));
    c->chid = -1;
    c->pulse_coid = -1;
    c->attach = name_attach(NULL, name, 0);
    if (!c->attach) {
        printf("TT-TEST %s: name_attach FAIL errno=%d (%s)\n",
               name, errno, strerror(errno));
        fflush(stdout);
        return -1;
    }
    c->chid = c->attach->chid;
    c->pulse_coid = ConnectAttach(ND_LOCAL_NODE, 0, c->chid,
                                  side_channel ? _NTO_SIDE_CHANNEL : 0, 0);
    if (c->pulse_coid == -1) {
        printf("TT-TEST %s: ConnectAttach FAIL errno=%d (%s)\n",
               name, errno, strerror(errno));
        fflush(stdout);
        name_detach(c->attach, 0);
        return -1;
    }
    return 0;
}

static void tt_destroy(struct tt_chan *c)
{
    if (c->pulse_coid != -1) ConnectDetach(c->pulse_coid);
    if (c->attach) name_detach(c->attach, 0);
}

/*
 * Run one TimerTimeout + MsgReceive attempt. Returns 0 if the sequence
 * completed (whatever the outcome), so the caller always moves on.
 * Prints one TT-TEST line with the verdict.
 *
 * v7: MsgReceive is called IMMEDIATELY after TimerTimeout — no
 * printf/fflush (no kernel call) in between — exactly like
 * qnx_ipc_receive(). The v6 hang may have been the intervening
 * write() consuming the armed timeout.
 */
static void tt_one(const char *tag, clockid_t clock, int use_pulse_event,
                   int side_channel)
{
    struct tt_chan ch;
    struct sigevent event;
    _Uint64t ntime_ns = (uint64_t)TT_TIMEOUT_MS * 1000000ULL;
    uint8_t buf[256];
    struct _msg_info info;
    int rcvid;
    long t0, dt;
    int trc, terr;

    if (tt_create(tag, side_channel, &ch) != 0)
        return;

    if (use_pulse_event) {
        SIGEV_PULSE_INIT(&event, ch.pulse_coid,
                         SIGEV_PULSE_PRIO_INHERIT, TT_PULSE_CODE, 0);
    }
    errno = 0;
    t0 = now_ms();
    trc = TimerTimeout(clock, _NTO_TIMEOUT_RECEIVE,
                       use_pulse_event ? &event : NULL,
                       &ntime_ns, NULL);
    terr = errno;
    if (trc == -1) {
        printf("TT-TEST %s: TimerTimeout FAIL rc=-1 errno=%d (%s) "
               "clock=%d flags=0x%x pulse_event=%d side_channel=%d "
               "ntime_ns=%llu\n",
               tag, terr, strerror(terr), (int)clock,
               (unsigned)_NTO_TIMEOUT_RECEIVE, use_pulse_event,
               side_channel, (unsigned long long)ntime_ns);
        fflush(stdout);
        printf("TT-TEST %s: SKIP MsgReceive (no timeout armed)\n", tag);
        fflush(stdout);
        tt_destroy(&ch);
        return;
    }
    /* Adjacent form: no call of any kind between TimerTimeout and
     * MsgReceive. The result lines are printed only afterwards. */
    rcvid = MsgReceive(ch.chid, buf, (int)sizeof(buf), &info);
    dt = now_ms() - t0;
    printf("TT-TEST %s: TimerTimeout OK (clock=%d pulse_event=%d "
           "side_channel=%d)\n",
           tag, (int)clock, use_pulse_event, side_channel);
    fflush(stdout);
    if (rcvid == -1) {
        printf("TT-TEST %s: MsgReceive rc=-1 errno=%d (%s) after %ld ms\n",
               tag, errno, strerror(errno), dt);
    } else if (rcvid == 0) {
        struct _pulse *p = (struct _pulse *)buf;
        printf("TT-TEST %s: MsgReceive PULSE code=%d value=%d after %ld ms\n",
               tag, (int)p->code, (int)p->value.sival_int, dt);
    } else {
        printf("TT-TEST %s: MsgReceive MESSAGE rcvid=%d len=%d after %ld ms\n",
               tag, rcvid, (int)info.msglen, dt);
        MsgReply(rcvid, 0, NULL, 0);
    }
    fflush(stdout);
    tt_destroy(&ch);
}

/* Sender thread for the message-arrival test: real MsgSend at +50 ms. */
static int g_sender_coid = -1;
static void *tt_sender_main(void *arg)
{
    (void)arg;
    usleep(50 * 1000);
    char reply[64];
    int rc = MsgSend(g_sender_coid, "hello-tt", 8, reply, sizeof(reply));
    printf("TT-TEST msg-arrives: sender MsgSend rc=%d errno=%d (%s)\n",
           rc, rc == -1 ? errno : 0, rc == -1 ? strerror(errno) : "ok");
    fflush(stdout);
    return NULL;
}

static void tt_msg_arrives(void)
{
    struct tt_chan ch;
    struct sigevent event;
    _Uint64t ntime_ns = (uint64_t)TT_TIMEOUT_MS * 1000000ULL;
    uint8_t buf[256];
    struct _msg_info info;
    pthread_t sender;
    int rcvid;
    long t0, dt;

    if (tt_create("tt_msg", 1, &ch) != 0)
        return;
    g_sender_coid = ConnectAttach(ND_LOCAL_NODE, 0, ch.chid, 0, 0);
    if (g_sender_coid == -1) {
        printf("TT-TEST msg-arrives: sender ConnectAttach FAIL errno=%d (%s)\n",
               errno, strerror(errno));
        fflush(stdout);
        tt_destroy(&ch);
        return;
    }
    /* Detached: if the receiver skips MsgReceive, the sender may block
     * in MsgSend; process exit cleans it up. */
    pthread_create(&sender, NULL, tt_sender_main, NULL);
    pthread_detach(sender);

    SIGEV_PULSE_INIT(&event, ch.pulse_coid,
                     SIGEV_PULSE_PRIO_INHERIT, TT_PULSE_CODE, 0);
    errno = 0;
    t0 = now_ms();
    int trc = TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE,
                           &event, &ntime_ns, NULL);
    if (trc == -1) {
        printf("TT-TEST msg-arrives: TimerTimeout FAIL rc=-1 errno=%d (%s) "
               "-> SKIP MsgReceive\n", errno, strerror(errno));
        fflush(stdout);
        ConnectDetach(g_sender_coid);
        tt_destroy(&ch);
        return;
    }
    rcvid = MsgReceive(ch.chid, buf, (int)sizeof(buf), &info);
    dt = now_ms() - t0;
    if (rcvid == -1) {
        printf("TT-TEST msg-arrives: MsgReceive rc=-1 errno=%d (%s) "
               "after %ld ms\n", errno, strerror(errno), dt);
    } else if (rcvid == 0) {
        struct _pulse *p = (struct _pulse *)buf;
        printf("TT-TEST msg-arrives: MsgReceive PULSE code=%d after %ld ms "
               "(expected MESSAGE ~50 ms)\n", (int)p->code, dt);
    } else {
        printf("TT-TEST msg-arrives: MsgReceive MESSAGE rcvid=%d len=%d "
               "after %ld ms msg='%.8s'\n",
               rcvid, (int)info.msglen, dt, buf);
        MsgReply(rcvid, 0, NULL, 0);
    }
    fflush(stdout);
    ConnectDetach(g_sender_coid);
    tt_destroy(&ch);
}

/* Test 6 (v7): does a MsgSendPulse wake a plain blocking MsgReceive?
 * No TimerTimeout is armed at all: the main thread blocks in
 * MsgReceive while a sender thread pulses at +50 ms. This is THE
 * mechanism the v7 worker ticker relies on (100 ms pulses + drain on
 * pulse). Expect: rcvid==0, pulse code TT_WAKE_CODE, after ~50 ms.
 * If the pulse never wakes the receiver, the 30 s watchdog aborts the
 * program with a TT-TEST GUARD line. */
static int g_pulse_coid = -1;
static void *tt_pulse_sender_main(void *arg)
{
    (void)arg;
    usleep(50 * 1000);
    int rc = MsgSendPulse(g_pulse_coid, SIGEV_PULSE_PRIO_INHERIT,
                          TT_WAKE_CODE, 0);
    printf("TT-TEST pulse-wake: sender MsgSendPulse rc=%d errno=%d (%s)\n",
           rc, rc == -1 ? errno : 0, rc == -1 ? strerror(errno) : "ok");
    fflush(stdout);
    return NULL;
}

static void tt_pulse_wake(void)
{
    struct tt_chan ch;
    uint8_t buf[256];
    struct _msg_info info;
    pthread_t sender;
    int rcvid;
    long t0, dt;

    if (tt_create("tt_pulse", 1, &ch) != 0)
        return;
    g_pulse_coid = ch.pulse_coid;
    pthread_create(&sender, NULL, tt_pulse_sender_main, NULL);
    pthread_detach(sender);

    /* No TimerTimeout: plain blocking receive. The pulse must wake it. */
    t0 = now_ms();
    rcvid = MsgReceive(ch.chid, buf, (int)sizeof(buf), &info);
    dt = now_ms() - t0;
    if (rcvid == -1) {
        printf("TT-TEST pulse-wake: MsgReceive rc=-1 errno=%d (%s) "
               "after %ld ms\n", errno, strerror(errno), dt);
    } else if (rcvid == 0) {
        struct _pulse *p = (struct _pulse *)buf;
        printf("TT-TEST pulse-wake: MsgReceive PULSE code=0x%x "
               "(expect 0x%x) after %ld ms\n",
               (int)p->code, TT_WAKE_CODE, dt);
    } else {
        printf("TT-TEST pulse-wake: MsgReceive MESSAGE rcvid=%d "
               "(unexpected) after %ld ms\n", rcvid, dt);
        MsgReply(rcvid, 0, NULL, 0);
    }
    fflush(stdout);
    tt_destroy(&ch);
}

/* Filesystem probe for the v6/v7 liveness plan: the broker's pid_file
 * needs a writable directory. /dev/shm is the only RAM-backed candidate
 * in the minimal image (/proc/boot is the read-only imagefs). v7 also
 * stats /dev/shmem and /dev/shm so the log shows whether the shm
 * filesystem is mounted at all (mount_shm runs before this program). */
static void tt_fs_check(void)
{
    struct stat sb;
    FILE *f;

    if (stat("/dev/shmem", &sb) == 0) {
        printf("TT-TEST fs: /dev/shmem EXISTS mode=%o\n",
               (unsigned)(sb.st_mode & 0777));
    } else {
        printf("TT-TEST fs: /dev/shmem MISSING errno=%d (%s)\n",
               errno, strerror(errno));
    }
    fflush(stdout);
    if (stat("/dev/shm", &sb) == 0) {
        printf("TT-TEST fs: /dev/shm EXISTS mode=%o\n",
               (unsigned)(sb.st_mode & 0777));
    } else {
        printf("TT-TEST fs: /dev/shm MISSING errno=%d (%s)\n",
               errno, strerror(errno));
    }
    fflush(stdout);

    f = fopen("/dev/shm/tt-test", "w");
    if (!f) {
        printf("TT-TEST fs: /dev/shm NOT writable errno=%d (%s)\n",
               errno, strerror(errno));
    } else {
        fputs("x", f);
        fclose(f);
        remove("/dev/shm/tt-test");
        printf("TT-TEST fs: /dev/shm writable OK (pidfile usable)\n");
    }
    fflush(stdout);
    f = fopen("/proc/boot/tt-test", "w");
    if (!f) {
        printf("TT-TEST fs: /proc/boot NOT writable errno=%d (%s) "
               "(expected: read-only imagefs)\n", errno, strerror(errno));
    } else {
        fclose(f);
        remove("/proc/boot/tt-test");
        printf("TT-TEST fs: /proc/boot writable (unexpected!)\n");
    }
    fflush(stdout);
}

int main(void)
{
    pthread_t watchdog;

    /* Unbuffered stdout: every line reaches the serial console at
     * once, even if the program is killed mid-test. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Watchdog first: no test may hang the boot past TT_GUARD_SECS. */
    pthread_create(&watchdog, NULL, tt_watchdog_main, NULL);
    pthread_detach(watchdog);

    printf("TT-TEST START: exact reproduction of worker call + variants\n");
    printf("TT-TEST consts: CLOCK_REALTIME=%d CLOCK_MONOTONIC=%d "
           "_NTO_TIMEOUT_RECEIVE=0x%x SIGEV_PULSE=%d timeout_ms=%d\n",
           (int)CLOCK_REALTIME, (int)CLOCK_MONOTONIC,
           (unsigned)_NTO_TIMEOUT_RECEIVE, (int)SIGEV_PULSE,
           TT_TIMEOUT_MS);
    fflush(stdout);

    /* Filesystem probe first: decides whether the v6/v7 broker pidfile
     * plan (/dev/shm/mosquitto.pid) is viable, and whether the shm
     * filesystem got mounted (v7: mount_shm runs before this program). */
    tt_fs_check();

    /* 1. Exact reproduction of qnx_ipc_qnx.c:214 (the v5 failing call). */
    tt_one("exact", CLOCK_REALTIME, 1, 1);
    /* 2. Same, but CLOCK_MONOTONIC (docs' other valid clock). */
    tt_one("mono", CLOCK_MONOTONIC, 1, 1);
    /* 3. Docs' recommended form: notify=NULL -> SIGEV_UNBLOCK assumed,
     * MsgReceive must fail ETIMEDOUT after ~100 ms (adjacent form). */
    tt_one("unblock", CLOCK_REALTIME, 0, 1);
    /* 4. Same pulse call, but plain self-connection (no _NTO_SIDE_CHANNEL). */
    tt_one("plain-coid", CLOCK_REALTIME, 1, 0);
    /* 5. Real message arrives at +50 ms, before the 100 ms timeout. */
    tt_msg_arrives();
    /* 6. (v7) Pulse wakes a plain blocking MsgReceive — the ticker
     * mechanism the v7 worker relies on. */
    tt_pulse_wake();

    printf("TT-TEST DONE\n");
    fflush(stdout);
    return 0;
}

#else /* !__QNX__ */
#  error "test_timer_timeout.c is a QNX-only diagnostic: build it with qcc (see header comment). It must never run on the host."
#endif /* __QNX__ */
