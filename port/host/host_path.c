/* Windows-style paths from the guest on Android's case-sensitive filesystem.
 *
 * The engine builds paths the Windows way: backslash separators, and names whose case need not
 * match the files a player copies from their PC install ("zone\\Common\\..." vs "zone/common").
 * Every path a guest syscall passes goes through host_guest_path: backslashes become slashes, and
 * if the path doesn't exist as written, each component is looked up without regard to case. A
 * component that matches nothing (a file about to be created) is kept as written. */
#define _GNU_SOURCE
#include "host.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int exists(int dirfd, const char *path)
{
    struct stat st;
    return fstatat(dirfd, path, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

/* finds `name` in directory `dir` ("" = dirfd itself) ignoring case; 1 and the real name on success */
static int find_ignoring_case(int dirfd, const char *dir, const char *name, char *found, size_t size)
{
    int fd = openat(dirfd, *dir ? dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0;
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return 0;
    }
    int ok = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcasecmp(e->d_name, name) && strlen(e->d_name) < size) {
            strcpy(found, e->d_name);
            ok = 1;
            break;
        }
    }
    closedir(d);
    return ok;
}

int host_path_debug = 0; /* 1: log each path component that matches nothing, whatever the case */

const char *host_guest_path(int dirfd, uint64_t guest, char *out, size_t size)
{
    if (!guest) return NULL;
    const char *in = GUEST_PTR(const char *, guest);
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        char c = *in == '\\' ? '/' : *in;
        if (c == '/' && n && out[n - 1] == '/') continue; /* "a//b" */
        out[n++] = c;
    }
    out[n] = 0;
    if (!n || exists(dirfd, out)) return out;

    /* rebuild it a component at a time */
    char fixed[1024], part[256], real[256];
    size_t f = 0;
    const char *p = out;
    if (*p == '/') {
        fixed[f++] = '/';
        p++;
    }
    fixed[f] = 0;
    int matching = 1; /* once a component is missing, the rest can't exist either */
    while (*p) {
        size_t len = strcspn(p, "/");
        if (len >= sizeof part) return out;
        memcpy(part, p, len);
        part[len] = 0;
        p += len;
        const char *use = part;
        if (matching && strcmp(part, ".") && strcmp(part, "..")) {
            char probe[1024];
            if (f + len + 1 >= sizeof probe) return out;
            memcpy(probe, fixed, f);
            strcpy(probe + f, part);
            if (!exists(dirfd, probe)) {
                char dir[1024];
                memcpy(dir, fixed, f);
                dir[f && f > 1 ? f - 1 : f] = 0; /* without the trailing slash, but keep "/" */
                if (find_ignoring_case(dirfd, dir, part, real, sizeof real)) use = real;
                else {
                    matching = 0;
                    if (host_path_debug) host_info("path: no '%s' in '%s' (%s)", part, *dir ? dir : ".", out);
                }
            }
        }
        size_t ulen = strlen(use);
        if (f + ulen + 2 >= sizeof fixed) return out;
        memcpy(fixed + f, use, ulen);
        f += ulen;
        if (*p == '/') {
            fixed[f++] = '/';
            p++;
        }
        fixed[f] = 0;
    }
    if (f < size) memcpy(out, fixed, f + 1);
    return out;
}
