/* $EPIC: server.c,v 1.142 2005/01/11 05:30:52 jnelson Exp $ */
/*
 * server.c:  Things dealing with that wacky program we call ircd.
 *
 * Copyright (c) 1990 Michael Sandroff.
 * Copyright (c) 1991, 1992 Troy Rollo.
 * Copyright (c) 1992-1996 Matthew Green.
 * Copyright � 1993, 2003 EPIC Software Labs.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notices, the above paragraph (the one permitting redistribution),
 *    this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the author(s) may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define NEED_SERVER_LIST
#include "irc.h"
#include "commands.h"
#include "functions.h"
#include "alias.h"
#include "parse.h"
#include "ssl.h"
#include "server.h"
#include "ircaux.h"
#include "lastlog.h"
#include "exec.h"
#include "window.h"
#include "output.h"
#include "names.h"
#include "hook.h"
#include "notify.h"
#include "alist.h"
#include "screen.h"
#include "status.h"
#include "vars.h"
#include "newio.h"
#include "translat.h"
#include "reg.h"

/************************ SERVERLIST STUFF ***************************/

	Server **server_list = (Server **) 0;
	int	number_of_servers = 0;

	int	primary_server = NOSERV;
	int	from_server = NOSERV;
	int	parsing_server_index = NOSERV;
	int	last_server = NOSERV;


/************************************************************************/
typedef struct ServerInfo {
	char *	freestr;
	int	refnum;
	char *	host;
	int	port;
	char *	password;
	char *	nick;
	char *	group;
	char *	server_type;
} ServerInfo;

static	int	str_to_serverinfo (const char *str, ServerInfo *s);
static	void	free_serverinfo (ServerInfo *s);
static	int	serverinfo_to_servref (ServerInfo *s);
static	int	serverinfo_to_newserv (ServerInfo *s);
static 	void 	remove_from_server_list (int i);


static	int	str_to_serverinfo (const char *str, ServerInfo *s)
{
	char *ptr;
	ssize_t span;
	char *after;

	s->refnum = NOSERV;
	s->host = NULL;
	s->port = 0;
	s->password = NULL;
	s->nick = NULL;
	s->group = NULL;
	s->server_type = NULL;

	ptr = s->freestr = malloc_strdup(str);
	if (ptr && is_number(ptr))
	{
		int	i;

		i = strtol(ptr, &after, 10);
		if ((!after || !*after) && get_server(i))
		{
			s->refnum = i;
			return 0;
		}
	}

	do
	{
		if (*ptr == '[')
		{
		    s->host = ptr + 1;
		    if ((span = MatchingBracket(ptr + 1, '[', ']')) >= 0)
		    {
			ptr = ptr + 1 + span;
			*ptr++ = 0;
		    }
		    else
			break;
		}
		else
		    s->host = ptr;

		if (!(ptr = strchr(ptr, ':')))
			break;
		*ptr++ = 0;
		if (!*ptr)
			break;
		s->port = atol(ptr);

		if (!(ptr = strchr(ptr, ':')))
			break;
		*ptr++ = 0;
		if (!*ptr)
			break;
		s->password = ptr;

		if (!(ptr = strchr(ptr, ':')))
			break;
		*ptr++ = 0;
		if (!*ptr)
			break;
		s->nick = ptr;

		if (!(ptr = strchr(ptr, ':')))
			break;
		*ptr++ = 0;
		if (!*ptr)
			break;
		s->group = ptr;

		if (!(ptr = strchr(ptr, ':')))
			break;
		*ptr++ = 0;
		if (!*ptr)
			break;
		s->server_type = ptr;

		if (!(ptr = strchr(ptr, ':')))
			break;
		else
			*ptr++ = 0;
	}
	while (0);

	if (s->port == 0)
		s->port = irc_port;

	return 0;
}

static	void	free_serverinfo (ServerInfo *si)
{
	new_free(&si->freestr);
	si->refnum = NOSERV;
	si->host = si->password = si->nick = NULL;
	si->group = si->server_type = NULL;
	si->port = 0;
}

static	int	serverinfo_to_servref (ServerInfo *si)
{
	int	i, j, opened;
	Server *s;

	if (si->refnum != NOSERV && get_server(si->refnum))
		return si->refnum;

	if (!si->host)
		return NOSERV;

	for (opened = 1; opened >= 0; opened--)
	{
	    for (i = 0; i < number_of_servers; i++)
	    {
		if (!(s = get_server(i)))
			continue;

		if (!s->name)
			continue;

		if (opened == 1 && s->des < 0)
			continue;

		if (si->port != 0 && si->port != s->port)
			continue;

		if (s->name && wild_match(si->host, s->name))
			return i;

		if (s->itsname && wild_match(si->host, s->itsname))
			return i;

		if (s->group && wild_match(si->host, s->group))
			return i;

		for (j = 0; j < s->altnames->numitems; j++)
		{
			if (!s->altnames->list[j].name)
				continue;

			if (wild_match(si->host, s->altnames->list[j].name))
				return i;
		}
	    }
	}

	return NOSERV;
}

static	int	serverinfo_to_newserv (ServerInfo *si)
{
	int	i;
	Server *s;

	for (i = 0; i < number_of_servers; i++)
		if (server_list[i] == NULL)
			break;

	if (i == number_of_servers)
	{
		number_of_servers++;
		RESIZE(server_list, Server *, number_of_servers);
	}

	s = server_list[i] = new_malloc(sizeof(Server));
	s->name = malloc_strdup(si->host);
	s->itsname = (char *) 0;
	s->password = (char *) 0;
	s->group = NULL;
	s->altnames = new_bucket();
	s->away = (char *) 0;
	s->version_string = (char *) 0;
	s->server2_8 = 0;
	s->operator = 0;
	s->des = -1;
	s->version = 0;
	s->status = SERVER_RECONNECT;
	s->nickname = (char *) 0;
	s->s_nickname = (char *) 0;
	s->d_nickname = (char *) 0;
	s->unique_id = (char *) 0;
	s->userhost = (char *) 0;
	s->port = si->port;
	s->line_length = IRCD_BUFFER_SIZE;
	s->max_cached_chan_size = -1;
	s->who_queue = NULL;
	s->ison_max = 1;
	s->ison_queue = NULL;
	s->ison_wait = NULL;
	s->userhost_max = 1;
	s->userhost_queue = NULL;
	s->userhost_wait = NULL;
	memset(&s->uh_addr, 0, sizeof(s->uh_addr));
	memset(&s->local_sockname, 0, sizeof(s->local_sockname));
	memset(&s->remote_sockname, 0, sizeof(s->remote_sockname));
	s->redirect = NULL;
	s->cookie = NULL;
	s->closing = 0;
	s->nickname_pending = 0;
	s->fudge_factor = 0;
	s->resetting_nickname = 0;
	s->quit_message = NULL;
	s->umode[0] = 0;
	s->addrs = NULL;
	s->next_addr = NULL;

	s->doing_privmsg = 0;
	s->doing_notice = 0;
	s->doing_ctcp = 0;
	s->waiting_in = 0;
	s->waiting_out = 0;
	s->start_wait_list = NULL;
	s->end_wait_list = NULL;

	s->invite_channel = NULL;
	s->last_notify_nick = NULL;
	s->joined_nick = NULL;
	s->public_nick = NULL;
	s->recv_nick = NULL;
	s->sent_nick = NULL;
	s->sent_body = NULL;

	s->funny_match = NULL;

	s->try_ssl = FALSE;
	s->ssl_enabled = FALSE;
	s->ssl_fd = NULL;

	if (si->password && *si->password)
		malloc_strcpy(&s->password, si->password);
	if (si->nick && *si->nick)
		malloc_strcpy(&s->d_nickname, si->nick);
	else if (!s->d_nickname)
		malloc_strcpy(&s->d_nickname, nickname);
	if (si->group && *si->group)
		malloc_strcpy(&s->group, si->group);
	if (si->server_type && *si->server_type)
	{
	    if (my_stricmp(si->server_type, "IRC-SSL") == 0)
		set_server_try_ssl(i, TRUE);
	    else
		set_server_try_ssl(i, FALSE);
	}

	make_notify_list(i);
	make_005(i);
	return i;
}

/***************************************************************************/
int	str_to_servref (const char *desc)
{
	char *	ptr;
	ServerInfo si;
	int	retval;

	ptr = LOCAL_COPY(desc);
	if (str_to_serverinfo(ptr, &si))
		return NOSERV;

	retval = serverinfo_to_servref(&si);
	free_serverinfo(&si);
	return retval;
}

int	str_to_newserv (const char *desc)
{
	char *	ptr;
	ServerInfo si;
	int	retval;

	ptr = LOCAL_COPY(desc);
	if (str_to_serverinfo(ptr, &si))
		return NOSERV;

	retval = serverinfo_to_newserv(&si);
	free_serverinfo(&si);
	return retval;
}

void	destroy_server_list (void)
{
	int	i;

	for (i = 0; i < number_of_servers; i++)
		remove_from_server_list(i);
	new_free((char **)&server_list);
}

static 	void 	remove_from_server_list (int i)
{
	Server  *s;
	int	count, j;

	if (!(s = get_server(i)))
		return;

	/* Count up how many servers are left. */
	for (count = 0, j = 0; j < number_of_servers; j++)
		if (get_server(j))
			count++;

	if (count == 1 && !dead)
	{
		say("You can't delete the last server!");
		return;
	}

	say("Deleting server [%d]", i);
	clean_server_queues(i);
	new_free(&s->name);
	new_free(&s->itsname);
	new_free(&s->password);
	new_free(&s->group);
	new_free(&s->away);
	new_free(&s->version_string);
	new_free(&s->nickname);
	new_free(&s->s_nickname);
	new_free(&s->d_nickname);
	new_free(&s->unique_id);
	new_free(&s->userhost);
	new_free(&s->cookie);
	new_free(&s->ison_queue);		/* XXX Aren't these free? */
	new_free(&s->ison_wait);
	new_free(&s->who_queue);
	new_free(&s->invite_channel);
	new_free(&s->last_notify_nick);
	new_free(&s->joined_nick);
	new_free(&s->public_nick);
	new_free(&s->recv_nick);
	new_free(&s->sent_nick);
	new_free(&s->sent_body);
	new_free(&s->funny_match);
	destroy_notify_list(i);
	destroy_005(i);

	if (get_server_ssl_enabled(i) == TRUE)
	{
#ifndef HAVE_SSL
		panic("Deleting server %d which claims to be using SSL on"
			"a non-ssl client", i);
#else
		SSL_free((SSL *)s->ssl_fd);
		SSL_CTX_free((SSL_CTX *)s->ctx);
#endif
	}
	new_free(&server_list[i]);
	s = NULL;
}


