#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/timerfd.h>

#define SIZE 640
#define CLIENTS_NO 700

typedef struct Client {
    int fd;
    int delivered_packages_no;
    int port;
    struct in_addr addr;
    int reservedBytes;
}Client;

static int requesting_clients = 0;
static int total_reserved_bytes = 0;

int all_clients = 0;
int createSocket(char* hostname, char* post);
void parseInput(int argc, char *const *argv, int c, float *production_speed, char **arg);
void parseHost(char* input, char* hostname, char* port);
void bytesFactory(char* tab, char ascii);
void asciiGenerator(char* tab);
void createChild(float speed, char* port, char* host);
void produceBytes(int descriptor, float speed);
int socket_comm(int descriptor, char* port, char* host);
int is_little_endian();
int createTimer(int clock_type);
void setTimer(float sec_interval, int fd);
void placeInPollTab(struct pollfd* pollfds, int fd);
void placeInClientsStruct(struct Client* clients, int fd, struct sockaddr_in client);
void deleteFromPollTab(struct pollfd* pollfds, int fd);
void deleteFromClientsStruct(struct Client* clients, int fd);
void generate_report(int desc, int pipe_begin, int pipe_end);
void timespec_diff(struct timespec *start, struct timespec *stop, int block_size, float speed);
void generate_disconnect_report(struct in_addr addr, int port, int wasted_bytes);
void deleteFromAllTabs(int y, struct pollfd *poll_fds, struct Client *cl_info);
void acceptNewClient(struct pollfd *poll_fds, struct Client *cl_info, struct sockaddr_in *client, socklen_t *cl_len);
void setupServer(int descriptor, int socket_desc, struct pollfd *poll_fds, struct sockaddr_in *client, socklen_t *cl_len, int *first_pipe_read);
void manageRecurringReport(int descriptor, int *pipe_end, struct pollfd *poll_fds, int first_pipe_read);
void manageClients(int descriptor, int y, struct pollfd *poll_fds, struct Client *cl_info);
void onClientDisconnect(int descriptor, int y, struct pollfd *poll_fds, struct Client *cl_info);

int main(int argc, char* argv[])
{
    float production_speed = 0;
    int c = 0;
    char *arg;
    parseInput(argc, argv, c, &production_speed, &arg);

    char hostname[100];
    char port[10];
    parseHost(arg, hostname, port);
    createChild(2662*production_speed, port, hostname);

    return 0;
}

void parseInput(int argc, char *const *argv, int c, float *production_speed, char **arg) {
    (*arg) = (char*)calloc(1, sizeof(argv[optind]));
    while ((c = getopt(argc, argv, "p:")) != -1)
    {
        switch (c)
        {
            case 'p':
            {
                char* pEnd = NULL;
                (*production_speed) = strtod(optarg, &pEnd); // unit: 2662B/s
                break;
            }
            default:
            {
                printf ("Usage:./%s -p <float> [<addr>:]port \n", argv[0]);
                exit(EXIT_FAILURE);
            }

        }
    }
    (*arg) = argv[optind];
}

int createSocket(char* hostname, char* port)
{
    int socket_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_ == -1) {
        printf("failed to create socket!\n");
        exit(1);
    }

    int enable = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        printf("Setsockopt(SO_REUSEADDR) failed!\n");

    struct sockaddr_in Address;
    Address.sin_family = AF_INET;
    char* pEnd;
    int system = is_little_endian();
    if(system) {
        Address.sin_port = strtoul(port, &pEnd, 10);
    }
    else Address.sin_port = htons(strtoul(port, &pEnd, 10));

    int r;
    if( inet_aton(hostname, &Address.sin_addr) == 0 )
        printf("Function inet_aton failed!\n ");

    if( (r = bind( socket_, (struct sockaddr*)&Address, sizeof(Address) ) ) == -1 )
        {
            printf("Failed to bind socket %d\n", socket_);
            exit(1);
        }

    if( (r = listen( socket_, CLIENTS_NO)) == -1) {
        printf("Changing to passive socket failed!\n");
	exit(1);
    }
    return socket_;
}

void bytesFactory(char* tab, char ascii)
{
    memset(tab, ascii, SIZE);
}

void asciiGenerator(char* tab)
{
    static char ascii = 65;
    bytesFactory(tab, ascii);
    ascii++;
    if(ascii == 91) ascii += 6;
    if(ascii == 123) ascii = 65;
}

