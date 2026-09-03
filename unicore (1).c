/* ============================================================================
 * UniCore — OS Resource Orchestration System for a Smart University
 * ----------------------------------------------------------------------------
 * A single-file C simulation demonstrating core Operating System concepts
 * applied to a "Smart University" resource-management scenario.
 *
 * Concepts implemented:
 *   1. Multi-resource allocation table (Classrooms, Lab PCs, Printers,
 *      Bandwidth units, Parking slots) shared among Department "processes".
 *   2. Deadlock AVOIDANCE using the Banker's Algorithm (Safety Algorithm +
 *      Resource-Request Algorithm).
 *   3. CPU-style scheduling of pending resource requests using a
 *      Priority + Round-Robin hybrid queue (priority = department urgency).
 *   4. Deadlock DETECTION via a Resource Allocation Graph / wait-for cycle
 *      check, used as a fallback safety net and for reporting.
 *   5. An orchestration event log (like a kernel log / audit trail).
 *
 * Build:
 *   gcc -std=c11 -Wall -Wextra -O2 -o unicore unicore.c
 *
 * Run:
 *   ./unicore
 *
 * Author: (your name here)
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

/* ---------------------------------------------------------------------- */
/* Configuration                                                          */
/* ---------------------------------------------------------------------- */

#define MAX_DEPTS      8      /* number of competing "processes"          */
#define NUM_RESOURCES  5      /* number of distinct resource types        */
#define MAX_LOG        256
#define NAME_LEN       32

/* Resource type indices */
enum {
    R_CLASSROOM = 0,
    R_LAB_PC,
    R_PRINTER,
    R_BANDWIDTH,
    R_PARKING
};

static const char *RES_NAME[NUM_RESOURCES] = {
    "Classrooms", "Lab-PCs", "Printers", "Bandwidth(Gbps)", "Parking-Slots"
};

/* ---------------------------------------------------------------------- */
/* Data structures                                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
    char name[NAME_LEN];
    int  priority;                  /* 1 (highest) .. 10 (lowest)         */
    int  max[NUM_RESOURCES];        /* Banker's "Max" claim                */
    int  allocation[NUM_RESOURCES]; /* currently held                     */
    int  need[NUM_RESOURCES];       /* max - allocation                   */
    bool finished;                  /* used during safety-sequence check  */
    bool blocked;                   /* waiting on a request               */
} Department;

typedef struct {
    Department dept[MAX_DEPTS];
    int        num_dept;

    int available[NUM_RESOURCES];   /* free/unallocated units             */
    int total[NUM_RESOURCES];       /* total system capacity              */

    char log[MAX_LOG][144];
    int  log_count;
} System;

static System sys_state;

/* ---------------------------------------------------------------------- */
/* Utility / logging                                                      */
/* ---------------------------------------------------------------------- */

static void log_event(const char *fmt, ...) {
    if (sys_state.log_count >= MAX_LOG) return;
    char buf[112];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char stamp[16];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", lt);

    snprintf(sys_state.log[sys_state.log_count], sizeof(sys_state.log[0]),
             "[%s] %s", stamp, buf);
    sys_state.log_count++;
}

static void print_log(void) {
    printf("\n===================== ORCHESTRATION EVENT LOG =====================\n");
    for (int i = 0; i < sys_state.log_count; i++)
        printf("%s\n", sys_state.log[i]);
    printf("=====================================================================\n");
}

static void recompute_need(Department *d) {
    for (int i = 0; i < NUM_RESOURCES; i++)
        d->need[i] = d->max[i] - d->allocation[i];
}

/* ---------------------------------------------------------------------- */
/* System initialisation                                                  */
/* ---------------------------------------------------------------------- */

