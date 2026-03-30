#define _POSIX_C_SOURCE 200809L
#include "server.h"


void *client_thread(void *arg) {
int client_fd = *(int *)arg; free(arg);
char lang[32] = {0}; char code[BUFFER_SIZE] = {0};


ssize_t n = recv(client_fd, lang, sizeof(lang)-1, 0);
if (n <= 0) { close(client_fd); return NULL; }
lang[n] = '\0';


n = recv(client_fd, code, sizeof(code)-1, 0);
if (n < 0) n = 0; code[n] = '\0';


log_event("Received request (fd=%d) lang=%s size=%zd", client_fd, lang, strlen(code));


char *out = execute_code(lang, code);
if (!out) out = strdup("Internal error\n");


send(client_fd, out, strlen(out), 0);
free(out);
close(client_fd);
log_event("Closed connection fd=%d", client_fd);
return NULL;
}


void sigchld_handler(int sig) { (void)sig; while (waitpid(-1, NULL, WNOHANG) > 0) {} }


int main() {
mkdir("logs", 0755);
signal(SIGCHLD, sigchld_handler);


int server_fd = socket(AF_INET, SOCK_STREAM, 0);
if (server_fd < 0) { perror("socket"); return 1; }
int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));


struct sockaddr_in serv;
serv.sin_family = AF_INET; serv.sin_addr.s_addr = INADDR_ANY; serv.sin_port = htons(PORT);


if (bind(server_fd, (struct sockaddr*)&serv, sizeof(serv)) < 0) { perror("bind"); return 1; }
if (listen(server_fd, 16) < 0) { perror("listen"); return 1; }


printf("[Server] Listening on port %d\n", PORT);
log_event("Server started on port %d", PORT);


while (1) {
struct sockaddr_in cli; socklen_t clen = sizeof(cli);
int *client_fd = malloc(sizeof(int));
*client_fd = accept(server_fd, (struct sockaddr*)&cli, &clen);
if (*client_fd < 0) { perror("accept"); free(client_fd); continue; }
char ipstr[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof(ipstr));
log_event("Accepted connection from %s:%d fd=%d", ipstr, ntohs(cli.sin_port), *client_fd);
pthread_t tid; pthread_create(&tid, NULL, client_thread, client_fd); pthread_detach(tid);
}
close(server_fd);
return 0;
}
