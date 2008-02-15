/* $EPIC: screen.c,v 1.129 2008/01/22 04:03:40 jnelson Exp $ */
/*
 * screen.c
 *
 * Copyright (c) 1993-1996 Matthew Green.
 * Copyright � 1998 J. Kean Johnston, used with permission
 * Copyright � 1997, 2005 EPIC Software Labs.
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
/*
 * This file includes major work contributed by FireClown (J. Kean Johnston), 
 * and I am indebted to him for the work he has graciously donated to the 
 * project.  Without his contributions, EPIC's robust handling of colors and 
 * escape sequences would never have been possible.
 */

#define __need_putchar_x__
#include "irc.h"
#include "alias.h"
#include "clock.h"
#include "exec.h"
#include "screen.h"
#include "window.h"
#include "output.h"
#include "vars.h"
#include "server.h"
#include "list.h"
#include "term.h"
#include "names.h"
#include "ircaux.h"
#include "input.h"
#include "log.h"
#include "hook.h"
#include "dcc.h"
#include "status.h"
#include "commands.h"
#include "parse.h"
#include "newio.h"

#define CURRENT_WSERV_VERSION	4

/*
 * When some code wants to override the default lastlog level, and needs
 * to have some output go into some explicit window (such as for /xecho -w),
 * then while to_window is set to some window, *ALL* output goes to that
 * window.  Dont forget to reset it to NULL when youre done!  ;-)
 */
	Window	*to_window;

/*
 * When all else fails, this is the screen that is attached to the controlling
 * terminal, and we know *that* screen will always be there.
 */
	Screen	*main_screen;

/*
 * This is the screen in which we last handled an input event.  This takes
 * care of the input duties that "current_screen" used to handle.
 */
	Screen	*last_input_screen;

/*
 * This is used to set the default output device for tputs_x().  This takes
 * care of the output duties that "current_screen" used to handle.  Since
 * the input screen and output screen are independant, it isnt impossible 
 * for a command in one screen to cause output in another.
 */
	Screen	*output_screen;

/*
 * The list of all the screens we're handling.  Under most cases, there's 
 * only one screen on the list, "main_screen".
 */
	Screen	*screen_list = NULL;

/*
 * How things output to the display get mangled (set via /set mangle_display)
 */
	int	display_line_mangler = 0;


/* * * * * * * * * * * * * OUTPUT CHAIN * * * * * * * * * * * * * * * * * * *
 * The front-end api to output stuff to windows is:
 *
 * 1) Set the window, either directly or indirectly:
 *     a) Directly with		l = message_setall(winref, target, level);
 *     b) Indirectly with	l = message_from(target, level);
 * 2) Call an output routine:
 *	say(), output(), yell(), put_it(), put_echo(), etc.
 * 3) Reset the window:
 *     b) Indirectly with	pop_message_from(l);
 *
 * This file implements the middle part of the "ircII window", everything
 * that sits behind the say/output/yell/put_it/put_echo functions, and in
 * front of the low-level terminal stuff.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
static void 	do_screens	(int fd);
static int 	rite 		(Window *, const unsigned char *);
static void 	scroll_window   (Window *);
static void 	add_to_window	(Window *, const unsigned char *);
static	int	ok_to_output	(Window *);
static ssize_t read_esc_seq     (const unsigned char *, void *, int *);
static ssize_t read_color_seq   (const unsigned char *, void *d, int);

/*
 * "Attributes" were an invention for epic5, and the general idea was
 * to handle all character markups (bold/color/reverse/etc) not as toggle
 * switches, but as absolute settings, handled inline.
 *
 * The function normalize_string() converts all of the character markup
 * toggle characters (^B, ^C, ^V, etc) into Attributes.  Nothing further
 * in the output chain needs to know about highlight toggle chars.
 *
 * Attributes are expressed in two formats, an umarshalled form and a 
 * marshalled form.  The unmarshalled form is (struct attributes) and is
 * a struct of 9 eight-bit ints which hold toggle switches for which 
 * attributes are currently active.  The marshalled form is 5 bytes which
 * can be stored in a C string.
 *
 * An unmarshalled attribute can be injected into a C string using the 
 * display_attributes() function.  A marshalled attribute can be extracted
 * from a C string using read_attributes().
 *
 * The logical_attributes() function will marshall an attribute into the
 * logical (un-normalized) equivalents.   The ignore_attributes() function
 * is a function that essentially ignores/strips attribute changes.
 */
struct 	attributes {
	char	reverse;
	char	bold;
	char	blink;
	char	underline;
	char	altchar;
	char	color_fg;
	char	color_bg;
	char	fg_color;
	char	bg_color;
};
typedef struct attributes Attribute;

const unsigned char *all_off (void)
{
#ifdef NO_CHEATING
	Attribute 	a;
	static	unsigned char	retval[6];

	a->reverse = a->bold = a->blink = a->underline = a->altchar = 0;
	a->color_fg = a->fg_color = a->color_bg = a->bg_color = 0;
	display_attributes(retval, NULL, &a);
	return retval;
#else
	static	char	retval[6];
	retval[0] = '\006';
	retval[1] = retval[2] = retval[3] = retval[4] = 0x80;
	retval[5] = 0;
	return retval;
#endif
}

/*
 * These are "attribute changer" functions.  They work like this:
 *	output		An output (string) buffer to write the change to
 *	old_a		The previous (Attribute *)
 *	new_a		The new (Attribute *)
 * Each function should write any changes between "old_a" and "new_a" to 
 * output in whatever way is appropriate, and should copy new_a to old_a 
 * before returning.  The special case is when "old_a" is NULL, which 
 * should be treated as an explicit "all off" before handling "a".
 */
static size_t	display_attributes (unsigned char *output, Attribute *old_a, Attribute *a)
{
	unsigned char	val1 = 0x80;
	unsigned char	val2 = 0x80;
	unsigned char	val3 = 0x80;
	unsigned char	val4 = 0x80;

	if (a->reverse)		val1 |= 0x01;
	if (a->bold)		val1 |= 0x02;
	if (a->blink)		val1 |= 0x04;
	if (a->underline)	val1 |= 0x08;
	if (a->altchar)		val1 |= 0x10;

	if (a->color_fg) {	val2 |= 0x01; val3 |= a->fg_color; }
	if (a->color_bg) {	val2 |= 0x02; val4 |= a->bg_color; }

	output[0] = '\006';
	output[1] = val1;
	output[2] = val2;
	output[3] = val3;
	output[4] = val4;
	output[5] = 0;

	old_a = a;
	return 5;
}
 
/* Put into 'output', logical characters so end result is 'a' */
static size_t	logic_attributes (unsigned char *output, Attribute *old_a, Attribute *a)
{
	char	*str = output;
	size_t	count = 0;
	Attribute dummy;

	if (old_a == NULL)
	{
		old_a = &dummy;
		old_a->reverse = old_a->bold = old_a->blink = 0;
		old_a->underline = old_a->altchar = 0;
		old_a->color_fg = old_a->fg_color = 0;
		old_a->color_bg = old_a->bg_color = 0;
		*str++ = ALL_OFF, count++;
	}

	if (a->reverse == 0 && a->bold == 0 && a->blink == 0 &&
	    a->underline == 0 && a->altchar == 0 && a->fg_color == 0 &&
	    a->bg_color == 0 && a->color_bg == 0 && a->color_fg == 0)
	{
	    if (old_a->reverse != 0 || old_a->bold != 0 || old_a->blink != 0 ||
		old_a->underline != 0 || old_a->altchar != 0 || old_a->fg_color != 0 ||
		old_a->bg_color != 0 || old_a->color_bg != 0 || old_a->color_fg != 0)
	    {
		*str++ = ALL_OFF;
		*old_a = *a;
		return 1;
	    }
	}

	/* Colors need to be set first, always */
	if (a->color_fg != old_a->color_fg || a->fg_color != old_a->fg_color)
	{
	    *str++ = '\003', count++;
	    if (a->color_fg)
	    {
		*str++ = '3', count++;
		*str++ = '0' + a->fg_color, count++;
	    }
	    else
	    {
		*str++ = '-', count++;
		*str++ = '1', count++;
	    }
	}
	if (a->color_bg != old_a->color_bg || a->bg_color != old_a->bg_color)
	{
	    if (!a->color_fg)
		*str++ = '\003', count++;
	    *str++ = ',', count++;
	    if (a->color_bg)
	    {
		*str++ = '4', count++;
		*str++ = '0' + a->bg_color, count++;
	    }
	    else
	    {
		*str++ = '-', count++;
		*str++ = '1', count++;
	    }
	}
	if (old_a->bold != a->bold)
		*str++ = BOLD_TOG, count++;
	if (old_a->blink != a->blink)
		*str++ = BLINK_TOG, count++;
	if (old_a->reverse != a->reverse)
		*str++ = REV_TOG, count++;
	if (old_a->underline != a->underline)
		*str++ = UND_TOG, count++;
	if (old_a->altchar != a->altchar)
		*str++ = ALT_TOG, count++;

	*old_a = *a;
	return count;
}

/* Suppress any attribute changes in the output */
static size_t	ignore_attributes (unsigned char *output, Attribute *old_a, Attribute *a)
{
	return 0;
}


/* Read an attribute marker from 'input', put results in 'a'. */
static int	read_attributes (const unsigned char *input, Attribute *a)
{
	if (!input)
		return -1;
	if (*input != '\006')
		return -1;
	if (!input[0] || !input[1] || !input[2] || !input[3] || !input[4])
		return -1;

	a->reverse = a->bold = a->blink = a->underline = a->altchar = 0;
	a->color_fg = a->fg_color = a->color_bg = a->bg_color = 0;

	input++;
	if (*input & 0x01)	a->reverse = 1;
	if (*input & 0x02)	a->bold = 1;
	if (*input & 0x04)	a->blink = 1;
	if (*input & 0x08)	a->underline = 1;
	if (*input & 0x10)	a->altchar = 1;

	input++;
	if (*input & 0x01) {	
		a->color_fg = 1; 
		a->fg_color = input[1] & 0x7F; 
	}
	if (*input & 0x02) {	
		a->color_bg = 1; 
		a->bg_color = input[2] & 0x7F; 
	}

	return 0;
}

/* Invoke all of the neccesary functions so output attributes reflect 'a'. */
static void	term_attribute (Attribute *a)
{
	term_all_off();
	if (a->reverse)		term_standout_on();
	if (a->bold)		term_bold_on();
	if (a->blink)		term_blink_on();
	if (a->underline)	term_underline_on();
	if (a->altchar)		term_altcharset_on();

	if (a->color_fg) {	if (a->fg_color > 7) abort(); 
				else term_set_foreground(a->fg_color); }
	if (a->color_bg) {	if (a->bg_color > 7) abort();
				else term_set_background(a->bg_color); }
}

/* * * * * * * * * * * * * COLOR SUPPORT * * * * * * * * * * * * * * * * */
/*
 * read_color_seq -- Parse out and count the length of a ^C color sequence
 * Arguments:
 *	start     - A string beginning with ^C that represents a color sequence
 *	d         - An (Attribute *) [or NULL] that shall be modified by the
 *		    color sequence.
 *	blinkbold - The value of /set bold_does_bright_blink
 * Return Value:
 *	The length of the ^C color sequence, such that (start + retval) is
 *	the first character that is not part of the ^C color sequence.
 *	In no case does the return value pass the string's terminating null.
 *
 * Note:
 *	Unlike some other clients, EPIC does not simply slurp up all digits 
 *	after a ^C sequence (either by calling strtol() or while (isdigit()),
 *	because some people put ^C sequences before legitimate output with 
 * 	numbers (like the time on your status bar).  This function is very
 *	careful only to consume the characters that represent a bona fide 
 *	^C code.  This means things like "^C49" resolve to "^C4" + "9"
 *
 * DO NOT USE ANY OTHER FUNCTION TO PARSE ^C CODES.  YOU HAVE BEEN WARNED!
 */