/*****************************************************************************/
/*
 * add_servers: Add a space-separated list of server descs to the server list.
 *	If the server description does not set a port, use the default port.
 *	If the server description does not set a group, use the provided group.
 *  This function modifies "servers".
 */
void	add_servers (char *servers, const char *group)
{
	char	*host;
	ServerInfo si;

	if (!servers)
		return;

	while ((host = next_arg(servers, &servers)))
	{
		str_to_serverinfo(host, &si);
		if (group && si.group == NULL)
			malloc_strcpy(&si.group, group);
		if (serverinfo_to_servref(&si) == NOSERV)
			serverinfo_to_newserv(&si);
		free_serverinfo(&si);
	}
}

/*
 * read_server_file: Add servers from "SERVERS FILE" to the server list.
 */
int 	read_server_file (void)
{
	FILE 	*fp;
	char	file_path[MAXPATHLEN + 1];
	char	buffer[BIG_BUFFER_SIZE + 1];
	Filename expanded;
	char	*defaultgroup = NULL;

	if (getenv("IRC_SERVERS_FILE"))
		strlcpy(file_path, getenv("IRC_SERVERS_FILE"), sizeof file_path);
	else
	{
#ifdef SERVERS_FILE
		*file_path = 0;
		if (SERVERS_FILE[0] != '/' && SERVERS_FILE[0] != '~')
			strlcpy(file_path, irc_lib, sizeof file_path);
		strlcat(file_path, SERVERS_FILE, sizeof file_path);
#else
		return -1;
#endif
	}

	if (normalize_filename(file_path, expanded))
		return -1;

	if (!(fp = fopen(expanded, "r")))
		return -1;

	while (fgets(buffer, BIG_BUFFER_SIZE, fp))
	{
		chop(buffer, 1);
		if (*buffer == '#')
			continue;
		else if (*buffer == '[')
		{
		    char *p;
		    if ((p = strrchr(buffer, ']')))
			*p++ = 0;
		    malloc_strcpy(&defaultgroup, buffer + 1);
		}
		else if (*buffer == 0)
			continue;
		else
			add_servers(buffer, defaultgroup);
	}

	fclose(fp);
	new_free(&defaultgroup);
	return 0;
}

/* display_server_list: just guess what this does */
void 	display_server_list (void)
{
	Server *s;
	int	i;

	if (!server_list)
	{
		say("The server list is empty");
		return;
	}

	if (from_server != NOSERV && (s = get_server(from_server)))
		say("Current server: %s %d", s->name, s->port);
	else
		say("Current server: <None>");

	if (primary_server != NOSERV && (s = get_server(primary_server)))
		say("Primary server: %s %d", s->name, s->port);
	else
		say("Primary server: <None>");

	say("Server list:");
	for (i = 0; i < number_of_servers; i++)
	{
		if (!(s = get_server(i)))
			continue;

		if (!s->nickname)
			say("\t%d) %s %d [%s] %s [%s]", i, s->name, s->port, 
				get_server_group(i), get_server_type(i),
				server_states[get_server_status(i)]);
		else if (is_server_open(i))
			say("\t%d) %s %d (%s) [%s] %s [%s]", 
				i, s->name, s->port,
				s->nickname, get_server_group(i),
				get_server_type(i),
				server_states[get_server_status(i)]);
		else
			say("\t%d) %s %d (was %s) [%s] %s [%s]", i, s->name, 
				s->port, s->nickname, get_server_group(i),
				get_server_type(i),
				server_states[get_server_status(i)]);
	}
}

char *	create_server_list (void)
{
	Server	*s;
	int	i;
	char	*buffer = NULL;
	size_t	bufclue = 0;

	for (i = 0; i < number_of_servers; i++)
	{
		if (!(s = get_server(i)))
			continue;

		if (s->des != -1)
		    malloc_strcat_wordlist_c(&buffer, space, 
					get_server_itsname(i), &bufclue);
	}

	RETURN_MSTR(buffer);
}

/* server_list_size: returns the number of servers in the server list */
int 	server_list_size (void)
{
	return number_of_servers;
}

/* 
 * Look for another server in the same group as 'oldserv'
 * Direction is 1 to go forward, -1 to go backward. 
 * Other values will lose.
 */
static int	next_server_in_group (int oldserv, int direction)
{
	int	newserv;
	int	counter;

	for (counter = 1; counter <= number_of_servers; counter++)
	{
		/* Starting with 'oldserv', move in the given direction */
		newserv = oldserv + (counter * direction);

		/* Make sure the new server is always a valid servref */
		while (newserv < 0)
			newserv += number_of_servers;

		/* Make sure the new server is valid. */
		if (newserv > number_of_servers)
			newserv %= number_of_servers;

		/* If there is no server at this refnum, skip it. */
		if (!get_server(newserv))
			continue;

		if (!my_stricmp(get_server_group(oldserv),
			        get_server_group(newserv)))
			return newserv;
	}
	return oldserv;		/* Couldn't find one. */
}

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/
static	void	server_is_unregistered (int refnum);

	int	connected_to_server = 0;	/* How many active server 
						   connections are open */
static	char    lame_wait_nick[] = "***LW***";
static	char    wait_nick[] = "***W***";
static	int	never_connected = 1;

const char *server_states[8] = {
	"RECONNECT",		"CONNECTING",		"REGISTERING",
	"SYNCING",		"ACTIVE",		"EOF",
	"CLOSING",		"CLOSED"
};



/*************************** SERVER STUFF *************************/
/*
 * server: the /SERVER command. Read the SERVER help page about 
 *
 * /SERVER
 *	Show the server list.
 * /SERVER -DELETE <refnum|desc>
 *	Remove server <refnum> (or <desc>) from server list.
 *	Fails if you do not give it a refnum or desc.
 *	Fails if server does not exist.
 * 	Fails if server is open.
 * /SERVER -ADD <desc>
 *	Add server <desc> to server list.
 *	Fails if you do not give it a <desc>
 * /SERVER +<refnum|desc>
 *	Allow server to reconnect if windows are pointed to it.
 *	Note: server reconnection is asynchronous
 * /SERVER -<refnum|desc>
 *	Unconditionally close a server connection
 *	Note: server disconnection is synchronous!
 * /SERVER +
 *	Switch windows from current server to next server in same group
 * /SERVER -
 *	Switch windows from current server to previous server in same group
 * /SERVER <refnum|desc>
 *	Switch windows from current server to another server.
 */
BUILT_IN_COMMAND(servercmd)
{
	char	*server = NULL;
	int	i;
	int	olds, news;
	char *	shadow;
	size_t	slen;

	olds = from_server;
	news = NOSERV;

	/*
	 * This is a new trick I'm testing out to see if it works
	 * better than the hack I was using.
	 */
	shadow = LOCAL_COPY(args);
	if ((server = next_arg(shadow, &shadow)) == NULL)
	{
		display_server_list();
		return;
	}
	slen = strlen(server);

	/*
	 * /SERVER -DELETE <refnum>             Delete a server from list
	 */
	if (slen > 1 && !my_strnicmp(server, "-DELETE", slen))
	{
		next_arg(args, &args);		/* Skip -DELETE */
		if (!(server = new_next_arg(args, &args)))
		{
			say("Need argument to /SERVER -DELETE");
			return;
		}

		if ((i = str_to_servref(server)) == NOSERV)
		{
			say("No such server [%s] in list", server);
			return;
		}

		if (is_server_open(i))
		{
			say("Can not delete server %d because it is open", i);
			return;
		}

		remove_from_server_list(i);
		return;
	}

	/*
	 * SERVER -ADD <host>                   Add a server to list
	 */
	if (slen > 1 && !my_strnicmp(server, "-ADD", slen))
	{
		next_arg(args, &args);		/* Skip -ADD */
		if (!(server = new_next_arg(args, &args)))
		{
			say("Need argument to /SERVER -ADD");
			return;
		}

		if ((from_server = str_to_servref(server)) != NOSERV)
		{
			say("Server [%s] already exists as server %d", 
					server, from_server);
			return;
		}

		from_server = str_to_newserv(server);
		say("Server [%s] added as server %d", server, from_server);
		return;
	}

	/*
	 * /server +host.com                    Allow server to reconnect
	 */
	if (slen > 1 && *server == '+')
	{
		args++;			/* Skip the + */
		server = new_next_arg(args, &args);

		if ((i = str_to_servref(server)) == NOSERV)
		{
			say("No such server [%s] in list", server);
			return;
		}

		if (get_server_status(i) == SERVER_CLOSED)
			set_server_status(i, SERVER_RECONNECT);
		return;
	}

	/*
	 * /server -host.com                    Force server to disconnect
	 */
	if (slen > 1 && *server == '-')
	{
		args++;			/* Skip the + */
		server = new_next_arg(args, &args);

		if ((i = str_to_servref(server)) == NOSERV)
		{
			say("No such server [%s] in list", server);
			return;
		}

		set_server_quit_message(from_server, 
				"Disconnected at user request");
		close_server(i, NULL);
		return;
	}


	/* * * * The rest of these actually move windows around * * * */
	olds = from_server;

	if (*server == '+')
		news = next_server_in_group(olds, 1);
	else if (*server == '-')
		news = next_server_in_group(olds, -1);
	else
	{
		if ((news = str_to_servref(server)) == NOSERV)
		{
		    if ((news = str_to_newserv(server)) == NOSERV)
		    {
			say("I can't parse server description [%s]", server);
			return;
		    }
		}
	}

	/* Always unconditionally allow new server to auto-reconnect */
	if (get_server_status(news) == SERVER_CLOSED)
		set_server_status(news, SERVER_RECONNECT);

	/* If the user is not actually changing server, just reassure them */
	if (olds == news)
	{
		say("Connected to port %d of server %s",
			get_server_port(olds), get_server_name(olds));
		return;
	}

	/* Do the switch! */
	set_server_quit_message(olds, "Changing servers");

	/* XXX - Should i really be doing this here? */
	if (my_stricmp(get_server_type(news), "IRC-SSL") == 0)
		set_server_ssl_enabled(news, TRUE);

	change_window_server(olds, news);
}


