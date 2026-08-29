#include "fakefd.h"
#include <switch.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

enum {
    ANDROID_F_DUPFD = 0,
    ANDROID_F_GETFD = 1,
    ANDROID_F_SETFD = 2,
    ANDROID_F_GETFL = 3,
    ANDROID_F_SETFL = 4,
    ANDROID_F_GETLK = 5,
    ANDROID_F_SETLK = 6,
    ANDROID_F_SETLKW = 7,
    ANDROID_F_DUPFD_CLOEXEC = 1030
};
#define ANDROID_O_NONBLOCK 0x800
#define PIPE_CAPACITY 16384u

typedef enum {
    FAKEFD_UNUSED = 0,
    FAKEFD_PIPE_READ,
    FAKEFD_PIPE_WRITE
} FakeFdKind;

typedef struct PipeState {
    uint8_t data[PIPE_CAPACITY];
    size_t head;
    size_t length;
    unsigned readers;
    unsigned writers;
} PipeState;

typedef struct FakeFdEntry {
    FakeFdKind kind;
    PipeState *pipe;
    int fd_flags;
    int status_flags;
} FakeFdEntry;

static FakeFdEntry g_fds[ASNX_FAKE_FD_COUNT];
static Mutex g_lock;
static CondVar g_cond;
static int g_initialized;

static void ensure_initialized(void) {
    if (g_initialized) return;
    mutexInit(&g_lock);
    condvarInit(&g_cond);
    g_initialized = 1;
}

static int slot_from_fd(int fd) {
    int slot = fd - ASNX_FAKE_FD_BASE;
    return (slot >= 0 && slot < ASNX_FAKE_FD_COUNT) ? slot : -1;
}

static int alloc_slot_from(int minimum_fd) {
    int first = 0;
    if (minimum_fd > ASNX_FAKE_FD_BASE)
        first = minimum_fd - ASNX_FAKE_FD_BASE;
    if (first < 0) first = 0;
    if (first >= ASNX_FAKE_FD_COUNT) return -1;
    for (int i = first; i < ASNX_FAKE_FD_COUNT; ++i)
        if (g_fds[i].kind == FAKEFD_UNUSED) return i;
    return -1;
}

int fakefd_is_range(int fd) {
    return slot_from_fd(fd) >= 0;
}

int fakefd_is_open(int fd) {
    int slot = slot_from_fd(fd);
    if (slot < 0) return 0;
    ensure_initialized();
    mutexLock(&g_lock);
    int open = g_fds[slot].kind != FAKEFD_UNUSED;
    mutexUnlock(&g_lock);
    return open;
}

int fakefd_pipe(int fds[2]) {
    if (!fds) {
        errno = EFAULT;
        return -1;
    }
    ensure_initialized();
    mutexLock(&g_lock);
    int r = alloc_slot_from(ASNX_FAKE_FD_BASE);
    if (r < 0) {
        mutexUnlock(&g_lock);
        errno = EMFILE;
        return -1;
    }
    g_fds[r].kind = FAKEFD_PIPE_READ;
    int w = alloc_slot_from(ASNX_FAKE_FD_BASE);
    if (w < 0) {
        memset(&g_fds[r], 0, sizeof(g_fds[r]));
        mutexUnlock(&g_lock);
        errno = EMFILE;
        return -1;
    }
    PipeState *p = calloc(1, sizeof(*p));
    if (!p) {
        memset(&g_fds[r], 0, sizeof(g_fds[r]));
        mutexUnlock(&g_lock);
        errno = ENOMEM;
        return -1;
    }
    p->readers = 1;
    p->writers = 1;
    g_fds[r].kind = FAKEFD_PIPE_READ;
    g_fds[r].pipe = p;
    g_fds[r].fd_flags = 0;
    g_fds[r].status_flags = 0;
    g_fds[w].kind = FAKEFD_PIPE_WRITE;
    g_fds[w].pipe = p;
    g_fds[w].fd_flags = 0;
    g_fds[w].status_flags = 0;
    fds[0] = ASNX_FAKE_FD_BASE + r;
    fds[1] = ASNX_FAKE_FD_BASE + w;

    mutexUnlock(&g_lock);
    return 0;
}

