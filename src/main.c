#define _GNU_SOURCE

#include "protocol.h"
#include "agentos_runtime.h"
#include "control_plane.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <limits.h>

#define MAX_MESSAGE 16384
#define IO_TIMEOUT_SECONDS 2
#define REQUEST_READ_DEADLINE_MS 3000

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/* CLOCK_MONOTONIC is unaffected by changes to wall-clock time. */
static int64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Read a newline-terminated message with BOTH an inactivity timeout and
 * a total elapsed-time deadline, measured from entry to this function.
 *
 *  0: valid complete line       -1: I/O error / EOF / embedded NUL
 * -2: message too large        -3: two seconds without receiving data
 * -4: total request-read deadline exceeded (even if bytes keep arriving)
 *
 * MSG_DONTWAIT plus poll() is deliberate: SO_RCVTIMEO by itself bounds
 * each recv(), not the duration of an entire slow-drip request.
 */
static int receive_line(int fd, char *buffer, size_t capacity)
{
    size_t length = 0;
    int64_t now = monotonic_milliseconds();
    if (now < 0)
        return -1;

    const int64_t absolute_deadline = now + REQUEST_READ_DEADLINE_MS;
    int64_t idle_deadline = now + IO_TIMEOUT_SECONDS * 1000;

    for (;;) {
        now = monotonic_milliseconds();
        if (now < 0)
            return -1;
        if (now >= absolute_deadline)
            return -4;
        if (now >= idle_deadline)
            return -3;

        char ch;
        ssize_t n = recv(fd, &ch, 1, MSG_DONTWAIT);
        if (n == 0)
            return -1;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;

            int64_t wait_ms = absolute_deadline - now;
            if (idle_deadline - now < wait_ms)
                wait_ms = idle_deadline - now;

            struct pollfd pending = {.fd = fd, .events = POLLIN};
            int ready = poll(&pending, 1, (int)wait_ms);
            if (ready < 0 && errno != EINTR)
                return -1;
            if (ready > 0 && (pending.revents & POLLNVAL))
                return -1;
            /* Timeout, signal, or readiness: recompute both deadlines. */
            continue;
        }

        idle_deadline = monotonic_milliseconds();
        if (idle_deadline < 0)
            return -1;
        idle_deadline += IO_TIMEOUT_SECONDS * 1000;

        if (ch == '\0')
            return -1;
        if (ch == '\n') {
            buffer[length] = '\0';
            return 0;
        }
        if (length >= capacity - 1)
            return -2;
        buffer[length++] = ch;
    }
}

// Send all bytes, even if send() writes only some.
/* STUDENT TODO M1.2 — Reliable response delivery. */
static int send_all(int fd, const char *message)
{
    (void)fd;
    (void)message;
    errno = ENOSYS;
    return -1;
}

static int configure_client_timeouts(int fd)
{
    struct timeval timeout = {
        .tv_sec = IO_TIMEOUT_SECONDS,
        .tv_usec = 0};

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1)
    {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) == -1)
    {
        return -1;
    }

    return 0;
}

/* STUDENT TODO M1.3 — One-client request lifecycle. */
static void serve_client(int client_fd)
{
    /* Deliberate scaffold: invokes helpers only so -Werror stays clean.
     * This does NOT implement the response protocol; replace this entire
     * body with the M1.3 instructions above. */
    char request[MAX_MESSAGE + 1];
    (void)receive_line(client_fd, request, sizeof(request));
    (void)send_all(client_fd, "");
}

static int recover_socket_path(const char *socket_path);

/* Separate operator-only AF_UNIX channel. The runtime directory is private
 * (0700), control socket is 0600, and Linux peer credentials are checked.
 * It is NEVER reachable as a privileged operation on the infer socket. */
static void serve_control(int client_fd)
{
    char request[8192];
    char response[300000];
    int r=receive_line(client_fd,request,sizeof(request));
    if(r!=0 || control_handle(request,response,sizeof(response))!=0){
        (void)send_all(client_fd,"{\"version\":1,\"status\":\"error\",\"code\":\"BAD_CONTROL_REQUEST\"}\n");
        return;
    }
    (void)send_all(client_fd,response);
}