/* SERVER INPUT STUFF */
static int	server_ssl_reader (int fd, char **buffer, size_t *buffer_size, size_t *start)
{
#ifndef HAVE_SSL
	panic("Attempt to call server_ssl_reader on non-ssl client");
#else
	Server *s;
	int	i;

	for (i = 0; i < number_of_servers; i++)
	{
		if (!(s = get_server(i)))
		    continue;
		if (s->des == fd)
		    return ssl_reader(s->ssl_fd, buffer, buffer_size, start);
	}

	panic("Server_ssl_reader callback on fd [%d] is not a server", fd);
#endif
	return -1;
}

/*
 * do_server: check the given fd_set against the currently open servers in
 * the server list.  If one have information available to be read, it is read
 * and and parsed appropriately.  If an EOF is detected from an open server,
 * and we haven't registered, window_check_servers() will restart for us.
 */
void	do_server (int fd)
{
	Server *s;
	char	buffer[IO_BUFFER_SIZE + 1];
	int	des,
		i;

	for (i = 0; i < number_of_servers; i++)
	{
		ssize_t	junk;
		char 	*bufptr = buffer;

		if (!(s = get_server(i)))
			continue;

		if ((des = s->des) < 0)
			continue;		/* Nothing to see here, */

		if (des != fd)
			continue;		/* Move along. */

		/*
		 * First look for nonblocking connects that are finished.
		 */
		if (s->status == SERVER_CONNECTING)
		{
		    SS name;
		    socklen_t len;

		    if (x_debug & DEBUG_SERVER_CONNECT)
			yell("do_server: server [%d] is now ready to write", i);
		    /*
		     * If the connect failed, then restart it.
		     */
		    len = sizeof(name);
		    if (getpeername(des, (SA *)&name, &len))
		    {
			if (x_debug & DEBUG_SERVER_CONNECT)
			    yell("Server [%d] is not connected.  Restarting connection", i);
			close_server(i, NULL);
			connect_to_server(i, 0);
			continue;
		    }

		    /* Update this! */
		    *(SA *)&s->remote_sockname = *(SA *)&name;
		    register_server(i, s->d_nickname);
		    new_open(des, do_server);
		}

	        /* Everything else is a normal read. */
		else
		{
		    last_server = from_server = i;
		    junk = dgets(des, bufptr, get_server_line_length(i), 1,
				  s->ssl_fd ? server_ssl_reader : NULL);

		    switch (junk)
		    {
		        case 0:		/* Sit on incomplete lines */
		   	    break;

			case -1:	/* EOF or other error */
			{
			    server_is_unregistered(i);
			    close_server(i, NULL);
			    say("Connection closed from %s: %s", 
				s->name,
				(dgets_errno == -1) ? 
				     "Remote end closed connection" : 
				     strerror(dgets_errno));
			    i++;		/* NEVER DELETE THIS! */
			    break;
		        }

		        default:	/* New inbound data */
		        {
			    char *end;
			    int	l;

			    end = strlen(buffer) + buffer;
			    if (*--end == '\n')
				*end-- = '\0';
			    if (*end == '\r')
				*end-- = '\0';

			    l = message_from(NULL, LEVEL_CRAP);
			    if (x_debug & DEBUG_INBOUND)
				yell("[%d] <- [%s]", 
					s->des, buffer);

			    if (translation)
				translate_from_server(buffer);
			    parsing_server_index = i;
			    parse_server(buffer, sizeof buffer);
			    parsing_server_index = NOSERV;
			    pop_message_from(l);
			    break;
		        }
		    }
	        }
	        from_server = primary_server;
	}
}


/* SERVER OUTPUT STUFF */
static void 	vsend_to_aserver (int, const char *format, va_list args);
void		send_to_aserver_raw (int, size_t len, const char *buffer);

void	send_to_aserver (int refnum, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsend_to_aserver(refnum, format, args);
	va_end(args);
}

void	send_to_server (const char *format, ...)
{
	va_list args;
	int	server;

	if ((server = from_server) == NOSERV)
		server = primary_server;

	va_start(args, format);
	vsend_to_aserver(server, format, args);
	va_end(args);
}

/* send_to_server: sends the given info the the server */
static void 	vsend_to_aserver (int refnum, const char *format, va_list args)
{
	Server *s;
	char	buffer[BIG_BUFFER_SIZE * 11 + 1]; /* make this buffer *much*
						  * bigger than needed */
	size_t	size = BIG_BUFFER_SIZE * 11;
	int	len,
		des;
	int	ofs;

	if (!(s = get_server(refnum)))
		return;

	if (refnum != NOSERV && (des = s->des) != -1 && format)
	{
		/* Keep the results short, and within reason. */
		len = vsnprintf(buffer, BIG_BUFFER_SIZE, format, args);

		if (translation)
			translate_to_server(buffer);

		if (outbound_line_mangler)
		{
			if (mangle_line(buffer, outbound_line_mangler, size) 
					> size)
				yell("mangle_line truncated results!  Ick.");
		}

		s->sent = 1;
		if (len > (IRCD_BUFFER_SIZE - 2) || len == -1)
			buffer[IRCD_BUFFER_SIZE - 2] = 0;
		if (x_debug & DEBUG_OUTBOUND)
			yell("[%d] -> [%s]", des, buffer);
		strlcat(buffer, "\r\n", sizeof buffer);

		/* This "from_server" hack is for the benefit of do_hook. */
		ofs = from_server;
		from_server = refnum;
		if (do_hook(SEND_TO_SERVER_LIST, "%d %d %s", from_server, des, buffer))
			send_to_aserver_raw(refnum, strlen(buffer), buffer);
		from_server = ofs;
	}
	else if (from_server == NOSERV)
        {
	    if (do_hook(DISCONNECT_LIST,"No Connection to %d", refnum))
		say("You are not connected to a server, "
			"use /SERVER to connect.");
        }
}

void	send_to_aserver_raw (int refnum, size_t len, const char *buffer)
{
	Server *s;
	int des;
	int err = 0;

	if (!(s = get_server(refnum)))
		return;

	if ((des = s->des) != -1 && buffer)
	{
	    if (get_server_ssl_enabled(refnum) == TRUE)
	    {
#ifndef HAVE_SSL
		panic("send_to_aserver_raw: Server %d claims to "
			"be using SSL on a non-ssl client", refnum);
#else
		if (s->ssl_fd == NULL)
		{
			say("SSL write error - ssl socket = 0");
			return;
		}
		err = SSL_write((SSL *)s->ssl_fd, buffer, strlen(buffer));
		BIO_flush(SSL_get_wbio((SSL *)s->ssl_fd));
#endif
	    }
	    else
		err = write(des, buffer, strlen(buffer));

	    if (err == -1 && (!get_int_var(NO_FAIL_DISCONNECT_VAR)))
	    {
		if (is_server_registered(refnum))
		{
			say("Write to server failed.  Resetting connection.");
			if (get_server_ssl_enabled(refnum) == TRUE)
#ifndef HAVE_SSL
			    panic("send_to_aserver_raw: Closing server %d "
				  "which claims to be using SSL on non-ssl "
				  "client", refnum);
#else
				SSL_shutdown((SSL *)s->ssl_fd);
#endif
			close_server(refnum, NULL);
		}
	    }
	}
}

void	flush_server (int servnum)
{
	if (!is_server_registered(servnum))
		return;
	set_server_redirect(servnum, "0");
	send_to_aserver(servnum, "%s", "***0");
}


/* CONNECTION/RECONNECTION STRATEGIES */
/*
 * Grab_server_address -- look up all of the addresses for a hostname and
 *	save them in the Server data for later use.  Someone must free
 * 	the results when they're done using the data (usually when we 
 *	successfully connect, or when we run out of addrs)
 */
