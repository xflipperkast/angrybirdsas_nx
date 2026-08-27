#ifndef ASNX_BIONIC_H
#define ASNX_BIONIC_H
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define BIONIC_TLS_SIZE      0x400u
#define BIONIC_TLS_TP_OFFSET 0x200u
void bionic_install_tls(void *block);
void bionic_install_main_tls(void);
void bionic_set_mmap_arena(void *base, size_t size);
void bionic_set_main_stack_range(void *base, size_t size);

int *bionic_errno(void);
void *bionic_mmap(void*,size_t,int,int,int,long);
int bionic_munmap(void*,size_t);
int bionic_mprotect(void*,size_t,int);
int bionic_madvise(void*,size_t,int);
long bionic_syscall(long,...);
unsigned long bionic_getauxval(unsigned long);
int bionic_gettid(void);
long bionic_sysconf(int name);
int bionic_clock_gettime(int android_clock, struct timespec *tp);
int bionic_clock_getres(int android_clock, struct timespec *tp);

int bionic_pthread_mutex_init(void*,const void*);
int bionic_pthread_mutex_destroy(void*);
int bionic_pthread_mutex_lock(void*);
int bionic_pthread_mutex_trylock(void*);
int bionic_pthread_mutex_unlock(void*);
int bionic_pthread_cond_init(void*,const void*);
int bionic_pthread_cond_destroy(void*);
int bionic_pthread_cond_wait(void*,void*);
int bionic_pthread_cond_timedwait(void*,void*,const struct timespec*);
int bionic_pthread_cond_signal(void*);
int bionic_pthread_cond_broadcast(void*);
int bionic_pthread_rwlock_init(void*,const void*);
int bionic_pthread_rwlock_rdlock(void*);
int bionic_pthread_rwlock_wrlock(void*);
int bionic_pthread_rwlock_unlock(void*);
int bionic_pthread_once(volatile int*,void(*)(void));
int bionic_pthread_create(uint64_t*,const void*,void*(*)(void*),void*);
int bionic_pthread_join(uint64_t,void**);
int bionic_pthread_detach(uint64_t);
void bionic_pthread_exit(void*) __attribute__((noreturn));
uint64_t bionic_pthread_self(void);
int bionic_pthread_equal(uint64_t,uint64_t);
int bionic_pthread_setname_np(uint64_t,const char*);
int bionic_pthread_attr_init(void*);
int bionic_pthread_attr_destroy(void*);
int bionic_pthread_attr_setdetachstate(void*,int);
int bionic_pthread_attr_setstacksize(void*,size_t);
int bionic_pthread_attr_getstack(const void*,void**,size_t*);
int bionic_pthread_getattr_np(uint64_t,void*);
int bionic_pthread_key_create(unsigned*,void(*)(void*));
int bionic_pthread_key_delete(unsigned);
int bionic_pthread_setspecific(unsigned,const void*);
void *bionic_pthread_getspecific(unsigned);
int bionic_pthread_kill(uint64_t,int);
void bionic_note_sigaction(int sig, void *handler);
int bionic_pthread_sigmask(int,const void*,void*);

int bionic_sem_init(void*,int,unsigned);
int bionic_sem_destroy(void*);
int bionic_sem_post(void*);
int bionic_sem_wait(void*);
int bionic_sem_trywait(void*);
int bionic_sem_timedwait(void*,const struct timespec*);
int bionic_sem_getvalue(void*,int*);

int bionic_setjmp(void *env);
void bionic_longjmp(void *env, int value) __attribute__((noreturn));
#endif
