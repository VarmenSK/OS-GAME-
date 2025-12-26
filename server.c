#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>

#define MAX_PLAYERS 5
#define MIN_PLAYERS 3
#define SHM_NAME "/game_shm"
#define LOG_FILE "game.log"
#define SCORE_FILE "scores.txt"
#define PIPE_IN_PREFIX "/tmp/player_%d_in"
#define PIPE_OUT_PREFIX "/tmp/player_%d_out"
#define BOARD_SIZE 3
#define LOG_QUEUE_SIZE 256
#define LOG_MSG_LEN 256

typedef struct {
    char messages[LOG_QUEUE_SIZE][LOG_MSG_LEN];
    int head;
    int tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} log_queue_t;

typedef struct {
    char player_name[32];
    int score;
} player_score_t;

typedef struct {
    player_score_t entries[MAX_PLAYERS];
    pthread_mutex_t mutex;
} score_board_t;

typedef struct {
    int current_turn;
    int turn_in_progress;
    int moves_made;
    int player_count;
    int active_players[MAX_PLAYERS];
    int shutdown;
    int game_active;
    char board[BOARD_SIZE][BOARD_SIZE];
    pthread_mutex_t state_mutex;
    pthread_cond_t turn_cv;
    pthread_cond_t turn_done_cv;
    log_queue_t log_queue;
    score_board_t scores;
} shared_state_t;

static shared_state_t *state = NULL;
static pid_t children[MAX_PLAYERS];
static int child_count = 0;
static volatile sig_atomic_t live_children = 0;
static volatile sig_atomic_t shutdown_requested = 0;

static void enqueue_log(const char *msg) {
    pthread_mutex_lock(&state->log_queue.mutex);
    int next_head = (state->log_queue.head + 1) % LOG_QUEUE_SIZE;
    if (next_head == state->log_queue.tail) {
        // drop oldest to make room
        state->log_queue.tail = (state->log_queue.tail + 1) % LOG_QUEUE_SIZE;
    }
    snprintf(state->log_queue.messages[state->log_queue.head], LOG_MSG_LEN, "%s", msg);
    state->log_queue.head = next_head;
    pthread_cond_signal(&state->log_queue.cond);
    pthread_mutex_unlock(&state->log_queue.mutex);
}

static void save_scores();

static void reset_board_locked() {
    state->moves_made = 0;
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            state->board[r][c] = ' ';
        }
    }
    state->game_active = 1;
    state->current_turn = 0;
    state->turn_in_progress = 0;
}

static int active_player_count_locked() {
    int count = 0;
    for (int i = 0; i < state->player_count; i++) {
        if (state->active_players[i]) {
            count++;
        }
    }
    return count;
}

static void load_scores() {
    FILE *fp = fopen(SCORE_FILE, "r");
    if (!fp) {
        // initialize with defaults and create the file for persistence
        pthread_mutex_lock(&state->scores.mutex);
        for (int i = 0; i < state->player_count; i++) {
            snprintf(state->scores.entries[i].player_name, sizeof(state->scores.entries[i].player_name), "Player%d", i);
            state->scores.entries[i].score = 0;
        }
        pthread_mutex_unlock(&state->scores.mutex);
        save_scores();
        return;
    }
    pthread_mutex_lock(&state->scores.mutex);
    for (int i = 0; i < state->player_count; i++) {
        if (fscanf(fp, "%31s %d", state->scores.entries[i].player_name, &state->scores.entries[i].score) != 2) {
            snprintf(state->scores.entries[i].player_name, sizeof(state->scores.entries[i].player_name), "Player%d", i);
            state->scores.entries[i].score = 0;
        }
    }
    pthread_mutex_unlock(&state->scores.mutex);
    fclose(fp);
}

static void save_scores() {
    FILE *fp = fopen(SCORE_FILE, "w");
    if (!fp) {
        perror("save_scores");
        return;
    }
    pthread_mutex_lock(&state->scores.mutex);
    for (int i = 0; i < state->player_count; i++) {
        fprintf(fp, "%s %d\n", state->scores.entries[i].player_name, state->scores.entries[i].score);
    }
    pthread_mutex_unlock(&state->scores.mutex);
    fclose(fp);
}