static ssize_t	read_color_seq (const unsigned char *start, void *d, int blinkbold)
{
	/* 
	 * The proper "attribute" color mapping is for each ^C lvalue.
	 * If the value is -1, then that is an illegal ^C lvalue.
	 */
	static	int	fore_conv[] = {
		 7,  0,  4,  2,  1,  1,  5,  3,		/*  0-7  */
		 3,  2,  6,  6,  4,  5,  0,  7,		/*  8-15 */
		 7, -1, -1, -1, -1, -1, -1, -1, 	/* 16-23 */
		-1, -1, -1, -1, -1, -1,  0,  1, 	/* 24-31 */
		 2,  3,  4,  5,  6,  7, -1, -1,		/* 32-39 */
		-1, -1, -1, -1, -1, -1, -1, -1,		/* 40-47 */
		-1, -1,  0,  1,  2,  3,  4,  5, 	/* 48-55 */
		 6,  7,	-1, -1, -1			/* 56-60 */
	};
	/* 
	 * The proper "attribute" color mapping is for each ^C rvalue.
	 * If the value is -1, then that is an illegal ^C rvalue.
	 */
	static	int	back_conv[] = {
		 7,  0,  4,  2,  1,  1,  5,  3,
		 3,  2,  6,  6,  4,  5,  0,  7,
		 7, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1,
		 0,  1,  2,  3,  4,  5,  6,  7, 
		-1, -1,  0,  1,  2,  3,  4,  5,
		 6,  7, -1, -1, -1
	};

	/*
	 * Some lval codes represent "bold" colors.  That actually reduces
	 * to ^C<non bold> + ^B, so that if you do ^B later, you get the
	 * <non bold> color.  This table indicates whether a ^C code 
	 * turns bold ON or OFF.  (Every color does one or the other)
	 */
	static	int	fore_bold_conv[] =  {
		1,  0,  0,  0,  1,  0,  0,  0,
		1,  1,  0,  1,  1,  1,  1,  0,
		1,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  1,  1,  1,  1,  1,  1,
		1,  1,  0,  0,  0
	};
	/*
	 * Some rval codes represent "blink" colors.  That actually reduces
	 * to ^C<non blink> + ^F, so that if you do ^F later, you get the
	 * <non blink> color.  This table indicates whether a ^C code 
	 * turns blink ON or OFF.  (Every color does one or the other)
	 */
	static	int	back_blink_conv[] = {
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  1,  1,  1,  1,  1,  1,
		1,  1,  0,  0,  0
	};
	/*
	 * If /set term_does_bright_blink is on, this will be used instead
	 * of back_blink_conv.  On an xterm, this will cause the background
	 * to be bold.
	 */
	static	int	back_bold_conv[] = {
		1,  0,  0,  0,  1,  0,  0,  0,
		1,  1,  0,  1,  1,  1,  1,  0,
		1,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  0,  1,  1,  1,  1,  1,  1,
		1,  1,  0,  0,  0
	};
	/*
	 * And switch between the two.
	 */
	int *	back_blinkbold_conv = blinkbold ? back_bold_conv : 
						  back_blink_conv;

	/* Local variables, of course */
	const 	unsigned char *	ptr = start;
		int		c1, c2;
		Attribute *	a;
		Attribute	ad;
		int		fg;
		int		val;
		int		noval;

        /* Reset all attributes to zero */
        ad.bold = ad.underline = ad.reverse = ad.blink = ad.altchar = 0;
        ad.color_fg = ad.color_bg = ad.fg_color = ad.bg_color = 0;

	/* Copy the inward attributes, if provided */
	a = (d) ? (Attribute *)d : &ad;

	/*
	 * If we're passed a non ^C code, dont do anything.
	 */
	if (*ptr != '\003')
		return 0;

	/*
	 * This is a one-or-two-time-through loop.  We find the maximum
	 * span that can compose a legit ^C sequence, then if the first
	 * nonvalid character is a comma, we grab the rhs of the code.
	 */
	for (fg = 1; ; fg = 0)
	{
		/*
		 * If its just a lonely old ^C, then its probably a terminator.
		 * Just skip over it and go on.
		 */
		ptr++;
		if (*ptr == 0)
		{
			if (fg)
				a->color_fg = a->fg_color = 0;
			a->color_bg = a->bg_color = 0;
			a->bold = a->blink = 0;
			return ptr - start;
		}

		/*
		 * Check for the very special case of a definite terminator.
		 * If the argument to ^C is -1, then we absolutely know that
		 * this ends the code without starting a new one
		 */
		/* XXX *cough* is 'ptr[1]' valid here? */
		else if (ptr[0] == '-' && ptr[1] == '1')
		{
			if (fg)
				a->color_fg = a->fg_color = 0;
			a->color_bg = a->bg_color = 0;
			a->bold = a->blink = 0;
			return (ptr + 2) - start;
		}

		/*
		 * Further checks against a lonely old naked ^C.
		 */
		else if (!isdigit(ptr[0]) && ptr[0] != ',')
		{
			if (fg)
				a->color_fg = a->fg_color = 0;
			a->color_bg = a->bg_color = 0;
			a->bold = a->blink = 0;
			return ptr - start;
		}


		/*
		 * Code certainly cant have more than two chars in it
		 */
		c1 = ptr[0];
		c2 = ptr[1];
		val = 0;
		noval = 0;

#define mkdigit(x) ((x) - '0')

		/* Our action depends on the char immediately after the ^C. */
		switch (c1)
		{
			/* These might take one or two characters */
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			{
			    if (c2 >= '0' && c2 <= '9')
			    {
				int	val1;
				int	val2;

			        ptr++;
				val1 = mkdigit(c1);
				val2 = mkdigit(c1) * 10 + mkdigit(c2);

				if (fg)
				{
					if (fore_conv[val2] == -1)
						val = val1;
					else
						val = val2, ptr++;
				}
				else
				{
					if (back_conv[val2] == -1)
						val = val1;
					else
						val = val2, ptr++;
				}
				break;
			    }

			    /* FALLTHROUGH */
			}

			/* These can only take one character */
			case '6':
			case '7':
			case '8':
			case '9':
			{
				ptr++;

				val = mkdigit(c1);
				break;
			}

			/*
			 * Y -> <stop> Y for any other nonnumeric Y
			 */
			default:
			{
				noval = 1;
				break;
			}
		}

		if (noval == 0)
		{
			if (fg)
			{
				a->color_fg = 1;
				a->bold = fore_bold_conv[val];
				a->fg_color = fore_conv[val];
			}
			else
			{
				a->color_bg = 1;
				a->blink = back_blinkbold_conv[val];
				a->bg_color = back_conv[val];
			}
		}

		if (fg && *ptr == ',')
			continue;
		break;
	}

	return ptr - start;
}

/**************************************************************************/
/*
 * read_esc_seq -- Parse out and count the length of an escape (ansi) sequence
 * Arguments:
 *	start     - A string beginning with ^C that represents a color sequence
 *	ptr_a     - An (Attribute *) [or NULL] that shall be modified by the
 *		    color sequence.
 *	nd_spaces - [OUTPUT] The number of ND_SPACES the sequence generates.
 * Return Value:
 *	The length of the escape (ansi) sequence, such that (start + retval) is
 *	the first character that is not part of the escape (ansi) sequence.
 *	In no case does the return value pass the string's terminating null.
 *
 * Note:
 *	EPIC supports the "Non-destructive space" escape sequence (^[[10C)
 *	which is used by ascii artists.  If this is used, then "nd_spaces" is
 *	set to the value, otherwise it is set to 0.
 *
 *	All escape sequences are parsed, but not all escape sequences are 
 *	honored.  If a dishonored escape sequence is encountered, then its
 *	length is counted, but 'ptr_a' will be unchanged.
 *
 * DO NOT USE ANY OTHER FUNCTION TO PARSE ESCAPES.  YOU HAVE BEEN WARNED!
 */
static ssize_t	read_esc_seq (const unsigned char *start, void *ptr_a, int *nd_spaces)
{
	Attribute *	a = NULL;
	Attribute 	safe_a;
	int 		args[10];
	int		nargs;
	unsigned char 	chr;
	const unsigned char *	str;
	ssize_t		len;

	if (ptr_a == NULL)
		a = &safe_a;
	else
		a = (Attribute *)ptr_a;

	*nd_spaces = 0;
	str = start;
	len = 0;

	switch ((chr = start[len]))
	{
	    /*
	     * These are two-character commands.  The second
	     * char is the argument.
	     */
	    case ('#') : case ('(') : case (')') :
	    case ('*') : case ('+') : case ('$') :
	    case ('@') :
	    {
		if (start[len+1] != 0)
			len++;
		break;
	    }

	    /*
	     * These are just single-character commands.
	     */
	    case ('7') : case ('8') : case ('=') :
	    case ('>') : case ('D') : case ('E') :
	    case ('F') : case ('H') : case ('M') :
	    case ('N') : case ('O') : case ('Z') :
	    case ('l') : case ('m') : case ('n') :
	    case ('o') : case ('|') : case ('}') :
	    case ('~') : case ('c') :
	    default:
	    {
		break;		/* Don't do anything */
	    }

	    /*
	     * Swallow up graphics sequences...
	     */
	    case ('G'):
	    {
		while ((chr = start[++len]) && chr != ':')
			;
		if (chr == 0)
			len--;
		break;
	    }

	    /*
	     * Not sure what this is, it's not supported by
	     * rxvt, but its supposed to end with an ESCape.
	     */
	    case ('P') :
	    {
		while ((chr = start[++len]) && chr != 033)
			;
		if (chr == 0)
			len--;
		break;
	    }

	    /*
	     * Strip out Xterm sequences
	     */
	    case (']') :
	    {
		while ((chr = start[++len]) && chr != 7)
			;
		if (chr == 0)
			len--;
		break;
	    }

	    case ('[') :
	    {
start_over:

	    /*
	     * Set up the arguments list
	     */
	    nargs = 0;
	    args[0] = args[1] = args[2] = args[3] = 0;
	    args[4] = args[5] = args[6] = args[7] = 0;
	    args[8] = args[9] = 0;
        
	   /*
	    * This stuff was taken/modified/inspired by rxvt.  We do it this 
	    * way in order to trap an esc sequence that is embedded in another
	    * (blah).  We're being really really really paranoid doing this, 
	    * but it is for the best.
	    */

	   /*
	    * Check to see if the stuff after the command is a "private" 
	    * modifier.  If it is, then we definitely arent interested.
	    *   '<' , '=' , '>' , '?'
	    */
	   chr = start[len];
	   if (chr >= '<' && chr <= '?')
		(void)0;		/* Skip it */

	   /*
	    * Now pull the arguments off one at a time.  Keep pulling them 
	    * off until we find a character that is not a number or a semicolon.
	    * Skip everything else.
	    */
	   for (nargs = 0; nargs < 10; str++)
	   {
		int n = 0;

		len++;
		for (n = 0; isdigit(start[len]); len++)
			n = n * 10 + (start[len] - '0');

		args[nargs++] = n;

		/*
		 * If we run out of code here, then we're totaly confused.
		 * just back out with whatever we have...
		 */
		if (!start[len])
			return len;

		if (start[len] != ';')
			break;
	    }

	    /*
	     * If we find a new ansi char, start all over from the top 
	     * and strip it out too
	     */
	    if (start[len] == 033)
		goto start_over;

	    /*
	     * Support "spaces" (cursor right) code
	     */
	    else if (start[len] == 'a' || start[len] == 'C')
	    {
		len++;
		if (nargs >= 1)
		{
		    /* Keep this within reality.  */
		    if (args[0] > 256)
			args[0] = 256;
		    *nd_spaces = args[0];
		}
	    }

	    /*
	     * Walk all of the numeric arguments, plonking the appropriate 
	     * attribute changes as needed.
	     */
	    else if (start[len] == 'm')
	    {
		int	i;

		len++;
		for (i = 0; i < nargs; i++)
		{
		    switch (args[i])
		    {
			case 0:		/* Reset to default */
			{
				a->reverse = a->bold = 0;
				a->blink = a->underline = 0;
				a->altchar = 0;
				a->color_fg = a->color_bg = 0;
				a->fg_color = a->bg_color = 0;
				break;
			}
			case 1:		/* bold on */
				a->bold = 1;
				break;
			case 2:		/* dim on -- not supported */
				break;
			case 4:		/* Underline on */
				a->underline = 1;
				break;
			case 5:		/* Blink on */
			case 26:	/* Blink on */
				a->blink = 1;
				break;
			case 6:		/* Blink off */
			case 25:	/* Blink off */
				a->blink = 0;
				break;
			case 7:		/* Reverse on */
				a->reverse = 1;
				break;
			case 21:	/* Bold off */
			case 22:	/* Bold off */
				a->bold = 0;
				break;
			case 24:	/* Underline off */
				a->underline = 0;
				break;
			case 27:	/* Reverse off */
				a->reverse = 0;
				break;
			case 30: case 31: case 32: case 33: case 34: case 35:
			case 36: case 37:	/* Set foreground color */
			{
				a->color_fg = 1;
				a->fg_color = args[i] - 30;
				break;
			}
			case 39:	/* Reset foreground color to default */
			{
				a->color_fg = 0;
				a->fg_color = 0;
				break;
			}
			case 40: case 41: case 42: case 43: case 44: case 45:
			case 46: case 47:	/* Set background color */
			{
				a->color_bg = 1;
				a->bg_color = args[i] - 40;
				break;
			}
			case 49:	/* Reset background color to default */
			{
				a->color_bg = 0;
				a->bg_color = 0;
				break;
			}

			default:	/* Everything else is not supported */
				break;
		    }
		} /* End of for loop */
	    } /* End of handling esc-[...m */
	    } /* End of case esc-[ */
	} /* End of case esc */

	/* All other escape sequences are ignored! */
	return len;
}

