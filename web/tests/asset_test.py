# Lists the assets of the bundled essentials libraries, to check the asset indexing in the browser.
# Run with: ?fetch=tests/asset_test.py&args=-b,--python,/tests/asset_test.py
import bpy, os, traceback

def try_load(path, **kw):
    try:
        with bpy.data.libraries.load(path, **kw) as (src, dst):
            print("LOAD OK", path, kw, "brushes", len(src.brushes), "node_groups", len(src.node_groups),
                  "objects", len(src.objects))
    except Exception as ex:
        print("LOAD FAILED", path, kw, ex)

path = os.path.join(bpy.utils.system_resource("DATAFILES"), "assets", "brushes", "essentials_brushes-mesh_sculpt.blend")
with open(path, "rb") as fh:
    head = fh.read(16)
print("HEAD", path, os.path.getsize(path), head)
try_load(path)
try_load(path, assets_only=True)

for compress in (False, True):
    out = "/home/web_user/asset_test_%d.blend" % compress
    bpy.ops.wm.save_as_mainfile(filepath=out, compress=compress, copy=True)
    with open(out, "rb") as fh:
        print("HEAD", out, os.path.getsize(out), fh.read(16))
    try_load(out)
print("RESULT: done")
