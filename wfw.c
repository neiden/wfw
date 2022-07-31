//
// Created by Pinkgodzilla on 4/24/2020.
//

#include "conf.h"
#include "hash.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet6/in6.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <w32api/d3dvec.inl>
#include <assert.h>

/* Constants */
#define STR1(x)   #x
#define STR(x)    STR1(x)
#define DEVICE    "device"
#define PORT      "port"
#define BROADCAST "broadcast"
#define ANYIF     "0.0.0.0"
#define ANYPORT   "0"
#define PID	  "pidfile"


/* Globals  */
static char* conffile   = STR(SYSCONFDIR) "/wfw.cfg";
static bool  printusage = false;
static bool foreground = false;

/* Prototypes */

/* Parse Options
 * argc, argv   The command line
 * returns      true iff the command line is successfully parsed
 *
 * This function sets the otherwise immutable global variables (above).
 */
static
bool parseoptions(int argc, char* argv[]);

/* Usage
 * cmd   The name by which this program was invoked
 * file  The steam to which the usage statement is printed
 *
 * This function prints the simple usage statement.  This is typically invoked
 * if the user provides -h on the command line or the options don't parse.
 */
static
void usage(char* cmd, FILE* file);

/* Ensure Tap
 * path     The full path to the tap device.
 * returns  If this function returns, it is the file descriptor for the tap
 *          device.
 *
 * This function tires to open the specified device for reading and writing.  If
 * that open fails, this function will report the error to stderr and exit the
 * program.
 */
static
int  ensuretap(char* path);

/* Ensure Socket
 * localaddress   The IPv4 address to bind this socket to.
 * port           The port number to bind this socket to.
 *
 * This function creates a bound socket.  Notice that both the local address and
 * the port number are strings.
 */
static
int ensuresocket(char* localaddr, char* port);

/* Verify if the given source address is a broadcast address.
 *
 * Returns 1 if yes, 0 if no.
 */
static
int isSpecialMAC(uint8_t src);

/* Make Socket Address
 * address, port  The string representation of an IPv4 socket address.
 *
 * This is a convince routine to convert an address-port pair to an IPv4 socket
 * address.
 */
static
struct sockaddr_in makesockaddr(char* address, char* port);

/* mkfdset
 * set    The fd_set to populate
 * ...    A list of file descriptors terminated with a zero.
 *
 * This function will clear the fd_set then populate it with the specified file
 * descriptors.
 */
static
int mkfdset(fd_set* set, ...);

/* Bridge
 * tap     The local tap device
 * in      The network socket that receives broadcast packets.
 * out     The network socket on with to send broadcast packets.
 * bcaddr  The broadcast address for the virtual ethernet link.
 *
 * This is the main loop for wfw.  Data from the tap is broadcast on the
 * socket.  Data broadcast on the socket is written to the tap.
 */
static
void bridge(int tap, int in, int out, struct sockaddr_in bcaddr);
/*daemonize
 *
 *
 *Makes process a background daemon process
*/
static
void daemonize(hashtable conf);


/* Network to String
 * sa       reference to a generic sockaddr
 * returns  statically allocated string containing the presentation format of
 *          the sockaddr's address.
 * Note that the return string is statically allocated.  This function is not
 * thread-safe and the returned pointer shall not be freed.
 */
static
const char* ntos(struct sockaddr* sa);


/* Connect To the specified host and service
 * host     The host name or address to connect to.
 * svc      The service name or service to connect to.
 * returns  -1 or a connected socket.
 *
 * Note a non-negative return is a newly created socket that shall be closed.
 */
static
int connectto(const char* name, const char* svc);




static maccmp(void* l, void* r) ;
/* Main
 *
 * Mostly, main parses the command line, the conf file, creates the necessary
 * structures and then calls bridge.  Bridge is where the real work is done.
 */