static int	grab_server_address (int server)
{
	Server *s;
	AI	hints, *results;
	int	err;
	int	i;

	if (x_debug & DEBUG_SERVER_CONNECT)
		yell("Grabbing server addresses for server [%d]", server);

	if (!(s = get_server(server)))
	{
		say("Server [%d] does not exist -- "
			"cannot do hostname lookup", server);
		return -1;		/* XXXX */
	}

	if (s->addrs)
	{
		if (x_debug & DEBUG_SERVER_CONNECT)
		    yell("This server still has addresses left over from "
			 "last time.  Starting over anyways...");
		Freeaddrinfo(s->addrs);
		s->addrs = NULL;
		s->next_addr = NULL;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((err = Getaddrinfo(s->name, ltoa(s->port), &hints, &results)))
	{
		yell("Could not resolve hostname [%s:%d] for server [%d]: "
					"%s (%d)",
			s->name, s->port, server, 
					gai_strerror(err), err);
		return -5;
	}

	s->addrs = results;
	s->next_addr = results;
	if (x_debug & DEBUG_SERVER_CONNECT)
	{
		for (i = 0; results; results = results->ai_next)
			i++;
		yell("Found [%d] addresses for server [%d]", i, server);
	}
	return 0;
}

/*
 * Make an attempt to connect to the next server address that will 
 * receive us.  This is the guts of "connectory", but "connectory" is
 * completely self-contained, and we have to be able to support looping
 * through getaddrinfo() results, possibly on multiple asynchronous
 * occasions.  So what we do is restart from where we left off before.
 */
static int	connect_next_server_address (int server)
{
	Server *s;
	int	err = -1, fd = -1;
	SS	localaddr;
	socklen_t locallen;
	const AI *	ai;

	if (!(s = get_server(server)))
		return -1;		/* XXXX */

	if (!s->addrs)
	{
	    if (x_debug & DEBUG_SERVER_CONNECT)
		yell("There are no addresses to connect to for server [%d]", 
					server);
	    return -5;		/* XXXX */
	}

	for (ai = s->next_addr; ai; ai = ai->ai_next)
	{
	    if (x_debug & DEBUG_SERVER_CONNECT)
		yell("Trying to connect to server [%d] using next address", 
					server);
	    err = inet_vhostsockaddr(ai->ai_family, -1, &localaddr, &locallen);
	    if (err < 0)
	    {
	        if (x_debug & DEBUG_SERVER_CONNECT)
			yell("Couldn't get vhost for family [%d], "
				"trying another address", ai->ai_family);
		continue;
	    }

	    fd = client_connect((SA *)&localaddr, locallen, 
					ai->ai_addr, ai->ai_addrlen);
	    if (fd < 0)
	    {
		/* XXXX - 'fd' might be negative, strerror() is wrong! */
	        if (x_debug & DEBUG_SERVER_CONNECT)
			yell("That address failed with error (%d:%s)", 
					fd, strerror(fd));
		err = fd;
		fd = -1;
		continue;
	    }
	    else
		break;
	}

	if (!ai)
	{
	        if (x_debug & DEBUG_SERVER_CONNECT)
			yell("Out of addresses to try for server [%d]!  "
					"Giving up", server);
		Freeaddrinfo(s->addrs);
		s->addrs = NULL;
		s->next_addr = NULL;
		return -11;
	}
	else
		s->next_addr = ai->ai_next;

	if (fd < 0)
		return err;

	if (x_debug & DEBUG_SERVER_CONNECT)
		yell("connect_next_server_address returning [%d]", fd);
	return fd;
}

/*
 * This establishes a new connection to 'new_server'.  This function does
 * not worry about why or where it is doing this.  It is only concerned
 * with getting a connection up and running.
 *
 * NOTICE! THIS MUST ONLY EVER BE CALLED BY connect_to_new_server()!
 * IF YOU CALL THIS ELSEWHERE, THINGS WILL BREAK AND ITS NOT MY FAULT!
 */
int 	connect_to_server (int new_server, int restart)
{
	int 		des;
	socklen_t	len;
	Server *	s;

	/*
	 * Can't connect to refnum -1, this is definitely an error.
	 */
	if (!(s = get_server(new_server)))
	{
		say("Connecting to refnum %d.  That makes no sense.", 
			new_server);
		return -1;		/* XXXX */
	}

	/*
	 * If we are already connected to the new server, go with that.
	 */
	if (s->des != -1)
	{
		say("Connected to port %d of server %s", s->port, s->name);
		from_server = new_server;
		return -1;		/* Server is already connected */
	}

	/*
	 * Make an attempt to connect to the new server.
	 */
	say("Connecting to port %d of server %s [refnum %d]", 
			s->port, s->name, new_server);

	set_server_status(new_server, SERVER_CONNECTING);
	s->closing = 0;
	oper_command = 0;
	errno = 0;
	memset(&s->local_sockname, 0, sizeof(s->local_sockname));
	memset(&s->remote_sockname, 0, sizeof(s->remote_sockname));

	if (restart || s->addrs == NULL)
		if (grab_server_address(new_server))
			return -1;

	if ((des = connect_next_server_address(new_server)) < 0)
	{
		if (x_debug & DEBUG_SERVER_CONNECT)
			say("new_des is %d", des);

		if ((s = get_server(new_server)))
		{
		    say("Unable to connect to port %d of server %s: [%d] %s", 
				s->port, s->name, des, my_strerror(des, errno));

		    /* Would cause client to crash, if not wiped out */
		    set_server_ssl_enabled(new_server, FALSE);
		}
		else
			say("Unable to connect to server.");

		set_server_status(new_server, SERVER_CLOSED);
		return -1;		/* Connect failed */
	}

	if (x_debug & DEBUG_SERVER_CONNECT)
		say("connect_next_server_address returned [%d]", des);
	from_server = new_server;	/* XXX sigh */
	new_open_for_writing(des, do_server);

	if (*s->name != '/')
	{
		len = sizeof(s->local_sockname);
		getsockname(des, (SA *)&s->local_sockname, &len);

		len = sizeof(s->remote_sockname);
		getpeername(des, (SA *)&s->remote_sockname, &len);
	}

	/*
	 * Initialize all of the server_list data items
	 * XXX - Calling add_to_server_list is a hack.
	 */
#if 0
	add_to_server_list(s->name, s->port, NULL, NULL, NULL, NULL, 1);
#endif
	s->des = des;
	s->operator = 0;
	if (!s->d_nickname)
		malloc_strcpy(&s->d_nickname, nickname);

	/*
	 * Reset everything and go on our way.
	 */
	update_all_status();
	return 0;			/* New connection established */
}

int 	close_all_servers (const char *message)
{
	int i;

	for (i = 0; i < number_of_servers; i++)
	{
		set_server_quit_message(i, message);
		close_server(i, NULL);
	}

	return 0;
}

/*
 * close_server: Given an index into the server list, this closes the
 * connection to the corresponding server.  If 'message' is anything other
 * than the NULL or the empty_string, it will send a protocol QUIT message
 * to the server before closing the connection.
 */
void	close_server (int refnum, const char *message)
{
	Server *s;
	int	was_registered;

	/* Make sure server refnum is valid */
	if (!(s = get_server(refnum)))
	{
		yell("Closing server [%d] makes no sense!", refnum);
		return;
	}

	if (!message)
		if (!(message = get_server_quit_message(refnum)))
			message = "Leaving";

	was_registered = is_server_registered(refnum);

	set_server_status(refnum, SERVER_CLOSING);
	clean_server_queues(refnum);
	if (s->waiting_out > s->waiting_in)		/* XXX - hack! */
		s->waiting_out = s->waiting_in = 0;

	destroy_waiting_channels(refnum);
	destroy_server_channels(refnum);

	s->operator = 0;
	new_free(&s->nickname);
	new_free(&s->s_nickname);

	if (s->des == -1)
		return;		/* Nothing to do here */

	if (message && *message && !s->closing)
	{
	    s->closing = 1;
	    if (x_debug & DEBUG_OUTBOUND)
		yell("Closing server %d because [%s]", refnum, message);

	    /*
	     * Only tell the server we are leaving if we are 
	     * registered.  This avoids an infinite loop in the
	     * D-line case.
	     */
	    if (was_registered)
		    send_to_aserver(refnum, "QUIT :%s\n", message);
	    if (get_server_ssl_enabled(refnum) == TRUE)
	    {
#ifndef HAVE_SSL
		panic("close_server: Server %d claims to be using "
		      "ssl on a non-ssl client", refnum);
#else
		say("Closing SSL connection");
		SSL_shutdown((SSL *)s->ssl_fd);
#endif
	    }
	}

	do_hook(SERVER_LOST_LIST, "%d %s %s", 
			refnum, s->name, message ? message : empty_string);
	s->des = new_close(s->des);
	set_server_status(refnum, SERVER_CLOSED);
}

/********************* OTHER STUFF ************************************/

/* AWAY STATUS */
/*
 * Encapsulates everything we need to change our AWAY status.
 * This improves greatly on having everyone peek into that member.
 * Also, we can deal centrally with someone changing their AWAY
 * message for a server when we're not registered to that server
 * (when we do connect, then we send out the AWAY command.)
 * All this saves a lot of headaches and crashes.
 */
void	set_server_away (int refnum, const char *message)
{
	Server *s;

	if (!(s = get_server(refnum)))
	{
		say("You are not connected to a server.");
		return;
	}

	if (message && *message)
	{
		if (!s->away || strcmp(s->away, message))
			malloc_strcpy(&s->away, message);
		if (is_server_registered(refnum))
			send_to_aserver(refnum, "AWAY :%s", message);
	}
	else
	{
		new_free(&s->away);
		if (is_server_registered(refnum))
			send_to_aserver(refnum, "AWAY :");
	}
}

const char *	get_server_away (int refnum)
{
	Server *s;

	if (refnum == NOSERV)
	{
		int	i;

		for (i = 0; i < number_of_servers; i++)
		{
			if (!(s = get_server(i)))
				continue;

			if (is_server_registered(i) && s->away)
				return s->away;
		}

		return NULL;
	}

	if (!(s = get_server(refnum)))
		return NULL;
	
	return s->away;
}


/* USER MODES */
const char *	get_umode (int refnum)
{
	Server *s;
	char *	retval;

	if (!(s = get_server(refnum)))
		return empty_string;

	retval = s->umode;
	return retval;		/* Eliminates a specious warning from gcc. */
}

static void	add_user_mode (int refnum, int mode)
{
	Server *s;
	char c, *p, *o;
	char new_umodes[1024];		/* Too huge for words */
	int	i;

	if (!(s = get_server(refnum)))
		return;

	/* 
	 * 'c' is the mode that is being added
	 * 'o' is the umodes that are already set
	 * 'p' is the string that we are building that adds 'c' to 'o'.
	 */
	c = (char)mode;
	o = s->umode;
	p = new_umodes;

	/* Copy the modes in 'o' that are alphabetically less than 'c' */
	for (i = 0; o && o[i]; i++)
	{
		if (o[i] >= c)
			break;
		*p++ = o[i];
	}

	/* If 'c' is already set, copy it, otherwise add it. */
	if (o && o[i] == c)
		*p++ = o[i++];
	else
		*p++ = c;

	/* Copy all the rest of the modes */
	for (; o && o[i]; i++)
		*p++ = o[i];

	/* Nul terminate the new string and reset the server's info */
	*p++ = 0;
	strlcpy(s->umode, new_umodes, 54);
}

static void	remove_user_mode (int refnum, int mode)
{
	Server *s;
	char c, *o, *p;
	char new_umodes[1024];		/* Too huge for words */
	int	i;

	if (!(s = get_server(refnum)))
		return;

	/* 
	 * 'c' is the mode that is being deleted
	 * 'o' is the umodes that are already set
	 * 'p' is the string that we are building that adds 'c' to 'o'.
	 */
	c = (char)mode;
	o = s->umode;
	p = new_umodes;

	/*
	 * Copy the whole of 'o' to 'p', except for any instances of 'c'.
	 */
	for (i = 0; o && o[i]; i++)
	{
		if (o[i] != c)
			*p++ = o[i];
	}

	/* Nul terminate the new string and reset the server's info */
	*p++ = 0;
	strlcpy(s->umode, new_umodes, 54);
}

void 	clear_user_modes (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	*s->umode = 0;
}

void	update_user_mode (int refnum, const char *modes)
{
	int		onoff = 1;

	for (; *modes; modes++)
	{
		if (*modes == '-')
			onoff = 0;
		else if (*modes == '+')
			onoff = 1;
		else if (onoff == 1)
			add_user_mode(refnum, *modes);
		else if (onoff == 0)
			remove_user_mode(refnum, *modes);

		if (*modes == 'O' || *modes == 'o')
			set_server_operator(from_server, onoff);
	}
	update_all_status();
}

void	reinstate_user_modes (void)
{
	const char *modes = get_umode(from_server);

	if (!modes && !*modes)
		modes = send_umode;

	if (modes && *modes)
	{
		if (x_debug & DEBUG_OUTBOUND)
			yell("Reinstating your user modes on server [%d] to [%s]", from_server, modes);
		send_to_server("MODE %s +%s", get_server_nickname(from_server), modes);
		clear_user_modes(from_server);
	}
}


/* get_server_isssl: returns 1 if the server is using SSL connection */
int	get_server_isssl (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return 0;

	return (s->ssl_enabled == TRUE ? 1 : 0);
}

const char	*get_server_cipher (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)) || s->ssl_enabled == FALSE)
		return empty_string;