static int make_control_listener(const char *path)
{
    if(recover_socket_path(path)!=0)return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0)return -1;
    struct sockaddr_un a={0};a.sun_family=AF_UNIX;
    if(snprintf(a.sun_path,sizeof(a.sun_path),"%s",path)>= (int)sizeof(a.sun_path))goto fail;
    mode_t old=umask(0077);
    int result=bind(fd,(struct sockaddr *)&a,sizeof(a));
    int err=errno;umask(old);errno=err;
    if(result!=0)goto fail;
    if(chmod(path,0600)!=0 || listen(fd,8)!=0)goto fail_unlink;
    int flags=fcntl(fd,F_GETFL);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)!=0)goto fail_unlink;
    return fd;
fail_unlink:
    unlink(path);
fail:
    close(fd);return -1;
}

/* Every AgentOS instance owns a private runtime directory.  By default
 * it is /tmp/agentos-<effective UID>; distinct projects may override it
 * with an absolute AGENTOS_RUNTIME_DIR.  The directory must be owned by
 * this process and inaccessible to other users. */
static int prepare_runtime_paths(char *runtime_dir, size_t runtime_capacity,
                                 char *socket_path, size_t socket_capacity,
                                 char *lock_path, size_t lock_capacity)
{
    const char *configured = getenv("AGENTOS_RUNTIME_DIR");
    int length;
    if (configured != NULL && configured[0] != '\0') {
        length = snprintf(runtime_dir, runtime_capacity, "%s", configured);
    } else {
        length = snprintf(runtime_dir, runtime_capacity, "/tmp/agentos-%lu",
                          (unsigned long)geteuid());
    }
    if (length < 0 || (size_t)length >= runtime_capacity ||
        runtime_dir[0] != '/' || strcmp(runtime_dir, "/") == 0) {
        fprintf(stderr, "Invalid AGENTOS_RUNTIME_DIR: expected an absolute, non-root path.\n");
        return -1;
    }

    /* Do not follow a symlink at the runtime directory itself. */
    if (mkdir(runtime_dir, 0700) == -1 && errno != EEXIST) {
        perror("mkdir runtime directory");
        return -1;
    }
    int dir_fd = open(runtime_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd == -1) {
        perror("open runtime directory");
        return -1;
    }
    struct stat st;
    int valid = fstat(dir_fd, &st) == 0 && S_ISDIR(st.st_mode) &&
                st.st_uid == geteuid() && (st.st_mode & 07777) == 0700;
    close(dir_fd);
    if (!valid) {
        fprintf(stderr, "Runtime directory must be owned by this user with mode 0700.\n");
        return -1;
    }

    length = snprintf(socket_path, socket_capacity, "%s/agentos.sock", runtime_dir);
    if (length < 0 || (size_t)length >= socket_capacity) {
        fprintf(stderr, "Unix socket path is too long.\n");
        return -1;
    }
    length = snprintf(lock_path, lock_capacity, "%s/agentos.lock", runtime_dir);
    if (length < 0 || (size_t)length >= lock_capacity) {
        fprintf(stderr, "Lock file path is too long.\n");
        return -1;
    }
    return 0;
}

static int acquire_server_lock(const char *lock_path)
{
    int fd = open(
        lock_path,
        O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
        0600);

    if (fd == -1)
    {
        perror("open lock file");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1 ||
        (st.st_mode & 07777) != 0600) {
        fprintf(stderr, "Unsafe AgentOS lock file.\n");
        close(fd);
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1)
    {
        if (errno == EWOULDBLOCK)
        {
            fprintf(stderr,
                    "AgentOS is already running.\n");
        }
        else
        {
            perror("flock");
        }

        close(fd);
        return -1;
    }

    return fd;
}

static int recover_socket_path(const char *socket_path)
{
    struct stat st;

    if (lstat(socket_path, &st) == -1)
    {
        if (errno == ENOENT)
            return 0;

        perror("lstat socket");
        return -1;
    }

    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid())
    {
        fprintf(stderr,
                "Refusing to remove non-socket path: %s\n",
                socket_path);
        return -1;
    }

    if (unlink(socket_path) == -1)
    {
        perror("unlink stale socket");
        return -1;
    }

    printf("Recovered stale socket path.\n");
    return 0;
}

