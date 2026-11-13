#ifndef OCEAN_WINDOWS_COMPAT_H
#define OCEAN_WINDOWS_COMPAT_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#define pid_t intptr_t
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#define SIG_BLOCK 0
#define SIG_SETMASK 1
#define WNOHANG 1
#define WIFEXITED(s) (((s) & 127) == 0)
#define WEXITSTATUS(s) ((unsigned)(s) >> 8)
#define WIFSIGNALED(s) (((s) & 127) != 0)
#define WTERMSIG(s) ((s) & 127)
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x40000000
#endif
#define F_SETFL 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
typedef unsigned long nfds_t;
struct pollfd { int fd; short events, revents; };
struct termios { int unused; };
#define TCSANOW 0
struct sigaction { void (*sa_handler)(int); int sa_mask; };
#define sigset_t int
struct utsname { char sysname[32], machine[32], release[32]; };
struct passwd { char *pw_dir; };
int oceanPoll(struct pollfd *, nfds_t, int);
int oceanSigaction(int, const struct sigaction *, struct sigaction *);
int oceanKill(pid_t, int);
pid_t oceanWait(pid_t, int *, int);
int oceanFcntl(int, int, int);
int oceanSetpgid(pid_t, pid_t);
int oceanRead(int, void *, unsigned);
FILE *oceanTmpfile(void);
FILE *oceanMemoryStream(char **, size_t *);
int oceanFlush(FILE *);
int oceanClose(FILE *);
int oceanMkstemp(char *);
char *oceanMkdtemp(char *);
char *oceanRealpath(const char *, char *);
int oceanRename(const char *, const char *);
int oceanUnlink(const char *);
int oceanStat(const char *, struct stat *);
char *oceanGetcwd(char *, int);
int oceanOpen(const char *, int, ...);
int oceanPipe(int [2]);
int oceanDup2(int, int);
int oceanClock(int, struct timespec *);
int oceanNanosleep(const struct timespec *, struct timespec *);
int oceanUname(struct utsname *);
struct passwd *oceanGetpwuid(int);
int oceanSetenv(const char *, const char *, int);
long long oceanGetline(char **, size_t *, FILE *);
char *oceanStrndup(const char *, size_t);
const char *oceanStrsignal(int);
int oceanRemoveDirectory(const char *);
#define CLOCK_MONOTONIC 1
int oceanScandir(const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **));
int oceanAlphasort(const struct dirent **, const struct dirent **);
#ifndef OCEAN_COMPAT_IMPLEMENTATION
#define tmpfile oceanTmpfile
#define open_memstream oceanMemoryStream
#define fflush oceanFlush
#define fclose oceanClose
#define mkstemp oceanMkstemp
#define mkdtemp oceanMkdtemp
#define realpath oceanRealpath
#define rename oceanRename
#define unlink oceanUnlink
#define lstat oceanStat
#define getcwd oceanGetcwd
#define mkdir(path, mode) _mkdir(path)
#define open oceanOpen
#define pipe oceanPipe
#define dup2 oceanDup2
#define read oceanRead
#define poll oceanPoll
#define waitpid oceanWait
#define kill oceanKill
#define fcntl oceanFcntl
#define sigaction(...) oceanSigaction(__VA_ARGS__)
#define sigemptyset(s) (*(s) = 0)
#define sigaddset(s, n) ((void)0)
#define sigprocmask(a, b, c) ((void)0)
#define setpgid oceanSetpgid
#define tcgetattr(a, b) (-1)
#define tcsetattr(a, b, c) (0)
#define uname oceanUname
#define getpwuid oceanGetpwuid
#define getuid() 0
#define setenv oceanSetenv
#define getline oceanGetline
#define strndup oceanStrndup
#define strsignal oceanStrsignal
#define gmtime_r(time, result) (gmtime_s((result), (time)) ? NULL : (result))
#define clock_gettime oceanClock
#define nanosleep oceanNanosleep
#define scandir oceanScandir
#define alphasort oceanAlphasort
#endif
#endif
#endif
