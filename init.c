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
char input[MAX_CMDLINE];
struct command {
  struct command *next;
  char  cmdline[MAX_CMDLINE]; /* Original cmdline; will be modified */
  int   argc;                 /* Number of parts */
  char *argv[MAX_ARGS];       /* Parts taken apart */
  pid_t pid;
  int status;
  BOOL completed;
} *clist;
static void prompt(void);
static void parse(void);
static int  run(void);
static void wait_for_completion();

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
int main(int optc, char *optv[]) {
  /* Initialize */
  vlist = NULL;
  clist = NULL;

  /* META-LOOP! */
  for (;;) {
    struct command *tmp;

    /* Read */
    prompt();
    if (!fgets(input, MAX_CMDLINE, stdin)) {
      /* Exit on Ctrl-D */
      putchar('\n');
      safe_exit(0);
      continue;
    }

    /* Parse and run */
    parse();
    if (run())
      wait_for_completion();

    /* Clean */
    for (tmp = clist; tmp; ) {
      struct command *next = tmp->next;
      free(tmp);
      tmp = next;
    }
    clist = NULL;
  }
}

/* Exit only if shell doesn't act as init */
static void safe_exit(int status) {
  if (getpid() == 1)
    fprintf(stderr, "init shouldn't exit\n");
  else
    exit(status);
}

/* Ready for a new command, printing prompt */
static void prompt() {
  char *PS1 = getenv("PS1");
  static char *default_PS1;

  default_PS1 = geteuid()? "$ ": "# ";
  PS1 = PS1 ? PS1 : default_PS1;
  fprintf(stderr, "%s", PS1);
  fflush(stderr);
}

/* Parse struct command */
static void
parse_cmd(struct command *cmd) {
  char *p;

  cmd->argc = 0;
  for (p = cmd->cmdline; *p && *p != '\n'; ++p) {
    int i;

    /* Skip whitespace */
    if (isspace(*p)) continue;

    /* Borrow until whitespace */
    cmd->argv[cmd->argc++] = p;
    for (i = 0; p[i] && !isspace(p[i]); ++i);
    p[i] = 0;
    p += i;
  }
  cmd->argv[cmd->argc] = 0;

  if (cmd->argv[0] == 0) {
    cmd->argc = 1;
    cmd->argv[0] = cmd->cmdline;
    cmd->argv[1] = NULL;
    cmd->cmdline[0] = 0;
  }

#ifdef DEBUG
  printf("DBG: argc = %d\n", cmd->argc);
  for (int i = 0; cmd->argv[i] != 0; ++i) {
    printf("DBG: argv[%d] = %s\n", i, cmd->argv[i]);
  }
#endif
}

/* Parse the input and get parts out of it */
static void parse() {
  char *p;
  int i;

  for (p = input; *p && *p != '\n'; ++p) {
    struct command *cmd;

    /* Create a new command node */
    cmd = malloc(sizeof(struct command));
    for (i = 0; *p && *p != '\n' && *p != '|'; )
      cmd->cmdline[i++] = *p++;
    cmd->cmdline[i] = '\0';
    cmd->cmdline[i+1] = '\0';
    cmd->argc = 0;
    cmd->next = NULL;
    cmd->completed = FALSE;
    cmd->pid = 0;

    /* Parse it */
    parse_cmd(cmd);

    /* Insert current cmd into clist */
    if (!clist) {
      clist = cmd;
      clist->next = NULL;
    } else {
      struct command *tail;
      for (tail = clist; tail->next; tail = tail->next);
      tail->next = cmd;
    }
  }
}

/* Launch an command */
static void launch_command(struct command *cmd, int infd, int outfd) {
  int i;

  if (cmd->argv[0] == NULL)
    exit(255);

  if (infd != STDIN_FILENO) {
    dup2(infd, STDIN_FILENO);
    close(infd);
  }
  if (outfd != STDOUT_FILENO) {
    dup2(outfd, STDOUT_FILENO);
    close(outfd);
  }

  for (i = 0; builtins[i].name; ++i) {
    if (!strcmp(builtins[i].name, cmd->argv[0])) {
      exit((builtins[i].func)(cmd->argv + 1));
    }
  }

  execvp(cmd->argv[0], cmd->argv);
  fprintf(stderr, "Couldn't execute command '%s': ", cmd->argv[0]);
  perror("");
  exit(255);
}

/* Run job
   Returns 1 when new children are forked */
static int run() {
  pid_t pid;
  int pipefd[2];
  int infd = STDIN_FILENO, outfd = STDOUT_FILENO;
  struct command *c;

  if (!clist)
    return 0;

  /* Only one command */
  if (clist->next == NULL) {
    int i;
    for (i = 0; builtins[i].name; ++i) {
      /* If it's a builtin, don't fork! */
      if (!strcmp(builtins[i].name, clist->argv[0])) {
	(builtins[i].func)(clist->argv + 1);
	return 0;
      }
    }
  }

  /* Pipeline */
  for (c = clist; c; c = c->next) {
    /* Set up pipes if c is not the last */
    if (c->next) {
      if (pipe(pipefd) < 0) {
	perror("Couldn't create pipes");
	return 1;
      }
      outfd = pipefd[1];
    }

    /* Create child processes */
    pid = fork();
    if (pid == 0)
      launch_command(c, infd, outfd);
    else if (pid < 0) {
      perror("Couldn't create child process");
      return 1;
    } else {
      c->pid = pid;
    }

    /* Close unused fds */
    if (infd != STDIN_FILENO)
      close(infd);
    if (outfd != STDOUT_FILENO)
      close(outfd);
    infd = pipefd[0];
  }
  return 1;
}

/* Wait for the completion of the pipeline */
static void wait_for_completion() {
  BOOL all_completed;
  int status;
  pid_t pid;
  struct command *c;

  while (TRUE) {
    pid = waitpid(WAIT_ANY, &status, WUNTRACED);

    /* Mark PID as completed */
    for (c = clist; c; c = c->next) {
      if (c->pid == pid) {
	c->status = status;
	c->completed = TRUE;
      }
    }

    /* Check whether all processes completed */
    all_completed = TRUE;
    for (c = clist; c; c = c->next)
      all_completed &= c->completed || (c->pid == 0);
    if (all_completed)
      break;
  }
}

/* Change current directory */
static int cmd_cd(char **args) {
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

/* Find the variable node */
static struct variable *var_find(const char *name) {
  struct variable *v = vlist;
  while (v && strcmp(name, v->name))
    v = v->next;
  return v;
}

/* Create a new variable, or modify an existing one.
   If export is TRUE or the variable is exported, 
   the environment variable will be changed as well. */
static void set(const char *name, const char *value, BOOL export) {
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
static void unset(const char *name) {
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
static int cmd_pwd(char **args) {
  char wd[4096];

  if (!getcwd(wd, 4096)) {
    perror("Couldn't print current working directory");
    return errno;
  }

  puts(wd);
  return 0;
}

/* Exit gracefully. */
static int cmd_exit(char **args) {
  safe_exit(0);
  return 0;
}

/* Expose a variable to children */
static int cmd_export(char **args) {
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
static int cmd_unset(char **args) {
  for (; *args; ++args) {
    unset(*args);

    /* Some environment variables are inherited from parent.
       Make sure they are unset. */
    unsetenv(*args);
  }

  return 0;
}
