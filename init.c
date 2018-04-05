/* simplistic shell that supports bare minimum subset of POSIX shell */

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_CMDLINE 4096
#define MAX_ARGS    256

char cmdline[MAX_CMDLINE];
int   argc;
char *argv[255];

static void prompt(void);
static void parse(void);
static int  run(void);

typedef int (*builtin_cmd)(char **args);
static int cmd_cd(char **args);
static int cmd_pwd(char **args);
static int cmd_exit(char **args);
struct builtin {
  char        *name;
  builtin_cmd  func;
} builtins[] = {
  { "cd",   cmd_cd },
  { "pwd",  cmd_pwd },
  { "exit", cmd_exit },
  { NULL, NULL },
};

int
main(int optc, char *optv[]) {
  /* META-LOOP! */
  for (;;) {
    argc = 0;

    /* Read */
    prompt();
    if (!fgets(cmdline, MAX_CMDLINE, stdin)) {
      /* Exit on Ctrl-D */
      putchar('\n');
      exit(0);
    }

    /* Parse and run */
    parse();
    run();
  }
}

/* Ready for a new command, printing prompt */
static void
prompt() {
  char *PS1 = getenv("PS1");
  static char *default_PS1;

  default_PS1 = geteuid()? "$ ": "# ";
  PS1 = PS1 ? PS1 : default_PS1;
  fprintf(stderr, "%s", PS1);
  fflush(stderr);
}

/* Parse the command line and get parts out of it */
static void
parse() {
  char *p = cmdline;

  for (p = cmdline; *p && *p != '\n'; ++p) {
    int i;

    /* Skip whitespace */
    if (isspace(*p)) continue;

    /* Borrow until whitespace */
    argv[argc++] = p;
    for (i = 0; p[i] && !isspace(p[i]); ++i);
    p[i] = 0;
    p += i;
  }
  argv[argc] = 0;

#ifdef DEBUG
  printf("argc = %d\n", argc);
  for (int i = 0; argv[i] != 0; ++i) {
    printf("argv[%d] = %s\n", i, argv[i]);
  }
#endif
}

/* Run the commandline */
int
run() {
  int i;

  /* Empty command? */
  if (!argv[0])
    return 0;

  /* Check whether it's a builtin command */
  for (i = 0; builtins[i].name; ++i) {
    if (!strcmp(builtins[i].name, argv[0])) {
      return (builtins[i].func)(argv + 1);
    }
  }

  /* External commands */
  pid_t pid = fork();
  if (pid < 0) {
    perror("Couldn't create process");
    return 1;
  } else if (pid == 0) {
    /* Child */
    execvp(argv[0], argv);
    fprintf(stderr, "Couldn't execute command '%s': ", argv[0]);
    perror("");
    return 127;
  } else {
    return wait(NULL);
  }
}

/* Change current directory */
static int
cmd_cd(char **args) {
  if (!args[0]) {
    args[0] = getenv("HOME");
    fprintf(stderr, "HOME is not set, exiting\n");
    return 1;
  }

  if (chdir(args[0])) {
    perror("Couldn't change directory");
    return 1;
  }

  return 0;
}

/* Print current working directory */
static int
cmd_pwd(char **args) {
  char wd[4096];

  if (!getcwd(wd, 4096)) {
    perror("Couldn't print current working directory");
    return errno;
  }

  puts(wd);
  return 0;
}

/* Exit gracefully. */
static int
cmd_exit(char **args) {
  exit(0);
  return 0;
}
