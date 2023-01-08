#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <dirent.h>

#define NUM_VALID_ARGS 4
#define SEARCH_PERMISSIONS (R_OK | X_OK)
#define PARENT_DIR ".."
#define CURR_DIR "."
#define SUCCESS 0
#define FAILURE (-1)

//-------------------------- Queue Implementation-----------------------------------
struct qnode { // queue node
    void *value; // pointer to the value
    struct qnode *next;
} typedef qnode;

struct queue { // general queue
    qnode *head;
    qnode *tail;
    int len; // # of nodes in the list
} typedef queue;

// Enqueueing value to the tail of q.
// return 0 on success, -1 on failure.
int enqueue(queue *q, void *value) {
    qnode *node = (qnode *) malloc(sizeof(qnode)); // Alloc new node
    if (node == NULL) { // Alloc fail
        fprintf(stderr, "qnode allocation fail: %s\n", strerror(ENOMEM));
        return FAILURE;
    }
    // Node assignment
    node->value = value;
    node->next = NULL;

    if (q->len == 0) { // Empty - head & tail point ot the new node
        q->head = node;
        q->tail = node;
    } else { // Add to the end of the queue
        q->tail->next = node;
        q->tail = node;
    }
    q->len++;
    return SUCCESS;
}

// Dequeue value from the head of q.
// User responsible to free dequeued nodes after he finishes work.
// return 0 on success, -1 on failure.
int dequeue(queue *q, qnode **node) {
    if (q->len == 0) // Empty
        return FAILURE;
    *node = q->head; // Node point to the dequeued value
    q->head = (*node)->next;
    q->len--;
    return SUCCESS;
}

// Helper function to enqueue a dir path (string) to the queue.
// return 0 on success, -1 on failure.
int enqueue_dir(queue *queue, char *new_dir) {
    char *path = (char *) malloc(PATH_MAX); // Alloc mem for new dir
    if (path == NULL) { // Alloc fail
        fprintf(stderr, "path copy alloc fail: %s\n", strerror(ENOMEM));
        return FAILURE;
    }
    strcpy(path, new_dir); // Copy new dir to path

    return enqueue(queue, (void *)path); // Use regular enqueue
}

//-------------------------- Global Vars - Shared between threads ----------------------------------

static int N_THREADS; // # of threads - user input
static int keep_search; // if 0, all threads stop searching
static queue *qdir; // Queue for the dir entries
static queue *qthreads; // Queue for the threads cond_var - maintain waking order
static char *search_term; // The search term - user input
static atomic_int search_appear; // # of term's appearances during the search
static atomic_int threads_ready; // # of threads ready to begin search
static atomic_int threads_failed; // # of threads failed == stopped searching
static mtx_t start_mutex, queue_mutex;              // Cond & mutex: start - for the threads' init
static cnd_t start_cnd, queue_cnd, main_thd_cond;   // queue for the shared queue resource, main_thd for main thread wait while init

//------------------------------------ Help Functions ----------------------------------------

// On success return 1 if path direct a dir, if stat fail return -1.
int isDir(char *path) {
    int rc; // return value
    struct stat st;

    rc = stat(path, &st);
    if (rc != SUCCESS) {
        fprintf(stderr, "Stat function failed on path %s: %s.\n", path, strerror(errno));
        return rc;
    }
    return S_ISDIR(st.st_mode); // Using macro S_ISDIR on mode to determine if a dir
}

