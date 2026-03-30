#include "server.h"


int main(int argc, char **argv) {
char server_ip[64] = "172.20.10.4";
if (argc >= 2) strncpy(server_ip, argv[1], sizeof(server_ip)-1);


int sock = socket(AF_INET, SOCK_STREAM, 0);
if (sock < 0) { perror("socket"); return 1; }


struct sockaddr_in serv; serv.sin_family = AF_INET; serv.sin_port = htons(PORT);
inet_pton(AF_INET, server_ip, &serv.sin_addr);


if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) { perror("connect"); return 1; }


char lang[32]; printf("Enter Language (C/Python): "); if (!fgets(lang, sizeof(lang), stdin)) return 0; lang[strcspn(lang, "\r\n")] = '\0';
send(sock, lang, strlen(lang), 0);


printf("Enter code. End by entering an empty line:\n");
char code[BUFFER_SIZE]; code[0] = '\0'; char line[1024];
while (fgets(line, sizeof(line), stdin)) {
if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) break;
if (strlen(code) + strlen(line) < sizeof(code)-1) strcat(code, line);
else break;
}
send(sock, code, strlen(code), 0);


char out[BUFFER_SIZE]; ssize_t n = recv(sock, out, sizeof(out)-1, 0);
if (n > 0) { out[n] = '\0'; printf("\n--- Execution Output ---\n%s\n--- End ---\n", out); }
else printf("No output or error\n");
close(sock);
return 0;
}