static void init_system(void) {
    memset(&sys_state, 0, sizeof(sys_state));

    /* Total campus resource capacity */
    sys_state.total[R_CLASSROOM]  = 10;
    sys_state.total[R_LAB_PC]     = 40;
    sys_state.total[R_PRINTER]    = 6;
    sys_state.total[R_BANDWIDTH]  = 100;
    sys_state.total[R_PARKING]    = 50;

    for (int i = 0; i < NUM_RESOURCES; i++)
        sys_state.available[i] = sys_state.total[i];

    /* Seed departments (processes) with realistic max claims */
    struct { const char *n; int pr; int mx[NUM_RESOURCES]; } seed[] = {
        {"CSE-Dept",      2, {3, 20, 2, 40, 10}},
        {"ECE-Dept",      3, {2, 10, 1, 20, 8}},
        {"Mechanical",    5, {2,  5, 1, 10, 12}},
        {"Admin-Office",  1, {1,  2, 2,  5,  4}},
        {"Library",       4, {2,  8, 1, 15,  6}},
        {"Exam-Cell",     1, {2,  4, 1, 10,  3}},
    };
    int n = (int)(sizeof(seed) / sizeof(seed[0]));
    sys_state.num_dept = n;

    for (int i = 0; i < n; i++) {
        Department *d = &sys_state.dept[i];
        strncpy(d->name, seed[i].n, NAME_LEN - 1);
        d->priority = seed[i].pr;
        memcpy(d->max, seed[i].mx, sizeof(d->max));
        memset(d->allocation, 0, sizeof(d->allocation));
        recompute_need(d);
        d->finished = false;
        d->blocked  = false;
    }

    log_event("System initialised: %d departments, %d resource types.",
               sys_state.num_dept, NUM_RESOURCES);
}

/* ---------------------------------------------------------------------- */
/* Banker's Algorithm — Safety Algorithm                                  */
/* ---------------------------------------------------------------------- */

/* Returns true if a safe sequence exists; fills seq[] with the order and
 * sets *seq_len. Uses the CURRENT available[]/allocation[]/need[] state. */
static bool is_safe_state(int seq[], int *seq_len) {
    int work[NUM_RESOURCES];
    bool finish[MAX_DEPTS] = {false};
    memcpy(work, sys_state.available, sizeof(work));

    int count = 0;
    *seq_len = 0;

    while (count < sys_state.num_dept) {
        bool progressed = false;

        for (int i = 0; i < sys_state.num_dept; i++) {
            if (finish[i]) continue;
            Department *d = &sys_state.dept[i];

            bool can_run = true;
            for (int r = 0; r < NUM_RESOURCES; r++) {
                if (d->need[r] > work[r]) { can_run = false; break; }
            }

            if (can_run) {
                for (int r = 0; r < NUM_RESOURCES; r++)
                    work[r] += d->allocation[r];
                finish[i] = true;
                seq[(*seq_len)++] = i;
                count++;
                progressed = true;
            }
        }

        if (!progressed) break; /* no department can proceed -> unsafe */
    }

    return (count == sys_state.num_dept);
}

/* ---------------------------------------------------------------------- */
/* Banker's Algorithm — Resource-Request Algorithm                        */
/* ---------------------------------------------------------------------- */

typedef enum { REQ_GRANTED, REQ_DENIED_UNSAFE, REQ_DENIED_EXCEEDS_NEED,
               REQ_DENIED_NOT_AVAILABLE, REQ_INVALID } RequestResult;

static RequestResult request_resources(int dept_idx, const int req[NUM_RESOURCES]) {
    if (dept_idx < 0 || dept_idx >= sys_state.num_dept) return REQ_INVALID;
    Department *d = &sys_state.dept[dept_idx];

    /* Step 1: request must not exceed declared need */
    for (int r = 0; r < NUM_RESOURCES; r++) {
        if (req[r] > d->need[r]) {
            log_event("DENY  %-14s request exceeds declared MAX need for %s.",
                       d->name, RES_NAME[r]);
            return REQ_DENIED_EXCEEDS_NEED;
        }
    }

    /* Step 2: request must not exceed currently available resources */
    for (int r = 0; r < NUM_RESOURCES; r++) {
        if (req[r] > sys_state.available[r]) {
            d->blocked = true;
            log_event("WAIT  %-14s blocked: insufficient %s (need %d, avail %d).",
                       d->name, RES_NAME[r], req[r], sys_state.available[r]);
            return REQ_DENIED_NOT_AVAILABLE;
        }
    }

    /* Step 3: tentatively allocate, then test system safety */
    int save_avail[NUM_RESOURCES], save_alloc[NUM_RESOURCES], save_need[NUM_RESOURCES];
    memcpy(save_avail, sys_state.available, sizeof(save_avail));
    memcpy(save_alloc, d->allocation, sizeof(save_alloc));
    memcpy(save_need,  d->need,       sizeof(save_need));

    for (int r = 0; r < NUM_RESOURCES; r++) {
        sys_state.available[r] -= req[r];
        d->allocation[r]       += req[r];
        d->need[r]              = d->max[r] - d->allocation[r];
    }

    int seq[MAX_DEPTS], seq_len;
    if (is_safe_state(seq, &seq_len)) {
        d->blocked = false;
        log_event("GRANT %-14s allocated request; system remains SAFE.", d->name);
        return REQ_GRANTED;
    } else {
        /* Roll back — granting this request would risk deadlock */
        memcpy(sys_state.available, save_avail, sizeof(save_avail));
        memcpy(d->allocation,       save_alloc, sizeof(save_alloc));
        memcpy(d->need,             save_need,  sizeof(save_need));
        d->blocked = true;
        log_event("DENY  %-14s request refused: would move system to UNSAFE state.",
                   d->name);
        return REQ_DENIED_UNSAFE;
    }
}

