import bpy, mathutils

def setup():
    for ob in bpy.data.objects:
        ob.hide_viewport = True
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                r3d = area.spaces.active.region_3d
                r3d.view_perspective = 'PERSP'
                # Oblique view: grid lines cross the camera plane, which SwiftShader (headless
                # Chromium) drops for lines; a top view (identity rotation) draws them all.
                r3d.view_rotation = mathutils.Euler((1.2, 0, 0.8)).to_quaternion()
                r3d.view_location = (0, 0, 0)
                r3d.view_distance = 10
    print('GRID_TEST set')
    return None

bpy.app.timers.register(setup, first_interval=2.0)
