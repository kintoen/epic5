/* $EPIC$ */
/*
 * ruby.c -- Calling RUBY from epic.
 *
 * Copyright � 2006 EPIC Software Labs.
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

#include "irc.h"
#include "ircaux.h"
#include "array.h"
#include "alias.h"
#include "commands.h"
#include "functions.h"
#include "output.h"
#include "if.h"
#include <ruby.h>

VALUE	rubyclass;
int	is_ruby_running = 0;

static VALUE epic_echo (VALUE module, VALUE string)
{
	char *my_string;

	my_string = rb_string_value_cstr(&string);
	yell("%s", my_string);
	return Qnil;
}

static VALUE epic_say (VALUE module, VALUE string)
{
	char *my_string;

	my_string = rb_string_value_cstr(&string);
	say("%s", my_string);
	return Qnil;
}

static VALUE epic_cmd (VALUE module, VALUE string)
{
	char *my_string;

	my_string = rb_string_value_cstr(&string);
	runcmds("$*", my_string);
	return Qnil;
}

static VALUE epic_eval (VALUE module, VALUE string)
{
	char *my_string;

	my_string = rb_string_value_cstr(&string);
	runcmds(my_string, "");
	return Qnil;
}

static VALUE epic_expr (VALUE module, VALUE string)
{
	char *my_string;
	char *exprval;
	VALUE retval;

	my_string = rb_string_value_cstr(&string);
	exprval = parse_inline(my_string, "");
	return rb_str_new(exprval, strlen(exprval));
}

static VALUE epic_call (VALUE module, VALUE string)
{
	char *my_string;
	char *funcval;
	VALUE retval;

	my_string = rb_string_value_cstr(&string);
	funcval = call_function(my_string, "");
	return rb_str_new(funcval, strlen(funcval));
}

/* Called by the epic hooks to activate tcl on-demand. */
void ruby_start (void)
{
	if (is_ruby_running)
		return ;

	++is_ruby_running;
	ruby_init();
	ruby_script(irc_version);
	rubyclass = rb_define_class("EPIC", rb_cObject);
	rb_define_singleton_method(rubyclass, "echo", epic_echo, 1);
	rb_define_singleton_method(rubyclass, "say", epic_say, 1);
	rb_define_singleton_method(rubyclass, "cmd", epic_cmd, 1);
	rb_define_singleton_method(rubyclass, "eval", epic_eval, 1);
	rb_define_singleton_method(rubyclass, "expr", epic_expr, 1);
	rb_define_singleton_method(rubyclass, "call", epic_call, 1);
	rb_gc_register_address(&rubyclass);

}

/*
 * Used by the $ruby(...) function: evalulate ... as a RUBY statement, and 
 * return the result of the statement.
 */
char *	rubyeval (char *input)
{
	char *retval;
	VALUE rubyval;
	int	foo;

	if (input && *input) 
	{
		ruby_start();
		rubyval = rb_eval_string_protect(input, &foo);
		if (rubyval == Qnil)
			retval = NULL;
		else
			retval = rb_string_value_cstr(&rubyval);
	}

	RETURN_STR(retval);	/* XXX Is this malloced or not? */
}

/*
 * The /RUBY function: Evalulate the args as a RUBY block and ignore the 
 * return value of the statement.
 */
BUILT_IN_COMMAND(rubycmd)
{
        char *body, *x;

        if (*args == '{')
        {
                if (!(body = next_expr(&args, '{')))
                {
                        error("RUBY: unbalanced {");
                        return;
                }
        }
        else
                body = args;

        x = rubyeval(body);
        new_free(&x);
}