/**************************** STRIP ANSI ***********************************/
/*
 * The THREE FUNCTIONS OF DOOM
 *
 * 1) normalize_string -- given any arbitrary string, make it "safe" to use.
 * 2) prepare_display -- given a "safe" string, break it into lines
 * 3) output_with_count -- given a broken "safe" string, actually output it.
 */

/*
 * State 0 is a "normal, printable character" (8 bits included)
 * State 1 is a "C1 character", aka a control character with high bit set.
 * State 2 is an "escape character" (\033)
 * State 3 is a "color code character" (\003)
 * State 4 is an "attribute change character"
 * State 5 is a "suppressed character" (always stripped)
 * State 6 is a "character that is never printable."
 * State 7 is a "beep"
 * State 8 is a "tab"
 * State 9 is a "non-destructive space"
 */
static	unsigned char	ansi_state[256] = {
/*	^@	^A	^B	^C	^D	^E	^F	^G(\a) */
	6,	6,	4,	3,	6,	4,	4,	7,  /* 000 */
/*	^H	^I	^J(\n)	^K	^L(\f)	^M(\r)	^N	^O */
	6,	8,	0,	6,	6,	5,	6,	4,  /* 010 */
/*	^P	^Q	^R	^S	^T	^U	^V	^W */
	6,	6,	6,	9,	6,	6,	4,	6,  /* 020 */
/*	^X	^Y	^Z	^[	^\	^]	^^	^_ */
	6,	6,	6,	2,	6,	6,	6,	4,  /* 030 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 040 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 050 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 060 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 070 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 100 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 110 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 120 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 130 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 140 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 150 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 160 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 170 */
	1,	1,	1,	1,	1,	1,	1,	1,  /* 200 */
	1,	1,	1,	1,	1,	1,	1,	1,  /* 210 */
	1,	1,	1,	1,	1,	1,	1,	1,  /* 220 */
	1,	1,	1,	1,	1,	1,	1,	1,  /* 230 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 240 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 250 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 260 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 270 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 300 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 310 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 320 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 330 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 340 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 350 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 360 */
	0,	0,	0,	0,	0,	0,	0,	0,  /* 370 */
};

/*
	A = Character or sequence converted into an attribute
	M = Character mangled
	S = Character stripped, sequence (if any) NOT stripped
	X = Character stripped, sequence (if any) also stripped
	T = Transformed into other (safe) chars
	- = No transformation

					Type
    			0    1    2    3    4    5    6    7    8    9
(Default)		-    -    -    -    A    -    -    T    T    T
NORMALIZE		-    -    A    A    -    X    M    -    -    -
MANGLE_ESCAPES		-    -    S    -    -    -    -    -    -    -
STRIP_COLOR		-    -    -    X    -    -    -    -    -    -
STRIP_*			-    -    -    -    X    -    -    -    -    -
STRIP_UNPRINTABLE	-    X    S    S    X    X    X    X    -    -
STRIP_OTHER		X    -    -    -    -    -    -    -    X    X
(/SET ALLOW_C1)		-    X    -    -    -    -    -    -    -    -
*/

/*
 * new_normalize_string -- Transform an untrusted input string into something
 *				we can trust.
 * Arguments:
 *   str	An untrusted input string
 *   logical	How attribute changes should look in the output:
 *		0	Marshalled form, suitable for displaying
 *		1	Un-normalized form, suitable for the user (ie, ^B/^V)
 *		2	Stripped out entirely
 *		3	Marshalled form, especially for the status bar.
 *   mangler	How we want the string to be transformed
 *		The above chart shows how the different types of characters
 *		are transformed by the different mangler types.  There are
 *		three ambiguous cases, which are resolved as such:
 *		Type 2:
 *			MANGLE_ESCAPES has the first priority, then
 *			NORMALIZE is next, finally STRIP_UNPRINTABLE.
 *		Type 3:
 *			STRIP_UNPRINTABLE has the first priority, then
 *			NORMALIZE and STRIP_COLOR.  You need to use both 
 *			NORMALIZE and STRIP_COLOR to strip color changes 
 *			in color sequences
 *		Type 6:
 *			STRIP_UNPRINTABLE has higher priority than NORMALIZE.
 *
 * Furthermore, the following two sets affect behavior:
 *	  /SET ALLOW_C1_CHARS
 *		ON  == Type 1 chars are treated as Type 0 chars (safe)
 *		OFF == Type 1 chars are treated as Type 5 chars (unsafe)
 *	  /SET TERM_DOES_BRIGHT_BLINK
 *		???
 *
 * Return Value:
 *	A new trusted string, that has been subjected to the transformations
 *	in "mangler", with attribute changes represented in the "logical"
 *	format.
 */
#define this_char() (*str)
#define next_char() (*str++)
#define put_back() (str--)
#define nlchar '\n'
unsigned char *	new_normalize_string (const unsigned char *str, int logical, int mangle)
{
	unsigned char *	output;
	unsigned char	chr;
	Attribute	a, olda;
	int 		pos;
	int		maxpos;
	int		i;
	int		pc = 0;
	int		mangle_escapes, normalize;
	int		strip_reverse, strip_bold, strip_blink, 
			strip_underline, strip_altchar, strip_color, 
			strip_all_off, strip_nd_space, strip_c1, boldback;
	int		strip_unprintable, strip_other;
	size_t		(*attrout) (unsigned char *, Attribute *, Attribute *) = NULL;

	mangle_escapes 	= ((mangle & MANGLE_ESCAPES) != 0);
	normalize	= ((mangle & NORMALIZE) != 0);

	strip_color 	= ((mangle & STRIP_COLOR) != 0);
	strip_reverse 	= ((mangle & STRIP_REVERSE) != 0);
	strip_underline	= ((mangle & STRIP_UNDERLINE) != 0);
	strip_bold 	= ((mangle & STRIP_BOLD) != 0);
	strip_blink 	= ((mangle & STRIP_BLINK) != 0);
	strip_nd_space	= ((mangle & STRIP_ND_SPACE) != 0);
	strip_altchar 	= ((mangle & STRIP_ALT_CHAR) != 0);
	strip_all_off 	= ((mangle & STRIP_ALL_OFF) != 0);
	strip_unprintable = ((mangle & STRIP_UNPRINTABLE) != 0);
	strip_other	= ((mangle & STRIP_OTHER) != 0);

	strip_c1	= !get_int_var(ALLOW_C1_CHARS_VAR);
	boldback	= get_int_var(TERM_DOES_BRIGHT_BLINK_VAR);

	if (logical == 0)
		attrout = display_attributes;	/* prep for screen output */
	else if (logical == 1)
		attrout = logic_attributes;	/* non-screen handlers */
	else if (logical == 2)
		attrout = ignore_attributes;	/* $stripansi() function */
	else if (logical == 3)
		attrout = display_attributes;	/* The status line */
	else
		panic(1, "'logical == %d' is not valid.", logical);

	/* Reset all attributes to zero */
	a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
	a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;
	olda = a;

	/* 
	 * The output string has a few extra chars on the end just 
	 * in case you need to tack something else onto it.
	 */
	maxpos = strlen(str);
	output = (unsigned char *)new_malloc(maxpos + 192);
	pos = 0;

	while ((chr = next_char()))
	{
	    if (pos > maxpos)
	    {
		maxpos += 192; /* Extend 192 chars at a time */
		RESIZE(output, unsigned char, maxpos + 192);
	    }

	    switch (ansi_state[chr])
	    {
		/*
		 * State 0 are characters that are permitted under all
		 * circumstances.
		 */
		case 0:
		{
			if (strip_other)
				break;

normal_char:
			output[pos++] = chr;
			pc++;
			break;
		}

		/*
		 * State 1 is a control char with high bit set (C1 char)
		 */
		case 1:
		{
			if (strip_unprintable)
				break;
			if (strip_c1)
			{
				chr = (chr | 0x60) & 0x7F;
abnormal_char:
				a.reverse = !a.reverse;
				pos += attrout(output + pos, &olda, &a);
				output[pos++] = chr;
				a.reverse = !a.reverse;
				pos += attrout(output + pos, &olda, &a);
				break;
			}
			goto normal_char;
		}

		/*
		 * State 5 are characters that are never permitted under
		 * any circumstances.
		 */
		case 5:
		{
			if (strip_unprintable)
				break;
			if (normalize)
				break;
			goto normal_char;
		}

		/*
		 * State 6 is a control character (without high bit set)
		 * that doesn't have a special meaning to ircII.
		 */
		case 6:
		{
			/*
			 * \f is a special case, state 0, for status bar.  
			 * Either I special case it here, or in 
			 * output_to_count.  I prefer here.
			 */
			if (logical == 3 && chr == '\f')
				goto normal_char;

			if (strip_unprintable)
				break;
			if (termfeatures & TERM_CAN_GCHAR)
				goto normal_char;

			if (normalize)
			{
				output[pos++] = (chr | 0x40) & 0x7F;
				goto abnormal_char;
			}
			goto normal_char;
		}

		/*
		 * State 2 is the escape character
		 */
		case 2:
		{
		    int	nd_spaces = 0;
		    ssize_t	esclen;

		    if (mangle_escapes == 1)
		    {
			chr = '[';
			goto abnormal_char;
		    }

		    if (normalize == 1)
		    {
			esclen = read_esc_seq(str, (void *)&a, &nd_spaces);

			if (nd_spaces != 0 && !strip_nd_space)
			{
			    /* This is just sanity */
			    if (pos + nd_spaces > maxpos)
			    {
				maxpos += nd_spaces; 
				RESIZE(output, unsigned char, maxpos + 192);
			    }
			    while (nd_spaces-- > 0)
			    {
				output[pos++] = ND_SPACE;
				pc++;
			    }
			    break;		/* attributes can't change */
			}

			if (a.reverse && strip_reverse)		a.reverse = 0;
			if (a.bold && strip_bold)		a.bold = 0;
			if (a.blink && strip_blink)		a.blink = 0;
			if (a.underline && strip_underline)	a.underline = 0;
			if (a.altchar && strip_altchar)		a.altchar = 0;
			if (strip_color)
			{
				a.color_fg = a.color_bg = 0;
				a.fg_color = a.bg_color = 0;
			}
			pos += attrout(output + pos, &olda, &a);
			str += esclen;
			break;
		    }

		    if (strip_unprintable)
			break;

		    goto normal_char;
		}

	        /*
		 * Normalize ^C codes...
	         */
		case 3:
		{
		   if (strip_unprintable)
			break;

		   if (strip_color || normalize)
		   {
			ssize_t	len;

			put_back();
			len = read_color_seq(str, (void *)&a, boldback);
			str += len;

			/* Suppress the color if no color is permitted */
			if (a.bold && strip_bold)		a.bold = 0;
			if (a.blink && strip_blink)		a.blink = 0;
			if (strip_color)
			{
				a.color_fg = a.color_bg = 0;
				a.fg_color = a.bg_color = 0;
			}

			/* Output the new attributes */
			pos += attrout(output + pos, &olda, &a);
			break;
		    }

		    goto normal_char;
		}

		/*
		 * State 4 is for the special highlight characters
		 */
		case 4:
		{
		    if (strip_unprintable)
			break;

		    put_back();
		    switch (this_char())
		    {
			case REV_TOG:
				if (!strip_reverse)
					a.reverse = !a.reverse;
				break;
			case BOLD_TOG:
				if (!strip_bold)
					a.bold = !a.bold;
				break;
			case BLINK_TOG:
				if (!strip_blink)
					a.blink = !a.blink;
				break;
			case UND_TOG:
				if (!strip_underline)
					a.underline = !a.underline;
				break;
			case ALT_TOG:
				if (!strip_altchar)
					a.altchar = !a.altchar;
				break;
			case ALL_OFF:
				if (!strip_all_off)
				{
				    a.reverse = a.bold = a.blink = 0;
				    a.underline = a.altchar = 0;
				    a.color_fg = a.color_bg = 0;
				    a.bg_color = a.fg_color = 0;
				    pos += attrout(output + pos, NULL, &a);
				    olda = a;
				}
				break;
			default:
				break;
		    }
		    (void)next_char();

		    /* After ALL_OFF, this is a harmless no-op */
		    pos += attrout(output + pos, &olda, &a);
		    break;
		}

		case 7:      /* bell */
		{
			if (strip_unprintable)
				break;

			output[pos++] = '\007';
			break;
		}

		case 8:		/* Tab */
		{
			int	len = 8 - (pc % 8);

			if (strip_other)
				break;

			for (i = 0; i < len; i++)
			{
				output[pos++] = ' ';
				pc++;
			}
			break;
		}

		case 9:		/* Non-destruct space */
		{
			if (strip_other)
				break;
			if (strip_nd_space)
				break;

			output[pos++] = ND_SPACE;
			break;
		}

		default:
		{
			panic(1, "Unknown normalize_string mode");
			break;
		}
	    } /* End of huge ansi-state switch */
	} /* End of while, iterating over input string */

	/* Terminate the output and return it. */
	if (logical == 0)
	{
		a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
		a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;
		pos += attrout(output + pos, &olda, &a);
	}
	output[pos] = output[pos + 1] = 0;
	return output;
}


