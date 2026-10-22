/* Explicit source extraction, independent of setup execution and the cache. */

#include "hacman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

/* Walk relative parents through directory descriptors so an existing symlink
 * cannot redirect extraction outside the selected target. The config parser
 * has already rejected absolute paths and dot/parent components. */
static int unpack_file(const hm_embedded_file *file, int root_fd, int script)
{
    char        path[HM_FILE_PATH_MAX + 1];
    char       *name, *slash;
    const char *cursor = file->content.ptr;
    hm_str      line;
    FILE       *fp;
    int         parent_fd, next_fd, fd, have_line, failed;

    hm_str_copy(path, sizeof(path), file->path);
    parent_fd = dup(root_fd);
    if (parent_fd < 0) {
        hm_err("hacman: cannot open unpack directory: %s\n", strerror(errno));
        return -1;
    }
    name = path;
    while ((slash = strchr(name, '/')) != NULL) {
        *slash = '\0';
        if (mkdirat(parent_fd, name, 0700) != 0 && errno != EEXIST) goto path_error;
        next_fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next_fd < 0) goto path_error;
        close(parent_fd);
        parent_fd = next_fd;
        name      = slash + 1;
    }

    /* Exclusive creation protects existing files, symlinks, and hard links.
     * Remove only our own newly created file if writing it fails. */
    fd = openat(parent_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                script ? 0700 : 0600);
    if (fd < 0) goto path_error;
    fp = fdopen(fd, "w");
    if (fp == NULL) {
        hm_err("hacman: %S: cannot open extracted file stream: %s\n", file->path, strerror(errno));
        close(fd);
        unlinkat(parent_fd, name, 0);
        close(parent_fd);
        return -1;
    }

    /* Shell fields become executable scripts. Preserve an existing shebang;
     * otherwise use the same interpreter as hacman's setup execution. */
    have_line = hm_file_next_line(file, &cursor, &line);
    if (script && (!have_line || line.len < 2 || memcmp(line.ptr, "#!", 2) != 0)) {
        fputs("#!" HM_SHELL "\n", fp);
    }
    while (have_line) {
        if (fwrite(line.ptr, 1, line.len, fp) != line.len || fputc('\n', fp) == EOF) break;
        have_line = hm_file_next_line(file, &cursor, &line);
    }
    failed = ferror(fp);
    if (fclose(fp) != 0) failed = 1;
    if (failed) {
        hm_err("hacman: %S: cannot write extracted file\n", file->path);
        unlinkat(parent_fd, name, 0);
    }
    close(parent_fd);
    return failed ? -1 : 0;

path_error:
    hm_err("hacman: %S: cannot extract file: %s\n", file->path, strerror(errno));
    close(parent_fd);
    return -1;
}

int hm_unpack(const hm_project *p, const char *target)
{
    const char      *script_name = NULL;
    hm_embedded_file script      = {0};
    size_t           i, name_len;
    int              root_fd, rc = 0;

    if (p->command.len > 0) script_name = "command.sh";
    else if (p->install.len > 0) script_name = "install.sh";

    /* A generated script and an embedded file must never compete for the same
     * path, including a directory where the script file needs to go. */
    if (script_name != NULL) {
        name_len = strlen(script_name);
        for (i = 0; i < p->file_count; ++i) {
            hm_str path = p->files[i].path;
            if (path.len >= name_len && memcmp(path.ptr, script_name, name_len) == 0 &&
                (path.len == name_len || path.ptr[name_len] == '/')) {
                hm_err("hacman: embedded path '%S' conflicts with extracted %s\n", path,
                       script_name);
                return -1;
            }
        }
    }

    if (hm_workdir_ensure(target) != 0) return -1;
    root_fd = open(target, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        hm_err("hacman: %s: cannot open unpack target: %s\n", target, strerror(errno));
        return -1;
    }
    for (i = 0; i < p->file_count; ++i) {
        if (unpack_file(&p->files[i], root_fd, 0) != 0) {
            rc = -1;
            break;
        }
    }
    if (rc == 0 && script_name != NULL) {
        script.path           = (hm_str){script_name, strlen(script_name)};
        script.content        = p->command.len > 0 ? p->command : p->install;
        script.content_indent = p->command.len > 0 ? 0 : p->install_indent;
        rc                    = unpack_file(&script, root_fd, 1);
    }
    close(root_fd);
    return rc;
}
