//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Recognize common standard c library functions and generate graphs for them
//
//===----------------------------------------------------------------------===//

#include "dsa/StdLibInfo.h"
#include "llvm/Module.h"
#include "llvm/Function.h"

using namespace llvm;

namespace {

//
// libActions for known functions
//

#define NRET_NARGS    {0,0,0,0,0,0,0,0,0,0}
#define YRET_NARGS    {1,0,0,0,0,0,0,0,0,0}
#define NRET_YARGS    {0,1,1,1,1,1,1,1,1,1}
#define YRET_YARGS    {1,1,1,1,1,1,1,1,1,1}
#define NRET_NYARGS   {0,0,1,1,1,1,1,1,1,1}
#define YRET_NYARGS   {1,0,1,1,1,1,1,1,1,1}
#define NRET_YNARGS   {0,1,0,0,0,0,0,0,0,0}
#define YRET_YNARGS   {1,1,0,0,0,0,0,0,0,0}
#define NRET_NYNARGS  {0,0,1,0,0,0,0,0,0,0}
#define YRET_NYNARGS  {1,0,1,0,0,0,0,0,0,0}
#define YRET_NNYARGS  {1,0,0,1,1,1,1,1,1,1}
#define NRET_NNYARGS  {0,0,0,1,1,1,1,1,1,1}
#define YRET_NNYNARGS {1,0,0,1,0,0,0,0,0,0}
#define NRET_NNNYARGS {0,0,0,0,1,1,1,1,1,1}
#define NRET_YYNARGS  {0,1,1,0,0,0,0,0,0,0}
#define NRET_YNYARGS  {0,1,0,1,1,1,1,1,1,1}
#define NRET_NNYYARGS {0,0,0,1,1,1,1,1,1,1}
#define NRET_YYARGS   {0,1,1,1,1,1,1,1,1,1}

#define NOFLAGS           false, false, -1, -1
#define COLLAPSE          true, false, -1, -1
#define MEMBARRIER        false, true, -1, -1
#define THREADCREATE(a,b) false, false, a, b

const struct {
  const char* name;
  StdLibInfo::LibAction action;
} recFuncs[] = {
  {"stat",       {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fstat",      {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"lstat",      {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  
  {"getenv",     {NRET_YNARGS, YRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"getrusage",  {NRET_YNARGS, YRET_NYARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"getrlimit",  {NRET_YNARGS, YRET_NYARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"setrlimit",  {NRET_YARGS,  YRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"getcwd",     {NRET_NYARGS, YRET_YNARGS, NRET_NARGS, YRET_YNARGS, NOFLAGS}},
  {"getuid",     {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"getgid",     {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"geteuid",    {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"getegid",    {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  
  {"select",    {NRET_YARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"_setjmp",   {NRET_YARGS, YRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"longjmp",   {NRET_YARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  
  {"remove",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"rename",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"unlink",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fileno",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"create",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"write",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"read",      {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"truncate",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"open",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
 
  {"chdir",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"mkdir",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"rmdir",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  
  {"chmod",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fchmod",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"chown",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"lchown",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
 
  {"kill",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pipe",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  
  {"execl",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"execlp",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"execle",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"execv",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"execvp",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
 
  {"time",      {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"times",     {NRET_YARGS,  YRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"ctime",     {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"asctime",   {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"utime",     {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"localtime", {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"gmtime",    {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 
  {"ftime",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}}, 

  // printf not strictly true, %n could cause a write
  {"printf",    {NRET_YARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fprintf",   {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fprintf",   {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"sprintf",   {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"snprintf",  {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"vsnprintf", {NRET_YARGS,  YRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"sscanf",    {NRET_YARGS,  YRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"scanf",     {NRET_YARGS,  YRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fscanf",    {NRET_YARGS,  YRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"calloc",    {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, NOFLAGS}},
  {"malloc",    {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, NOFLAGS}},
  {"valloc",    {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, NOFLAGS}},
  {"realloc",   {NRET_NARGS, YRET_NARGS, YRET_YNARGS, YRET_YNARGS,NOFLAGS}},
  {"free",      {NRET_NARGS, NRET_NARGS, NRET_YNARGS, NRET_NARGS, NOFLAGS}},
 
  {"strdup",    {NRET_YARGS, YRET_NARGS, YRET_NARGS, YRET_YARGS, NOFLAGS}},
  {"__strdup",  {NRET_YARGS, YRET_NARGS, YRET_NARGS, YRET_YARGS, NOFLAGS}},
  {"wcsdup",    {NRET_YARGS, YRET_NARGS, YRET_NARGS, YRET_YARGS, NOFLAGS}},
 
  {"strlen",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"wcslen",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
 
  {"atoi",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"atof",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"atol",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"atoll",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"atoq",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
 
  {"memcmp",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"strcmp",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"wcscmp",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"strncmp",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"wcsncmp",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"strcasecmp",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"wcscasecmp",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"strncasecmp", {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"wcsncasecmp", {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  
  {"strcat",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"strncat",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  
  {"strcpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"stpcpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"wcscpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"strncpy",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"wcsncpy",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"memcpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"memccpy",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"wmemccpy",   {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}},
  {"memmove",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, COLLAPSE}}, 
  
  {"bcopy",      {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_YARGS, COLLAPSE}},
  {"bcmp",       {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, COLLAPSE}},
  
  {"strerror",   {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  COLLAPSE}},
  {"clearerr",   {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"strstr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"wcsstr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"strspn",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  COLLAPSE}},
  {"wcsspn",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  COLLAPSE}},
  {"strcspn",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  COLLAPSE}},
  {"wcscspn",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  COLLAPSE}},
  {"strtok",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"strpbrk",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"wcspbrk",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},

  {"strchr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"wcschr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"strrchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"wcsrchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"strchrnul",  {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"wcschrnul",  {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},

  {"memchr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"wmemchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},
  {"memrchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, COLLAPSE}},

  {"memalign",   {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, NOFLAGS}},
  {"posix_memalign",  {NRET_YARGS, YRET_YNARGS, NRET_NARGS,  NRET_NARGS, NOFLAGS}},

  {"perror",     {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  
  {"feof",       {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fflush",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fpurge",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fclose",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fopen",      {NRET_YARGS,  YRET_NARGS, YRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"ftell",      {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fseek",      {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, COLLAPSE}},
  {"lseek64",    {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"rewind",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, COLLAPSE}},
  {"ferror",     {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fwrite",     {NRET_YARGS,  NRET_NYARGS,NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fread",      {NRET_NYARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"fdopen",     {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"__errno_location", {NRET_NARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"puts",       {NRET_YARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"gets",       {NRET_NARGS,  YRET_YARGS,  NRET_NARGS, YRET_YNARGS, NOFLAGS}},
  {"fgets",      {NRET_NYARGS, YRET_YNARGS, NRET_NARGS, YRET_YNARGS, NOFLAGS}},
  {"getc",       {NRET_YNARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"ungetc",     {NRET_YNARGS, YRET_YARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"_IO_getc",   {NRET_YNARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"fgetc",      {NRET_YNARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"putc",       {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"_IO_putc",   {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"putchar",    {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"fputs",      {NRET_YARGS,  NRET_NYARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},
  {"fputc",      {NRET_YARGS,  NRET_NYARGS, NRET_NARGS, NRET_NARGS,  NOFLAGS}},

#if 0
  {"wait",       {false, false, false, false,  true, false, false, false, NOFLAGS}},
#endif

  {"__assert_fail",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"syscall",        {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  // POSIX Threads
  {"pthread_attr_destroy",         {NRET_YARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_init",            {NRET_YARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_attr_getdetachstate",  {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getguardsize",    {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getinheritsched", {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getschedparam",   {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getschedpolicy",  {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getscope",        {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getstackaddr",    {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_getstacksize",    {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_attr_setdetachstate",  {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setguardsize",    {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setinheritsched", {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setschedparam",   {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setschedpolicy",  {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setscope",        {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setstackaddr",    {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_attr_setstacksize",    {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_barrier_destroy",   {NRET_YARGS, NRET_YARGS,   NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_barrier_init",      {NRET_YARGS, NRET_YNARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_barrier_wait",      {NRET_YARGS, NRET_YARGS,   NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"pthread_barrierattr_destroy",    {NRET_YARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_barrierattr_getpshared", {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_barrierattr_init",       {NRET_NARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_barrierattr_setpshared", {NRET_YNARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_cancel", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  // TODO: pthread_cleanup_push
  // TODO: pthread_cleanup_pop

  {"pthread_cond_broadcast", {NRET_YARGS, NRET_YARGS,   NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_cond_destroy",   {NRET_YARGS, NRET_YARGS,   NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_cond_init",      {NRET_YARGS, NRET_YNARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_cond_signal",    {NRET_YARGS, NRET_YARGS,   NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_cond_timedwait", {NRET_YARGS, NRET_YYNARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_cond_wait",      {NRET_YARGS, NRET_YARGS,   NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"pthread_condattr_destroy",    {NRET_YARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_condattr_getpshared", {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_condattr_init",       {NRET_NARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_condattr_setpshared", {NRET_YNARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_create",  {NRET_YYNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, THREADCREATE(2,3)}},

  {"pthread_detach", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_equal",  {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_exit",   {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_getconcurrency", {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_getschedparam",  {NRET_NYARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_getspecific",    {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_join",       {NRET_NARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_key_create", {NRET_NARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_key_delete", {NRET_NARGS, NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_mutex_destroy", {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutex_init",    {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutex_lock",    {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_mutex_trylock", {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_mutex_unlock",  {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"pthread_mutex_getprioceiling", {NRET_YNARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutex_setprioceiling", {NRET_YARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_mutexattr_destroy",        {NRET_YARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_getprioceiling", {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_getprotocol",    {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_getpshared",     {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_gettype",        {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_init",           {NRET_NARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_setprioceiling", {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_setprotocol",    {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_setpshared",     {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_mutexattr_settype",        {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_once", {NRET_YARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"pthread_rwlock_destroy",   {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_rwlock_init",      {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_rwlock_rdlock",    {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_rwlock_tryrdlock", {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_rwlock_trywrlock", {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_rwlock_unlock",    {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"pthread_rwlock_wrlock",    {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"pthread_rwlockattr_destroy",    {NRET_YARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_rwlockattr_getpshared", {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_rwlockattr_init",       {NRET_NARGS,  NRET_YARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_rwlockattr_setpshared", {NRET_YNARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"pthread_self",           {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_setcancelstate", {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_setcanceltype",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_setconcurrency", {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_setschedparam",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_setspecific",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"pthread_testcancel",     {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  // Klee
  {"klee_print_expr",           {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_print_expr_only",      {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_print_expr_concretizations",
                                {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_print_range",          {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_debug",                {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_warning",              {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_warning_once",         {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_stack_trace",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_prefer_cex",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"klee_is_symbolic",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_make_symbolic",        {NRET_YNYARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_read_symbolic_input",  {NRET_YNYARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"klee_int8",                 {NRET_YNARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_int32",                {NRET_YNARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_int64",                {NRET_YNARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_range8",               {NRET_NNYARGS, NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_range32",              {NRET_NNYARGS, NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_range64",              {NRET_NNYARGS, NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_and",             {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_or",              {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_iff",             {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_implies",         {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_select8",         {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_select32",        {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_expr_select64",        {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_valuef",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_valued",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_valuel",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_valuell",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_value_i32",        {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_value_i64",        {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_to_unique8",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_to_unique32",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_to_unique64",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_load8_no_boundscheck", {NRET_YNARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_load32_no_boundscheck",{NRET_YNARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_load64_no_boundscheck",{NRET_YNARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"klee_check_memory_access",  {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_make_shared",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_mark_global",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_define_fixed_object",  {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"klee_is_running",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_slient_exit",          {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_abort",                {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_report_error",         {NRET_YNYARGS, NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_assume",               {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_event",                {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_set_option",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_option",           {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_fork",                 {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_merge",                {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_alias_function",       {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_ics_begin",            {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_ics_region_marker",    {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_ics_once_per_region",  {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_fork_for_concrete_initialization",
                                {NRET_NYNARGS, NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"klee_assert_fail",          {NRET_YARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_malloc",               {NRET_NARGS,   YRET_NARGS,  YRET_YNARGS,YRET_YNARGS,NOFLAGS}},
  {"klee_free",                 {NRET_NARGS,   NRET_NARGS,  NRET_YNARGS,NRET_NARGS, NOFLAGS}},
  {"klee_memset",               {NRET_YNARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_memcpy",               {NRET_YARGS,   NRET_YARGS,  NRET_NARGS, NRET_YARGS, COLLAPSE}},
  {"klee_memmove",              {NRET_YARGS,   NRET_YARGS,  NRET_NARGS, NRET_YARGS, COLLAPSE}},
  {"klee_strdup",               {NRET_YARGS,   YRET_NARGS,  YRET_NARGS, YRET_YARGS, NOFLAGS}},
  {"klee_get_time",             {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_set_time",             {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_errno",            {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_syscall",              {NRET_YARGS,   NRET_YARGS,  NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"klee_thread_terminate",     {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_process_terminate",    {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_thread_create",        {NRET_NYARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, THREADCREATE(1,2)}},
  {"klee_process_fork",         {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_process_context",  {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_thread_context",   {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_get_wlist",            {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_thread_preempt",       {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"klee_thread_sleep",         {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, MEMBARRIER}},
  {"klee_thread_notify",        {NRET_NARGS,   NRET_NARGS,  NRET_NARGS, NRET_NARGS, MEMBARRIER}},

  {"klee_add_hbnode",               {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_last_hbnode",              {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_add_hbedge",               {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_add_hbnode_to_hbgroup",    {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_add_hbedges_from_hbgroup", {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_add_hbedges_to_hbgroup",   {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_remove_hbgroup",           {NRET_NARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},

  {"klee_enumerate_possible_barrier_init_counts",
                             {NRET_NARGS,     NRET_NNYARGS,  NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_enumerate_possible_lock_owners",
                             {NRET_NARGS,     NRET_NARGS,    NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_enumerate_possible_waiters",
                             {NRET_NNYYARGS,  NRET_NNYYARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_enumerate_threads", {NRET_YYARGS,    NRET_YYARGS,   NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_lockset_add",       {NRET_NARGS,     NRET_NARGS,    NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"klee_lockset_remove",    {NRET_NARGS,     NRET_NARGS,    NRET_NARGS, NRET_NARGS, NOFLAGS}},

  // Klee memset/memcpy wrappers
  {"__klee_model_memset",    {NRET_YNARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"__klee_model_bzero",     {NRET_YNARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  {"__klee_model_memcpy",    {NRET_YARGS,   NRET_YARGS,  NRET_NARGS, NRET_YARGS, COLLAPSE}},
  {"__klee_model_memmove",   {NRET_YARGS,   NRET_YARGS,  NRET_NARGS, NRET_YARGS, COLLAPSE}},
  {"__klee_model_bcopy",     {NRET_YARGS,   NRET_YARGS,  NRET_NARGS, NRET_YARGS, COLLAPSE}},

  // C++ functions, as mangled on linux gcc 4.2
  // operator new(unsigned long)
  {"_Znwm",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, NOFLAGS}},
  // operator new[](unsigned long)
  {"_Znam",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, NOFLAGS}},
  // operator new(unsigned int)
  {"_Znwj",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, NOFLAGS}},
  // operator new[](unsigned int)
  {"_Znaj",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, NOFLAGS}},
  // operator delete(void*)
  {"_ZdlPv",     {NRET_NARGS, NRET_NARGS, NRET_YNARGS,NRET_NARGS, NOFLAGS}},
  // operator delete[](void*)
  {"_ZdaPv",     {NRET_NARGS, NRET_NARGS, NRET_YNARGS, NRET_NARGS, NOFLAGS}},
  // flush
  {"_ZNSo5flushEv", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  // << operator
  {"_ZNSolsEd", {NRET_YARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  // << operator
  {"_ZNSolsEPFRSoS_E", {NRET_YARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  //endl
  {"_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
  // Terminate the list of special functions recognized by this pass
  {0,            {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, NOFLAGS}},
};

const struct {
  Intrinsic::ID id;
  StdLibInfo::LibAction action;
} recIntrinsics[] = {
  {Intrinsic::memory_barrier, {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, MEMBARRIER}},
};

/*
   Functions to add
   freopen
   strftime
   strtoul
   strtol
   strtoll
   ctype family
   setbuf
   setvbuf
   __strpbrk_c3
   open64/fopen64/lseek64
 */

}

void StdLibInfo::initialize(Module &M) {
  if (&M == module)
    return;

  module = &M;
  functionMap.clear();
  intrinsicMap.clear();

  for (int x = 0; recFuncs[x].name; ++x) {
    if (Function* F = M.getFunction(recFuncs[x].name))
      if (F->isDeclaration())
        functionMap[F] = &recFuncs[x].action;
  }

  for (int x = 0; recIntrinsics[x].id; ++x) {
    intrinsicMap[recIntrinsics[x].id] = &recIntrinsics[x].action;
  }
}

const StdLibInfo::LibAction*
StdLibInfo::getLibActionForFunction(const Function *F) const {
  assert(module && "Forgot to call StdLibInfo::initializeForModule?");

  if (!F)
    return NULL;

  Intrinsic::ID id = (Intrinsic::ID)F->getIntrinsicID();
  IntrinsicMapTy::const_iterator ii = intrinsicMap.find(id);
  if (ii != intrinsicMap.end())
    return ii->second;

  FunctionMapTy::const_iterator fi = functionMap.find(F);
  if (fi != functionMap.end())
    return fi->second;

  return NULL;
}

const StdLibInfo::LibAction*
StdLibInfo::getLibActionForFunctionName(StringRef name) const {
  // Linear search since this isn't used on a critical path
  for (int x = 0; recFuncs[x].name; ++x) {
    if (recFuncs[x].name == name)
      return &recFuncs[x].action;
  }
  return NULL;
}