/* 
 * XXX I'm not sure where this belongs, but for now it goes here.
 * This function takes a type-1 normalized string (with the attribute
 * markers) and converts them back to logical characters.  This is needed
 * for lastlog and the status line and so forth.
 */
unsigned char *	denormalize_string (const unsigned char *str)
{
	unsigned char *	output = NULL;
	size_t		maxpos;
	Attribute 	olda, a;
	size_t		span;
	size_t		pos;

        /* Reset all attributes to zero */
        a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
        a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;
	olda = a;

	/* 
	 * The output string has a few extra chars on the end just 
	 * in case you need to tack something else onto it.
	 */
	maxpos = strlen(str);
	output = (unsigned char *)new_malloc(maxpos + 192);
	pos = 0;

	while (*str)
	{
		if (pos > maxpos)
		{
			maxpos += 192; /* Extend 192 chars at a time */
			RESIZE(output, unsigned char, maxpos + 192);
		}
		switch (*str)
		{
		    case '\006':
		    {
			if (read_attributes(str, &a))
				continue;		/* Mangled */
			str += 5;

			span = logic_attributes(output + pos, &olda, &a);
			pos += span;
			break;
		    }
		    default:
		    {
			output[pos++] = *str++;
			break;
		    }
		}
	}
	output[pos] = 0;
	return output;
}



/*
 * Prepare_display -- this is a new twist on FireClown's original function.
 * We dont do things quite the way they were explained in the previous 
 * comment that used to be here, so here's the rewrite. ;-)
 *
 * This function is used to break a logical line of display into some
 * number of physical lines of display, while accounting for various kinds
 * of display codes.  The logical line is passed in the 'orig_str' variable,
 * and the width of the physical display is passed in 'max_cols'.   If 
 * 'lused' is not NULL, then it points at an integer that specifies the 
 * maximum number of lines that should be prepared.  The actual number of 
 * lines that are prepared is stored into 'lused'.  The 'flags' variable
 * specifies some extra options, the only one of which that is supported
 * right now is "PREPARE_NOWRAP" which indicates that you want the function
 * to break off the text at 'max_cols' and not to "wrap" the last partial
 * word to the next line. ($leftpc() depends on this)
 */
