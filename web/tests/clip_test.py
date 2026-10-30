import bpy, gpu, mathutils
from gpu_extras.batch import batch_for_shader

def run(prim, coords, label):
    off = gpu.types.GPUOffScreen(64, 64)
    with off.bind():
        fb = gpu.state.active_framebuffer_get()
        fb.clear(color=(0.0, 0.0, 0.0, 1.0))
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')
        batch = batch_for_shader(shader, prim, {"pos": coords})
        shader.uniform_float("color", (1.0, 1.0, 1.0, 1.0))
        gpu.matrix.load_matrix(mathutils.Matrix.Identity(4))
        proj = mathutils.Matrix.Identity(4)
        # Perspective: w = -z (camera looking down -Z), near 0.1, far 100.
        n, f = 0.1, 100.0
        proj[0][0] = 1.0; proj[1][1] = 1.0
        proj[2][2] = -(f + n) / (f - n); proj[2][3] = -2 * f * n / (f - n)
        proj[3][2] = -1.0; proj[3][3] = 0.0
        gpu.matrix.load_projection_matrix(proj)
        batch.draw(shader)
        buf = fb.read_color(0, 0, 64, 64, 4, 0, 'FLOAT')
        lit = sum(1 for row in buf.to_list() for px in row if px[0] > 0.5)
        print('CLIPTEST', label, 'lit pixels:', lit)

def test():
    # Line fully in front of the camera.
    run('LINES', [(-0.5, 0.0, -2.0), (0.5, 0.0, -2.0)], 'line_front')
    # Line from in front to behind the camera (crosses w = 0).
    run('LINES', [(0.0, -0.5, -2.0), (0.0, -0.5, 5.0)], 'line_cross')
    # Triangle crossing the camera plane.
    run('TRIS', [(-1.0, -0.5, -2.0), (1.0, -0.5, -2.0), (0.0, -0.5, 5.0)], 'tri_cross')
    return None

bpy.app.timers.register(test, first_interval=1.0)