#ifndef HAVE_SSL
	return empty_string;
#else
	return SSL_get_cipher((SSL *)s->ssl_fd);
#endif
}


/* CONNECTION/REGISTRATION STATUS */
void	register_server (int refnum, const char *nick)
{
	Server *	s;
	int		ofs = from_server;

	if (!(s = get_server(refnum)))
		return;

	if (get_server_status(refnum) != SERVER_CONNECTING)
	{
		if (x_debug & DEBUG_SERVER_CONNECT)
			say("Server [%d] state should be [%d] but it is [%d]", refnum, SERVER_CONNECTING, get_server_status(refnum));
		return;		/* Whatever */
	}

	if (is_server_registered(refnum))
	{
		if (x_debug & DEBUG_SERVER_CONNECT)
			say("Server [%d] is already registered", refnum);
		return;		/* Whatever */
	}

	set_server_status(refnum, SERVER_REGISTERING);

	from_server = refnum;
	do_hook(SERVER_ESTABLISHED_LIST, "%s %d",
		get_server_name(refnum), get_server_port(refnum));
	from_server = ofs;

	if (get_server_try_ssl(refnum) == TRUE)
	{
#ifndef HAVE_SSL
		panic("register_server on server %d claims to be doing"
			"SSL on a non-ssl client.", refnum);
#else
		char *		cert_issuer;
		char *		cert_subject;
		X509 *		server_cert;
		EVP_PKEY *	server_pkey;

		say("SSL negotiation in progress...");
		/* Set up SSL connection */
		s->ctx = SSL_CTX_init(0);
		s->ssl_fd = (void *)SSL_FD_init(s->ctx, s->des);

		if (x_debug & DEBUG_SSL)
			say("SSL negotiation using %s",
				get_server_cipher(refnum));
		say("SSL negotiation on port %d of server %s complete",
			s->port, get_server_name(refnum));
		server_cert = SSL_get_peer_certificate((SSL *)s->ssl_fd);

		if (!server_cert) {
			say ("SSL negotiation failed");
			say ("WARNING: Bailing to no encryption");
			SSL_CTX_free((SSL_CTX *)s->ctx);
			send_to_aserver(refnum, "%s", empty_string);
		} else {
			char *u_cert_subject, *u_cert_issuer;

			cert_subject = X509_NAME_oneline(
				X509_get_subject_name(server_cert),0,0);
			u_cert_subject = urlencode(cert_subject);
			cert_issuer = X509_NAME_oneline(
				X509_get_issuer_name(server_cert),0,0);
			u_cert_issuer = urlencode(cert_issuer);

			server_pkey = X509_get_pubkey(server_cert);

			if (do_hook(SSL_SERVER_CERT_LIST, "%s %s %s %d",
				s->name, u_cert_subject, u_cert_issuer, EVP_PKEY_bits(server_pkey))) {
				say("SSL certificate subject: %s", cert_subject);
				say("SSL certificate issuer: %s", cert_issuer);
				say("SSL certificate public key length: %d bits", EVP_PKEY_bits(server_pkey));
			}

			set_server_ssl_enabled(refnum, TRUE);

			new_free(&u_cert_issuer);
			new_free(&u_cert_subject);
			free(cert_issuer);
			free(cert_subject);
		}
#endif
	}
	if (s->password)
		send_to_aserver(refnum, "PASS %s", s->password);

	send_to_aserver(refnum, "USER %s %s %s :%s", username, 
			(send_umode && *send_umode) ? send_umode : 
			(LocalHostName ? LocalHostName : hostname), 
			username, (get_string_var(REALNAME_VAR) ? 
				   get_string_var(REALNAME_VAR) : space));
	change_server_nickname(refnum, nick);
	if (x_debug & DEBUG_SERVER_CONNECT)
		yell("Registered with server [%d]", refnum);
}

/*
 * password_sendline: called by send_line() in get_password() to handle
 * hitting of the return key, etc 
 * -- Callback function
 */
void 	password_sendline (char *data, char *line)
{
	int	new_server;

	if (!line || !*line)
		return;

	new_server = str_to_servref(data);
	set_server_password(new_server, line);
	close_server(new_server, NULL);
	set_server_status(new_server, SERVER_RECONNECT);
}

static const char *	get_server_password (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return NULL;

	return s->password;
}

/*
 * set_server_password: this sets the password for the server with the given
 * index. If 'password' is NULL, the password is cleared
 */
char	*set_server_password (int refnum, const char *password)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return NULL;

	if (password)
		malloc_strcpy(&s->password, password);
	else
		new_free(&s->password);

	return s->password;
}


/*
 * is_server_open: Returns true if the given server index represents a server
 * with a live connection, returns false otherwise 
 */
int	is_server_open (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return 0;

	return (s->des != -1);
}

int	is_server_registered (int refnum)
{
	Server *s;
	int	status;

	if (!(s = get_server(refnum)))
		return 0;

	status = get_server_status(refnum);
	if (status == SERVER_SYNCING  || status == SERVER_ACTIVE)
		return 1;
	else
		return 0;
}


/*
 * Informs the client that the user is now officially registered or not
 * registered on the specified server.
 */
void  server_is_registered (int refnum, const char *itsname, const char *ourname)
{
	Server *s;
	int	winref;

	if (!(s = get_server(refnum)))
		return;

	set_server_status(refnum, SERVER_SYNCING);

	accept_server_nickname(refnum, ourname);
	set_server_itsname(refnum, itsname);

	if ((winref = get_winref_by_servref(refnum)) != -1)
		set_mask_by_winref(winref, new_server_lastlog_mask);

	reinstate_user_modes();
	userhostbase(from_server, NULL, got_my_userhost, 1);
	isonbase(from_server, NULL, NULL);

	if (never_connected)
	{
		never_connected = 0;
		permit_status_update(1);

		if (!ircrc_loaded)
			load_ircrc();

		if (default_channel)
		{
			e_channel("JOIN", default_channel, empty_string);
			new_free(&default_channel);
		}
	}
	if (get_server_away(refnum))
		set_server_away(from_server, get_server_away(from_server));

	update_all_status();
	do_hook(CONNECT_LIST, "%s %d %s", get_server_name(refnum), 
					get_server_port(refnum), 
					get_server_itsname(from_server));
	window_check_channels();
	set_server_status(refnum, SERVER_ACTIVE);
}

void	server_is_unregistered (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	destroy_005(refnum);
	set_server_status(refnum, SERVER_EOF);
}

int	is_server_active (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return 0;

	if (get_server_status(refnum) == SERVER_ACTIVE)
		return 1;
	return 0;
}

BUILT_IN_COMMAND(disconnectcmd)
{
	char	*server;
	const char *message;
	int	i;
	int	recon = strcmp(command, "DISCONNECT");

	if (!(server = next_arg(args, &args)))
		i = get_window_server(0);
	else
	{
		if ((i = str_to_servref(server)) == NOSERV)
		{
			say("No such server!");
			return;
		}
	}

	if (get_server(i))
	{
		if (args && *args)
			message = args;
		else if (recon)
			message = "Reconnecting";
		else
			message = "Disconnecting";

		say("Disconnecting from server %s", get_server_itsname(i));
		close_server(i, message);
		update_all_status();
	}

	if (!connected_to_server)
                if (do_hook(DISCONNECT_LIST, "Disconnected by user request"))
			say("You are not connected to a server, use /SERVER to connect.");

	if (recon)
	{
		set_server_status(i, SERVER_RECONNECT);
		say("Reconnecting to server %s", get_server_itsname(i));
	}
} 

BUILT_IN_COMMAND(reconnectcmd)
{
	disconnectcmd(command, args, subargs);
}

/* PORTS */
static void    set_server_port (int refnum, int port)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	s->port = port;
}

