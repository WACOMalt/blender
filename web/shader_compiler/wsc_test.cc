/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Command line test: `wsc_test <vert|frag|comp> file.glsl` prints the WGSL. */

#include "web_shader_compiler.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

int main(int argc, char **argv)
{
  if (argc != 3) {
    fprintf(stderr, "Usage: wsc_test <vert|frag|comp> file.glsl\n");
    return 2;
  }
  const WSC_Stage stage = (strcmp(argv[1], "vert") == 0) ? WSC_STAGE_VERTEX :
                          (strcmp(argv[1], "frag") == 0) ? WSC_STAGE_FRAGMENT :
                                                           WSC_STAGE_COMPUTE;
  std::ifstream file(argv[2]);
  std::stringstream source;
  source << file.rdbuf();

  WSC_init();
  WSC_Result result = WSC_glsl_to_wgsl(source.str().c_str(), stage, nullptr, 0);
  if (result.log) {
    fprintf(stderr, "%s\n", result.log);
  }
  const bool ok = result.wgsl != nullptr;
  if (ok) {
    printf("%s", result.wgsl);
  }
  WSC_result_free(&result);
  WSC_exit();
  return ok ? 0 : 1;
}
