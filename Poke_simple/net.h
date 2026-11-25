#ifndef NET_H
# define NET_H

/*
** Petite couche reseau TCP (Winsock sous Windows, sockets BSD ailleurs).
** Volontairement independante de raylib : windows.h et raylib.h ne
** peuvent pas vivre dans le meme fichier (noms en conflit).
** Protocole : chaque message fait toujours NET_LINE octets ("pos 120 340 1 1").
*/

# define NET_LINE 80   /* taille d un message (comme MAX dans l exemple) */

enum e_net
{
	NET_IDLE, NET_LISTEN, NET_CONNECTING, NET_UP, NET_FAILED
};

int		net_listen(int port);
int		net_connect(const char *host, int port);
int		net_state(void);
void	net_update(void);
void	net_send(const char *fmt, ...);
int		net_recv(char *line);
void	net_close(void);
int		net_local_ip(char *out, int size);
const char	*net_error(void);

#endif
