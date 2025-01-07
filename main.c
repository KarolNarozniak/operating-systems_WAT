/****************************************************
 * File: project_sigaction_fixed.c
 ****************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/types.h>

/* -------------------- Config -------------------- */
#define SHM_BUFFER_SIZE 1024
#define HEX_BUFFER_SIZE (SHM_BUFFER_SIZE * 2 + 1)

/* Choose signals for S1, S2 */
/* S1: SIGUSR1 - Termination */
/* S2: SIGUSR2 - Toggle Pause/Resume */
#define S1 SIGUSR1   // Termination
#define S2 SIGUSR2   // Toggle Pause/Resume

/* -------------------- Shared Memory Struct -------------------- */
typedef struct {
    sem_t sem_can_write;   // Controls write permission
    sem_t sem_can_read;    // Controls read permission
    char  buffer[SHM_BUFFER_SIZE];
} shared_data_t;

/* -------------------- Message Queue Struct -------------------- */
struct msgbuf {
    long mtype;
    char mtext[HEX_BUFFER_SIZE];
};

/* -------------------- Global Variables -------------------- */
static pid_t child_pids[3] = {0}; // Store PIDs of process1, process2, process3
static pid_t parent_pid = 0;      // Will store parent’s PID

/* Flags set by signals */
volatile sig_atomic_t paused_flag = 0;
volatile sig_atomic_t terminate_flag = 0;

/* Mode Flag */
volatile sig_atomic_t using_dev_urandom = 0; // 1 if reading from /dev/urandom, else 0

/* -------------------------------------------------------------------------
 * handle_signal_in_parent
 *
 * Parent’s reaction to signals:
 *   - Sets flags
 *   - Broadcasts to all children once.
 * ------------------------------------------------------------------------- */
void handle_signal_in_parent(int signo) {
    switch (signo) {
        case S1: // Termination
            terminate_flag = 1;
            fprintf(stderr, "[Parent %d] Received termination.\n", getpid());
            // Forward termination signal to children
            for (int i = 0; i < 3; i++) {
                if (child_pids[i] > 0) {
                    kill(child_pids[i], S1);
                }
            }
            break;

        case S2: // Toggle Pause/Resume
            paused_flag = !paused_flag;
            if (paused_flag) {
                fprintf(stderr, "[Parent %d] Pausing data flow.\n", getpid());
                // Forward pause signal to children
                for (int i = 0; i < 3; i++) {
                    if (child_pids[i] > 0) {
                        kill(child_pids[i], S2);
                    }
                }
            } else {
                fprintf(stderr, "[Parent %d] Resuming data flow.\n", getpid());
                // Forward resume signal to children
                for (int i = 0; i < 3; i++) {
                    if (child_pids[i] > 0) {
                        kill(child_pids[i], S2);
                    }
                }
            }
            break;

        default:
            // Ignore other signals
            break;
    }
}

/* -------------------------------------------------------------------------
 * handle_signal_in_child
 *
 * Child’s reaction to signals from parent or operator.
 *   - Only sets local flags, logs it. No further broadcasting.
 * ------------------------------------------------------------------------- */
void handle_signal_in_child(int signo) {
    switch (signo) {
        case S1: // Termination
            terminate_flag = 1;
            fprintf(stderr, "[Child %d] Received termination.\n", getpid());
            break;
        case S2: // Toggle Pause/Resume
            paused_flag = !paused_flag;
            if (paused_flag) {
                fprintf(stderr, "[Child %d] Pausing data flow.\n", getpid());
            } else {
                fprintf(stderr, "[Child %d] Resuming data flow.\n", getpid());
            }
            break;
        default:
            // Ignore other signals
            break;
    }
}

/* -------------------------------------------------------------------------
 * my_sigaction_handler
 *
 * Uses siginfo to see who sent the signal. Only the parent broadcasts to
 * children. Children forward signals from outside (operator) up to parent.
 * ------------------------------------------------------------------------- */
