#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/*
** Reseau TCP tout simple, dans le style de l'exemple serveur/client :
**   serveur : socket() -> bind() -> listen() -> accept()
**   client  : socket() -> connect()
**   ensuite : write() pour envoyer, read() pour recevoir, close() a la fin
** Difference avec l'exemple : les sockets sont "non bloquants". Sinon
** accept() / connect() / read() figeraient la fenetre raylib en attendant.
** Chaque message fait toujours MAX octets (comme buff[MAX] dans l'exemple).
*/

#ifdef _WIN32
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
# define close(s)        closesocket(s)
# define read(s, b, n)   recv(s, (char *)(b), n, 0)
# define write(s, b, n)  send(s, (const char *)(b), n, 0)
# define bzero(b, n)     memset(b, 0, n)
# define pas_pret()      (WSAGetLastError() == WSAEWOULDBLOCK)
# define en_cours()      (WSAGetLastError() == WSAEWOULDBLOCK)
#else
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
# include <signal.h>
# include <strings.h>
# include <sys/socket.h>
# include <sys/select.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <ifaddrs.h>
# include <net/if.h>
# define pas_pret()      (errno == EAGAIN || errno == EWOULDBLOCK)
# define en_cours()      (errno == EINPROGRESS)
#endif

#define MAX NET_LINE
#define SA  struct sockaddr

static int  sockfd = -1;        /* socket d'ecoute (serveur) ou de connexion (client) */
static int  connfd = -1;        /* socket du joueur connecte (cote serveur) */
static int  state = NET_IDLE;
static char err[96];
static char in[MAX * 64];       /* octets recus, pas encore lus */
static int  in_len;
static char out[MAX * 64];      /* messages a envoyer */
static int  out_len;

/* Le socket sur lequel on parle : connfd pour le serveur, sockfd pour le client */
static int  talk(void)
{
    return connfd != -1 ? connfd : sockfd;
}

static void non_bloquant(int fd)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void start_once(void)
{
#ifdef _WIN32
    static int done;
    WSADATA d;
    if (!done && WSAStartup(MAKEWORD(2, 2), &d) == 0)
        done = 1;
#else
    signal(SIGPIPE, SIG_IGN);       /* ecrire sur un socket ferme ne tue pas le jeu */
#endif
}

void net_close(void)
{
    if (connfd != -1)
        close(connfd);
    if (sockfd != -1)
        close(sockfd);
    connfd = -1;
    sockfd = -1;
    state = NET_IDLE;
    in_len = 0;
    out_len = 0;
    err[0] = '\0';
}

static int fail(const char *msg)
{
    net_close();
    snprintf(err, sizeof(err), "%s", msg);
    state = NET_FAILED;
    return 0;
}

int net_state(void)         { return state; }
const char *net_error(void) { return err; }

/* ------------------------------------------------------------ serveur --- */

int net_listen(int port)
{
    struct sockaddr_in servaddr;
    int on = 1;

    start_once();
    net_close();
    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        return fail("création du socket impossible");
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    bzero(&servaddr, sizeof(servaddr));
    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    // Binding newly created socket to given IP and verification
    if (bind(sockfd, (SA *)&servaddr, sizeof(servaddr)) != 0)
        return fail("bind impossible : port déjà utilisé ?");
    // Now server is ready to listen and verification
    if (listen(sockfd, 5) != 0)
        return fail("listen impossible");
    non_bloquant(sockfd);           /* accept() ne bloquera plus */
    state = NET_LISTEN;
    return 1;
}

static void try_accept(void)
{
    // Accept the data packet from client and verification
    connfd = accept(sockfd, NULL, NULL);
    if (connfd < 0)
    {
        connfd = -1;                /* personne pour l'instant */
        return;
    }
    non_bloquant(connfd);
    close(sockfd);                  /* on n'attend plus personne d'autre */
    sockfd = -1;
    state = NET_UP;
}

/* ------------------------------------------------------------- client --- */

int net_connect(const char *host, int port)
{
    struct sockaddr_in servaddr;

    start_once();
    net_close();
    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        return fail("création du socket impossible");
    bzero(&servaddr, sizeof(servaddr));
    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(host);
    servaddr.sin_port = htons(port);
    if (servaddr.sin_addr.s_addr == INADDR_NONE)
        return fail("adresse IP invalide (ex : 192.168.1.20)");
    non_bloquant(sockfd);
    // connect the client socket to server socket
    if (connect(sockfd, (SA *)&servaddr, sizeof(servaddr)) != 0 && !en_cours())
        return fail("connexion refusée");
    state = NET_CONNECTING;
    return 1;
}

/* connect() non bloquant : on regarde si la connexion est finie */
static void try_connected(void)
{
    fd_set w;
    struct timeval tv = {0, 0};
    int e = 0;
    socklen_t len = sizeof(e);

    FD_ZERO(&w);
    FD_SET(sockfd, &w);
    if (select(sockfd + 1, NULL, &w, NULL, &tv) <= 0)
        return;
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&e, &len);
    if (e != 0)
        fail("connexion refusée (IP, port ou pare-feu ?)");
    else
        state = NET_UP;
}