#define SPLIT_EXTENT 40
unsigned char **prepare_display (int winref,
				 const unsigned char *str,
                                 int max_cols,
                                 int *lused,
                                 int flags)
{
static 	int 	recursion = 0, 
		output_size = 0;
	int 	pos = 0,            /* Current position in "buffer" */
		col = 0,            /* Current column in display    */
		word_break = 0,     /* Last end of word             */
		indent = 0,         /* Start of second word         */
		firstwb = 0,	    /* Buffer position of second word */
		line = 0,           /* Current pos in "output"      */
		do_indent,          /* Use indent or continued line? */
		newline = 0;        /* Number of newlines           */
static	unsigned char 	**output = (unsigned char **)0;
const 	unsigned char	*ptr;
	unsigned char 	buffer[BIG_BUFFER_SIZE + 1],
			c,
			*pos_copy;
const	unsigned char	*cont_ptr;
	unsigned char	*cont = NULL;
	const char 	*words;
	Attribute	a, olda;
	Attribute	saved_a;
	unsigned char	*cont_free = NULL;

	if (recursion)
		panic(1, "prepare_display() called recursively");
	recursion++;

        /* Reset all attributes to zero */
        a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
        a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;
        saved_a.bold = saved_a.underline = saved_a.reverse = 0;
	saved_a.blink = saved_a.altchar = 0;
        saved_a.color_fg = saved_a.color_bg = saved_a.fg_color = 0;
	saved_a.bg_color = 0;

	/* do_indent = get_int_var(INDENT_VAR); */
	do_indent = get_indent_by_winref(winref);
	if (!(words = get_string_var(WORD_BREAK_VAR)))
		words = " \t";
	if (!(cont_ptr = get_string_var(CONTINUED_LINE_VAR)))
		cont_ptr = empty_string;

	buffer[0] = 0;

	if (!output_size)
	{
		int 	new_i = SPLIT_EXTENT;
		RESIZE(output, char *, new_i);
		while (output_size < new_i)
			output[output_size++] = 0;
	}

	/*
	 * Start walking through the entire string.
	 */
	for (ptr = str; *ptr && (pos < BIG_BUFFER_SIZE - 8); ptr++)
	{
		switch (*ptr)
		{
			case '\007':      /* bell */
				buffer[pos++] = *ptr;
				break;

			case '\n':      /* Forced newline */
			{
				newline = 1;
				if (indent == 0)
					indent = -1;
				word_break = pos;
				break; /* case '\n' */
			}

                        /* Attribute changes -- copy them unmodified. */
                        case '\006':
                        {
                                if (read_attributes(ptr, &a) == 0)
                                {
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr;
                                }
                                else
                                        abort();

				/*
				 * XXX This isn't a hack, but it _is_ ugly!
				 * Because I'm too lazy to find a better place
				 * to put this (down among the line wrapping
				 * logic would be a good place), I take the
				 * cheap way out by "saving" any attribute
				 * changes that occur prior to the first space
				 * in a line.  If there are no spaces for the
				 * rest of the line, then this *is* the saved
				 * attributes we will need to start the next
				 * line.  This fixes an abort().
				 */
				if (word_break == 0)
					saved_a = a;

                                continue;          /* Skip the column check */
                        }

			default:
			{
				if (!strchr(words, *ptr))
				{
					if (indent == -1)
						indent = col;
					buffer[pos++] = *ptr;
					col++;
					break;
				}
				/* FALLTHROUGH */
			}

			case ' ':
			case ND_SPACE:
			{
				if (indent == 0)
				{
					indent = -1;
					firstwb = pos;
				}
				word_break = pos;
				saved_a = a;
				if (*ptr != ' ' && ptr[1] &&
				    (col + 1 < max_cols))
					word_break++;
				buffer[pos++] = *ptr;
				col++;
				break;
			}
		} /* End of switch (*ptr) */

		/*
		 * Must check for cols >= maxcols+1 becuase we can have a 
		 * character on the extreme screen edge, and we would still 
		 * want to treat this exactly as 1 line, and col has already 
		 * been incremented.
		 */
		if ((col > max_cols) || newline)
		{
			/*
			 * We just incremented col, but we need to decrement
			 * it in order to keep the count correct!
			 *		--zinx
			 */
			if (col > max_cols)
				col--;

			/*
			 * XXX Hackwork and trickery here.  In the very rare
			 * case where we end the output string *exactly* at
			 * the end of the line, then do not do any more of
			 * the following handling.  Just punt right here.
			 */
			if (ptr[1] == 0)
				break;		/* stop all processing */

			/*
			 * Default the end of line wrap to the last character
			 * we parsed if there were no spaces in the line, or
			 * if we're preparing output that is not to be
			 * wrapped (such as for counting output length.
			 */
			if (word_break == 0 || (flags & PREPARE_NOWRAP))
				word_break = pos;

			/*
			 * XXXX Massive hackwork here.
			 *
			 * Due to some ... interesting design considerations,
			 * if you have /set indent on and your first line has
			 * exactly one word seperation in it, then obviously
			 * there is a really long "word" to the right of the 
			 * first word.  Normally, we would just break the 
			 * line after the first word and then plop the really 
			 * big word down to the second line.  Only problem is 
			 * that the (now) second line needs to be broken right 
			 * there, and we chew up (and lose) a character going 
			 * through the parsing loop before we notice this.
			 * Not good.  It seems that in this very rare case, 
			 * people would rather not have the really long word 
			 * be sent to the second line, but rather included on 
			 * the first line (it does look better this way), 
			 * and so we can detect this condition here, without 
			 * losing a char but this really is just a hack when 
			 * it all comes down to it.  Good thing its cheap. ;-)
			 */
			if (!cont && (firstwb == word_break) && do_indent) 
				word_break = pos;

			/*
			 * If we are approaching the number of lines that
			 * we have space for, then resize the master line
			 * buffer so we dont run out.
			 */
			if (line >= output_size - 3)
			{
				int new_i = output_size + SPLIT_EXTENT;
				RESIZE(output, char *, new_i);
				while (output_size < new_i)
					output[output_size++] = 0;
			}

			/* XXXX HACK! XXXX HACK! XXXX HACK! XXXX */
			/*
			 * Unfortunately, due to the "line wrapping bug", if
			 * you have a really long line at the end of the first
			 * line of output, and it needs to be wrapped to the
			 * second line of input, we were blindly assuming that
			 * it would fit on the second line, but that may not
			 * be true!  If the /set continued_line jazz ends up
			 * being longer than whatever was before the wrapped
			 * word on the first line, then the resulting second
			 * line would be > max_cols, causing corruption of the
			 * display (eg, the status bar gets written over)!
			 *
			 * To counteract this bug, at the end of the first
			 * line, we calcluate the continued line marker
			 * *before* we commit the first line.  That way, we
			 * can know if the word to be wrapped will overflow
			 * the second line, and in such case, we break that
			 * word precisely at the current point, rather than
			 * at the word_break point!  This prevents the
			 * "line wrap bug", albeit in a confusing way.
			 */

			/*
			 * Calculate the continued line marker.  This is
			 * a little bit tricky because we cant figure it out
			 * until after the first line is done.  The first
			 * time through, cont == empty_string, so if !*cont,
			 * we know it has not been initialized.
			 *
			 * So if it has not been initialized and /set indent
			 * is on, and the place to indent is less than a third
			 * of the screen width and /set continued_line is
			 * less than the indented width, then we pad the 
			 * /set continued line value out to the appropriate
			 * width.
			 */
			if (!cont)
			{
				int	lhs_count = 0;
				int	continued_count = 0;

				/* Because Blackjac asked me to */
				if (indent > max_cols / 3)
					indent = max_cols / 3;
				if (indent <= 0)
					indent = max_cols / 3;

				if (do_indent)
				{
				    unsigned char *fixedstr;

				    fixedstr = prepare_display2(cont_ptr, 
							indent, 0, ' ', 1);
				    cont = LOCAL_COPY(fixedstr);
				    new_free(&fixedstr);
				}

				/*
				 * Otherwise, we just use /set continued_line, 
				 * whatever it is.
				 */
				else /* if ((!cont || !*cont) && *cont_ptr) */
					cont = LOCAL_COPY(cont_ptr);

				cont_free = cont = new_normalize_string(cont, 
						0, display_line_mangler);

				/*
				 * XXXX "line wrap bug" fix.  If we are here,
				 * then we are between the first and second
				 * lines, and we might have a word that does
				 * not fit on the first line that also does
				 * not fit on the second line!  We check for
				 * that right here, and if it won't fit on
				 * the next line, we revert "word_break" to
				 * the current position.
				 *
				 * THIS IS UNFORTUNATELY VERY EXPENSIVE! :(
				 */
				c = buffer[word_break];
				buffer[word_break] = 0;
				lhs_count = output_with_count(buffer, 0, 0);
				buffer[word_break] = c;
				continued_count = output_with_count(cont, 0, 0);

				/* 
				 * Chop the line right here if it will
				 * overflow the next line.
				 *
				 * Save the attributes, too! (05/29/02)
				 *
				 * XXX Saving the attributes may be 
				 * spurious but i'm erring on the side
				 * of caution for the moment.
				 */
				if (lhs_count <= continued_count) {
					word_break = pos;
					saved_a = a;
				}

				/*
				 * XXXX End of nasty nasty hack.
				 */
			}

			/*
			 * Now we break off the line at the last space or
			 * last char and copy it off to the master buffer.
			 */
			c = buffer[word_break];
			buffer[word_break] = 0;
			malloc_strcpy((char **)&(output[line++]), buffer);
			buffer[word_break] = c;


			/*
			 * Skip over all spaces that occur after the break
			 * point, up to the right part of the screen (where
			 * we are currently parsing).  This is what allows
			 * lots and lots of spaces to take up their room.
			 * We let spaces fill in lines as much as neccesary
			 * and if they overflow the line we let them bleed
			 * to the next line.
			 */
			while (word_break < pos && buffer[word_break] == ' ')
				word_break++;

			/*
			 * At this point, we still have some junk left in
			 * 'buffer' that needs to be moved to the next line.
			 * But of course, all new lines must be prefixed by
			 * the /set continued_line and /set indent stuff, so
			 * we copy off the stuff we have to a temporary
			 * buffer, copy the continued-line stuff into buffer
			 * and then re-append the junk into buffer.  Then we
			 * fix col and pos appropriately and continue parsing 
			 * str...
			 */
			/* 'pos' has already been incremented... */
			buffer[pos] = 0;
			pos_copy = LOCAL_COPY(buffer + word_break);
			strlcpy(buffer, cont, sizeof(buffer) / 2);
			display_attributes(buffer + strlen(buffer), &olda, &saved_a);
			strlcat(buffer, pos_copy, sizeof(buffer) / 2);
			display_attributes(buffer + strlen(buffer), &olda, &a);

			pos = strlen(buffer);
			/* Watch this -- ugh. how expensive! :( */
			col = output_with_count(buffer, 0, 0);
			word_break = 0;
			newline = 0;

			/*
			 * The 'lused' argument allows us to truncate the
			 * parsing at '*lused' lines.  This is most helpful
			 * for the $leftpc() function, which sets a logical
			 * screen width and asks us to "output" one line.
			 */
			if (*lused && line >= *lused)
			{
				*buffer = 0;
				break;
			}
		} /* End of new line handling */
	} /* End of (ptr = str; *ptr && (pos < BIG_BUFFER_SIZE - 8); ptr++) */

        /* Reset all attributes to zero */
        a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
        a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;
	pos += display_attributes(buffer + pos, &olda, &a);
	buffer[pos] = '\0';
	if (*buffer)
		malloc_strcpy((char **)&(output[line++]),buffer);

	recursion--;
	new_free(&output[line]);
	new_free(&cont_free);
	*lused = line - 1;
	return output;
}

/*
 * An evil bastard child hack that pulls together the important parts of
 * prepare_display(), fix_string_width(), and output_with_count().
 *
 * This function is evil because it was spawned by copying the above 
 * functions and they were not refactored to make use of this function,
 * which they would do if I were not doing this in a rush.
 *
 * If you change the above three functions, you would do well to make sure to 
 * adjust this function, for if you do not, then HIGGLEDY PIGGLEDY WILL ENSUE.
 *
 * XXX - This is a bletcherous inelegant hack and i hate it.
 */
unsigned char *prepare_display2 (const unsigned char *orig_str, int max_cols, int allow_truncate, char fillchar, int denormalize)
{
	int 	pos = 0,            /* Current position in "buffer" */
		col = 0,            /* Current column in display    */
		line = 0,           /* Current pos in "output"      */
		newline = 0;        /* Number of newlines           */
	unsigned char 	*str = NULL;
	unsigned char	*retval = NULL;
	size_t		clue = 0;
const 	unsigned char	*ptr;
	unsigned char 	buffer[BIG_BUFFER_SIZE + 1];
	Attribute	a;
	unsigned char *	real_retval;

        /* Reset all attributes to zero */
        a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
        a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;

        str = new_normalize_string(orig_str, 3, NORMALIZE);
	buffer[0] = 0;

	/*
	 * Start walking through the entire string.
	 */
	for (ptr = str; *ptr && (pos < BIG_BUFFER_SIZE - 8); ptr++)
	{
		switch (*ptr)
		{
			case '\007':      /* bell */
				buffer[pos++] = *ptr;
				break;

			case '\n':      /* Forced newline */
				newline = 1;
				break; /* case '\n' */

                        /* Attribute changes -- copy them unmodified. */
                        case '\006':
                        {
                                if (read_attributes(ptr, &a) == 0)
                                {
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr++;
                                        buffer[pos++] = *ptr;
                                }
                                else
                                        abort();

                                continue;          /* Skip the column check */
                        }

			default:
			{
				buffer[pos++] = *ptr;
				col++;
				break;
			}
		} /* End of switch (*ptr) */

		/*
		 * Must check for cols >= maxcols+1 becuase we can have a 
		 * character on the extreme screen edge, and we would still 
		 * want to treat this exactly as 1 line, and col has already 
		 * been incremented.
		 */
		if ((allow_truncate && col > max_cols) || newline)
			break;
	}
	buffer[pos] = 0;

	if (*buffer)
		malloc_strcpy_c((char **)&retval, buffer, &clue);

	/*
	 * If we get here, either we have slurped up 'max_cols' cols, or
	 * we hit a newline, or the string was too short.
	 */
	if (col < max_cols && fillchar != '\0')
	{
		char fillstr[2];
		fillstr[0] = fillchar;
		fillstr[1] = 0;

		/* XXX One col per byte assumption! */
		while (col++ < max_cols)
			malloc_strcat_c((char **)&retval, fillstr, &clue);
	}

	if (denormalize)
		real_retval = denormalize_string(retval);
	else
		real_retval = retval, retval = NULL;

	new_free(&retval);
	new_free(&str);
	return real_retval;
}

/*
 * rite: This is the primary display wrapper to the 'output_with_count'
 * function.  This function is called whenever a line of text is to be
 * displayed to an irc window.  It is assumed that the cursor has been
 * placed in the appropriate position before this function is called.
 *
 * This function will "scroll" the target window.  Note that "scrolling"
 * is both a logical and physical act.  The window needs to be told that
 * a line is going to be output, and so it needs to be able to adjust its
 * top_of_display pointer; the hardware terminal also needs to be scrolled
 * so that there is room to put the new text.  scroll_window() handles both
 * of these tasks for us.
 *
 * output_with_count() actually calls putchar_x() for each character in
 * the string, doing the physical output.  It also emits any attribute
 * markers that are in the string.  It does do a clear-to-line, but it does
 * NOT move the cursor away from the end of the line.  We do that after it
 * has returned.
 *
 * This function is used by both irciiwin_display, and irciiwin_repaint.
 * Dont ever 'fold' it in anywhere.
 *
 * The arguments:
 *	window		- The target window for the output
 *	str		- What is to be outputted
 */
static int 	rite (Window *window, const unsigned char *str)
{
	output_screen = window->screen;
	scroll_window(window);

	if (window->screen && window->display_lines)
		output_with_count(str, 1, foreground);

	window->cursor++;
	return 0;
}

/*
 * This is the main physical output routine.  In its most obvious
 * use, 'cleareol' and 'output' is 1, and it outputs 'str' to the main
 * display (controlled by output_screen), outputting any attribute markers
 * that it finds along the way.  The return value is the number of physical
 * printable characters output.  However, if 'output' is 0, then no actual
 * output is performed, but the counting still takes place.  If 'clreol'
 * is 0, then the rest of the line is not cleared after 'str' has been
 * completely output.  If 'output' is 0, then clreol is ignored.
 *
 * In some cases, you may want to output in multiple calls, and "all_off"
 * should be set to 1 when you're all done with the end of the 
 * If 'output' is 1 and 'all_off' is 1, do a term_all_off() when the output
 * is done.  If 'all_off' is 0, then don't do an all_off, because
 */
