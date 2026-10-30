import bpy, os

def test():
    path = os.path.expanduser('~/persist_test.blend')
    if os.path.exists(path):
        print('PERSIST found', path, os.path.getsize(path))
    else:
        bpy.ops.mesh.primitive_monkey_add(location=(3, 0, 0))
        bpy.ops.wm.save_as_mainfile(filepath=path)
        print('PERSIST saved', path)
    print('RESULT: done')

bpy.app.timers.register(test, first_interval=2.0)
