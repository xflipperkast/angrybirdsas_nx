#ifndef ASNX_FILE_BRIDGE_H
#define ASNX_FILE_BRIDGE_H
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <stddef.h>

int file_prepare_virtual_system_files(void);
int file_translate(const char *in, char *out, size_t cap);
int file_open(const char *, int, ...);
ssize_t file_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t file_pwrite(int fd, const void *buf, size_t count, off_t offset);
void file_note_fd_closed(int fd);
void file_note_fd_duplicated(int oldfd, int newfd);
FILE *file_fopen(const char *, const char *);
int file_fclose(FILE *fp);

int file_stat(const char *, void *);
int file_lstat(const char *, void *);
int file_fstat(int, void *);
int file_truncate(const char *, off_t);
int file_statfs(const char *, void *);

int file_access(const char *, int);
DIR *file_opendir(const char *);
void *file_readdir(DIR *);
int file_closedir(DIR *);
int file_mkdir(const char *, mode_t);
int file_unlink(const char *);
int file_remove(const char *);
int file_rename(const char *, const char *);
char *file_realpath(const char *, char *);
#endif
