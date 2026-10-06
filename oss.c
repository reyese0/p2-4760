#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#define SIZE 20
#define SHM_KEY 1234
#define CLOCK_INCREMENT 100000

typedef struct {
    int seconds;
    int nanoseconds;
} MyClock;

typedef struct {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
    int endingTimeSeconds;
    int endingTimeNano;
} PCB;

static PCB processTable[SIZE];

static void print_help(void) {
    printf("How to use: oss [-h] [-n proc] [-s simul] [-t timelimit] [-i interval]\n");
    printf("  -h Show help message\n");
    printf("  -n proc Total number of children to launch\n");
    printf("  -s simul Maximum number of children to run simultaneously\n");
    printf("  -t Simulated time for each worker in seconds\n");
    printf("  -i minimum interval between launches in seconds\n");
}

static int compare_time(int leftSec, int leftNano, int rightSec, int rightNano) {
    if (leftSec < rightSec) {
        return -1;
    }
    if (leftSec > rightSec) {
        return 1;
    }
    if (leftNano < rightNano) {
        return -1;
    }
    if (leftNano > rightNano) {
        return 1;
    }
    return 0;
}

static void add_time(int *seconds, int *nanoseconds, int addSec, int addNano) {
    *seconds += addSec;
    *nanoseconds += addNano;
    if (*nanoseconds >= 1000000000) {
        *seconds += *nanoseconds / 1000000000;
        *nanoseconds %= 1000000000;
    }
}

static int find_open_pcb_slot() {
    for (int i = 0; i < SIZE; ++i) {
        if (!processTable[i].occupied) {
            return i;
        }
    }
    return -1;
}

static void record_runtime_total(const PCB *entry, int *totalSec, int *totalNano) {
    int runSec = entry->endingTimeSeconds - entry->startSeconds;
    int runNano = entry->endingTimeNano - entry->startNano;

    if (runNano < 0) {
        runSec -= 1;
        runNano += 1000000000;
    }

    add_time(totalSec, totalNano, runSec, runNano);
}

static int launch_worker(const MyClock *clock, int workerRunSec, int workerRunNano, int *launchedCount) {
    int slot = find_open_pcb_slot();
    char secArg[32];
    char nanoArg[32];

    if (slot == -1) {
        return 0;
    }

    snprintf(secArg, sizeof(secArg), "%d", workerRunSec);
    snprintf(nanoArg, sizeof(nanoArg), "%d", workerRunNano);

    pid_t pid = fork();

     if (pid < 0) {
        fprintf(stderr, "Fork failed\n");
        return 0;
    }
    if (pid == 0) {
        execl("./worker", "worker", secArg, nanoArg, (char *)NULL);
        exit(1);
    }

    processTable[slot].occupied = 1;
    processTable[slot].pid = pid;
    processTable[slot].startSeconds = clock->seconds;
    processTable[slot].startNano = clock->nanoseconds;
    processTable[slot].endingTimeSeconds = clock->seconds;
    processTable[slot].endingTimeNano = clock->nanoseconds;
    add_time(&processTable[slot].endingTimeSeconds, &processTable[slot].endingTimeNano, workerRunSec, workerRunNano);

    *launchedCount += 1;
    return 1;
}

