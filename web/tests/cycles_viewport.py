import bpy

def setup():
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.preview_samples = 16
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.shading.type = 'RENDERED'
    print('CYCLES_VIEWPORT set')
    return None

bpy.app.timers.register(setup, first_interval=2.0)