// Search for term inside dir 'path'. If found new subdirs add to queue.
// return 0 on success, -1 on failure.
int search(char *path) {
    int ret_status = SUCCESS, status; // return status for the function
    struct dirent *entry;
    char new_path[PATH_MAX]; // Temp array to hold nwe paths
    qnode *thread_node = NULL;

    DIR *search_dir = opendir(path);
    if (search_dir == NULL) { // Open fail
        fprintf(stderr, "Open %s fail: %s.\n", path, strerror(errno));
        return FAILURE;
    }

    while (!(errno = 0) && (entry=readdir(search_dir)) != NULL) { // errno set to 0 to distinguish between end of stream & error
        if (!strcmp(entry->d_name, PARENT_DIR) || !strcmp(entry->d_name, CURR_DIR)) continue;
        if (sprintf(new_path, "%s/%s", path, entry->d_name) < 0) { // Fail to concat
            fprintf(stderr, "Open %s fail: %s.\n", path, strerror(errno));
            ret_status = FAILURE;
            continue;
        }

        status = isDir(new_path);
        if (status == FAILURE) { // isDir fail
            ret_status = status;
            continue;
        } else if (status == 1) { // Entry is a dir
            if (access(new_path, SEARCH_PERMISSIONS) != SUCCESS) { // No access
                printf("Directory %s: Permission denied.\n", new_path);
                continue;
            }
            // accessible dir
            mtx_lock(&queue_mutex);
            if (enqueue_dir(qdir, new_path) == FAILURE) { // Enqueue new dir entry
                ret_status = FAILURE;
            } else if(dequeue(qthreads, &thread_node) != FAILURE) { // Signal waiting thread (if exist) according to queue order
                cnd_signal(thread_node->value);
                free(thread_node); // Free dequeued node
            }
            mtx_unlock(&queue_mutex);
        } else if(strstr(entry->d_name, search_term) != NULL) { // Entry is a regular file and contain term in filename
            search_appear++;
            printf("%s\n", new_path);
        }
    }
    if (errno != 0) ret_status = FAILURE; // readdir error
    closedir(search_dir);

    return ret_status;
}

// Validate and assign user input
// If failed, exit program.
void validate_user_input(int argc, char *argv[], char **root_dir) {
    int rc; // Return value

    if (argc != NUM_VALID_ARGS) { // Valid # of arguments
        fprintf(stderr, "Number of passed arguments should be %d: %s.\n", NUM_VALID_ARGS - 1, strerror(EINVAL));
        exit(EXIT_FAILURE);
    }
    N_THREADS = atoi(argv[3]);
    *root_dir = argv[1];
    search_term = argv[2];

    // Check that root dir is an accessible dir!
    if (access(*root_dir, SEARCH_PERMISSIONS) != SUCCESS) { // Not accessible entry
        fprintf(stderr, "Can't access root path %s: %s.\n", *root_dir, strerror(errno));
        exit(EXIT_FAILURE);
    }
    rc = isDir(*root_dir);
    if (rc == FAILURE) // isDIr fail
        exit(EXIT_FAILURE);
    else if (rc == 0) { // Not a dir
        fprintf(stderr, "Root path %s isn't a directory: %s.\n", *root_dir, strerror(ENOTDIR));
        exit(EXIT_FAILURE);
    }
}

// Init 2 queues & mutex/cond_vars for the program.
// qdir for dir entries queue. qthreads for cond_vars of threads.
// If failed, exit program.
void init_queues_and_values(char *root_dir) {
    qdir = (queue *) malloc(sizeof(queue));
    if (qdir == NULL) {
        fprintf(stderr, "Queue directories allocation fail: %s\n", strerror(ENOMEM));
        exit(EXIT_FAILURE);
    }
    qdir->head = qdir->tail = NULL;
    qdir->len=0;
    if (enqueue_dir(qdir, root_dir) == FAILURE) {
        exit(EXIT_FAILURE);
    }

    qthreads = (queue *) malloc(sizeof(queue));
    if (qthreads == NULL) {
        fprintf(stderr, "Queue threads conds allocation fail: %s\n", strerror(ENOMEM));
        exit(EXIT_FAILURE);
    }
    qthreads->head = qthreads->tail = NULL;
    qthreads->len=0;

    // Initialize mutex and condition variable objects
    mtx_init(&start_mutex, mtx_plain);
    mtx_init(&queue_mutex, mtx_plain);
    cnd_init(&start_cnd);
    cnd_init(&queue_cnd);
    cnd_init(&main_thd_cond);

    // Init global vars
    search_appear = threads_ready = threads_failed = 0;
    keep_search = 1;
}