static int check_winner(char symbol) {
    for (int i = 0; i < BOARD_SIZE; i++)
        if (state->board[i][0] == symbol && state->board[i][1] == symbol && state->board[i][2] == symbol)
            return 1;
    for (int i = 0; i < BOARD_SIZE; i++)
        if (state->board[0][i] == symbol && state->board[1][i] == symbol && state->board[2][i] == symbol)
            return 1;
    if (state->board[0][0] == symbol && state->board[1][1] == symbol && state->board[2][2] == symbol)
        return 1;
    if (state->board[0][2] == symbol && state->board[1][1] == symbol && state->board[2][0] == symbol)
        return 1;
    return 0;
}

static int next_active_player(int current) {
    for (int offset = 1; offset <= state->player_count; offset++) {
        int candidate = (current + offset) % state->player_count;
        if (state->active_players[candidate]) {
            return candidate;
        }
    }
    return current;
}

static void *logger_thread(void *arg) {
    FILE *fp = fopen(LOG_FILE, "a");
    if (!fp) {
        perror("logger");
        return NULL;
    }
    while (1) {
        pthread_mutex_lock(&state->log_queue.mutex);
        while (state->log_queue.head == state->log_queue.tail && !state->shutdown) {
            pthread_cond_wait(&state->log_queue.cond, &state->log_queue.mutex);
        }
        while (state->log_queue.head != state->log_queue.tail) {
            fprintf(fp, "%s\n", state->log_queue.messages[state->log_queue.tail]);
            fflush(fp);
            state->log_queue.tail = (state->log_queue.tail + 1) % LOG_QUEUE_SIZE;
        }
        int shutting_down = state->shutdown;
        pthread_mutex_unlock(&state->log_queue.mutex);
        if (shutting_down)
            break;
    }
    fclose(fp);
    return NULL;
}

static void *scheduler_thread(void *arg) {
    while (1) {
        int persist_scores = 0;
        pthread_mutex_lock(&state->state_mutex);
        while (state->shutdown) {
            pthread_mutex_unlock(&state->state_mutex);
            return NULL;
        }
        if (!state->game_active) {
            persist_scores = 1;
            reset_board_locked();
            enqueue_log("Scheduler: new game prepared");
        }
        int next = next_active_player(state->current_turn);
        state->current_turn = next;
        state->turn_in_progress = 1;
        char msg[128];
        snprintf(msg, sizeof(msg), "Scheduler: Player %d turn", next);
        enqueue_log(msg);
        pthread_cond_broadcast(&state->turn_cv);
        while (state->turn_in_progress && !state->shutdown) {
            pthread_cond_wait(&state->turn_done_cv, &state->state_mutex);
        }
        pthread_mutex_unlock(&state->state_mutex);
        if (persist_scores) {
            enqueue_log("Scheduler: persisting scores for completed round");
            save_scores();
        }
        if (state->shutdown)
            break;
    }
    return NULL;
}

static void cleanup_pipes() {
    char name[64];
    for (int i = 0; i < state->player_count; i++) {
        snprintf(name, sizeof(name), PIPE_IN_PREFIX, i);
        unlink(name);
        snprintf(name, sizeof(name), PIPE_OUT_PREFIX, i);
        unlink(name);
    }
}

static void handle_sigint(int sig) {
    (void)sig;
    if (!state) {
        _exit(0);
    }
    pthread_mutex_lock(&state->state_mutex);
    state->shutdown = 1;
    pthread_cond_broadcast(&state->turn_cv);
    pthread_cond_broadcast(&state->turn_done_cv);
    pthread_mutex_unlock(&state->state_mutex);
    pthread_mutex_lock(&state->log_queue.mutex);
    pthread_cond_broadcast(&state->log_queue.cond);
    pthread_mutex_unlock(&state->log_queue.mutex);
    save_scores();
}

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (live_children > 0) {
            live_children--;
        }
    }
    if (live_children <= 0) {
        shutdown_requested = 1;
    }
    errno = saved_errno;
}

