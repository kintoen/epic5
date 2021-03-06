/*
 * names.h: Header for names.c
 *
 * Copyright 1997 EPIC Software Labs
 * See the COPYRIGHT file, or do a HELP IRCII COPYRIGHT 
 */

#ifndef __names_h__
#define __names_h__

#ifdef Char
#undef Char
#endif
#define Char const char

	void	add_channel		(Char *, int); 
	void	remove_channel		(Char *, int);
	void	add_to_channel		(Char *, Char *, int, int, int, int, int);
	void	add_userhost_to_channel	(Char *, Char *, int, Char *);
	void	remove_from_channel	(Char *, Char *, int);
	void	rename_nick		(Char *, Char *, int);
	int	im_on_channel		(Char *, int);
	int	is_on_channel		(Char *, Char *);
	int	is_chanop		(Char *, Char *);
	int	is_chanvoice		(Char *, Char *);
	int	is_halfop		(Char *, Char *);
	int	number_on_channel	(Char *, int);
	char *	create_nick_list	(Char *, int);
	char *	create_chops_list	(Char *, int);
	char *	create_nochops_list	(Char *, int);
	void	update_channel_mode	(Char *, Char *);
	Char *	get_channel_key		(Char *, int);
	char *	get_channel_mode	(Char *, int);
	int	is_channel_private	(Char *, int);
	int	is_channel_nomsgs	(Char *, int);
	void	list_channels		(void);
	void	switch_channels		(char, char *);
	void	change_server_channels	(int, int);
	void	destroy_server_channels	(int);
	void	reconnect_all_channels	(void);
	Char *	what_channel		(Char *);
	Char *	walk_channels		(int, Char *);
	Char *	fetch_userhost		(int, Char *);
	int	get_channel_oper	(Char *, int);
	int	get_channel_voice	(Char *, int);
	int     get_channel_halfop	(Char *, int);
	int     chanmodetype		(char);
	int	get_channel_winref	(Char *, int);
	void	set_channel_window	(Char *, int, int, int);
	char *	create_channel_list	(int);
	void	channel_server_delete	(int);
	void	save_channels		(int);
	void	remove_from_mode_list	(Char *);
	int	auto_rejoin_callback	(void *);
	void	reassign_window_channels (int);
	void	move_channel_to_window	(Char *, int, int, int);
	char *	scan_channel		(char *);
	Char *	check_channel_type	(Char *);
	int	channel_is_syncing	(Char *, int);
	void	channel_not_waiting	(Char *, int); 
	void	channel_check_windows	(void);
	void	cant_join_channel	(Char *, int);
	Char *	window_current_channel	(int, int);
	char *	window_all_channels	(int, int);

#endif /* _NAMES_H_ */