/* get_server_port: Returns the connection port for the given server index */
int	get_server_port (int refnum)
{
	Server *s;
	char	p_port[12];

	if (!(s = get_server(refnum)))
		return 0;

	if (!inet_ntostr((SA *)&s->remote_sockname, NULL, 0, p_port, 12, 0))
		return atol(p_port);

	return s->port;
}

int	get_server_local_port (int refnum)
{
	Server *s;
	char	p_port[12];

	if (!(s = get_server(refnum)))
		return 0;

	if (!inet_ntostr((SA *)&s->local_sockname, NULL, 0, p_port, 12, 0))
		return atol(p_port);

	return 0;
}

SS	get_server_remote_addr (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
	    panic("Refnum %d isn't valid in get_server_remote_addr", refnum);

	return s->remote_sockname;
}

SS	get_server_local_addr (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		panic("Refnum %d isn't valid in get_server_local_addr", refnum);

	return s->local_sockname;
}

SS	get_server_uh_addr (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		panic("Refnum %d isn't valid in get_server_uh_addr", refnum);

	return s->uh_addr;
}

/* USERHOST */
static void	set_server_userhost (int refnum, const char *uh)
{
	Server *s;
	char *host;

	if (!(s = get_server(refnum)))
		return;

	if (!(host = strchr(uh, '@')))
	{
		yell("Cannot set your userhost to [%s] because it does not"
		      "contain a @ character!", uh);
		return;
	}

	malloc_strcpy(&s->userhost, uh);

	/* Ack!  Oh well, it's for DCC. */
	FAMILY(s->uh_addr) = AF_INET;
	if (inet_strton(host + 1, zero, (SA *)&s->uh_addr, 0))
		yell("Ack.  The server says your userhost is [%s] and "
		     "I can't figure out the IPv4 address of that host! "
		     "You won't be able to use /SET DCC_USE_GATEWAY_ADDR ON "
		     "with this server connection!", host + 1);
}

/*
 * get_server_userhost: return the userhost for this connection to server
 */
const char	*get_server_userhost (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)) || !s->userhost)
		return get_userhost();

	return s->userhost;
}


/* COOKIES */
void	use_server_cookie (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	if (s->cookie)
		send_to_aserver(refnum, "COOKIE %s", s->cookie);
}


/* NICKNAMES */
/*
 * get_server_nickname: returns the current nickname for the given server
 * index 
 */
const char	*get_server_nickname (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return "<invalid server>";

	if (s->nickname)
		return s->nickname;

	return "<not registered yet>";
}

int	is_me (int refnum, const char *nick)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return 0;

	if (s->nickname && nick)
		return !my_stricmp(nick, s->nickname);

	return 0;
}



/*
 * This is the function to attempt to make a nickname change.  You
 * cannot send the NICK command directly to the server: you must call
 * this function.  This function makes sure that the neccesary variables
 * are set so that if the NICK command fails, a sane action can be taken.
 *
 * If ``nick'' is NULL, then this function just tells the server what
 * we're trying to change our nickname to.  If we're not trying to change
 * our nickname, then this function does nothing.
 */
void	change_server_nickname (int refnum, const char *nick)
{
	Server *s;
	char *	n;
	const char *id;

	if (!(s = get_server(refnum)))
		return;			/* Uh, no. */

	s->resetting_nickname = 0;
	if (nick)
	{
		n = LOCAL_COPY(nick);

		id = get_server_unique_id(refnum);
		if ((id && my_stricmp(n, id)) && strcmp(n, "0"))
		{
		    if (!(n = check_nickname(n, 1)))
		    {
			if (is_server_registered(refnum))
			    say("Cannot change nickname: '%s' is invalid",
					nick);
			else
			    reset_nickname(refnum);
			return;	
		    }
		}

		malloc_strcpy(&s->d_nickname, n);
		malloc_strcpy(&s->s_nickname, n);
	}

	if (s->s_nickname)
		send_to_aserver(refnum, "NICK %s", s->s_nickname);
}

const char *	get_pending_nickname (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return NULL;

	return s->s_nickname;
}

void	accept_server_nickname (int refnum, const char *nick)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	malloc_strcpy(&s->nickname, nick);
	malloc_strcpy(&s->d_nickname, nick);
	new_free(&s->s_nickname);
	s->fudge_factor = 0;

	if (refnum == primary_server)
		strlcpy(nickname, nick, sizeof nickname);

	update_all_status();
}

/* 
 * This will generate up to 18 nicknames plus the 9-length(nickname)
 * that are unique but still have some semblance of the original.
 * This is intended to allow the user to get signed back on to
 * irc after a nick collision without their having to manually
 * type a new nick every time..
 * 
 * The func will try to make an intelligent guess as to when it is
 * out of guesses, and if it ever gets to that point, it will do the
 * manually-ask-you-for-a-new-nickname thing.
 */
void 	fudge_nickname (int refnum)
{
	Server *s;
const	char	*nicklen_005;
	int	nicklen;
	char 	l_nickname[NICKNAME_LEN + 1];

	if (!(s = get_server(refnum)))
		return;			/* Uh, no. */

	/*
	 * If we got here because the user did a /NICK command, and
	 * the nick they chose doesnt exist, then we just dont do anything,
	 * we just cancel the pending action and give up.
	 */
	if (s->nickname_pending)
	{
		set_server_nickname_pending(refnum, 0);
		new_free(&s->s_nickname);
		return;
	}

	/*
	 * Ok.  So we're not doing a /NICK command, so we need to see
	 * if maybe we're doing some other type of NICK change.
	 */
	if (s->s_nickname)
		strlcpy(l_nickname, s->s_nickname, sizeof l_nickname);
	else if (s->nickname)
		strlcpy(l_nickname, s->nickname, sizeof l_nickname);
	else
		strlcpy(l_nickname, nickname, sizeof l_nickname);


	if (s->fudge_factor < strlen(l_nickname))
		s->fudge_factor = strlen(l_nickname);
	else
	{
		if (++s->fudge_factor == 17)
		{
			/* give up... */
			reset_nickname(refnum);
			s->fudge_factor = 0;
			return;
		}
	}

	/* 
	 * Process of fudging a nickname:
	 * If the nickname length is less then 9, add an underscore.
	 */
	nicklen_005 = get_server_005(refnum, "NICKLEN");
	nicklen = nicklen_005 ? atol(nicklen_005) : 9;
	nicklen = nicklen >= 0 ? nicklen : 9;

	if (strlen(l_nickname) < (size_t)nicklen)
		strlcat(l_nickname, "_", sizeof l_nickname);

	/* 
	 * The nickname is 9 characters long. roll the nickname
	 */
	else
	{
		char tmp = l_nickname[nicklen-1];
		int foo;
		for (foo = nicklen-1; foo>0; foo--)
			l_nickname[foo] = l_nickname[foo-1];
		l_nickname[0] = tmp;
	}

	/*
	 * This is the degenerate case
	 */
	if (strspn(l_nickname, "_") >= (size_t)nicklen)
	{
		reset_nickname(refnum);
		return;
	}

	change_server_nickname(refnum, l_nickname);
}


/*
 * -- Callback function
 */
void 	nickname_sendline (char *data, char *nick)
{
	int	new_server;

	new_server = str_to_servref(data);
	change_server_nickname(new_server, nick);
}

/*
 * reset_nickname: when the server reports that the selected nickname is not
 * a good one, it gets reset here. 
 * -- Called by more than one place
 */
void 	reset_nickname (int refnum)
{
	Server *s;
	char	server_num[10];
	char *	old_pending = NULL;

	if (!(s = get_server(refnum)))
		return; 		/* Don't repeat the reset */

	s->resetting_nickname = 1;
	if (s->s_nickname)
		old_pending = LOCAL_COPY(s->s_nickname);

	do_hook(NEW_NICKNAME_LIST, "%d %s %s", refnum, 
			s->nickname ? s->nickname : "*", 
			s->s_nickname ? s->s_nickname : "*");

	if (!(s = get_server(refnum)))
		return;			/* Just in case the user punted */

	/* Did the user do a /NICK in the /ON NEW_NICKNAME ? */
	if (s->s_nickname == NULL || 
		(old_pending && !strcmp(old_pending, s->s_nickname)))
	{
	    say("You need to give me a new nickname before I can continue");
	    if (!dumb_mode)
	    {
		say("Please enter a new nickname");
		strlcpy(server_num, ltoa(refnum), sizeof server_num);
		add_wait_prompt("Nickname: ", nickname_sendline, server_num,
			WAIT_PROMPT_LINE, 1);
	    }
	}
	update_all_status();
}


/* REDIRECT STUFF */
int	check_server_redirect (int refnum, const char *who)
{
	Server *s;

	if (!who || !(s = get_server(refnum)) || !s->redirect)
		return 0;

	if (!strncmp(who, "***", 3) && !strcmp(who + 3, s->redirect))
	{
		set_server_redirect(refnum, NULL);
		if (!strcmp(who + 3, "0"))
			say("Server flush done.");
		return 1;
	}

	return 0;
}

const char *get_server_type (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return NULL;

	if (get_server_try_ssl(refnum) == TRUE)
		return "IRC-SSL";
	else
		return "IRC";
}

/*****************************************************************************/
#define SET_IATTRIBUTE(param, member) \
void	set_server_ ## member (int servref, int param )	\
{							\
	Server *s;					\
							\
	if (!(s = get_server(servref)))			\
		return;					\
							\
	s-> member = param;				\
}

#define GET_IATTRIBUTE(member) \
int	get_server_ ## member (int servref)		\
{							\
	Server *s;					\
							\
	if (!(s = get_server(servref)))			\
		return -1;				\
							\
	return s-> member ;				\
}

#define SET_SATTRIBUTE(param, member) \
void	set_server_ ## member (int servref, const char * param )	\
{							\
	Server *s;					\
							\
	if (!(s = get_server(servref)))			\
		return;					\
							\
	malloc_strcpy(&s-> member , param);		\
}

