/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Constrained Execution Runtime Library
 * A simple printf library so we don't need to include any
 * std headers (see comments in lib.h).
 *
 */

#include "lib.h"
// these declare nothing except a few typedefs and macros
#include <stdarg.h>
#include <syscall.h>

extern long int syscall(long int syscallno, ...);

typedef uint64_t uval_t;
typedef int64_t  sval_t;

typedef int bool;
#define true  1
#define false 0

//-----------------------------------------------------------------------------
// Minimal printf impl
// Uses a fixed-sized buffer and silently truncates past that buffer
//-----------------------------------------------------------------------------

static char* writestr(char *const buf, const char *const endp, const char *src) {
  char *outp = buf;

  for (const char *inp = src; src && *inp && outp < endp; ++inp, ++outp) {
    *outp = *inp;
  }

  return outp;
}

#define WriteChecked(c)   \
  do {                        \
    *(outp++) = (c);          \
    if (outp == endp)         \
      return outp;            \
  } while(0)

static char* writenumber(char *const buf, const char *const endp, const uval_t base, uval_t num) {
  char *outp = buf;

  // Prefix
  if (base == 8) {
    WriteChecked('0');
  }
  if (base == 16) {
    WriteChecked('0');
    WriteChecked('x');
  }

  // Find the multiplier for the most-significant digit
  // CAREFUL: need to check for overflow
  uval_t m = base;
  while (num/m >= 1) {
    if (m*base < m)
      goto print;
    m *= base;
  }

  m /= base;

print:
  // Print from most-to-least significant
  for (/* nop */; m && outp < endp; m /= base, ++outp) {
    uval_t digit = num / m;
    *outp = (digit >= 10) ? ((char)(digit-10) + 'a') : ((char)digit + '0');
    num %= m;
  }

  return outp;
}

static char* writedecimal(char *const buf, const char *const endp, bool isSigned, uval_t num) {
  char *outp = buf;

  if (isSigned) {
    sval_t snum = (sval_t)num;
    if (snum < 0) {
      WriteChecked('-');
      num = -num;
    }
  }

  return writenumber(outp, endp, 10, num);
}

// Not gcc's builtin vsnprintf
#define vsnprintf __ics_vsnprintf
static size_t vsnprintf(char *const buf, const char *const endp, const char *fmt, va_list args) {
  char *outp = buf;

  for (const char *inp = fmt; *inp && outp < endp; ++inp) {
    if (*inp != '%') {
      *(outp++) = *inp;
      continue;
    }

    ++inp;

    // For printing numbers
    enum { Hex, Octal, Decimal } type;
    bool isLong = false;
    bool isSigned = false;
    uval_t val;
    switch (*inp) {
    case 'l':
      isLong = 1;
      ++inp;
      break;
    }

    // Formatting type
    switch (*inp) {
    case '\0':
      goto done;

    case 's':
      outp = writestr(outp, endp, va_arg(args, const char*));
      break;

    case 'c':
      *(outp++) = (char)va_arg(args, int);
      break;

    case 'p':
      outp = writenumber(outp, endp, 16, (unsigned long long)va_arg(args, void*));
      break;

    case 'x':
      type = Hex;
      goto number;

    case 'o':
      type = Octal;
      goto number;

    case 'u':
      type = Decimal;
      isSigned = false;
      goto number;

    case 'd':
      type = Decimal;
      isSigned = true;
      goto number;

    number:
      val = isLong ? va_arg(args, long int) : va_arg(args, int);
      switch (type) {
      case Hex:
        outp = writenumber(outp, endp, 16, val);
        break;
      case Octal:
        outp = writenumber(outp, endp, 8, val);
        break;
      case Decimal:
        outp = writedecimal(outp, endp, isSigned, val);
        break;
      }
      break;

    default:
      *(outp++) = *inp;
      break;
    }
  }

done:
  return outp - buf;
}

//-----------------------------------------------------------------------------
// API
//-----------------------------------------------------------------------------

char* __ics_snprintf(char *buf, size_t bufsz, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  size_t strlen = vsnprintf(buf, buf + bufsz, fmt, ap);
  va_end(ap);
  if (strlen < bufsz)
    buf[strlen] = '\0';
  return buf + strlen;
}

void __ics_printf(int fd, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  size_t strlen = vsnprintf(buf, buf + sizeof(buf), fmt, ap);
  va_end(ap);
  syscall(SYS_write, fd, buf, strlen);
}

void __ics_vprintf(int fd, const char *fmt, va_list ap) {
  char buf[256];
  size_t strlen = vsnprintf(buf, buf + sizeof(buf), fmt, ap);
  syscall(SYS_write, fd, buf, strlen);
}

unsigned long int __ics_strlen(const char *str) {
  unsigned long int sz = 0;
  for (; *str; ++str)
    ++sz;
  return sz;
}
