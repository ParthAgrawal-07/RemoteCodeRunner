#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>

#define TIMEOUT_SEC 3
#define MAX_MEM_BYTES (64*1024*1024)

volatile pid_t child_pid = 0;

void alarm_handler(int sig) {
    (void)sig;
    if (child_pid > 0) kill(child_pid, SIGKILL);
}

char* execute_code(const char *lang, const char *code) 
{
    int src_fd;
    char src_path[256];

    
    if (strcmp(lang, "C") == 0) {
        strcpy(src_path, "/tmp/codeXXXXXX.c");
        src_fd = mkstemps(src_path, 2);
    } 
    else if (strcmp(lang, "Python") == 0) {
        strcpy(src_path, "/tmp/codeXXXXXX.py");
        src_fd = mkstemps(src_path, 3);
    } 
    else {
        return strdup("Unsupported language.\n");
    }

    if (src_fd < 0) return strdup("mkstemps failed.\n");

    FILE *src_file = fdopen(src_fd, "w");
    if (!src_file) { unlink(src_path); return strdup("fdopen failed.\n"); }
    fprintf(src_file, "%s", code);
    fclose(src_file);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        unlink(src_path);
        return strdup("pipe failed.\n");
    }

    pid_t pid = fork();
    if (pid < 0) {
        unlink(src_path);
        close(pipefd[0]); close(pipefd[1]);
        return strdup("fork failed.\n");
    }

    if (pid == 0) {
        
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

      
        struct rlimit rl;
        rl.rlim_cur = TIMEOUT_SEC; rl.rlim_max = TIMEOUT_SEC+1;
        setrlimit(RLIMIT_CPU, &rl);
        rl.rlim_cur = MAX_MEM_BYTES; rl.rlim_max = MAX_MEM_BYTES;
        setrlimit(RLIMIT_AS, &rl);

        if (strcmp(lang, "C") == 0) {
            char exe_path[256];
            snprintf(exe_path, sizeof(exe_path), "/tmp/exe_%d", getpid());

            char cmd[512];
            snprintf(cmd, sizeof(cmd), "gcc %s -o %s 2>&1 && %s",
                     src_path, exe_path, exe_path);

            execlp("bash", "bash", "-c", cmd, NULL);
            _exit(127);
        } 
        else {
            execlp("python3", "python3", src_path, NULL);
            _exit(127);
        }
    }

    
    close(pipefd[1]);
    child_pid = pid;
    signal(SIGALRM, alarm_handler);
    alarm(TIMEOUT_SEC + 1);

    char buffer[4096];
    size_t cap = 4096, len = 0;
    char *result = malloc(cap);
    result[0] = '\0';

    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        if (len + n + 1 > cap) {
            cap *= 2;
            result = realloc(result, cap);
        }
        memcpy(result + len, buffer, n);
        len += n;
        result[len] = '\0';
    }
    close(pipefd[0]);

    alarm(0);
    child_pid = 0;

    int status;
    waitpid(pid, &status, 0);

    if (len == 0) {
        if (WIFSIGNALED(status)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Process killed by signal %d\n",
                     WTERMSIG(status));
            free(result);
            return strdup(msg);
        }
        free(result);
        return strdup("Program returned no output.\n");
    }

    unlink(src_path);
    char exe_rm[64];
    snprintf(exe_rm, sizeof(exe_rm), "/tmp/exe_%d", pid);
    unlink(exe_rm);

    return result;
}

