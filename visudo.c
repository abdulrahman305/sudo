/*
 * Copyright (c) 1996, 1998-2000 Todd C. Miller <Todd.Miller@courtesan.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * 4. Products derived from this software may not be called "Sudo" nor
 *    may "Sudo" appear in their names without specific prior written
 *    permission from the author.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Lock the sudoers file for safe editing (ala vipw) and check for parse errors.
 */

#include "config.h"

#include <stdio.h>
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif /* STDC_HEADERS */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif /* HAVE_STRINGS_H */
#if defined(HAVE_MALLOC_H) && !defined(STDC_HEADERS)
#include <malloc.h>
#endif /* HAVE_MALLOC_H && !STDC_HEADERS */
#include <ctype.h>
#include <pwd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>

#include "sudo.h"
#include "version.h"

#ifndef STDC_HEADERS
#ifndef __GNUC__	/* gcc has its own malloc */
extern char *malloc	__P((size_t));
#endif /* __GNUC__ */
extern char *getenv	__P((const char *));
extern int stat		__P((const char *, struct stat *));
#endif /* !STDC_HEADERS */

#ifndef lint
static const char rcsid[] = "$Sudo$";
#endif /* lint */

/*
 * Function prototypes
 */
static void usage		__P((void));
static char whatnow		__P((void));
static RETSIGTYPE Exit		__P((int));
static void setup_signals	__P((void));
static int run_command		__P((char *, char **));
int command_matches		__P((char *, char *, char *, char *));
int addr_matches		__P((char *));
int hostname_matches		__P((char *, char *, char *));
int netgr_matches		__P((char *, char *, char *, char *));
int usergr_matches		__P((char *, char *));
void init_parser		__P((void));
void yyrestart			__P((FILE *));

/*
 * External globals exported by the parser
 */
extern FILE *yyin, *yyout;
extern int errorlineno;
extern int pedantic;

/*
 * Globals
 */
char **Argv;
char *sudoers = _PATH_SUDOERS;
char *stmp = _PATH_SUDOERS_TMP;
struct sudo_user sudo_user;
int parse_error = FALSE;