/* Release resources back to the pool (like a process exiting / freeing). */
static void release_resources(int dept_idx, const int rel[NUM_RESOURCES]) {
    if (dept_idx < 0 || dept_idx >= sys_state.num_dept) return;
    Department *d = &sys_state.dept[dept_idx];
    for (int r = 0; r < NUM_RESOURCES; r++) {
        int amt = rel[r];
        if (amt > d->allocation[r]) amt = d->allocation[r];
        d->allocation[r]       -= amt;
        sys_state.available[r] += amt;
        d->need[r]               = d->max[r] - d->allocation[r];
    }
    d->blocked = false;
    log_event("RELEASE %-14s returned resources to the shared pool.", d->name);
}

/* ---------------------------------------------------------------------- */
/* Deadlock detection (wait-for graph cycle check, informational)         */
/* ---------------------------------------------------------------------- */

static bool detect_deadlock(int stuck[], int *stuck_len) {
    int seq[MAX_DEPTS], seq_len;
    bool safe = is_safe_state(seq, &seq_len);
    *stuck_len = 0;

    if (!safe) {
        bool in_seq[MAX_DEPTS] = {false};
        for (int i = 0; i < seq_len; i++) in_seq[seq[i]] = true;
        for (int i = 0; i < sys_state.num_dept; i++)
            if (!in_seq[i]) stuck[(*stuck_len)++] = i;
    }
    return !safe;
}

/* ---------------------------------------------------------------------- */
/* Scheduler: Priority + Round-Robin hybrid for pending requests          */
/* ---------------------------------------------------------------------- */

typedef struct {
    int dept_idx;
    int req[NUM_RESOURCES];
} PendingRequest;

/* Sort indices of pending[] array by department priority (ascending value
 * = higher urgency), stable for equal priorities (round-robin order kept). */
static void schedule_and_process(PendingRequest *pending, int count) {
    /* simple stable insertion sort by priority */
    for (int i = 1; i < count; i++) {
        PendingRequest key = pending[i];
        int keypr = sys_state.dept[key.dept_idx].priority;
        int j = i - 1;
        while (j >= 0 && sys_state.dept[pending[j].dept_idx].priority > keypr) {
            pending[j + 1] = pending[j];
            j--;
        }
        pending[j + 1] = key;
    }

    printf("\n-- Scheduler dispatch order (by priority, RR among ties) --\n");
    for (int i = 0; i < count; i++) {
        Department *d = &sys_state.dept[pending[i].dept_idx];
        printf("  #%d %-14s (priority %d)\n", i + 1, d->name, d->priority);
    }

    for (int i = 0; i < count; i++) {
        RequestResult r = request_resources(pending[i].dept_idx, pending[i].req);
        Department *d = &sys_state.dept[pending[i].dept_idx];
        const char *verdict =
            (r == REQ_GRANTED) ? "GRANTED" :
            (r == REQ_DENIED_UNSAFE) ? "DENIED (unsafe)" :
            (r == REQ_DENIED_EXCEEDS_NEED) ? "DENIED (exceeds need)" :
            (r == REQ_DENIED_NOT_AVAILABLE) ? "DENIED (unavailable)" : "INVALID";
        printf("  -> %-14s : %s\n", d->name, verdict);
    }
}

/* ---------------------------------------------------------------------- */
/* Display helpers                                                        */
/* ---------------------------------------------------------------------- */

