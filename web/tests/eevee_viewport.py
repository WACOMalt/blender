import bpy

def setup():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.shading.type = 'MATERIAL'
    print('EEVEE_VIEWPORT set')
    return None

bpy.app.timers.register(setup, first_interval=2.0)
