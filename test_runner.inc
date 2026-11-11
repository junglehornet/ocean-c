/* ocean test owns the native runner and reporting; test behavior lives in the
 * source standard library. A separate descriptor keeps arbitrary test stdout
 * from corrupting machine-readable results. */
#include <poll.h>
#include <stdarg.h>
#include <time.h>

typedef struct testResult {
    char *name, *output;
    size_t outputSize;
    int status; /* 0 running, 1 pass, 2 fail, 3 skip */
    double elapsed;
    struct testResult *next;
} testResult;
typedef struct testReporter {
    FILE *report, *jsonFile;
    const char *package;
    int json, color, includePassed, includeSkipped, passed, failed, skipped, completed;
    double timeout;
    testResult *results, **tail;
    char *packageOutput; size_t packageOutputSize;
} testReporter;
static double testClock(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static void testPrint(testReporter *r, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    char *text = NULL; size_t size = 0;
    FILE *buffer = open_memstream(&text, &size);
    if (!buffer) { va_end(a); return; }
    vfprintf(buffer, fmt, a); va_end(a); fclose(buffer);
    if (!r->json) fputs(text, stdout);
    if (r->report) {
        /* Reports stay plain even when the terminal uses color. */
        for (char *p = text; *p; p++) {
            if (*p == '\033' && p[1] == '[') { while (*p && *p != 'm') p++; if (!*p) break; }
            else fputc(*p, r->report);
        }
    }
    free(text);
}
static void testQuote(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default: if (*p < 32) fprintf(out, "\\u%04x", *p); else fputc(*p, out);
        }
    }
    fputc('"', out);
}
/* Events originate in testing.oc, with fixed fields and JSON string escaping. */
static char *testField(const char *line, const char *key) {
    char prefix[80]; snprintf(prefix, sizeof(prefix), "\"%s\":\"", key);
    const char *p = strstr(line, prefix);
    if (!p) return strdup("");
    p += strlen(prefix);
    char *value = malloc(strlen(p) + 1), *out = value;
    if (!value) return NULL;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'n') *out++ = '\n';
            else if (*p == 'r') *out++ = '\r';
            else if (*p == 't') *out++ = '\t';
            else if (*p == 'u' && strlen(p) >= 5) {
                unsigned c = 0; sscanf(p + 1, "%4x", &c); *out++ = (char)c; p += 4;
            } else if (*p) *out++ = *p;
            if (*p) p++;
        } else *out++ = *p++;
    }
    *out = 0; return value;
}
static void testJSON(testReporter *r, const char *action, const char *name, const char *output, double elapsed) {
    FILE *streams[] = {r->json ? stdout : NULL, r->jsonFile};
    struct timespec now; struct tm utc;
    clock_gettime(CLOCK_REALTIME, &now); gmtime_r(&now.tv_sec, &utc);
    char timestamp[40]; strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
    for (size_t i = 0; i < 2; i++) {
        FILE *out = streams[i]; if (!out) continue;
        fprintf(out, "{\"Time\":\"%s.%09ldZ\",\"Action\":", timestamp, now.tv_nsec); testQuote(out, action);
        fputs(",\"Package\":", out); testQuote(out, r->package);
        if (*name) { fputs(",\"Test\":", out); testQuote(out, name); }
        if (*output) { fputs(",\"Output\":", out); testQuote(out, output); }
        fprintf(out, ",\"Elapsed\":%.9f}\n", elapsed); fflush(out);
    }
}
static void testEvent(testReporter *r, const char *line) {
    char *action = testField(line, "Action"), *name = testField(line, "Test"), *output = testField(line, "Output");
    if (!action || !name || !output) { free(action); free(name); free(output); return; }
    const char *e = strstr(line, "\"Elapsed\":");
    double elapsed = e ? strtod(e + 10, NULL) : 0;
    if (!strcmp(action, "complete")) {
        r->completed = 1; free(action); free(name); free(output); return;
    }
    testJSON(r, action, name, output, elapsed);
    testResult *result = NULL;
    for (testResult *t = r->results; t; t = t->next) if (!strcmp(t->name, name)) { result = t; break; }
    if (*name && !strcmp(action, "run") && !result) {
        result = calloc(1, sizeof(*result));
        if (!result) exit(1);
        result->name = strdup(name); *r->tail = result; r->tail = &result->next;
    }
    if (result && !strcmp(action, "output")) {
        size_t n = strlen(output);
        char *grown = realloc(result->output, result->outputSize + n + 1);
        if (!grown) exit(1);
        result->output = grown;
        memcpy(result->output + result->outputSize, output, n + 1); result->outputSize += n;
    } else if (result && (!strcmp(action, "pass") || !strcmp(action, "fail") || !strcmp(action, "skip"))) {
        result->status = !strcmp(action, "pass") ? 1 : !strcmp(action, "fail") ? 2 : 3;
        result->elapsed = elapsed;
        if (result->status == 1) r->passed++;
        if (result->status == 2) r->failed++;
        if (result->status == 3) r->skipped++;
    } else if (!*name && (!strcmp(action, "pass") || !strcmp(action, "fail"))) r->completed = 1;
    free(action); free(name); free(output);
}
static void testPackageOutput(testReporter *r, const char *bytes) {
    size_t n = strlen(bytes);
    char *grown = realloc(r->packageOutput, r->packageOutputSize + n + 1);
    if (!grown) exit(1);
    r->packageOutput = grown;
    memcpy(grown + r->packageOutputSize, bytes, n + 1); r->packageOutputSize += n;
}
static void testOutputLines(testReporter *r, const char *text, int indent) {
    if (!text) return;
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n'); size_t n = end ? (size_t)(end - p) : strlen(p);
        testPrint(r, "%*s%.*s\n", indent, "", (int)n, p);
        p += n; if (*p == '\n') p++;
    }
}
static void testRule(testReporter *r, int count) { for (int i = 0; i < count; i++) testPrint(r, "─"); }
static void testDuration(double elapsed, char text[48]) {
    if (elapsed >= 1) snprintf(text, 48, "%.3fs", elapsed);
    else if (elapsed >= .001) snprintf(text, 48, "%.3fms", elapsed * 1e3);
    else if (elapsed >= .000001) snprintf(text, 48, "%.3fµs", elapsed * 1e6);
    else snprintf(text, 48, "%.0fns", elapsed * 1e9);
    char *dot = strchr(text, '.');
    if (dot) {
        char *end = dot + 1; while (*end >= '0' && *end <= '9') end++;
        char *trim = end; while (trim > dot + 1 && trim[-1] == '0') trim--;
        if (trim == dot + 1) trim = dot;
        memmove(trim, end, strlen(end) + 1);
    }
}
static int testDigits(int n) { int width = 1; while (n >= 10) { n /= 10; width++; } return width; }
static void testSummary(testReporter *r, double elapsed, int status) {
    int total = r->passed + r->failed + r->skipped;
    int widths[] = {testDigits(r->passed), testDigits(r->failed), testDigits(r->skipped), testDigits(total)};
    char duration[48]; testDuration(elapsed, duration);
    int timeWidth = (int)strlen(duration);
    int statusWidth = status ? 4 : 2;
    int nameWidth = (int)strlen(r->package);
    if (nameWidth + statusWidth + 4 < 13) nameWidth = 13 - statusWidth - 4;
    int countWidth = widths[0] + widths[1] + widths[2] + widths[3] + 9;
    if (nameWidth + statusWidth + 4 + countWidth + timeWidth + 4 > 150)
        nameWidth = 150 - statusWidth - 4 - countWidth - timeWidth - 4;
    if (nameWidth < 1) nameWidth = 1;
    int labelWidth = statusWidth + 4 + nameWidth;
    int lineWidth = labelWidth + countWidth + timeWidth + 4;
    int details = 0;
    if (r->packageOutputSize) {
        testPrint(r, "─── Messages "); testRule(r, lineWidth - 13);
        testPrint(r, "\n=== %s\n", r->package);
        testOutputLines(r, r->packageOutput, 4); details = 1;
    }
    for (testResult *t = r->results; t; t = t->next) {
        if (t->status == 1 && !r->includePassed) continue;
        if (t->status == 3 && !r->includeSkipped) continue;
        if (!details++) {
            testPrint(r, "─── Messages "); testRule(r, lineWidth - 13);
            testPrint(r, "\n=== %s\n", r->package);
        }
        int depth = 0; for (char *p = t->name; *p; p++) if (*p == '/') depth++;
        const char *word = t->status == 1 ? "PASS" : t->status == 3 ? "SKIP" : "FAIL";
        const char *color = r->color ? (t->status == 1 ? "\033[32m" : t->status == 3 ? "\033[33m" : "\033[31m") : "";
        testPrint(r, "%*s--- %s%s%s: %s (%.2fs)\n", 4 + depth * 4, "", color, word,
                  r->color ? "\033[0m" : "", t->name, t->elapsed);
        testOutputLines(r, t->output, 4 + depth * 4);
    }
    if (details) testPrint(r, "\n");
    if (status && (!r->completed || !r->failed)) testPrint(r, "✗ %s: test process exited before completing (status %d)\n", r->package, status);
    /* Match tang's static Summary table: zero package counts are blank,
     * totals include zeroes, count columns align with the symbol header. */
    for (int row = 0; row < 2; row++) {
        if (!row) { testPrint(r, "─── Summary "); testRule(r, labelWidth + widths[0] - 12); }
        else testRule(r, labelWidth + widths[0]);
        testPrint(r, " ✓ "); testRule(r, widths[1]);
        testPrint(r, " ✗ "); testRule(r, widths[2]);
        testPrint(r, " ∅ "); testRule(r, widths[3]);
        testPrint(r, " ∑ "); testRule(r, timeWidth > 1 ? timeWidth - 1 : 1);
        testPrint(r, " ⏱\n");
        if (!row) {
            const char *color = r->color ? (status ? "\033[31m" : "\033[32m") : "";
            testPrint(r, "%s%-*s%s    %-*.*s  ", color, statusWidth, status ? "FAIL" : "ok",
                      r->color ? "\033[0m" : "", nameWidth, nameWidth, r->package);
        } else testPrint(r, "%-*s  ", labelWidth, "(1 packages)");
        int counts[] = {r->passed, r->failed, r->skipped, total};
        for (int i = 0; i < 4; i++) {
            if (i) testPrint(r, "   ");
            if (!row && !counts[i]) testPrint(r, "%*s", widths[i], "");
            else testPrint(r, "%*d", widths[i], counts[i]);
        }
        testPrint(r, "  %*s\n", timeWidth, duration);
    }
}
static int testExecute(testReporter *r, const char *executable) {
    int events[2], output[2];
    if (pipe(events)) { perror("ocean test: pipe"); return 1; }
    if (pipe(output)) { close(events[0]); close(events[1]); perror("ocean test: pipe"); return 1; }
    double start = testClock();
    pid_t child = fork();
    if (!child) {
        close(events[0]); close(output[0]);
        if (dup2(events[1], 198) < 0 || dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0) _exit(127);
        close(events[1]); close(output[1]);
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        execl(executable, executable, (char *)NULL); _exit(127);
    }
    close(events[1]); close(output[1]);
    if (child < 0) { close(events[0]); close(output[0]); perror("ocean test: fork"); return 1; }
    runningChild = child;
    if (interrupted) kill(child, interrupted);
    int timedOut = 0;
    struct pollfd descriptors[] = {{events[0], POLLIN, 0}, {output[0], POLLIN, 0}};
    char *pending = NULL; size_t pendingSize = 0;
    int openCount = 2;
    while (openCount) {
        if (!timedOut && r->timeout > 0 && testClock() - start >= r->timeout) {
            timedOut = 1; kill(child, SIGKILL);
            testJSON(r, "output", "", "test timed out\n", testClock() - start);
            char message[100]; snprintf(message, sizeof(message), "test timed out after %.3fs\n", r->timeout);
            testPackageOutput(r, message);
        }
        if (poll(descriptors, 2, 100) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; i++) {
            if (descriptors[i].fd < 0 || !(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char bytes[4097]; ssize_t n = read(descriptors[i].fd, bytes, 4096);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(descriptors[i].fd); descriptors[i].fd = -1; openCount--; continue; }
            bytes[n] = 0;
            if (i) { testJSON(r, "output", "", bytes, 0); testPackageOutput(r, bytes); }
            else {
                char *grown = realloc(pending, pendingSize + (size_t)n + 1);
                if (!grown) exit(1); pending = grown;
                memcpy(pending + pendingSize, bytes, (size_t)n + 1); pendingSize += (size_t)n;
                char *end;
                while ((end = memchr(pending, '\n', pendingSize))) {
                    *end = 0; testEvent(r, pending);
                    size_t consumed = (size_t)(end - pending) + 1;
                    memmove(pending, pending + consumed, pendingSize - consumed);
                    pendingSize -= consumed; pending[pendingSize] = 0;
                }
            }
        }
    }
    free(pending);
    for (int i = 0; i < 2; i++) if (descriptors[i].fd >= 0) close(descriptors[i].fd);
    int status = 0; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    runningChild = 0;
    status = waited < 0 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (!r->completed && !status) status = 1;
    if (r->failed && !status) status = 1;
    testJSON(r, status ? "fail" : "pass", "", !r->completed ? "test process did not complete" : "", testClock() - start);
    testSummary(r, testClock() - start, status);
    return status;
}
static int testMain(int argc, char **argv) {
    const char *location = ".", *reportPath = NULL, *jsonPath = NULL;
    int haveLocation = 0;
    testReporter reporter = {.includeSkipped = 1, .timeout = 600}; reporter.tail = &reporter.results;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            puts("Usage: ocean test [location] [--json] [--outfile path] [--jsonfile path]\n"
                 "                  [--include-passed | -v] [--no-color] [--hide-skipped]\n"
                 "                  [--timeout duration] (default 10m; 0 disables)\n"
                 "Run Test*(t *testing.T) functions in _test.oc files.\n"
                 "--json emits newline-delimited test events; --jsonfile saves them.\n"
                 "--outfile (or -o) saves the plain tang-style report. NO_COLOR disables color.");
            return 0;
        } else if (!strcmp(arg, "--timeout") || !strcmp(arg, "-timeout")) {
            if (++i == argc) { fputs("ocean test: --timeout requires a duration\n", stderr); return 1; }
            char *end; errno = 0;
            reporter.timeout = strtod(argv[i], &end);
            if (!strcmp(end, "ms")) reporter.timeout /= 1000;
            else if (!strcmp(end, "m")) reporter.timeout *= 60;
            else if (!strcmp(end, "s") || !*end) { }
            else { fputs("ocean test: timeout units must be ms, s, or m\n", stderr); return 1; }
            if (errno || end == argv[i] || !(reporter.timeout >= 0) || reporter.timeout > 31536000) {
                fputs("ocean test: invalid timeout\n", stderr); return 1;
            }
        } else if (!strcmp(arg, "--json") || !strcmp(arg, "-json")) reporter.json = 1;
        else if (!strcmp(arg, "--include-passed") || !strcmp(arg, "-v")) reporter.includePassed = 1;
        else if (!strcmp(arg, "--hide-skipped")) reporter.includeSkipped = 0;
        else if (!strcmp(arg, "--no-color")) reporter.color = -1;
        else if (!strcmp(arg, "--outfile") || !strcmp(arg, "-outfile") || !strcmp(arg, "-o") ||
                 !strcmp(arg, "--jsonfile") || !strcmp(arg, "-jsonfile")) {
            if (++i == argc) { fprintf(stderr, "ocean test: %s requires a path\n", arg); return 1; }
            if (strstr(arg, "jsonfile")) jsonPath = argv[i]; else reportPath = argv[i];
        } else if (*arg == '-') { fprintf(stderr, "ocean test: unknown option: %s\n", arg); return 1; }
        else if (haveLocation++) { fputs("ocean test accepts one package location\n", stderr); return 1; }
        else location = arg;
    }
    reporter.color = !reporter.json && reporter.color != -1 && !getenv("NO_COLOR") && isatty(STDOUT_FILENO);
    if (reportPath && jsonPath && (!strcmp(reportPath, jsonPath) || sameFile(reportPath, jsonPath))) {
        fputs("ocean test: report and JSON output must be different files\n", stderr); return 1;
    }
    char directory[] = "/tmp/.ocean-test-XXXXXX";
    if (!mkdtemp(directory)) { perror("ocean test: temporary directory"); return 1; }
    char executable[sizeof(directory) + 16]; snprintf(executable, sizeof(executable), "%s/program", directory);
    packageBuild *build = loadTestPackage(location, directory);
    parseResult *program = packageProgram(build);
    int failed = !program;
    if (!failed && (validateBackendTarget() || backendWasm())) {
        fputs("ocean test requires a native target\n", stderr); failed = 1;
    }
    if (!failed) failed = analyzeSemantics(program).nErrors != 0;
    /* Refuse report paths that would destroy compiler input. */
    for (size_t i = 0; !failed && i < program->nSources; i++)
        if ((reportPath && sameFile(reportPath, program->sources[i].filename)) ||
            (jsonPath && sameFile(jsonPath, program->sources[i].filename))) {
            fputs("ocean test: output would overwrite an input file\n", stderr); failed = 1;
        }
    if (!failed) failed = nativeBuild(NULL, program, executable, 0, 0);
    if (!failed && reportPath && !(reporter.report = fopen(reportPath, "w"))) { perror("ocean test: report file"); failed = 1; }
    if (!failed && jsonPath && !(reporter.jsonFile = fopen(jsonPath, "w"))) { perror("ocean test: JSON file"); failed = 1; }
    char *packagePath = realpath(location, NULL);
    if (packagePath) {
        struct stat info;
        if (!stat(packagePath, &info) && !S_ISDIR(info.st_mode)) {
            char *slash = strrchr(packagePath, '/');
            if (slash && slash != packagePath) *slash = 0;
        }
    }
    reporter.package = packagePath ? packagePath : location;
    if (!failed) {
        struct sigaction action = {0}, oldInt, oldTerm, oldHup;
        action.sa_handler = forwardSignal; sigemptyset(&action.sa_mask);
        interrupted = runningChild = 0;
        sigaction(SIGINT, &action, &oldInt); sigaction(SIGTERM, &action, &oldTerm); sigaction(SIGHUP, &action, &oldHup);
        failed = testExecute(&reporter, executable);
        sigaction(SIGINT, &oldInt, NULL); sigaction(SIGTERM, &oldTerm, NULL); sigaction(SIGHUP, &oldHup, NULL);
    }
    if (reporter.report) {
        int ioFailed = ferror(reporter.report);
        if (fclose(reporter.report)) ioFailed = 1;
        if (ioFailed) { fputs("ocean test: could not write report file\n", stderr); failed = 1; }
    }
    if (reporter.jsonFile) {
        int ioFailed = ferror(reporter.jsonFile);
        if (fclose(reporter.jsonFile)) ioFailed = 1;
        if (ioFailed) { fputs("ocean test: could not write JSON file\n", stderr); failed = 1; }
    }
    if (fflush(stdout) || ferror(stdout)) failed = 1;
    while (reporter.results) { testResult *t = reporter.results; reporter.results = t->next; free(t->name); free(t->output); free(t); }
    free(reporter.packageOutput);
    free(packagePath); freePackage(build); unlink(executable); rmdir(directory);
    return failed;
}
