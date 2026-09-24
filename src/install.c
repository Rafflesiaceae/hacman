/* The slow path: actually doing the work.
 *
 * For a url project that is the install script, run only once a change has
 * been found; for a command project it is the command itself, run only once
 * the schedule allows. Either way this is where ergonomics beats speed: it
 * uses stdio, writes real files to $TMPDIR, and hands the script to a shell
 * with a documented set of HACMAN_* variables. Successful setup output is
 * hidden unless tracing is enabled; failed setup output is replayed to stderr.
 * Set HACMAN_KEEP_TEMP=1 to keep the generated files. */

#include "hacman.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

static const char *tmpdir(void)
{
    const char *dir = getenv("TMPDIR");
    return (dir != NULL && dir[0] != '\0') ? dir : "/tmp";
}

/* Successful setup is silent by default. A seekable anonymous file avoids
 * pipe backpressure for noisy builds and lets failures replay every diagnostic
 * to stderr after the child exits. Trace mode keeps output live instead. */
static FILE *capture_file(int trace)
{
    FILE *fp;

    if (trace) return NULL;
    fp = tmpfile();
    if (fp == NULL) {
        fprintf(stderr, "hacman: cannot create setup-output capture: %s\n", strerror(errno));
    }
    return fp;
}

static int redirect_capture(FILE *fp)
{
    int fd = fileno(fp);

    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) return -1;
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) close(fd);
    return 0;
}

static void replay_capture(FILE *fp)
{
    char    buf[8192];
    int     fd = fileno(fp);
    ssize_t count;

    if (lseek(fd, 0, SEEK_SET) < 0) return;
    while ((count = read(fd, buf, sizeof(buf))) > 0) {
        hm_err_bytes(buf, (size_t)count);
    }
    hm_err_stream_end();
}

/* Streams setup stderr through hm_err_bytes so live diagnostics identify
 * hacman while the final bin-path program can still inherit stderr directly. */
static void relay_setup_stderr(int fd)
{
    char    buf[8192];
    ssize_t count;

    while ((count = read(fd, buf, sizeof(buf))) > 0) {
        hm_err_bytes(buf, (size_t)count);
    }
    hm_err_stream_end();
}

static int mkdir_p(const char *path)
{
    char   copy[HM_PATH_MAX * 2 + 2];
    size_t i, n = strlen(path);

    if (n == 0 || n >= sizeof(copy)) {
        fprintf(stderr, "hacman: work directory path is too long\n");
        return -1;
    }
    memcpy(copy, path, n + 1);

    for (i = 1; i <= n; ++i) {
        if (copy[i] == '/' || copy[i] == '\0') {
            char saved = copy[i];
            copy[i]    = '\0';
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
                fprintf(stderr, "hacman: %s: cannot create directory: %s\n", copy, strerror(errno));
                return -1;
            }
            copy[i] = saved;
        }
    }
    return 0;
}

/* Removes one tree bottom-up. lstat() ensures a symlink below the cache is
 * unlinked as an entry rather than followed into an unrelated directory. */
