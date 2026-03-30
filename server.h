#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>   
#include <stddef.h>     

#define PORT 9000
#define BUFFER_SIZE 8192

extern pthread_mutex_t log_mutex;

char *execute_code(const char *lang, const char *code);
void log_event(const char *fmt, ...);

#endif

