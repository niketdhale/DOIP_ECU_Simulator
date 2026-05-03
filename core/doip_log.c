#include "core/doip_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static uint8_t          g_level   = DOIP_LOG_DEFAULT_LEVEL;
static uint32_t         g_modules = DOIP_LOG_MODULE_ALL;
static FILE            *g_file    = NULL;
static pthread_mutex_t  g_mutex   = PTHREAD_MUTEX_INITIALIZER;

#define COLOR_RESET  "\033[0m"

static const char *level_labels[] = { "ERROR", "WARN ", "INFO ", "DEBUG", "VERB " };
static const char *level_colors[] = {
    "\033[31m",  /* ERROR — red    */
    "\033[33m",  /* WARN  — yellow */
    "\033[32m",  /* INFO  — green  */
    "\033[36m",  /* DEBUG — cyan   */
    "\033[37m"   /* VERB  — white  */
};

int DoIP_Log_Init(uint8_t level, uint32_t module_filter, const char *file_path) {
    pthread_mutex_lock(&g_mutex);
    g_level   = level;
    g_modules = module_filter;
    if (g_file) { fclose(g_file); g_file = NULL; }
    if (file_path) {
        g_file = fopen(file_path, "a");
        if (!g_file) { pthread_mutex_unlock(&g_mutex); return -1; }
    }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void DoIP_Log_DeInit(void) {
    pthread_mutex_lock(&g_mutex);
    if (g_file) { fclose(g_file); g_file = NULL; }
    pthread_mutex_unlock(&g_mutex);
}

void DoIP_Log_SetLevel(uint8_t level) {
    pthread_mutex_lock(&g_mutex);
    g_level = level;
    pthread_mutex_unlock(&g_mutex);
}

void doip_log_write(uint8_t level, uint32_t module,
                    const char *file, int line,
                    const char *fmt, ...) {
    if (level > g_level) return;
    if (module != 0 && !(module & g_modules)) return;

    pthread_mutex_lock(&g_mutex);

    FILE *out = g_file ? g_file : stderr;
    int use_color = DOIP_LOG_ENABLE_COLORS && (out == stderr);

#if DOIP_LOG_ENABLE_TIMESTAMPS
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(out, "[%4lu.%03lu] ",
            (unsigned long)(ts.tv_sec % 10000),
            (unsigned long)(ts.tv_nsec / 1000000U));
#endif

    if (use_color)
        fprintf(out, "%s[%s]%s ", level_colors[level], level_labels[level], COLOR_RESET);
    else
        fprintf(out, "[%s] ", level_labels[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

#if DOIP_LOG_ENABLE_SOURCE_LOC
    const char *base = strrchr(file, '/');
    if (!base) base = strrchr(file, '\\');
    base = base ? base + 1 : file;
    fprintf(out, "  (%s:%d)", base, line);
#else
    (void)file; (void)line;
#endif

    fprintf(out, "\n");
    fflush(out);

    pthread_mutex_unlock(&g_mutex);
}
