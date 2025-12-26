# Concurrent Networked Tic-Tac-Toe Server

Hybrid-concurrency implementation of a multiplayer Tic-Tac-Toe server for 3–5 players. The server forks one child process per player and coordinates turns/logging using pthread threads plus POSIX shared memory. Inter-process communication uses named pipes under `/tmp`.

## Building
```
make
```
This produces the `server` and `client` binaries (requires `gcc` with pthread support).

## Running the server
```
./server <player_count>
```
* `player_count` must be between **3** and **5**.
* The server auto-creates per-player FIFOs: `/tmp/player_<id>_in` (client → server) and `/tmp/player_<id>_out` (server → client).
* Scores load from `scores.txt` at startup; the file is created automatically if missing.
* `game.log` captures all scheduler events, moves, disconnects, and round completions.
* The server supports multiple rounds without restarting. Scores are persisted after every completed round and on shutdown.

Shut down gracefully with `Ctrl+C`; the server will persist scores, notify the logger, and reap children.

## Running clients
Launch one client per player (in separate terminals) after starting the server:
```
./client <player_id>
```
* `player_id` is zero-based (e.g., `0`, `1`, `2`).
* The client prints board updates and prompts. Submit moves as `row col` (0-based). Enter `quit` to disconnect.

## Game rules
* 3x3 grid, symbols assigned as `'X' + player_id`.
* Players take turns in round-robin order managed by the scheduler thread.
* A player wins with three of their symbols in a row (horizontal, vertical, or diagonal).
* A draw occurs when all nine cells are filled without a winner.
* After a win or draw, the board resets automatically and a new round begins; scores persist across rounds and restarts.

## Architecture notes
* **IPC mode:** Named pipes (FIFOs) for client/server messaging.
* **Processes:** Parent forks one child per player for isolation.
* **Threads (parent-only):** Scheduler thread controls turn order; logger thread drains a shared log queue to `game.log` without blocking gameplay.
* **Shared memory:** All game state, synchronization primitives (process-shared mutexes/condition variables), and the log queue reside in a POSIX shared memory segment.
* **Persistence:** Scores are guarded by a process-shared mutex and written to `scores.txt` after each round and on shutdown.

## Team responsibilities
| Member | Responsibilities |
| --- | --- |
| AL-SALOUL, ASHRAF ALI HUSSEIN | Server core, fork lifecycle, shared memory setup |
| Rashad Ali Qaid Sofan | Scheduler thread, turn synchronization, process-shared primitives |
| Varmen A/L Siva Kumar | Logger thread, FIFO IPC plumbing |
| Abdullah Bin Ahmad | Score persistence, multi-round reset handling, shutdown/signals |