// Free al resources
void clean_up() {
    qnode *node;

    // destroy mutex & cond_vars
    mtx_destroy(&start_mutex);
    mtx_destroy(&queue_mutex);
    cnd_destroy(&start_cnd);
    cnd_destroy(&queue_cnd);
    cnd_destroy(&main_thd_cond);

    // Free all queues - If needed
    node = qdir->head;
    while (node != NULL) {
        qdir->head = node->next;
        free(node);
        node = qdir->head;
    }
    free(qdir);

    node = qthreads->head;
    while (node != NULL) {
        qthreads->head = node->next;
        free(node);
        node = qthreads->head;
    }
    free(qthreads);
}

//---------------------------- Thread Function ---------------------------------
// Main function for searching threads. exit with 0 on success & -1 on failure.
int thread_search(void *t) {
    int status, exit_status = SUCCESS, q_status = SUCCESS;
    qnode *deq_node = NULL;
    cnd_t thread_cnd; // Unique cond_Var for current thread
    cnd_init(&thread_cnd);

    threads_ready++;
    mtx_lock(&start_mutex);
    if (threads_ready == N_THREADS) { // All threads initiated
        cnd_signal(&main_thd_cond); // Wake up main thread
    }
    while (threads_ready != N_THREADS) { // Not all threads init - wait
        cnd_wait(&start_cnd, &start_mutex);
    }
    mtx_unlock(&start_mutex);

    while (1) {
        mtx_lock(&queue_mutex);
        while (keep_search && qdir->len == 0) { // Qdir empty
            if (N_THREADS - qthreads->len - threads_failed == 1) { // This is the only live thread and qdir is empty
                keep_search = 0; // Stop search
                while (dequeue(qthreads, &deq_node) != FAILURE) { // Wake up all waiting threads
                    cnd_signal(deq_node->value);
                    free(deq_node);
                }
                break;
            }
            // Put thread to wait, enqueue to qthread
            q_status = enqueue(qthreads, &thread_cnd);
            if (q_status == FAILURE) { // Alloc fail
                exit_status = EXIT_FAILURE;
                threads_failed++; // Fail alloc - thread die
                break;
            }
            cnd_wait(&thread_cnd, &queue_mutex);
        }
        if (q_status == FAILURE || !keep_search) { // Stop searching for curr thread (possibly all)
            mtx_unlock(&queue_mutex);
            break;
        }
        if (dequeue(qdir, &deq_node) == FAILURE) {
            fprintf(stderr, "Unexpected behavior on dequeue: %s\n", strerror(ENODATA));
            q_status = FAILURE;
            exit_status = EXIT_FAILURE;
            threads_failed++; // Fail deq - thread die
        }
        mtx_unlock(&queue_mutex);
        if (q_status == FAILURE) break; // Queue op fail

        status = search(deq_node->value); // Search method
        if (status == FAILURE) exit_status = EXIT_FAILURE; // If search encounter an error, thread will exit with 1
        free(deq_node->value); // Free dequeued resource
        free(deq_node);
    }

    cnd_destroy(&thread_cnd); // Unique cond destroy
    thrd_exit(exit_status);
}

//-------------------------------- MAIN ---------------------------------
int main(int argc, char *argv[]) {
    int status, exit_status = SUCCESS;
    char *root_dir;

    validate_user_input(argc, argv, &root_dir);
    init_queues_and_values(root_dir);

    thrd_t threads[N_THREADS];
    mtx_lock(&start_mutex);
    for (int i = 0; i < N_THREADS; ++i) { // Create threads
        if (thrd_create(&threads[i], thread_search, NULL) != thrd_success) {
            fprintf(stderr, "ERROR in thrd_create()\n");
            exit(EXIT_FAILURE);
        }
    }
    cnd_wait(&main_thd_cond, &start_mutex); // Wait for all thread to init
    cnd_broadcast(&start_cnd); // Wakeup all searching threads
    mtx_unlock(&start_mutex);

    // Wait for all threads to complete
    for (int i = 0; i < N_THREADS; i++) {
        if (thrd_join(threads[i], &status) != thrd_success) {
            fprintf(stderr, "ERROR in thrd_join().\n");
            exit(EXIT_FAILURE);
        }
        if (status != SUCCESS) exit_status = EXIT_FAILURE; // At least one thread encountered an error
    }

    printf("Done searching, found %d files\n", search_appear);

    clean_up(); // Clean up and exit
    return exit_status;
}
//============================== END OF FILE =================================