static int remove_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "hacman: %s: cannot inspect stale work path: %s\n", path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        struct dirent *entry;
        mode_t         writable_mode = st.st_mode | S_IRUSR | S_IWUSR | S_IXUSR;
        DIR           *dir;

        /* Tools such as Go deliberately make cache directories read-only.
         * This workspace belongs to hacman, so restore owner traversal and
         * write permissions before removing entries below it. */
        if ((st.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
            (S_IRUSR | S_IWUSR | S_IXUSR) &&
            chmod(path, writable_mode) != 0) {
            fprintf(stderr, "hacman: %s: cannot make stale work directory writable: %s\n", path,
                    strerror(errno));
            return -1;
        }

        dir = opendir(path);
        if (dir == NULL) {
            fprintf(stderr, "hacman: %s: cannot open stale work directory: %s\n", path,
                    strerror(errno));
            return -1;
        }
        errno = 0;
        while ((entry = readdir(dir)) != NULL) {
            char  *child;
            size_t path_len, name_len;
            int    rc;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            path_len = strlen(path);
            name_len = strlen(entry->d_name);
            child    = (char *)malloc(path_len + 1 + name_len + 1);
            if (child == NULL) {
                fprintf(stderr, "hacman: cannot allocate a stale work path\n");
                closedir(dir);
                return -1;
            }
            memcpy(child, path, path_len);
            child[path_len] = '/';
            memcpy(child + path_len + 1, entry->d_name, name_len + 1);
            rc = remove_tree(child);
            free(child);
            if (rc != 0) {
                closedir(dir);
                return -1;
            }
            errno = 0;
        }
        if (errno != 0) {
            int saved = errno;
            closedir(dir);
            fprintf(stderr, "hacman: %s: cannot read stale work directory: %s\n", path,
                    strerror(saved));
            return -1;
        }
        if (closedir(dir) != 0) {
            fprintf(stderr, "hacman: %s: cannot close stale work directory: %s\n", path,
                    strerror(errno));
            return -1;
        }
        if (rmdir(path) != 0) {
            fprintf(stderr, "hacman: %s: cannot remove stale work directory: %s\n", path,
                    strerror(errno));
            return -1;
        }
        return 0;
    }

    if (unlink(path) != 0) {
        fprintf(stderr, "hacman: %s: cannot remove stale work file: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

int hm_workdir_reset(const char *workdir)
{
    return remove_tree(workdir);
}

int hm_workdir_ensure(const char *workdir)
{
    struct stat st;

    if (mkdir_p(workdir) != 0) return -1;
    /* The writable rule must name a real directory, never a cache entry that
     * redirects policy elsewhere through a final-component symlink. */
    if (lstat(workdir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "hacman: %s: sandbox work path is not a directory\n", workdir);
        return -1;
    }
    return 0;
}

static int write_embedded_file(const hm_embedded_file *file, const char *workdir)
{
    char        target[HM_PATH_MAX * 2 + 2];
    char        parent[HM_PATH_MAX * 2 + 2];
    char        temp[HM_PATH_MAX * 2 + 32];
    const char *cursor = file->content.ptr;
    hm_str      line;
    FILE       *fp;
    int         fd;
    size_t      i;

    if ((size_t)snprintf(target, sizeof(target), "%s/%.*s", workdir, (int)file->path.len,
                         file->path.ptr) >= sizeof(target)) {
        fprintf(stderr, "hacman: embedded-file path is too long\n");
        return -1;
    }
    memcpy(parent, target, strlen(target) + 1);
    for (i = strlen(parent); i > 0; --i) {
        if (parent[i] == '/') {
            parent[i] = '\0';
            break;
        }
    }
    if (mkdir_p(parent) != 0) return -1;

    if ((size_t)snprintf(temp, sizeof(temp), "%s.tmp.%ld", target, (long)getpid()) >=
        sizeof(temp)) {
        fprintf(stderr, "hacman: embedded-file temporary path is too long\n");
        return -1;
    }
    fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr, "hacman: %s: cannot create embedded file: %s\n", temp, strerror(errno));
        return -1;
    }
    fp = fdopen(fd, "w");
    if (fp == NULL) {
        fprintf(stderr, "hacman: %s: fdopen failed\n", temp);
        close(fd);
        unlink(temp);
        return -1;
    }
    while (hm_file_next_line(file, &cursor, &line)) {
        if (fwrite(line.ptr, 1, line.len, fp) != line.len || fputc('\n', fp) == EOF) {
            fprintf(stderr, "hacman: %s: cannot write embedded file\n", temp);
            fclose(fp);
            unlink(temp);
            return -1;
        }
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "hacman: %s: cannot write embedded file\n", temp);
        unlink(temp);
        return -1;
    }
    if (rename(temp, target) != 0) {
        fprintf(stderr, "hacman: %s: cannot install embedded file: %s\n", target, strerror(errno));
        unlink(temp);
        return -1;
    }
    return 0;
}

int hm_materialize_files(const hm_project *p, const char *workdir)
{
    size_t i;

    if (hm_workdir_ensure(workdir) != 0) return -1;
    for (i = 0; i < p->file_count; ++i) {
        if (write_embedded_file(&p->files[i], workdir) != 0) return -1;
    }
    return 0;
}

/* Creates $TMPDIR/hacman-<what>-XXXXXX and returns its FILE*, storing the
 * final path in `path_out`. */
static FILE *temp_file(const char *what, char *path_out, size_t cap)
{
    int   fd;
    FILE *fp;

    if ((size_t)snprintf(path_out, cap, "%s/hacman-%s-XXXXXX", tmpdir(), what) >= cap) {
        fprintf(stderr, "hacman: temporary path too long\n");
        return NULL;
    }
    fd = mkstemp(path_out);
    if (fd < 0) {
        fprintf(stderr, "hacman: %s: cannot create temporary file: %s\n", path_out,
                strerror(errno));
        return NULL;
    }
    fp = fdopen(fd, "w");
    if (fp == NULL) {
        fprintf(stderr, "hacman: %s: fdopen failed\n", path_out);
        close(fd);
        return NULL;
    }
    return fp;
}

/* Writes the install block out as a runnable script.
 *
 * The parser kept the block as a raw region of the config buffer, complete
 * with its original indentation; hm_install_next_line() undoes that here, off
 * the fast path, and is the same iterator `--plan` prints from. */