int main(void)
{
    int exit_code = EXIT_FAILURE;
    int lock_fd = -1;
    int server_fd = -1;
    int control_fd = -1;
    int owns_control = 0;
    char control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int owns_socket = 0;
    char runtime_dir[PATH_MAX];
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char lock_path[PATH_MAX];

    if (prepare_runtime_paths(runtime_dir, sizeof(runtime_dir),
                              socket_path, sizeof(socket_path),
                              lock_path, sizeof(lock_path)) == -1)
        return EXIT_FAILURE;

    if(agentos_runtime_init()!=0)return EXIT_FAILURE;

    struct sigaction action = {0};

    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1)
    {
        perror("sigaction");
        goto cleanup;
    }

    /*
     * Only one AgentOS instance can own the
     * socket at a time.
     */
    lock_fd = acquire_server_lock(lock_path);

    if (lock_fd == -1)
        goto cleanup;

    /*
     * We hold the exclusive lock, so another
     * cooperating AgentOS cannot own this path.
     */
    if (recover_socket_path(socket_path) == -1)
        goto cleanup;

    /* STUDENT TODO M1.1 — Listening endpoint.
     * Create, bind and listen on a private Unix stream socket at socket_path.
     * Keep ownership/locking rules and set owns_socket only when bound.
     * Backlog: 8. Do not modify instructor-provided control socket. */
    errno = ENOSYS;
    fprintf(stderr, "M1.1 TODO: socket/bind/listen not implemented\n");
    goto cleanup;

    /*
     * A signal can arrive after a stop-flag check but before accept().
     * A blocking accept() would then wait forever for another client.
     * Set the listening socket nonblocking and use bounded poll() waits,
     * so every shutdown request is observed even in that race window.
     */
    int flags = fcntl(server_fd, F_GETFL);
    if (flags == -1 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl listening socket");
        goto cleanup;
    }

    if(snprintf(control_path,sizeof(control_path),"%s/control.sock",runtime_dir)>=
       (int)sizeof(control_path))goto cleanup;
    control_fd=make_control_listener(control_path);
    if(control_fd<0){fprintf(stderr,"Cannot start private control socket.\n");goto cleanup;}
    owns_control=1;

    printf("AgentOS server ready!\n");
    fflush(stdout);

    exit_code = EXIT_SUCCESS;

    while (!stop_requested)
    {
        struct pollfd listeners[2] = {
            {.fd=server_fd,.events=POLLIN},
            {.fd=control_fd,.events=POLLIN}
        };
        int ready = poll(listeners, 2, 200);

        if (ready == -1)
        {
            if (errno == EINTR)
                continue;
            perror("poll listening socket");
            exit_code = EXIT_FAILURE;
            break;
        }
        if (ready == 0 || stop_requested)
            continue;
        if (listeners[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "Listening socket failed.\n");
            exit_code = EXIT_FAILURE;
            break;
        }
        if(listeners[1].revents & (POLLERR | POLLHUP | POLLNVAL)){
            fprintf(stderr,"Control socket failed.\n");exit_code=EXIT_FAILURE;break;
        }
        if(listeners[1].revents & POLLIN){
            int control_client=accept(control_fd,NULL,NULL);
            if(control_client>=0){
                struct ucred peer={0};socklen_t len=sizeof(peer);
                if(getsockopt(control_client,SOL_SOCKET,SO_PEERCRED,&peer,&len)==0 &&
                   peer.uid==geteuid()){
                    if(configure_client_timeouts(control_client)==0)
                        serve_control(control_client);
                }
                close(control_client);
            }
        }
        if (!(listeners[0].revents & POLLIN))
            continue;

        /* STUDENT TODO M1.4 — accept one infer client.
         * Keep the instructor-supplied control socket polling intact. */
        int client_fd = -1;
        errno = ENOSYS;

        if (client_fd == -1)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            perror("accept");
            exit_code = EXIT_FAILURE;
            break;
        }

        if (configure_client_timeouts(client_fd) == -1)
        {
            perror("setsockopt");
        }
        else
        {
            serve_client(client_fd);
        }

        close(client_fd);
    }

cleanup:
    if(control_fd!=-1)close(control_fd);
    if(owns_control)unlink(control_path);
    agentos_runtime_destroy();
    if (server_fd != -1)
        close(server_fd);

    if (owns_socket)
    {
        if (unlink(socket_path) == -1 &&
            errno != ENOENT)
        {
            perror("unlink socket");
            exit_code = EXIT_FAILURE;
        }
    }

    if (lock_fd != -1)
        close(lock_fd);

    printf("AgentOS server stopped.\n");

    return exit_code;
}