void createChild(float speed,char* port, char* host)
{
    int fd[2];
    pipe(fd);
    int status;

    switch(fork()) {

        case -1: {
            printf("Error, can't create a child process!\n");
            break;
        }
        case 0: {
            close(fd[0]);
            produceBytes(fd[1], speed);
            break;
        }
        default: {
            close(fd[1]);
            socket_comm(fd[0], port, host);
            wait(&status);
            break;
        }
    }
}

void produceBytes(int descriptor, float speed)
{
    char bytes_array[SIZE];
    static int usage;
    struct timespec tm, tm2;
    long pipe_size = (long)fcntl(descriptor, F_GETPIPE_SZ);
    while(1) {
        if (usage < pipe_size) {
            clock_gettime(CLOCK_REALTIME, &tm);
            asciiGenerator(bytes_array);
            clock_gettime(CLOCK_REALTIME, &tm2);
            timespec_diff(&tm, &tm2, SIZE, speed);
            if (write(descriptor, bytes_array, sizeof(bytes_array)) == -1)
                printf("Write() error in bytes production!\n");
            else {
                usage += 640;
            }
        } else {
            while(1) {
                int occupiedBytes;
                ioctl(descriptor, FIONREAD, &occupiedBytes);
                int freeBytes = pipe_size - occupiedBytes;
                if(freeBytes >= 640) {
                    usage -= freeBytes;
                    break;
                }
            }
        }
    }
}

int socket_comm(int descriptor, char* port, char* host)
{
    int rc;
    int socket_desc;
    int y = 0;
    int pipe_end;
    struct pollfd* poll_fds = (struct pollfd*)calloc(CLIENTS_NO, sizeof(struct pollfd));
    struct Client* cl_info = (struct Client*)calloc(CLIENTS_NO, sizeof(struct Client));

    socket_desc = createSocket(host, port);
    if (socket_desc == -1) {
        printf("Could not create socket!\n");
        exit(1);
    }
    memset(poll_fds, 0 , sizeof(&poll_fds));
    struct sockaddr_in client;
    socklen_t cl_len;
    int first_pipe_read;
    setupServer(descriptor, socket_desc, poll_fds, &client, &cl_len, &first_pipe_read);

    while(1) {
        int avail;
        ioctl(descriptor, FIONREAD, &avail);
        //transmission can begin if we have enough resources in the warehouse
        if((avail - total_reserved_bytes) >= 13312) {
            rc = poll(poll_fds, CLIENTS_NO, -1);
            if (rc < 0) {
                printf("Calling poll function failed\n");
                break;
            }
            if (poll_fds[0].revents & POLLIN) {

                acceptNewClient(poll_fds, cl_info, &client, &cl_len);
            }
            if (poll_fds[1].revents & POLLIN) {
                manageRecurringReport(descriptor, &pipe_end, poll_fds, first_pipe_read);
            }
        }
        if(requesting_clients > 0){
            manageClients(descriptor, y, poll_fds, cl_info);
        }
    }
    return 1;
}

void manageClients(int descriptor, int y, struct pollfd *poll_fds, struct Client *cl_info)
{
    char message[1024] = {0};
    char check_buffer[10];
    y = 0;
    while (requesting_clients != y) {
        memset(message, '\0', sizeof(message));
        memset(check_buffer, '\0', sizeof(check_buffer));
        if (cl_info[y].fd == 0) break;
        while (1) {
            if (cl_info[y].reservedBytes == 0) {
                cl_info[y].reservedBytes = 13312;
                total_reserved_bytes += 13312;
            }

            if (recv(cl_info[y].fd, check_buffer, sizeof(check_buffer), MSG_PEEK | MSG_DONTWAIT) == 0) {
                onClientDisconnect(descriptor, y, poll_fds, cl_info);
                if (cl_info[y].fd == 0) break;
                //order is deleted
            }
            ssize_t count = read(descriptor, message, sizeof(message));
            if (count == -1) {
                printf("Error reading from pipe");
               return;
            }
            int x = write(cl_info[y].fd, message, sizeof(message));
            if (!x) printf("Write error\n");
            cl_info[y].reservedBytes -= x;
            cl_info[y].delivered_packages_no += x;
            total_reserved_bytes -= x;
            if (cl_info[y].delivered_packages_no >= 13312) {
                generate_disconnect_report(cl_info[y].addr, cl_info[y].port, 0);
                deleteFromAllTabs(y, poll_fds, cl_info);
                //clients were moved forward in the table, so y is not incremented
            } else {
                y++;
            }
            break;
        }
    }
}