static int write_script(const hm_project *p, FILE *fp, const char *path)
{
    const char *cursor = p->install.ptr;
    hm_str      line;

    fputs("#!" HM_SHELL "\n", fp);
    while (hm_install_next_line(p, &cursor, &line)) {
        fwrite(line.ptr, 1, line.len, fp);
        fputc('\n', fp);
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "hacman: %s: cannot write script\n", path);
        return -1;
    }
    return 0;
}

/* Dumps the fetched response so install scripts can grep it instead of
 * re-downloading. Returns 0 if a file was written. */
static int write_response(const hm_check_result *res, char *path_out, size_t cap)
{
    FILE *fp;

    if (res == NULL || res->body_len == 0) return -1;

    fp = temp_file("response", path_out, cap);
    if (fp == NULL) return -1;

    if (fwrite(res->body, 1, res->body_len, fp) != res->body_len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

int hm_install(const hm_project *p, const char *old_mark, const char *new_mark,
               const hm_check_result *res, const char *sandbox_workdir, const char *cache_dir,
               int trace)
{
    char  script_path[512];
    char  response_path[512];
    int   have_response;
    FILE *fp;
    pid_t pid;
    int   status = 0;
    int   rc     = 0;
    FILE *capture;
    int   stderr_pipe[2] = {-1, -1};
    char  name[HM_NAME_MAX + 1];
    char  url[HM_URL_MAX + 1];

    hm_str_copy(name, sizeof(name), p->name);
    hm_str_copy(url, sizeof(url), p->url);

    if (p->install.len == 0) {
        fprintf(stderr, "hacman: %s: changed, but no 'install' script is configured\n", name);
        return 0;
    }

    if (p->sandboxed && hm_workdir_ensure(sandbox_workdir) != 0) return -1;

    fp = temp_file("install", script_path, sizeof(script_path));
    if (fp == NULL) return -1;
    if (write_script(p, fp, script_path) != 0) {
        unlink(script_path);
        return -1;
    }

    have_response = (write_response(res, response_path, sizeof(response_path)) == 0);

    fflush(stdout);
    capture = capture_file(trace);
    if (!trace && capture == NULL) {
        rc = -1;
        goto cleanup;
    }
    if (trace && pipe(stderr_pipe) != 0) {
        fprintf(stderr, "hacman: cannot create setup stderr pipe: %s\n", strerror(errno));
        rc = -1;
        goto cleanup;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hacman: fork() failed: %s\n", strerror(errno));
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        rc = -1;
        goto cleanup;
    }
    if (pid == 0) {
        char *const plain_argv[] = {(char *)HM_SHELL, (char *)"-e", script_path, NULL};
        char *const trace_argv[] = {(char *)HM_SHELL, (char *)"-e", (char *)"-x", script_path,
                                    NULL};

        if (trace) {
            close(stderr_pipe[0]);
            if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) _exit(126);
            if (stderr_pipe[1] != STDERR_FILENO) close(stderr_pipe[1]);
        }
        if (capture != NULL && redirect_capture(capture) != 0) {
            fprintf(stderr, "hacman: cannot capture setup output: %s\n", strerror(errno));
            _exit(126);
        }

        setenv("HACMAN_NAME", name, 1);
        setenv("HACMAN_URL", url, 1);
        setenv("HACMAN_CHECK", hm_check_name(p->check), 1);
        setenv("HACMAN_VERSION", new_mark, 1);
        setenv("HACMAN_PREVIOUS", old_mark ? old_mark : "", 1);
        if (have_response) setenv("HACMAN_RESPONSE", response_path, 1);

        /* Sandboxed scripts naturally create relative and temporary output in
         * their one writable tree. The absolute script and response paths stay
         * readable through the global read-only rule. */
        if (p->sandboxed) {
            if (chdir(sandbox_workdir) != 0) {
                fprintf(stderr, "hacman: %s: cannot enter sandbox work directory %s: %s\n", name,
                        sandbox_workdir, strerror(errno));
                _exit(126);
            }
            setenv("TMPDIR", sandbox_workdir, 1);
            if (hm_sandbox_enter(sandbox_workdir, cache_dir) != 0) _exit(126);
        }

        execv(HM_SHELL, trace ? trace_argv : plain_argv);
        fprintf(stderr, "hacman: cannot execute %s: %s\n", HM_SHELL, strerror(errno));
        _exit(127);
    }

    if (trace) {
        close(stderr_pipe[1]);
        relay_setup_stderr(stderr_pipe[0]);
        close(stderr_pipe[0]);
        stderr_pipe[0] = -1;
        stderr_pipe[1] = -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "hacman: waitpid() failed: %s\n", strerror(errno));
            rc = -1;
            goto cleanup;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (capture != NULL) replay_capture(capture);
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "hacman: %s: install script killed by signal %d\n", name,
                    WTERMSIG(status));
        } else {
            fprintf(stderr, "hacman: %s: install script failed with exit code %d\n", name,
                    WEXITSTATUS(status));
        }
        fprintf(stderr, "hacman: %s: script kept at %s\n", name, script_path);
        /* Keep the script around; the state is not advanced either, so the
         * next run retries this project. */
        if (have_response) unlink(response_path);
        if (capture != NULL) fclose(capture);
        return -1;
    }

