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
#include <netinet/sctp.h>
#include <time.h>


#include "../lib/ff_config.h"
#include "../lib/ff_api.h"
#include "../lib/ff_epoll.h"

#define MAX_EVENTS 512
#define DEFAULT_CONN 512
#define MESSSAGE_SIZE 8192

int no_conn = 4; 
unsigned message_size = 64; 

char output_buffer[MESSSAGE_SIZE]; 
char input_buffer[MESSSAGE_SIZE]; 

/* epoll */
struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];
int epfd;
#ifdef INET6
int sockfd6;
#endif

long total_bytes_sent = 0 ; 
long total_bytes_received = 0 ; 
struct timespec start_time, current_time;
unsigned duration = 10 ;

int loop(void *arg)
{
    int *sockfd = (int *)arg;  
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    if (current_time.tv_sec - start_time.tv_sec > duration) {
        double throughput = ((double)(total_bytes_sent + total_bytes_received) / duration) / 1024;
        double througput_recv = ((double) total_bytes_received / duration) / 1024 ; 
        printf( "Total data transfer : %ld KBytes\n", ((total_bytes_received + total_bytes_sent)/ 1024));
        printf("Total KBytes sent : %ld\n", (total_bytes_sent/1024)); 
        printf("Total KBytes recv : %ld\n", (total_bytes_received/1024)); 
        printf("Througput_recv : %f Kb/sec", througput_recv); 
        printf("Throughput: %f Kb/sec\n", throughput);

        for (int i = 0; i < no_conn; i++) {
            ff_close(sockfd[i]);
        }
        ff_close(epfd);        
        exit(EXIT_SUCCESS); 
    }

    /* Wait for events to happen */
    int nevents = ff_epoll_wait(epfd,  events, MAX_EVENTS, 0);
    int i;
    
    if (nevents < 0) {
        printf("ff_kevent failed:%d, %s\n", errno, strerror(errno));
        return -1;
    }

    for (i = 0; i < nevents; ++i) {
        int clientfd = events[i].data.fd;
        if ( events[i].events & EPOLLERR ){
            ff_epoll_ctl(epfd, EPOLL_CTL_DEL,  events[i].data.fd, NULL);
            ff_close(events[i].data.fd);

        }else if ( events[i].events & EPOLLOUT ){
            /* check the socket option */
            int error = 0 ; 
            socklen_t errlen = sizeof(error); 
            if ( ff_getsockopt(clientfd, SOL_SOCKET, SO_ERROR, &error, &errlen) == -1 ){
                // ff_close(clientfd); 
                continue;
            }
            if (error != 0 ){
                // fprintf(stderr, "Connection failed: %s\n", strerror(error));
                ff_close(clientfd);
                continue;                
            }
            size_t sendlen = ff_write(clientfd, output_buffer, message_size);
             if (sendlen == -1 ){
                // perror("send failed"); 
                continue;
            }else if (sendlen== 0){
                ff_close(clientfd);
                continue;
            }            

            total_bytes_sent += sendlen;
            ev.data.fd = clientfd;
            ev.events  = EPOLLIN;
            ff_epoll_ctl(epfd, EPOLL_CTL_MOD,  clientfd, &ev);

        }else if (events[i].events & EPOLLIN){

            ssize_t readlen = ff_read(clientfd, input_buffer, sizeof(input_buffer));
            if (readlen== -1) {
                continue;
            } else if (readlen == 0) {
                continue;
            }
            total_bytes_received += readlen;

            ev.data.fd = clientfd;
            ev.events  = EPOLLOUT;
            ff_epoll_ctl(epfd, EPOLL_CTL_MOD,  clientfd, &ev);
        }

    }

    return 0;
}

int main(int argc, char * argv[])
{

    ff_init(argc, argv);

    printf("argc %d\n", argc);

    if ( argc > 5){
        no_conn = atoi(argv[5]);
        message_size = atoi(argv[6]);
    }

    printf("no_conn: %d, message_size: %d\n", no_conn, message_size); 

    epfd = ff_epoll_create(0);
    if (epfd < 0) {
        printf("ff_kqueue failed, errno:%d, %s\n", errno, strerror(errno));
        exit(1);
    }

    /* Create a message buffer */
    bzero(output_buffer, message_size);

    int sockfd[no_conn];
    uint16_t port = 4000;

    for ( int i = 0 ; i < no_conn ; i ++ ){
        sockfd[i] = ff_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
        if (sockfd[i] < 0) {
            printf("ff_socket failed, sockfd:%d, errno:%d, %s\n", sockfd[i], errno, strerror(errno));
            exit(1);
        }

        struct sockaddr_in host_addr; 
        bzero(&host_addr, sizeof(host_addr)); 
        host_addr.sin_family = AF_INET; 
        host_addr.sin_port = htons(port); 
        inet_pton(AF_INET, "10.10.1.1", &host_addr.sin_addr);

        if (ff_bind(sockfd[i], (struct linux_sockaddr *)&host_addr, sizeof(struct sockaddr_in))){
            printf("ff_bind failed\n"); 
            exit(1); 
        }; 
        /* Set non blocking */
        int on = 1;
        ff_ioctl(sockfd[i], FIONBIO, &on);

        struct sctp_initmsg initmsg = {
                .sinit_num_ostreams = DEFAULT_CONN,
                .sinit_max_instreams = DEFAULT_CONN,
                .sinit_max_attempts = 3,
        };    

        struct sockaddr_in my_addr;
        bzero(&my_addr, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(3000);
        inet_pton(AF_INET, "10.10.1.2", &my_addr.sin_addr); 

        int ret = ff_setsockopt(sockfd[i], IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof(initmsg));
        if (ret < 0) {
            printf("ff_setsockopt failed, sockfd:%d, errno:%d, %s\n", sockfd[i], errno, strerror(errno));
            exit(1);
        }

        /* connect to server*/
        ret = ff_connect(sockfd[i], (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
        if (ret == -1 && errno != EINPROGRESS){
            printf("ff_connect failed, sockfd6:%d, errno:%d, %s\n", sockfd[i], errno, strerror(errno));
            exit(1); 
        }

        // int error = 0 ; 
        // socklen_t errlen = sizeof(error); 
        // if ( ff_getsockopt(sockfd[i], SOL_SOCKET, SO_ERROR, &error, &errlen) == -1 ){
        //     printf("ff_getsockopt \n");
        //     ff_close(sockfd[i]); 
        //     continue;
        // }
        // if (error != 0 ){
        //     fprintf(stderr, "Connection failed: %s\n", strerror(error));
        //     ff_close(sockfd[i]);
        //     continue;                
        //}
        port += 1;

        /* add to epoll */
        ev.data.fd = sockfd[i];
        ev.events = EPOLLIN | EPOLLOUT;
        ff_epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd[i], &ev);

    }

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    ff_run(loop, sockfd);

    return 0;
}
