import bpy, gpu
from gpu_extras.batch import batch_for_shader

def test():
    try:
        off = gpu.types.GPUOffScreen(64, 64)
        with off.bind():
            fb = gpu.state.active_framebuffer_get()
            fb.clear(color=(1.0, 0.5, 0.25, 1.0))
            buf = fb.read_color(0, 0, 2, 1, 4, 0, 'FLOAT')
            print('GPUTEST clear', [round(v, 3) for v in buf.to_list()[0][0]])
            shader = gpu.shader.from_builtin('UNIFORM_COLOR')
            batch = batch_for_shader(shader, 'TRIS', {"pos": [(-1, -1), (3, -1), (-1, 3)]})
            shader.uniform_float("color", (0.0, 1.0, 0.0, 1.0))
            gpu.matrix.load_identity()
            gpu.matrix.load_projection_matrix(gpu.matrix.get_projection_matrix().identity() if False else __import__('mathutils').Matrix.Identity(4))
            batch.draw(shader)
            buf = fb.read_color(0, 0, 2, 1, 4, 0, 'FLOAT')
            print('GPUTEST draw', [round(v, 3) for v in buf.to_list()[0][0]])
        # Sample the offscreen texture into another one.
        off2 = gpu.types.GPUOffScreen(64, 64)
        with off2.bind():
            fb = gpu.state.active_framebuffer_get()
            fb.clear(color=(0.0, 0.0, 0.0, 0.0))
            shader = gpu.shader.from_builtin('IMAGE')
            batch = batch_for_shader(shader, 'TRIS', {"pos": [(-1, -1), (1, -1), (1, 1), (-1, -1), (1, 1), (-1, 1)], "texCoord": [(0, 0), (1, 0), (1, 1), (0, 0), (1, 1), (0, 1)]})
            shader.uniform_sampler("image", off.texture_color)
            gpu.matrix.load_identity()
            gpu.matrix.load_projection_matrix(__import__('mathutils').Matrix.Identity(4))
            batch.draw(shader)
            buf = fb.read_color(0, 0, 2, 1, 4, 0, 'FLOAT')
            print('GPUTEST sample', [round(v, 3) for v in buf.to_list()[0][0]])
    except Exception as e:
        import traceback
        traceback.print_exc()
        print('GPUTEST error', e)
    print('RESULT: done')
    return None

bpy.app.timers.register(test, first_interval=3.0)
