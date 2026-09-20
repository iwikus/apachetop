/*
 * apachetop - Apache HTTP Server scoreboard monitor
 *
 * Copyright (C) 2026 apachetop contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License only.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

/*
 * apachetop.c
 *
 * Read an Apache httpd scoreboard directly from the APR shared-memory object.
 *
 * The binary layout is intentionally NOT duplicated here.  Apache's own
 * scoreboard.h is the source of truth for global_score, process_score and
 * worker_score, while APR_ALIGN_DEFAULT() is used exactly like Apache uses
 * it when laying out the shared segment.
 *
 * Build against the Apache headers installed on the same machine as httpd.
 * On Debian/Ubuntu this normally means the apache2-dev package.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "httpd.h"
#include "scoreboard.h"
#include "version.h"

#define SHM_PREFIX "/dev/shm/ShM."
#define MAX_SLOTS 4096
#define MAX_VISIBLE 40

/* Apache uses these exact aligned sizes in server/scoreboard.c. */
#define SCORE_SIZEOF(type) APR_ALIGN_DEFAULT(sizeof(type))

/* APR's POSIX shm implementation reserves an aligned apr_size_t at the start
 * of the backing object for metadata. This is deliberately derived from APR,
 * not hard-coded as 8 bytes. */
#define APR_SHM_META_SIZE APR_ALIGN_DEFAULT(sizeof(apr_size_t))

struct shm_view {
    int fd;
    size_t map_size;
    unsigned char *map;
    size_t scoreboard_size;
    size_t global_size;
    size_t process_size;
    size_t worker_size;
    size_t worker_base;
    int server_limit;
    int thread_limit;
    char path[PATH_MAX];
};

struct worker {
    int slot;
    int thread;
    pid_t pid;
    int generation;
    unsigned char status;
    unsigned short conn_count;
    apr_off_t conn_bytes;
    unsigned long access_count;
    apr_off_t bytes_served;
    unsigned long my_access_count;
    apr_off_t my_bytes_served;
    apr_time_t start_time;
    apr_time_t stop_time;
    apr_time_t last_used;
    apr_time_t duration;

    char client[sizeof(((worker_score *)0)->client) + 1];
    char request[sizeof(((worker_score *)0)->request) + 1];
    char vhost[sizeof(((worker_score *)0)->vhost) + 1];
    char protocol[sizeof(((worker_score *)0)->protocol) + 1];
    char client64[sizeof(((worker_score *)0)->client64) + 1];
};

struct snapshot {
    bool valid;
    uint64_t total_access;
    apr_off_t total_bytes;
    double mono;
};

static volatile sig_atomic_t stop_flag = 0;

enum sort_mode {
    SORT_SLOT,
    SORT_AGE,
    SORT_CLIENT,
    SORT_VHOST
};

