#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>

#define PIPE_IN_PREFIX "/tmp/player_%d_in"
#define PIPE_OUT_PREFIX "/tmp/player_%d_out"

static int in_fd = -1;
static int out_fd = -1;
static volatile int running = 1;

static void *reader_thread(void *arg) {
    char buffer[512];
    while (running) {
        ssize_t n = read(out_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            printf("%s", buffer);
            fflush(stdout);
        } else {
            usleep(100000);
        }
    }
    return NULL;
}

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <player_id>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int player_id = atoi(argv[1]);
    char in_pipe[64], out_pipe[64];
    snprintf(in_pipe, sizeof(in_pipe), PIPE_IN_PREFIX, player_id);
    snprintf(out_pipe, sizeof(out_pipe), PIPE_OUT_PREFIX, player_id);

    out_fd = open(out_pipe, O_RDONLY);
    if (out_fd < 0) {
        perror("open out pipe");
        return EXIT_FAILURE;
    }
    in_fd = open(in_pipe, O_WRONLY);
    if (in_fd < 0) {
        perror("open in pipe");
        close(out_fd);
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_sigint);

    pthread_t reader;
    if (pthread_create(&reader, NULL, reader_thread, NULL) != 0) {
        perror("pthread_create");
        close(in_fd);
        close(out_fd);
        return EXIT_FAILURE;
    }

    printf("Connected as Player %d. Enter moves as 'row col' or 'quit' to exit.\n", player_id);
    char line[128];
    while (running && fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, "quit", 4) == 0) {
            running = 0;
            break;
        }
        write(in_fd, line, strlen(line));
    }

    running = 0;
    pthread_join(reader, NULL);
    close(in_fd);
    close(out_fd);
    return EXIT_SUCCESS;
}
