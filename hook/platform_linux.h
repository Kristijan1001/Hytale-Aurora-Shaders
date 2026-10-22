// Small POSIX implementation of the OS services used by the shared shader engine.
// Windowing/input lives in linux_runtime.h and uses the game's SDL3 on X11, Wayland or Cocoa.
#ifndef AURORA_PLATFORM_LINUX_H
#define AURORA_PLATFORM_LINUX_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cairo/cairo.h>
#ifdef __APPLE__
#include <dirent.h>
#define st_mtim st_mtimespec
#else
#include <sys/inotify.h>
#endif

#define WINAPI
#define APIENTRY
#define __declspec(x) __attribute__((visibility("default")))
#define MAX_PATH 4096
#define _strdup strdup
#define strtok_s strtok_r
#define _snwprintf swprintf
#define INVALID_FILE_ATTRIBUTES (-1)
typedef void *HMODULE;
typedef void (*PROC)(void);
typedef const char *LPCSTR;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef unsigned int UINT;
typedef pthread_mutex_t CRITICAL_SECTION;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { int wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { uint32_t dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { FILETIME ftLastWriteTime; } WIN32_FILE_ATTRIBUTE_DATA;
#define GetFileExInfoStandard 0

static void InitializeCriticalSection(CRITICAL_SECTION *lock)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
#define EnterCriticalSection pthread_mutex_lock
#define LeaveCriticalSection pthread_mutex_unlock
static LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static void QueryPerformanceFrequency(LARGE_INTEGER *n) { n->QuadPart = 1000000000; }
static void QueryPerformanceCounter(LARGE_INTEGER *n)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    n->QuadPart = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static ULONGLONG GetTickCount64(void)
{
    struct timespec tick;
    clock_gettime(CLOCK_MONOTONIC, &tick);
    return (ULONGLONG)tick.tv_sec * 1000 + tick.tv_nsec / 1000000;
}
static void GetLocalTime(SYSTEMTIME *out)
{
    struct timespec t;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &t);
    localtime_r(&t.tv_sec, &tm);
    *out = (SYSTEMTIME){tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(t.tv_nsec / 1000000)};
}

// Use a thread-local UTF-8 locale, leaving the game's numeric and process locale alone.
static locale_t au_utf8;
static pthread_once_t au_locale_once = PTHREAD_ONCE_INIT;
static void au_locale_init(void) { au_utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0); }
static void au_to_wide(wchar_t *out, size_t cap, const char *s)
{
    pthread_once(&au_locale_once, au_locale_init);
    locale_t prev = au_utf8 ? uselocale(au_utf8) : (locale_t)0;
    size_t n = mbstowcs(out, s, cap - 1);
    out[n == (size_t)-1 ? 0 : n] = 0;
    if (prev) uselocale(prev);
}
static void au_to_utf8(char *out, size_t cap, const wchar_t *s)
{
    pthread_once(&au_locale_once, au_locale_init);
    locale_t prev = au_utf8 ? uselocale(au_utf8) : (locale_t)0;
    size_t n = wcstombs(out, s, cap - 1);
    out[n == (size_t)-1 ? 0 : n] = 0;
    if (prev) uselocale(prev);
}
#define CP_UTF8 65001
static int MultiByteToWideChar(unsigned page, unsigned flags, const char *source, int length, wchar_t *output, int capacity)
{
    if (capacity <= 0) return 0;
    au_to_wide(output, (size_t)capacity, source);
    return (int)wcslen(output) + 1;
}
static FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char p[MAX_PATH], m[16];
    au_to_utf8(p, sizeof p, path);
    au_to_utf8(m, sizeof m, mode);
    return fopen(p, m);
}
static int GetFileAttributesW(const wchar_t *path)
{
    char p[MAX_PATH]; struct stat st;
    au_to_utf8(p, sizeof p, path);
    return stat(p, &st) == 0 ? 0 : INVALID_FILE_ATTRIBUTES;
}
static int GetFileAttributesExW(const wchar_t *path, int kind, WIN32_FILE_ATTRIBUTE_DATA *data)
{
    char native[MAX_PATH];
    struct stat info;
    au_to_utf8(native, sizeof native, path);
    if (stat(native, &info) != 0) return 0;
    uint64_t stamp = (uint64_t)info.st_mtim.tv_sec * 1000000000u + info.st_mtim.tv_nsec;
    data->ftLastWriteTime = (FILETIME){(uint32_t)stamp, (uint32_t)(stamp >> 32)};
    return 1;
}
static int CompareFileTime(const FILETIME *left, const FILETIME *right)
{
    uint64_t first = ((uint64_t)left->dwHighDateTime << 32) | left->dwLowDateTime;
    uint64_t second = ((uint64_t)right->dwHighDateTime << 32) | right->dwLowDateTime;
    return (first > second) - (first < second);
}
static void DeleteFileW(const wchar_t *path)
{
    char p[MAX_PATH]; au_to_utf8(p, sizeof p, path); unlink(p);
}
static void CreateDirectoryW(const wchar_t *path, void *unused)
{
    char p[MAX_PATH]; au_to_utf8(p, sizeof p, path); mkdir(p, 0755);
}
static PROC linux_gl_proc(const char *name);
#define GetProcAddress(module, name) linux_gl_proc(name)
#endif
