// SPDX-License-Identifier: MIT
// Adapted from d2r-mobile dm/host/hle_thread.c, revision 685d3ddef8bf75768fd4c51228566f2e8d2a5182.
// Copyright (c) 2026 jhaizhou. See third_party/d2r-mobile-LICENSE.
// Android adaptation: remove Mach diagnostics/QoS and D2R profiling; enter through guest_entry.S.
#include "thread_services.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <string.h>
#define LOG ot2_host_log
#define die ot2_host_die
static uint64_t now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static uint64_t guest_ns(void) { return now_ns(); }
static int dm_detclock_enabled(void) { return 0; }
#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_TRANSFER };
typedef struct transfer { void* source;uint64_t size;int fd; } transfer_t;
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    int wtype; int32_t wval;   // svcWaitForAddress 的比较方式与比较值：0 小于则等 / 1 减一再等 / 2 相等则等 | Compare mode and value of svcWaitForAddress: 0 wait if less, 1 decrement then wait, 2 wait if equal
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    // 死锁现场最有用的东西：每个线程停住之前最后几个 SVC。等待本身只说明它停在哪，
    // 前一条才说明它有没有先发过唤醒（0x1B 放锁 / 0x1D 发键 / 0x35 发地址）。
    // The most useful thing at a deadlock: the last few SVCs each thread issued before
    // stopping. The wait itself only says where it stopped; the entry before it says
    // whether it had just signalled (0x1B unlock, 0x1D signal key, 0x35 signal address).
    uint16_t hist[8]; unsigned hist_pos;
    // 当前/最近一次等待的超时参数与两个计数器。三者合起来能一次区分「宿主没把带
    // 超时的等待当超时」和「超时确实到了、线程却不前进」：前者 wns 有值但 nto 不
    // 动，后者 nto 一直涨而线程仍在同一个等待里打转。
    // The timeout argument of the current/last wait plus two counters. Together they
    // separate "the host never treated a timed wait as timed" (wns set, nto frozen)
    // from "timeouts do fire but the thread makes no progress" (nto keeps climbing
    // while the thread keeps re-entering the same wait).
    int64_t wns; uint64_t nto, nwake;
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    uint32_t mport;         // 宿主线程端口（取 CPU 时间） | Host thread port (to get CPU time)
    uint32_t qos;
} thr_t;
static thr_t *g_all;
// 全局 TIMEOUT_FIRED 额度：有线程每 5 ms 醒一次且线程反复重建，每线程上限形同虚设 | Global TIMEOUT_FIRED quota
static int timeout_log_budget = 200;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static pthread_once_t g_key_once=PTHREAD_ONCE_INIT;
static void create_thread_key(void){if(pthread_key_create(&g_keythr,NULL))die("thread key creation");}
static void init_condition(pthread_cond_t* cv){pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);pthread_cond_init(cv,&attr);pthread_condattr_destroy(&attr);}
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) {
        g_h[i].type = type; g_h[i].obj = obj;
        // Every kernel object the title can wait on passes through here. When a
        // wait later uses handle 0, this list says whether the object was ever
        // created and under which type.
        {
            static unsigned long long created;
            const char *names[] = {"free", "thread", "eventReadable", "eventWritable", "transferMemory"};
            LOG("H_NEW #%llu handle=%#x type=%s", ++created, 0x1000u + (unsigned)i,
                (type >= 0 && type <= 4) ? names[type] : "?");
        }
        return 0x1000 + i;
    }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq; t->wns = ns;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; t->nto++; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_MONOTONIC, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; t->nto++; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; t->nto++; return R_TIMEOUT; }
    }
    t->nwake++;
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    crash_thread_init();
    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}
void svc_thread_exit_on_host(void) {
    thr_t *t=cur();
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_once(&g_key_once,create_thread_key);
    if(pthread_getspecific(g_keythr))return cur()->handle;
    thr_t *t = calloc(1, sizeof *t);
    init_condition(&t->cv);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self();
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    return t->handle;
}

