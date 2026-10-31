# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Render with Cycles and show the result in an image editor (the whole window), to check the
# render result display, also without the draw manager (WebGL2):
#   ?gpu=webgl2&fetch=tests/render_result_test.py&args=--python,/tests/render_result_test.py

import bpy


def render():
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 8
    scene.render.resolution_percentage = 25
    bpy.context.preferences.view.render_display_type = 'SCREEN'
    bpy.ops.render.render('INVOKE_DEFAULT')
    print("RESULT: render started")


bpy.app.timers.register(render, first_interval=2.0)