static void print_resource_summary(void) {
    printf("\n===================== CAMPUS RESOURCE POOL =========================\n");
    printf("%-18s %10s %10s %10s\n", "Resource", "Total", "Available", "In-Use");
    for (int r = 0; r < NUM_RESOURCES; r++) {
        printf("%-18s %10d %10d %10d\n", RES_NAME[r], sys_state.total[r],
               sys_state.available[r], sys_state.total[r] - sys_state.available[r]);
    }
    printf("=====================================================================\n");
}

static void print_dept_table(void) {
    printf("\n=================== DEPARTMENT ALLOCATION TABLE ====================\n");
    printf("%-14s %-4s | %-24s | %-24s | %-24s\n",
           "Dept", "Pri", "Allocation", "Max", "Need");
    for (int i = 0; i < sys_state.num_dept; i++) {
        Department *d = &sys_state.dept[i];
        char alloc_s[64] = "", max_s[64] = "", need_s[64] = "";
        char tmp[16];
        for (int r = 0; r < NUM_RESOURCES; r++) {
            snprintf(tmp, sizeof(tmp), "%d ", d->allocation[r]); strcat(alloc_s, tmp);
            snprintf(tmp, sizeof(tmp), "%d ", d->max[r]);        strcat(max_s, tmp);
            snprintf(tmp, sizeof(tmp), "%d ", d->need[r]);       strcat(need_s, tmp);
        }
        printf("%-14s %-4d | %-24s | %-24s | %-24s %s\n",
               d->name, d->priority, alloc_s, max_s, need_s,
               d->blocked ? "[BLOCKED]" : "");
    }
    printf("=====================================================================\n");
}

/* ---------------------------------------------------------------------- */
/* Demo scenario driver                                                   */
/* ---------------------------------------------------------------------- */

static void run_demo_scenario(void) {
    printf("\n#################### UniCore SIMULATION START #####################\n");
    print_resource_summary();
    print_dept_table();

    /* --- Round 1: a batch of simultaneous requests arrives ------------ */
    PendingRequest batch1[] = {
        {0 /*CSE*/,        {1, 8, 1, 15, 2}},
        {1 /*ECE*/,        {1, 4, 0,  8, 3}},
        {2 /*Mechanical*/, {1, 2, 1,  4, 5}},
        {3 /*Admin*/,      {1, 1, 1,  2, 2}},
        {4 /*Library*/,    {1, 3, 0,  5, 1}},
        {5 /*Exam-Cell*/,  {1, 2, 1,  4, 1}},
    };
    schedule_and_process(batch1, (int)(sizeof(batch1) / sizeof(batch1[0])));
    print_resource_summary();
    print_dept_table();

    /* --- Round 2: a request that would push the system into an unsafe
     *              state is deliberately issued to demonstrate Banker's
     *              deadlock-avoidance refusal.                          */
    printf("\n-- Testing an over-ambitious request (CSE wants remaining Lab-PCs) --\n");
    int risky_req[NUM_RESOURCES] = {1, 12, 1, 20, 3};
    RequestResult rr = request_resources(0, risky_req);
    printf("Result: %s\n",
           rr == REQ_GRANTED ? "GRANTED" :
           rr == REQ_DENIED_UNSAFE ? "DENIED - would enter UNSAFE state (deadlock risk)" :
           rr == REQ_DENIED_EXCEEDS_NEED ? "DENIED - exceeds declared max need" :
           "DENIED - not enough available resources");

    /* --- Round 3: Exam-Cell finishes and releases its resources -------- */
    printf("\n-- Exam-Cell finishes and releases all held resources --\n");
    int rel[NUM_RESOURCES];
    memcpy(rel, sys_state.dept[5].allocation, sizeof(rel));
    release_resources(5, rel);
    print_resource_summary();

    /* --- Round 4: deadlock detection sweep ------------------------------ */
    int stuck[MAX_DEPTS], stuck_len;
    bool dl = detect_deadlock(stuck, &stuck_len);
    printf("\n-- Deadlock detection sweep --\n");
    if (dl) {
        printf("WARNING: potential deadlock involving:");
        for (int i = 0; i < stuck_len; i++)
            printf(" %s", sys_state.dept[stuck[i]].name);
        printf("\n");
        log_event("DEADLOCK CHECK: unsafe cycle detected among %d department(s).", stuck_len);
    } else {
        printf("System state is SAFE. No deadlock detected.\n");
        log_event("DEADLOCK CHECK: system confirmed safe.");
    }

    print_dept_table();
    print_log();
    printf("\n##################### UniCore SIMULATION END #######################\n");
}