void signal_handler(int sig) {
    printf("\nReceived SIGALRM (60 second timeout). Cleaning up...\n");
    
    // Send kill signal to all children based on their PIDs in process table
    for (int i = 0; i < 20; i++) {
        if (processTable[i].occupied && processTable[i].pid > 0) {
            printf("Killing child process %d\n", processTable[i].pid);
            kill(processTable[i].pid, SIGTERM);
        }
    }
    printf("OSS terminated due to 60 second timeout\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    char opt;
    const char optstring[] = "hn:s:t:i:";
    int maxProcesses  = 5;
    int maxSimul = 0;
    double timeLimit = 0.0;
    double interval = 0.0;
    int launchedChildren = 0;
    int childrenInSystem = 0;
    int finishedChildren = 0;
    int combinedRunSec = 0;
    int combinedRunNano = 0;
    int workerRunSec;
    int workerRunNano;
    int nextLaunchSec = 0;
    int nextLaunchNano = 0;
    int nextTablePrintSec = 0;
    int nextTablePrintNano = 500000000;

    signal(SIGALRM, signal_handler);

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                return 0;
            case 'n':
                maxProcesses  = atoi(optarg);
                break;
            case 's':
                maxSimul = atoi(optarg);
                break;
            case 't':
                timeLimit = atof(optarg);
                break;
            case 'i':
                interval = atof(optarg);
                break;
            default:
                fprintf(stderr, "Invalid option\n");
                print_help();
                return 1;
        }
    }

    workerRunSec = (int)timeLimit;
    workerRunNano = (int)((timeLimit - workerRunSec) * 1000000000);
    if (workerRunNano < 0) {
        workerRunNano = 0;
    }

    for (int i = 0; i < SIZE; ++i) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].startSeconds = 0;
        processTable[i].startNano = 0;
        processTable[i].endingTimeSeconds = 0;
        processTable[i].endingTimeNano = 0;
    }

    int shmid = shmget(SHM_KEY, sizeof(MyClock), IPC_CREAT | 0666);
    if (shmid < 0) {
        fprintf(stderr,"Parent:... Error in shmget\n");
        exit(1);
    }

    MyClock *myClock = (MyClock *)shmat(shmid, NULL, 0);
    if (myClock <= 0) {
        fprintf(stderr,"OSS:... Error in shmat\n");
        exit(1);
    }

    myClock->seconds = 0;
    myClock->nanoseconds = 0;

    printf("OSS starting, PID: %d PPID: %d\n", getpid(), getppid());
    printf("Called with:\n");
    printf("-n %d\n", maxProcesses );
    printf("-s %d\n", maxSimul);
    printf("-t %2f\n", timeLimit);
    printf("-i %3f\n", interval);

    while (launchedChildren < maxProcesses  || childrenInSystem > 0) {
        int status;

        //Advance the simulated clock every time through the loop
        add_time(&myClock->seconds, &myClock->nanoseconds, 0, CLOCK_INCREMENT);

        //Show the whole process table every half second of simulated time
        if (compare_time(myClock->seconds, myClock->nanoseconds, nextTablePrintSec, nextTablePrintNano) >= 0) {
            printf("OSS PID:%d SysClockS: %d SysclockNano: %d\n", getpid(), myClock->seconds, myClock->nanoseconds);
            printf("Process Table:\n");
            printf("Entry Occupied PID StartS StartN EndingTimeS EndingTimeNano\n");

            for (int i = 0; i < SIZE; ++i) {
                printf("%d %d %d %d %d %d %d\n", i,
                    processTable[i].occupied,
                    (int)processTable[i].pid,
                    processTable[i].startSeconds,
                    processTable[i].startNano,
                    processTable[i].endingTimeSeconds,
                    processTable[i].endingTimeNano);
            }
            add_time(&nextTablePrintSec, &nextTablePrintNano, 0, 500000000);
        }

        //Check for terminated children
        pid_t terminatedPid = waitpid(-1, &status, WNOHANG);
        if (terminatedPid > 0) {
            //find free entry in table entry
            for (int i = 0; i < SIZE; i++) {
                if (processTable[i].occupied && processTable[i].pid == terminatedPid) {
                    record_runtime_total(&processTable[i], &combinedRunSec, &combinedRunNano);
                    processTable[i].occupied = 0;
                    processTable[i].pid = 0;
                    processTable[i].startSeconds = 0;
                    processTable[i].startNano = 0;
                    processTable[i].endingTimeSeconds = 0;
                    processTable[i].endingTimeNano = 0;
                }
            }

            if (childrenInSystem > 0) {
                childrenInSystem -= 1;
            }

            finishedChildren += 1;
        }

        //Launch a new child only if we still need to launch more and stay under simul processes
        if (launchedChildren < maxProcesses  && childrenInSystem < maxSimul && compare_time(myClock->seconds, myClock->nanoseconds, nextLaunchSec, nextLaunchNano) >= 0) {
            if (launch_worker(myClock, workerRunSec, workerRunNano, &launchedChildren)) {
                int launchIntervalSec = (int)interval;
                int launchIntervalNano = (int)((interval - launchIntervalSec) * 1000000000);
                if (launchIntervalNano < 0) {
                    launchIntervalNano = 0;
                }
                childrenInSystem += 1;
                nextLaunchSec = myClock->seconds;
                nextLaunchNano = myClock->nanoseconds;
                add_time(&nextLaunchSec, &nextLaunchNano, launchIntervalSec, launchIntervalNano);
            }
        }
    }

    printf("OSS PID:%d Terminating\n", getpid());
    printf("%d workers were launched and terminated\n", finishedChildren);
    printf("Workers ran for a combined time of %d seconds %d nanoseconds.\n", combinedRunSec, combinedRunNano);

    shmdt(myClock);
    myClock = 0;
    shmctl(shmid, IPC_RMID, NULL);
    alarm(60);

    return 0;
}