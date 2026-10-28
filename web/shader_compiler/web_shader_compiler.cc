/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "web_shader_compiler.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

static char *copy_string(const std::string &str)
{
  if (str.empty()) {
    return nullptr;
  }
  char *result = static_cast<char *>(malloc(str.size() + 1));
  memcpy(result, str.c_str(), str.size() + 1);
  return result;
}

void WSC_init()
{
  glslang::InitializeProcess();
  tint::Initialize();
}

void WSC_exit()
{
  glslang::FinalizeProcess();
}

static bool glsl_to_spirv(const char *glsl,
                          const WSC_Stage stage,
                          std::vector<uint32_t> &r_spirv,
                          std::string &r_log)
{
  const EShLanguage language = (stage == WSC_STAGE_VERTEX)   ? EShLangVertex :
                               (stage == WSC_STAGE_FRAGMENT) ? EShLangFragment :
                                                               EShLangCompute;
  glslang::TShader shader(language);
  shader.setStrings(&glsl, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

  const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
    r_log += shader.getInfoLog();
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    r_log += program.getInfoLog();
    return false;
  }
  glslang::SpvOptions options;
  options.generateDebugInfo = false;
  options.disableOptimizer = true;
  glslang::GlslangToSpv(*program.getIntermediate(language), r_spirv, &options);
  return true;
}

WSC_Result WSC_glsl_to_wgsl(const char *glsl,
                            WSC_Stage stage,
                            const WSC_SamplerMapping *sampler_mappings,
                            int sampler_mappings_num)
{
  WSC_Result result = {nullptr, nullptr};
  std::string log;

  std::vector<uint32_t> spirv;
  if (!glsl_to_spirv(glsl, stage, spirv, log)) {
    result.log = copy_string(log);
    return result;
  }

  tint::spirv::reader::Options reader_options;
  for (int i = 0; i < sampler_mappings_num; i++) {
    const WSC_SamplerMapping &mapping = sampler_mappings[i];
    reader_options.sampler_mappings[tint::BindingPoint{mapping.group, mapping.binding}] =
        tint::BindingPoint{mapping.sampler_group, mapping.sampler_binding};
  }
  auto ir = tint::spirv::reader::ReadIR(spirv, reader_options);
  if (ir != tint::Success) {
    result.log = copy_string(log + ir.Failure().reason);
    return result;
  }

  tint::wgsl::writer::Options writer_options;
  /* GLSL doesn't require derivatives in uniform control flow, Blender's shaders rely on it. */
  writer_options.allow_non_uniform_derivatives = true;
  auto output = tint::wgsl::writer::WgslFromIR(ir.Get(), writer_options);
  if (output != tint::Success) {
    result.log = copy_string(log + output.Failure().reason);
    return result;
  }
  result.wgsl = copy_string(output->wgsl);
  result.log = copy_string(log);
  return result;
}

void WSC_result_free(WSC_Result *result)
{
  free(result->wgsl);
  free(result->log);
  result->wgsl = nullptr;
  result->log = nullptr;
}
