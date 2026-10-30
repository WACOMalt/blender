import bpy, gpu, blf
from mathutils import Matrix

def count_text(text):
    off = gpu.types.GPUOffScreen(256, 64)
    with off.bind():
        fb = gpu.state.active_framebuffer_get()
        fb.clear(color=(0.0, 0.0, 0.0, 0.0))
        gpu.matrix.load_identity()
        gpu.matrix.load_projection_matrix(Matrix.Identity(4))
        with gpu.matrix.push_pop_projection():
            gpu.matrix.load_projection_matrix(Matrix(((2/256, 0, 0, -1), (0, 2/64, 0, -1), (0, 0, 1, 0), (0, 0, 0, 1))))
            blf.position(0, 10, 20, 0)
            blf.size(0, 24)
            blf.color(0, 1, 1, 1, 1)
            blf.draw(0, text)
        buf = fb.read_color(0, 0, 256, 64, 4, 0, 'FLOAT')
    lit = sum(1 for row in buf.to_list() for px in row if px[3] > 0.1)
    off.free()
    return lit

def test():
    try:
        for text in ["A", "AA", "AAAA", "Hello"]:
            print("TEXTTEST", repr(text), count_text(text))
    except Exception as e:
        import traceback
        traceback.print_exc()
    print('RESULT: done')

bpy.app.timers.register(test, first_interval=3.0)
