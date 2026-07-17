#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>

#include <wlr/util/log.h>

/* Pull in the minimum needed from wlroots for XWayland (WLR_USE_UNSTABLE set via the Makefile). */
#include <wlr/xwayland.h>
#include <wlr/backend.h>
#include <wlr/backend/x11.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/multi.h>

/* wlr_backend_autocreate() always wraps the chosen backend in a multi-backend, so we must walk its children to detect X11/Wayland nesting. */
static void mark_if_nested(struct wlr_backend *backend, void *data) {
    bool *found = data;
    if (wlr_backend_is_x11(backend) || wlr_backend_is_wl(backend)) {
        *found = true;
    }
}

bool tinywl_backend_is_nested(struct wlr_backend *backend) {
    if (wlr_backend_is_x11(backend) || wlr_backend_is_wl(backend)) {
        return true;
    }
    if (wlr_backend_is_multi(backend)) {
        bool found = false;
        wlr_multi_for_each_backend(backend, mark_if_nested, &found);
        return found;
    }
    return false;
}

#include "tinywl.h"
#include "services.h"

/* Maximum number of services we track */
#define MAX_SERVICES 32

struct service_entry {
    pid_t   pid;
    char    name[64];
};

struct tinywl_services {
    struct service_entry entries[MAX_SERVICES];
    int                  count;

    /* XWayland managed by wlroots */
    struct wlr_xwayland         *xwayland;
    struct wl_listener           xwayland_ready;

    struct tinywl_server        *server;

    /* Set to true once XWayland emits the "ready" signal */
    bool                         xwayland_ready_flag;
};

/* Internal helpers */

/*
 * record_pid – store a child PID so we can kill it on shutdown.
 */
static void record_pid(struct tinywl_services *svc, pid_t pid, const char *name) {
    /* Reuse a freed slot (already reaped, pid <= 0) if one exists, so
     * short-lived ad-hoc entries (menu-launched apps, screenshots) don't
     * permanently eat into the fixed-size pool. */
    for (int i = 0; i < svc->count; i++) {
        if (svc->entries[i].pid <= 0) {
            svc->entries[i].pid = pid;
            strncpy(svc->entries[i].name, name, sizeof(svc->entries[i].name) - 1);
            svc->entries[i].name[sizeof(svc->entries[i].name) - 1] = '\0';
            return;
        }
    }
    if (svc->count >= MAX_SERVICES) {
        wlr_log(WLR_ERROR, "services: too many tracked processes, cannot record %s", name);
        return;
    }
    svc->entries[svc->count].pid = pid;
    strncpy(svc->entries[svc->count].name, name, sizeof(svc->entries[0].name) - 1);
    svc->entries[svc->count].name[sizeof(svc->entries[0].name) - 1] = '\0';
    svc->count++;
}

/* tinywl_services_try_reap: reap only PIDs we recorded ourselves. Called from
 * the compositor's SIGCHLD handler. Deliberately targeted (waitpid on each
 * known pid) rather than waitpid(-1, ...), which would also reap children
 * other subsystems own (e.g. wlroots forks and waits on its own Xwayland
 * child; a blanket wait races with that and makes wlroots think Xwayland
 * died, tearing the whole X11 connection down). */
void tinywl_services_try_reap(struct tinywl_services *svc) {
    if (!svc)
        return;
    for (int i = 0; i < svc->count; i++) {
        if (svc->entries[i].pid <= 0)
            continue;
        int status;
        pid_t r = waitpid(svc->entries[i].pid, &status, WNOHANG);
        if (r == svc->entries[i].pid) {
            /* Reaped; stop tracking so a later call doesn't wait on a
             * potentially-recycled pid. */
            svc->entries[i].pid = -1;
        }
    }
}

void tinywl_services_track_pid(struct tinywl_services *svc, pid_t pid, const char *name) {
    if (!svc || pid <= 0)
        return;
    record_pid(svc, pid, name ? name : "ad-hoc");
}

/* spawn_service: fork+exec argv (NULL-terminated). Returns the child PID, or -1 on failure. */
static pid_t spawn_service(const char *name, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "services: fork() failed for %s: %s", name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* Unblock all signals: wl_event_loop_add_signal() blocks SIGINT/
         * SIGTERM/SIGCHLD at the process level so it can deliver them via
         * the wayland event loop, and fork() inherits that blocked mask.
         * Spawned services must not inherit it — e.g. the polkit agent's
         * own SIGCHLD-based child-watch handling breaks if SIGCHLD stays
         * blocked, which is what caused deny clicks to hang. */
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        sigprocmask(SIG_SETMASK, &empty_mask, NULL);

        /* Child: redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* Start new process group so SIGTERM doesn't cascade from compositor */
        setsid();
        execvp(argv[0], argv);
        /* execvp only returns on error */
        _exit(127);
    }
    wlr_log(WLR_INFO, "services: started %s (pid %d)", name, pid);
    return pid;
}

