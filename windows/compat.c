#ifndef OCEAN_COMPAT_IMPLEMENTATION
#define OCEAN_COMPAT_IMPLEMENTATION
#endif
#include "compat.h"
#include "../platform.h"
#include <dirent.h>
#include <errno.h>
#include <process.h>
#include <stdarg.h>

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *out = n ? malloc((size_t)n * sizeof(*out)) : NULL;
    if (out) MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}
static char *utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = n ? malloc((size_t)n) : NULL;
    if (out) WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    return out;
}
char *oceanExecutable(void) {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    if (!n || n == 32768) return NULL;
    char *result = utf8(path);
    if (result) for (char *p = result; *p; p++) if (*p == '\\') *p = '/';
    return result;
}
/* _spawn passes a command line to the CRT. Quote every argument, including
 * trailing backslashes, without passing any of them through cmd.exe. */
static wchar_t *quoted(const char *s) {
    wchar_t *input = wide(s); if (!input) return NULL;
    wchar_t *result = malloc((wcslen(input) * 2 + 3) * sizeof(*result));
    if (!result) { free(input); return NULL; }
    wchar_t *out = result; *out++ = L'"';
    size_t slashes = 0;
    for (const wchar_t *p = input; ; p++) {
        if (*p == L'\\') { slashes++; continue; }
        size_t count = (*p == L'"' || !*p) ? slashes * 2 : slashes;
        while (count--) *out++ = L'\\'; slashes = 0;
        if (!*p) break;
        if (*p == L'"') *out++ = L'\\';
        *out++ = *p;
    }
    *out++ = L'"'; *out = 0; free(input); return result;
}
pid_t oceanSpawn(char *const argv[], const oceanDescriptor *descriptors, size_t count) {
    size_t argc = 0; while (argv[argc]) argc++;
    wchar_t **args = calloc(argc + 1, sizeof(*args));
    wchar_t *program = wide(argv[0]);
    int *saved = malloc((count ? count : 1) * sizeof(*saved));
    if (!args || !program || !saved) { free(args); free(program); free(saved); errno = ENOMEM; return -1; }
    for (size_t i = 0; i < argc; i++) {
        if (!(args[i] = quoted(argv[i]))) { for (size_t j = 0; j < i; j++) free(args[j]); free(args); free(program); free(saved); return -1; }
    }
    fflush(NULL);
    size_t mapped = 0; pid_t child = -1;
    for (; mapped < count; mapped++) {
        int target = descriptors[mapped].destination;
        saved[mapped] = _dup(target);
        if (saved[mapped] >= 0) SetHandleInformation((HANDLE)_get_osfhandle(saved[mapped]), HANDLE_FLAG_INHERIT, 0);
        if (_dup2(descriptors[mapped].source, target)) break;
        SetHandleInformation((HANDLE)_get_osfhandle(target), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    if (mapped == count) child = _wspawnvp(_P_NOWAIT, program, (const wchar_t *const *)args);
    int error = errno;
    for (size_t i = 0; i < mapped; i++) {
        if (saved[i] >= 0) { _dup2(saved[i], descriptors[i].destination); _close(saved[i]); }
        else _close(descriptors[i].destination);
    }
    if (mapped < count && saved[mapped] >= 0) _close(saved[mapped]);
    for (size_t i = 0; i < argc; i++) free(args[i]);
    free(args); free(program); free(saved); errno = error; return child;
}
/* Package supervisors own job objects so cancellation and normal cleanup cannot
 * strand test grandchildren holding output pipes or worker slots. */
typedef struct processGroup { HANDLE process, job; struct processGroup *next; } processGroup;
static processGroup *groups;
int oceanSetpgid(pid_t process, pid_t group) {
    (void)group;
    HANDLE job = CreateJobObjectW(NULL, NULL); if (!job) return -1;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, (HANDLE)process)) { CloseHandle(job); return -1; }
    processGroup *entry = malloc(sizeof(*entry)); if (!entry) { CloseHandle(job); return -1; }
    *entry = (processGroup){(HANDLE)process, job, groups}; groups = entry; return 0;
}
pid_t oceanWait(pid_t child, int *status, int options) {
    HANDLE process = (HANDLE)child;
    DWORD result = WaitForSingleObject(process, options & WNOHANG ? 0 : INFINITE);
    if (result == WAIT_TIMEOUT) return 0;
    DWORD code;
    if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(process, &code)) { errno = ECHILD; return -1; }
    *status = (int)((code & 0xffffff) << 8);
    for (processGroup **p = &groups; *p; p = &(*p)->next) if ((*p)->process == process) {
        processGroup *entry = *p; *p = entry->next; CloseHandle(entry->job); free(entry); break;
    }
    CloseHandle(process); return child;
}
int oceanKill(pid_t child, int number) {
    if (child < 0) {
        child = -child;
        for (processGroup *p = groups; p; p = p->next) if (p->process == (HANDLE)child)
            return TerminateJobObject(p->job, 128 + number) ? 0 : -1;
    }
    return TerminateProcess((HANDLE)child, 128 + number) ? 0 : -1;
}
int oceanSigaction(int number, const struct sigaction *action, struct sigaction *old) {
    if (number == SIGHUP || number == SIGPIPE) { if (old) old->sa_handler = SIG_DFL; return 0; }
    void (*previous)(int) = signal(number, action ? action->sa_handler : SIG_DFL);
    if (old) old->sa_handler = previous;
    return previous == SIG_ERR ? -1 : 0;
}
int oceanPipe(int descriptors[2]) { return _pipe(descriptors, 65536, _O_BINARY | _O_NOINHERIT); }
int oceanDup2(int from, int to) { return _dup2(from, to); }
static unsigned char nonblocking[2048];
int oceanFcntl(int fd, int command, int value) {
    if (fd < 0 || fd >= 2048) { errno = EBADF; return -1; }
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (command == F_SETFD) return SetHandleInformation(handle, HANDLE_FLAG_INHERIT, value & FD_CLOEXEC ? 0 : HANDLE_FLAG_INHERIT) ? 0 : -1;
    if (command == F_SETFL) { nonblocking[fd] = !!(value & O_NONBLOCK); return 0; }
    errno = EINVAL; return -1;
}
int oceanRead(int fd, void *buffer, unsigned size) {
    if (fd >= 0 && fd < 2048 && nonblocking[fd]) {
        HANDLE handle = (HANDLE)_get_osfhandle(fd); DWORD available = 0;
        if (GetFileType(handle) == FILE_TYPE_PIPE && PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL) && !available) { errno = EAGAIN; return -1; }
    }
    return _read(fd, buffer, size);
}
int oceanPoll(struct pollfd *items, nfds_t count, int timeout) {
    ULONGLONG started = GetTickCount64();
    for (;;) {
        int ready = 0;
        for (nfds_t i = 0; i < count; i++) {
            struct pollfd *p = &items[i]; p->revents = 0; if (p->fd < 0) continue;
            HANDLE handle = (HANDLE)_get_osfhandle(p->fd);
            if (handle == INVALID_HANDLE_VALUE) p->revents = POLLERR;
            else if (p->events & POLLIN) {
                DWORD available = 0, type = GetFileType(handle);
                if (type == FILE_TYPE_PIPE) {
                    if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) p->revents |= POLLHUP;
                    else if (available) p->revents |= POLLIN;
                } else if (type == FILE_TYPE_DISK || WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) p->revents |= POLLIN;
            }
            if (p->events & POLLOUT) p->revents |= POLLOUT;
            if (p->revents) ready++;
        }
        if (ready || (timeout >= 0 && GetTickCount64() - started >= (ULONGLONG)timeout)) return ready;
        Sleep(1);
    }
}
typedef struct memoryStream { FILE *file; char **text; size_t *length; struct memoryStream *next; } memoryStream;
static memoryStream *streams;
static int synchronize(memoryStream *s) {
    if (fflush(s->file)) return -1;
    long position = ftell(s->file); if (position < 0 || fseek(s->file, 0, SEEK_END)) return -1;
    long end = ftell(s->file); if (end < 0) return -1;
    char *text = realloc(*s->text, (size_t)end + 1); if (!text) return -1;
    *s->text = text; *s->length = (size_t)end;
    rewind(s->file); size_t n = fread(text, 1, (size_t)end, s->file); text[n] = 0;
    return n != (size_t)end || fseek(s->file, position, SEEK_SET) ? -1 : 0;
}
FILE *oceanTmpfile(void) {
    char path[4096]; oceanTemporary(path, sizeof(path), "ocean-stream-XXXXXX");
    if (_mktemp_s(path, strlen(path) + 1)) return NULL;
    int fd = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    if (fd < 0) return NULL;
    FILE *file = _fdopen(fd, "w+b"); if (!file) _close(fd); return file;
}
FILE *oceanMemoryStream(char **text, size_t *length) {
    FILE *file = oceanTmpfile(); if (!file) return NULL;
    memoryStream *s = malloc(sizeof(*s)); if (!s) { fclose(file); return NULL; }
    *text = calloc(1, 1); *length = 0;
    if (!*text) { fclose(file); free(s); return NULL; }
    *s = (memoryStream){file, text, length, streams}; streams = s; return file;
}
int oceanFlush(FILE *file) {
    int failed = fflush(file);
    for (memoryStream *s = streams; s; s = s->next) if (!file || file == s->file) if (synchronize(s)) failed = EOF;
    return failed;
}
int oceanClose(FILE *file) {
    int failed = 0;
    for (memoryStream **p = &streams; *p; p = &(*p)->next) if ((*p)->file == file) {
        memoryStream *s = *p; failed = synchronize(s); *p = s->next; free(s); break;
    }
    return fclose(file) || failed ? EOF : 0;
}
int oceanMkstemp(char *path) {
    if (_mktemp_s(path, strlen(path) + 1)) return -1;
    return _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
}
char *oceanMkdtemp(char *path) {
    for (int i = 0; i < 100; i++) {
        size_t length = strlen(path); if (length < 6) { errno = EINVAL; return NULL; }
        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        unsigned char random[6];
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return NULL;
        for (int j = 0; j < 6; j++) path[length - 6 + j] = alphabet[random[j] % 36];
        if (!_mkdir(path)) return path;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}
char *oceanRealpath(const char *path, char *result) {
    wchar_t *name = wide(path); if (!name) return NULL;
    HANDLE handle = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(name); if (handle == INVALID_HANDLE_VALUE) { errno = ENOENT; return NULL; }
    wchar_t resolved[32768]; DWORD n = GetFinalPathNameByHandleW(handle, resolved, 32768, FILE_NAME_NORMALIZED);
    CloseHandle(handle); if (!n || n >= 32768) return NULL;
    char *text = utf8(!wcsncmp(resolved, L"\\\\?\\", 4) ? resolved + 4 : resolved);
    if (!text) return NULL;
    for (char *p = text; *p; p++) if (*p == '\\') *p = '/';
    if (result) { strcpy(result, text); free(text); return result; } return text;
}
int oceanRename(const char *from, const char *to) {
    wchar_t *a = wide(from), *b = wide(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a); free(b); if (!ok) errno = EACCES; return ok ? 0 : -1;
}
int oceanUnlink(const char *path) { return _unlink(path); }
int oceanStat(const char *path, struct stat *info) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) { errno = ELOOP; return -1; }
    return stat(path, info);
}
char *oceanGetcwd(char *path, int capacity) {
    char *result = _getcwd(path, capacity);
    if (result) for (char *p = result; *p; p++) if (*p == '\\') *p = '/'; return result;
}
int oceanOpen(const char *path, int flags, ...) {
    int mode = _S_IREAD | _S_IWRITE;
    if (flags & _O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return _open(!strcmp(path, "/dev/null") ? "NUL" : path, flags | _O_BINARY, mode);
}
int oceanClock(int clock, struct timespec *out) {
    if (clock == CLOCK_REALTIME) return timespec_get(out, TIME_UTC) == TIME_UTC ? 0 : -1;
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count); QueryPerformanceFrequency(&frequency);
    out->tv_sec = count.QuadPart / frequency.QuadPart;
    out->tv_nsec = (long)((count.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart); return 0;
}
int oceanNanosleep(const struct timespec *delay, struct timespec *remaining) { (void)remaining; Sleep((DWORD)(delay->tv_sec * 1000 + (delay->tv_nsec + 999999) / 1000000)); return 0; }
int oceanUname(struct utsname *out) {
    strcpy(out->sysname, "Windows"); strcpy(out->release, "NT");
#if defined(__aarch64__)
    strcpy(out->machine, "aarch64");
#else
    strcpy(out->machine, "x86_64");
#endif
    return 0;
}
struct passwd *oceanGetpwuid(int uid) { static struct passwd value; (void)uid; value.pw_dir = getenv("USERPROFILE"); return &value; }
int oceanSetenv(const char *name, const char *value, int overwrite) { return !overwrite && getenv(name) ? 0 : _putenv_s(name, value); }
long long oceanGetline(char **line, size_t *capacity, FILE *file) {
    size_t size = 0; int c;
    while ((c = fgetc(file)) != EOF) {
        if (size + 2 > *capacity) { size_t n = *capacity ? *capacity * 2 : 128; char *p = realloc(*line, n); if (!p) return -1; *line = p; *capacity = n; }
        (*line)[size++] = (char)c; if (c == '\n') break;
    }
    if (!size) return -1; (*line)[size] = 0; return (long long)size;
}
char *oceanStrndup(const char *text, size_t n) { size_t size = strnlen(text, n); char *out = malloc(size + 1); if (out) { memcpy(out, text, size); out[size] = 0; } return out; }
const char *oceanStrsignal(int number) { (void)number; return "process terminated"; }
int oceanAlphasort(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
int oceanScandir(const char *path, struct dirent ***list, int (*filter)(const struct dirent *), int (*compare)(const struct dirent **, const struct dirent **)) {
    DIR *dir = opendir(path); if (!dir) return -1; int count = 0; struct dirent *item;
    *list = NULL;
    while ((item = readdir(dir))) {
        if (filter && !filter(item)) continue;
        struct dirent *copy = malloc(sizeof(*copy)); struct dirent **grown = realloc(*list, ((size_t)count + 1) * sizeof(**list));
        if (!copy || !grown) { free(copy); for (int i = 0; i < count; i++) free((*list)[i]); free(grown ? grown : *list); closedir(dir); return -1; }
        *list = grown; memcpy(copy, item, sizeof(*copy)); (*list)[count++] = copy;
    }
    closedir(dir); if (compare) qsort(*list, (size_t)count, sizeof(**list), (int (*)(const void *, const void *))compare); return count;
}
int oceanRemoveDirectory(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : 1;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return _unlink(path) != 0;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return _rmdir(path) != 0;
    DIR *dir = opendir(path); if (!dir) return 1; struct dirent *item; int failed = 0;
    while ((item = readdir(dir))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        char *child = malloc(strlen(path) + strlen(item->d_name) + 2);
        if (!child) { failed = 1; break; }
        sprintf(child, "%s/%s", path, item->d_name); failed |= oceanRemoveDirectory(child); free(child);
    }
    closedir(dir); return _rmdir(path) != 0 || failed;
}
