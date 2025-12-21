// server.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

#define MAX_PLAYERS 3
#define BOARD 3
#define LOG_Q 64
#define LOG_LEN 128

typedef struct {
    char msg[LOG_LEN];
} log_entry;

typedef struct {
    pthread_mutex_t game_mutex;      // protects board, turn, active
    pthread_mutex_t log_mutex;       // protects log queue
    pthread_mutex_t score_mutex;     // protects scores

    char board[BOARD][BOARD];
    int turn;
    int active[MAX_PLAYERS];
    int game_over;

    log_entry logq[LOG_Q];
    int log_head, log_tail;

    int scores[MAX_PLAYERS];
} shared_state;

shared_state *state;

/* ---------- Utility ---------- */
void push_log(const char *m) {
    pthread_mutex_lock(&state->log_mutex);
    snprintf(state->logq[state->log_tail].msg, LOG_LEN, "%s", m);
    state->log_tail = (state->log_tail + 1) % LOG_Q;
    pthread_mutex_unlock(&state->log_mutex);
}

/* ---------- Logger Thread ---------- */
void *logger_thread(void *arg) {
    FILE *f = fopen("game.log", "a");
    while (1) {
        pthread_mutex_lock(&state->log_mutex);
        if (state->log_head != state->log_tail) {
            fprintf(f, "%s\n", state->logq[state->log_head].msg);
            fflush(f);
            state->log_head = (state->log_head + 1) % LOG_Q;
        }
        pthread_mutex_unlock(&state->log_mutex);
        usleep(10000);
    }
}

/* ---------- Scheduler Thread ---------- */
void *scheduler_thread(void *arg) {
    while (!state->game_over) {
        pthread_mutex_lock(&state->game_mutex);
        do {
            state->turn = (state->turn + 1) % MAX_PLAYERS;
        } while (!state->active[state->turn]);
        pthread_mutex_unlock(&state->game_mutex);
        sleep(1);
    }
}

/* ---------- Game Logic ---------- */
int check_win(char s) {
    for (int i = 0; i < 3; i++)
        if ((state->board[i][0]==s && state->board[i][1]==s && state->board[i][2]==s) ||
            (state->board[0][i]==s && state->board[1][i]==s && state->board[2][i]==s))
            return 1;
    if ((state->board[0][0]==s && state->board[1][1]==s && state->board[2][2]==s) ||
        (state->board[0][2]==s && state->board[1][1]==s && state->board[2][0]==s))
        return 1;
    return 0;
}

/* ---------- Client Handler (CHILD) ---------- */
void handle_client(int pid) {
    char fifo[32];
    sprintf(fifo, "p%d.fifo", pid);
    mkfifo(fifo, 0666);
    int fd = open(fifo, O_RDONLY);

    while (!state->game_over) {
        pthread_mutex_lock(&state->game_mutex);
        if (state->turn == pid) {
            int r,c;
            read(fd, &r, sizeof(int));
            read(fd, &c, sizeof(int));

            if (r>=0 && r<3 && c>=0 && c<3 && state->board[r][c]==' ') {
                char sym = 'X' + pid;
                state->board[r][c] = sym;

                char log[64];
                sprintf(log, "Player %d -> (%d,%d)", pid, r, c);
                push_log(log);

                if (check_win(sym)) {
                    state->game_over = 1;
                    pthread_mutex_lock(&state->score_mutex);
                    state->scores[pid]++;
                    pthread_mutex_unlock(&state->score_mutex);
                    push_log("GAME OVER");
                }
            }
        }
        pthread_mutex_unlock(&state->game_mutex);
        usleep(10000);
    }
    close(fd);
    unlink(fifo);
    exit(0);
}

/* ---------- Cleanup ---------- */
void save_scores() {
    FILE *f = fopen("scores.txt", "w");
    for (int i=0;i<MAX_PLAYERS;i++)
        fprintf(f, "Player%d %d\n", i, state->scores[i]);
    fclose(f);
}

void sigint_handler(int s) {
    save_scores();
    exit(0);
}

/* ---------- Main ---------- */
int main() {
    signal(SIGINT, sigint_handler);

    int shm = shm_open("/game_shm", O_CREAT|O_RDWR, 0666);
    ftruncate(shm, sizeof(shared_state));
    state = mmap(NULL, sizeof(shared_state), PROT_READ|PROT_WRITE, MAP_SHARED, shm, 0);

    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&state->game_mutex, &a);
    pthread_mutex_init(&state->log_mutex, &a);
    pthread_mutex_init(&state->score_mutex, &a);

    memset(state->board, ' ', sizeof(state->board));
    memset(state->scores, 0, sizeof(state->scores));
    for(int i=0;i<MAX_PLAYERS;i++) state->active[i]=1;
    state->turn = 0;
    state->game_over = 0;

    pthread_t log_t, sched_t;
    pthread_create(&log_t, NULL, logger_thread, NULL);
    pthread_create(&sched_t, NULL, scheduler_thread, NULL);

    for (int i=0;i<MAX_PLAYERS;i++)
        if (fork()==0) handle_client(i);

    while (wait(NULL)>0);
    save_scores();
    shm_unlink("/game_shm");
    return 0;
}