/* program_exists: check whether a binary is on PATH via a `which` invocation. */
static bool program_exists(const char *prog) {
    /* Fast path: try common absolute locations */
    char buf[256];
    const char *dirs[] = {
        "/usr/bin", "/usr/local/bin", "/bin",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        snprintf(buf, sizeof(buf), "%s/%s", dirs[i], prog);
        if (access(buf, X_OK) == 0)
            return true;
    }
    return false;
}

/* find_polkit_agent: search for a known polkit authentication agent binary; returns a malloc'd path or NULL. */
static char *find_polkit_agent(void) {
    /* Preferred agents in order */
    const char *agents[] = {
        "/usr/libexec/xfce4-polkit-authentication-agent-1",
        NULL
    };
    for (int i = 0; agents[i]; i++) {
        if (access(agents[i], X_OK) == 0)
            return strdup(agents[i]);
    }
    return NULL;
}

/* D-Bus session bus */

static pid_t start_dbus_session(void) {
    /* If a bus is already running (e.g. user session via systemd), honour it */
    if (getenv("DBUS_SESSION_BUS_ADDRESS")) {
        wlr_log(WLR_INFO, "services: D-Bus session already available at %s",
                getenv("DBUS_SESSION_BUS_ADDRESS"));
        return 0; /* 0 = not our child */
    }

    /* Create a pipe to read the bus address from dbus-daemon */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        wlr_log(WLR_ERROR, "services: pipe() failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "services: fork() for dbus-daemon failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        /* Redirect stdout to the write end of the pipe so we get the address */
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        setsid();
        execlp("dbus-daemon", "dbus-daemon",
               "--session", "--print-address", "--nofork",
               (char *)NULL);
        _exit(127);
    }

    /* Parent: read the bus address */
    close(pipefd[1]);
    char addr[512] = {0};
    ssize_t n = 0, total = 0;
    /* Give dbus-daemon up to 3 seconds to print its address */
    for (int tries = 0; tries < 30; tries++) {
        n = read(pipefd[0], addr + total, sizeof(addr) - total - 1);
        if (n > 0) {
            total += n;
            /* Check if we have a complete line */
            if (memchr(addr, '\n', total))
                break;
        } else if (n == 0) {
            break; /* EOF */
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    close(pipefd[0]);

    /* Strip trailing whitespace/newline */
    for (ssize_t i = total - 1; i >= 0; i--) {
        if (addr[i] == '\n' || addr[i] == '\r' || addr[i] == ' ')
            addr[i] = '\0';
        else
            break;
    }

    if (strlen(addr) == 0) {
        wlr_log(WLR_ERROR, "services: dbus-daemon did not print an address");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }

    setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
    wlr_log(WLR_INFO, "services: D-Bus session bus at %s (pid %d)", addr, pid);
    return pid;
}

/* XWayland  */

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
    struct tinywl_services *svc =
        wl_container_of(listener, svc, xwayland_ready);

    const char *display_name = svc->xwayland->display_name;
    if (display_name) {
        setenv("DISPLAY", display_name, 1);
        wlr_log(WLR_INFO, "services: XWayland ready, DISPLAY=%s", display_name);
    }

    /* wlr_xwayland_set_seat must be called from the ready handler to wire up keyboard/pointer focus for X11 windows. */
    wlr_xwayland_set_seat(svc->xwayland, svc->server->seat);

    svc->xwayland_ready_flag = true;

    /* xfsettingsd is spawned only here, once DISPLAY points at our own XWayland, and skipped when nested (see tinywl_services_init). */
    bool nested = tinywl_backend_is_nested(svc->server->backend);
    if (!nested && program_exists("xfsettingsd")) {
        char *argv[] = { "xfsettingsd", NULL };
        pid_t pid = spawn_service("xfsettingsd", argv);
        if (pid > 0)
            record_pid(svc, pid, "xfsettingsd");
    }
}

static bool start_xwayland(struct tinywl_services *svc) {
    struct tinywl_server *server = svc->server;

    /* wlr_xwayland_create() starts Xwayland immediately (lazy=false) so DISPLAY is set before dependent services launch. */
    svc->xwayland = wlr_xwayland_create(server->wl_display,
                                         server->compositor,
                                         false /* lazy */);
    if (!svc->xwayland) {
        wlr_log(WLR_ERROR, "services: wlr_xwayland_create() failed "
                "(is Xwayland installed and wlroots built with xwayland support?)");
        return false;
    }

    svc->xwayland_ready.notify = handle_xwayland_ready;
    wl_signal_add(&svc->xwayland->events.ready, &svc->xwayland_ready);

    wlr_log(WLR_INFO, "services: XWayland initialised");
    return true;
}

/* GVFS */

static void start_gvfs(struct tinywl_services *svc) {
    /* gvfsd handles GVFS mounts (smb://, sftp://, ...); gvfs-udisks2-volume-monitor covers physical drives. */
    if (!program_exists("gvfsd")) {
        wlr_log(WLR_INFO, "services: gvfsd not found, skipping GVFS");
        return;
    }

    char *argv_gvfsd[] = { "gvfsd", NULL };
    pid_t pid = spawn_service("gvfsd", argv_gvfsd);
    if (pid > 0)
        record_pid(svc, pid, "gvfsd");

    /* Short delay so gvfsd registers on D-Bus before we start monitors */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    if (program_exists("gvfsd-fuse")) {
        /* Mount point: ~/.gvfs (created if missing) */
        const char *home = getenv("HOME");
        if (home) {
            char mntpath[512];
            snprintf(mntpath, sizeof(mntpath), "%s/.gvfs", home);
            /* mkdir -p equivalent */
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", mntpath);
            if (system(cmd) != 0) {
                /* Non-fatal: mount point may already exist or mkdir failed;
                 * gvfsd-fuse spawn below will fail gracefully if so. */
            }

            char *argv_fuse[] = { "gvfsd-fuse", mntpath, NULL };
            pid_t fpid = spawn_service("gvfsd-fuse", argv_fuse);
            if (fpid > 0)
                record_pid(svc, fpid, "gvfsd-fuse");
        }
    }

    if (program_exists("gvfs-udisks2-volume-monitor")) {
        char *argv_udisks[] = { "gvfs-udisks2-volume-monitor", NULL };
        pid_t upid = spawn_service("gvfs-udisks2-volume-monitor", argv_udisks);
        if (upid > 0)
            record_pid(svc, upid, "gvfs-udisks2-volume-monitor");
    }
}

/* Polkit authentication agent */

/* is_process_running_named: scan /proc for a process whose comm matches
 * `name` (comm is truncated to 15 chars by the kernel, so compare only up
 * to that length). Used to avoid spawning a second polkit agent on top of
 * one left over from a previous session — two agents racing to answer the
 * same authorization request is what causes the "deny does nothing /
 * freezes" symptom, since the second one hangs waiting on a D-Bus reply
 * for a check the first agent already resolved. */
static bool is_process_running_named(const char *name) {
    DIR *proc = opendir("/proc");
    if (!proc)
        return false;

    char comm_name[16];
    snprintf(comm_name, sizeof(comm_name), "%s", name);

    struct dirent *entry;
    bool found = false;
    while (!found && (entry = readdir(proc)) != NULL) {
        const char *p = entry->d_name;
        bool all_digits = *p != '\0';
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                all_digits = false;
                break;
            }
        }
        if (!all_digits)
            continue;

        char path[6 + NAME_MAX + 5 + 1]; /* "/proc/" + d_name + "/comm" + NUL */
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char comm[32] = {0};
        if (fgets(comm, sizeof(comm), f)) {
            size_t len = strlen(comm);
            if (len > 0 && comm[len - 1] == '\n')
                comm[len - 1] = '\0';
            if (strncmp(comm, comm_name, sizeof(comm_name) - 1) == 0)
                found = true;
        }
        fclose(f);
    }
    closedir(proc);
    return found;
}