int 	output_with_count (const unsigned char *str1, int clreol, int output)
{
	int 		beep = 0, 
			out = 0;
	Attribute	a;
	const unsigned char *	str;

        /* Reset all attributes to zero */
        a.bold = a.underline = a.reverse = a.blink = a.altchar = 0;
        a.color_fg = a.color_bg = a.fg_color = a.bg_color = 0;

	for (str = str1; str && *str; str++)
	{
	    switch (*str)
	    {
		/* Attribute marker */
		case '\006':
		{
			if (read_attributes(str, &a))
				break;
			if (output)
				term_attribute(&a);
			str += 4;
			break;
		}

		/* Terminal beep */
		case '\007':
		{
			beep++;
			break;
		}

		/* Non-destructive space */
		case ND_SPACE:
		{
			if (output)
				term_cursor_right();
			out++;		/* Ooops */
			break;
		}

		/* Any other printable character */
		default:
		{
			/*
			 * Note that 'putchar_x()' is safe here because 
			 * normalize_string() has already removed all of the 
			 * nasty stuff that could end up getting here.  And
			 * for those things that are nasty that get here, its 
			 * probably because the user specifically asked for it.
			 */
			if (output)
				putchar_x(*str);
			out++;
			break;
		}
	    }
	}

	if (output)
	{
		if (beep)
			term_beep();
		if (clreol)
			term_clear_to_eol();
		term_all_off();		/* Clean up after ourselves! */
	}

	return out;
}


/*
 * add_to_screen: This adds the given null terminated buffer to the screen.
 * That is, it routes the line to the appropriate window.  It also handles
 * /redirect handling.
 */
void 	add_to_screen (const unsigned char *buffer)
{
	Window *tmp = NULL;
	int	winref;

	/*
	 * Just paranoia.
	 */
	if (!current_window)
	{
		puts(buffer);
		return;
	}

	if (dumb_mode)
	{
		add_to_lastlog(current_window, buffer);
		if (privileged_output || 
		    do_hook(WINDOW_LIST, "%u %s", current_window->refnum, buffer))
			puts(buffer);
		fflush(stdout);
		return;
	}

	/* All windows MUST be "current" before output can occur */
	update_all_windows();

	/*
	 * The highest priority is if we have explicitly stated what
	 * window we want this output to go to.
	 */
	if (to_window)
	{
		add_to_window(to_window, buffer);
		return;
	}

	/*
	 * The next priority is "LEVEL_NONE" which is only ever
	 * used by the /WINDOW command, but I'm not even sure it's very
	 * useful.  Maybe I'll think about this again later.
	 */
	else if ((who_level == LEVEL_NONE) && 
	        ((winref = get_winref_by_servref(from_server)) > -1) && 
                (tmp = get_window_by_refnum(winref)))
	{
		add_to_window(tmp, buffer);
		return;
	}

	/*
	 * Next priority is if the output is targeted at a certain
	 * user or channel (used for /window channel or /window add targets)
	 */
	else if (who_from)
	{
	    if (is_channel(who_from))
	    {
		if (from_server == NOSERV)
		    panic(0, "Output to channel [%s:NOSERV]: %s",
				who_from, buffer);

	        if ((tmp = get_window_by_refnum(
				get_channel_winref(who_from, from_server))))
		{
		    add_to_window(tmp, buffer);
		    return;
		}
	    }
	    else
	    {
#if 0
	      for (;;)
	      {
#endif
		tmp = NULL;
		while (traverse_all_windows(&tmp))
		{
		    /* Must be for our server */
		    if (who_level != LEVEL_DCC && (tmp->server != from_server))
			continue;

		    /* Must be on the nick list */
		    if (!find_in_list((List **)&(tmp->nicks), 
					who_from, !USE_WILDCARDS))
			continue;

		    add_to_window(tmp, buffer);
		    return;
		}

#if 0
		/*
		 * EPIC4 had a hideously complicated if() that handled 
		 * DCC CHAT nicks ("=nick") against /query's that look like
		 * "nick" or "=nick".  I'm cheating here by just removing
		 * the = and letting this go through a second pass.
		 */
		if (*who_from == '=')
		{
		    who_from++;
		    continue;
		}
		else
		    break;
	      }
#endif
	    }
	}

	/*
	 * Check to see if this level should go to current window
	 */
	if ((mask_isset(&current_window_mask, who_level)) &&
	    ((winref = get_winref_by_servref(from_server)) > -1) && 
            (tmp = get_window_by_refnum(winref)))
	{
		add_to_window(tmp, buffer);
		return;
	}

	/*
	 * Check to see if any window can claim this level
	 */
	tmp = NULL;
	while (traverse_all_windows(&tmp))
	{
		/*
		 * Check for /WINDOW LEVELs that apply
		 */
		if (who_level == LEVEL_DCC && 
			mask_isset(&tmp->window_mask, who_level))
		{
			add_to_window(tmp, buffer);
			return;
		}
		if ((from_server == tmp->server || from_server == NOSERV)
			&& mask_isset(&tmp->window_mask, who_level))
		{
			add_to_window(tmp, buffer);
			return;
		}
	}

	/*
	 * If all else fails, if the current window is connected to the
	 * given server, use the current window.
	 */
	if (current_window->server == from_server)
	{
		add_to_window(current_window, buffer);
		return;
	}

	/*
	 * And if that fails, look for ANY window that is bound to the
	 * given server (this never fails if we're connected.)
	 */
	tmp = NULL;
	while (traverse_all_windows(&tmp))
	{
		if (tmp->server != from_server)
			continue;

		add_to_window(tmp, buffer);
		return;
	}

	/*
	 * No window found for a server is usually because we're
	 * disconnected or not yet connected.
	 */
	add_to_window(current_window, buffer);
	return;
}

/*
 * add_to_window: Given a window and a line to display, this handles all
 * of the window-level stuff like the logfile, the lastlog, splitting
 * the line up into rows, adding it to the display (scrollback) buffer, and
 * if we're invisible and the user wants notification, we handle that too.
 *
 * add_to_display_list() handles the *composure* of the buffer that backs the
 * screen, handling HOLD_MODE, trimming the scrollback buffer if it gets too
 * big, scrolling the window and moving the top_of_window pointer as neccesary.
 * It also tells us if we should display to the screen or not.
 *
 * rite() handles the *appearance* of the display, writing to the screen as
 * neccesary.
 */
static void 	add_to_window (Window *window, const unsigned char *str)
{
	char *		pend;
	unsigned char *	strval;
	unsigned char *	free_me = NULL;
        unsigned char **       lines;
        int             cols;
	int		numl = 0;
	intmax_t	refnum;

	if (get_server_redirect(window->server))
		if (redirect_text(window->server, 
			        get_server_redirect(window->server),
				str, NULL, 0))
			return;

	if (!privileged_output)
	{
	   static int recursion = 0;

	   if (!do_hook(WINDOW_LIST, "%u %s", window->refnum, str))
		return;

	   /* 
	    * If output rewriting causes more output, (such as a stray error
	    * message) allow a few levels of nesting [just to be kind], but 
	    * cut the recursion off at its knees at 5 levels.  This is an 
	    * entirely arbitrary value.  Change it if you wish.
	    * (Recursion detection by larne in epic4-2.1.3)
	    */
	    recursion++;
	    if (recursion < 5 && (pend = get_string_var(OUTPUT_REWRITE_VAR)))
	    {
		unsigned char	argstuff[10240];

		/* Create $* and then expand with it */
		snprintf(argstuff, 10240, "%u %s", window->refnum, str);
		str = free_me = expand_alias(pend, argstuff);
	    }
	    recursion--;
	}

	/* Add to logs + lastlog... */
	add_to_log(0, window->log_fp, window->refnum, str, 0, NULL);
	add_to_logs(window->refnum, from_server, who_from, who_level, str);
	refnum = add_to_lastlog(window, str);

	/* Add to scrollback + display... */
	cols = window->columns - 1;
	strval = new_normalize_string(str, 0, display_line_mangler);
        for (lines = prepare_display(window->refnum, strval, cols, &numl, 0); *lines; lines++)
	{
		if (add_to_scrollback(window, *lines, refnum))
		    if (ok_to_output(window))
			rite(window, *lines);
	}
	new_free(&strval);

	/* Check the status of the window and scrollback */
	check_window_cursor(window);
	trim_scrollback(window);

	cursor_in_display(window);
	cursor_to_input();

	/*
	 * Handle special cases for output to hidden windows -- A beep to
	 * a hidden window with /window beep_always on results in a real beep 
	 * and a message to the current window.  Output to a hidden window 
	 * with /window notify on results in a message to the current window 
	 * and a status bar redraw.
	 *
	 * /XECHO -F sets "do_window_notifies" which overrules this.
	 */
	if (!window->screen && do_window_notifies)
	{
	    const char *type = NULL;

	    /* /WINDOW BEEP_ALWAYS added for archon.  */
	    if (window->beep_always && strchr(str, '\007'))
	    {
		type = "Beep";
		term_beep();
	    }
	    if (!(window->notified) &&
			mask_isset(&window->notify_mask, who_level))
	    {
		window->notified = 1;
		if (window->notify_when_hidden)
			type = "Activity";
		update_all_status();
	    }

	    if (type)
	    {
		int l = message_setall(current_window->refnum, who_from, who_level);
		say("%s in window %d", type, window->refnum);
		pop_message_from(l);
	    }
	}
	if (free_me)
		new_free(&free_me);
}

/*
 * add_to_window_scrollback: XXX -- doesn't belong here. oh well.
 * This unifies the important parts of add_to_window and window_disp
 * for the purpose of reconstituting the scrollback of a window after
 * a resize event.
 */
void 	add_to_window_scrollback (Window *window, const unsigned char *str, intmax_t refnum)
{
	unsigned char *	strval;
        unsigned char **       lines;
        int             cols;
	int		numl = 0;

	/* Normalize the line of output */
	cols = window->columns - 1;
	strval = new_normalize_string(str, 0, display_line_mangler);
        for (lines = prepare_display(window->refnum, strval, cols, &numl, 0); *lines; lines++)
		add_to_scrollback(window, *lines, refnum);
	new_free(&strval);
}

/*
 * This returns 1 if the window does not need to scroll for new output.
 * This returns 0 if the window does need to scroll for new output.
 *
 * This call should be used to guard calls to rite(), because rite() will
 * call scroll_window() if the window is full.  Scroll_window() will panic 
 * if the window is not using the "scrolling" view.  Therefore, this function
 * differentiates between a window that is full because it is in hold mode or
 * scrollback, and a window that is full and can be scrolled.
 */
static int	ok_to_output (Window *window)
{
	/*
	 * Output is ok as long as the three top of displays all are 
	 * within a screenful of the insertion point!
	 */
	if (window->scrollback_top_of_display)
	{
	    if (window->scrollback_distance_from_display_ip >
				window->display_lines)
		return 0;	/* Definitely no output here */
	}

	if (window->holding_top_of_display)
	{
	    if (window->holding_distance_from_display_ip >
				window->display_lines)
		return 0;	/* Definitely no output here */
	}

	return 1;		/* Output is authorized */
}

/*
 * scroll_window: Given a window, this is responsible for making sure that
 * the cursor is placed onto the "next" line.  If the window is full, then
 * it will scroll the window as neccesary.  The cursor is always set to the
 * correct place when this returns.
 *
 * This is only ever (to be) called by rite(), and you must always call
 * ok_to_output() before you call rite().  If you do not call ok_to_output(),
 * this function will panic if the window needs to be scrolled.
 */