static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
int ot2_signal_event(uint32_t handle) {
    pthread_mutex_lock(&G);
    event_t* event=h_peek(handle,O_EVENT_W);
    if(event){event->signaled=1;wake_sync_waiters();}
    pthread_mutex_unlock(&G);
    return event!=NULL;
}
// Dumps the wait state of every guest thread.
//
// A stalled title shows all its threads in futex/ppoll, which says nothing about
// the shape of the deadlock. Grouping the threads by the lock address they are
// queued on turns that into an answer: two threads on the same address with the
// condition key already set means one of them should have woken the other and
// did not, and a thread waiting on an address nobody holds is waiting on work
// that was never scheduled.
// Per-address wait/signal tallies for the two address-based primitives.
//
// When a title stops progressing, every waking path looks the same in a stack
// dump: someone is parked on an address or an event. What separates "nobody
// signals this" from "the signal raced the wait" is the pair of counts per
// address, and neither is visible from a single snapshot. An address with waits
// and no signals at all is a producer that never ran; an address with both is a
// race in the wake path, which is a host bug rather than a missing subsystem.
typedef struct { uint64_t addr; unsigned waits, signals; } addr_stat;
static addr_stat addr_stats[192];
static int addr_stat_count;
static addr_stat *addr_stat_for(uint64_t addr) {
    for (int i = 0; i < addr_stat_count; i++) if (addr_stats[i].addr == addr) return &addr_stats[i];
    if (addr_stat_count >= (int)(sizeof addr_stats / sizeof *addr_stats)) return NULL;
    addr_stat *s = &addr_stats[addr_stat_count++];
    s->addr = addr; s->waits = s->signals = 0;
    return s;
}
void svc_thread_note_address(uint64_t addr, int is_signal) {
    // Called under G from the syscall handlers, so the table needs no lock.
    addr_stat *s = addr_stat_for(addr);
    if (!s) return;
    if (is_signal) s->signals++; else s->waits++;
}
void svc_thread_dump_addresses(void) {
    pthread_mutex_lock(&G);
    extern const char* ot2_host_module_of(uint64_t, uintptr_t*);
    for (int i = 0; i < addr_stat_count; i++) {
        uintptr_t offset = 0;
        const char *where = ot2_host_module_of(addr_stats[i].addr, &offset);
        LOG("ADDRSTAT %s+0x%zx waits=%u signals=%u", where ? where : "?",
            offset, addr_stats[i].waits, addr_stats[i].signals);
    }
    pthread_mutex_unlock(&G);
}
// 读客体地址但不让宿主自己崩：等待对象所在的页有可能已被客体释放，
// 直接解引用会把看门狗线程打死，诊断工具就成了新的故障源。
// Read a guest address without crashing the host: the page holding the wait object
// may already have been released by the guest, and dereferencing it directly kills
// the watchdog thread, turning the diagnostic into a new fault source.
static int safe_ld32(uint64_t address, uint32_t *out) {
    static int mem = -1;
    if (mem < 0) mem = open("/proc/self/mem", O_RDONLY);
    if (mem < 0 || !address || (address & 3)) return 0;
    return pread(mem, out, sizeof *out, (off_t)address) == (ssize_t)sizeof *out;
}

