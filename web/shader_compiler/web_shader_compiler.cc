/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "web_shader_compiler.h"

#include "src/tint/lang/wgsl/inspector/inspector.h"
#include "src/tint/lang/wgsl/reader/reader.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
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

/**
 * Called by Tint on internal compiler errors (see `patch_tint.py`), which would otherwise
 * terminate the process. Tint is compiled without exceptions, throw from here.
 */
extern "C" [[noreturn]] void wsc_tint_internal_compiler_error(const char *message)
{
  throw std::runtime_error(message);
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

static WSC_Result glsl_to_wgsl(const char *glsl,
                               WSC_Stage stage,
                               const WSC_SamplerMapping *sampler_mappings,
                               int sampler_mappings_num)
{
  WSC_Result result = {nullptr, nullptr, nullptr, 0};
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
  /* Emit `enable`/`requires` directives for whatever the shader needs (read-write storage
   * textures, dual source blending, ...); the browser reports what it doesn't support. */
  writer_options.allowed_features = tint::wgsl::AllowedFeatures::Everything();
  auto output = tint::wgsl::writer::WgslFromIR(ir.Get(), writer_options);
  if (output != tint::Success) {
    result.log = copy_string(log + output.Failure().reason);
    return result;
  }
  result.wgsl = copy_string(output->wgsl);
  result.log = copy_string(log);

  /* The bindings the entry point uses, the others don't need to be visible to its stage. */
  tint::Source::File file("", output->wgsl);
  tint::Program program = tint::wgsl::reader::Parse(&file);
  if (program.IsValid()) {
    tint::inspector::Inspector inspector(program);
    const std::vector<tint::inspector::ResourceBinding> bindings = inspector.GetResourceBindings(
        "main");
    result.bindings = static_cast<WSC_Binding *>(malloc(sizeof(WSC_Binding) * bindings.size()));
    result.bindings_len = int(bindings.size());
    for (size_t i = 0; i < bindings.size(); i++) {
      result.bindings[i] = {bindings[i].bind_group, bindings[i].binding};
    }
  }
  return result;
}

WSC_Result WSC_glsl_to_wgsl(const char *glsl,
                            WSC_Stage stage,
                            const WSC_SamplerMapping *sampler_mappings,
                            int sampler_mappings_num)
{
  try {
    return glsl_to_wgsl(glsl, stage, sampler_mappings, sampler_mappings_num);
  }
  catch (const std::exception &error) {
    WSC_Result result = {nullptr, copy_string(error.what()), nullptr, 0};
    return result;
  }
}

void WSC_result_free(WSC_Result *result)
{
  free(result->wgsl);
  free(result->log);
  free(result->bindings);
  result->wgsl = nullptr;
  result->log = nullptr;
  result->bindings = nullptr;
  result->bindings_len = 0;
}