int main(int argc, char* argv[]) {
    int result = EXIT_SUCCESS;

    if(!parseoptions(argc, argv)) {
        usage(argv[0], stderr);
        result = EXIT_FAILURE;
    }
    else if(printusage) {
        usage(argv[0], stdout);
    }
    else {
        hashtable conf = readconf (conffile);
        int       tap  = ensuretap (htstrfind (conf, DEVICE));
        int       out  = ensuresocket(ANYIF, ANYPORT);
        int       in   = ensuresocket(htstrfind (conf, BROADCAST),
                                      htstrfind (conf, PORT));
        struct sockaddr_in
                bcaddr       = makesockaddr (htstrfind (conf,BROADCAST),
                                             htstrfind (conf, PORT));
        if(!foreground)
            daemonize(conf);

        bridge(tap, in, out, bcaddr);

        close(in);
        close(out);
        close(tap);
        htfree(conf);
    }

    return result;
}

/* Network to String
 *
 * The buffer is of length 40, which is the maximum length of an IPv6 address---
 * eight groups of four hex digits separated by colon plus a terminating null.
 * The final assert ensures that the buffer has not be overwritten.
 */
static
const char* ntos(struct sockaddr* sa) {
    assert(sa != NULL);

    static char buffer[40];

    const char* str;
    switch(sa->sa_family) {
        case AF_INET:
            str = inet_ntop(sa->sa_family,
                            &((struct sockaddr_in*)sa)->sin_addr,
                            buffer,
                            sizeof(buffer));
            break;
        case AF_INET6:
            str = inet_ntop(sa->sa_family,
                            &((struct sockaddr_in6*)sa)->sin6_addr,
                            buffer,
                            sizeof(buffer));
            break;
        default:
            str = "Unknown";
    }

    assert (buffer[40] == '\0');
    return str;
}


/* Connect To the specified host and service
 *
 */

/* Timed Connect
 *
 * This function tries to connect to the specified sockaddr if a connection can
 * be made within tval time.
 *
 * The socket is temporarily put in non-blocking mode, a connection is tarted,
 * and select is used to do the actual timeout logic.
 */
static
int timedconnect(int              sock,
                 struct sockaddr* addr,
                 socklen_t        leng,
                 struct timeval   tval) {

    int status = -1;

    int ostate = fcntl(sock, F_GETFL, NULL);
    int nstate = ostate | O_NONBLOCK;

    if( ostate < 0 || fcntl(sock, F_SETFL, nstate) < 0) {
        perror("fcntl");
    }
    else {
        status = connect(sock, addr, leng);
        if(status < 0 && errno == EINPROGRESS) {
            fd_set wrset;
            int maxfd = mkfdset(&wrset, sock, 0);
            status = (0 < select(maxfd+1, NULL, &wrset, NULL, &tval) ?
                      0 : -1);
        }

        ostate = fcntl(sock, F_GETFL, NULL);
        nstate = ostate & ~O_NONBLOCK;
        if(ostate < 0 || fcntl(sock, F_SETFL, &nstate) < 0) {
            perror("fcntl");
        }
    }


    return status;

}

/* Try to connect
 * ai       An addrinfo structure.
 * returns  -1 or a socket connected to the sockaddr within ai.
 *
 * This function will create a new socket and try to connect to the socketaddr
 * contained within the provided addrinfo structure.
 */
static
int tryconnect(struct addrinfo* ai) {
    assert(ai);
    struct timeval tv = {1,0};
    int s = socket(ai->ai_family, ai->ai_socktype, 0);
    if(s != -1 && 0 != timedconnect(s, ai->ai_addr, ai->ai_addrlen, tv)) {
        close(s);
        s = -1;
    }

    return s;
}


static
int connectto(const char* name, const char* svc) {
    assert(name != NULL);
    assert(svc  != NULL);

    int s = -1;

    struct addrinfo hint;
    bzero(&hint, sizeof(struct addrinfo));
    hint.ai_socktype = SOCK_STREAM;

    struct addrinfo* info = NULL;

    if (0    == getaddrinfo(name, svc, &hint, &info) &&
        NULL != info ) {

        struct addrinfo* p = info;

        s = tryconnect(p);
        while (s == -1 && p->ai_next != NULL) {
            p = p->ai_next;
            s = tryconnect(p);
        }
    }

    freeaddrinfo(info);
    return s;
}




/* Parse Options
 *
 * see man 3 getopt
 */