static enum sort_mode g_sort_mode = SORT_SLOT;
static apr_time_t g_sort_now_us = 0;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static double now_wall_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void copy_field(char *dst, size_t dstsz, const char *src, size_t srcsz)
{
    size_t n = strnlen(src, srcsz);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void copy_worker_field(char *dst, size_t dstsz, const char *src, size_t srcsz)
{
    copy_field(dst, dstsz, src, srcsz);
}

static void read_worker(const struct shm_view *v, int slot, int thread,
                        struct worker *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = slot;
    out->thread = thread;

    const size_t worker_index =
        (size_t)slot * (size_t)v->thread_limit + (size_t)thread;

    const unsigned char *base = v->map + APR_SHM_META_SIZE;
    const process_score *ps = (const process_score *)
        (base + v->global_size + (size_t)slot * v->process_size);
    const worker_score *ws = (const worker_score *)
        (base + v->worker_base + worker_index * v->worker_size);

    /* Copy the structs first.  The scoreboard is concurrently updated by
     * Apache; making a local copy avoids repeatedly dereferencing shared data
     * while rendering one row. */
    process_score pscopy;
    worker_score wscopy;
    memcpy(&pscopy, ps, sizeof(pscopy));
    memcpy(&wscopy, ws, sizeof(wscopy));

    out->pid = wscopy.pid != 0 ? wscopy.pid : pscopy.pid;
    out->generation = wscopy.generation;
    out->status = wscopy.status;
    out->conn_count = wscopy.conn_count;
    out->conn_bytes = wscopy.conn_bytes;
    out->access_count = wscopy.access_count;
    out->bytes_served = wscopy.bytes_served;
    out->my_access_count = wscopy.my_access_count;
    out->my_bytes_served = wscopy.my_bytes_served;
    out->start_time = wscopy.start_time;
    out->stop_time = wscopy.stop_time;
    out->last_used = wscopy.last_used;
    out->duration = wscopy.duration;

    copy_worker_field(out->client, sizeof(out->client),
                      wscopy.client, sizeof(wscopy.client));
    copy_worker_field(out->request, sizeof(out->request),
                      wscopy.request, sizeof(wscopy.request));
    copy_worker_field(out->vhost, sizeof(out->vhost),
                      wscopy.vhost, sizeof(wscopy.vhost));
    copy_worker_field(out->protocol, sizeof(out->protocol),
                      wscopy.protocol, sizeof(wscopy.protocol));
    copy_worker_field(out->client64, sizeof(out->client64),
                      wscopy.client64, sizeof(wscopy.client64));
}

static int validate_layout(size_t map_size, int servers, int threads,
                           bool verbose)
{
    if (servers <= 0 || servers > MAX_SLOTS ||
        threads <= 0 || threads > MAX_SLOTS) {
        if (verbose) {
            fprintf(stderr, "Invalid scoreboard limits: server_limit=%d thread_limit=%d\n",
                    servers, threads);
        }
        return -EINVAL;
    }

    const size_t global_size = SCORE_SIZEOF(global_score);
    const size_t process_size = SCORE_SIZEOF(process_score);
    const size_t worker_size = SCORE_SIZEOF(worker_score);
    const size_t score_size = global_size
        + process_size * (size_t)servers
        + worker_size * (size_t)servers * (size_t)threads;
    const size_t expected_file_size = APR_SHM_META_SIZE + score_size;

    if (expected_file_size != map_size) {
        if (verbose) {
            fprintf(stderr,
                    "Scoreboard size mismatch: Apache headers calculate %zu bytes "
                    "(+ %zu APR metadata) = %zu, file is %zu\n",
                    score_size, (size_t)APR_SHM_META_SIZE,
                    expected_file_size, map_size);
            fprintf(stderr,
                    "  global_score=%zu process_score=%zu worker_score=%zu\n",
                    global_size, process_size, worker_size);
        }
        return -EINVAL;
    }

    return 0;
}

static int open_view(const char *path, struct shm_view *v, bool verbose)
{
    memset(v, 0, sizeof(*v));
    v->fd = -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        const int e = errno;
        close(fd);
        return -e;
    }

    const size_t meta_size = APR_SHM_META_SIZE;
    if (st.st_size < (off_t)(meta_size + SCORE_SIZEOF(global_score))) {
        close(fd);
        return -EINVAL;
    }

    const size_t map_size = (size_t)st.st_size;
    void *map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        const int e = errno;
        close(fd);
        return -e;
    }

    /* Verify the APR metadata using APR's actual type/size instead of a magic
     * 64-bit integer. On the Linux APR implementation used for /dev/shm/ShM.*,
     * this value is the total backing-object size. */
    apr_size_t stored_size = 0;
    memcpy(&stored_size, map, sizeof(stored_size));
    const size_t stored_real_size = (size_t)stored_size;
    if (stored_real_size != map_size) {
        if (verbose) {
            fprintf(stderr,
                    "%s: APR metadata says %zu bytes, file is %zu\n",
                    path, stored_real_size, map_size);
        }
        munmap(map, map_size);
        close(fd);
        return -EINVAL;
    }

    const unsigned char *base = (const unsigned char *)map + meta_size;
    global_score global;
    memcpy(&global, base, sizeof(global));

    if (validate_layout(map_size, global.server_limit,
                        global.thread_limit, verbose) != 0) {
        munmap(map, map_size);
        close(fd);
        return -ENOTSUP;
    }

    v->fd = fd;
    v->map_size = map_size;
    v->map = (unsigned char *)map;
    v->global_size = SCORE_SIZEOF(global_score);
    v->process_size = SCORE_SIZEOF(process_score);
    v->worker_size = SCORE_SIZEOF(worker_score);
    v->worker_base = v->global_size
                   + v->process_size * (size_t)global.server_limit;
    v->scoreboard_size = v->global_size
                       + v->process_size * (size_t)global.server_limit
                       + v->worker_size * (size_t)global.server_limit
                       * (size_t)global.thread_limit;
    v->server_limit = global.server_limit;
    v->thread_limit = global.thread_limit;
    snprintf(v->path, sizeof(v->path), "%s", path);
    return 0;
}

