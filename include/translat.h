/*
 * Global stuff for translation tables.
 *
 * Tomten, tomten@solace.hsh.se / tomten@lysator.liu.se
 *
 * @(#)$Id$
 */

#ifndef __translat_h_
# define __translat_h_

extern	void	set_translation (char *);
extern	unsigned char	transToClient[256];
extern	unsigned char	transFromClient[256];
extern	char	translation;

#endif /* __translat_h_ */
