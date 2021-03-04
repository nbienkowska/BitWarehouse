#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/timerfd.h>

typedef struct Report {
    struct in_addr addr;
    int port;
    int pid;
    struct timespec connection_time;
    struct timespec first_portion;
    struct timespec closing_time;
}Report;

void parseHost(char* input, char* hostname, char* port);
int createSocket();
int connectSocket(int socket_, int port, char* host);
int receiveData(int socket_,char* Rsp,int RvcSize);
int socket_comm(int port, float consumption_speed, long warehouse_capacity, char* host, int degradation_speed);
void timespec_diff(struct timespec *start, struct timespec *stop, int msg_size, float consumption_speed);
int is_little_endian();
int createTimer(int clock_type);
void setTimer(float sec_interval, int fd);
void parseArgs(int argc, char *const *argv, float *consumption_speed, float *degradation_speed, long *capacity, char **arg);
void final_report(int counter, struct Report* report_info);
void timespec_diff_2(struct timespec start, struct timespec stop, struct timespec *result);
void manageTimer(int degradation_speed, int* usage, const struct pollfd *poll_fds);
int setupSocket(int port, char *host, int socket_);
void setupTimespec(struct Report *report_info, int counter, int flag);
void initialStructSetup(int socket_, struct Report *report_info, int counter);
int readAndMeasureTime(float consumption_speed, int socket_, struct timespec *tm, struct timespec *tm2);

void cleanUp(struct Report *report_info, int *socket_, int *counter);

int main(int argc, char* argv[])
{
    float consumption_speed;
    float degradation_speed;
    long capacity;
    char *arg;
    parseArgs(argc, argv, &consumption_speed, &degradation_speed, &capacity, &arg);

    char hostname[100];
    char port[10];
    parseHost(arg, hostname, port);
    char* pEnd;
    socket_comm(strtoul(port, &pEnd, 10), consumption_speed * 4435, 30*1024*capacity, hostname, 819 * degradation_speed);

    return 0;
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

int createSocket()
{
    int socket_;
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_ == -1)
    {
        printf("Error creating socket!\n");
        exit(1);
    }
    return socket_;
}

int connectSocket(int socket_, int port, char* host)
{
    int value;
    struct sockaddr_in remote={0};
    remote.sin_family = AF_INET;
    int system = is_little_endian();
    if(system) {
        remote.sin_port = port;
    }
    else remote.sin_port = htons(port);
    int address = inet_aton(host,&remote.sin_addr);
    if(!address) {
        printf("Incorrect address: %s\n",host);
        exit(1);
    }
    value = connect(socket_,(struct sockaddr *)&remote,sizeof(struct sockaddr_in));
    return value;
}

int receiveData(int socket_,char* Rsp,int RvcSize)
{
    int count;
    count = read(socket_, Rsp, RvcSize);
    if(count == -1) {
        printf("Error reading from pipe!\n");
        exit(1);
    }
    return count;
}

int socket_comm(int port, float consumption_speed, long warehouse_capacity, char* host, int degradation_speed) {

    int full_flag = 0;
    int socket_, read_size;
    struct timespec tm, tm2;
    int usage = 0;
    int currently_downloaded = 0;
    struct pollfd poll_fds[2];
    int x;

    //minimum warehouse size is 30 KiB, so there will always be at least two 13 KiB blocks
    struct Report* report_info = (struct Report*)calloc(2, sizeof(struct Report));

    memset(poll_fds, '\0' , sizeof(poll_fds));
    int degradationTimer = createTimer(CLOCK_REALTIME);
    poll_fds[0].fd = degradationTimer;
    poll_fds[0].events = POLLIN;

    setTimer(1, poll_fds[0].fd);
    int counter = 0;
    while(1) {

        if(counter >= 2) {
            report_info = realloc(report_info, (counter + 1) * sizeof(struct Report));
            if (report_info == NULL) {
                printf("Error reallocating memory");
                return -1;
            }
        }
        socket_ = setupSocket(port, host, socket_);
        setupTimespec(report_info, counter, 1);
        initialStructSetup(socket_, report_info, counter);
        x = poll(poll_fds, 2, -1);
        if (x < 0) {
            printf("Function poll failed!\n");
            break;
        }

        while (1) {
        if (poll_fds[0].revents & POLLIN) {
            manageTimer(degradation_speed, &usage, poll_fds);
        }
            read_size = readAndMeasureTime(consumption_speed, socket_, &tm, &tm2);
            if(currently_downloaded == 0) {
                setupTimespec(report_info, counter, 2);
            }
            usage += read_size;
            currently_downloaded += read_size;
            if (currently_downloaded >= 13312) {
                currently_downloaded = 0;
                if (warehouse_capacity - usage < 13312) {
                    full_flag = 1;
                }
                break;
            }
        }
        cleanUp(report_info, &socket_, &counter);
        if (full_flag == 1) {
            final_report(counter, report_info);
            break;
        }
    }
    free(report_info);
    return 0;
}

void cleanUp(struct Report *report_info, int *socket_, int *counter) {
    close((*socket_));
    setupTimespec(report_info, (*counter), 3);
    (*socket_) = -1;
    (*counter)++;
}

