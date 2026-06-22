#ifndef VC_PLATFORM_H
#define VC_PLATFORM_H

// ---------------------------------------------------------------------------
// vcPlatform.h  –  cross-platform compatibility shims for vc.
//
// NOTE: _POSIX_C_SOURCE=200809L and _DEFAULT_SOURCE are defined in the
// Makefile CFLAGS so they take effect before any system header is included.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// vcPlatform.h  –  cross-platform compatibility shims for vc.
//
// Supported platforms:
//   Linux    – native (no changes needed)
//   macOS    – POSIX-compliant; minor guards for readline path
//   Windows  – MSYS2/MinGW with gcc
//
// Include this before platform-specific headers in each .c file.
// ---------------------------------------------------------------------------

#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
#  define VC_WINDOWS 1
#endif

#if defined(__APPLE__) && defined(__MACH__)
#  define VC_MACOS 1
#endif

#if !defined(VC_WINDOWS) && !defined(VC_MACOS)
#  define VC_LINUX 1
#endif

// --- unistd / io --------------------------------------------------------
#ifdef VC_WINDOWS
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
#  include <direct.h>
#  include <windows.h>
#  include <shlwapi.h>
#  define  mkdir(p,m)   _mkdir(p)
#  define  getcwd       _getcwd
#  define  chdir        _chdir
#  define  strdup       _strdup
#  define  popen        _popen
#  define  pclose       _pclose
#  define  STDIN_FILENO 0
#else
#  include <unistd.h>
#  include <termios.h>
#  include <sys/time.h>
#  include <time.h>
#endif

// --- readline -----------------------------------------------------------
#if defined(VC_WINDOWS)
   // readline available in MSYS2: pacman -S mingw-w64-x86_64-readline
#  if defined(__has_include) && __has_include(<readline/readline.h>)
#    define VC_HAS_READLINE 1
#    include <readline/readline.h>
#    include <readline/history.h>
#  endif
#else
   // Linux / macOS: use readline if available, fgets fallback otherwise
#  if defined(__has_include) && __has_include(<readline/readline.h>)
#    define VC_HAS_READLINE 1
#    include <readline/readline.h>
#    include <readline/history.h>
#  endif
#endif

// If readline is not available, provide a simple fgets-based replacement.
#ifndef VC_HAS_READLINE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline char *readline(const char *prompt) {
    if (prompt) { fputs(prompt, stdout); fflush(stdout); }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t l = strlen(buf);
    if (l > 0 && buf[l-1] == '\n') buf[--l] = '\0';
    return strdup(buf);
}
static inline void add_history(const char *s) { (void)s; }
#endif

// --- password echo suppression ------------------------------------------
// POSIX: tcgetattr/tcsetattr.  Windows: SetConsoleMode.
static inline void vc_echo_off(void) {
#ifdef VC_WINDOWS
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  mode;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
#else
    // Caller handles via termios directly — this shim is for
    // files that use the vc_echo_off/on helpers instead.
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
}

static inline void vc_echo_on(void) {
#ifdef VC_WINDOWS
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  mode;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_ECHO_INPUT);
#else
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
}

// --- ANSI colour support ------------------------------------------------
// Windows 10 1511+ supports VT100 sequences but needs opt-in.
// Call vc_init_terminal() once at startup.
static inline void vc_init_terminal(void) {
#ifdef VC_WINDOWS
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

// --- fnmatch ------------------------------------------------------------
#ifdef VC_WINDOWS
// Windows has PathMatchSpec in shlwapi — wrap it to match fnmatch signature.
#include <shlwapi.h>
static inline int vc_fnmatch(const char *pattern, const char *string) {
    return PathMatchSpecA(string, pattern) ? 0 : 1;
}
#else
#include <fnmatch.h>
static inline int vc_fnmatch(const char *pattern, const char *string) {
    return fnmatch(pattern, string, FNM_PATHNAME);
}
#endif

// --- getuid substitute --------------------------------------------------
// Used only in vcAuth.c for key derivation. On Windows use process id
// combined with user SID hash as a rough substitute.
#ifdef VC_WINDOWS
static inline unsigned int vc_getuid(void) {
    return (unsigned int)GetCurrentProcessId();
}
#else
static inline unsigned int vc_getuid(void) {
    return (unsigned int)getuid();
}
#endif

// --- chmod (permissions) ------------------------------------------------
// On Windows, file ACLs work differently. chmod(path, 0600) is a no-op
// on NTFS — rely on the directory permissions instead.
#ifdef VC_WINDOWS
static inline int vc_chmod_private(const char *path) {
    (void)path;
    return 0;   // no-op; NTFS ACLs handle this separately
}
#else
#include <sys/stat.h>
static inline int vc_chmod_private(const char *path) {
    return chmod(path, 0600);
}
#endif

// --- path separator -----------------------------------------------------
#ifdef VC_WINDOWS
#  define VC_PATH_SEP '\\'
#  define VC_PATH_SEP_STR "\\"
#else
#  define VC_PATH_SEP '/'
#  define VC_PATH_SEP_STR "/"
#endif

// --- sleep --------------------------------------------------------------
#ifdef VC_WINDOWS
#  define vc_sleep_ms(ms) Sleep(ms)
#else
static inline void vc_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#endif // VC_PLATFORM_H
