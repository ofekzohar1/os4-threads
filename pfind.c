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

struct qnode {
    void *value;
    struct qnode *next;
} typedef qnode;

struct queue {
    qnode *head;
    qnode *tail;
    int len;
} typedef queue;

int enqueue(queue *q, void *value) {
    qnode *node = (qnode *) malloc(sizeof(qnode));
    if (node == NULL) {
        fprintf(stderr, "qnode allocation fail: %s\n", strerror(ENOMEM));
        return -1;
    }
    node->value = value;
    node->next = NULL;

    if (q->head == NULL) { // Empty
        q->head = node;
        q->tail = node;
        q->len = 1;
    } else {
        q->tail->next = node;
        q->tail = node;
        q->len++;
    }
    return 0;
}

int dequeue(queue *q, qnode **node) {
    if (q->head == NULL) // Empty
        return -1;
    *node = q->head;
    q->head = (*node)->next;
    q->len--;
    return 0;
}

int enqueue_dir(queue *queue, char *new_dir) {
    char *path = (char *) malloc(PATH_MAX);
    if (path == NULL) {
        fprintf(stderr, "path copy alloc fail: %s\n", strerror(ENOMEM));
        return -1;
    }
    strcpy(path, new_dir);

    return enqueue(queue, (void *)path);
}

static int N_THREADS;
static int keep_search;
static queue *qdir;
static queue *qthreads;
static char *search_term;
static atomic_int search_appear;
static atomic_int threads_ready;
static atomic_int threads_failed;
static mtx_t start_mutex, qdir_mutex;
static cnd_t start_cnd, qdir_cnd;

int isDir(char *path) {
    int rc;
    struct stat st;
    rc = stat(path, &st);
    if (rc != 0) {
        fprintf(stderr, "Stat function failed on path %s: %s.\n", path, strerror(errno));
        return rc;
    }
    return S_ISDIR(st.st_mode);
}
//----------------------------------------------------------------------------
int search(char *path) {
    int exit_status = 0, status;
    struct dirent *entry;
    char new_path[PATH_MAX];
    qnode *thread_node = NULL;

    DIR *search_dir = opendir(path);
    if (search_dir == NULL) { // Open fail
        fprintf(stderr, "Open %s fail: %s.\n", path, strerror(errno));
        return EXIT_FAILURE;
    }
    errno = 0;
    while ((entry=readdir(search_dir)) != NULL) {
        if (!strcmp(entry->d_name, PARENT_DIR) || !strcmp(entry->d_name, CURR_DIR)) continue;
        if (sprintf(new_path, "%s/%s", path, entry->d_name) < 0) {
            fprintf(stderr, "Open %s fail: %s.\n", path, strerror(errno));
            exit_status = EXIT_FAILURE;
            continue;
        }
        status = isDir(new_path);
        if (status == -1) {
            exit_status = EXIT_FAILURE;
            continue;
        } else if(status == 1) { // Entry is a dir
            if (access(new_path, SEARCH_PERMISSIONS) != 0) {
                printf("Directory %s: Permission denied.\n", new_path);
                continue;
            }
            mtx_lock(&qdir_mutex);
            if (enqueue_dir(qdir, new_path) == -1) { // Enqueue fail
                exit_status = EXIT_FAILURE;
            } else if(dequeue(qthreads, &thread_node) != -1) { // New dir enqueued and there is a thread waiting
                cnd_signal(thread_node->value);
                free(thread_node);
            }
            mtx_unlock(&qdir_mutex);
        } else if(strstr(entry->d_name, search_term) != NULL) { // Entry is a regular file and contain term in filename
            search_appear++;
            printf("%s\n", new_path);
        }
    }
    if (errno != 0) exit_status = EXIT_FAILURE;
    closedir(search_dir);

    return exit_status;
}

//----------------------------------------------------------------------------

