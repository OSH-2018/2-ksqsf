#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Constants */
#define MAX_CMDLINE 4096
#define MAX_ARGS    256

/* Markers */
#define owned

/* boolean */
typedef int BOOL;
#define TRUE (!0)
#define FALSE (0)

/* Helper */
static void safe_exit(int status);

/* Command interpretation */
char  cmdline[MAX_CMDLINE]; /* Original cmdline; will be modified */
int   argc;                 /* Number of parts */
char *argv[MAX_ARGS];       /* Parts taken apart */
static void prompt(void);
static void parse(void);
static int  run(void);

/* Built-in commands */
typedef int (*builtin_cmd)(char **args);
static int cmd_cd(char **args);
static int cmd_pwd(char **args);
static int cmd_exit(char **args);
static int cmd_export(char **args);
static int cmd_unset(char **args);
struct builtin {
  const char  *name;
  builtin_cmd  func;
} builtins[] = {
  { "cd",   cmd_cd },
  { "pwd",  cmd_pwd },
  { "exit", cmd_exit },
  { "export", cmd_export },
  { "unset", cmd_unset },
  { NULL, NULL },
};

/* Variable */
struct variable {
  struct variable *next;
  owned char      *name;
  owned char      *value;
  BOOL             exported;
} *vlist;
static void set(const char *name, const char *value, BOOL export);
static void unset(const char *name);

/* Rea1 C0DE Beg1ns here! :-) */
int
main(int optc, char *optv[]) {
  /* Initialize */
  vlist = NULL;

  /* META-LOOP! */
  for (;;) {
    /* Read */
    prompt();
    if (!fgets(cmdline, MAX_CMDLINE, stdin)) {
      /* Exit on Ctrl-D */
      putchar('\n');
      safe_exit(0);
      continue;
    }

    /* Parse and run */
    parse();
    run();
  }
}

/* Exit only if shell doesn't act as init */
static void
safe_exit(int status) {
  if (getpid() == 1)
    fprintf(stderr, "init shouldn't exit\n");
  else
    exit(status);
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
static int
run() {
  int i;
  pid_t pid;

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
  pid = fork();
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
    if (!args[0]) {
      fprintf(stderr, "HOME is not set, exiting\n");
      return 1;
    }
  }

  if (chdir(args[0])) {
    perror("Couldn't change directory");
    return 1;
  }

  return 0;
}

/* Variable */
static struct variable *
var_find(const char *name) {
  struct variable *v = vlist;
  while (v && strcmp(name, v->name))
    v = v->next;
  return v;
}

/* Create a new variable, or modify an existing one */
static void
set(const char *name, const char *value, BOOL export) {
  struct variable *v = var_find(name);

  if (v) {
    free(v->value);
    v->value = strdup(value);
  } else {
    v = malloc(sizeof(struct variable));
    v->name = strdup(name);
    v->value = strdup(value);
    v->next = vlist;
    v->exported = export;
    vlist = v;
  }

  /* Environment vars should follow the changes */
  if (v->exported || export)
    setenv(v->name, v->value, TRUE);
}

/* Remove a variable from the variable list */
static void
unset(const char *name) {
  struct variable *v = vlist, *u;

  while (v && v->next) {
    if (!strcmp(v->next->name, name))
      goto remove;
    else
      v = v->next;
  }
  return;

 remove:
  u = v->next;
  if (u->exported)
    unsetenv(u->name);
  v->next = u->next;
  free(u->name);
  free(u->value);
  free(u);
}

/* Builtin Commands */
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
  safe_exit(0);
  return 0;
}

/* Expose a variable to children */
static int
cmd_export(char **args) {
  for (; *args; ++args) {
    char *p;

    for (p = *args; *p && *p != '='; ++p);
    if (*p == '=') {
      *p = '\0';
      set(*args, p+1, TRUE);
    } else {
      struct variable *v = var_find(*args);

      if (v) {
	v->exported = TRUE;
	setenv(v->name, v->value, TRUE);
      }
    }
  }

  return 0;
}

/* Unset a variable */
static int
cmd_unset(char **args) {
  for (; *args; ++args) {
    unset(*args);

    /* Some environment variables are inherited from parent.
       Make sure they are unset. */
    unsetenv(*args);
  }

  return 0;
}