static
bool parseoptions(int argc, char* argv[]) {
    static const char* OPTS = "hc:";

    bool parsed = true;

    char c = getopt(argc, argv, OPTS);
    while(c != -1) {
        switch (c) {
            case 'c':
                conffile = optarg;
                break;

            case 'h':
                printusage = true;
                break;
            case 'f':
                foreground = true;
                break;
            case '?':
                parsed = false;
                break;
        }

        c = parsed ? getopt(argc, argv, OPTS) : -1;
    }

    if(parsed) {
        argc -= optind;
        argv += optind;
    }

    return parsed;
}

/* Print Usage Statement
 *
 */

static
void usage(char* cmd, FILE* file) {
    fprintf(file, "Usage: %s -c file.cfg [-h]\n", cmd);
}

/* Ensure Tap device is open.
 *
 */
static
int ensuretap(char* path) {
    int fd = open(path, O_RDWR | O_NOSIGPIPE);
    if(-1 == fd) {
        perror("open");
        fprintf(stderr, "Failed to open device %s\n", path);
        exit(EXIT_FAILURE);
    }
    return fd;
}

/* Ensure socket
 *
 * Note the use of atoi, htons, and inet_pton.
 */
static
int ensuresocket(char* localaddr, char* port) {
    int sock = socket(PF_INET, SOCK_DGRAM, 0);
    if(-1 == sock) {
        perror("socket");
        exit (EXIT_FAILURE);
    }

    int bcast = 1;
    if (-1 == setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                         &bcast, sizeof(bcast))) {
        perror("setsockopt(broadcast)");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = makesockaddr(localaddr, port);
    if(0 != bind(sock, (struct sockaddr*)&addr, sizeof(addr))) {
        perror("bind");
        char buf[80];
        fprintf(stderr,
                "failed to bind to %s\n",
                inet_ntop(AF_INET, &(addr.sin_addr), buf, 80));
        exit(EXIT_FAILURE);
    }

    return sock;
}

/* Make Sock Addr
 *
 * Note the use of inet_pton and htons.
 */
static
struct sockaddr_in makesockaddr(char* address, char* port) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(atoi(port));
    inet_pton(AF_INET, address, &(addr.sin_addr));

    return addr;
}

/* mkfdset
 *
 * Note the use of va_list, va_arg, and va_end.
 */
static
int mkfdset(fd_set* set, ...) {
    int max = 0;

    FD_ZERO(set);

    va_list ap;
    va_start(ap, set);
    int s = va_arg(ap, int);
    while(s != 0) {
        if(s > max)
            max = s;
        FD_SET(s, set);
        s = va_arg(ap, int);
    }
    va_end(ap);

    return max;
}

static
void kvfree(void* key, void* value) {
    free(key);
    free(value);
}
struct frame {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
    uint8_t data[1500]
};

typedef struct ip6Header{
    uint32_t vers : 4;
    uint32_t class : 8;
    uint32_t flow : 20;
    uint32_t payloadLen : 16;
    uint32_t nextHdr : 8;
    uint32_t hopLim : 8;
    unsigned char sourceAddr[16];
    unsigned char destAddr[16];
    uint8_t headers[];
} ip6Header_t;

typedef struct segment {
    uint16_t srcPort;
    uint16_t destPort;
    uint32_t sequenceNum;
    uint32_t ackNum;
    uint16_t            : 4;
    uint16_t headerSize : 4;
    uint16_t FIN        : 1;
    uint16_t SYN        : 1;
    uint16_t RST        : 1;
    uint16_t PSH        : 1;
    uint16_t ACK        : 1;
    uint16_t URG        : 1;
    uint16_t            : 2;
    uint16_t window;
    uint16_t checkSum;
    uint16_t urgent;
    uint32_t options[];
} tcpSeg_t;

struct ipData{
    uint16_t localPort;
    unsigned char remoteAdr[16];
    uint16_t remotePort;
};

static
int maccmp(void* l, void* r){
    return memcmp(l, r, 6);
}

static
int ipDataCmp(void* l, void* r){
    return memcmp(l, r, 48);
}

static
int blacklistCmp(void* l, void* r ){
    return memcmp(l, r, 16);
}