static void start_polkit(struct tinywl_services *svc) {
    char *agent = find_polkit_agent();
    if (!agent) {
        wlr_log(WLR_INFO, "services: no polkit authentication agent found, skipping");
        return;
    }

    /* basename of the agent path, truncated the same way /proc/PID/comm is */
    const char *base = strrchr(agent, '/');
    base = base ? base + 1 : agent;
    if (is_process_running_named(base)) {
        wlr_log(WLR_INFO, "services: a polkit authentication agent is already "
                "running, not starting a second one");
        free(agent);
        return;
    }

    char *argv[] = { agent, NULL };
    pid_t pid = spawn_service("polkit-agent", argv);
    if (pid > 0)
        record_pid(svc, pid, "polkit-agent");

    free(agent);
}

/* PulseAudio */
static void start_audio(struct tinywl_services *svc) {
    /* Start PulseAudio if its socket doesn't already exist. */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        runtime = "/run/user/1000"; /* reasonable fallback */
    
    /* Check for existing PulseAudio socket */
    char pa_socket[512];
    snprintf(pa_socket, sizeof(pa_socket), "%s/pulse/native", runtime);
    if (access(pa_socket, F_OK) == 0) {
        wlr_log(WLR_INFO, "services: PulseAudio socket already exists, not starting audio daemon");
        return;
    }
    
    /* Start PulseAudio */
    if (program_exists("pulseaudio")) {
        char *argv_pa[] = { "pulseaudio", "--start", "--log-target=syslog", NULL };
        pid_t pid = spawn_service("pulseaudio", argv_pa);
        if (pid > 0)
            record_pid(svc, pid, "pulseaudio");
    } else {
        wlr_log(WLR_INFO, "services: pulseaudio not found, skipping audio");
    }
}

