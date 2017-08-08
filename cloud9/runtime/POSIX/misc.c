//===-- stubs.c -----------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "common.h"
#include "multiprocess.h"

FORCE_LINKAGE(misc)

////////////////////////////////////////////////////////////////////////////////
// Fakes
////////////////////////////////////////////////////////////////////////////////

DEFINE_MODEL(unsigned int, gnu_dev_major, unsigned long long int __dev) {
  return ((__dev >> 8) & 0xfff) | ((unsigned int) (__dev >> 32) & ~0xfff);
}

DEFINE_MODEL(unsigned int, gnu_dev_minor, unsigned long long int __dev) {
  return (__dev & 0xff) | ((unsigned int) (__dev >> 12) & ~0xff);
}

DEFINE_MODEL(unsigned long long int, gnu_dev_makedev, unsigned int __major, unsigned int __minor) {
  return ((__minor & 0xff) | ((__major & 0xfff) << 8)
	  | (((unsigned long long int) (__minor & ~0xff)) << 12)
	  | (((unsigned long long int) (__major & ~0xfff)) << 32));
}

////////////////////////////////////////////////////////////////////////////////
// Ignore with a warning
// TODO: organize this list more intelligently
// TODO: make sure these are all syscalls, not libc calls
////////////////////////////////////////////////////////////////////////////////

#define IGNORED_SILENT(...) \
  DEFINE_MODEL(__VA_ARGS__) { \
    klee_warning("silently ignoring"); \
    return 0; \
  }

#define IGNORED_VOID(...) \
  DEFINE_MODEL(__VA_ARGS__) { \
    klee_warning("silently ignoring"); \
  }