void onClientDisconnect(int descriptor, int y, struct pollfd *poll_fds, struct Client *cl_info) {
    int wasted_bytes = 0;
    if (cl_info[y].delivered_packages_no > 0)
        wasted_bytes = (13312 - cl_info[y].delivered_packages_no);
    generate_disconnect_report(cl_info[y].addr, cl_info[y].port, wasted_bytes);
    deleteFromAllTabs(y, poll_fds, cl_info);
    total_reserved_bytes -= 13312;
    if (wasted_bytes > 0) {
        char *wasted = (char *) calloc(1, wasted_bytes + 1);
        memset(wasted, '\0', sizeof(wasted_bytes));
        read(descriptor, wasted, sizeof(wasted));
    }
}

void manageRecurringReport(int descriptor, int *pipe_end, struct pollfd *poll_fds, int first_pipe_read)
{
    int pipe_start;
    static int counter = 0;
    if (counter == 0) {
        pipe_start = first_pipe_read;
        counter++;
    } else pipe_start = (*pipe_end);
    ioctl(descriptor, FIONREAD, pipe_end);
    char buffer[8];
    read(poll_fds[1].fd, buffer, sizeof(buffer));
    generate_report(descriptor, pipe_start, (*pipe_end));
}

void setupServer(int descriptor, int socket_desc, struct pollfd *poll_fds, struct sockaddr_in *client, socklen_t *cl_len, int *first_pipe_read)
{
    (*cl_len) = sizeof(client);
    poll_fds[0].fd = socket_desc;
    poll_fds[0].events = POLLIN;

    int reportTimer = createTimer(CLOCK_REALTIME);
    poll_fds[1].fd = reportTimer;
    poll_fds[1].events = POLLIN;
    setTimer(5, poll_fds[1].fd);
    ioctl(descriptor, FIONREAD, first_pipe_read);
}

void acceptNewClient(struct pollfd *poll_fds, struct Client *cl_info, struct sockaddr_in *client, socklen_t *cl_len)
{
    int fd;
    if ((fd = accept4(poll_fds[0].fd, (struct sockaddr *) client, cl_len, SOCK_NONBLOCK)) == -1)
        printf("Error accepting new client");

    placeInPollTab(poll_fds, fd);

    placeInClientsStruct(cl_info, fd, (*client));


}

void deleteFromAllTabs(int y, struct pollfd *poll_fds,struct Client *cl_info)
{
    deleteFromPollTab(poll_fds, cl_info[y].fd);
    deleteFromClientsStruct(cl_info, cl_info[y].fd);
}

void parseHost(char* input, char* hostname, char* port)
{
    /*if(input[0] == ':')
    {
        strcpy(hostname, "127.0.0.1");
        memmove(port, input+1, strlen(input)+1);
    }*/
    if(strlen(input)<=6)
    {
        strcpy(hostname, "127.0.0.1");
        memmove(port, input, strlen(input)+1);
    }
    else {
        char* addr = strtok(input, ":");
        if(!strcmp("localhost", addr))
        {
            strcpy(hostname, "127.0.0.1");
        }
        else {
            strcpy(hostname, addr);
        }
        strcpy(port,strtok(NULL, ":"));
    }

}

void timespec_diff(struct timespec *start, struct timespec *stop, int block_size, float speed)
{
    struct timespec prod_time;
    struct timespec result;
    struct timespec final;
    long double to_elapse = (double)block_size/(double)speed;
    prod_time.tv_sec = (int)to_elapse;
    prod_time.tv_nsec = to_elapse * 1000000000;
    long double sec = stop->tv_nsec - start->tv_nsec;
    if( sec < 0 )
    {
        sec = (stop->tv_nsec + 1000000000) - start->tv_nsec;
        start->tv_sec--;
    }

    result.tv_sec = stop->tv_sec-start->tv_sec;
    result.tv_nsec = sec;

    if(result.tv_sec > prod_time.tv_sec)
    {
        return;
    }

    long double sec2 = prod_time.tv_nsec - result.tv_nsec;
    if( sec2 < 0 )
    {
        sec2 = (prod_time.tv_nsec + 1000000000) - result.tv_nsec;
        result.tv_sec--;
    }

    final.tv_sec = prod_time.tv_sec - result.tv_sec;
    final.tv_nsec = sec2;
    nanosleep(&final, &result);
}

