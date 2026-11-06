#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 4242
#define DEFAULT_HOST "127.0.0.1"

int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_in servaddr;
    const char *host;
    int port;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s \"COMMAND\"\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Read environment variables for host and port
    host = getenv("IRRIGATION_HOST");
    if (!host) host = DEFAULT_HOST;

    port = getenv("IRRIGATION_PORT") ? atoi(getenv("IRRIGATION_PORT")) : DEFAULT_PORT;

    // Create socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &servaddr.sin_addr) <= 0) {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    // Send the command
    if (send(sockfd, argv[1], strlen(argv[1]), 0) < 0) {
        perror("send");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Optionally wait for response
    char buf[256];
    ssize_t n = recv(sockfd, buf, sizeof(buf)-1, 0);
    if (n > 0) {
        buf[n] = '\0';
        printf("Response: %s\n", buf);
    }

    close(sockfd);
    return 0;
}