/* ------------------------------------------------------ envoyer / lire --- */

/* Envoie ce qui attend dans out[] (write comme dans l'exemple) */
static void flush_out(void)
{
    int n;

    while (out_len > 0)
    {
        n = (int)write(talk(), out, out_len);
        if (n <= 0)
        {
            if (n < 0 && !pas_pret())
                fail("connexion perdue");
            return;
        }
        memmove(out, out + n, out_len - n);
        out_len -= n;
    }
}

/* Lit tout ce qui est arrive (read comme dans l'exemple) */
static void fill_in(void)
{
    int n;

    while (in_len < (int)sizeof(in))
    {
        n = (int)read(talk(), in + in_len, sizeof(in) - in_len);
        if (n == 0)
        {
            fail("l'autre joueur s'est deconnecte");
            return;
        }
        if (n < 0)
        {
            if (!pas_pret())
                fail("connexion perdue");
            return;
        }
        in_len += n;
    }
}

/* A appeler une fois par image : accept / connect, puis envoi et reception */
void net_update(void)
{
    if (state == NET_LISTEN)
        try_accept();
    else if (state == NET_CONNECTING)
        try_connected();
    if (state == NET_UP)
    {
        flush_out();
        if (state == NET_UP)
            fill_in();
    }
}

/* net_send("pos %d %d", x, y) : comme printf, mais ca part vers l'autre joueur */
void net_send(const char *fmt, ...)
{
    char buff[MAX];
    va_list ap;

    if (state != NET_UP || out_len + MAX > (int)sizeof(out))
        return;
    bzero(buff, MAX);
    va_start(ap, fmt);
    vsnprintf(buff, MAX, fmt, ap);
    va_end(ap);
    memcpy(out + out_len, buff, MAX);   /* un message = MAX octets */
    out_len += MAX;
    flush_out();
}

/* Renvoie 1 et remplit line si un message complet est arrive */
int net_recv(char *line)
{
    if (in_len < MAX)
        return 0;
    memcpy(line, in, MAX);
    line[MAX - 1] = '\0';
    memmove(in, in + MAX, in_len - MAX);
    in_len -= MAX;
    return 1;
}

/* -------------------------------------------------------------- mon IP --- */

/* Adresse IPv4 a donner a l'autre joueur */
int net_local_ip(char *out_ip, int size)
{
    struct sockaddr_in a, me;
    socklen_t len = sizeof(me);
    int s;

    start_once();
    snprintf(out_ip, size, "127.0.0.1");
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != -1)
    {
        /* astuce : "connecter" un socket UDP (rien n'est envoye) donne l'IP utilisee */
        bzero(&a, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(53);
        a.sin_addr.s_addr = inet_addr("8.8.8.8");
        if (connect(s, (SA *)&a, sizeof(a)) == 0
            && getsockname(s, (SA *)&me, &len) == 0 && me.sin_addr.s_addr != 0)
        {
            snprintf(out_ip, size, "%s", inet_ntoa(me.sin_addr));
            close(s);
            return 1;
        }
        close(s);
    }
#ifndef _WIN32
    /* Pas d'Internet (point d'acces, reseau local seul) : on lit les interfaces */
    {
        struct ifaddrs *all, *i;
        int found = 0;

        if (getifaddrs(&all) == 0)
        {
            for (i = all; i && !found; i = i->ifa_next)
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET
                    && (i->ifa_flags & IFF_UP) && !(i->ifa_flags & IFF_LOOPBACK))
                {
                    snprintf(out_ip, size, "%s",
                        inet_ntoa(((struct sockaddr_in *)i->ifa_addr)->sin_addr));
                    found = 1;
                }
            freeifaddrs(all);
        }
        return found;
    }
#else
    return 0;
#endif
}
