/*
 * POSIX I/O wrapper
 * Author: Tom Bergan
 *
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <ftw.h>

#include "common.h"

FORCE_LINKAGE(io)

////////////////////////////////////////////////////////////////////////////////
// Macros for things we don't yet model
////////////////////////////////////////////////////////////////////////////////

// XXX: This is temporary
#define IGNORED(...) \
  DEFINE_MODEL(__VA_ARGS__) { \
    klee_warning("silently ignoring IO function"); \
    return 0; \
  }

// XXX: This is temporary: returns expected number of bytes
#define IGNORED_HACK(...) \
  DEFINE_MODEL(__VA_ARGS__) { \
    klee_warning("silently ignoring IO function"); \
    return count; \
  }

////////////////////////////////////////////////////////////////////////////////
// Macros for things we specifically model
////////////////////////////////////////////////////////////////////////////////

// Maybe: could constrain to errno further, based on the possible
// return values as specified in man pages?

#define ReadSyscallResult(op) \
  do { klee_make_symbolic(&ret, sizeof ret, op "_retval"); \
       klee_make_symbolic(&errno, sizeof errno, op "_errno"); \
  } while(0)

#define DEFINE_INPUT_MODEL_SIMPLE(type, op, ...) \
  DEFINE_INPUT_MODEL(type, op, __VA_ARGS__) { \
    type ret; \
    ReadSyscallResult(#op); \
    klee_assume(-1 <= ret); \
    return ret; \
  }

////////////////////////////////////////////////////////////////////////////////
// File ops
////////////////////////////////////////////////////////////////////////////////

DEFINE_INPUT_MODEL(ssize_t, read, int fd, void *buf, size_t count) {
  ssize_t ret;
  ReadSyscallResult("read");
  klee_assume(-1 <= ret);
  klee_assume((size_t)ret <= count);

  // Read the buffer
  const size_t bytesRead = klee_expr_select64(ret == -1, 0, ret);
  klee_make_symbolic(buf, bytesRead, "read_buffer");

  return ret;
}

DEFINE_INPUT_MODEL(ssize_t, write, int fd, const void *buf, size_t count) {
  ssize_t ret;
  ReadSyscallResult("write");
  klee_assume(-1 <= ret);
  klee_assume((size_t)ret <= count);
  return ret;
}

DEFINE_INPUT_MODEL_SIMPLE(int, open, const char *pathname, int flags, ...)
DEFINE_INPUT_MODEL_SIMPLE(int, creat, const char *pathname, mode_t mode)
DEFINE_INPUT_MODEL_SIMPLE(int, close, int fd)

static inline int do_stat(struct stat *buf) {
  ssize_t ret;
  ReadSyscallResult("stat");
  klee_assume(klee_expr_or(ret == -1, ret == 0));
  klee_make_symbolic(buf, sizeof *buf, "stat_buffer");
  return ret;
}

DEFINE_INPUT_MODEL(int, fstat, int fd, struct stat *buf) { return do_stat(buf); }
DEFINE_INPUT_MODEL(int, stat, const char *path, struct stat *buf) { return do_stat(buf); }
DEFINE_INPUT_MODEL(int, lstat, const char *path, struct stat *buf) { return do_stat(buf); }

////////////////////////////////////////////////////////////////////////////////
// Mmap
////////////////////////////////////////////////////////////////////////////////

static inline void* do_mmap(size_t length) {
  void *ret;
  ReadSyscallResult("mmap");
  void *ptr = klee_malloc(length, NULL, 0, 4096);
  klee_assume(ptr != 0);
  klee_assume(klee_expr_or(ret == (void*)-1, ret == ptr));
  klee_make_symbolic(ptr, length, "map_buffer");
  return ret;
}

DEFINE_INPUT_MODEL(void*, mmap, void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  return do_mmap(length);
}

DEFINE_INPUT_MODEL(void*, mmap2, void *addr, size_t length, int prot, int flags, int fd, off_t pgoffset) {
  return do_mmap(length);
}

DEFINE_INPUT_MODEL_SIMPLE(int, munmap, void *addr, size_t length)

////////////////////////////////////////////////////////////////////////////////
// FIXME: Should not be ignoring these, but our stupid alias analysis
// sucks so much it's screwing everything up
////////////////////////////////////////////////////////////////////////////////

#define DEFINE_DUMB_INPUT_MODEL(type, op, ...) \
  DEFINE_MODEL(type, op, __VA_ARGS__) { \
    type ret; \
    klee_make_symbolic(&ret, sizeof ret, #op "_retval"); \
    return ret; \
  }

DEFINE_DUMB_INPUT_MODEL(int, printf, const char *fmt, ...)
DEFINE_DUMB_INPUT_MODEL(int, fprintf, FILE *stream, const char *fmt, ...)
DEFINE_DUMB_INPUT_MODEL(int, vprintf, const char *fmt, va_list arg)

DEFINE_MODEL(void, perror, const char *s) {}
DEFINE_DUMB_INPUT_MODEL(char*, strerror, int errnum)

DEFINE_DUMB_INPUT_MODEL(int, fputc, int c, FILE *stream)
DEFINE_DUMB_INPUT_MODEL(int, __fputc_unlocked, int c, FILE *stream)
DEFINE_DUMB_INPUT_MODEL(int, fputs, const char *s, FILE *stream)
DEFINE_DUMB_INPUT_MODEL(int, putc, int c, FILE *stream)
DEFINE_DUMB_INPUT_MODEL(int, putchar, int c)
DEFINE_DUMB_INPUT_MODEL(int, puts, const char *s)
DEFINE_DUMB_INPUT_MODEL(int, fflush, FILE *stream)

DEFINE_MODEL(__attribute__((noreturn)) void, exit, int status) {
  _exit(status);
}

////////////////////////////////////////////////////////////////////////////////
// Super-simple FTW model
////////////////////////////////////////////////////////////////////////////////

typedef int (*ftwfunc)(const char *fpath, const struct stat *sb, int typeflag);

DEFINE_INPUT_MODEL(int, ftw, const char *dirpath, ftwfunc fn, int nopenfd) {
  // XXX: TEMP HACK (for SAS submission)
  //klee_ics_region_marker();

  // We steal typeflag == -1 as a code to finish the walk
  assert(FTW_F   != -1);
  assert(FTW_D   != -1);
  assert(FTW_DNR != -1);
  assert(FTW_NS  != -1);

  while (1) {
    // Since this function will be replaced with a wrapper at runtime (see
    // runtime/ConstrainedExec/lib.cpp), we need to ensure that all region
    // markers in this model function correspond directly with a region marker
    // in the wrapper function.  Hence, we place this marker manually.
    klee_ics_region_marker();

    // Simulate ftw() completion
    int typeflag;
    klee_make_symbolic(&typeflag, sizeof typeflag, "ftw_typeflag");
    if (typeflag == -1)
      return klee_int32("ftw_retval");

    // Call the user func
    char fpath[64];
    struct stat statbuf;
    klee_make_symbolic(&fpath, sizeof fpath, "ftw_fpath");
    klee_make_symbolic(&statbuf, sizeof statbuf, "ftw_statbuf");
    const int ret = fn(fpath, &statbuf, typeflag);
    if (ret != 0) {
      klee_int32("ftw_unused_retval");  // need an (unused) input here for the retval
      return ret;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Unmodeled: Read
////////////////////////////////////////////////////////////////////////////////

// read() is modeled
IGNORED(ssize_t, readv, int fd, const struct iovec *iov, int iovcnt)
IGNORED(ssize_t, pread, int fd, void *buf, size_t count, off_t offset)

////////////////////////////////////////////////////////////////////////////////
// Unmodeled: Write
////////////////////////////////////////////////////////////////////////////////

// write() is modeled
IGNORED(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)
IGNORED(ssize_t, pwrite, int fd, const void *buf, size_t count, off_t offset)

////////////////////////////////////////////////////////////////////////////////
// Unmodeled: Creating FDs
////////////////////////////////////////////////////////////////////////////////

// open() is modeled
// creat() is modeled
IGNORED(int, pipe, int pipefd[2])
IGNORED(int, dup, int oldfd)
IGNORED(int, dup2, int oldfd, int newfd)
IGNORED(int, dup3, int oldfd, int newfd, int flags)

////////////////////////////////////////////////////////////////////////////////
// Unmodeled: Sockets
////////////////////////////////////////////////////////////////////////////////

IGNORED(int, socket, int domain, int type, int protocol)
IGNORED(int, bind, int sockfd, const struct sockaddr *addr, socklen_t addrlen)
IGNORED(int, getsockname, int sockfd, struct sockaddr *addr, socklen_t *addrlen)
IGNORED(int, getpeername, int sockfd, struct sockaddr *addr, socklen_t *addrlen)
IGNORED(int, listen, int sockfd, int backlog)
IGNORED(int, connect, int sockfd, const struct sockaddr *addr, socklen_t addrlen)
IGNORED(int, accept, int sockfd, struct sockaddr *addr, socklen_t *addrlen)
IGNORED(int, accept4, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
IGNORED(int, shutdown, int sockfd, int how)
IGNORED(ssize_t, send, int sockfd, const void *buf, size_t len, int flags)
IGNORED(ssize_t, sendto, int fd, const void *buf, size_t count, int flags, const struct sockaddr* addr, socklen_t addr_len)
IGNORED(ssize_t, sendmsg, int sockfd, const struct msghdr *msg, int flags)
IGNORED(ssize_t, recv, int sockfd, void *buf, size_t len, int flags)
IGNORED(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr* addr, socklen_t* addr_len)
IGNORED(ssize_t, recvmsg, int sockfd, struct msghdr *msg, int flags)
IGNORED(int, getsockopt, int sockfd, int level, int optname, void *optval, socklen_t *optlen)
IGNORED(int, setsockopt, int sockfd, int level, int optname, const void *optval, socklen_t optlen)
IGNORED(int, socketpair, int domain, int type, int protocol, int sv[2])
IGNORED(int, getnameinfo, const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, unsigned int flags)
IGNORED(int, getaddrinfo, const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)

DEFINE_MODEL(void, freeaddrinfo, struct addrinfo *res) {
  // nop
}

////////////////////////////////////////////////////////////////////////////////
// Unmodeled: Misc
////////////////////////////////////////////////////////////////////////////////

IGNORED(ssize_t, sendfile, int out_fd, int in_fd, off_t *offset, size_t count)

// TODO: poll, epoll?
IGNORED(int, select, int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
IGNORED(int, fcntl, int fd, int cmd, ...)
IGNORED(int, ioctl, int fd, unsigned long request, ...)
IGNORED(char *, getcwd, char *buf, size_t size)
IGNORED(off_t, lseek, int fd, off_t offset, int whence)
IGNORED(int, chmod, const char *path, mode_t mode)
IGNORED(int, fchmod, int fd, mode_t mode)
IGNORED(int, getdents, unsigned int fd, struct dirent64 *dirp, unsigned int count)
IGNORED(int, getdents64, unsigned int fd, struct dirent *dirp, unsigned int count)

IGNORED(int, rmdir, const char *pathname)
IGNORED(ssize_t, readlink, const char *pathname, char *buf, size_t bufsize)
IGNORED(int, unlink, const char *pathname)
IGNORED(int, chroot, const char *pathname)
IGNORED(int, chown, const char *pathname, uid_t owner, gid_t group)
IGNORED(int, lchown, const char *pathname, uid_t owner, gid_t group)
IGNORED(int, chdir, const char *pathname)

DEFINE_MODEL(void, sync, void) {
  // nop
}

IGNORED(int, fsync, int fd)
IGNORED(int, fdatasync, int fd)
IGNORED(int, fchdir, int fd)
IGNORED(int, fchown, int fd, uid_t owner, gid_t group)
IGNORED(int, fstatfs, int fd, struct statfs *buf)
IGNORED(int, statfs, const char *pathname, struct statfs *buf)
IGNORED(int, ftruncate, int fd, off_t length)
IGNORED(int, truncate, const char *pathname, off_t length)
IGNORED(int, access, const char *pathname, int mode)

