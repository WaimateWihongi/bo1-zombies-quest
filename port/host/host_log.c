/* Logging to logcat (tag BO1). The guest's stdout and stderr arrive here a write at a time and
   leave as whole lines. */
#include "host.h"

#include <android/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_logf(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    __android_log_vprint(priority, HOST_TAG, format, ap);
    va_end(ap);
}

_Noreturn void host_fatal(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    __android_log_vprint(ANDROID_LOG_FATAL, HOST_TAG, format, ap);
    va_end(ap);
    abort();
}

/* imports */
void host_log(int priority, const char *text)
{
    __android_log_write(priority, HOST_TAG, text);
}

_Noreturn void host_abort(const char *why)
{
    host_fatal("guest: %s", why);
}

static pthread_mutex_t line_lock = PTHREAD_MUTEX_INITIALIZER;
static char line[2][1024];
static size_t line_length[2];

void host_log_guest_write(int fd, const char *data, size_t size)
{
    int i = fd == 2;
    pthread_mutex_lock(&line_lock);
    for (size_t k = 0; k < size; k++) {
        char c = data[k];
        if (c != '\n' && line_length[i] < sizeof line[i] - 1) {
            line[i][line_length[i]++] = c;
            continue;
        }
        line[i][line_length[i]] = 0;
        __android_log_write(i ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, HOST_TAG, line[i]);
        line_length[i] = 0;
        if (c != '\n') line[i][line_length[i]++] = c;
    }
    pthread_mutex_unlock(&line_lock);
}