#define IGNORED_ERROR(ERR, ...) \
  DEFINE_MODEL(__VA_ARGS__) { \
    klee_warning("ignoring (" #ERR ")"); \
    errno = ERR; \
    return -1; \
  }

#define IGNORED_SLEEP_FOREVER(...) \
  DEFINE_MODEL(__VA_ARGS__) { \
    klee_warning("sleeping forever"); \
    uint64_t wlist = klee_get_wlist(); \
    __thread_sleep(&wlist, SyncOpOther); \
    return 0; \
  }

IGNORED_SILENT(pid_t, getpid, void)
IGNORED_SILENT(pid_t, getppid, void)
IGNORED_SILENT(mode_t, umask, mode_t mask)
IGNORED_SILENT(pid_t, wait, int *status)
IGNORED_SILENT(pid_t, waitpid, pid_t pid, int *status, int options)
IGNORED_SILENT(int, waitid, idtype_t idtype, id_t id, siginfo_t *infop, int options)

IGNORED_SILENT(int, __syscall_rt_sigaction, int signum, const struct sigaction *act, struct sigaction *oldact, size_t _something)
IGNORED_VOID(void, __syscall_rt_sigreturn, void)
IGNORED_VOID(void, __syscall_sigreturn, void)
IGNORED_SILENT(int, __syscall_sigaction, int signum, const struct sigaction *act, struct sigaction *oldact)
IGNORED_SILENT(int, sigaction, int signum, const struct sigaction *act, struct sigaction *oldact)

typedef void (*sighandler_t)(int);

IGNORED_SILENT(sighandler_t, signal, int signum, sighandler_t handler)
IGNORED_SILENT(sighandler_t, sigset, int sig, sighandler_t disp)
IGNORED_SILENT(int, sighold, int sig)
IGNORED_SILENT(int, sigrelse, int sig)
IGNORED_SILENT(int, sigignore, int sig)
IGNORED_SILENT(unsigned int, alarm, unsigned int seconds)
IGNORED_SILENT(int, sigprocmask, int how, const sigset_t *set, sigset_t *oldset)
IGNORED_SLEEP_FOREVER(int, sigwait, const sigset_t *set, int *sig)

IGNORED_ERROR(EIO, int, mkdir, const char *pathname, mode_t mode)
IGNORED_ERROR(EIO, int, mkfifo, const char *pathname, mode_t mode)
IGNORED_ERROR(EIO, int, mknod, const char *pathname, mode_t mode, dev_t dev)

IGNORED_ERROR(EPERM, int, link, const char *oldpath, const char *newpath)
IGNORED_ERROR(EPERM, int, symlink, const char *oldpath, const char *newpath)
IGNORED_ERROR(EPERM, int, rename, const char *oldpath, const char *newpath)

IGNORED_ERROR(EPERM, int, clock_settime, clockid_t clk_id, const struct timespec *res)
IGNORED_ERROR(EPERM, int, utime, const char *filename, const struct utimbuf *buf)
IGNORED_ERROR(EPERM, int, utimes, const char *filename, const struct timeval times[2])

IGNORED_ERROR(EBADF, int, futimes, int fd, const struct timeval times[2])

DEFINE_MODEL(int, getloadavg, double loadavg[], int nelem) {
  klee_warning("ignoring (-1 result)");
  return -1;
}

IGNORED_SILENT(uid_t, getuid, void)
IGNORED_SILENT(uid_t, geteuid, void)
IGNORED_SILENT(gid_t, getgid, void)
IGNORED_SILENT(gid_t, getegid, void)
IGNORED_SILENT(int, euidaccess, const char *pathname, int mode)
IGNORED_SILENT(int, eaccess, const char *pathname, int mode)
IGNORED_SILENT(int, group_member, gid_t __gid)

/* ACL */

/* FIXME: We need autoconf magic for this. */

#ifdef HAVE_SYS_ACL_H

#include <sys/acl.h>

IGNORED_ERROR(EPERM, int, acl_delete_def_file, const char *path_p)
IGNORED_ERROR(ENOENT, int, acl_extended_file, const char path_p)
IGNORED_ERROR(EINVAL, int, acl_entries, acl_t acl)
IGNORED_ERROR(ENOMEM, acl_t, acl_from_mode, mode_t mode)
IGNORED_ERROR(ENOMEM, acl_t, acl_get_fd, int fd)
IGNORED_ERROR(ENOMEM, acl_t, acl_get_file, const char *pathname, acl_type_t type)
IGNORED_ERROR(EPERM, int, acl_set_fd, int fd, acl_t acl)
IGNORED_ERROR(EPERM, int, acl_set_file, const char *path_p, acl_type_t type, acl_t acl)
IGNORED_ERROR(EINVAL, int, acl_free, void *obj_p)

#endif

IGNORED_ERROR(EPERM, int, mount, const char *source, const char *target, const char *filesystemtype, unsigned long mountflags, const void *data)
IGNORED_ERROR(EPERM, int, umount, const char *target)
IGNORED_ERROR(EPERM, int, umount2, const char *target, int flags)
IGNORED_ERROR(EPERM, int, swapon, const char *path, int swapflags)
IGNORED_ERROR(EPERM, int, swapoff, const char *path)

IGNORED_SILENT(int, setgid, gid_t gid)
IGNORED_ERROR(EPERM, int, setgroups, size_t size, const gid_t *list)
IGNORED_ERROR(EPERM, int, sethostname, const char *name, size_t len)
IGNORED_ERROR(EPERM, int, setpgid, pid_t pid, pid_t pgid)
IGNORED_ERROR(EPERM, int, setpgrp, void)
IGNORED_ERROR(EPERM, int, setpriority, __priority_which_t which, id_t who, int prio)
IGNORED_ERROR(EPERM, int, setresgid, gid_t rgid, gid_t egid, gid_t sgid)
IGNORED_ERROR(EPERM, int, setresuid, uid_t ruid, uid_t euid, uid_t suid)

IGNORED_SILENT(int, getrlimit, __rlimit_resource_t resource, struct rlimit *rlim)
IGNORED_SILENT(int, setrlimit, __rlimit_resource_t resource, const struct rlimit *rlim)
IGNORED_SILENT(int, setrlimit64, __rlimit_resource_t resource, const struct rlimit64 *rlim)

IGNORED_SILENT(int, getrusage, int who, struct rusage *usage)

IGNORED_ERROR(EPERM, pid_t, setsid, void)
IGNORED_ERROR(EPERM, int, setuid, uid_t uid)
IGNORED_ERROR(EPERM, int, reboot, int flag)
IGNORED_ERROR(EPERM, int, mlock, const void *addr, size_t len)
IGNORED_ERROR(EPERM, int, munlock, const void *addr, size_t len)
IGNORED_ERROR(EPERM, int, pause, void)
IGNORED_ERROR(EPERM, ssize_t, readahead, int fd, off64_t *offset, size_t count)

IGNORED_VOID(void, openlog, const char *ident, int option, int facility)
IGNORED_VOID(void, syslog, int priority, const char *format, ...)
IGNORED_VOID(void, closelog, void)
IGNORED_VOID(void, vsyslog, int priority, const char *format, va_list ap)