// ---------- 时序环形缓冲 ----------
// 等待链的转储是**快照**，它说得出"谁在等"，说不出"谁最后停的"。而停机顺序才是
// 死锁的形状：最后一条唤醒之后还有谁睡下、睡在谁身上、那个地址后来有没有再被碰过。
// 有了顺序，"丢了一次唤醒"和"根本没有生产者"就能区分开——前者表现为信号排在等待
// 之前，后者表现为某地址只被等过、事后连一次信号都没有。
// The wait dump is a snapshot: it says who is waiting, not who stopped last. The
// order of stopping is the shape of the deadlock. With it, "a wakeup was lost" and
// "there never was a producer" stop looking the same: a lost wakeup shows the signal
// ordered before the wait, a missing producer shows an address that was only ever
// waited on and never touched again.
enum { EV_COND = 1, EV_KEYSIGNAL, EV_ADDR, EV_ADDRSIGNAL, EV_LOCK };
typedef struct { uint64_t t_ns, a0, a1, lr; uint32_t who; uint16_t what; int32_t extra; } svc_ev;
#define SVC_EV_N 256
static svc_ev g_evs[SVC_EV_N];
static unsigned g_ev_pos;
static uint64_t g_ev_t0;
static unsigned g_keysignal_empty, g_addrsignal_empty;   // 空发：唤醒时没找到任何等待者 | Empty signals: wakeups that found no waiter
static unsigned g_addrsignal_touched;                     // 带类型/值的信号（真去改了内存） | Typed signals that actually touched memory
// 每个条件键的"被等 / 被发"次数与两侧的调用点。死锁时最想知道的是：主线程最后睡的
// 那个 key，到底有没有人发过信号。发过 → 是时序问题；一次都没发过 → 是生产者根本没跑，
// 而且调用点会直接指出该唤它的是哪个组件。
// Per condition key: how often it was waited on / signalled, plus the call sites on both
// sides. The question at a stall is whether the key the main thread finally parked on was
// ever signalled at all. Signalled -> a timing problem; never signalled -> the producer
// never ran, and the call site names which component owed the wake.
typedef struct { uint64_t key, last_wait_lr, last_signal_lr, last_wait_ms, last_signal_ms; unsigned waits, signals; } key_stat;
#define KEY_STAT_N 96
static key_stat g_keys[KEY_STAT_N];
static unsigned g_key_count;
static key_stat *key_stat_for(uint64_t key) {
    for (unsigned i = 0; i < g_key_count; ++i) if (g_keys[i].key == key) return &g_keys[i];
    if (g_key_count >= KEY_STAT_N) return NULL;
    key_stat *s = &g_keys[g_key_count++];
    s->key = key; s->waits = s->signals = 0;
    s->last_wait_lr = s->last_signal_lr = 0;
    s->last_wait_ms = s->last_signal_ms = 0;
    return s;
}
// 调用方持 G | Called holding G
static void ev_note(uint16_t what, uint32_t who, uint64_t a0, uint64_t a1, int32_t extra, uint64_t lr) {
    uint64_t now = now_ns();
    if (!g_ev_t0) g_ev_t0 = now;
    if (what == EV_COND || what == EV_KEYSIGNAL) {
        key_stat *s = key_stat_for(what == EV_COND ? a1 : a0);
        if (s) {
            const uint64_t ms = (now - g_ev_t0) / 1000000;
            if (what == EV_COND) { s->waits++; s->last_wait_lr = lr; s->last_wait_ms = ms; }
            else                 { s->signals++; s->last_signal_lr = lr; s->last_signal_ms = ms; }
        }
    }
    svc_ev *e = &g_evs[g_ev_pos++ % SVC_EV_N];
    e->t_ns = now; e->who = who; e->what = what; e->a0 = a0; e->a1 = a1; e->extra = extra; e->lr = lr;
}
void svc_thread_dump_timeline(void) {
    static int dumped;
    if (dumped) return;
    dumped = 1;
    pthread_mutex_lock(&G);
    extern const char* ot2_host_module_of(uint64_t, uintptr_t*);
    const unsigned total = g_ev_pos < SVC_EV_N ? g_ev_pos : SVC_EV_N;
    LOG("TIMELINE entries=%u total=%u empty_keysig=%u empty_addrsig=%u typed_addrsig=%u",
        total, g_ev_pos, g_keysignal_empty, g_addrsignal_empty, g_addrsignal_touched);
    const char *what_name[] = {"?", "cond-wait", "key-signal", "addr-wait", "addr-signal", "lock-wait"};
    for (unsigned i = 0; i < total; ++i) {
        const svc_ev *e = &g_evs[(g_ev_pos - total + i) % SVC_EV_N];
        uintptr_t off = 0;
        const char *mod = ot2_host_module_of(e->lr, &off);
        const char *what = e->what <= EV_LOCK ? what_name[e->what] : "?";
        LOG("EV +%7.3fms h=%#x %-11s a0=%#llx a1=%#llx x=%d at=%s+0x%zx",
            (double)(e->t_ns - g_ev_t0) / 1e6, e->who, what,
            (unsigned long long)e->a0, (unsigned long long)e->a1, e->extra,
            mod ? mod : "?", off);
    }
    // 条件键全表：waits>0 且 signals==0 的键，就是没有人会来唤醒的那个。
    // The whole key table: a key with waits>0 and signals==0 is the one nobody will ever
    // wake, and the LR next to it names the code that waits on it.
    for (unsigned i = 0; i < g_key_count; ++i) {
        const key_stat *s = &g_keys[i];
        uintptr_t wo = 0, so = 0;
        const char *wm = ot2_host_module_of(s->last_wait_lr, &wo);
        const char *sm = ot2_host_module_of(s->last_signal_lr, &so);
        LOG("KEY %#llx waits=%u signals=%u last_wait=%llums@%s+0x%zx last_sig=%llums@%s+0x%zx",
            (unsigned long long)s->key, s->waits, s->signals,
            (unsigned long long)s->last_wait_ms, wm ? wm : "?", wo,
            (unsigned long long)s->last_signal_ms, sm ? sm : "?", so);
    }
    pthread_mutex_unlock(&G);
}

