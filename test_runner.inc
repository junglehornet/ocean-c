/* ocean test owns the native runner and reporting; test behavior lives in the
 * source standard library. A separate descriptor keeps arbitrary test stdout
 * from corrupting machine-readable results. */
#include <poll.h>
#include <dirent.h>
#include <stdarg.h>
#include <time.h>
#include <termios.h>

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
    int json, color, includePassed, includeSkipped, passed, failed, skipped, completed, workers;
    double timeout;
    testResult *results, **tail;
    char *packageOutput; size_t packageOutputSize;
    int displayFD, displayInterrupted, terminalSaved;
    int grantFD, heldSlots, waitingSlots;
    pid_t displayPID;
    struct termios savedTerminal;
    char *displayPending; size_t displayPendingSize, displayPendingOffset;
} testReporter;
static int multiMode, multiLive, multiTextOpened, multiJSONOpened, sharedBudget;
static testReporter multiUI;
/* The frontend owns all terminal rendering through term/tui. This bridge only
 * transports events and restores the terminal if the frontend crashes. */
static void testFlushDisplay(testReporter *r) {
    if (multiMode && r != &multiUI) r = &multiUI;
    while (r->displayFD >= 0 && r->displayPendingOffset < r->displayPendingSize) {
        ssize_t n = write(r->displayFD, r->displayPending + r->displayPendingOffset,
                          r->displayPendingSize - r->displayPendingOffset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { close(r->displayFD); r->displayFD = -1; break; }
        r->displayPendingOffset += (size_t)n;
    }
    if (r->displayFD < 0 || r->displayPendingOffset == r->displayPendingSize) {
        free(r->displayPending); r->displayPending = NULL;
        r->displayPendingSize = r->displayPendingOffset = 0;
    }
}
static void testDisplaySend(testReporter *r, const char *action, const char *name, const char *output, double elapsed) {
    const char *package = r->package;
    if (multiMode && r != &multiUI) r = &multiUI;
    if (r->displayFD < 0 && !(multiMode && multiLive && !r->displayInterrupted)) return;
    char *frame = NULL; size_t length = 0;
    FILE *stream = open_memstream(&frame, &length);
    if (!stream) return;
    fprintf(stream, "%s\t%.9f\t", action, elapsed);
    const char *fields[] = {name, output};
    for (size_t i = 0; i < 2; i++) {
        for (const unsigned char *p = (const unsigned char *)fields[i]; *p; p++) fprintf(stream, "%02x", *p);
        fputc(i ? (multiMode ? '\t' : '\n') : '\t', stream);
    }
    if (multiMode) {
        for (const unsigned char *p = (const unsigned char *)(package ? package : ""); *p; p++) fprintf(stream, "%02x", *p);
        fputc('\n', stream);
    }
    fclose(stream);
    size_t remaining = r->displayPendingSize - r->displayPendingOffset;
    if (remaining && r->displayPendingOffset) memmove(r->displayPending, r->displayPending + r->displayPendingOffset, remaining);
    char *pending = realloc(r->displayPending, remaining + length);
    if (!pending) { free(frame); return; }
    r->displayPending = pending;
    memcpy(pending + remaining, frame, length);
    r->displayPendingSize = remaining + length; r->displayPendingOffset = 0;
    if (r->displayFD >= 0) testFlushDisplay(r);
    free(frame);
}
static void testRestoreTerminal(testReporter *r) {
    if (r->terminalSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &r->savedTerminal);
        r->terminalSaved = 0;
    }
}
static void testCheckDisplay(testReporter *r) {
    if (multiMode && r != &multiUI) r = &multiUI;
    if (r->displayPID <= 0) return;
    int status = 0;
    pid_t result = waitpid(r->displayPID, &status, WNOHANG);
    if (result != r->displayPID) return;
    r->displayPID = 0;
    if (r->displayFD >= 0) { close(r->displayFD); r->displayFD = -1; }
    testRestoreTerminal(r);
    if (WIFSIGNALED(status)) {
        const char visible[] = "\033[?25h";
        write(STDOUT_FILENO, visible, sizeof(visible) - 1);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 130) {
        r->displayInterrupted = 1;
        if (runningChild > 0) kill((pid_t)runningChild, SIGINT);
    }
}
static void testStartDisplay(testReporter *r, const char *executable, int events[2], int output[2]) {
    if (multiMode && r != &multiUI) r = &multiUI;
    if (!executable || r->displayPID > 0 || r->displayInterrupted) return;
    int input[2];
    if (pipe(input)) { perror("ocean test: live display pipe"); return; }
    r->terminalSaved = tcgetattr(STDIN_FILENO, &r->savedTerminal) == 0;
    pid_t child = fork();
    if (!child) {
        close(input[1]);
        close(events[0]); close(events[1]); close(output[0]); close(output[1]);
        if (dup2(input[0], 197) < 0) _exit(2);
        if (input[0] != 197) close(input[0]);
        /* Signals sent to the compiler are forwarded to the test process.
         * The UI shuts down via its event channel, allowing tui to restore it. */
        signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
        execl(executable, executable, r->package, r->color ? "1" : "0", r->includeSkipped ? "1" : "0", (char *)NULL);
        _exit(2);
    }
    close(input[0]);
    if (child < 0) { close(input[1]); perror("ocean test: live display fork"); return; }
    r->displayFD = input[1]; r->displayPID = child;
    fcntl(r->displayFD, F_SETFD, FD_CLOEXEC);
    fcntl(r->displayFD, F_SETFL, O_NONBLOCK);
}
static void testStopDisplay(testReporter *r, double elapsed, int status) {
    if (multiMode && r != &multiUI) return;
    if (r->displayPID <= 0) return;
    testDisplaySend(r, "done", "", status ? "1" : "0", elapsed);
    for (int attempts = 0; attempts < 200 && r->displayFD >= 0 && r->displayPendingSize; attempts++) {
        struct pollfd output = {r->displayFD, POLLOUT, 0};
        poll(&output, 1, 10); testFlushDisplay(r); testCheckDisplay(r);
    }
    if (r->displayFD >= 0) { close(r->displayFD); r->displayFD = -1; }
    for (int attempts = 0; attempts < 200 && r->displayPID > 0; attempts++) {
        testCheckDisplay(r);
        if (r->displayPID > 0) { struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL); }
    }
    if (r->displayPID > 0) {
        /* A crashed/stuck renderer must not strand raw terminal settings. */
        kill(r->displayPID, SIGKILL);
        int childStatus; while (waitpid(r->displayPID, &childStatus, 0) < 0 && errno == EINTR) { }
        r->displayPID = 0;
        const char visible[] = "\033[?25h";
        write(STDOUT_FILENO, visible, sizeof(visible) - 1);
    }
    testRestoreTerminal(r);
}

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
    if (!r->json && !multiLive) fputs(text, stdout);
    if (multiLive && multiUI.report) fputs(text, multiUI.report);
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
    testDisplaySend(r, action, name, output, elapsed);
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
static void testGrant(testReporter *r) {
    while (r->waitingSlots > 0 && r->heldSlots < r->workers) {
        if (write(r->grantFD, "+", 1) != 1) break;
        r->waitingSlots--; r->heldSlots++;
    }
}
static void testEvent(testReporter *r, const char *line) {
    char *action = testField(line, "Action"), *name = testField(line, "Test"), *output = testField(line, "Output");
    if (!action || !name || !output) { free(action); free(name); free(output); return; }
    if (!strcmp(action, "acquire") || !strcmp(action, "release")) {
        if (sharedBudget) testJSON(r, action, "", "", 0);
        else {
            if (!strcmp(action, "acquire")) r->waitingSlots++;
            else if (r->heldSlots > 0) r->heldSlots--;
            testGrant(r);
        }
        free(action); free(name); free(output); return;
    }
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
static int testExecute(testReporter *r, const char *executable, const char *displayExecutable) {
    int events[2], output[2], grants[2] = {-1, -1};
    if (!sharedBudget && pipe(grants)) { perror("ocean test: worker slots"); return 1; }
    if (pipe(events)) { perror("ocean test: pipe"); return 1; }
    if (pipe(output)) { close(events[0]); close(events[1]); perror("ocean test: pipe"); return 1; }
    if (multiMode) { multiUI.color = r->color; multiUI.includeSkipped = r->includeSkipped; }
    testStartDisplay(r, displayExecutable, events, output);
    testReporter *ui = multiMode ? &multiUI : r;
    double start = testClock();
    pid_t child = fork();
    if (!child) {
        close(events[0]); close(output[0]);
        if (!sharedBudget) {
            close(grants[1]);
            if (dup2(grants[0], 199) < 0) _exit(127);
            if (grants[0] != 199) close(grants[0]);
            fcntl(199, F_SETFL, O_NONBLOCK);
        }
        if (ui->displayFD >= 0) {
            close(ui->displayFD);
            int empty = open("/dev/null", O_RDONLY);
            if (empty >= 0) { dup2(empty, STDIN_FILENO); close(empty); }
        }
        signal(SIGPIPE, SIG_DFL);
        if (dup2(events[1], 198) < 0 || dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0) _exit(127);
        close(events[1]); close(output[1]);
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        char workers[32]; snprintf(workers, sizeof(workers), "%d", r->workers);
        execl(executable, executable, "--ocean-test-workers", workers, (char *)NULL); _exit(127);
    }
    close(events[1]); close(output[1]);
    if (!sharedBudget) { close(grants[0]); r->grantFD = grants[1]; fcntl(r->grantFD, F_SETFL, O_NONBLOCK); }
    if (child < 0) { close(events[0]); close(output[0]); testStopDisplay(r, 0, 1); perror("ocean test: fork"); return 1; }
    runningChild = child;
    if (interrupted) kill(child, interrupted);
    int timedOut = 0;
    struct pollfd descriptors[] = {{events[0], POLLIN, 0}, {output[0], POLLIN, 0}, {-1, 0, 0}};
    char *pending = NULL; size_t pendingSize = 0;
    int openCount = 2;
    while (openCount) {
        if (!sharedBudget) testGrant(r);
        testCheckDisplay(r);
        if (!timedOut && r->timeout > 0 && testClock() - start >= r->timeout) {
            timedOut = 1; kill(child, SIGKILL);
            testJSON(r, "output", "", "test timed out\n", testClock() - start);
            char message[100]; snprintf(message, sizeof(message), "test timed out after %.3fs\n", r->timeout);
            testPackageOutput(r, message);
        }
        descriptors[2].fd = ui->displayFD;
        descriptors[2].events = ui->displayPendingSize ? POLLOUT : 0;
        if (poll(descriptors, 3, 100) < 0) { if (errno == EINTR) continue; break; }
        if (descriptors[2].revents & (POLLOUT | POLLHUP | POLLERR)) testFlushDisplay(r);
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
    if (r->grantFD >= 0) { close(r->grantFD); r->grantFD = -1; }
    status = waited < 0 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (!r->completed && !status) status = 1;
    if (r->failed && !status) status = 1;
    testStopDisplay(r, testClock() - start, status);
    if (ui->displayInterrupted) status = 130;
    testJSON(r, status ? "fail" : "pass", "", !r->completed ? "test process did not complete" : "", testClock() - start);
    testSummary(r, testClock() - start, status);
    return status;
}
static int testOneMain(int argc, char **argv) {
    const char *location = ".", *reportPath = NULL, *jsonPath = NULL;
    int haveLocation = 0, noTTY = 0;
    testReporter reporter = {.includeSkipped = 1, .timeout = 600, .displayFD = -1, .workers = 16, .grantFD = -1}; reporter.tail = &reporter.results;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            puts("Usage: ocean test [location] [--json] [--outfile path] [--jsonfile path]\n"
                 "                  [--include-passed | -v] [--no-color] [--hide-skipped]\n"
                 "                  [--timeout duration] [--notty] [-t nthreads | --single-threaded]\n"
                 "Run Test*(t *testing.T) functions in _test.oc files.\n"
                 "--json emits newline-delimited test events; --jsonfile saves them.\n"
                 "--outfile (or -o) saves the plain tang-style report. NO_COLOR disables color.\n"
                 "Interactive terminals use the term/tui live display; --notty disables it.\n"
                 "Timeout defaults to 10m; 0 disables it. The command-wide worker limit defaults to 16.\n"
                 "-t sets the shared worker limit; --single-threaded sets it to 1. Parallel tests obey this limit.");
            return 0;
        } else if (!strcmp(arg, "--single-threaded")) reporter.workers = 1;
        else if (!strcmp(arg, "-t")) {
            if (++i == argc) { fputs("ocean test: -t requires a positive thread count\n", stderr); return 1; }
            char *end; errno = 0; long count = strtol(argv[i], &end, 10);
            if (errno || !*argv[i] || *end || count < 1 || count > 2147483647L) {
                fputs("ocean test: -t requires a positive thread count (1..2147483647)\n", stderr); return 1;
            }
            reporter.workers = (int)count;
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
        else if (!strcmp(arg, "--notty") || !strcmp(arg, "-notty") || !strcmp(arg, "--no-tui")) noTTY = 1;
        else if (!strcmp(arg, "--no-color")) reporter.color = -1;
        else if (!strcmp(arg, "--outfile") || !strcmp(arg, "-outfile") || !strcmp(arg, "-o") ||
                 !strcmp(arg, "--jsonfile") || !strcmp(arg, "-jsonfile")) {
            if (++i == argc) { fprintf(stderr, "ocean test: %s requires a path\n", arg); return 1; }
            if (strstr(arg, "jsonfile")) jsonPath = argv[i]; else reportPath = argv[i];
        } else if (*arg == '-') { fprintf(stderr, "ocean test: unknown option: %s\n", arg); return 1; }
        else if (haveLocation++) { fputs("ocean test accepts one package location\n", stderr); return 1; }
        else location = arg;
    }
    const char *terminal = getenv("TERM");
    int live = !noTTY && !reporter.json && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
               (!terminal || strcmp(terminal, "dumb"));
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
    char displayExecutable[sizeof(directory) + 16];
    snprintf(displayExecutable, sizeof(displayExecutable), "%s/display", directory);
    if (!failed && live && (!multiMode || multiUI.displayPID <= 0)) {
        packageBuild *displayBuild = loadTestDisplay();
        parseResult *displayProgram = packageProgram(displayBuild);
        int displayFailed = !displayProgram || analyzeSemantics(displayProgram).nErrors != 0;
        buildCache *cache = displayFailed ? NULL : openBuildCache(argv[0], displayProgram, packageDirectory(displayBuild));
        if (!displayFailed) displayFailed = nativeBuild(cache, displayProgram, displayExecutable, 0, 0);
        closeBuildCache(cache); freePackage(displayBuild);
        if (displayFailed) { fputs("ocean test: live display unavailable; using final report\n", stderr); live = 0; }
    }
    if (!failed && reportPath && !(reporter.report = fopen(reportPath, multiMode && multiTextOpened ? "a" : "w"))) { perror("ocean test: report file"); failed = 1; }
    if (!failed && jsonPath && !(reporter.jsonFile = fopen(jsonPath, multiMode && multiJSONOpened ? "a" : "w"))) { perror("ocean test: JSON file"); failed = 1; }
    if (reporter.report) multiTextOpened = 1;
    if (reporter.jsonFile) multiJSONOpened = 1;
    char *packagePath = multiMode ? packageTestPath(location) : realpath(location, NULL);
    if (packagePath && !multiMode) {
        struct stat info;
        if (!stat(packagePath, &info) && !S_ISDIR(info.st_mode)) {
            char *slash = strrchr(packagePath, '/');
            if (slash && slash != packagePath) *slash = 0;
        }
    }
    reporter.package = packagePath ? packagePath : location;
    int ran = 0;
    if (!failed) {
        struct sigaction action = {0}, oldInt, oldTerm, oldHup, oldPipe, ignorePipe = {0};
        action.sa_handler = forwardSignal; sigemptyset(&action.sa_mask);
        interrupted = runningChild = 0;
        sigaction(SIGINT, &action, &oldInt); sigaction(SIGTERM, &action, &oldTerm); sigaction(SIGHUP, &action, &oldHup);
        ignorePipe.sa_handler = SIG_IGN; sigemptyset(&ignorePipe.sa_mask);
        sigaction(SIGPIPE, &ignorePipe, &oldPipe);
        ran = 1;
        failed = testExecute(&reporter, executable, live ? displayExecutable : NULL);
        sigaction(SIGPIPE, &oldPipe, NULL);
        sigaction(SIGINT, &oldInt, NULL); sigaction(SIGTERM, &oldTerm, NULL); sigaction(SIGHUP, &oldHup, NULL);
    }
    if (multiMode && !ran) {
        testJSON(&reporter, "fail", "", "package could not be compiled", 0);
        testPrint(&reporter, "FAIL %s: package could not be compiled\n", reporter.package);
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
    free(reporter.packageOutput); free(reporter.displayPending);
    free(packagePath); freePackage(build); unlink(executable); unlink(displayExecutable); rmdir(directory);
    return failed;
}

typedef struct testLocations { char **items; size_t count; int failed; } testLocations;
static void testAddLocation(testLocations *list, const char *path) {
    char *resolved = packageTestLocation(path);
    if (!resolved) { perror(path); list->failed = 1; return; }
    struct stat info;
    if (!stat(resolved, &info) && !S_ISDIR(info.st_mode)) {
        size_t n = strlen(resolved);
        if (n < 3 || strcmp(resolved + n - 3, ".oc")) {
            fprintf(stderr, "ocean test: expected an Ocean file or package directory: %s\n", path);
            free(resolved); list->failed = 1; return;
        }
        char *slash = strrchr(resolved, '/'); if (slash) *slash = 0;
    }
    for (size_t i = 0; i < list->count; i++) if (!strcmp(list->items[i], resolved)) { free(resolved); return; }
    char **grown = realloc(list->items, (list->count + 1) * sizeof(*grown));
    if (!grown) exit(1);
    list->items = grown; list->items[list->count++] = resolved;
}
static void testFindLocations(testLocations *list, const char *path) {
    struct dirent **entries = NULL;
    int count = scandir(path, &entries, NULL, alphasort), sources = 0;
    if (count < 0) { perror(path); list->failed = 1; return; }
    for (int i = 0; i < count; i++) {
        const char *name = entries[i]->d_name; size_t n = strlen(name);
        if (n > 3 && !strcmp(name + n - 3, ".oc")) sources = 1;
    }
    if (sources) testAddLocation(list, path);
    for (int i = 0; i < count; i++) {
        const char *name = entries[i]->d_name;
        if (*name != '.' && strcmp(name, "vendor") && strcmp(name, "node_modules")) {
            char *child = malloc(strlen(path) + strlen(name) + 2);
            sprintf(child, "%s/%s", path, name);
            struct stat info;
            if (!lstat(child, &info) && S_ISDIR(info.st_mode)) testFindLocations(list, child);
            free(child);
        }
        free(entries[i]);
    }
    free(entries);
}
#include "test_packages.inc"

static int testMain(int argc, char **argv) {
    int count = 0, recursive = 0;
    const char *report = NULL, *jsonFile = NULL;
    char **flags = calloc((size_t)argc + 2, sizeof(*flags)); int nflags = 2;
    flags[0] = argv[0]; flags[1] = argv[1];
    testLocations list = {0};
    int json = 0, notty = 0;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            puts("Usage: ocean test [packages...] [options]\n"
                 "Locations may be directories or files; ./... and engine/... recursively select packages.\n"
                 "Overlapping selections run once. Packages share the configured worker limit.\n"
                 "Options: --json --outfile path --jsonfile path -v --no-color --hide-skipped\n"
                 "         --timeout duration --notty -t nthreads --single-threaded");
            free(flags); return 0;
        }
        if (*arg == '-') {
            flags[nflags++] = argv[i];
            if (!strcmp(arg, "--json") || !strcmp(arg, "-json")) json = 1;
            if (!strcmp(arg, "--notty") || !strcmp(arg, "-notty") || !strcmp(arg, "--no-tui")) notty = 1;
            int value = !strcmp(arg, "-t") || !strcmp(arg, "--timeout") || !strcmp(arg, "-timeout") ||
                !strcmp(arg, "--outfile") || !strcmp(arg, "-outfile") || !strcmp(arg, "-o") ||
                !strcmp(arg, "--jsonfile") || !strcmp(arg, "-jsonfile");
            if (value && i + 1 < argc) {
                flags[nflags++] = argv[++i];
                if (strstr(arg, "jsonfile")) jsonFile = argv[i];
                else if (!strcmp(arg, "-o") || strstr(arg, "outfile")) report = argv[i];
            }
            continue;
        }
        count++;
        size_t n = strlen(arg);
        if (!strcmp(arg, "...") || (n >= 4 && !strcmp(arg + n - 4, "/..."))) {
            recursive = 1;
            char *base = !strcmp(arg, "...") ? strdup(".") : strndup(arg, n - 4);
            char *resolved = packageTestLocation(*base ? base : "/");
            if (resolved) { testFindLocations(&list, resolved); free(resolved); }
            else { fprintf(stderr, "ocean test: package not found: %s\n", base); list.failed = 1; }
            free(base);
        } else testAddLocation(&list, arg);
    }
    if (count == 0) {
        for (size_t i = 0; i < list.count; i++) free(list.items[i]);
        free(list.items); free(flags); return testOneMain(argc, argv);
    }
    if (count == 1 && !recursive && !list.failed && list.count == 1) {
        flags[nflags] = list.items[0];
        int result = testOneMain(nflags + 1, flags);
        free(list.items[0]); free(list.items); free(flags); return result;
    }
    if (list.failed || !list.count) {
        if (!list.count) fputs("ocean test: no packages matched\n", stderr);
        for (size_t i = 0; i < list.count; i++) free(list.items[i]);
        free(list.items); free(flags); return 1;
    }
    // Protect sources in later packages before any report gets truncated.
    const char *outputs[] = {report, jsonFile};
    for (int i = 0; i < 2; i++) if (outputs[i]) {
        size_t n = strlen(outputs[i]);
        if (n >= 3 && !strcmp(outputs[i] + n - 3, ".oc")) {
            fputs("ocean test: output would overwrite an Ocean source file\n", stderr); list.failed = 1;
        }
    }
    int status = list.failed ? 1 : testPackages(argc, flags, nflags, &list, report, jsonFile, json, notty);
    for (size_t i = 0; i < list.count; i++) free(list.items[i]);
    free(list.items); free(flags); return status;
}