int readAndMeasureTime(float consumption_speed, int socket_,  struct timespec *tm, struct timespec *tm2)
{
    char server_reply[1024] = {0};
    int read_size;
    clock_gettime(CLOCK_REALTIME, tm);
    read_size = receiveData(socket_, server_reply, sizeof(server_reply));
    clock_gettime(CLOCK_REALTIME, tm2);
    timespec_diff(tm, tm2, sizeof(server_reply), consumption_speed);
    return read_size;
}

void initialStructSetup(int socket_, struct Report *report_info, int counter) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getsockname(socket_, &addr, &addrlen);
    report_info[counter].addr = addr.sin_addr;
    report_info[counter].port = addr.sin_port;
    report_info[counter].pid = getpid();
}

void setupTimespec(struct Report *report_info, int counter, int flag) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    if(flag == 1)
    report_info[counter].connection_time = time;
    if(flag == 2)
        report_info[counter].first_portion = time;
    if(flag == 3)
        report_info[counter].closing_time = time;
}

int setupSocket(int port, char *host, int socket_)
{
    socket_ = createSocket();
    if (socket_ == -1) {
        printf("Could not create socket\n");
        exit(1);
    }

    if (connectSocket(socket_, port, host) < 0) {
        printf("Connect failed.\n");
        exit(1);
    }

    return socket_;
}

void manageTimer(int degradation_speed, int* usage, const struct pollfd *poll_fds)
{
    if (*usage >= degradation_speed)
        *usage -= degradation_speed;
    else *usage = 0;
    char buffer[8];
    read(poll_fds[0].fd, buffer, sizeof(buffer));
    //return usage;
}

void timespec_diff(struct timespec *start, struct timespec *stop, int msg_size, float consumption_speed)
{
    struct timespec prod_time;
    struct timespec result;
    struct timespec final;
    long double to_elapse = (double)msg_size/(double)consumption_speed;
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

void timespec_diff_2(struct timespec start, struct timespec stop, struct timespec *result) {
    long sec = stop.tv_nsec - start.tv_nsec;
    if (sec < 0) {
        sec = (stop.tv_nsec + 1000000000) - start.tv_nsec;
        start.tv_sec--;
    }

    result->tv_sec = stop.tv_sec - start.tv_sec;
    result->tv_nsec = sec;
}

void final_report(int counter, struct Report* report_info)
{
    struct timespec tm;
    clock_gettime(CLOCK_REALTIME, &tm);
    fprintf(stderr, "TS: %ld seconds, %ld nanoseconds\n", tm.tv_sec, tm.tv_nsec);
    for(int i=0; i < counter; i++)
    {
        int system = is_little_endian();
        if(!system) {
            report_info[i].port = htons(report_info[i].port);
        }
        struct timespec delay1, delay2;
        timespec_diff_2(report_info[i].connection_time, report_info[i].first_portion, &delay1);
        timespec_diff_2(report_info[i].first_portion, report_info[i].closing_time, &delay2);
        char saddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &report_info[i].addr.s_addr, saddr, INET_ADDRSTRLEN);
        fprintf(stderr, "PID: %d\naddress: %s:%d\nconnection to first portion delay: %ld seconds, %ld nanoseconds\nfirst portion to closing connection delay: %ld seconds, %ld nanoseconds\n", report_info[i].pid,saddr, report_info[i].port, delay1.tv_sec, delay1.tv_nsec, delay2.tv_sec, delay2.tv_nsec);
    }

}

int is_little_endian() {
    short x = 0x0100; //256
    char *p = (char*) &x;
    if (p[0] == 0) {
        return 1;
    }
    return 0;
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

void parseArgs(int argc, char *const *argv, float *consumption_speed, float *degradation_speed, long *capacity, char **arg)
{
    (*arg) = (char*)calloc(1, sizeof(argv[optind]));
    int c;
    while ((c = getopt(argc, argv, "p:c:d:")) != -1)
    {
        switch (c)
        {
            case 'p':
            {
                char* pEnd;
                (*consumption_speed) = strtod(optarg, &pEnd); // jednostka: 4435B/s
                break;
            }
            case 'c':
            {
                char* pEnd;
                (*capacity) = strtoul(optarg, &pEnd, 10); // jednostka: 30 KiB
                break;
            }
            case 'd':
            {
                char* pEnd;
                (*degradation_speed) = strtod(optarg, &pEnd); // jednostka: 819B/s
                break;
            }
            default:
            {
                printf("Usage:./%s -c <int> -p <float> -f <float> [<addr>:]port \n", argv[0]);
                exit(EXIT_FAILURE);
            }

        }
    }
    (*arg) = argv[optind];
}

// While creating the programme I used the following sources:

// little_endian() function:
// stackoverflow.com/questions/1024951/does-my-amd-based-machine-use-little-endian-or-big-endian?fbclid=IwAR3sVn7AAWyiYLWleuKTpmgFrpm1jcHo7g2rKHTDQRq7pYRiU0B_1qbEsqY