int isSpecialMAC(uint8_t src){
    return 0xFFFFFF == src;
}








/* Bridge
 *
 * Note the use of select, sendto, and recvfrom.
 */
static
void bridge(int tap, int in, int out, struct sockaddr_in bcaddr) {

    fd_set rdset;
    struct ipData hashData;
    hashtable ipHT = htnew(sizeof(hashData), ipDataCmp, kvfree);
    hashtable ht = htnew(32, maccmp, kvfree);
    hashtable blacklist = htnew(16, blacklistCmp, kvfree);

    int maxfd = mkfdset(&rdset, tap, in, out, 0);

    struct frame buffer;
    ip6Header_t *ipv6Header;
    tcpSeg_t *tcpHeader;

    while (0 <= select(1 + maxfd, &rdset, NULL, NULL, NULL)) {

        if (FD_ISSET(tap, &rdset)) {

            ssize_t rdct = read(tap, &buffer, sizeof(struct frame));
            if (rdct < 0) {
                perror("read");
            }
            else{
                if(!htons(buffer.type == 0x86DD)){
                    ipv6Header = (ip6Header_t *)(buffer.data);
                    if(ipv6Header->nextHdr == 6){
                        tcpHeader = (tcpSeg_t *)ipv6Header->headers;
                        if(tcpHeader->SYN){
                            memcpy(hashData.remoteAdr, ipv6Header->destAddr, 16);
                            memcpy(hashData.localPort, tcpHeader->srcPort, 16);
                            memcpy(hashData.remotePort, tcpHeader->destPort, 16);
                            
                            htinsert(ipHT, &hashData, sizeof(hashData), 1);
                        }
                    }
                }


                struct sockaddr* to = htfind(ht, buffer.dst, 6);
                if(to == NULL){
                    to = (struct sockaddr*)(&bcaddr);
                }

                if (-1 == sendto(out, &buffer, rdct, 0,
                        (struct sockaddr *) to, sizeof(buffer.dst))) {
                        perror("sendto");
                    }
            }

        }
        else if (FD_ISSET(in, &rdset) || FD_ISSET(out, &rdset)) {
            struct sockaddr_in from;
            int sock = FD_ISSET(in, &rdset) ? in : out;
            socklen_t flen = sizeof(from);
            ssize_t rdct = recvfrom(sock, &buffer, sizeof(struct frame), 0,
                                    (struct sockaddr *) &from, &flen);

            if (rdct < 0) {
                perror("recvfrom");
            } else {
                if (!htons(buffer.type) == 0x86DD) {
                    ipv6Header = (ip6Header_t *) (buffer.data);
                    if (ipv6Header->nextHdr == 6) {
                        tcpHeader = (tcpSeg_t *) ipv6Header->headers;

                        memcpy(hashData.remoteAdr, ipv6Header->destAddr, 16);
                        memcpy(hashData.remotePort, tcpHeader->srcPort, 16);
                        memcpy(hashData.localPort, tcpHeader->destPort, 16);

                        if(hthaskey(ipHT, &hashData, sizeof(hashData))) {
                            if (!isSpecialMAC(buffer.src)) {
                                if (hthaskey(ht, buffer.src, 6)) {
                                    memcpy(htfind(ht, buffer.src, 6), &from, sizeof(struct sockaddr_in));
                                } else {
                                    void *key = buffer.src;
                                    void *val = &from;
                                    htstrinsert(ht, key, val);
                                }
                            }
                        }
                        else{
                            htstrinsert(blacklist, hashData.remoteAdr, 0);
                            //ipv6, they're initiating connection, and they're not already in hashtable, designate bad address.
                        }
                    }
                }
            }

            if (-1 == write(tap, &buffer, rdct)) {
                perror("write");
            }
        }

        maxfd = mkfdset(&rdset, tap, in, out, 0);
    }

}


static
void daemonize(hashtable conf){
    daemon(0,0);
    if(hthasstrkey(conf, PID)){
        FILE* pidfile = fopen(htstrfind(conf, PID), "w");
        if(pidfile != NULL){
            fprintf(pidfile, "%d\n", getpid());
            fclose(pidfile);
        }
    }
}