/* dconf service (GSettings backend) */

static void start_dconf(struct tinywl_services *svc) {
    if (!program_exists("dconf-service"))
        return;
    char *argv[] = { "dconf-service", NULL };
    pid_t pid = spawn_service("dconf-service", argv);
    if (pid > 0)
        record_pid(svc, pid, "dconf-service");
}

/* Public API */

struct tinywl_services *tinywl_services_init(struct tinywl_server *server) {
    struct tinywl_services *svc = calloc(1, sizeof(*svc));
    if (!svc)
        return NULL;

    svc->server = server;

    /* Skip per-session daemons (settings, GVFS, polkit, audio) when nested inside an existing X11 session, since the host already runs them on the shared D-Bus bus. */
    bool nested = tinywl_backend_is_nested(server->backend);
    if (nested) {
        wlr_log(WLR_INFO,
            "services: running nested inside an existing X11 session; "
            "skipping settings daemon / GVFS / polkit agent / audio "
            "daemon startup (host session already provides these)");
    }

    /*
     * 1. D-Bus session bus — must be first, everything else depends on it.
     */
    pid_t dbus_pid = start_dbus_session();
    if (dbus_pid > 0)
        record_pid(svc, dbus_pid, "dbus-daemon");
    /* dbus_pid == 0 means we reused an existing bus — that's fine */

    /* XWayland starts early so DISPLAY is available; xfsettingsd is spawned later from handle_xwayland_ready once DISPLAY is confirmed. */
    start_xwayland(svc);

    if (!nested) {
        /*
         * 3. dconf — GSettings backend (apps query it on startup)
         */
        start_dconf(svc);

        /*
         * 4. GVFS — virtual filesystem, needed for external drives / Trash
         */
        start_gvfs(svc);

        /*
         * 5. Polkit authentication agent
         */
        start_polkit(svc);

        /*
         * 6. Audio daemon (PipeWire or PulseAudio)
         */
        start_audio(svc);
    }

    return svc;
}

void tinywl_services_destroy(struct tinywl_services *svc) {
    if (!svc)
        return;

    /* Destroy XWayland via wlroots API first */
    if (svc->xwayland) {
        wl_list_remove(&svc->xwayland_ready.link);
        wlr_xwayland_destroy(svc->xwayland);
        svc->xwayland = NULL;
    }

    /* Send SIGTERM to all tracked children, then reap */
    for (int i = 0; i < svc->count; i++) {
        if (svc->entries[i].pid > 0) {
            wlr_log(WLR_INFO, "services: stopping %s (pid %d)",
                    svc->entries[i].name, svc->entries[i].pid);
            kill(svc->entries[i].pid, SIGTERM);
        }
    }

    /* Give services 2 s to exit gracefully, then SIGKILL */
    struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
    nanosleep(&ts, NULL);

    for (int i = 0; i < svc->count; i++) {
        if (svc->entries[i].pid > 0) {
            int status;
            pid_t r = waitpid(svc->entries[i].pid, &status, WNOHANG);
            if (r == 0) {
                /* Still running — SIGKILL */
                kill(svc->entries[i].pid, SIGKILL);
                waitpid(svc->entries[i].pid, NULL, 0);
            }
        }
    }

    free(svc);
}
