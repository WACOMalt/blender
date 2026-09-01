/* Launcher shim for the Xe-LP Blender fork.
 * - Restores LD_LIBRARY_PATH from *_ORIG if a PyInstaller parent polluted it.
 * - Prepends Intel UMF lib dir, required by the oneAPI Level-Zero adapter on
 *   Arch (split oneAPI packages, no setvars.sh).
 * - Execs blender-bin from its own directory.
 * Compiled ELF instead of a shell script: Blender Launcher's build probe
 * fails to spawn interpreter scripts (exit 127), a plain binary works.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  (void)argc;
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0) {
    return 127;
  }
  self[n] = '\0';
  char *slash = strrchr(self, '/');
  if (slash) {
    *slash = '\0';
  }

  const char *orig = getenv("LD_LIBRARY_PATH_ORIG");
  const char *keep = orig ? orig : "";
  char libpath[8192];
  snprintf(libpath, sizeof(libpath), "/opt/intel/oneapi/umf/1.1/lib%s%s",
           *keep ? ":" : "", keep);
  setenv("LD_LIBRARY_PATH", libpath, 1);

  char exe[8192];
  snprintf(exe, sizeof(exe), "%s/blender-bin", self);
  argv[0] = exe;
  execv(exe, argv);
  perror("xelp shim: exec blender-bin");
  return 127;
}