#define GET_SATTRIBUTE(member, default)			\
const char *	get_server_ ## member (int servref ) \
{							\
	Server *s;					\
							\
	if (!(s = get_server(servref)))			\
		return default ;			\
							\
	if (s-> member )				\
		return s-> member ;			\
	else						\
		return default ;			\
}

#define IACCESSOR(param, member)		\
SET_IATTRIBUTE(param, member)			\
GET_IATTRIBUTE(member)

#define SACCESSOR(param, member, default)	\
SET_SATTRIBUTE(param, member)			\
GET_SATTRIBUTE(member, default)

IACCESSOR(v, doing_privmsg)
IACCESSOR(v, doing_notice)
IACCESSOR(v, doing_ctcp)
IACCESSOR(v, nickname_pending)
IACCESSOR(v, sent)
IACCESSOR(v, version)
IACCESSOR(v, line_length)
IACCESSOR(v, max_cached_chan_size)
IACCESSOR(v, ison_max)
IACCESSOR(v, userhost_max)
SACCESSOR(chan, invite_channel, NULL)
SACCESSOR(nick, last_notify_nick, NULL)
SACCESSOR(nick, joined_nick, NULL)
SACCESSOR(nick, public_nick, NULL)
SACCESSOR(nick, recv_nick, NULL)
SACCESSOR(nick, sent_nick, NULL)
SACCESSOR(text, sent_body, NULL)
SACCESSOR(nick, redirect, NULL)
SACCESSOR(group, group, "<default>")
SACCESSOR(message, quit_message, get_string_var(QUIT_MESSAGE_VAR))
SACCESSOR(cookie, cookie, NULL)
SACCESSOR(ver, version_string, NULL)
SACCESSOR(id, unique_id, NULL)

GET_IATTRIBUTE(status)
void	set_server_status (int refnum, int new_status)
{
	Server *s;
	int	old_status;
	const char *oldstr, *newstr;

	if (!(s = get_server(refnum)))
		return;

	if (new_status < 0 || new_status > SERVER_CLOSED)
		return;			/* Not acceptable */

	old_status = s->status;
	if (old_status < 0 || old_status > SERVER_CLOSED)
		oldstr = "UNKNOWN";

	s->status = new_status;

	newstr = server_states[new_status];
	oldstr = server_states[old_status];
	do_hook(SERVER_STATUS_LIST, "%d %s %s", refnum, oldstr, newstr);
}


GET_IATTRIBUTE(operator)
void	set_server_operator (int refnum, int flag)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	s->operator = flag;
	oper_command = 0;		/* No longer doing oper */
}

SACCESSOR(name, name, "<none>")
SET_SATTRIBUTE(name, itsname)
const char	*get_server_itsname (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return "<none>";

	if (s->itsname)
		return s->itsname;
	else
		return s->name;
}

int	get_server_protocol_state (int refnum)
{
	int	retval;

	retval = get_server_doing_ctcp(refnum);
	retval = retval << 8;

	retval += get_server_doing_notice(refnum);
	retval = retval << 8;

	retval += get_server_doing_privmsg(refnum);

	return retval;
}

void	set_server_protocol_state (int refnum, int state)
{
	int	val;

	val = state & 0xFF;
	set_server_doing_privmsg(refnum, val);
	state = state >> 8;

	val = state & 0xFF;
	set_server_doing_notice(refnum, val);
	state = state >> 8;

	val = state & 0xFF;
	set_server_doing_ctcp(refnum, val);
	state = state >> 8;
}

void	set_server_try_ssl (int refnum, int flag)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

#ifndef HAVE_SSL
	if (flag == TRUE)
		say("This server does not have SSL support.");
	flag = FALSE;
#endif
	s->try_ssl = flag;
}
GET_IATTRIBUTE(try_ssl)

void	set_server_ssl_enabled (int refnum, int flag)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

#ifndef HAVE_SSL
	if (flag == TRUE)
		say("This server does not have SSL support.");
	flag = FALSE;
	s->try_ssl = flag;
#endif
	s->ssl_enabled = flag;
}
GET_IATTRIBUTE(ssl_enabled)


/*****/
/* WAIT STUFF */
/*
 * This isnt a command, its used by the wait command.  Since its extern,
 * and it doesnt use anything static in this file, im sure it doesnt
 * belong here.
 */
void 	server_hard_wait (int i)
{
	Server *s;
	int	proto, old_from_server;
	char	reason[1024];

	if (!(s = get_server(i)))
		return;

	if (!is_server_registered(i))
		return;

	snprintf(reason, 1024, "WAIT on server %d", i);
	proto = get_server_protocol_state(i);
	old_from_server = from_server;

	s->waiting_out++;
	lock_stack_frame();
	send_to_aserver(i, "%s", lame_wait_nick);
	while ((s = get_server(i)) && (s->waiting_in < s->waiting_out))
		io(reason);

	set_server_protocol_state(i, proto);
	from_server = old_from_server;
}

void	server_passive_wait (int i, const char *stuff)
{
	Server *s;
	WaitCmd	*new_wait;

	if (!(s = get_server(i)))
		return;

	new_wait = (WaitCmd *)new_malloc(sizeof(WaitCmd));
	new_wait->stuff = malloc_strdup(stuff);
	new_wait->next = NULL;

	if (s->end_wait_list)
		s->end_wait_list->next = new_wait;
	s->end_wait_list = new_wait;
	if (!s->start_wait_list)
		s->start_wait_list = new_wait;

	send_to_aserver(i, "%s", wait_nick);
}

/*
 * How does this work?  Well, when we issue the /wait command it increments
 * a variable "waiting_out" which is the number of times that wait has been
 * caled so far.  If we get a wait token, we increase the waiting_in level
 * by one, and if the number of inbound waiting tokens is the same as the 
 * number of outbound tokens, then we are free to clear this stack frame
 * which will cause all of the pending waits to just fall out.
 */
int	check_server_wait (int refnum, const char *nick)
{
	Server	*s;

	if (!(s = get_server(refnum)))
		return 0;

	if ((s->waiting_out > s->waiting_in) && !strcmp(nick, lame_wait_nick))
	{
		s->waiting_in++;
		unlock_stack_frame();
	        return 1;
	}

	if (s->start_wait_list && !strcmp(nick, wait_nick))
	{
		WaitCmd *old = s->start_wait_list;

		s->start_wait_list = old->next;
		if (old->stuff)
		{
			call_lambda_command("WAIT", old->stuff, empty_string);
			new_free(&old->stuff);
		}
		if (s->end_wait_list == old)
			s->end_wait_list = NULL;
		new_free((char **)&old);
		return 1;
	}
	return 0;
}

/****** FUNNY STUFF ******/
IACCESSOR(v, funny_min)
IACCESSOR(v, funny_max)
IACCESSOR(v, funny_flags)
SACCESSOR(match, funny_match, NULL)

void	set_server_funny_stuff (int refnum, int min, int max, int flags, const char *stuff)
{
	set_server_funny_min(refnum, min);
	set_server_funny_max(refnum, max);
	set_server_funny_flags(refnum, flags);
	set_server_funny_match(refnum, stuff);
}

/*****************************************************************************/
static void	add_server_altname (int refnum, char *altname)
{
	Server *s;
	char *v;

	if (!(s = get_server(refnum)))
		return;

	v = malloc_strdup(altname);
	add_to_bucket(s->altnames, v, NULL);
}

static void	reset_server_altnames (int refnum, char *new_altnames)
{
	Server *s;
	int	i;
	char *	value;

	if (!(s = get_server(refnum)))
		return;

	for (i = 0; i < s->altnames->numitems; i++)
		new_free(&s->altnames->list[i].name);

	s->altnames->numitems = 0;

	while ((value = new_next_arg(new_altnames, &new_altnames)))
		add_server_altname(refnum, value);
}

static char *	get_server_altnames (int refnum)
{
	Server *s;
	char *	retval = NULL;
	size_t	clue = 0;
	int	i;

	if (!(s = get_server(refnum)))
		return NULL;

	for (i = 0; i < s->altnames->numitems; i++)
		malloc_strcat_word_c(&retval, space, s->altnames->list[i].name, &clue);

	return retval;
}

/*****************************************************************************/

/* 005 STUFF */

void make_005 (int refnum)
{
	Server *s;

	if (!(s = get_server(refnum)))
		return;

	s->a005.list = NULL;
	s->a005.max = 0;
	s->a005.total_max = 0;
	s->a005.func = (alist_func)strncmp;
	s->a005.hash = HASH_SENSITIVE; /* One way to deal with rfc2812 */
}

static void destroy_a_005 (A005_item *item)
{
	if (item) {
		new_free(&((*item).name));
		new_free(&((*item).value));
		new_free(&item);
	}
}

void destroy_005 (int refnum)
{
	Server *s;
	A005_item *new_i;

	if (!(s = get_server(refnum)))
		return;

	while ((new_i = (A005_item*)array_pop((array*)(&s->a005), 0)))
		destroy_a_005(new_i);
	s->a005.max = 0;
	s->a005.total_max = 0;
	new_free(&s->a005.list);
}

static GET_ARRAY_NAMES_FUNCTION(get_server_005s, (__FROMSERV->a005))

const char* get_server_005 (int refnum, const char *setting)
{
	Server *s;
	A005_item *item;
	int cnt, loc;

	if (!(s = get_server(refnum)))
		return NULL;
	item = (A005_item*)find_array_item((array*)(&s->a005), setting, &cnt, &loc);
	if (0 > cnt)
		return ((*item).value);
	else
		return NULL;
}

/* value should be null pointer or empty to clear. */
void set_server_005 (int refnum, char *setting, const char *value)
{
	Server *s;
	A005_item *new_005;
	int	destroy = (!value || !*value);

	if (!(s = get_server(refnum)))
		return;

	new_005 = (A005_item*)array_lookup((array*)(&s->a005), setting, 0, destroy);

	if (destroy) {
		if (new_005 && !strcmp(setting, (*new_005).name))
			destroy_a_005(new_005);
	} else if (new_005 && !strcmp(setting, (*new_005).name)) {
		malloc_strcpy(&((*new_005).value), value);
	} else {
		new_005 = (A005_item *)new_malloc(sizeof(A005_item));
		(*new_005).name = malloc_strdup(setting);
		(*new_005).value = malloc_strdup(value);
		add_to_array((array*)(&s->a005), (array_item*)new_005);
	}
}