int is_little_endian() {
    short x = 0x0100; //256
    char *p = (char*) &x;
    if (p[0] == 0) {
        return 1;
    }
    return 0;
}

void placeInPollTab(struct pollfd* pollfds, int fd)
{
    int i = 3;
    struct pollfd to_insert;
    to_insert.fd = fd;
    to_insert.events = POLLIN;
    while(pollfds[i].fd != 0) i++;
    pollfds[i] = to_insert;
    all_clients++;
    requesting_clients++;
}

void placeInClientsStruct(struct Client* clients, int fd, struct sockaddr_in client)
{
    int i = 0;
    while(clients[i].fd != 0) i++;
    clients[i].fd = fd;
    clients[i].addr = client.sin_addr;
    clients[i].port = client.sin_port;
    clients[i].delivered_packages_no = 0;
    clients[i].reservedBytes = 0;

}

int createTimer(int clock_type)
{
    int fd = timerfd_create( clock_type, TFD_NONBLOCK);
    if( fd == -1 )
        printf("Error creating timer\n");
    return fd;
}

void setTimer(float sec_interval, int fd)
{
    struct itimerspec timer;

    timer.it_interval.tv_sec = (int)sec_interval;
    timer.it_interval.tv_nsec = (sec_interval - (int)sec_interval) * 1000000000;

    timer.it_value.tv_sec = (int)sec_interval;
    timer.it_value.tv_nsec = (sec_interval - (int)sec_interval) * 1000000000;

    int res;
    if( (res = timerfd_settime( fd, 0, &timer, NULL)) == -1)
        printf("Error setting timer\n");
}

void deleteFromPollTab(struct pollfd* pollfds, int fd)
{
    int i = 3;
    while(pollfds[i].fd != fd) i++;
    pollfds[i].fd = 0;
    pollfds[i].events = -1;
    for(int j=i+1; j<CLIENTS_NO-1; j++)
    {
        pollfds[j-1].fd = pollfds[j].fd;
        pollfds[j-1].events = pollfds[j].events;
        pollfds[j-1].revents = pollfds[j].revents;
    }
    all_clients--;
    requesting_clients--;
}

void deleteFromClientsStruct(struct Client* clients, int fd)
{
    int i = 0;
    while(clients[i].fd != fd) i++;
    for(int j=i+1; j<CLIENTS_NO-1; j++)
    {
        clients[j-1].fd = clients[j].fd;
        clients[j-1].addr = clients[j].addr;
        clients[j-1].port = clients[j].port;
        clients[j-1].delivered_packages_no = clients[j].delivered_packages_no;
        clients[j-1].reservedBytes = clients[j].reservedBytes;
    }
}

void generate_report(int desc, int pipe_begin, int pipe_end)
{
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    int bytes_in_warehouse;
    ioctl(desc, FIONREAD, &bytes_in_warehouse);
    long pipe_size = (long)fcntl(desc, F_GETPIPE_SZ);
    double percent = ((double)bytes_in_warehouse/pipe_size)*100;
    int data_flow = pipe_end - pipe_begin;
    fprintf(stderr, "TS: %ld seconds, %ld nanoseconds\ncurrently connected clients: %d\nwarehouse usage: %d B %f%%\ndata flow: %d\n\n", time.tv_sec, time.tv_nsec, all_clients, bytes_in_warehouse, percent, data_flow);
    
}

void generate_disconnect_report(struct in_addr addr, int port, int wasted_bytes)
{
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    char saddr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&addr.s_addr, saddr, INET_ADDRSTRLEN);
    fprintf(stderr, "TS: seconds:%ld, nanoseconds: %ld\ndisconnected client:%s:%d\nwasted bytes: %d\n\n",time.tv_sec, time.tv_nsec, saddr, port, wasted_bytes);
}

// W trakcie tworzenia programu korzystałam z następujących źródeł zewnętrznych:

// funkcja little_endian()
// stackoverflow.com/questions/1024951/does-my-amd-based-machine-use-little-endian-or-big-endian?fbclid=IwAR3sVn7AAWyiYLWleuKTpmgFrpm1jcHo7g2rKHTDQRq7pYRiU0B_1qbEsqY

// szkielet serwera inspirowany był serwerem zamieszczonym na stronie
// https://www.ibm.com/support/knowledgecenter/ssw_ibm_i_71/rzab6/poll.htm

// przykladowe parametry, dla ktorych program dziala:
// ./producent -p 3 127.0.0.1:8888
// ./producent -p 1 127.0.0.1:8888
