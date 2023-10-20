#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <rte_mbuf.h>

#include "ff_config.h"
#include "ff_api.h"
#include "ff_veth.h"

#define MAX_EVENTS 512

/* kevent set */
struct kevent kevSet;
/* events */
struct kevent events[MAX_EVENTS];
/* kq */
int kq;
int sockfd;
#ifdef INET6
int sockfd6;
#endif

char *udp_message = "Hello, f-stack UDP server";
void printf_msg(char *input, ssize_t readlen){
    printf("Printing message: \n");
    for(int i = 0 ; i < readlen; i++){
        printf("%02x ", (unsigned char)input[i]);
    }
    printf("\n");
}
int loop(void *arg)
{
    /* Wait for events to happen */
    int nevents = ff_kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);
    int i;

    if (nevents < 0) {
        printf("ff_kevent failed:%d, %s\n", errno,
                        strerror(errno));
        return -1;
    }

    for (i = 0; i < nevents; ++i) {
        struct kevent event = events[i];
        int clientfd = (int)event.ident;

        if (event.filter == EVFILT_READ) {
            void *mb;
            struct sockaddr_in recvaddress;
            socklen_t addr_len = sizeof(recvaddress);
            ssize_t readlen = ff_recvfrom(clientfd, &mb, 256, 0, (struct linux_sockaddr *) &recvaddress, &addr_len);

            struct rte_mbuf *rte_mb = ff_rte_frm_extcl(mb);
            ff_mbuf_detach_rte(mb);
            ff_mbuf_free(mb);

            rte_pktmbuf_reset(rte_mb);
            char *data = rte_pktmbuf_mtod(rte_mb, char *);

            struct sockaddr_in sendaddress;
            bzero(&sendaddress, sizeof(sendaddress));
            sendaddress.sin_family = AF_INET;
            sendaddress.sin_port = htons(5005);
            sendaddress.sin_addr.s_addr = inet_addr("10.10.1.1");

            memcpy(data, udp_message, strlen(udp_message));
            rte_mb->data_len = strlen(udp_message);
            rte_mb->pkt_len = rte_mb->data_len;
            mb = ff_mbuf_get(NULL, rte_mb, data, rte_mb->data_len);
            ssize_t writelen = ff_sendto(clientfd, mb, rte_mb->data_len, 0,
                                        (struct linux_sockaddr *) &sendaddress, sizeof(sendaddress));
            if (writelen < 0){
                printf("ff_write failed:%d, %s\n", errno,
                    strerror(errno));
                ff_close(clientfd);
            }    
        } else {
            printf("unknown event: %8.8X\n", event.flags);
        }
    }

    return 0;
}

int main(int argc, char * argv[])
{
    ff_init(argc, argv);

    kq = ff_kqueue();
    if (kq < 0) {
        printf("ff_kqueue failed, errno:%d, %s\n", errno, strerror(errno));
        exit(1);
    }

    sockfd = ff_socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        printf("ff_socket failed, sockfd:%d, errno:%d, %s\n", sockfd, errno, strerror(errno));
        exit(1);
    }

    /* Set non blocking */
    int on = 1;
    ff_ioctl(sockfd, FIONBIO, &on);

    struct sockaddr_in my_addr;
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(80);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int ret = ff_bind(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
    if (ret < 0) {
        printf("ff_bind failed, sockfd:%d, errno:%d, %s\n", sockfd, errno, strerror(errno));
        exit(1);
    }

    EV_SET(&kevSet, sockfd, EVFILT_READ, EV_ADD, 0, MAX_EVENTS, NULL);
    /* Update kqueue */
    ff_kevent(kq, &kevSet, 1, NULL, 0, NULL);

#ifdef INET6
    sockfd6 = ff_socket(AF_INET6, SOCK_STREAM, 0);
    if (sockfd6 < 0) {
        printf("ff_socket failed, sockfd6:%d, errno:%d, %s\n", sockfd6, errno, strerror(errno));
        exit(1);
    }

    struct sockaddr_in6 my_addr6;
    bzero(&my_addr6, sizeof(my_addr6));
    my_addr6.sin6_family = AF_INET6;
    my_addr6.sin6_port = htons(80);
    my_addr6.sin6_addr = in6addr_any;

    ret = ff_bind(sockfd6, (struct linux_sockaddr *)&my_addr6, sizeof(my_addr6));
    if (ret < 0) {
        printf("ff_bind failed, sockfd6:%d, errno:%d, %s\n", sockfd6, errno, strerror(errno));
        exit(1);
    }

    ret = ff_listen(sockfd6, MAX_EVENTS);
    if (ret < 0) {
        printf("ff_listen failed, sockfd6:%d, errno:%d, %s\n", sockfd6, errno, strerror(errno));
        exit(1);
    }

    EV_SET(&kevSet, sockfd6, EVFILT_READ, EV_ADD, 0, MAX_EVENTS, NULL);
    ret = ff_kevent(kq, &kevSet, 1, NULL, 0, NULL);
    if (ret < 0) {
        printf("ff_kevent failed:%d, %s\n", errno, strerror(errno));
        exit(1);
    }
#endif

    ff_run(loop, NULL);
    return 0;
}
