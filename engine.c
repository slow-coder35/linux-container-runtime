/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 16384
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int log_read_fd;      /* for pipe-based logging */
    void *stack_ptr;      /* track allocated stack for cleanup */
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_code;        /* for blocking run command */
    int exit_signal;      /* signal that killed process */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    int rc;
    rc = pthread_mutex_lock(&buffer->mutex);
    if (rc != 0)
        return -1;

    while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        rc = pthread_cond_wait(&buffer->not_full, &buffer->mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -2;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    int rc;
    rc = pthread_mutex_lock(&buffer->mutex);
    if (rc != 0)
        return -1;

    while (buffer->count == 0 && !buffer->shutting_down) {
        rc = pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&buffer->mutex);
            return -1;
        }
    }

    if (buffer->count == 0) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return -2;
        }
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc, fd;
    char log_path[PATH_MAX];
    ssize_t written;

    while (1) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == -2) {
            break;
        }
        if (rc != 0) {
            continue;
        }

        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("open log file");
            continue;
        }

        written = write(fd, item.data, item.length);
        if (written < 0) {
            perror("write to log file");
        }
        close(fd);
    }

    return NULL;
}

/*
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t config;
    char *shell_argv[] = {"/bin/sh", "-c", config.command, NULL};

    /* Copy config locally so parent can free it */
    memcpy(&config, (const child_config_t *)arg, sizeof(config));
    
    if (dup2(config.log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(config.log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(config.log_write_fd);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        if (errno != EBUSY) {
            perror("mount /proc");
        }
    }

    if (chdir(config.rootfs) < 0) {
        perror("chdir");
        return 1;
    }

    if (chroot(config.rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (setpriority(PRIO_PROCESS, 0, config.nice_value) != 0) {
        perror("setpriority");
    }

    if (execvp(shell_argv[0], shell_argv) < 0) {
        perror("execvp");
        return 1;
    }

    return 0;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static volatile int g_should_stop = 0;
static volatile int g_sigchld_received = 0;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        g_should_stop = 1;
    } else if (signum == SIGCHLD) {
        g_sigchld_received = 1;
    }
}

static container_record_t *container_find(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (strcmp(rec->id, id) == 0)
            return rec;
        rec = rec->next;
    }
    return NULL;
}

static void container_add(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next = ctx->containers;
    ctx->containers = rec;
}

static char *container_list_to_string(supervisor_ctx_t *ctx)
{
    static char buf[8192];
    container_record_t *rec;
    size_t pos = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%-15s %-8s %-12s %-10s %-20s\n",
                     "ID", "PID", "STATE", "UPTIME", "EXIT");
    for (rec = ctx->containers; rec; rec = rec->next) {
        time_t uptime = time(NULL) - rec->started_at;
        char exit_info[32];

        if (rec->state == CONTAINER_EXITED) {
            snprintf(exit_info, sizeof(exit_info), "code:%d", rec->exit_code);
        } else if (rec->state == CONTAINER_KILLED) {
            snprintf(exit_info, sizeof(exit_info), "sig:%d", rec->exit_signal);
        } else if (rec->state == CONTAINER_STOPPED) {
            snprintf(exit_info, sizeof(exit_info), "stopped");
        } else {
            snprintf(exit_info, sizeof(exit_info), "-");
        }

        pos += snprintf(buf + pos, sizeof(buf) - pos, "%-15s %-8d %-12s %-10lds %-20s\n",
                        rec->id, rec->host_pid,
                        state_to_string(rec->state),
                        uptime, exit_info);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    return buf;
}

/*
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc, client_fd;
    struct sockaddr_un addr;
    struct sockaddr_un client_addr;
    socklen_t client_addr_len;
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1) open /dev/container_monitor */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
        ctx.monitor_fd = -1;
    }

    /* 2) create the control socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctx.server_fd);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        close(ctx.server_fd);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    mkdir(LOG_DIR, 0755);

    /* 3) install signal handling */
    signal(SIGCHLD, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 4) spawn the logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        close(ctx.server_fd);
        if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    fprintf(stderr, "Supervisor running for base-rootfs: %s\n", rootfs);

    /* 5) enter the supervisor event loop */
    while (!g_should_stop) {
        pid_t wpid;
        int status;
        container_record_t *rec;

        /* Reap children if SIGCHLD was received */
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
                pthread_mutex_lock(&ctx.metadata_lock);
                for (rec = ctx.containers; rec; rec = rec->next) {
                    if (rec->host_pid == wpid) {
                        if (WIFEXITED(status)) {
                            rec->state = CONTAINER_EXITED;
                            rec->exit_code = WEXITSTATUS(status);
                        } else if (WIFSIGNALED(status)) {
                            rec->state = CONTAINER_KILLED;
                            rec->exit_signal = WTERMSIG(status);
                        }
                        if (ctx.monitor_fd >= 0) {
                            unregister_from_monitor(ctx.monitor_fd, rec->id, wpid);
                        }
                        /* Close pipe FDs if any */
                        if (rec->log_read_fd >= 0) {
                            close(rec->log_read_fd);
                            rec->log_read_fd = -1;
                        }
                        /* Free allocated stack memory */
                        if (rec->stack_ptr) {
                            free(rec->stack_ptr);
                            rec->stack_ptr = NULL;
                        }
                        break;
                    }
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
        }

        client_addr_len = sizeof(client_addr);
        client_fd = accept(ctx.server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        n = recv(client_fd, &req, sizeof(req), 0);
        if (n != sizeof(req)) {
            close(client_fd);
            continue;
        }

        memset(&resp, 0, sizeof(resp));
        resp.status = 0;

        /* Command dispatch */
        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            /* Check for duplicate container ID */
            pthread_mutex_lock(&ctx.metadata_lock);
            if (container_find(&ctx, req.container_id) != NULL) {
                snprintf(resp.message, sizeof(resp.message), "Container %s already exists", req.container_id);
                resp.status = 1;
                pthread_mutex_unlock(&ctx.metadata_lock);
            } else {
                pthread_mutex_unlock(&ctx.metadata_lock);

                /* Allocate container record */
                rec = malloc(sizeof(*rec));
                if (!rec) {
                    snprintf(resp.message, sizeof(resp.message), "Memory allocation failed");
                    resp.status = 1;
                } else {
                    char *stack = malloc(STACK_SIZE);
                    if (!stack) {
                        snprintf(resp.message, sizeof(resp.message), "Stack allocation failed");
                        resp.status = 1;
                        free(rec);
                    } else {
                        int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
                        child_config_t *child_cfg;
                        pid_t cpid;

                        child_cfg = malloc(sizeof(*child_cfg));
                        if (!child_cfg) {
                            snprintf(resp.message, sizeof(resp.message), "Child config allocation failed");
                            resp.status = 1;
                            free(stack);
                            free(rec);
                        } else {
                            /* Setup child config */
                            char log_path[PATH_MAX];
                            int log_fd;
                            
                            strncpy(child_cfg->id, req.container_id, sizeof(child_cfg->id) - 1);
                            strncpy(child_cfg->rootfs, req.rootfs, sizeof(child_cfg->rootfs) - 1);
                            strncpy(child_cfg->command, req.command, sizeof(child_cfg->command) - 1);
                            child_cfg->nice_value = req.nice_value;
                            
                            /* Open log file for child's stdout/stderr */
                            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
                            log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            if (log_fd < 0) {
                                snprintf(resp.message, sizeof(resp.message), "Failed to open log file: %s", strerror(errno));
                                resp.status = 1;
                                free(child_cfg);
                                free(stack);
                                free(rec);
                            } else {
                                fprintf(stderr, "[supervisor] Opened log file %s fd=%d for container %s\n", log_path, log_fd, req.container_id);
                                child_cfg->log_write_fd = log_fd;

                                /* Clone child process */
                                cpid = clone(child_fn, stack + STACK_SIZE, flags, (void *)child_cfg);

                                if (cpid < 0) {
                                    snprintf(resp.message, sizeof(resp.message), "Clone failed: %s", strerror(errno));
                                    resp.status = 1;
                                    close(log_fd);
                                    free(child_cfg);
                                    free(stack);
                                    free(rec);
                                } else {
                                    /* Child has now copied config, safe to free parent's copy */
                                    free(child_cfg);
                                    /* Parent must close log_fd since child inherited it */
                                    close(log_fd);

                                    /* Setup and add container record */
                                    memset(rec, 0, sizeof(*rec));
                                    rec->log_read_fd = -1;  /* initialize to invalid FD */
                                    strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
                                    rec->host_pid = cpid;
                                    rec->started_at = time(NULL);
                                    rec->state = CONTAINER_RUNNING;
                                    rec->soft_limit_bytes = req.soft_limit_bytes;
                                    rec->hard_limit_bytes = req.hard_limit_bytes;
                                    rec->stack_ptr = stack;  /* track stack for cleanup */
                                    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);

                                    /* Register with monitor if available */
                                    if (ctx.monitor_fd >= 0) {
                                        register_with_monitor(ctx.monitor_fd, req.container_id, cpid,
                                                              req.soft_limit_bytes, req.hard_limit_bytes);
                                    }

                                    pthread_mutex_lock(&ctx.metadata_lock);
                                    container_add(&ctx, rec);
                                    pthread_mutex_unlock(&ctx.metadata_lock);

                                    /* For CMD_RUN: wait for child to exit and return exit code */
                                    if (req.kind == CMD_RUN) {
                                        int wstatus;
                                        pid_t wpid = waitpid(cpid, &wstatus, 0);
                                        if (wpid > 0) {
                                            pthread_mutex_lock(&ctx.metadata_lock);
                                            if (WIFEXITED(wstatus)) {
                                                rec->state = CONTAINER_EXITED;
                                                rec->exit_code = WEXITSTATUS(wstatus);
                                                resp.exit_code = rec->exit_code;
                                                snprintf(resp.message, sizeof(resp.message), "Container %s exited with code %d",
                                                         req.container_id, rec->exit_code);
                                            } else if (WIFSIGNALED(wstatus)) {
                                                rec->state = CONTAINER_KILLED;
                                                rec->exit_signal = WTERMSIG(wstatus);
                                                resp.exit_signal = rec->exit_signal;
                                                snprintf(resp.message, sizeof(resp.message), "Container %s killed by signal %d",
                                                         req.container_id, rec->exit_signal);
                                            }
                                            if (ctx.monitor_fd >= 0) {
                                                unregister_from_monitor(ctx.monitor_fd, rec->id, cpid);
                                            }
                                            pthread_mutex_unlock(&ctx.metadata_lock);
                                        }
                                    } else {
                                        snprintf(resp.message, sizeof(resp.message), "Container %s started (PID %d)",
                                                 req.container_id, cpid);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (req.kind == CMD_PS) {
            strncpy(resp.message, container_list_to_string(&ctx), sizeof(resp.message) - 1);
        } else if (req.kind == CMD_LOGS) {
            int log_fd;
            char log_path[PATH_MAX];
            char *logbuf;
            ssize_t rd;

            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
            log_fd = open(log_path, O_RDONLY);
            if (log_fd < 0) {
                snprintf(resp.message, sizeof(resp.message), "No logs for container %s", req.container_id);
            } else {
                logbuf = malloc(CONTROL_MESSAGE_LEN - 1);
                if (logbuf) {
                    rd = read(log_fd, logbuf, CONTROL_MESSAGE_LEN - 2);
                    if (rd > 0) {
                        logbuf[rd] = 0;
                        strncpy(resp.message, logbuf, sizeof(resp.message) - 1);
                    } else {
                        snprintf(resp.message, sizeof(resp.message), "[empty log]");
                    }
                    free(logbuf);
                } else {
                    snprintf(resp.message, sizeof(resp.message), "Memory allocation failed");
                }
                close(log_fd);
            }
        } else if (req.kind == CMD_STOP) {
            pthread_mutex_lock(&ctx.metadata_lock);
            rec = container_find(&ctx, req.container_id);
            if (!rec) {
                snprintf(resp.message, sizeof(resp.message), "Container %s not found", req.container_id);
                resp.status = 1;
            } else {
                if (kill(rec->host_pid, SIGTERM) < 0) {
                    snprintf(resp.message, sizeof(resp.message), "Failed to stop container: %s", strerror(errno));
                    resp.status = 1;
                } else {
                    rec->state = CONTAINER_STOPPED;
                    snprintf(resp.message, sizeof(resp.message), "Container %s stopped", req.container_id);
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        } else {
            snprintf(resp.message, sizeof(resp.message), "Unknown command");
            resp.status = 1;
        }

        if (send(client_fd, &resp, sizeof(resp), 0) < 0) {
            perror("send response");
        }
        close(client_fd);
    }

    /* Clean up resources */
    close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    /* Signal and clean up all running containers */
    {
        container_record_t *rec;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (rec = ctx.containers; rec; rec = rec->next) {
            if (rec->state == CONTAINER_RUNNING) {
                kill(rec->host_pid, SIGTERM);
            }
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    while (ctx.containers) {
        container_record_t *tmp = ctx.containers;
        ctx.containers = ctx.containers->next;
        /* Close any open FDs */
        if (tmp->log_read_fd >= 0) {
            close(tmp->log_read_fd);
        }
        /* Free allocated stack memory */
        if (tmp->stack_ptr) {
            free(tmp->stack_ptr);
        }
        free(tmp);
    }

    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (send(sock, req, sizeof(*req), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    n = recv(sock, &resp, sizeof(resp), 0);
    if (n > 0) {
        printf("%s\n", resp.message);
    }

    close(sock);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * The supervisor responds with container metadata.
     * Format is kept simple for demos and debugging.
     */
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