int thread_search(void *t) {
    int status, exit_status = 0, q_status = 0;
    qnode *deq_node = NULL;
    cnd_t thread_cnd;
    cnd_init(&thread_cnd);

    threads_ready++;
    mtx_lock(&start_mutex);
    if (threads_ready == N_THREADS) {
        cnd_broadcast(&start_cnd);
    }
    while (threads_ready != N_THREADS) {
        cnd_wait(&start_cnd, &start_mutex);
    }
    mtx_unlock(&start_mutex);

    while (1) {
        mtx_lock(&qdir_mutex);
        while (keep_search && qdir->len == 0) {
            if (N_THREADS - qthreads->len - threads_failed == 1) { // This is the only live thread and qdir is empty
                keep_search = 0;
                while (dequeue(qthreads, &deq_node) != -1) { // Not empty
                    cnd_signal(deq_node->value);
                    free(deq_node);
                }
                break;
            }
            q_status = enqueue(qthreads, &thread_cnd);
            if (q_status == -1) { // Alloc fail
                exit_status = EXIT_FAILURE;
                threads_failed++;
                break;
            }
            cnd_wait(&thread_cnd, &qdir_mutex);
        }
        if (q_status == -1 || !keep_search) { // Stop searching for this thread
            mtx_unlock(&qdir_mutex);
            break;
        }
        if (dequeue(qdir, &deq_node) == -1) {
            fprintf(stderr, "Unexpected behavior on dequeue: %s\n", strerror(ENODATA));
            q_status = -1;
            exit_status = EXIT_FAILURE;
            threads_failed++;
        }
        mtx_unlock(&qdir_mutex);
        if (q_status == -1) break; // Queue op fail
        status = search(deq_node->value);
        if (status == -1) exit_status = EXIT_FAILURE;
        free(deq_node->value);
        free(deq_node);
    }

    cnd_destroy(&thread_cnd);
    thrd_exit(exit_status);
}

//----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    int rc, status, exit_status;
    char *root_dir;

    if (argc != NUM_VALID_ARGS) {
        fprintf(stderr, "Number of passed arguments should be %d: %s.\n", NUM_VALID_ARGS - 1,
                strerror(EINVAL));
        exit(EXIT_FAILURE);
    }
    N_THREADS = atoi(argv[3]);
    root_dir = argv[1];
    search_term = argv[2];
    if (access(root_dir, SEARCH_PERMISSIONS) != 0) {
        fprintf(stderr, "Can't access root path %s: %s.\n", root_dir, strerror(errno));
        exit(EXIT_FAILURE);
    }
    rc = isDir(root_dir);
    if (rc == -1)
        exit(EXIT_FAILURE);
    else if (rc == 0) {
        fprintf(stderr, "Root path %s isn't a directory: %s.\n", root_dir, strerror(ENOTDIR));
        exit(EXIT_FAILURE);
    }

    qdir = (queue *) malloc(sizeof(queue));
    if (qdir == NULL) {
        fprintf(stderr, "Queue directories allocation fail: %s\n", strerror(ENOMEM));
        exit(EXIT_FAILURE);
    }
    qdir->len=0;
    if (enqueue_dir(qdir, root_dir) == -1) {
        exit(EXIT_FAILURE);
    }

    qthreads = (queue *) malloc(sizeof(queue));
    if (qthreads == NULL) {
        fprintf(stderr, "Queue threads conds allocation fail: %s\n", strerror(ENOMEM));
        exit(EXIT_FAILURE);
    }
    qthreads->len=0;

    // Initialize mutex and condition variable objects
    mtx_init(&start_mutex, mtx_plain);
    mtx_init(&qdir_mutex, mtx_plain);
    cnd_init(&start_cnd);
    cnd_init(&qdir_cnd);

    search_appear = threads_ready = threads_failed = 0;
    keep_search = 1;
    thrd_t threads[N_THREADS];
    mtx_lock(&start_mutex);
    for (int i = 0; i < N_THREADS; ++i) {
        if (thrd_create(&threads[i], thread_search, NULL) != thrd_success) {
            fprintf(stderr, "ERROR in thrd_create()\n");
            exit(EXIT_FAILURE);
        }
    }
    cnd_wait(&start_cnd, &start_mutex);
    mtx_unlock(&start_mutex);

    exit_status = 0;
    // Wait for all threads to complete
    for (int i = 0; i < N_THREADS; i++) {
        thrd_join(threads[i], &status);
        if (status != 0) exit_status = EXIT_FAILURE;
    }

    printf("Done searching, found %d files\n", search_appear);

    // Clean up and exit
    mtx_destroy(&start_mutex);
    mtx_destroy(&qdir_mutex);
    cnd_destroy(&start_cnd);
    cnd_destroy(&qdir_cnd);
    free(qdir);
    free(qthreads);
    return exit_status;
}
//============================== END OF FILE =================================
