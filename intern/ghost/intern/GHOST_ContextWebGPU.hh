/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Drawing context of the WebGPU back-end. WebGPU is owned by Blender's GPU module, which draws
 * into the canvas of the window, so the context itself has nothing to do.
 */

#pragma once

#include "GHOST_ContextNone.hh"

class GHOST_ContextWebGPU : public GHOST_ContextNone {
 public:
  GHOST_ContextWebGPU(const GHOST_ContextParams &context_params)
      : GHOST_ContextNone(context_params)
  {
  }
};
