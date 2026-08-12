/* The slow path: actually doing the work.
 *
 * For a url project that is the install script, run only once a change has
 * been found; for a command project it is the command itself, run only once
 * the schedule allows. Either way this is where ergonomics beats speed: it
 * uses stdio, writes real files to $TMPDIR, hands the script to a shell with a
 * documented set of HACMAN_* variables, and lets the output stream straight to
 * the terminal. Set HACMAN_KEEP_TEMP=1 to keep the generated files. */

#include "hacman.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

static const char *tmpdir(void)
{
    const char *dir = getenv("TMPDIR");
    return (dir != NULL && dir[0] != '\0') ? dir : "/tmp";
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
        fprintf(stderr, "hacman: %s: cannot create temporary file: %s\n",
                path_out, strerror(errno));
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
               const hm_check_result *res)
{
    char  script_path[512];
    char  response_path[512];
    int   have_response;
    FILE *fp;
    pid_t pid;
    int   status = 0;
    int   rc     = 0;
    char  name[HM_NAME_MAX + 1];
    char  url[HM_URL_MAX + 1];

    hm_str_copy(name, sizeof(name), p->name);
    hm_str_copy(url, sizeof(url), p->url);

    if (p->install.len == 0) {
        printf("hacman: %s: changed, but no 'install' script is configured\n", name);
        return 0;
    }

    fp = temp_file("install", script_path, sizeof(script_path));
    if (fp == NULL) return -1;
    if (write_script(p, fp, script_path) != 0) {
        unlink(script_path);
        return -1;
    }

    have_response = (write_response(res, response_path, sizeof(response_path)) == 0);

    fflush(stdout);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hacman: fork() failed: %s\n", strerror(errno));
        rc = -1;
        goto cleanup;
    }
    if (pid == 0) {
        char *const argv[] = { (char *)HM_SHELL, (char *)"-e", script_path, NULL };

        setenv("HACMAN_NAME", name, 1);
        setenv("HACMAN_URL", url, 1);
        setenv("HACMAN_CHECK", hm_check_name(p->check), 1);
        setenv("HACMAN_VERSION", new_mark, 1);
        setenv("HACMAN_PREVIOUS", old_mark ? old_mark : "", 1);
        if (have_response) setenv("HACMAN_RESPONSE", response_path, 1);

        execv(HM_SHELL, argv);
        fprintf(stderr, "hacman: cannot execute %s: %s\n", HM_SHELL, strerror(errno));
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "hacman: waitpid() failed: %s\n", strerror(errno));
            rc = -1;
            goto cleanup;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "hacman: %s: install script killed by signal %d\n",
                    name, WTERMSIG(status));
        } else {
            fprintf(stderr, "hacman: %s: install script failed with exit code %d\n",
                    name, WEXITSTATUS(status));
        }
        fprintf(stderr, "hacman: %s: script kept at %s\n", name, script_path);
        /* Keep the script around; the state is not advanced either, so the
         * next run retries this project. */
        if (have_response) unlink(response_path);
        return -1;
    }

cleanup:
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
int hm_command_run(const hm_project *p)
{
    char  command[HM_COMMAND_MAX + 1];
    char  name[HM_NAME_MAX + 1];
    pid_t pid;
    int   status = 0;

    hm_str_copy(command, sizeof(command), p->command);
    hm_str_copy(name, sizeof(name), p->name);

    fflush(stdout);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hacman: fork() failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (chdir(p->workdir_path) != 0) {
            fprintf(stderr, "hacman: %s: cannot enter %s: %s\n",
                    name, p->workdir_path, strerror(errno));
            _exit(127);
        }
        setenv("HACMAN_NAME", name, 1);
        setenv("HACMAN_WORKDIR", p->workdir_path, 1);

        execl(HM_SHELL, HM_SHELL, "-c", command, (char *)NULL);
        fprintf(stderr, "hacman: cannot execute %s: %s\n", HM_SHELL, strerror(errno));
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "hacman: waitpid() failed: %s\n", strerror(errno));
            return -1;
        }
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "hacman: %s: command killed by signal %d\n",
                name, WTERMSIG(status));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "hacman: %s: command failed with exit code %d\n",
                name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
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
    execv(p->bin_path, argv);

    fprintf(stderr, "hacman: cannot execute %s: %s\n",
            p->bin_path, strerror(errno));
    return 127;
}
