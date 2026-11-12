/* Package supervisors run independently. Only this process writes public
 * results and grants execution slots, so -t is a command-wide bound. */
typedef struct packageRun {
    pid_t pid;
    int output, errors, grant, exited, status, finished, held, pending, finalFailed;
    char *buffer; size_t length;
    double started, elapsed;
    testReporter reporter;
} packageRun;
static void packageSummary(packageRun *runs, size_t count, double elapsed) {
    if (!count) return;
    testReporter *out = &runs[0].reporter;
    int totals[4] = {0}, widths[4] = {1,1,1,1}, nameWidth = 5, statusWidth = 2;
    char totalTime[48]; testDuration(elapsed, totalTime);
    int timeWidth = (int)strlen(totalTime);
    for (size_t i = 0; i < count; i++) {
        testReporter *r = &runs[i].reporter;
        int n = (int)strlen(r->package); if (n > nameWidth) nameWidth = n;
        if (runs[i].status) statusWidth = 4;
        int values[] = {r->passed, r->failed, r->skipped, r->passed+r->failed+r->skipped};
        for (int j = 0; j < 4; j++) totals[j] += values[j];
        char duration[48]; testDuration(runs[i].elapsed > 0 ? runs[i].elapsed : elapsed, duration);
        if ((int)strlen(duration) > timeWidth) timeWidth = (int)strlen(duration);
    }
    for (int j = 0; j < 4; j++) widths[j] = testDigits(totals[j]);
    char label[64]; snprintf(label, sizeof(label), "(%zu packages)", count);
    int minLabel = (int)strlen(label); if (minLabel < 13) minLabel = 13;
    if (nameWidth + statusWidth + 4 < minLabel) nameWidth = minLabel - statusWidth - 4;
    int countsWidth = widths[0]+widths[1]+widths[2]+widths[3]+9;
    int maxName = 150 - statusWidth - 4 - countsWidth - timeWidth - 4;
    if (maxName < 1) maxName = 1;
    if (nameWidth > maxName) nameWidth = maxName;
    int labelWidth = statusWidth + 4 + nameWidth;
    int lineWidth = labelWidth + countsWidth + timeWidth + 4;
    for (size_t i = 0; i < count; i++) testDetails(&runs[i].reporter, lineWidth, runs[i].status);
    for (size_t row = 0; row <= count; row++) {
        if (row == 0 || row == count) {
            if (!row) { testPrint(out, "─── Summary "); testRule(out, labelWidth + widths[0] - 12); }
            else testRule(out, labelWidth + widths[0]);
            const char *icons[] = {"✓", "✗", "∅", "∑"};
            for (int j = 0; j < 4; j++) {
                testPrint(out, " %s ", icons[j]);
                testRule(out, j == 3 ? (timeWidth > 1 ? timeWidth-1 : 1) : widths[j+1]);
            }
            testPrint(out, " ⏱\n");
        }
        int values[4]; char duration[48];
        if (row < count) {
            packageRun *p = &runs[row]; testReporter *r = &p->reporter;
            const char *color = out->color ? (p->status ? "\033[31m" : "\033[32m") : "";
            testPrint(out, "%s%-*s%s    %-*.*s  ", color, statusWidth, p->status ? "FAIL" : "ok",
                      out->color ? "\033[0m" : "", nameWidth, nameWidth, r->package);
            values[0]=r->passed; values[1]=r->failed; values[2]=r->skipped; values[3]=r->passed+r->failed+r->skipped;
            testDuration(p->elapsed > 0 ? p->elapsed : elapsed, duration);
        } else {
            testPrint(out, "%-*s  ", labelWidth, label);
            memcpy(values, totals, sizeof(values)); strcpy(duration, totalTime);
        }
        for (int j = 0; j < 4; j++) {
            if (j) testPrint(out, "   ");
            if (row < count && !values[j]) testPrint(out, "%*s", widths[j], "");
            else testPrint(out, "%*d", widths[j], values[j]);
        }
        testPrint(out, "  %*s\n", timeWidth, duration);
    }
}
static void packageLine(packageRun *p, const char *line) {
    char *action = testField(line, "Action");
    if (!strcmp(action, "acquire")) p->pending++;
    else if (!strcmp(action, "release")) { if (p->held) p->held--; }
    else if (*action) {
        if (!strcmp(action, "output")) {
            char *name = testField(line, "Test");
            if (!*name) { char *output = testField(line, "Output"); testPackageOutput(&p->reporter, output); free(output); }
            free(name);
        }
        testEvent(&p->reporter, line);
        if (!strcmp(action, "pass") || !strcmp(action, "fail")) {
            char *name = testField(line, "Test");
            const char *elapsed = strstr(line, "\"Elapsed\":");
            if (!*name) {
                p->finalFailed = !strcmp(action, "fail");
                if (elapsed) p->elapsed = strtod(elapsed + 10, NULL);
            }
            free(name);
        }
    }
    else { testJSON(&p->reporter, "output", "", line, 0); testPackageOutput(&p->reporter, line); }
    free(action);
}
static int testPackages(int argc, char **flags, int nflags, testLocations *list,
                        const char *reportPath, const char *jsonPath, int json, int notty) {
    int workers = 16, color = !getenv("NO_COLOR"), includePassed = 0, includeSkipped = 1;
    (void)argc;
    for (int i = 2; i < nflags; i++) {
        if (!strcmp(flags[i], "--single-threaded")) workers = 1;
        else if (!strcmp(flags[i], "-t")) {
            if (++i == nflags) return fputs("ocean test: -t requires a thread count\n", stderr), 1;
            char *end; errno = 0; long n = strtol(flags[i], &end, 10);
            if (errno || !*flags[i] || *end || n < 1 || n > 2147483647L)
                return fputs("ocean test: invalid thread count\n", stderr), 1;
            workers = (int)n;
        } else if (!strcmp(flags[i], "--no-color")) color = 0;
        else if (!strcmp(flags[i], "-v") || !strcmp(flags[i], "--include-passed")) includePassed = 1;
        else if (!strcmp(flags[i], "--hide-skipped")) includeSkipped = 0;
    }
    if (reportPath && jsonPath && (!strcmp(reportPath, jsonPath) || sameFile(reportPath, jsonPath)))
        return fputs("ocean test: report and JSON paths must differ\n", stderr), 1;
    FILE *report = reportPath ? fopen(reportPath, "w") : NULL;
    FILE *events = jsonPath ? fopen(jsonPath, "w") : NULL;
    if ((reportPath && !report) || (jsonPath && !events)) {
        perror("ocean test: result file"); if (report) fclose(report); if (events) fclose(events); return 1;
    }
    packageRun *runs = calloc(list->count, sizeof(*runs));
    struct pollfd *polls = calloc(list->count * 2 + 1, sizeof(*polls));
    if (!runs || !polls) exit(1);
    const char *term = getenv("TERM");
    multiMode = 1;
    multiLive = !json && !notty && isatty(0) && isatty(1) && (!term || strcmp(term, "dumb"));
    multiUI = (testReporter){.displayFD = -1, .package = "packages", .color = color, .includeSkipped = includeSkipped};
    if (multiLive) multiUI.report = tmpfile();
    if (multiLive && !multiUI.report) { perror("ocean test: capture"); multiLive = 0; }
    struct sigaction action = {0}, oldInt, oldTerm, oldHup, ignore = {0}, oldPipe;
    action.sa_handler = forwardSignal; sigemptyset(&action.sa_mask);
    ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    interrupted = runningChild = 0;
    sigaction(SIGINT, &action, &oldInt); sigaction(SIGTERM, &action, &oldTerm); sigaction(SIGHUP, &action, &oldHup);
    sigaction(SIGPIPE, &ignore, &oldPipe);
    char directory[] = "/tmp/.ocean-packages-XXXXXX", executable[128] = {0};
    int temporary = mkdtemp(directory) != NULL;
    if (multiLive && temporary) {
        snprintf(executable, sizeof(executable), "%s/display", directory);
        packageBuild *b = loadTestDisplay(); parseResult *program = packageProgram(b);
        int failed = !program || analyzeSemantics(program).nErrors != 0;
        buildCache *cache = failed ? NULL : openBuildCache(flags[0], program, packageDirectory(b));
        if (!failed) failed = nativeBuild(cache, program, executable, 0, 0);
        closeBuildCache(cache); freePackage(b);
        if (!failed) { int unused[2] = {-1, -1}; testStartDisplay(&multiUI, executable, unused, unused); }
    }
    char **childArgs = calloc((size_t)nflags + 5, sizeof(*childArgs));
    int childCount = 2; childArgs[0] = flags[0]; childArgs[1] = flags[1];
    for (int i = 2; i < nflags; i++) {
        if (!strcmp(flags[i], "--outfile") || !strcmp(flags[i], "-outfile") || !strcmp(flags[i], "-o") ||
            !strcmp(flags[i], "--jsonfile") || !strcmp(flags[i], "-jsonfile")) { i++; continue; }
        childArgs[childCount++] = flags[i];
    }
    childArgs[childCount++] = "--json"; childArgs[childCount++] = "--notty";
    for (size_t i = 0; i < list->count; i++) {
        runs[i].output = runs[i].errors = runs[i].grant = -1;
        runs[i].reporter = (testReporter){.package = packageTestPath(list->items[i]), .json = json,
            .jsonFile = events, .report = report, .includePassed = includePassed, .includeSkipped = includeSkipped,
            .color = color && isatty(1), .displayFD = -1, .grantFD = -1};
        if (!runs[i].reporter.package) runs[i].reporter.package = strdup(list->items[i]);
        runs[i].reporter.tail = &runs[i].reporter.results;
    }
    size_t next = 0, finished = 0, active = 0, fair = 0;
    int status = 0; double start = testClock(), cancelled = 0;
    while (finished < list->count) {
        testCheckDisplay(&multiUI);
        if (multiUI.displayInterrupted && !interrupted) interrupted = SIGINT;
        if (interrupted && !cancelled) {
            cancelled = testClock(); status = 128 + interrupted;
            for (size_t i = 0; i < next; i++) if (!runs[i].finished) kill(-runs[i].pid, interrupted);
        }
        while (!cancelled && next < list->count && active < (size_t)workers) {
            packageRun *p = &runs[next]; int out[2], err[2], grant[2];
            if (pipe(out) || pipe(err) || pipe(grant)) { perror("ocean test: pipe"); interrupted = SIGTERM; break; }
            fflush(NULL);
            pid_t pid = fork();
            if (!pid) {
                setpgid(0, 0);
                signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
                dup2(out[1], 1); dup2(err[1], 2); dup2(grant[0], 199);
                for (int j = 0; j < 2; j++) { if (out[j] != 1 && out[j] != 199) close(out[j]); if (err[j] != 2 && err[j] != 199) close(err[j]); if (grant[j] != 199) close(grant[j]); }
                for (size_t j = 0; j < next; j++) {
                    if (runs[j].output >= 0 && runs[j].output != 199) close(runs[j].output);
                    if (runs[j].errors >= 0 && runs[j].errors != 199) close(runs[j].errors);
                    if (runs[j].grant >= 0 && runs[j].grant != 199) close(runs[j].grant);
                }
                if (multiUI.displayFD >= 0 && multiUI.displayFD != 199) close(multiUI.displayFD);
                int empty = open("/dev/null", O_RDONLY); if (empty >= 0) { dup2(empty, 0); close(empty); }
                fcntl(199, F_SETFL, O_NONBLOCK);
                multiMode = multiLive = 0; sharedBudget = 1;
                childArgs[childCount] = list->items[next];
                _exit(testOneMain(childCount + 1, childArgs));
            }
            close(out[1]); close(err[1]); close(grant[0]);
            if (pid < 0) { close(out[0]); close(err[0]); close(grant[1]); interrupted = SIGTERM; break; }
            setpgid(pid, pid);
            p->started = testClock(); p->pid = pid; p->output = out[0]; p->errors = err[0]; p->grant = grant[1];
            fcntl(p->grant, F_SETFL, O_NONBLOCK);
            next++; active++;
        }
        if (cancelled && next == finished) break;
        int used = 0;
        for (size_t i = 0; i < next; i++) {
            packageRun *p = &runs[i];
            if (cancelled && testClock() - cancelled > 1 && !p->finished) kill(-p->pid, SIGKILL);
            polls[used++] = (struct pollfd){p->output, POLLIN, 0};
            polls[used++] = (struct pollfd){p->errors, POLLIN, 0};
        }
        polls[used] = (struct pollfd){multiUI.displayFD, multiUI.displayPendingSize ? POLLOUT : 0, 0};
        if (poll(polls, (nfds_t)used + 1, 10) < 0 && errno != EINTR) interrupted = SIGTERM;
        if (polls[used].revents) testFlushDisplay(&multiUI);
        for (size_t i = 0; i < next; i++) {
            packageRun *p = &runs[i]; if (p->finished) continue;
            for (int which = 0; which < 2; which++) {
                struct pollfd *fd = &polls[i * 2 + (size_t)which];
                if (fd->fd < 0 || !(fd->revents & (POLLIN | POLLHUP | POLLERR))) continue;
                char bytes[8193]; ssize_t n = read(fd->fd, bytes, 8192);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { close(fd->fd); if (which) p->errors = -1; else p->output = -1; continue; }
                bytes[n] = 0;
                if (which) { testPackageOutput(&p->reporter, bytes); testJSON(&p->reporter, "output", "", bytes, 0); }
                else {
                    char *buffer = realloc(p->buffer, p->length + (size_t)n + 1); if (!buffer) exit(1);
                    p->buffer = buffer; memcpy(buffer + p->length, bytes, (size_t)n + 1); p->length += (size_t)n;
                    char *end;
                    while ((end = memchr(p->buffer, '\n', p->length))) {
                        *end = 0; packageLine(p, p->buffer);
                        size_t consumed = (size_t)(end - p->buffer) + 1;
                        memmove(p->buffer, p->buffer + consumed, p->length - consumed); p->length -= consumed; p->buffer[p->length] = 0;
                    }
                }
            }
            if (!p->exited) {
                int code; pid_t done = waitpid(p->pid, &code, WNOHANG);
                if (done == p->pid) {
                    p->exited = 1; p->status = WIFEXITED(code) ? WEXITSTATUS(code) : 128 + WTERMSIG(code);
                    kill(-p->pid, SIGKILL); // no orphan test processes may retain the pipes or slots
                }
            }
            if (p->exited && p->output < 0 && p->errors < 0) {
                if (p->length) packageLine(p, p->buffer);
                p->held = p->pending = 0; close(p->grant); p->grant = -1;
                p->finished = 1; active--; finished++;
                if (!p->reporter.completed || p->reporter.failed) { if (!p->status) p->status = 1; }
                if (p->status) status = p->status;
                if (!p->reporter.completed || (p->status && !p->finalFailed))
                    testJSON(&p->reporter, "fail", "", "package process failed", testClock() - start);
            }
        }
        int held = 0;
        for (size_t i = 0; i < next; i++) held += runs[i].held;
        // Round-robin grants prevent one package's parallel phase from taking
        // slots ahead of all other packages waiting to make serial progress.
        while (held < workers && next) {
            int granted = 0;
            for (size_t attempts = 0; attempts < next && held < workers; attempts++) {
                size_t i = fair++ % next; packageRun *p = &runs[i];
                if (!p->finished && p->pending && write(p->grant, "+", 1) == 1) { p->pending--; p->held++; held++; granted = 1; }
            }
            if (!granted) break;
        }
    }
    if (interrupted) status = 128 + interrupted;
    packageSummary(runs, next, testClock() - start);
    testStopDisplay(&multiUI, testClock() - start, status);
    if (multiUI.report) {
        rewind(multiUI.report); char bytes[8192]; size_t n;
        while ((n = fread(bytes, 1, sizeof(bytes), multiUI.report))) if (fwrite(bytes, 1, n, stdout) != n) status = 1;
        fclose(multiUI.report);
    }
    sigaction(SIGPIPE, &oldPipe, NULL); sigaction(SIGINT, &oldInt, NULL); sigaction(SIGTERM, &oldTerm, NULL); sigaction(SIGHUP, &oldHup, NULL);
    if (report) { int bad = ferror(report); if (fclose(report)) bad = 1; if (bad) status = 1; }
    if (events) { int bad = ferror(events); if (fclose(events)) bad = 1; if (bad) status = 1; }
    if (fflush(stdout) || ferror(stdout)) status = 1;
    for (size_t i = 0; i < list->count; i++) {
        testResult *t = runs[i].reporter.results;
        while (t) { testResult *nextResult = t->next; free(t->name); free(t->output); free(t); t = nextResult; }
        free((char *)runs[i].reporter.package); free(runs[i].reporter.packageOutput); free(runs[i].buffer);
    }
    if (*executable) unlink(executable); if (temporary) rmdir(directory);
    free(childArgs); free(polls); free(runs); free(multiUI.displayPending); multiMode = multiLive = 0;
    return status;
}
