#ifndef ASNX_FAKEFD_H
#define ASNX_FAKEFD_H

#include <stddef.h>
#include <sys/types.h>

#define ASNX_FAKE_FD_BASE 900
#define ASNX_FAKE_FD_COUNT 48

#define ASNX_FD_READY_READ  0x01
#define ASNX_FD_READY_WRITE 0x02
#define ASNX_FD_READY_HUP   0x04
#define ASNX_FD_READY_ERR   0x08

int fakefd_is_range(int fd);
int fakefd_is_open(int fd);
int fakefd_pipe(int fds[2]);
ssize_t fakefd_read(int fd, void *buf, size_t count);
ssize_t fakefd_write(int fd, const void *buf, size_t count);
int fakefd_close(int fd);
int fakefd_fcntl(int fd, int cmd, int arg);
int fakefd_dup(int fd, int min_fd, int cloexec);
int fakefd_dup2(int oldfd, int newfd);
int fakefd_ready(int fd);

#endif