/* ---------------------------------------------------------------------- */
/* Interactive menu (optional manual exploration)                         */
/* ---------------------------------------------------------------------- */

static void interactive_menu(void) {
    int choice;
    do {
        printf("\n======================= UniCore MENU =========================\n");
        printf(" 1. Show resource pool\n");
        printf(" 2. Show department allocation table\n");
        printf(" 3. Request resources for a department\n");
        printf(" 4. Release resources from a department\n");
        printf(" 5. Run safety check (Banker's Algorithm)\n");
        printf(" 6. Run deadlock detection sweep\n");
        printf(" 7. Show event log\n");
        printf(" 8. Run full demo scenario (resets state)\n");
        printf(" 0. Exit\n");
        printf("Choice: ");
        if (scanf("%d", &choice) != 1) { while (getchar() != '\n'); choice = -1; }

        switch (choice) {
            case 1: print_resource_summary(); break;
            case 2: print_dept_table(); break;
            case 3: {
                printf("Department index (0-%d):\n", sys_state.num_dept - 1);
                for (int i = 0; i < sys_state.num_dept; i++)
                    printf("  %d = %s\n", i, sys_state.dept[i].name);
                int idx; printf("Select: "); if (scanf("%d", &idx) != 1) idx = -1;
                int req[NUM_RESOURCES];
                for (int r = 0; r < NUM_RESOURCES; r++) {
                    printf("  Requested %s: ", RES_NAME[r]);
                    if (scanf("%d", &req[r]) != 1) req[r] = 0;
                }
                RequestResult rr = request_resources(idx, req);
                printf("Result: %s\n",
                       rr == REQ_GRANTED ? "GRANTED" :
                       rr == REQ_DENIED_UNSAFE ? "DENIED (unsafe state)" :
                       rr == REQ_DENIED_EXCEEDS_NEED ? "DENIED (exceeds max need)" :
                       rr == REQ_DENIED_NOT_AVAILABLE ? "DENIED (not enough available)" :
                       "INVALID");
                break;
            }
            case 4: {
                printf("Department index (0-%d):\n", sys_state.num_dept - 1);
                for (int i = 0; i < sys_state.num_dept; i++)
                    printf("  %d = %s\n", i, sys_state.dept[i].name);
                int idx; printf("Select: "); if (scanf("%d", &idx) != 1) idx = -1;
                int rel[NUM_RESOURCES];
                for (int r = 0; r < NUM_RESOURCES; r++) {
                    printf("  Release amount of %s: ", RES_NAME[r]);
                    if (scanf("%d", &rel[r]) != 1) rel[r] = 0;
                }
                release_resources(idx, rel);
                break;
            }
            case 5: {
                int seq[MAX_DEPTS], seq_len;
                if (is_safe_state(seq, &seq_len)) {
                    printf("System is SAFE. Safe sequence: ");
                    for (int i = 0; i < seq_len; i++)
                        printf("%s ", sys_state.dept[seq[i]].name);
                    printf("\n");
                } else {
                    printf("System is UNSAFE — no safe sequence exists.\n");
                }
                break;
            }
            case 6: {
                int stuck[MAX_DEPTS], stuck_len;
                if (detect_deadlock(stuck, &stuck_len)) {
                    printf("Deadlock risk involving:");
                    for (int i = 0; i < stuck_len; i++)
                        printf(" %s", sys_state.dept[stuck[i]].name);
                    printf("\n");
                } else {
                    printf("No deadlock detected.\n");
                }
                break;
            }
            case 7: print_log(); break;
            case 8: init_system(); run_demo_scenario(); break;
            case 0: printf("Shutting down UniCore. Goodbye.\n"); break;
            default: printf("Invalid choice.\n");
        }
    } while (choice != 0);
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    init_system();

    if (argc > 1 && strcmp(argv[1], "--interactive") == 0) {
        interactive_menu();
    } else {
        run_demo_scenario();
        printf("\n(Tip: run with '--interactive' for a menu-driven session,\n");
        printf(" e.g.  ./unicore --interactive)\n");
    }
    return 0;
}