void my_sigaction_handler(int signo, siginfo_t *info, void *context) {
    pid_t sender = info->si_pid; // Who sent this signal?

    if (getpid() == parent_pid) {
        // We are the parent
        if (sender == parent_pid) {
            // Rare or not typical, ignore or handle
        } else if (sender == 0) {
            // Some kernel signal, treat as external
            handle_signal_in_parent(signo);
        } else if (sender == child_pids[0] ||
                   sender == child_pids[1] ||
                   sender == child_pids[2]) {
            // Child => means operator signaled child
            // => parent handles it, then broadcast
            handle_signal_in_parent(signo);
        } else {
            // Operator signaled parent
            handle_signal_in_parent(signo);
        }
    } else {
        // We are a child
        if (sender == parent_pid) {
            // The parent broadcast => handle locally
            handle_signal_in_child(signo);
        } else {
            // Operator signaled this child directly
            // => forward to parent for broadcast
            kill(parent_pid, signo);
            // Also handle locally if we want immediate action
            handle_signal_in_child(signo);
        }
    }
}

/* -------------------------------------------------------------------------
 * install_sigaction_handlers
 *
 * Uses SA_SIGINFO (for siginfo_t) without SA_RESTART to allow interrupted
 * system calls to be handled properly.
 * ------------------------------------------------------------------------- */
void install_sigaction_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // Use SA_SIGINFO without SA_RESTART
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = my_sigaction_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(S1, &sa, NULL);
    sigaction(S2, &sa, NULL);
}

/* -------------------------------------------------------------------------
 * process1 (Producer)
 * Reads from stdin, writes raw data to shared memory (K1).
 * Uses semaphores to coordinate with process2.
 *
 * Implements state change detection to print pause/resume messages only once.
 * ------------------------------------------------------------------------- */