static void 	scroll_window (Window *window)
{
	if (dumb_mode)
		return;

	if (window->cursor > window->display_lines)
		panic(1, "Window [%d]'s cursor [%d] is off the display [%d]",
			window->refnum, window->cursor, window->display_lines);

	/*
	 * If the cursor is beyond the window then we should probably
	 * look into scrolling.
	 */
	if (window->cursor == window->display_lines)
	{
		int scroll;

		/*
		 * If we ever need to scroll a window that is in scrollback
		 * or in hold_mode, then that means either display_window isnt
		 * doing its job or something else is completely broken.
		 * Probably shouldnt be fatal, but i want to trap these.
		 */
		if (window->holding_distance_from_display_ip > 
						window->display_lines)
			panic(1, "Can't output to window [%d] "
				"because it is holding stuff: [%d] [%d]", 
				window->refnum, 
				window->holding_distance_from_display_ip, 
				window->display_lines);

		if (window->scrollback_distance_from_display_ip > 
						window->display_lines)
			panic(1, "Can't output to window [%d] "
				"because it is scrolling back: [%d] [%d]", 
				window->refnum, 
				window->scrollback_distance_from_display_ip, 
				window->display_lines);

		/* Scroll by no less than 1 line */
		if ((scroll = get_int_var(SCROLL_LINES_VAR)) <= 0)
			scroll = 1;

		/* Adjust the top of the physical display */
		if (window->screen && foreground && window->display_lines)
		{
			term_scroll(window->top,
				window->top + window->cursor - 1, 
				scroll);
		}

		/* Adjust the cursor */
		window->cursor -= scroll;
	}

	/*
	 * Move to the new line and wipe it
	 */
	if (window->screen && window->display_lines)
	{
		window->screen->cursor_window = window;
		term_move_cursor(0, window->top + window->cursor);
		term_clear_to_eol();
		cursor_in_display(window);
	}
}

/* * * * * CURSORS * * * * * */
/*
 * cursor_not_in_display: This forces the cursor out of the display by
 * setting the cursor window to null.  This doesn't actually change the
 * physical position of the cursor, but it will force rite() to reset the
 * cursor upon its next call 
 */
void 	cursor_not_in_display (Screen *s)
{
	if (!s)
		s = output_screen;
	if (s->cursor_window)
		s->cursor_window = NULL;
}

/*
 * cursor_in_display: this forces the cursor_window to be the
 * current_screen->current_window. 
 * It is actually only used in hold.c to trick the system into thinking the
 * cursor is in a window, thus letting the input updating routines move the
 * cursor down to the input line.  Dumb dumb dumb 
 */
void 	cursor_in_display (Window *w)
{
	if (!w)
		w = current_window;
	if (w->screen)
		w->screen->cursor_window = w;
}

/*
 * is_cursor_in_display: returns true if the cursor is in one of the windows
 * (cursor_window is not null), false otherwise 
 */
int 	is_cursor_in_display (Screen *screen)
{
	if (!screen && current_window->screen)
		screen = current_window->screen;

	return (screen->cursor_window ? 1 : 0);
}


/* * * * * * * SCREEN UDPATING AND RESIZING * * * * * * * * */
/*
 * repaint_window_body: redraw the entire window's scrollable region
 * The old logic for doing a partial repaint has been removed with prejudice.
 */
void 	repaint_window_body (Window *window)
{
	Display *curr_line;
	int 	count;

	if (!window)
		window = current_window;

	if (dumb_mode || !window->screen)
		return;

	global_beep_ok = 0;		/* Suppress beeps */

	if (window->scrollback_distance_from_display_ip > window->holding_distance_from_display_ip)
	{
	    if (window->scrolling_distance_from_display_ip >= window->scrollback_distance_from_display_ip)
		curr_line = window->scrolling_top_of_display;
	    else
		curr_line = window->scrollback_top_of_display;
	}
	else
	{
	    if (window->scrolling_distance_from_display_ip >= window->holding_distance_from_display_ip)
		curr_line = window->scrolling_top_of_display;
	    else
		curr_line = window->holding_top_of_display;
	}

	if (window->screen && window->toplines_showing)
	    for (count = 0; count < window->toplines_showing; count++)
	    {
		int	cols;
		int	numls = 1;
		unsigned char **lines;
		unsigned char *n, *widthstr;
		const unsigned char *str;

		if (!(str = window->topline[count]))
			str = empty_string;

		window->screen->cursor_window = window;
		term_move_cursor(0, window->top - window->toplines_showing + count);
		term_clear_to_eol();
		cols = window->columns - 1;

		widthstr = prepare_display2(str, cols, 1, ' ', 0);
		output_with_count(widthstr, 1, foreground);
		new_free(&widthstr);

/*
		n = new_normalize_string(widthstr, 0, display_line_mangler);
		lines = prepare_display(window->refnum, n, cols, 
					&numls, PREPARE_NOWRAP);
		if (*lines)
			output_with_count(*lines, 1, foreground);
		new_free(&n);
*/

		cursor_in_display(window);
	   }

	window->cursor = 0;
	for (count = 0; count < window->display_lines; count++)
	{
		rite(window, curr_line->line);

		/*
		 * Clean off the rest of this window.
		 */
		if (curr_line == window->display_ip)
		{
			window->cursor--;		/* Bumped by rite */
			for (; count < window->display_lines; count++)
			{
				term_clear_to_eol();
				term_newline();
			}
			break;
		}

		curr_line = curr_line->next;
	}

	global_beep_ok = 1;		/* Suppress beeps */
}


/* * * * * * * * * * * * * * SCREEN MANAGEMENT * * * * * * * * * * * * */
/*
 * create_new_screen creates a new screen structure. with the help of
 * this structure we maintain ircII windows that cross screen window
 * boundaries.
 *
 * The new screen is stored in "last_input_screen"!
 */
void	create_new_screen (void)
{
	Screen	*new_s = NULL, *list;
	static	int	refnumber = 0;

	for (list = screen_list; list; list = list->next)
	{
		if (!list->alive)
		{
			new_s = list;
			break;
		}

		if (!list->next)
			break;		/* XXXX */
	}

	if (!new_s)
	{
		new_s = (Screen *)new_malloc(sizeof(Screen));
		new_s->screennum = ++refnumber;
		new_s->next = NULL;
		if (list)
			list->next = new_s;
		else
			screen_list = new_s;
	}

	new_s->last_window_refnum = 1;
	new_s->window_list = NULL;
	new_s->window_list_end = NULL;
	new_s->cursor_window = NULL;
	new_s->current_window = NULL;
	new_s->visible_windows = 0;
	new_s->window_stack = NULL;
	new_s->last_press.tv_sec = new_s->last_press.tv_usec  = 0;
	new_s->last_key = NULL;
	new_s->quote_hit = 0;
	new_s->fdout = 1;
	new_s->fpout = stdout;
#ifdef WITH_THREADED_STDOUT
	new_s->tio_file = tio_open(new_s->fpout);
#endif
	new_s->fdin = 0;
	if (use_input)
		new_open(0, do_screens, NEWIO_READ, 0, -1);
	new_s->fpin = stdin;
	new_s->control = -1;
	new_s->wserv_version = 0;
	new_s->alive = 1;
	new_s->promptlist = NULL;
	new_s->tty_name = (char *) 0;
	new_s->li = current_term->TI_lines;
	new_s->co = current_term->TI_cols;
	new_s->old_li = 0; 
	new_s->old_co = 0;

	new_s->buffer_pos = 0;
	new_s->input_buffer[0] = '\0';
	new_s->input_cursor = 0;
	new_s->input_visible = 0;
	new_s->input_start_zone = 0;
	new_s->input_prompt = malloc_strdup(empty_string);
	new_s->input_prompt_len = 0;
	new_s->input_line = 23;

	last_input_screen = new_s;

	if (!main_screen)
		main_screen = new_s;

	init_input();
}


#ifdef WINDOW_CREATE
#define ST_NOTHING      -1
#define ST_SCREEN       0
#define ST_XTERM        1
Window	*create_additional_screen (void)
{
        Window  	*win;
        Screen  	*oldscreen, *new_s;
        char    	*displayvar,
                	*termvar;
        int     	screen_type = ST_NOTHING;
	ISA		local_sockaddr;
        ISA		new_socket;
	int		new_cmd;
	pid_t		child;
	unsigned short 	port;
	socklen_t		new_sock_size;
	char *		wserv_path;

	if (!use_input)
		return NULL;

	if (!(wserv_path = get_string_var(WSERV_PATH_VAR)))
	{
		say("You need to /SET WSERV_PATH before using /WINDOW CREATE");
		return NULL;
	}

	/*
	 * Environment variable STY has to be set for screen to work..  so it is
	 * the best way to check screen..  regardless of what TERM is, the 
	 * execpl() for screen won't just open a new window if STY isn't set,
	 * it will open a new screen process, and run the wserv in its first
	 * window, not what we want...  -phone
	 */
	if (getenv("STY") && getenv("DISPLAY"))
	{
		char *p = get_string_var(WSERV_TYPE_VAR);
		if (p && !my_stricmp(p, "SCREEN"))
			screen_type = ST_SCREEN;
		else if (p && !my_stricmp(p, "XTERM"))
			screen_type = ST_XTERM;
		else
			screen_type = ST_SCREEN;	/* Sucks to be you */
	}
	else if (getenv("STY"))
		screen_type = ST_SCREEN;
	else if (getenv("DISPLAY") && getenv("TERM"))
		screen_type = ST_XTERM;
	else
	{
		say("I don't know how to create new windows for this terminal");
		return NULL;
	}

	if (screen_type == ST_SCREEN)
		say("Opening new screen...");
	else if (screen_type == ST_XTERM)
	{
		displayvar = getenv("DISPLAY");
		termvar = getenv("TERM");
		say("Opening new window...");
	}
	else
		panic(1, "Opening new wound");

	local_sockaddr.sin_family = AF_INET;
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001
#endif
	local_sockaddr.sin_addr.s_addr = htonl((INADDR_ANY));
	local_sockaddr.sin_port = 0;

	if ((new_cmd = client_bind((SA *)&local_sockaddr, sizeof(local_sockaddr))) < 0)
	{
		yell("Couldn't establish server side of new screen");
		return NULL;
	}
	port = ntohs(local_sockaddr.sin_port);

	oldscreen = current_window->screen;
	create_new_screen();
	new_s = last_input_screen;

	/*
	 * At this point, doing a say() or yell() or anything else that would
	 * output to the screen will cause a refresh of the status bar and
	 * input line.  new_s->current_window is NULL after the above line,
	 * so any attempt to reference $C or $T will be to NULL pointers,
	 * which will cause a crash.  For various reasons, we can't fire up
	 * a new window this early, so its just easier to make sure we don't
	 * output anything until kill_screen() or new_window() is called first.
	 * You have been warned!
	 */
	switch ((child = fork()))
	{
		case -1:
		{
			kill_screen(new_s);
			say("I couldnt fire up a new wserv process");
			break;
		}

		case 0:
		{
			char *opts;
			const char *xterm;
			char *args[64];
			char **args_ptr = args;
			char geom[32];
			int i;

			setuid(getuid());
			setgid(getgid());
			setsid();

			/*
			 * Make sure that no inhereted file descriptors
			 * are left over past the exec.  xterm will reopen
			 * any fd's that it is interested in.
			 * (Start at three sb kanan).
			 */
			for (i = 3; i < 256; i++)
				close(i);

			/*
			 * Try to restore some sanity to the signal
			 * handlers, since theyre not really appropriate here
			 */
			my_signal(SIGINT,  SIG_IGN);
			my_signal(SIGSEGV, SIG_DFL);
			my_signal(SIGBUS,  SIG_DFL);
			my_signal(SIGABRT, SIG_DFL);

			if (screen_type == ST_SCREEN)
			{
			    opts = malloc_strdup(get_string_var(SCREEN_OPTIONS_VAR));
			    *args_ptr++ = malloc_strdup("screen");
			    while (opts && *opts)
				*args_ptr++ = malloc_strdup(new_next_arg(opts, &opts));
			}
			else if (screen_type == ST_XTERM)
			{
			    snprintf(geom, 31, "%dx%d", 
				oldscreen->co + 1, 
				oldscreen->li);

			    opts = malloc_strdup(get_string_var(XTERM_OPTIONS_VAR));
			    if (!(xterm = getenv("XTERM")))
				if (!(xterm = get_string_var(XTERM_VAR)))
				    xterm = "xterm";

			    *args_ptr++ = malloc_strdup(xterm);
			    *args_ptr++ = malloc_strdup("-geometry");
			    *args_ptr++ = malloc_strdup(geom);
			    while (opts && *opts)
				*args_ptr++ = malloc_strdup(new_next_arg(opts, &opts));
			    *args_ptr++ = malloc_strdup("-e");
			}

			*args_ptr++ = malloc_strdup(wserv_path);
			*args_ptr++ = malloc_strdup("localhost");
			*args_ptr++ = malloc_strdup(ltoa((long)port));
			*args_ptr++ = NULL;

			execvp(args[0], args);
			_exit(0);
		}
	}

	/* All the rest of this is the parent.... */
	new_sock_size = sizeof(new_socket);

	/* 
	 * This infinite loop sb kanan to allow us to trap transitory
	 * error signals
	 */
	for (;;)

	/* 
	 * You need to kill_screen(new_s) before you do say() or yell()
	 * if you know what is good for you...
	 */
	switch (my_isreadable(new_cmd, 10))
	{
	    case -1:
	    {
		if ((errno == EINTR) || (errno == EAGAIN))
			continue;
		/* FALLTHROUGH */
	    }
	    case 0:
	    {
		int 	old_errno = errno;
		int 	errnod = get_child_exit(child);

		close(new_cmd);
		kill_screen(new_s);
		kill(child, SIGKILL);
		if (new_s->fdin != 0)
		{
			say("The wserv only connected once -- it's probably "
			    "an old, incompatable version.");
		}

                yell("child %s with %d", (errnod < 1) ? "signaled" : "exited",
                                         (errnod < 1) ? -errnod : errnod);
		yell("Errno is %d", old_errno);
		return NULL;
	    }
	    default:
	    {
		if (new_s->fdin == 0) 
		{
			new_s->fdin = accept(new_cmd, (SA *)&new_socket,
						&new_sock_size);
			if ((new_s->fdout = new_s->fdin) < 0)
			{
				close(new_cmd);
				kill_screen(new_s);
				yell("Couldn't establish data connection "
					"to new screen");
				return NULL;
			}
			new_open(new_s->fdin, do_screens, NEWIO_RECV, 1, -1);
			new_s->fpin = new_s->fpout = fdopen(new_s->fdin, "r+");
#ifdef WITH_THREADED_STDOUT
			new_s->tio_file = tio_open(new_s->fpout);
#endif
			continue;
		}
		else
		{
			new_s->control = accept(new_cmd, (SA *)&new_socket,
						&new_sock_size);
			close(new_cmd);
			if (new_s->control < 0)
			{
                                kill_screen(new_s);
                                yell("Couldn't establish control connection "
                                        "to new screen");
                                return NULL;
                        }

			new_open(new_s->control, do_screens, NEWIO_RECV, 1, -1);

                        if (!(win = new_window(new_s)))
                                panic(1, "WINDOW is NULL and it shouldnt be!");
                        return win;
		}
	    }
	}
	return NULL;
}