int
main(argc, argv)
    int argc;
    char **argv;
{
    char buf[MAXPATHLEN*2];		/* buffer used for copying files */
    char *Editor;			/* editor to use */
    char *UserEditor;			/* editor user wants to use */
    char *EditorPath;			/* colon-separated list of editors */
    char *av[4];			/* argument vector for run_command */
    int sudoers_fd;			/* sudoers file descriptor */
    int stmp_fd;			/* stmp file descriptor */
    int n;				/* length parameter */
    time_t now;				/* time now */
    struct stat stmp_sb, sudoers_sb;	/* to check for changes */

    /* Warn about aliases that are used before being defined. */
    pedantic = 1;

    /*
     * Parse command line options
     */
    Argv = argv;

    /*
     * Arg handling.
     */
    while (--argc) {
	if (!strcmp(argv[argc], "-V")) {
	    (void) printf("visudo version %s\n", version);
	    exit(0);
	} else if (!strcmp(argv[argc], "-s")) {
	    pedantic++;			/* strict mode */
	} else {
	    usage();
	}
    }

    /* Mock up a fake sudo_user struct. */
    user_host = user_shost = user_cmnd = "";
    if ((sudo_user.pw = getpwuid(getuid())) == NULL) {
	(void) fprintf(stderr, "%s: Can't find you in the passwd database.\n",
	    Argv[0]);
	exit(1);
    }

    /* Setup defaults data structures. */
    init_defaults();

    /*
     * Open sudoers, lock it and stat it.  
     * sudoers_fd must remain open throughout in order to hold the lock.
     */
    sudoers_fd = open(sudoers, O_RDWR | O_CREAT, SUDOERS_MODE);
    if (sudoers_fd == -1) {
	(void) fprintf(stderr, "%s: %s: %s\n", Argv[0], sudoers,
	    strerror(errno));
	Exit(-1);
    }
    if (!lock_file(sudoers_fd, SUDO_TLOCK)) {
	(void) fprintf(stderr, "%s: sudoers file busy, try again later.\n",
	    Argv[0]);
	exit(1);
    }
#ifdef HAVE_FSTAT
    if (fstat(sudoers_fd, &sudoers_sb) == -1) {
#else
    if (stat(sudoers, &sudoers_sb) == -1) {
#endif
	(void) fprintf(stderr, "%s: can't stat %s: %s\n",
	    Argv[0], sudoers, strerror(errno));
	Exit(-1);
    }

    /*
     * Open sudoers temp file.
     */
    stmp_fd = open(stmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (stmp_fd < 0) {
	(void) fprintf(stderr, "%s: %s: %s\n", Argv[0], stmp, strerror(errno));
	exit(1);
    }

    /* Install signal handlers to clean up stmp if we are killed. */
    setup_signals();

    /* Copy sudoers -> stmp and reset the mtime */
    if (sudoers_sb.st_size) {
	while ((n = read(sudoers_fd, buf, sizeof(buf))) > 0)
	    if (write(stmp_fd, buf, n) != n) {
		(void) fprintf(stderr, "%s: Write failed: %s\n", Argv[0],
		strerror(errno));
		Exit(-1);
	    }

	(void) close(stmp_fd);
	(void) touch(stmp, sudoers_sb.st_mtime);

	/* Parse sudoers to pull in editor and env_editor conf values. */
	if ((yyin = fopen(stmp, "r"))) {
	    yyout = stdout;
	    init_parser();
	    yyparse();
	    parse_error = FALSE;
	    yyrestart(yyin);
	    fclose(yyin);
	}
    } else
	(void) close(stmp_fd);

    /*
     * Check EDITOR and VISUAL environment variables to see which editor
     * the user wants to use (we may not end up using it though).
     * If the path is not fully-qualified, make it so and check that
     * the specified executable actually exists.
     */
    if ((UserEditor = getenv("EDITOR")) == NULL || *UserEditor == '\0')
	UserEditor = getenv("VISUAL");
    if (UserEditor && *UserEditor == '\0')
	UserEditor = NULL;
    else if (UserEditor) {
	if (find_path(UserEditor, &Editor, getenv("PATH")) == FOUND) {
	    UserEditor = Editor;
	} else {
	    if (def_flag(I_ENV_EDITOR)) {
		/* If we are honoring $EDITOR this is a fatal error. */
		(void) fprintf(stderr,
		    "%s: specified editor (%s) doesn't exist!\n",
		    Argv[0], UserEditor);
		Exit(-1);
	    } else {
		/* Otherwise, just ignore $EDITOR. */
		UserEditor = NULL;
	    }
	}
    }

    /*
     * See if we can use the user's choice of editors either because
     * we allow any $EDITOR or because $EDITOR is in the allowable list.
     */
    Editor = NULL;
    if (def_flag(I_ENV_EDITOR) && UserEditor)
	Editor = UserEditor;
    else if (UserEditor) {
	struct stat editor_sb;
	struct stat user_editor_sb;
	char *base, *userbase;

	if (stat(UserEditor, &user_editor_sb) != 0) {
	    /* Should never happen since we already checked above. */
	    (void) fprintf(stderr, "%s: unable to stat editor (%s): %s\n",
		Argv[0], UserEditor, strerror(errno));
	    Exit(-1);
	}
	EditorPath = estrdup(def_str(I_EDITOR));
	Editor = strtok(EditorPath, ":");
	do {
	    /*
	     * Both Editor and UserEditor should be fully qualified but
	     * check anyway...
	     */
	    if ((base = strrchr(Editor, '/')) == NULL)
		continue;
	    if ((userbase = strrchr(UserEditor, '/')) == NULL) {
		Editor = NULL;
		break;
	    }
	    base++, userbase++;

	    /*
	     * We compare the basenames first and then use stat to match
	     * for sure.
	     */
	    if (strcmp(base, userbase) == 0) {
		if (stat(Editor, &editor_sb) == 0 && S_ISREG(editor_sb.st_mode)
		    && (editor_sb.st_mode & 0000111) &&
		    editor_sb.st_dev == user_editor_sb.st_dev &&
		    editor_sb.st_ino == user_editor_sb.st_ino)
		    break;
	    }
	} while ((Editor = strtok(NULL, ":")));
	free(EditorPath);
    }

    /*
     * Can't use $EDITOR, try each element of I_EDITOR until we
     * find one that exists, is regular, and is executable.
     */
    if (Editor == NULL || *Editor == '\0') {
	EditorPath = estrdup(def_str(I_EDITOR));
	Editor = strtok(EditorPath, ":");
	do {
	    if (sudo_goodpath(Editor))
		break;
	} while ((Editor = strtok(NULL, ":")));

	/* Bleah, none of the editors existed! */
	if (Editor == NULL || *Editor == '\0') {
	    (void) fprintf(stderr, "%s: no editor found (editor path = %s)\n",
		Argv[0], def_str(I_EDITOR));
	    Exit(-1);
	}
    }

    /*
     * Edit the temp file and parse it (for sanity checking)
     */
    do {
	char linestr[64];

	/* Build up argument vector for the command */
	if ((av[0] = strrchr(Editor, '/')) != NULL)
	    av[0]++;
	else
	    av[0] = Editor;
	n = 1;
	if (parse_error == TRUE) {
	    (void) snprintf(linestr, sizeof(linestr), "+%d", errorlineno);
	    av[n++] = linestr;
	}
	av[n++] = stmp;
	av[n++] = NULL;

	/* Do the edit -- some SYSV editors exit with 1 instead of 0 */
	now = time(NULL);
	n = run_command(Editor, av);
	if (n != -1 && ((n >> 8) == 0 || (n >> 8) == 1)) {
	    /*
	     * Sanity checks.
	     */
	    if (stat(stmp, &stmp_sb) < 0) {
		(void) fprintf(stderr,
		    "%s: Can't stat temporary file (%s), %s unchanged.\n",
		    Argv[0], stmp, sudoers);
		Exit(-1);
	    }
	    if (stmp_sb.st_size == 0) {
		(void) fprintf(stderr,
		    "%s: Zero length temporary file (%s), %s unchanged.\n",
		    Argv[0], stmp, sudoers);
		Exit(-1);
	    }

	    /*
	     * Passed sanity checks so reopen stmp file and check
	     * for parse errors.
	     */
	    yyout = stdout;
	    if (parse_error)
		yyin = freopen(stmp, "r", yyin);
	    else
		yyin = fopen(stmp, "r");
	    if (yyin == NULL) {
		(void) fprintf(stderr,
		    "%s: Can't re-open temporary file (%s), %s unchanged.\n",
		    Argv[0], stmp, sudoers);
		Exit(-1);
	    }

	    /* Clean slate for each parse */
	    user_runas = NULL;
	    init_defaults();
	    init_parser();

	    /* Parse the sudoers file */
	    if (yyparse() && parse_error != TRUE) {
		(void) fprintf(stderr,
		    "%s: Failed to parse temporary file (%s), unknown error.\n",
		    Argv[0], stmp);
		parse_error = TRUE;
	    }
	} else {
	    (void) fprintf(stderr,
		"%s: Editor (%s) failed with exit status %d, %s unchanged.\n",
		Argv[0], Editor, n, sudoers);
	    Exit(-1);
	}

	/*
	 * Got an error, prompt the user for what to do now
	 */
	if (parse_error == TRUE) {
	    switch (whatnow()) {
		case 'Q' :	parse_error = FALSE;	/* ignore parse error */
				break;
		case 'x' :	if (sudoers_sb.st_size == 0)
				    unlink(sudoers);
				Exit(0);
				break;
	    }
	    yyrestart(yyin);	/* reset lexer */
	}
    } while (parse_error == TRUE);

    /*
     * If the user didn't change the temp file, just unlink it.
     */
    if (sudoers_sb.st_mtime != now && sudoers_sb.st_mtime == stmp_sb.st_mtime &&
	sudoers_sb.st_size == stmp_sb.st_size) {
	(void) fprintf(stderr, "%s: sudoers file unchanged.\n", Argv[0]);
	Exit(0);
    }

    /*
     * Change mode and ownership of temp file so when
     * we move it to sudoers things are kosher.
     */
    if (chown(stmp, SUDOERS_UID, SUDOERS_GID)) {
	(void) fprintf(stderr,
	    "%s: Unable to set (uid, gid) of %s to (%d, %d): %s\n",
	    Argv[0], stmp, SUDOERS_UID, SUDOERS_GID, strerror(errno));
	Exit(-1);
    }
    if (chmod(stmp, SUDOERS_MODE)) {
	(void) fprintf(stderr,
	    "%s: Unable to change mode of %s to %o: %s\n",
	    Argv[0], stmp, SUDOERS_MODE, strerror(errno));
	Exit(-1);
    }

    /*
     * Now that we have a sane stmp file (parses ok) it needs to be
     * rename(2)'d to sudoers.  If the rename(2) fails we try using
     * mv(1) in case stmp and sudoers are on different filesystems.
     */
    if (rename(stmp, sudoers)) {
	if (errno == EXDEV) {
	    (void) fprintf(stderr,
	      "%s: %s and %s not on the same filesystem, using mv to rename.\n",
	      Argv[0], stmp, sudoers);

	    /* Build up argument vector for the command */
	    if ((av[0] = strrchr(_PATH_MV, '/')) != NULL)
		av[0]++;
	    else
		av[0] = _PATH_MV;
	    av[1] = stmp;
	    av[2] = sudoers;
	    av[3] = NULL;

	    /* And run it... */
	    if (run_command(_PATH_MV, av)) {
		(void) fprintf(stderr,
			       "%s: Command failed: '%s %s %s', %s unchanged.\n",
			       Argv[0], _PATH_MV, stmp, sudoers, sudoers);
		Exit(-1);
	    }
	} else {
	    (void) fprintf(stderr, "%s: Error renaming %s, %s unchanged: %s\n",
				   Argv[0], stmp, sudoers, strerror(errno));
	    Exit(-1);
	}
    }

    exit(0);
}

/*
 * Dummy *_matches routines.
 * These exist to allow us to use the same parser as sudo(8).
 */
int
command_matches(cmnd, cmnd_args, path, sudoers_args)
    char *cmnd;
    char *cmnd_args;
    char *path;
    char *sudoers_args;
{
    return(TRUE);
}

int
addr_matches(n)
    char *n;
{
    return(TRUE);
}

int
hostname_matches(s, l, p)
    char *s, *l, *p;
{
    return(TRUE);
}

int
usergr_matches(g, u)
    char *g, *u;
{
    return(TRUE);
}

int
netgr_matches(n, h, sh, u)
    char *n, *h, *sh, *u;
{
    return(TRUE);
}

void
set_fqdn()
{
    return;
}

int
user_is_exempt()
{
    return(TRUE);
}

/*
 * Assuming a parse error occurred, prompt the user for what they want
 * to do now.  Returns the first letter of their choice.
 */
static char
whatnow()
{
    int choice, c;

    for (;;) {
	(void) fputs("What now? ", stdout);
	choice = getchar();
	for (c = choice; c != '\n' && c != EOF;)
	    c = getchar();

	switch (choice) {
	    case EOF:
		choice = 'x';
		/* FALLTHROUGH */
	    case 'e':
	    case 'x':
	    case 'Q':
		return(choice);
	    default:
		(void) puts("Options are:");
		(void) puts("  (e)dit sudoers file again");
		(void) puts("  e(x)it without saving changes to sudoers file");
		(void) puts("  (Q)uit and save changes to sudoers file (DANGER!)\n");
	}
    }
}

/*
 * Install signal handlers for visudo.
 */
static void
setup_signals()
{
#ifdef POSIX_SIGNALS
	struct sigaction action;		/* POSIX signal structure */
#endif /* POSIX_SIGNALS */

	/*
	 * Setup signal handlers to cleanup nicely.
	 */
#ifdef POSIX_SIGNALS
	(void) memset((VOID *)&action, 0, sizeof(action));
	sigemptyset(&sact.sa_mask);
	action.sa_flags = 0;
	action.sa_handler = Exit;
	(void) sigaction(SIGTERM, &action, NULL);
	(void) sigaction(SIGHUP, &action, NULL);
	(void) sigaction(SIGINT, &action, NULL);
	(void) sigaction(SIGQUIT, &action, NULL);
#else
	(void) signal(SIGTERM, Exit);
	(void) signal(SIGHUP, Exit);
	(void) signal(SIGINT, Exit);
	(void) signal(SIGQUIT, Exit);
#endif /* POSIX_SIGNALS */
}

static int
run_command(path, argv)
    char *path;
    char **argv;
{
    int status;
    pid_t pid;
#ifndef POSIX_SIGNALS
    int omask = sigblock(sigmask(SIGCHLD));
#else
    sigset_t set, oset;

    (void) sigemptyset(&set);
    (void) sigaddset(&set, SIGCHLD);
    (void) sigprocmask(SIG_BLOCK, &set, &oset);
#endif /* POSIX_SIGNALS */

    switch (pid = fork()) {
	case -1:
	    (void) fprintf(stderr,
		"%s: unable to run %s: %s\n", Argv[0], path, strerror(errno));
	    Exit(-1);
	    break;	/* NOTREACHED */
	case 0:
#ifdef POSIX_SIGNALS
	    (void) sigprocmask(SIG_SETMASK, &oset, NULL);
#else
	    (void) sigsetmask(omask);
#endif /* POSIX_SIGNALS */
	    execv(path, argv);
	    (void) fprintf(stderr,
		"%s: unable to run %s: %s\n", Argv[0], path, strerror(errno));
	    _exit(127);
	    break;	/* NOTREACHED */
    }

#ifdef sudo_waitpid
    pid = sudo_waitpid(pid, &status, 0);
#else
    pid = wait(&status);
#endif

#ifdef POSIX_SIGNALS
    (void) sigprocmask(SIG_SETMASK, &oset, NULL);
#else
    (void) sigsetmask(omask);
#endif /* POSIX_SIGNALS */

    if (pid == -1)
	status = -1;
    return(status);
}

/*
 * Unlink the sudoers temp file (if it exists) and exit.
 * Used in place of a normal exit() and as a signal handler.
 * A positive parameter is considered to be a signal and is reported.
 */
static RETSIGTYPE
Exit(sig)
    int sig;
{
#ifdef POSIX_SIGNALS
    sigset_t mask;

    sigfillset(&mask);
    (void) sigprocmask(SIG_BLOCK, &mask, NULL);
#else
    (void) sigblock(~0);
#endif /* POSIX_SIGNALS */

    (void) unlink(stmp);

    if (sig > 0)
	(void) fprintf(stderr, "%s exiting, caught signal %d.\n", Argv[0], sig);

    exit(-sig);
}

static void
usage()
{
    (void) fprintf(stderr, "usage: %s [-s] [-V]\n", Argv[0]);
    exit(1);
}