void process1(int shmid) {
    shared_data_t *shm_ptr = (shared_data_t *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("shmat in process1");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[Process1] Attached to shared memory.\n");

    char buffer[SHM_BUFFER_SIZE];
    int last_paused_flag = 0; // To track state changes

    while (!terminate_flag) {
        // Detect state changes
        if (paused_flag != last_paused_flag) {
            if (paused_flag) {
                fprintf(stderr, "[Process1] Paused.\n");
            } else {
                fprintf(stderr, "[Process1] Resumed.\n");
            }
            last_paused_flag = paused_flag;
        }

        if (paused_flag) {
            usleep(100000); // Sleep for 100ms while paused
            continue;
        }

        /* Read from stdin */
        ssize_t bytesRead = read(STDIN_FILENO, buffer, SHM_BUFFER_SIZE);

        if (bytesRead < 0) {
            if (errno == EINTR && !terminate_flag) {
                // read was interrupted by a signal, just retry
                continue;
            }
            perror("[Process1] read from stdin");
            break;
        }
        if (bytesRead == 0) {
            /* EOF reached */
            if (using_dev_urandom) {
                // For /dev/urandom, EOF is unexpected; retry
                fprintf(stderr, "[Process1] Unexpected EOF from /dev/urandom, retrying.\n");
                usleep(100000); // Sleep before retrying
                continue;
            } else {
                fprintf(stderr, "[Process1] End of input.\n");
                break;
            }
        }

        /* Wait until slot is free (also handle EINTR) */
        while (sem_wait(&shm_ptr->sem_can_write) == -1) {
            if (errno == EINTR) {
                if (terminate_flag) {
                    // If termination was requested during wait
                    fprintf(stderr, "[Process1] sem_wait interrupted by termination.\n");
                    break;
                }
                // Otherwise, retry sem_wait
                continue;
            } else {
                perror("[Process1] sem_wait(sem_can_write)");
                break;
            }
        }
        if (terminate_flag) {
            break;
        }

        /* Copy data into shared memory */
        memcpy(shm_ptr->buffer, buffer, bytesRead);
        if (bytesRead < SHM_BUFFER_SIZE) {
            memset(shm_ptr->buffer + bytesRead, 0, SHM_BUFFER_SIZE - bytesRead);
        }

        /* Signal process2 that data is ready */
        if (sem_post(&shm_ptr->sem_can_read) == -1) {
            perror("[Process1] sem_post(sem_can_read)");
            break;
        }

        usleep(50000); // Sleep for 50ms to prevent tight looping
    }

    /* Detach */
    if (shmdt(shm_ptr) == -1) {
        perror("[Process1] shmdt");
    }
    fprintf(stderr, "[Process1] Exiting.\n");
    exit(EXIT_SUCCESS);
}

/* -------------------------------------------------------------------------
 * process2 (Consumer→Producer)
 * Waits for data from shared memory (K1), converts to hex,
 * sends via message queue (K2). Coordinates with process1 using semaphores.
 *
 * Implements state change detection to print pause/resume messages only once.
 * ------------------------------------------------------------------------- */
void process2(int shmid, int msqid) {
    shared_data_t *shm_ptr = (shared_data_t *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("shmat in process2");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "[Process2] Attached to shared memory.\n");

    struct msgbuf message;
    message.mtype = 1;
    int last_paused_flag = 0; // To track state changes

    while (!terminate_flag) {
        // Detect state changes
        if (paused_flag != last_paused_flag) {
            if (paused_flag) {
                fprintf(stderr, "[Process2] Paused.\n");
            } else {
                fprintf(stderr, "[Process2] Resumed.\n");
            }
            last_paused_flag = paused_flag;
        }

        if (paused_flag) {
            usleep(100000); // Sleep for 100ms while paused
            continue;
        }

        /* Wait for data to be ready (handle EINTR) */
        while (sem_wait(&shm_ptr->sem_can_read) == -1) {
            if (errno == EINTR) {
                if (terminate_flag) {
                    fprintf(stderr, "[Process2] sem_wait interrupted by termination.\n");
                    break;
                }
                // Otherwise, retry sem_wait
                continue;
            } else {
                perror("[Process2] sem_wait(sem_can_read)");
                break;
            }
        }
        if (terminate_flag) {
            break;
        }

        /* Convert to hex */
        char hex_buffer[HEX_BUFFER_SIZE] = {0};
        size_t hex_index = 0;
        for (int i = 0; i < SHM_BUFFER_SIZE && hex_index < HEX_BUFFER_SIZE - 1; i++) {
            if (shm_ptr->buffer[i] == '\0') {
                break;
            }
            snprintf(&hex_buffer[hex_index], 3, "%02x", (unsigned char)shm_ptr->buffer[i]);
            hex_index += 2;
        }

        /* Send to process3 */
        strncpy(message.mtext, hex_buffer, sizeof(message.mtext) - 1);
        message.mtext[sizeof(message.mtext) - 1] = '\0';
        if (msgsnd(msqid, &message, sizeof(message.mtext), 0) == -1) {
            perror("[Process2] msgsnd");
            break;
        }

        /* Mark shared memory free for process1 */
        if (sem_post(&shm_ptr->sem_can_write) == -1) {
            perror("[Process2] sem_post(sem_can_write)");
            break;
        }

        usleep(50000); // Sleep for 50ms to prevent tight looping
    }

    /* Detach */
    if (shmdt(shm_ptr) == -1) {
        perror("[Process2] shmdt");
    }
    fprintf(stderr, "[Process2] Exiting.\n");
    exit(EXIT_SUCCESS);
}

/* -------------------------------------------------------------------------
 * process3 (Consumer)
 * Receives hex data from message queue (K2) and prints to stderr
 * in 15-chunk lines.
 *
 * Implements state change detection to print pause/resume messages only once.
 * ------------------------------------------------------------------------- */
void process3(int msqid) {
    struct msgbuf message;
    int last_paused_flag = 0; // To track state changes

    while (!terminate_flag) {
        // Detect state changes
        if (paused_flag != last_paused_flag) {
            if (paused_flag) {
                fprintf(stderr, "[Process3] Paused.\n");
            } else {
                fprintf(stderr, "[Process3] Resumed.\n");
            }
            last_paused_flag = paused_flag;
        }

        if (paused_flag) {
            usleep(100000); // Sleep for 100ms while paused
            continue;
        }

        /* Blocking receive with EINTR handling */
        ssize_t ret = msgrcv(msqid, &message, sizeof(message.mtext), 0, 0);
        if (ret == -1) {
            if (errno == EINTR) {
                if (terminate_flag) {
                    fprintf(stderr, "[Process3] msgrcv interrupted by termination.\n");
                    break;
                }
                // Otherwise, retry msgrcv
                continue;
            }
            perror("[Process3] msgrcv");
            break;
        }

        if (message.mtext[0] == '\0') {
            continue; // empty message
        }

        /* Print 15 hex pairs per line */
        int len = strlen(message.mtext);
        int chunk_count = 0;

        for (int i = 0; i < len; i += 2) {
            char chunk[3] = {0};
            chunk[0] = message.mtext[i];
            if (i + 1 < len) {
                chunk[1] = message.mtext[i + 1];
            }

            fprintf(stderr, "%s ", chunk);
            chunk_count++;

            if (chunk_count == 15) {
                fprintf(stderr, "\n");
                chunk_count = 0;
            }
        }
        if (chunk_count != 0) {
            fprintf(stderr, "\n");
        }
    }

    fprintf(stderr, "[Process3] Exiting.\n");
    exit(EXIT_SUCCESS);
}

/* -------------------------------------------------------------------------
 * run_pipeline
 *
 * Wraps entire pipeline: open input (if any), set up shared memory + sems,
 * queue, fork children, wait, cleanup.
 * ------------------------------------------------------------------------- */
int run_pipeline(const char *filePathOption) {
    /* 1. Reopen stdin if needed */
    if (filePathOption != NULL) {
        if (freopen(filePathOption, "r", stdin) == NULL) {
            perror("[run_pipeline] Failed to open input");
            return -1;
        }
        fprintf(stderr, "Reading from: %s\n", filePathOption);

        if (strcmp(filePathOption, "/dev/urandom") == 0) {
            using_dev_urandom = 1;
        } else {
            using_dev_urandom = 0;
        }
    } else {
        using_dev_urandom = 0;
    }

    /* 2. Shared memory */
    key_t shm_key = ftok(".", 65);
    if (shm_key == -1) {
        perror("[run_pipeline] ftok for shared memory");
        return -1;
    }
    int shmid = shmget(shm_key, sizeof(shared_data_t), 0666 | IPC_CREAT);
    if (shmid < 0) {
        perror("[run_pipeline] shmget");
        return -1;
    }

    shared_data_t *shm_ptr = (shared_data_t *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("[run_pipeline] shmat for init");
        return -1;
    }

    if (sem_init(&shm_ptr->sem_can_write, 1, 1) == -1) {
        perror("[run_pipeline] sem_init(sem_can_write)");
        shmdt(shm_ptr);
        return -1;
    }
    if (sem_init(&shm_ptr->sem_can_read, 1, 0) == -1) {
        perror("[run_pipeline] sem_init(sem_can_read)");
        sem_destroy(&shm_ptr->sem_can_write);
        shmdt(shm_ptr);
        return -1;
    }

    if (shmdt(shm_ptr) == -1) {
        perror("[run_pipeline] shmdt after init");
    }

    /* 3. Message queue */
    key_t msg_key = ftok(".", 66);
    if (msg_key == -1) {
        perror("[run_pipeline] ftok for message queue");
        // Cleanup shared memory
        shmctl(shmid, IPC_RMID, NULL);
        return -1;
    }
    int msqid = msgget(msg_key, 0666 | IPC_CREAT);
    if (msqid < 0) {
        perror("[run_pipeline] msgget");
        // Cleanup shared memory
        shmctl(shmid, IPC_RMID, NULL);
        return -1;
    }

    /* 4. (Re)Install sigaction-based handlers (without SA_RESTART) */
    install_sigaction_handlers();

    /* 5. Fork children */
    pid_t pid1, pid2, pid3;
    if ((pid1 = fork()) == 0) {
        process1(shmid);
    } else if (pid1 < 0) {
        perror("[run_pipeline] fork for process1");
        // Cleanup resources
        shmctl(shmid, IPC_RMID, NULL);
        msgctl(msqid, IPC_RMID, NULL);
        return -1;
    }
    child_pids[0] = pid1;

    if ((pid2 = fork()) == 0) {
        process2(shmid, msqid);
    } else if (pid2 < 0) {
        perror("[run_pipeline] fork for process2");
        // Cleanup resources
        kill(pid1, S1); // Terminate process1
        waitpid(pid1, NULL, 0);
        shmctl(shmid, IPC_RMID, NULL);
        msgctl(msqid, IPC_RMID, NULL);
        return -1;
    }
    child_pids[1] = pid2;

    if ((pid3 = fork()) == 0) {
        process3(msqid);
    } else if (pid3 < 0) {
        perror("[run_pipeline] fork for process3");
        // Cleanup resources
        kill(pid1, S1); // Terminate process1
        kill(pid2, S1); // Terminate process2
        waitpid(pid1, NULL, 0);
        waitpid(pid2, NULL, 0);
        shmctl(shmid, IPC_RMID, NULL);
        msgctl(msqid, IPC_RMID, NULL);
        return -1;
    }
    child_pids[2] = pid3;

    /* 6. Wait for children to finish */
    for (int i = 0; i < 3; i++) {
        while (waitpid(child_pids[i], NULL, 0) == -1) {
            if (errno == EINTR) {
                if (terminate_flag) {
                    // If termination was requested during wait
                    break;
                }
                // Otherwise, retry waitpid
                continue;
            } else {
                perror("[run_pipeline] waitpid");
                break;
            }
        }
    }

    /* 7. Cleanup resources */
    shm_ptr = (shared_data_t *)shmat(shmid, NULL, 0);
    if (shm_ptr != (void *)-1) {
        sem_destroy(&shm_ptr->sem_can_write);
        sem_destroy(&shm_ptr->sem_can_read);
        shmdt(shm_ptr);
    } else {
        perror("[run_pipeline] shmat before cleanup");
    }
    if (shmctl(shmid, IPC_RMID, NULL) < 0) {
        perror("[run_pipeline] shmctl(IPC_RMID)");
    }
    if (msgctl(msqid, IPC_RMID, NULL) < 0) {
        perror("[run_pipeline] msgctl(IPC_RMID)");
    }

    fprintf(stderr, "Pipeline finished. Resources cleaned.\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 *
 * Displays a menu in a loop:
 * 1) Keyboard input
 * 2) File input
 * 3) /dev/urandom
 * 4) Quit
 *
 * After each run_pipeline, we return to the menu.
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    parent_pid = getpid();
    install_sigaction_handlers(); // In case we get signals even before pipeline

    while (1) {
        printf("\n===== Producer-Consumer Menu =====\n");
        printf("1) User input (keyboard)\n");
        printf("2) Input from file (full path)\n");
        printf("3) /dev/urandom (read 1KB)\n");
        printf("4) Quit\n");
        printf("Choice: ");

        int choice = 0;
        if (scanf("%d", &choice) != 1) {
            fprintf(stderr, "[Menu] Invalid input.\n");
            /* flush bad input */
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { /* do nothing */ }
            continue;
        }
        /* flush trailing newline */
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { /* do nothing */ }

        if (terminate_flag) {
            // If a termination signal arrived while we were reading choice
            fprintf(stderr, "[Menu] Termination requested. Exiting.\n");
            break;
        }

        if (choice == 4) {
            printf("[Menu] Exiting.\n");
            break;
        }

        switch (choice) {
            case 1: {
                /* Keyboard input */
                fprintf(stderr, "[Menu] Using keyboard input.\n");
                run_pipeline(NULL);
                break;
            }
            case 2: {
                /* Prompt user for file path */
                char filePath[512] = {0};
                printf("Enter full file path (e.g. /home/kali/Desktop/test.txt):\n");
                if (fgets(filePath, sizeof(filePath), stdin) == NULL) {
                    fprintf(stderr, "[Menu] Error reading file path.\n");
                    break;
                }
                /* Remove trailing newline */
                filePath[strcspn(filePath, "\n")] = '\0';

                if (strlen(filePath) == 0) {
                    fprintf(stderr, "[Menu] No file path entered.\n");
                    break;
                }
                run_pipeline(filePath);
                break;
            }
            case 3: {
                /* /dev/urandom */
                fprintf(stderr, "[Menu] Reading 1KB from /dev/urandom.\n");
                run_pipeline("/dev/urandom");
                break;
            }
            default:
                fprintf(stderr, "[Menu] Invalid choice. Try again.\n");
                break;
        }

        // If a signal might have set terminate_flag while pipeline was running
        if (terminate_flag) {
            fprintf(stderr, "[Menu] Termination requested. Exiting.\n");
            break;
        }
    }

    printf("Main menu loop ended. Bye.\n");
    return 0;
}
