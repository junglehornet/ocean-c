/* Compiler-owned C ABI for os/exec. No shell is inserted on either platform. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char **environ;
#endif
#endif

static volatile sig_atomic_t interrupted;
#ifdef _WIN32
static BOOL WINAPI interrupted_console(DWORD event) { if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) { interrupted = 1; return TRUE; } return FALSE; }
#else
static void interrupted_signal(int number) { interrupted = number; }
#endif
void ocean_process_clear_interrupt(void) { interrupted = 0; }
int32_t ocean_process_interrupted(void) { return interrupted; }
void ocean_process_watch(void) {
#ifdef _WIN32
    SetConsoleCtrlHandler(interrupted_console, TRUE);
#else
    struct sigaction action = {0}; action.sa_handler = interrupted_signal;
    sigemptyset(&action.sa_mask); sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
#endif
}
typedef struct {
    const char *path;
    const char *const *argv;
    const char *directory;
    const char *const *environment;
    int64_t capture, combined, exit_code;
    char *output;
    int64_t output_size;
    const char *error;
} OceanProcess;
static int fail(OceanProcess *p, const char *text) { p->error = text; return 1; }
static int append(OceanProcess *p, const char *bytes, size_t count) {
    if (count > SIZE_MAX - (size_t)p->output_size - 1) return 0;
    char *out = realloc(p->output, (size_t)p->output_size + count + 1);
    if (!out) return 0;
    memcpy(out + p->output_size, bytes, count); p->output_size += (int64_t)count;
    out[p->output_size] = 0; p->output = out; return 1;
}
#ifdef _WIN32
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
char *ocean_process_argument(int64_t index) {
    int count = 0; wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return NULL;
    char *out = index >= 0 && index < count ? utf8(args[index]) : NULL;
    LocalFree(args); return out;
}
/* Quoting follows CommandLineToArgvW / the Microsoft CRT, including trailing \. */
static wchar_t *command_line(const char *const *argv) {
    size_t length = 1;
    for (size_t i = 0; argv[i]; i++) length += strlen(argv[i]) * 2 + 4;
    wchar_t *line = calloc(length, sizeof(*line)), *out = line;
    if (!line) return NULL;
    for (size_t i = 0; argv[i]; i++) {
        wchar_t *arg = wide(argv[i]); if (!arg) { free(line); return NULL; }
        if (i) *out++ = L' '; *out++ = L'"'; size_t slash = 0;
        for (const wchar_t *c = arg; ; c++) {
            if (*c == L'\\') { slash++; continue; }
            size_t n = (*c == L'"' || !*c) ? slash * 2 : slash;
            while (n--) *out++ = L'\\'; slash = 0;
            if (!*c) break;
            if (*c == L'"') *out++ = L'\\';
            *out++ = *c;
        }
        *out++ = L'"'; free(arg);
    }
    *out = 0; return line;
}
char *ocean_process_lookpath(const char *name) {
    wchar_t *input = wide(name), found[32768]; if (!input) return NULL;
    DWORD n = SearchPathW(NULL, input, L".exe", 32768, found, NULL); free(input);
    if (!n || n >= 32768 || (GetFileAttributesW(found) & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    /* Reject the implicit current-directory search, matching exec.LookPath. */
    if (!strchr(name, '/') && !strchr(name, '\\') && !strchr(name, ':')) {
        wchar_t cwd[32768]; GetCurrentDirectoryW(32768, cwd);
        wchar_t *slash = wcsrchr(found, L'\\');
        if (slash) { *slash = 0; int same = !_wcsicmp(found, cwd); *slash = L'\\'; if (same) return NULL; }
    }
    return utf8(found);
}
static int compare_env(const void *a, const void *b) { return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b); }
static HANDLE inherited(HANDLE original) {
    HANDLE result = INVALID_HANDLE_VALUE;
    if (!original || original == INVALID_HANDLE_VALUE || !DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &result, 0, TRUE, DUPLICATE_SAME_ACCESS)) return INVALID_HANDLE_VALUE;
    return result;
}
int32_t ocean_process_run(OceanProcess *p) {
    p->exit_code = -1; p->output = NULL; p->output_size = 0; p->error = NULL;
    char *resolved = ocean_process_lookpath(p->path);
    if (!resolved) return fail(p, "exec: executable not found");
    wchar_t *application = wide(resolved), *line = command_line(p->argv);
    wchar_t *directory = p->directory && *p->directory ? wide(p->directory) : NULL;
    free(resolved);
    if (!application || !line || (p->directory && *p->directory && !directory)) { free(application); free(line); free(directory); return fail(p, "exec: invalid UTF-8 or allocation failed"); }
    wchar_t *environment = NULL;
    if (p->environment) {
        size_t count = 2; for (size_t i = 0; p->environment[i]; i++) count += strlen(p->environment[i]) + 1;
        environment = calloc(count, sizeof(*environment));
        if (!environment) { free(application); free(line); free(directory); return fail(p, "exec: allocation failed"); }
        size_t total = 0; while (p->environment[total]) total++;
        wchar_t **entries = calloc(total ? total : 1, sizeof(*entries));
        if (!entries) { free(application); free(line); free(directory); free(environment); return fail(p, "exec: allocation failed"); }
        int valid = 1;
        for (size_t i = 0; i < total; i++) if (!(entries[i] = wide(p->environment[i]))) valid = 0;
        if (!valid) { for (size_t i = 0; i < total; i++) free(entries[i]); free(entries); free(application); free(line); free(directory); free(environment); return fail(p, "exec: invalid environment"); }
        qsort(entries, total, sizeof(*entries), compare_env);
        wchar_t *at = environment;
        for (size_t i = 0; i < total; i++) { size_t n = wcslen(entries[i])+1; memcpy(at,entries[i],n*sizeof(*at)); at += n; free(entries[i]); }
        free(entries);
    }
    HANDLE capture = INVALID_HANDLE_VALUE;
    if (p->capture) {
        wchar_t base[32768], path[32768];
        if (!GetTempPathW(32768, base) || !GetTempFileNameW(base, L"ocx", 0, path)) { free(application); free(line); free(directory); free(environment); return fail(p, "exec: cannot create output file"); }
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        capture = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (capture == INVALID_HANDLE_VALUE) { DeleteFileW(path); free(application); free(line); free(directory); free(environment); return fail(p, "exec: cannot open output file"); }
    }
    HANDLE handles[3] = {inherited(GetStdHandle(STD_INPUT_HANDLE)), p->capture ? capture : inherited(GetStdHandle(STD_OUTPUT_HANDLE)), p->combined ? capture : inherited(GetStdHandle(STD_ERROR_HANDLE))};
    STARTUPINFOEXW startup = {0}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0]; startup.StartupInfo.hStdOutput = handles[1]; startup.StartupInfo.hStdError = handles[2];
    HANDLE allowed[3]; size_t used = 0;
    for (int i = 0; i < 3; i++) if (handles[i] != INVALID_HANDLE_VALUE) { int duplicate = 0; for (size_t j = 0; j < used; j++) if (allowed[j] == handles[i]) duplicate = 1; if (!duplicate) allowed[used++] = handles[i]; }
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    startup.lpAttributeList = malloc(bytes);
    int initialized = startup.lpAttributeList && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes);
    int configured = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allowed, used * sizeof(HANDLE), NULL, NULL);
    PROCESS_INFORMATION process = {0};
    int created = configured && CreateProcessW(application, line, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment, directory, &startup.StartupInfo, &process);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList); free(startup.lpAttributeList);
    for (int i = 0; i < 3; i++) if (handles[i] != INVALID_HANDLE_VALUE && handles[i] != capture) CloseHandle(handles[i]);
    free(application); free(line); free(directory); free(environment);
    if (!created) { if (capture != INVALID_HANDLE_VALUE) CloseHandle(capture); return fail(p, "exec: executable could not be started"); }
    CloseHandle(process.hThread);
    DWORD code = 0;
    DWORD waiting;
    do { waiting = WaitForSingleObject(process.hProcess, 20); if (interrupted) TerminateProcess(process.hProcess, 130); } while (waiting == WAIT_TIMEOUT);
    int waited = waiting == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); p->exit_code = waited ? (int64_t)code : -1;
    int read_ok = 1;
    if (capture != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER zero = {0}; read_ok = SetFilePointerEx(capture, zero, NULL, FILE_BEGIN) != 0;
        char buffer[8192]; DWORD n;
        while (read_ok) { if (!ReadFile(capture, buffer, sizeof(buffer), &n, NULL)) { read_ok = 0; break; } if (!n) break; if (!append(p, buffer, n)) read_ok = 0; }
        CloseHandle(capture);
    }
    if (!waited) return fail(p, "exec: wait failed");
    if (!read_ok) return fail(p, "exec: output read failed");
    return code ? fail(p, "exec: process exited unsuccessfully") : 0;
}
#else
char *ocean_process_argument(int64_t index) { (void)index; return NULL; }
char *ocean_process_lookpath(const char *name) {
    const char *path = getenv("PATH"); if (!path) path = "/bin:/usr/bin";
    int explicit = strchr(name, '/') != NULL;
    for (const char *at = path; ; ) {
        const char *end = strchr(at, ':'); size_t n = end ? (size_t)(end - at) : strlen(at);
        char *candidate = explicit ? strdup(name) : malloc(n + strlen(name) + 3);
        if (!candidate) return NULL;
        if (!explicit) { memcpy(candidate, at, n); candidate[n] = '/'; strcpy(candidate + n + 1, name); if (!n) { free(candidate); candidate = malloc(strlen(name) + 3); if (!candidate) return NULL; sprintf(candidate, "./%s", name); } }
        if (!access(candidate, X_OK)) { DIR *dir = opendir(candidate); if (dir) closedir(dir); else { if (!explicit && candidate[0] != '/') { free(candidate); return NULL; } return candidate; } }
        free(candidate); if (explicit || !end) return NULL; at = end + 1;
    }
}
int32_t ocean_process_run(OceanProcess *p) {
    p->exit_code = -1; p->output = NULL; p->output_size = 0; p->error = NULL;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) return fail(p, "exec: cannot initialize spawn actions");
    FILE *file = p->capture ? tmpfile() : NULL;
    int failed = p->capture && !file;
    if (!failed && file) {
        failed = posix_spawn_file_actions_adddup2(&actions, fileno(file), 1);
        if (!failed && p->combined) failed = posix_spawn_file_actions_adddup2(&actions, fileno(file), 2);
    }
    if (!failed && p->directory && *p->directory) failed = posix_spawn_file_actions_addchdir_np(&actions, p->directory);
#ifdef __APPLE__
    char **env = p->environment ? (char **)p->environment : *_NSGetEnviron();
#else
    char **env = p->environment ? (char **)p->environment : environ;
#endif
    pid_t child = -1;
    if (!failed) failed = posix_spawnp(&child, p->path, &actions, NULL, (char *const *)p->argv, env);
    posix_spawn_file_actions_destroy(&actions);
    if (failed) { if (file) fclose(file); return fail(p, "exec: executable could not be started"); }
    int status;
    while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) { if (file) fclose(file); return fail(p, "exec: wait failed"); } if (interrupted) kill(child, SIGTERM); }
    p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int read_ok = 1;
    if (file) { rewind(file); char buffer[8192]; size_t n; while ((n = fread(buffer, 1, sizeof(buffer), file))) if (!append(p, buffer, n)) { read_ok = 0; break; } if (ferror(file)) read_ok = 0; fclose(file); }
    if (!read_ok) return fail(p, "exec: output read failed");
    return p->exit_code ? fail(p, "exec: process exited unsuccessfully") : 0;
}
#endif
