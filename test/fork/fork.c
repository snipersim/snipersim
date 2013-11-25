#include <stdio.h>

int main()
{
  int status;
  int pid = fork();

  printf("Hello world from %s\n", pid ? "parent" : "child");

  while(waitpid(-1, &status, 0) > 0);
  return;
}