cleanup:
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    if (capture != NULL) fclose(capture);
    if (getenv("HACMAN_KEEP_TEMP") == NULL) {
        unlink(script_path);
        if (have_response) unlink(response_path);
    } else {
        fprintf(stderr, "hacman: %s: kept %s\n", name, script_path);
    }
    return rc;
}

/* Runs a command project: `sh -c <command>`, in its working directory.
 *
 * No temporary file and no script: a single command is already the simplest
 * thing a shell can be handed. The caller records the run only if this
 * returns 0, so a failing command is retried on the next run. */
int hm_command_run(const hm_project *p, const char *workdir, const char *sandbox_workdir,
                    const char *cache_dir, int trace)
{
    char  command[HM_COMMAND_MAX + 1];
    char  name[HM_NAME_MAX + 1];
    pid_t pid;
    int   status = 0;
    FILE *capture;
    int   stderr_pipe[2] = {-1, -1};

    hm_str_copy(command, sizeof(command), p->command);
    hm_str_copy(name, sizeof(name), p->name);

    if (p->sandboxed && hm_workdir_ensure(sandbox_workdir) != 0) return -1;

    fflush(stdout);
    capture = capture_file(trace);
    if (!trace && capture == NULL) return -1;
    if (trace && pipe(stderr_pipe) != 0) {
        fprintf(stderr, "hacman: cannot create setup stderr pipe: %s\n", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hacman: fork() failed: %s\n", strerror(errno));
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
        if (capture != NULL) fclose(capture);
        return -1;
    }
    if (pid == 0) {
        if (trace) {
            close(stderr_pipe[0]);
            if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) _exit(126);
            if (stderr_pipe[1] != STDERR_FILENO) close(stderr_pipe[1]);
        }
        if (capture != NULL && redirect_capture(capture) != 0) {
            fprintf(stderr, "hacman: cannot capture setup output: %s\n", strerror(errno));
            _exit(126);
        }
        if (chdir(workdir) != 0) {
            fprintf(stderr, "hacman: %s: cannot enter %s: %s\n", name, workdir, strerror(errno));
            _exit(127);
        }
        setenv("HACMAN_NAME", name, 1);
        setenv("HACMAN_WORKDIR", workdir, 1);
        if (p->sandboxed) {
            /* Keep generic temporary-file users inside the only writable
             * hierarchy even when the configured cwd is read-only. */
            setenv("TMPDIR", sandbox_workdir, 1);
            if (hm_sandbox_enter(sandbox_workdir, cache_dir) != 0) _exit(126);
        }

        if (trace) {
            execl(HM_SHELL, HM_SHELL, "-x", "-c", command, (char *)NULL);
        } else {
            execl(HM_SHELL, HM_SHELL, "-c", command, (char *)NULL);
        }
        fprintf(stderr, "hacman: cannot execute %s: %s\n", HM_SHELL, strerror(errno));
        _exit(127);
    }

    if (trace) {
        close(stderr_pipe[1]);
        relay_setup_stderr(stderr_pipe[0]);
        close(stderr_pipe[0]);
        stderr_pipe[0] = -1;
        stderr_pipe[1] = -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "hacman: waitpid() failed: %s\n", strerror(errno));
            if (capture != NULL) fclose(capture);
            return -1;
        }
    }

    if (WIFSIGNALED(status)) {
        if (capture != NULL) replay_capture(capture);
        fprintf(stderr, "hacman: %s: command killed by signal %d\n", name, WTERMSIG(status));
        if (capture != NULL) fclose(capture);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (capture != NULL) replay_capture(capture);
        fprintf(stderr, "hacman: %s: command failed with exit code %d\n", name,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        if (capture != NULL) fclose(capture);
        return -1;
    }
    if (capture != NULL) fclose(capture);
    return 0;
}

/* Becomes the program this project sets up.
 *
 * `argv` is the tail of hacman's own argv starting at the FILE slot, so
 * overwriting that slot with the resolved bin-path yields exactly the argument
 * vector the program should see - already NUL-terminated, with no copying and
 * no limit on how many arguments may be forwarded.
 *
 * execv() replaces the process, so the program inherits the terminal and its
 * exit status becomes hacman's. This only returns if the program could not be
 * started at all. */
int hm_exec_bin(const hm_project *p, char **argv)
{
    argv[0] = (char *)p->bin_path;

    fflush(stdout);
    hm_err_stream_end();
    execv(p->bin_path, argv);

    fprintf(stderr, "hacman: cannot execute %s: %s\n", p->bin_path, strerror(errno));
    return 127;
}
