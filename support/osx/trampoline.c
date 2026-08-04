#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
  char bundled[PATH_MAX];

  snprintf(bundled, sizeof(bundled), "%s", argv[0]);
  char *x = strrchr(bundled, '/');
  if(x == NULL)
    x = bundled;
  else
    x++;
  snprintf(x, sizeof(bundled) - (x - bundled), "%s", "movian.bin");

  /*
   * Always launch the payload from the signed application bundle.  The old
   * self-update path preferred a user-writable executable under ~/.hts,
   * bypassing both the installed version and its notarized code signature.
   */
  execv(bundled, argv);
  fprintf(stderr, "Unable to launch %s: %s\n", bundled, strerror(errno));
  return 1;
}
