/*
 * credential-server.c - Root daemon that serves git credentials over a Unix socket.
 *
 * This runs as root (PID 1 or a child of entrypoint) and reads the secret
 * from /run/secrets/gh_token. It listens on /run/git-credentials.sock and
 * responds to "get" requests with git credential protocol output.
 *
 * Security model:
 * - The token file is mode 600 root:root — only this daemon can read it
 * - The socket is mode 777 so the agent user can connect
 * - The daemon only responds to "get" operations (not "store" or "erase")
 * - Only serves credentials for github.com (prevents exfiltration to other hosts)
 * - The agent user never sees the raw token
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

#define SOCKET_PATH "/run/git-credentials.sock"
#define TOKEN_PATH "/run/secrets/gh_token"
#define MAX_TOKEN_LEN 256
#define MAX_BUF 4096

static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* Read token from secret file, strip trailing newline */
static int read_token(char *buf, size_t buflen) {
    FILE *f = fopen(TOKEN_PATH, "r");
    if (!f) {
        fprintf(stderr, "credential-server: cannot open %s: %s\n", TOKEN_PATH, strerror(errno));
        return -1;
    }
    if (!fgets(buf, buflen, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    /* Strip trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }
    return 0;
}

/* Handle a single client connection */
static void handle_client(int client_fd, const char *token) {
    char buf[MAX_BUF];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    /* Parse the request - expect "get\nhost=github.com\nprotocol=https\n\n" */
    /* First line should be "get" */
    char *line = strtok(buf, "\n");
    if (!line || strcmp(line, "get") != 0) {
        /* Only respond to "get" operations */
        const char *err = "error: only 'get' supported\n";
        send(client_fd, err, strlen(err), 0);
        close(client_fd);
        return;
    }

    /* Check for github.com host */
    int is_github = 0;
    while ((line = strtok(NULL, "\n")) != NULL) {
        if (strcmp(line, "host=github.com") == 0) {
            is_github = 1;
        }
    }

    if (!is_github) {
        const char *err = "error: only github.com credentials served\n";
        send(client_fd, err, strlen(err), 0);
        close(client_fd);
        return;
    }

    /* Send back credential protocol response */
    char response[MAX_BUF];
    snprintf(response, sizeof(response),
             "protocol=https\n"
             "host=github.com\n"
             "username=x-access-token\n"
             "password=%s\n\n",
             token);
    send(client_fd, response, strlen(response), 0);
    close(client_fd);
}

int main(void) {
    char token[MAX_TOKEN_LEN];

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    /* Read the token once at startup */
    if (read_token(token, sizeof(token)) != 0) {
        fprintf(stderr, "credential-server: failed to read token\n");
        return 1;
    }

    fprintf(stderr, "credential-server: token loaded (%zu chars)\n", strlen(token));

    /* Remove old socket if exists */
    unlink(SOCKET_PATH);

    /* Create Unix domain socket */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    /* Make socket accessible by all users in container */
    chmod(SOCKET_PATH, 0777);

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    fprintf(stderr, "credential-server: listening on %s\n", SOCKET_PATH);

    /* Signal readiness */
    FILE *ready = fopen("/run/credential-server.ready", "w");
    if (ready) {
        fprintf(ready, "ready\n");
        fclose(ready);
    }

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        handle_client(client_fd, token);
    }

    /* Cleanup */
    close(server_fd);
    unlink(SOCKET_PATH);
    /* Wipe token from memory */
    memset(token, 0, sizeof(token));
    fprintf(stderr, "credential-server: shutting down\n");
    return 0;
}