void svc_thread_dump_waiters(void) {
    pthread_mutex_lock(&G);
    extern const char* ot2_host_module_of(uint64_t, uintptr_t*);
    unsigned counts[5] = {0, 0, 0, 0, 0};
    unsigned total = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        uintptr_t offset = 0;
        const char *lr = ot2_host_module_of(t->last_lr, &offset);
        const char *kind = t->wkind == 1 ? "lock" : t->wkind == 2 ? "cond"
                         : t->wkind == 3 ? "address" : t->wkind == 4 ? "sync" : "none";
        ++total;
        if (t->wkind >= 0 && t->wkind <= 4) counts[t->wkind]++;
        // 等对象在客体内存里的当前值。丢掉唤醒的现场长这样：条件键已经是 1（发送方
        // 用户态先置键再发 svc 0x1D）、锁字是 0（已交出去）、可线程还挂在等待链上。
        // 反过来，锁字带 WAITERS_BIT 却没人持有，说明交锁那一步丢了。
        // The waiting object's current value in guest memory. A lost wakeup looks
        // like: condition key already 1 (the sender sets the key in user mode
        // before issuing svc 0x1D), lock word 0 (already handed over), yet the
        // thread is still linked in the wait chain. The other shape is a lock word
        // carrying WAITERS_BIT with no owner, which means the handoff was dropped.
        uint32_t lock_value = 0, key_value = 0;
        int lock_ok = (t->wkind == 1 || t->wkind == 2 || t->wkind == 3) ? safe_ld32(t->waddr, &lock_value) : 0;
        int key_ok = t->wkind == 2 ? safe_ld32(t->wkey, &key_value) : 0;
        char history[64] = ""; size_t used = 0;
        const unsigned recorded = t->hist_pos < 8 ? t->hist_pos : 8;
        for (unsigned i = 0; i < recorded && used + 5 < sizeof history; ++i) {
            const unsigned slot = (t->hist_pos - recorded + i) & 7;
            used += (size_t)snprintf(history + used, sizeof history - used, i ? ",%x" : "%x", t->hist[slot]);
        }
        LOG("WAITER h=%#x id=%llu kind=%s waddr=%#llx wkey=%#llx lock=%s%#x key=%s%#x cmp=%d/%d tag=%u svc=%#x hist=%s lr=%s+0x%zx started=%d exited=%d wns=%lld nto=%llu nwake=%llu",
            t->handle, (unsigned long long)t->id, kind,
            (unsigned long long)t->waddr, (unsigned long long)t->wkey,
            lock_ok ? "" : "?", lock_value, key_ok ? "" : "?", key_value,
            t->wtype, t->wval, t->wtag,
            t->last_svc, history, lr ? lr : "?", offset, t->started, t->exited,
            (long long)t->wns, (unsigned long long)t->nto, (unsigned long long)t->nwake);
    }
    LOG("WAITER_SUMMARY threads=%u none=%u lock=%u cond=%u address=%u sync=%u",
        total, counts[0], counts[1], counts[2], counts[3], counts[4]);
    pthread_mutex_unlock(&G);
}
int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++;
        me->hist[me->hist_pos & 7] = (uint16_t)num; me->hist_pos++; } }
    if (0 && num != 0x0B) { thr_t *me = pthread_getspecific(g_keythr); LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        init_condition(&t->cv);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        if (1) {
            // The entry the guest passes is always the SDK's own trampoline, so
            // it says nothing about which subsystem spawned the thread. The
            // caller's return address does: each of the handful of CreateThread
            // sites in the title maps to one subsystem, and a subsystem whose
            // site never appears never got its thread.
            extern const char* ot2_host_module_of(uint64_t, uintptr_t*);
            uintptr_t off = 0;
            const char *mod = ot2_host_module_of(regs[30], &off);
            LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d at=%s+%#zx",
                t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core,
                mod ? mod : "?", off);
        }
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: guest_thread_exit(); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else sched_yield();
        return 1;
    }
    case 0x0C: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G);return 1; }
    case 0x0D: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G);return 1; }
    case 0x0E: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; pthread_mutex_unlock(&G);return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2]; }
        regs[0] = t ? 0 : R_BAD_HANDLE; pthread_mutex_unlock(&G);return 1;
    }
    case 0x10: regs[0] = (uint64_t)(uint32_t)cur()->core; return 1;
    case 0x25: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; pthread_mutex_unlock(&G);return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        // 交锁时 lock_handoff() 写进互斥字的是**标签**（w2），而这里原先拿 w0（线程句柄）
        // 去比，两个值域不同，条件永远不成立，所以慢路径从来没睡过（现场转储里 lock 等待
        // 恒为 0）。看起来像 bug，实测却不是：
        //   打开 arbitrate-handoff 改成"等到互斥字里出现自己的标签"，主线程立刻死在自己
        //   addr=0x4000000048（值 0，锁本来就空闲）上——因为 SDK 在 SVC 返回后会在用户态
        //   重新 CAS 抢锁，压根不指望这个 SVC 替它拿到锁。改成阻塞只会把可重试变成永久挂起。
        // 结论：这里的"立即返回"是正确行为，别再改。开关留着，只是为了下次能一次复现这个结论。
        // lock_handoff() stores the *tag* (w2) in the mutex word while this used to compare
        // against w0 (the thread handle): different value spaces, so the condition never held
        // and the slow path never slept (a live dump always shows zero lock waiters). It looks
        // like a bug but measurably is not:
        //   enabling arbitrate-handoff ("wait until the mutex word carries my tag") makes the
        //   main thread deadlock immediately on addr=0x4000000048, whose value was 0 -- the
        //   lock was free. The SDK re-attempts the CAS in user mode after the call returns and
        //   never expects this SVC to acquire the lock for it. Blocking only converts a retry
        //   into a permanent hang.
        // Conclusion: returning immediately here is correct; do not "fix" it. The switch stays
        // only so this conclusion can be reproduced in one run if it is ever doubted again.
        static int handoff_mode = -1;
        if (handoff_mode < 0) {
            handoff_mode = ot2_game_flag("arbitrate-handoff");
            if (handoff_mode) LOG("ARBITRATE handoff mode: wait until the tag in the mutex word is ours");
        }
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (!handoff_mode) {
            if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        } else {
            t->waddr = addr; t->wtag = (uint32_t)regs[2];
            // 被唤醒后再核一次：唤醒可能是别的原因，锁必须确实在自己名下才返回
            // Re-check after each wake: a wake may have another cause, and we return only
            // once the mutex word really carries our tag.
            while (!t->cancel && (ld32(addr) & ~WAITERS_BIT) != t->wtag) r = block(t, 1, -1);
            if ((ld32(addr) & ~WAITERS_BIT) != t->wtag) r = R_CANCELLED;
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        // 只记"真的会一直睡下去"的等待：带超时的等待是轮询（本项目里有个线程每 5ms
        // 醒一次），把它记下来会把 256 格的环形缓冲压成三百毫秒的窗口，正好盖掉出事
        // 之前那几秒。
        // Only waits that can sleep indefinitely are recorded: a wait with a timeout is a
        // poll (this title has a thread that wakes every 5 ms), and recording those shrinks
        // the 256-slot ring to a 300 ms window -- exactly hiding the seconds before the stall.
        if ((int64_t)regs[3] < 0) ev_note(EV_COND, t->handle, addr, key, -1, t->last_lr);
        const int64_t ns = (int64_t)regs[3];
        int64_t r = block(t, 2, ns);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        // 每条线程只记前几次带超时的返回，够判断"超时到底有没有到"而不把日志撑爆
        // Record only the first few timed returns per thread: enough to tell whether
        // timeouts ever fire, without flooding the log.
        //
        // 那个"每线程"上限不够：这个 title 有一个线程每 5 ms 醒一次，而线程是反复新建的，
        // 于是每一条新线程都重新拿到 5 次额度 —— 一分钟写出一万三千行、1.7 MB，占掉整个
        // boot.log 的七成，光 write 系统调用就把渲染线程拖慢。再加一个全局额度。
        // The per-thread cap alone was not enough: this title has a thread that wakes
        // every 5 ms and its threads are recreated, so each new one earned a fresh
        // quota -- 13k lines and 1.7 MB in a minute, 71% of the whole log, with the
        // write syscalls alone slowing the render thread down. Cap it globally too.
        if (ns > 0 && t->nto <= 4 && timeout_log_budget > 0) {
            if (__atomic_fetch_sub(&timeout_log_budget, 1, __ATOMIC_RELAXED) > 0)
                LOG("TIMEOUT_FIRED h=%#x id=%llu waddr=%#llx wkey=%#llx ns=%lld res=%#llx nto=%llu",
                    t->handle, (unsigned long long)t->id, (unsigned long long)addr,
                    (unsigned long long)key, (long long)ns, (unsigned long long)r,
                    (unsigned long long)t->nto);
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        if (!n) g_keysignal_empty++;   // 空发：键上没有等待者 / Empty signal: nobody was waiting on this key
        // 只有真的把某个线程叫醒的信号才是"交接"，进时序；空发只计数。
        // Only a signal that actually woke a thread is a handoff and goes into the
        // timeline; empty ones are counted only.
        if (n > 0) ev_note(EV_KEYSIGNAL, cur()->handle, key, (uint64_t)n, cnt, cur()->last_lr);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { svc_thread_note_address(addr, 0); t->waddr = addr; t->wtype = type; t->wval = val;
               ev_note(EV_ADDR, t->handle, addr, (uint64_t)(uint32_t)val, type, t->last_lr);
               r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        svc_thread_note_address(addr, 1);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            g_addrsignal_touched++;
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        int woke = 0;
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0);
            woke = n; }
        if (!nw) g_addrsignal_empty++;   // 空发：这个地址上没有等待者 / Empty signal: nobody was waiting on this address
        // 空发（谁也没叫醒）才是异常，进时序；真正叫醒了人的信号每秒上千次，只计数。
        // An empty signal is the anomaly and goes into the timeline; signals that did wake
        // someone run in the thousands per second and are only counted.
        if (!woke) ev_note(EV_ADDRSIGNAL, cur()->handle, addr, (uint64_t)(uint32_t)val, type == 0 ? -1 : 0, cur()->last_lr);
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        // A title that polls a handle the host never signals burns a core and
        // logs nothing, because each id reports only its first call. Sample the
        // handle and its object type often enough to name the culprit.
        {
            static unsigned long long seen;
            if (++seen % 2000000ULL == 0ULL) {
                const unsigned i = n > 0 ? hs[0] - 0x1000 : 0;
                const int type = (i && i < MAX_H) ? g_h[i].type : -1;
                const char *names[] = {"free", "thread", "eventReadable", "eventWritable", "transferMemory"};
                LOG("WAIT_SPIN count=%llu handle=%#x type=%s n=%d timeout=%lld",
                    seen, n > 0 ? hs[0] : 0,
                    (type >= 0 && type <= 4) ? names[type] : "none", n, (long long)ns);
            }
        }
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        // A handle outside the table names the reply that failed to produce one,
        // and the SDK's own `TryWait` turns "timed out" into a caller-side spin,
        // so this is worth one line per distinct site even though the wait
        // itself is legal. Budgeted: a stall repeats it thousands of times a
        // second and a full log is unreadable.
        if (n > 0) {
            const uint32_t h0 = hs[0];
            if (h0 < 0x1000 || h0 >= 0x1000 + MAX_H) {
                static unsigned long long bad_calls;
                static unsigned bad_printed;
                static uint32_t bad_handle[8];
                static uintptr_t bad_site[8];
                static unsigned bad_seen;
                ++bad_calls;
                uintptr_t offset = 0;
                extern const char* ot2_host_module_of(uint64_t, uintptr_t*);
                const char *mod = ot2_host_module_of(regs[30], &offset);
                const uintptr_t site = regs[30];
                unsigned known = 0;
                for (unsigned i = 0; i < bad_seen; i++) if (bad_handle[i] == h0 && bad_site[i] == site) known = 1;
                if (!known && bad_printed < 8) {
                    if (bad_seen < 8) { bad_handle[bad_seen] = h0; bad_site[bad_seen] = site; bad_seen++; }
                    bad_printed++;
                    LOG("WAIT_BAD handle=%#x n=%d timeout=%lld calls=%llu from=%s+0x%zx",
                        h0, n, (long long)ns, bad_calls, mod ? mod : "?", (size_t)offset);
                }
            }
        }
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1; int64_t sleep_ns = 0;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                // An invalid handle still has to consume the caller's timespan.
                // Returning a timeout immediately makes the SDK's wait loop spin:
                // it sees "timed out", recomputes the remaining time from an
                // unchanged deadline and calls again, so a 100 ms wait turns into
                // millions of syscalls per second and one core stays pinned.
                // The sleep itself must happen after G is released -- holding the
                // global lock while sleeping stops every other guest thread, which
                // trades a spin for a total deadlock. Waiter bookkeeping is
                // already clear at this point, so nothing is lost by leaving.
                sleep_ns = ns > 0 ? ns : 0;
                r = R_TIMEOUT; break;
            }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        if (sleep_ns > 0) {
            struct timespec rest;
            rest.tv_sec = (time_t)(sleep_ns / 1000000000LL);
            rest.tv_nsec = (long)(sleep_ns % 1000000000LL);
            nanosleep(&rest, NULL);
        }
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x15: { // Same-process transfer memory with real shared backing.
        if(!regs[2] || ((regs[1]|regs[2])&4095)){regs[0]=0xd401;return 1;}
        transfer_t* m=calloc(1,sizeof(*m));m->source=(void*)regs[1];m->size=regs[2];
        m->fd=(int)syscall(__NR_memfd_create,"ot2-transfer",1);
        if(m->fd<0 || ftruncate(m->fd,m->size))die("Transfer-memory backing failed");
        if(pwrite(m->fd,m->source,m->size,0)!=(ssize_t)m->size)die("Transfer-memory initialization failed");
        if(mmap(m->source,m->size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,m->fd,0)==MAP_FAILED)die("Transfer-memory source map failed");
        pthread_mutex_lock(&G);regs[1]=h_new(O_TRANSFER,m);pthread_mutex_unlock(&G);regs[0]=0;return 1;
    }
    case 0x51: case 0x52: {
        pthread_mutex_lock(&G);transfer_t* m=h_peek((uint32_t)regs[0],O_TRANSFER);
        if(!m || regs[2]!=m->size || (regs[1]&4095)){pthread_mutex_unlock(&G);regs[0]=m?0xd401:R_BAD_HANDLE;return 1;}
        int flags=num==0x51?MAP_SHARED|MAP_FIXED:MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED;
        void* result=mmap((void*)regs[1],m->size,num==0x51?PROT_READ|PROT_WRITE:PROT_NONE,flags,num==0x51?m->fd:-1,0);
        pthread_mutex_unlock(&G);regs[0]=result==MAP_FAILED?R_OUT_OF_RES:0;return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_TRANSFER) {
            transfer_t* m=g_h[i].obj;close(m->fd);free(m);g_h[i].type=O_FREE;g_h[i].obj=NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    }
    return 0;
}


#ifdef OT2_THREAD_TESTING
size_t ot2_thread_test_objects(void){pthread_mutex_lock(&G);size_t n=0;for(thr_t* t=g_all;t;t=t->all)n++;pthread_mutex_unlock(&G);return n;}
size_t ot2_thread_test_waiters(int kind,uint64_t address){pthread_mutex_lock(&G);size_t n=0;for(thr_t* t=g_wait;t;t=t->next)if(t->wkind==kind&&(!address||t->waddr==address))n++;pthread_mutex_unlock(&G);return n;}
#endif
