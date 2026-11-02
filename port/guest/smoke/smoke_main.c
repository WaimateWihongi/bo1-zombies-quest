/* Milestone 2 smoke test: runs as the guest's main and reports each check to logcat. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int smoke_cxx(void);

/* in another file from smoke_cxx's static constructor, so it really runs at startup */
int smoke_seed(void)
{
    return 1234;
}

static int failures;

#define CHECK(name, cond)                                                   \
    do {                                                                    \
        int ok_ = (cond);                                                   \
        printf("[smoke] %-28s %s\n", name, ok_ ? "ok" : "FAILED");          \
        if (!ok_) failures++;                                               \
    } while (0)

static _Thread_local int tls_value = 7;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int counter;

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    tls_value = 100 + id;
    for (int i = 0; i < 10000; i++) {
        pthread_mutex_lock(&mutex);
        counter++;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mutex);
    }
    return (void *)(intptr_t)(tls_value == 100 + id);
}

static jmp_buf jump;

static void jump_back(int value)
{
    longjmp(jump, value);
}

int main(int argc, char **argv)
{
    printf("[smoke] guest main: argc %d, argv[0] %s, sizeof(void *) %u, sizeof(long) %u\n", argc,
           argv[0], (unsigned)sizeof(void *), (unsigned)sizeof(long));
    CHECK("ILP32 data model", sizeof(void *) == 4 && sizeof(long) == 4 && sizeof(long long) == 8);

    void *big = malloc(64 << 20);
    CHECK("malloc 64 MB below 4 GB", big && (unsigned long long)(uintptr_t)big < 0x100000000ull);
    if (big) memset(big, 0x5a, 64 << 20);
    free(big);
    char *small = malloc(100);
    snprintf(small, 100, "%d-%.2f-%s", 42, 3.14159, "x");
    CHECK("snprintf", !strcmp(small, "42-3.14-x"));
    free(small);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec nap = {0, 20 * 1000 * 1000};
    nanosleep(&nap, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ms = (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK("clock + nanosleep 20 ms", ms >= 19 && ms < 500);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++) pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
    int tls_ok = 1;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(threads[i], &r);
        tls_ok &= (int)(intptr_t)r;
    }
    CHECK("4 threads + mutex", counter == 40000);
    CHECK("thread-local storage", tls_ok && tls_value == 7);

    int v = setjmp(jump);
    if (!v) jump_back(5);
    CHECK("setjmp/longjmp", v == 5);

    const char *root = getenv("BO1_DATA_ROOT");
    CHECK("BO1_DATA_ROOT set", root != NULL);
    if (root) {
        char path[512];
        snprintf(path, sizeof path, "%s/smoke.txt", root);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "hello from the guest\n");
            fclose(f);
        }
        char buf[64] = {0};
        f = fopen(path, "r");
        if (f) {
            fgets(buf, sizeof buf, f);
            fclose(f);
        }
        struct stat st;
        CHECK("file write/read/stat", !strcmp(buf, "hello from the guest\n") && !stat(path, &st) &&
                                          st.st_size == 21);
        printf("[smoke] data folder %s (errno %d)\n", root, errno);
    }

    CHECK("C++ (ctors, virtuals, new)", smoke_cxx());

    printf("[smoke] done: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