static void setup_shared_state(int player_count) {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }
    if (ftruncate(shm_fd, sizeof(shared_state_t)) < 0) {
        perror("ftruncate");
        exit(EXIT_FAILURE);
    }
    state = mmap(NULL, sizeof(shared_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (state == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    memset(state, 0, sizeof(shared_state_t));
    state->player_count = player_count;
    for (int i = 0; i < player_count; i++) {
        state->active_players[i] = 1;
    }

    pthread_mutexattr_t m_attr;
    pthread_condattr_t c_attr;
    pthread_mutexattr_init(&m_attr);
    pthread_mutexattr_setpshared(&m_attr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_init(&c_attr);
    pthread_condattr_setpshared(&c_attr, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&state->state_mutex, &m_attr);
    pthread_cond_init(&state->turn_cv, &c_attr);
    pthread_cond_init(&state->turn_done_cv, &c_attr);

    pthread_mutex_init(&state->log_queue.mutex, &m_attr);
    pthread_cond_init(&state->log_queue.cond, &c_attr);
    state->log_queue.head = state->log_queue.tail = 0;

    pthread_mutex_init(&state->scores.mutex, &m_attr);

    reset_board_locked();
    load_scores();
}

static void send_board(int out_fd) {
    char buffer[512];
    int idx = 0;
    idx += snprintf(buffer + idx, sizeof(buffer) - idx, "Current board:\n");
    for (int r = 0; r < BOARD_SIZE; r++) {
        for (int c = 0; c < BOARD_SIZE; c++) {
            idx += snprintf(buffer + idx, sizeof(buffer) - idx, " %c ", state->board[r][c]);
            if (c < BOARD_SIZE - 1)
                idx += snprintf(buffer + idx, sizeof(buffer) - idx, "|");
        }
        idx += snprintf(buffer + idx, sizeof(buffer) - idx, "\n");
        if (r < BOARD_SIZE - 1)
            idx += snprintf(buffer + idx, sizeof(buffer) - idx, "-----------\n");
    }
    write(out_fd, buffer, strlen(buffer));
}

static void handle_client(int player_id) {
    char in_pipe[64], out_pipe[64];
    snprintf(in_pipe, sizeof(in_pipe), PIPE_IN_PREFIX, player_id);
    snprintf(out_pipe, sizeof(out_pipe), PIPE_OUT_PREFIX, player_id);

    int in_fd = open(in_pipe, O_RDONLY);
    if (in_fd < 0) {
        perror("open in pipe");
        exit(EXIT_FAILURE);
    }
    int out_fd = open(out_pipe, O_WRONLY);
    if (out_fd < 0) {
        perror("open out pipe");
        exit(EXIT_FAILURE);
    }

    char symbol = 'X' + player_id;
    char buffer[256];

    while (1) {
        pthread_mutex_lock(&state->state_mutex);
        while ((!state->turn_in_progress || state->current_turn != player_id) && !state->shutdown) {
            pthread_cond_wait(&state->turn_cv, &state->state_mutex);
        }
        if (state->shutdown) {
            pthread_mutex_unlock(&state->state_mutex);
            break;
        }
        send_board(out_fd);
        dprintf(out_fd, "Your turn Player %d (%c). Enter move as: row col\n", player_id, symbol);
        pthread_mutex_unlock(&state->state_mutex);

        ssize_t n = read(in_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            snprintf(buffer, sizeof(buffer), "Player %d disconnected", player_id);
            enqueue_log(buffer);
            pthread_mutex_lock(&state->state_mutex);
            state->active_players[player_id] = 0;
            state->turn_in_progress = 0;
            pthread_cond_signal(&state->turn_done_cv);
            if (active_player_count_locked() < MIN_PLAYERS) {
                state->shutdown = 1;
                enqueue_log("Shutting down: fewer than minimum active players");
                pthread_cond_broadcast(&state->turn_cv);
                pthread_cond_broadcast(&state->turn_done_cv);
            }
            pthread_mutex_unlock(&state->state_mutex);
            if (state->shutdown) {
                pthread_mutex_lock(&state->log_queue.mutex);
                pthread_cond_broadcast(&state->log_queue.cond);
                pthread_mutex_unlock(&state->log_queue.mutex);
            }
            break;
        }
        buffer[n] = '\0';
        int row = -1, col = -1;
        if (sscanf(buffer, "%d %d", &row, &col) != 2 || row < 0 || row >= BOARD_SIZE || col < 0 || col >= BOARD_SIZE) {
            dprintf(out_fd, "Invalid input. Format: row col within 0-%d.\n", BOARD_SIZE - 1);
            continue;
        }

        pthread_mutex_lock(&state->state_mutex);
        if (state->board[row][col] != ' ') {
            dprintf(out_fd, "Cell already occupied. Try again.\n");
            pthread_mutex_unlock(&state->state_mutex);
            continue;
        }
        state->board[row][col] = symbol;
        state->moves_made++;
        snprintf(buffer, sizeof(buffer), "Player %d placed %c at (%d,%d)", player_id, symbol, row, col);
        enqueue_log(buffer);

        int has_winner = check_winner(symbol);
        if (has_winner) {
            snprintf(buffer, sizeof(buffer), "Player %d wins this round!", player_id);
            enqueue_log(buffer);
            dprintf(out_fd, "%s\n", buffer);
            pthread_mutex_lock(&state->scores.mutex);
            state->scores.entries[player_id].score++;
            pthread_mutex_unlock(&state->scores.mutex);
            state->game_active = 0;
        } else if (state->moves_made == BOARD_SIZE * BOARD_SIZE) {
            enqueue_log("Game ended in a draw.");
            state->game_active = 0;
        }

        state->turn_in_progress = 0;
        pthread_cond_signal(&state->turn_done_cv);
        pthread_mutex_unlock(&state->state_mutex);
    }

    close(in_fd);
    close(out_fd);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    int player_count = MIN_PLAYERS;
    if (argc == 2) {
        player_count = atoi(argv[1]);
        if (player_count < MIN_PLAYERS || player_count > MAX_PLAYERS) {
            fprintf(stderr, "Player count must be between %d and %d\n", MIN_PLAYERS, MAX_PLAYERS);
            return EXIT_FAILURE;
        }
    }

    live_children = player_count;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction SIGCHLD");
        return EXIT_FAILURE;
    }

    setup_shared_state(player_count);
    cleanup_pipes();
    for (int i = 0; i < player_count; i++) {
        char pipe_name[64];
        snprintf(pipe_name, sizeof(pipe_name), PIPE_IN_PREFIX, i);
        if (mkfifo(pipe_name, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo in");
            exit(EXIT_FAILURE);
        }
        snprintf(pipe_name, sizeof(pipe_name), PIPE_OUT_PREFIX, i);
        if (mkfifo(pipe_name, 0666) < 0 && errno != EEXIST) {
            perror("mkfifo out");
            exit(EXIT_FAILURE);
        }
    }

    pthread_t logger_tid, scheduler_tid;
    if (pthread_create(&logger_tid, NULL, logger_thread, NULL) != 0) {
        perror("pthread_create logger");
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&scheduler_tid, NULL, scheduler_thread, NULL) != 0) {
        perror("pthread_create scheduler");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < player_count; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            handle_client(i);
        } else {
            children[i] = pid;
            child_count++;
        }
    }

    while (!state->shutdown) {
        if (shutdown_requested && !state->shutdown) {
            pthread_mutex_lock(&state->state_mutex);
            state->shutdown = 1;
            pthread_cond_broadcast(&state->turn_cv);
            pthread_cond_broadcast(&state->turn_done_cv);
            pthread_mutex_unlock(&state->state_mutex);
            pthread_mutex_lock(&state->log_queue.mutex);
            pthread_cond_broadcast(&state->log_queue.cond);
            pthread_mutex_unlock(&state->log_queue.mutex);
        }
        if (state->shutdown) {
            break;
        }
        pause();
    }

    pthread_join(scheduler_tid, NULL);
    pthread_join(logger_tid, NULL);

    for (int i = 0; i < child_count; i++) {
        kill(children[i], SIGTERM);
    }

    save_scores();
    cleanup_pipes();
    shm_unlink(SHM_NAME);
    return 0;
}
