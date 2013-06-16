#include "pin.H"

#include <cstring>
#include <cassert>

int orig_argc;
const char *const *orig_argv;
bool have_followed = false;

static BOOL followChild(CHILD_PROCESS childProcess, VOID *val)
{
   // Follow only the first execv
   if (have_followed)
      return FALSE;

   have_followed = true;

   int argc = 0;
   char ** argv = (char**)malloc(orig_argc * sizeof(char *));

   for(int i = 0; i < orig_argc; ++i)
   {
      if (strcmp(orig_argv[i], "-t") == 0)
      {
         // Copy -t
         argv[argc++] = strdup(orig_argv[i]);
         ++i;
         // Update pin tool name: keep path, replace follow_execv with pin_sim
         char *pintool = strdup(orig_argv[i]);
         char *baseptr = strrchr(pintool, '/');
         assert(baseptr != NULL);
         strcpy(baseptr + 1, "pin_sim");
         argv[argc++] = pintool;
      }
      else if (strcmp(orig_argv[i], "-follow_execv") == 0)
      {
         // Skip -follow_execv itself and its argument
         ++i;
         continue;
      }
      else
      {
         // Copy unmodified
         argv[argc++] = strdup(orig_argv[i]);
      }

      // After copying --, stop
      if (strcmp(orig_argv[i], "--") == 0)
         break;
   }

   CHILD_PROCESS_SetPinCommandLine(childProcess, argc, argv);

   return TRUE;
}

int main(int argc, char *argv[])
{
   PIN_InitSymbols();
   PIN_Init(argc, argv);

   // Save command line arguments
   orig_argc = argc;
   orig_argv = argv;

   PIN_AddFollowChildProcessFunction(followChild, NULL);

   PIN_StartProgram();

   return 0;
}