/* Old screens never die. They just fade away. */
void 	kill_screen (Screen *screen)
{
	Window	*window;

	if (!screen)
	{
		say("You may not kill the hidden screen.");
		return;
	}
	if (main_screen == screen)
	{
		say("You may not kill the main screen");
		return;
	}
	if (screen->fdin)
	{
		if (use_input)
			screen->fdin = new_close(screen->fdin);
		close(screen->fdout);
		close(screen->fdin);
	}
	if (screen->control)
		screen->control = new_close(screen->control);
	while ((window = screen->window_list))
	{
		screen->window_list = window->next;
		add_to_invisible_list(window);
	}

#ifdef WITH_THREADED_STDOUT
	tio_close(screen->tio_file);
#endif

	/* Take out some of the garbage left around */
	screen->current_window = NULL;
	screen->window_list = NULL;
	screen->window_list_end = NULL;
	screen->cursor_window = NULL;
	screen->last_window_refnum = -1;
	screen->visible_windows = 0;
	screen->window_stack = NULL;
	screen->fpin = NULL;
	screen->fpout = NULL;
	screen->fdin = -1;
	screen->fdout = -1;
	new_free(&screen->input_prompt);

	/* Dont fool around. */
	if (last_input_screen == screen)
		last_input_screen = main_screen;

	screen->alive = 0;
	make_window_current(NULL);
	say("The screen is now dead.");
}
#endif /* WINDOW_CREATE */


/* * * * * * * * * * * * * USER INPUT HANDLER * * * * * * * * * * * */
static void 	do_screens (int fd)
{
	Screen *screen;
	char 	buffer[IO_BUFFER_SIZE + 1];

	if (use_input)
	for (screen = screen_list; screen; screen = screen->next)
	{
		if (!screen->alive)
			continue;

#ifdef WINDOW_CREATE
		/* wserv control */
		if (screen->control != -1 && screen->control == fd)
		{
			if (dgets(screen->control, buffer, IO_BUFFER_SIZE, 1) < 0)
			{
				kill_screen(screen);
				yell("Error from remote screen.");
				continue;
			}

			if (!strncmp(buffer, "tty=", 4))
				malloc_strcpy(&screen->tty_name, buffer + 4);
			else if (!strncmp(buffer, "geom=", 5))
			{
				char *ptr;
				if ((ptr = strchr(buffer, ' ')))
					*ptr++ = 0;
				screen->li = atoi(buffer + 5);
				screen->co = atoi(ptr);
				recalculate_windows(screen);
			}
			else if (!strncmp(buffer, "version=", 8))
			{
				int     version;
				version = atoi(buffer + 8);
				if (version != CURRENT_WSERV_VERSION)
				{
				    yell("WSERV version %d is incompatable with this binary",
						version);
				    kill_screen(screen);
				}
				screen->wserv_version = version;
			}
		}
#endif

		if (screen->fdin == fd)
		{
			int	server;

#ifdef WINDOW_CREATE
			if (screen != main_screen && screen->wserv_version == 0)
			{
				kill_screen(screen);
				yell("The WSERV used to create this new screen is too old.");
				return;
			}
#endif

			/*
			 * This section of code handles all in put from 
			 * the terminal(s) connected to ircII.  Perhaps the 
			 * idle time *shouldn't* be reset unless its not a 
			 * screen-fd that was closed..
			 */
			get_time(&idle_time);
			if (cpu_saver)
				reset_system_timers();

			server = from_server;
			last_input_screen = screen;
			output_screen = screen;
			make_window_current(screen->current_window);
			/*
			 * In a multi-screen environment, it's possible for
			 * the user to "switch" between windows connected to
			 * the same server on multiple screens; this would
			 * be the only place we would know about that.  So 
			 * every time the user presses a key we have to set 
			 * the screen's current window to be that window's
			 * server's current window.  Right.
			 * XXX Why do I know I'm going to regret this?
			 */
			current_window->priority = current_window_priority++;
			from_server = current_window->server;

			if (dumb_mode)
			{
				if (dgets(screen->fdin, buffer, IO_BUFFER_SIZE, 1) < 0)
				{
					say("IRCII exiting on EOF from stdin");
					irc_exit(1, "EPIC - EOF from stdin");
				}

				if (strlen(buffer))
					buffer[strlen(buffer) - 1] = 0;
				parse_statement(buffer, 1, NULL);
			}
			else
			{
				int	n, i;

				/*
				 * Read in from stdin.
				 */
				n = dgets(screen->fdin, buffer,
						BIG_BUFFER_SIZE, -1);
				if (n > 0)
				{
					int proto = get_server_protocol_state(from_server);
					set_server_protocol_state(from_server, 0);

					for (i = 0; i < n; i++)
						edit_char(buffer[i]);
					set_server_protocol_state(from_server, proto);
				}

#ifdef WINDOW_CREATE
				/* 
				 * If this is a /window create screen, then 
				 * an EOF or an error means the screen has
				 * gone away.  We don't need it, so swap out
				 * all of its windows and kill it off.
				 */
				else if (screen != main_screen)
					kill_screen(screen);
#endif

				/*
				 * If we get an EOF or error for the primary 
				 * screen, then we've lost access to the pty
				 * (probably because the user has been logged
				 * out).  There is nothing more we can do but
				 * throw up our hands and quit.
				 */
				else
					irc_exit(1, "Hey!  Where'd my controlling terminal go?");
			}
			from_server = server;
		}
	} 
} 


/* * * * * * * * * INPUT PROMPTS * * * * * * * * * * */
/* 
 * add_wait_prompt:  Given a prompt string, a function to call when
 * the prompt is entered.. some other data to pass to the function,
 * and the type of prompt..  either for a line, or a key, we add 
 * this to the prompt_list for the current screen..  and set the
 * input prompt accordingly.
 *
 * XXXX - maybe this belongs in input.c? =)
 */
void 	add_wait_prompt (const char *prompt, void (*func)(char *, char *), const char *data, int type, int echo)
{
	WaitPrompt **AddLoc,
		   *New;
	Screen *	s;

	if (current_window->screen)
		s = current_window->screen;
	else
		s = main_screen;

	New = (WaitPrompt *) new_malloc(sizeof(WaitPrompt));
	New->prompt = malloc_strdup(prompt);
	New->data = malloc_strdup(data);
	New->type = type;
	New->echo = echo;
	New->func = func;
	New->next = NULL;

	for (AddLoc = &s->promptlist; *AddLoc; AddLoc = &(*AddLoc)->next)
		/* nothing */;
	*AddLoc = New;
	if (AddLoc == &s->promptlist)
		change_input_prompt(1);
}

/*
 * edit_char: handles each character for an input stream.  Not too difficult
 * to work out.
 */
void    edit_char (unsigned char key)
{
        unsigned char          extended_key;
        WaitPrompt *    oldprompt;
        unsigned char          dummy[2];
        int             xxx_return = 0;         /* XXXX Need i say more? */

        if (dumb_mode)
        {
#ifdef TIOCSTI  
                ioctl(0, TIOCSTI, &key);
#else   
                say("Sorry, your system doesnt support 'faking' user input...");
#endif  
                return;
        }

        /* were we waiting for a keypress? */
        if (last_input_screen->promptlist && 
                last_input_screen->promptlist->type == WAIT_PROMPT_KEY)
        {
                dummy[0] = key, dummy[1] = 0;
                oldprompt = last_input_screen->promptlist;
                last_input_screen->promptlist = oldprompt->next;
                (*oldprompt->func)(oldprompt->data, dummy);
                new_free(&oldprompt->data);
                new_free(&oldprompt->prompt);
                new_free((char **)&oldprompt);
 
                set_input(empty_string);
                change_input_prompt(-1);
                xxx_return = 1;
        }

        /*
         * This is only used by /pause to see when a keypress event occurs,
         * but not to impact how that keypress is handled at all.
         */
        if (last_input_screen->promptlist &&
                last_input_screen->promptlist->type == WAIT_PROMPT_DUMMY)
        {
                oldprompt = last_input_screen->promptlist;
                last_input_screen->promptlist = oldprompt->next;
                (*oldprompt->func)(oldprompt->data, NULL);
                new_free(&oldprompt->data);
                new_free(&oldprompt->prompt);
                new_free((char **)&oldprompt);
        }

        if (xxx_return)
                return;

        /* If the high bit is set, mangle it as neccesary. */
        if (key & 0x80 && current_term->TI_meta_mode)
        {
                edit_char('\033');
                key &= ~0x80;
        }

        extended_key = key;

        /* If we just hit the quote character, add this character literally */
        if (last_input_screen->quote_hit)
        {
                last_input_screen->quote_hit = 0;
                input_add_character(extended_key, NULL);
        }

        /* Otherwise, let the keybinding system take care of the work. */
        else {
                last_input_screen->last_key = handle_keypress(
                        last_input_screen->last_key,
                        last_input_screen->last_press, key);
                get_time(&last_input_screen->last_press);
        }
}