static void close_view(struct shm_view *v)
{
    if (v->map && v->map != MAP_FAILED) {
        munmap(v->map, v->map_size);
    }
    if (v->fd >= 0) {
        close(v->fd);
    }
    memset(v, 0, sizeof(*v));
    v->fd = -1;
}

static bool choose_scoreboard(char *out, size_t outsz, const char *forced)
{
    if (forced) {
        snprintf(out, outsz, "%s", forced);
        return access(out, R_OK) == 0;
    }

    glob_t g;
    memset(&g, 0, sizeof(g));
    const int rc = glob(SHM_PREFIX "*", GLOB_NOSORT, NULL, &g);
    if (rc != 0 || g.gl_pathc == 0) {
        globfree(&g);
        return false;
    }

    const char *best_path = NULL;
    off_t best_size = -1;
    time_t best_mtime = 0;

    for (size_t i = 0; i < g.gl_pathc; ++i) {
        struct stat st;
        if (stat(g.gl_pathv[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (st.st_size < 1024) {
            continue;
        }

        /* Check only the APR metadata. Full validation happens in open_view(). */
        int fd = open(g.gl_pathv[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        apr_size_t stored_size = 0;
        const ssize_t n = pread(fd, &stored_size, sizeof(stored_size), 0);
        close(fd);
        if (n != (ssize_t)sizeof(stored_size) ||
            (off_t)stored_size != st.st_size) {
            continue;
        }

        if (st.st_size > best_size ||
            (st.st_size == best_size && st.st_mtime > best_mtime)) {
            best_path = g.gl_pathv[i];
            best_size = st.st_size;
            best_mtime = st.st_mtime;
        }
    }

    if (best_path) {
        snprintf(out, outsz, "%s", best_path);
    }
    const bool found = best_path != NULL;
    globfree(&g);
    return found;
}

static char status_char(unsigned char st)
{
    switch (st) {
        case SERVER_DEAD: return '.';
        case SERVER_STARTING: return 'S';
        case SERVER_READY: return '_';
        case SERVER_BUSY_READ: return 'R';
        case SERVER_BUSY_WRITE: return 'W';
        case SERVER_BUSY_KEEPALIVE: return 'K';
        case SERVER_BUSY_LOG: return 'L';
        case SERVER_BUSY_DNS: return 'D';
        case SERVER_CLOSING: return 'C';
        case SERVER_GRACEFUL: return 'G';
        case SERVER_IDLE_KILL: return 'I';
        default: return '?';
    }
}

static void fmt_bytes(double n, char *buf, size_t sz)
{
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) {
        n /= 1024.0;
        ++i;
    }
    snprintf(buf, sz, "%.1f %s", n, u[i]);
}

static void fmt_age_us(apr_time_t us, char *buf, size_t sz)
{
    if (us < 0) {
        us = 0;
    }
    if (us < 1000) {
        snprintf(buf, sz, "%lldus", (long long)us);
    }
    else if (us < 1000000) {
        snprintf(buf, sz, "%.1fms", (double)us / 1000.0);
    }
    else if (us < 60000000) {
        snprintf(buf, sz, "%.2fs", (double)us / 1000000.0);
    }
    else {
        snprintf(buf, sz, "%.1fm", (double)us / 60000000.0);
    }
}

static int cmp_slot(const struct worker *x, const struct worker *y)
{
    if (x->slot != y->slot) return x->slot - y->slot;
    return x->thread - y->thread;
}

static apr_time_t worker_age_us(const struct worker *w, apr_time_t now_us)
{
    if (w->start_time <= 0 || now_us <= w->start_time) {
        return 0;
    }
    return now_us - w->start_time;
}

static int cmp_main(const void *a, const void *b)
{
    const struct worker *x = (const struct worker *)a;
    const struct worker *y = (const struct worker *)b;

    switch (g_sort_mode) {
        case SORT_AGE: {
            const apr_time_t ax = worker_age_us(x, g_sort_now_us);
            const apr_time_t ay = worker_age_us(y, g_sort_now_us);
            /* Newest first: smaller age means more recent. */
            if (ax < ay) return -1;
            if (ax > ay) return 1;
            break;
        }
        case SORT_CLIENT: {
            const bool xe = x->client[0] == '\0';
            const bool ye = y->client[0] == '\0';
            if (xe != ye) return xe ? 1 : -1;
            if (!xe) {
                int c = strcmp(x->client, y->client);
                if (c != 0) return c;
            }
            break;
        }
        case SORT_VHOST: {
            const bool xe = x->vhost[0] == '\0';
            const bool ye = y->vhost[0] == '\0';
            if (xe != ye) return xe ? 1 : -1;
            if (!xe) {
                int c = strcmp(x->vhost, y->vhost);
                if (c != 0) return c;
            }
            break;
        }
        case SORT_SLOT:
        default:
            break;
    }

    return cmp_slot(x, y);
}

static int cmp_recent(const void *a, const void *b)
{
    const struct worker *x = (const struct worker *)a;
    const struct worker *y = (const struct worker *)b;
    if (x->last_used < y->last_used) return 1;
    if (x->last_used > y->last_used) return -1;
    return cmp_slot(x, y);
}

static const char *sort_name(enum sort_mode mode)
{
    switch (mode) {
        case SORT_AGE: return "age";
        case SORT_CLIENT: return "client";
        case SORT_VHOST: return "vhost";
        case SORT_SLOT:
        default: return "slot";
    }
}

static bool parse_sort(const char *s, enum sort_mode *out)
{
    if (strcmp(s, "slot") == 0) {
        *out = SORT_SLOT;
        return true;
    }
    if (strcmp(s, "age") == 0) {
        *out = SORT_AGE;
        return true;
    }
    if (strcmp(s, "client") == 0 || strcmp(s, "ip") == 0) {
        *out = SORT_CLIENT;
        return true;
    }
    if (strcmp(s, "vhost") == 0 || strcmp(s, "host") == 0) {
        *out = SORT_VHOST;
        return true;
    }
    return false;
}


static int terminal_rows(void)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return (int)ws.ws_row;
    }
    return 24;
}

/* Fixed lines are deliberately conservative. The current main view uses
 * 9 lines before the worker rows and 3 after them; reserve one extra line so
 * the last row is never pushed into terminal scroll/wrap behaviour. With
 * -r, the recent section adds 4 fixed lines between the two row sets. */
#define MAIN_FIXED_LINES   13
#define RECENT_EXTRA_LINES 4

static int rows_for_terminal(int requested_rows, bool show_recent, int max_workers)
{
    const int term = terminal_rows();
    const int sections = show_recent ? 2 : 1;
    int available = term - MAIN_FIXED_LINES -
                    (show_recent ? RECENT_EXTRA_LINES : 0);

    if (available < sections) {
        available = sections;
    }

    int per_section = available / sections;
    if (per_section < 1) {
        per_section = 1;
    }

    if (requested_rows > 0 && per_section > requested_rows) {
        per_section = requested_rows;
    }

    if (per_section > max_workers) {
        per_section = max_workers;
    }

    return per_section;
}

static void draw(const struct shm_view *v, struct snapshot *snap,
                 double interval, int rows, bool active_only, bool show_all_slots,
                 bool show_recent, bool debug, enum sort_mode sort_mode)
{
    const apr_time_t wall_us = (apr_time_t)now_wall_us();
    const double mono = now_mono();
    uint64_t total_access = 0;
    apr_off_t total_bytes = 0;
    int counts[SERVER_NUM_STATUS] = {0};
    int active_count = 0;

    struct worker workers[MAX_SLOTS];
    struct worker recent[MAX_SLOTS];
    int na = 0;
    int nr = 0;

    const int max_workers = v->server_limit * v->thread_limit;
    rows = rows_for_terminal(rows, show_recent, max_workers);

    for (int slot = 0; slot < v->server_limit; ++slot) {
        for (int thread = 0; thread < v->thread_limit; ++thread) {
            struct worker w;
            read_worker(v, slot, thread, &w);

            if (w.status < SERVER_NUM_STATUS) {
                counts[w.status]++;
            }

            /* A scoreboard slot can exist even when there is no process
             * behind it. In Apache's scoreboard that is normally represented
             * by SERVER_DEAD ('.') and pid == 0. Such slots are capacity,
             * not workers, so they should not occupy the worker table.
             *
             * We still keep SERVER_READY ('_') rows: those are real idle
             * workers/processes and are useful in the default view. */
            const bool exists = w.pid > 0 && w.status != SERVER_DEAD;
            const bool is_active =
                exists && w.status != SERVER_READY;

            if (is_active) {
                ++active_count;
            }

            const bool show_worker = active_only ? is_active :
                (show_all_slots ? true : exists);

            if (show_worker && na < MAX_SLOTS) {
                workers[na++] = w;
            }

            if (w.request[0] && w.last_used > 0 && nr < MAX_SLOTS) {
                recent[nr++] = w;
            }

            total_access += (uint64_t)w.access_count;
            if (w.bytes_served > 0) {
                total_bytes += w.bytes_served;
            }
        }
    }

    double reqs = 0.0;
    double bytes_s = 0.0;
    if (snap->valid) {
        const double dt = mono - snap->mono;
        if (dt > 0.0 && dt < 10.0) {
            const uint64_t da = total_access >= snap->total_access
                              ? total_access - snap->total_access : 0;
            const apr_off_t db = total_bytes >= snap->total_bytes
                               ? total_bytes - snap->total_bytes : 0;
            reqs = (double)da / dt;
            bytes_s = (double)db / dt;
        }
    }
    snap->valid = true;
    snap->total_access = total_access;
    snap->total_bytes = total_bytes;
    snap->mono = mono;

    g_sort_mode = sort_mode;
    g_sort_now_us = wall_us;
    qsort(workers, na, sizeof(workers[0]), cmp_main);
    qsort(recent, nr, sizeof(recent[0]), cmp_recent);

    char btotal[32];
    char bs[32];
    fmt_bytes((double)(total_bytes > 0 ? total_bytes : 0), btotal, sizeof(btotal));
    fmt_bytes(bytes_s, bs, sizeof(bs));

    printf("\033[H\033[J");
    printf("ApacheTop scoreboard   %s\n", v->path);
    printf("Apache layout: global=%zu process=%zu worker=%zu | shm=%zu | slots=%d x %d\n",
           v->global_size, v->process_size, v->worker_size,
           v->map_size, v->server_limit, v->thread_limit);

    printf("\nRequests: %-12" PRIu64
           "  Req/s: %-8.2f  Bytes: %-11s  Bytes/s: %-11s  Active: %d\n",
           total_access, reqs, btotal, bs, active_count);

    printf("States:  _ wait=%-3d  R=%-3d  W=%-3d  K=%-3d  L=%-3d  D=%-3d  C=%-3d  G=%-3d\n",
           counts[SERVER_READY], counts[SERVER_BUSY_READ],
           counts[SERVER_BUSY_WRITE], counts[SERVER_BUSY_KEEPALIVE],
           counts[SERVER_BUSY_LOG], counts[SERVER_BUSY_DNS],
           counts[SERVER_CLOSING], counts[SERVER_GRACEFUL]);

    const char *view_name = active_only ? "ACTIVE REQUESTS" :
                             (show_all_slots ? "ALL SCOREBOARD SLOTS" : "WORKERS / SCOREBOARD");
    printf("\n%s (sort: %s)\n", view_name, sort_name(sort_mode));
    printf("%-5s %-8s %-4s %-8s %-22s %-24s %-48s\n",
           "SLOT", "PID", "STAT", "AGE", "CLIENT", "VHOST", "REQUEST");
    printf("%-5s %-8s %-4s %-8s %-22s %-24s %-48s\n",
           "-----", "--------", "----", "--------", "----------------------",
           "------------------------", "------------------------------------------------");

    int shown = na < rows ? na : rows;
    for (int i = 0; i < shown; ++i) {
        struct worker *w = &workers[i];
        char age[32];
        const apr_time_t age_us =
            w->start_time > 0 ? wall_us - w->start_time : 0;
        fmt_age_us(age_us, age, sizeof(age));
        printf("%-5d %-8d %-4c %-8s %-22.22s %-24.24s %-48.48s\n",
               w->slot, (int)w->pid, status_char(w->status), age,
               w->client[0] ? w->client : "-",
               w->vhost[0] ? w->vhost : "-",
               w->request[0] ? w->request : "-");
    }
    if (shown == 0) {
        printf("(no active requests right now)\n");
    }

    if (show_recent) {
        printf("\nRECENT / LAST REQUESTS\n");
        printf("%-5s %-8s %-4s %-8s %-22s %-24s %-48s\n",
               "SLOT", "PID", "STAT", "AGE", "CLIENT", "VHOST", "REQUEST");
        printf("%-5s %-8s %-4s %-8s %-22s %-24s %-48s\n",
               "-----", "--------", "----", "--------", "----------------------",
               "------------------------", "------------------------------------------------");

        shown = nr < rows ? nr : rows;
        for (int i = 0; i < shown; ++i) {
            struct worker *w = &recent[i];
            char age[32];
            const apr_time_t age_us =
                w->last_used > 0 ? wall_us - w->last_used : 0;
            fmt_age_us(age_us, age, sizeof(age));
            printf("%-5d %-8d %-4c %-8s %-22.22s %-24.24s %-48.48s\n",
                   w->slot, (int)w->pid, status_char(w->status), age,
                   w->client[0] ? w->client : "-",
                   w->vhost[0] ? w->vhost : "-",
                   w->request[0] ? w->request : "-");
        }
    }

    printf("\nLegend: R read client | W processing | K keepalive | L logging | D DNS | C closing | G graceful\n");
    printf("Refresh: %.1fs   Ctrl-C quits\n", interval);

    if (debug) {
        const size_t expected = APR_SHM_META_SIZE + v->scoreboard_size;
        printf("\nDEBUG: APR metadata=%zu | expected file=%zu | actual=%zu | layout=OK\n",
               (size_t)APR_SHM_META_SIZE, expected, v->map_size);
    }
    fflush(stdout);
}

static void print_version(void)
{
    printf("apachetop %s\\n", APACHETOP_VERSION);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-f /dev/shm/ShM....] [-i seconds] [-t seconds] [-n rows] [-s sort] [-a] [-1] [-r] [-d]\n"
        "  -f PATH   explicit scoreboard file; default auto-detect /dev/shm/ShM.*\n"
        "  -i SEC    refresh interval, default 1.0\n"
        "  -t SEC    alias for -i (refresh interval)\n"
        "  -n ROWS   maximum rows per section; default auto-fits terminal height\n"
        "  -s SORT   sort main workers: slot, age, client/ip, vhost/host (default: slot)\n"
        "  -a        show only active workers\n"
        "  -A        include all scoreboard slots, including unused/dead PID=0 slots\n"
        "  -1        display once and exit\n"
        "  -r        show recent/last-request section\n"
        "  -d        show calculated header/APR layout information\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *forced = NULL;
    double interval = 1.0;
    int rows = 0; /* auto-fit to terminal height */
    bool once = false;
    bool active_only = false;
    bool show_all_slots = false;
    bool recent = false;
    bool debug = false;
    enum sort_mode sort_mode = SORT_SLOT;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        print_version();
        return 0;
    }

    int opt;
    while ((opt = getopt(argc, argv, "f:i:t:n:s:1aArdhv")) != -1) {
        switch (opt) {
            case 'f':
                forced = optarg;
                break;
            case 'i':
            case 't':
                interval = atof(optarg);
                if (interval < 0.1) interval = 0.1;
                break;
            case 'n':
                rows = atoi(optarg);
                if (rows < 1) rows = 1;
                if (rows > MAX_VISIBLE) rows = MAX_VISIBLE;
                break;
            case '1':
                once = true;
                break;
            case 's':
                if (!parse_sort(optarg, &sort_mode)) {
                    fprintf(stderr, "Invalid sort '%s'. Use slot, age, client, or vhost.\n", optarg);
                    return 2;
                }
                break;
            case 'a':
                active_only = true;
                show_all_slots = false;
                break;
            case 'A':
                show_all_slots = true;
                active_only = false;
                break;
            case 'r':
                recent = true;
                break;
            case 'd':
                debug = true;
                break;
            case 'v':
                print_version();
                return 0;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct snapshot snap = {0};
    char path[PATH_MAX];

    while (!stop_flag) {
        if (!choose_scoreboard(path, sizeof(path), forced)) {
            fprintf(stderr, "No usable Apache scoreboard found in /dev/shm/ShM.*\n");
            return 1;
        }

        struct shm_view view;
        const int rc = open_view(path, &view, debug);
        if (rc != 0) {
            if (once || forced) {
                errno = -rc;
                fprintf(stderr, "Cannot open scoreboard %s: %s\n",
                        path, strerror(errno));
                return 1;
            }
            usleep(250000);
            continue;
        }

        draw(&view, &snap, interval, rows, active_only, show_all_slots, recent, debug, sort_mode);
        close_view(&view);

        if (once) {
            break;
        }

        struct timespec ts;
        ts.tv_sec = (time_t)interval;
        ts.tv_nsec = (long)((interval - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }

    return 0;
}