ssize_t fakefd_read(int fd, void *buf, size_t count) {
    int slot = slot_from_fd(fd);
    if (slot < 0 || (!buf && count)) {
        errno = slot < 0 ? EBADF : EFAULT;
        return -1;
    }
    if (!count) return 0;
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry *e = &g_fds[slot];
    PipeState *p = e->pipe;
    if (e->kind != FAKEFD_PIPE_READ || !p) {
        mutexUnlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    while (p->length == 0 && p->writers != 0) {
        if (e->status_flags & ANDROID_O_NONBLOCK) {
            mutexUnlock(&g_lock);
            errno = EAGAIN;
            return -1;
        }
        condvarWait(&g_cond, &g_lock);

        if (g_fds[slot].kind != FAKEFD_PIPE_READ || g_fds[slot].pipe != p) {
            mutexUnlock(&g_lock);
            errno = EBADF;
            return -1;
        }
    }
    if (p->length == 0 && p->writers == 0) {
        mutexUnlock(&g_lock);
        return 0;
    }
    size_t amount = count < p->length ? count : p->length;
    uint8_t *dst = (uint8_t *)buf;
    size_t first = amount;
    if (first > PIPE_CAPACITY - p->head) first = PIPE_CAPACITY - p->head;
    memcpy(dst, p->data + p->head, first);
    if (amount > first) memcpy(dst + first, p->data, amount - first);
    p->head = (p->head + amount) % PIPE_CAPACITY;
    p->length -= amount;
    condvarWakeAll(&g_cond);
    mutexUnlock(&g_lock);
    return (ssize_t)amount;
}

ssize_t fakefd_write(int fd, const void *buf, size_t count) {
    int slot = slot_from_fd(fd);
    if (slot < 0 || (!buf && count)) {
        errno = slot < 0 ? EBADF : EFAULT;
        return -1;
    }
    if (!count) return 0;
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry *e = &g_fds[slot];
    PipeState *p = e->pipe;
    if (e->kind != FAKEFD_PIPE_WRITE || !p) {
        mutexUnlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    if (p->readers == 0) {
        mutexUnlock(&g_lock);
        errno = EPIPE;
        return -1;
    }
    const uint8_t *src = (const uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        while (p->length == PIPE_CAPACITY && p->readers != 0) {
            if (e->status_flags & ANDROID_O_NONBLOCK) {
                mutexUnlock(&g_lock);
                if (total) return (ssize_t)total;
                errno = EAGAIN;
                return -1;
            }
            condvarWait(&g_cond, &g_lock);
            if (g_fds[slot].kind != FAKEFD_PIPE_WRITE || g_fds[slot].pipe != p) {
                mutexUnlock(&g_lock);
                errno = EBADF;
                return -1;
            }
        }
        if (p->readers == 0) {
            mutexUnlock(&g_lock);
            if (total) return (ssize_t)total;
            errno = EPIPE;
            return -1;
        }
        size_t free_space = PIPE_CAPACITY - p->length;
        if (!free_space) continue;
        size_t amount = count - total;
        if (amount > free_space) amount = free_space;
        size_t tail = (p->head + p->length) % PIPE_CAPACITY;
        size_t first = amount;
        if (first > PIPE_CAPACITY - tail) first = PIPE_CAPACITY - tail;
        memcpy(p->data + tail, src + total, first);
        if (amount > first) memcpy(p->data, src + total + first, amount - first);
        p->length += amount;
        total += amount;
        condvarWakeAll(&g_cond);
        if (e->status_flags & ANDROID_O_NONBLOCK) break;
    }
    mutexUnlock(&g_lock);
    return (ssize_t)total;
}

int fakefd_close(int fd) {
    int slot = slot_from_fd(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry *e = &g_fds[slot];
    PipeState *p = e->pipe;
    if (e->kind == FAKEFD_UNUSED || !p) {
        mutexUnlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    if (e->kind == FAKEFD_PIPE_READ && p->readers) --p->readers;
    if (e->kind == FAKEFD_PIPE_WRITE && p->writers) --p->writers;
    memset(e, 0, sizeof(*e));
    int free_pipe = p->readers == 0 && p->writers == 0;
    condvarWakeAll(&g_cond);
    if (free_pipe) free(p);
    mutexUnlock(&g_lock);
    return 0;
}

static int clone_into_slot_locked(const FakeFdEntry *src, int slot, int cloexec) {
    if (slot < 0 || slot >= ASNX_FAKE_FD_COUNT || g_fds[slot].kind != FAKEFD_UNUSED)
        return -1;
    g_fds[slot] = *src;
    if (cloexec) g_fds[slot].fd_flags |= 1;
    if (src->kind == FAKEFD_PIPE_READ) ++src->pipe->readers;
    else if (src->kind == FAKEFD_PIPE_WRITE) ++src->pipe->writers;
    return ASNX_FAKE_FD_BASE + slot;
}

int fakefd_dup(int fd, int min_fd, int cloexec) {
    int slot = slot_from_fd(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry src = g_fds[slot];
    if (src.kind == FAKEFD_UNUSED || !src.pipe) {
        mutexUnlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    int dst = alloc_slot_from(min_fd);
    int ret = clone_into_slot_locked(&src, dst, cloexec);
    mutexUnlock(&g_lock);
    if (ret < 0) errno = EMFILE;
    return ret;
}

int fakefd_dup2(int oldfd, int newfd) {
    int oldslot = slot_from_fd(oldfd);
    int newslot = slot_from_fd(newfd);
    if (oldslot < 0 || newslot < 0) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd) return fakefd_is_open(oldfd) ? newfd : -1;
    if (fakefd_is_open(newfd)) fakefd_close(newfd);
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry src = g_fds[oldslot];
    if (src.kind == FAKEFD_UNUSED || !src.pipe) {
        mutexUnlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    int ret = clone_into_slot_locked(&src, newslot, 0);
    mutexUnlock(&g_lock);
    if (ret < 0) errno = EBUSY;
    return ret;
}

int fakefd_fcntl(int fd, int cmd, int arg) {
    int slot = slot_from_fd(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }
    if (cmd == ANDROID_F_DUPFD || cmd == ANDROID_F_DUPFD_CLOEXEC)
        return fakefd_dup(fd, arg, cmd == ANDROID_F_DUPFD_CLOEXEC);
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry *e = &g_fds[slot];
    if (e->kind == FAKEFD_UNUSED || !e->pipe) {
        mutexUnlock(&g_lock);
        errno = EBADF;
        return -1;
    }
    int ret = 0;
    switch (cmd) {
        case ANDROID_F_GETFD: ret = e->fd_flags; break;
        case ANDROID_F_SETFD: e->fd_flags = arg; break;
        case ANDROID_F_GETFL: ret = e->status_flags; break;
        case ANDROID_F_SETFL: e->status_flags = arg; break;
        case ANDROID_F_GETLK:
        case ANDROID_F_SETLK:
        case ANDROID_F_SETLKW:
            ret = 0; break;
        default:
            errno = EINVAL; ret = -1; break;
    }
    mutexUnlock(&g_lock);
    return ret;
}

int fakefd_ready(int fd) {
    int slot = slot_from_fd(fd);
    if (slot < 0) return ASNX_FD_READY_ERR;
    ensure_initialized();
    mutexLock(&g_lock);
    FakeFdEntry *e = &g_fds[slot];
    PipeState *p = e->pipe;
    int ready = 0;
    if (e->kind == FAKEFD_UNUSED || !p) {
        ready = ASNX_FD_READY_ERR;
    } else if (e->kind == FAKEFD_PIPE_READ) {
        if (p->length || p->writers == 0) ready |= ASNX_FD_READY_READ;
        if (p->writers == 0) ready |= ASNX_FD_READY_HUP;
    } else if (e->kind == FAKEFD_PIPE_WRITE) {
        if (p->readers == 0) ready |= ASNX_FD_READY_ERR | ASNX_FD_READY_HUP;
        else if (p->length < PIPE_CAPACITY) ready |= ASNX_FD_READY_WRITE;
    }
    mutexUnlock(&g_lock);
    return ready;
}
