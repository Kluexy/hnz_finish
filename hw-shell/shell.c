#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>


#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_run_prog(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc
{
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "prints the current working directory"},
    {cmd_cd, "cd", "change the current working directory to the given directory"},
    {cmd_run_prog, "run_prog", "execute programs when they are entered into the shell"}};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens)
{
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) { exit(0); }

/* print the lujin to standard*/
int cmd_pwd(unused struct tokens *tokens)
{
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != NULL)
  {
    printf("%s\n", cwd);
  }
  else
  {
    perror("获取当前工作路径错误！");
  }
  return 0;
}

/* exchange to a ptr*/
int cmd_cd(struct tokens *tokens)
{
  if (tokens_get_length(tokens) != 2)
  {
    printf("需要一个参数（目标路径）！\n");
    return 1;
  }

  char *path = tokens_get_token(tokens, 1);
  if (chdir(path) != 0)
  {
    perror("转换路径出现错误! ");
    return 1;
  }
  return 0;
}

// 根据输入的字符串返回完整路径，确保返回的路径是存在的
char *get_full_path(const char *path_input)
{
  if (path_input == NULL)
  {
    return NULL;
  }
  if (path_input[0] == '/')
  {
    if (access(path_input, F_OK) != -1)
    {
      // 路径存在
      return strdup(path_input);
    }
    else
    {
      return NULL;
    }
  }
  else
  {
    char *env_path = getenv("PATH");
    if (env_path == NULL)
    {
      return NULL;
    }
    char *env_path_copy = strdup(env_path);
    if (env_path_copy == NULL)
    {
      perror("内存分配失败");
      exit(EXIT_FAILURE);
    }

    char *path = strtok(env_path_copy, ":");
    while (path != NULL)
    {
      size_t path_length = strlen(path) + strlen(path_input) + 2;
      char *full_path = (char *)malloc(path_length);
      if (full_path == NULL)
      {
        perror("内存分配失败");
        exit(EXIT_FAILURE);
      }
      snprintf(full_path, path_length, "%s/%s", path, path_input);
      if (access(full_path, F_OK) != -1)
      {
        free(env_path_copy);
        char *result = strdup(full_path);
        if (result == NULL)
        {
          perror("内存分配失败");
          exit(EXIT_FAILURE);
        }
        free(full_path);
        return result;
      }
      free(full_path);
      path = strtok(NULL, ":");
    }
    free(env_path_copy);
    return NULL;
  }
}

int redirect_output(char *file_path)
{
  if (file_path == NULL)
  {
    perror("loss the file params！");
    return 1;
  }
  FILE *file = fopen(file_path, "w");
  if (file == NULL)
  {
    perror("open file error！");
    return 1;
  }
  if (freopen(file_path, "w", stdout) == NULL)
  {
    perror("redirect file error！");
    fclose(file);
    return 1;
  }
  return 0;
}

int redirect_input(char *file_path)
{
  if (file_path == NULL)
  {
    perror("缺少文件路径参数！");
    return 1;
  }
  // 打开文件，若文件不存在应该报错
  FILE *file = fopen(file_path, "r");
  if (file == NULL)
  {
    perror("打开文件出错!");
    return 1;
  }
  if (freopen(file_path, "r", stdin) == NULL)
  {
    perror("重定向标准输入到文件失败！");
    fclose(file);
    return 1;
  }
  return 0;
}

int reset_stdio()
{
  freopen("/dev/tty", "w", stdout);
  freopen("/dev/tty", "r", stdin);
  return 0;
}

int cmd_run_prog(struct tokens *tokens)
{
  int tokens_length = tokens->tokens_length;
  if (tokens_length < 1)
  {
    printf("run: 需要至少一个命令或可执行程序的路径\n");
    return 1;
  }

  char *program_path = get_full_path(tokens->tokens[0]);
  if (program_path == NULL)
  {
    printf("找不到文件！\n");
    return 1;
  }
  int is_redirected = 0;
  if (tokens_length >= 3)
  {
    if (strcmp(tokens->tokens[tokens_length - 2], ">") == 0)
    {
      is_redirected = 1;
      if (redirect_output(tokens->tokens[tokens_length - 1]) != 0)
      {
        return 1;
      }
    }
    else if (strcmp(tokens->tokens[tokens_length - 2], "<") == 0)
    {
      is_redirected = 1;
      if (redirect_input(tokens->tokens[tokens_length - 1]) != 0)
      {
        return 1;
      }
    }
  }
  // 创建子进程
  pid_t pid = fork();
  if (pid == -1)
  {
    perror("创建子进程失败!");
    return 1;
  }
  else if (pid == 0) // 子进程会执行的代码
  {
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    int new_argc = tokens->tokens_length;
    if (is_redirected == 1) 
      new_argc = new_argc - 2;
    char **argv = malloc((new_argc + 1) * sizeof(char *));
    if (argv == NULL)
    {
      perror("分配内存失败！\n");
      return 1;
    }
    // 复制 tokens 中的字符串到新的数组中
    for (size_t i = 0; i < new_argc; ++i)
    {
      argv[i] = tokens->tokens[i];
    }
    // 在数组的最后放置 NULL 结束符
    argv[new_argc] = NULL;
    // 在子进程中执行程序。
    execv(program_path, argv);
    perror("执行子进程错误!");
    exit(EXIT_FAILURE);
  }
  else // pid为正数，表示当前为父进程
  {
    signal(SIGINT, SIG_IGN);
    tcsetpgrp(shell_terminal, pid);
    int status;
    waitpid(pid, &status, 0);
    tcsetpgrp(shell_terminal, shell_pgid);
    signal(SIGINT, SIG_DFL);
    reset_stdio();
  }
  // tcsetpgrp(shell_terminal, getpid());
  reset_stdio();
  return 0;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[])
{
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell()
{
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  signal(SIGINT, SIG_DFL);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive)
  {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[])
{
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin))
  {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0)
    {
      cmd_table[fundex].fun(tokens);
    }
    else
    {
      /* REPLACE this to run commands as programs. */
      // fprintf(stdout, "This shell doesn't know how to run programs.\n");
      cmd_run_prog(tokens);
    }
    if (shell_is_interactive)
    {
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);
    }
    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