/* Used by function_serverctl */
/*
 * $serverctl(REFNUM server-desc)
 * $serverctl(MAX)
 * $serverctl(GET 0 [LIST])
 * $serverctl(SET 0 [ITEM] [VALUE])
 * $serverctl(MATCH [pattern])
 * $serverctl(PMATCH [pattern])
 * $serverctl(GMATCH [group])
 *
 * [LIST] and [ITEM] are one of the following:
 *	NAME		"ourname" for the server connection
 * 	ITSNAME		"itsname" for the server connection
 *	PASSWORD	The password we will use on connect
 *	PORT		The port we will use on connect
 *	GROUP		The group that this server belongs to
 *	NICKNAME	The nickname we will use on connect
 *	USERHOST	What the server thinks our userhost is.
 *	AWAY		The away message
 *	VERSION		The server's claimed version
 *	UMODE		Our user mode
 *	CONNECTED	Whether or not we are connected
 *	COOKIE		Our TS/4 cookie
 *	QUIT_MESSAGE	The quit message we will use next.
 *	SSL		Whether this server is SSL-enabled or not.
 *      005             Individual PROTOCTL elements.
 *      005s            The full list of PROTOCTL elements.
 *	ALIAS		An alternate server designation
 *			(SETting ALIAS adds a new alternate designation) 
 *	ALIASES		All of the alternate server designations
 *			(SETting ALIASES replaces all alternate designations)
 *			(This is the only way to delete a designation)
 */
char 	*serverctl 	(char *input)
{
	int	refnum, num, len;
	char	*listc, *listc1;
	const char *ret;

	GET_STR_ARG(listc, input);
	len = strlen(listc);
	if (!my_strnicmp(listc, "ADD", len)) {
	} else if (!my_strnicmp(listc, "DELETE", len)) {
	} else if (!my_strnicmp(listc, "REFNUM", len)) {
		char *server;

		GET_STR_ARG(server, input);
		refnum = str_to_servref(server);
		if (refnum != NOSERV)
			RETURN_STR(server);
		RETURN_EMPTY;
	} else if (!my_strnicmp(listc, "GET", len)) {
		GET_INT_ARG(refnum, input);
		if (!get_server(refnum))
			RETURN_EMPTY;

		GET_STR_ARG(listc, input);
		len = strlen(listc);
		if (!my_strnicmp(listc, "AWAY", len)) {
			ret = get_server_away(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "MAXCACHESIZE", len)) {
			num = get_server_max_cached_chan_size(refnum);
			RETURN_INT(num);
		} else if (!my_strnicmp(listc, "MAXISON", len)) {
			num = get_server_ison_max(refnum);
			RETURN_INT(num);
		} else if (!my_strnicmp(listc, "MAXUSERHOST", len)) {
			num = get_server_userhost_max(refnum);
			RETURN_INT(num);
		} else if (!my_strnicmp(listc, "CONNECTED", len)) {
			num = is_server_registered(refnum);
			RETURN_INT(num);
		} else if (!my_strnicmp(listc, "COOKIE", len)) {
			ret = get_server_cookie(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "GROUP", len)) {
			ret = get_server_group(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "ITSNAME", len)) {
			ret = get_server_itsname(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "NAME", len)) {
			ret = get_server_name(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "NICKNAME", len)) {
			ret = get_server_nickname(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "PASSWORD", len)) {
			ret = get_server_password(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "PORT", len)) {
			num = get_server_port(refnum);
			RETURN_INT(num);
		} else if (!my_strnicmp(listc, "QUIT_MESSAGE", len)) {
			if (!(ret = get_server_quit_message(refnum)))
				ret = empty_string;
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "SSL", len)) {
			num = get_server_try_ssl(refnum);
			RETURN_INT(num);
		} else if (!my_strnicmp(listc, "UMODE", len)) {
			ret = get_umode(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "UNIQUE_ID", len)) {
			ret = get_server_unique_id(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "USERHOST", len)) {
			ret = get_server_userhost(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "VERSION", len)) {
			ret = get_server_version_string(refnum);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "005", len)) {
			GET_STR_ARG(listc1, input);
			ret = get_server_005(refnum, listc1);
			RETURN_STR(ret);
		} else if (!my_strnicmp(listc, "005s", len)) {
			int ofs = from_server;
			char *	retval;

			from_server = refnum;
			retval = get_server_005s(input);
			from_server = ofs;
			RETURN_MSTR(retval);
		} else if (!my_strnicmp(listc, "STATUS", len)) {
			RETURN_STR(server_states[get_server_status(refnum)]);
		} else if (!my_strnicmp(listc, "ALTNAME", len)) {
			RETURN_MSTR(get_server_altnames(refnum));
		} else if (!my_strnicmp(listc, "ALTNAMES", len)) {
			RETURN_MSTR(get_server_altnames(refnum));
		} else if (!my_strnicmp(listc, "ADDRFAMILY", len)) {
			SS a;
			SA *x;

			a = get_server_remote_addr(refnum);
			x = (SA *)&a;
			if (x->sa_family == AF_INET)
				RETURN_STR("ipv4");
			else if (x->sa_family == AF_INET6)
				RETURN_STR("ipv6");
			else if (x->sa_family == AF_UNIX)
				RETURN_STR("unix");
			else
				RETURN_STR("unknown");
		}
	} else if (!my_strnicmp(listc, "SET", len)) {
		GET_INT_ARG(refnum, input);
		if (!get_server(refnum))
			RETURN_EMPTY;

		GET_STR_ARG(listc, input);
		len = strlen(listc);
		if (!my_strnicmp(listc, "AWAY", len)) {
			set_server_away(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "MAXCACHESIZE", len)) {
			int	size;
			GET_INT_ARG(size, input);
			set_server_max_cached_chan_size(refnum, size);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "MAXISON", len)) {
			int	size;
			GET_INT_ARG(size, input);
			set_server_ison_max(refnum, size);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "MAXUSERHOST", len)) {
			int	size;
			GET_INT_ARG(size, input);
			set_server_userhost_max(refnum, size);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "CONNECTED", len)) {
			RETURN_EMPTY;		/* Read only. */
		} else if (!my_strnicmp(listc, "COOKIE", len)) {
			set_server_cookie(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "GROUP", len)) {
			set_server_group(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "ITSNAME", len)) {
			set_server_itsname(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "NAME", len)) {
			set_server_name(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "NICKNAME", len)) {
			change_server_nickname(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "PASSWORD", len)) {
			set_server_password(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "PORT", len)) {
			int port;

			GET_INT_ARG(port, input);
			set_server_port(refnum, port);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "PRIMARY", len)) {
			primary_server = refnum;
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "QUIT_MESSAGE", len)) {
			set_server_quit_message(refnum, input);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "SSL", len)) {
			int value;

			GET_INT_ARG(value, input);
			set_server_try_ssl(refnum, value);
			RETURN_INT(1);
		} else if (!my_strnicmp(listc, "UMODE", len)) {
			RETURN_EMPTY;		/* Read only for now */
		} else if (!my_strnicmp(listc, "UNIQUE_ID", len)) {
			set_server_unique_id(refnum, input);
		} else if (!my_strnicmp(listc, "USERHOST", len)) {
			set_server_userhost(refnum, input);
		} else if (!my_strnicmp(listc, "VERSION", len)) {
			set_server_version_string(refnum, input);
		} else if (!my_strnicmp(listc, "005", len)) {
			GET_STR_ARG(listc1, input);
			set_server_005(refnum, listc1, input);
			RETURN_INT(!!*input);
		} else if (!my_strnicmp(listc, "ALTNAME", len)) {
			add_server_altname(refnum, input);
		} else if (!my_strnicmp(listc, "ALTNAMES", len)) {
			reset_server_altnames(refnum, input);
		}
	} else if (!my_strnicmp(listc, "OMATCH", len)) {
		int	i;
		size_t	clue = 0;
		char *retval = NULL;

		for (i = 0; i < number_of_servers; i++)
			if (wild_match(input, get_server_name(i)))
				malloc_strcat_wordlist_c(&retval, space, ltoa(i), &clue);
		RETURN_MSTR(retval);
	} else if (!my_strnicmp(listc, "IMATCH", len)) {
		int	i;
		size_t	clue = 0;
		char *retval = NULL;

		for (i = 0; i < number_of_servers; i++)
			if (wild_match(input, get_server_itsname(i)))
				malloc_strcat_wordlist_c(&retval, space, ltoa(i), &clue);
		RETURN_MSTR(retval);
	} else if (!my_strnicmp(listc, "GMATCH", len)) {
		int	i;
		size_t	clue = 0;
		char *retval = NULL;

		for (i = 0; i < number_of_servers; i++)
			if (wild_match(input, get_server_group(i)))
				malloc_strcat_wordlist_c(&retval, space, ltoa(i), &clue);
		RETURN_MSTR(retval);
	} else if (!my_strnicmp(listc, "MAX", len)) {
		RETURN_INT(number_of_servers);
	} else
		RETURN_EMPTY;

	RETURN_EMPTY;
}

/*
 * got_my_userhost -- callback function, XXXX doesnt belong here
 * XXX Really does not belong here. 
 */
void 	got_my_userhost (int refnum, UserhostItem *item, const char *nick, const char *stuff)
{
	char *freeme;

	freeme = malloc_strdup3(item->user, "@", item->host);
	set_server_userhost(refnum, freeme);
	new_free(&freeme);
}

int	server_more_addrs (int refnum)
{
	Server	*s;

	if (!(s = get_server(refnum)))
		return 0;

	if (s->next_addr)
		return 1;
	else
		return 0;
}

