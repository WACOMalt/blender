/* Prototype: Blender-like blocking main loop on a pthread (PROXY_TO_PTHREAD),
 * WebGPU on the browser main thread, synchronous readback via proxying. */
#include <cstdio>
#include <cstring>
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <webgpu/webgpu.h>

static WGPUInstance instance;
static WGPUAdapter adapter;
static WGPUDevice device;
static WGPUQueue queue;
static WGPUSurface surface;
static WGPUTextureFormat surface_format;

static WGPUStringView sv(const char *s)
{
  return WGPUStringView{s, strlen(s)};
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView msg, void *ctx, void *)
{
  if (status != WGPURequestDeviceStatus_Success) {
    printf("device failed: %.*s\n", int(msg.length), msg.data);
    emscripten_proxy_finish((em_proxying_ctx *)ctx);
    return;
  }
  device = d;
  queue = wgpuDeviceGetQueue(device);

  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src = {};
  canvas_src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
  canvas_src.selector = sv("#canvas");
  WGPUSurfaceDescriptor sdesc = {};
  sdesc.nextInChain = &canvas_src.chain;
  surface = wgpuInstanceCreateSurface(instance, &sdesc);

  WGPUSurfaceCapabilities caps = {};
  wgpuSurfaceGetCapabilities(surface, adapter, &caps);
  surface_format = caps.formats[0];
  WGPUSurfaceConfiguration config = {};
  config.device = device;
  config.format = surface_format;
  config.usage = WGPUTextureUsage_RenderAttachment;
  config.width = 320;
  config.height = 240;
  config.alphaMode = WGPUCompositeAlphaMode_Opaque;
  config.presentMode = WGPUPresentMode_Fifo;
  wgpuSurfaceConfigure(surface, &config);
  printf("[main thread] device ready, surface format %d\n", int(surface_format));
  emscripten_proxy_finish((em_proxying_ctx *)ctx);
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView msg, void *ctx, void *)
{
  if (status != WGPURequestAdapterStatus_Success) {
    printf("adapter failed: %.*s\n", int(msg.length), msg.data);
    emscripten_proxy_finish((em_proxying_ctx *)ctx);
    return;
  }
  adapter = a;
  WGPURequestDeviceCallbackInfo info = {};
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.callback = on_device;
  info.userdata1 = ctx;
  wgpuAdapterRequestDevice(adapter, nullptr, info);
}

/* Runs on the main thread; finishes asynchronously. */
static void gpu_init(em_proxying_ctx *ctx, void *)
{
  instance = wgpuCreateInstance(nullptr);
  WGPURequestAdapterCallbackInfo info = {};
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.callback = on_adapter;
  info.userdata1 = ctx;
  wgpuInstanceRequestAdapter(instance, nullptr, info);
}

static void clear_pass(WGPUTextureView view, double r, double g, double b, WGPUCommandEncoder enc)
{
  WGPURenderPassColorAttachment att = {};
  att.view = view;
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {r, g, b, 1.0};
  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

/* Runs on the main thread, synchronous. */
static void gpu_draw_frame(void *)
{
  WGPUSurfaceTexture st = {};
  wgpuSurfaceGetCurrentTexture(surface, &st);
  WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
  clear_pass(view, 0.9, 0.45, 0.1, enc);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  wgpuTextureViewRelease(view);
  wgpuTextureRelease(st.texture);
}

static unsigned char readback_pixel[4];
static WGPUBuffer readback_buffer;

static void on_mapped(WGPUMapAsyncStatus status, WGPUStringView, void *ctx, void *)
{
  if (status == WGPUMapAsyncStatus_Success) {
    const void *data = wgpuBufferGetConstMappedRange(readback_buffer, 0, 256);
    memcpy(readback_pixel, data, 4);
    wgpuBufferUnmap(readback_buffer);
  }
  emscripten_proxy_finish((em_proxying_ctx *)ctx);
}

/* Runs on the main thread; finishes when the GPU result is mapped. */
static void gpu_readback(em_proxying_ctx *ctx, void *)
{
  WGPUTextureDescriptor td = {};
  td.size = {4, 4, 1};
  td.format = WGPUTextureFormat_RGBA8Unorm;
  td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = WGPUTextureDimension_2D;
  WGPUTexture tex = wgpuDeviceCreateTexture(device, &td);
  WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

  WGPUBufferDescriptor bd = {};
  bd.size = 256 * 4;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  readback_buffer = wgpuDeviceCreateBuffer(device, &bd);

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
  clear_pass(view, 0.25, 0.5, 0.75, enc);
  WGPUTexelCopyTextureInfo src = {};
  src.texture = tex;
  WGPUTexelCopyBufferInfo dst = {};
  dst.buffer = readback_buffer;
  dst.layout.bytesPerRow = 256;
  dst.layout.rowsPerImage = 4;
  WGPUExtent3D extent = {4, 4, 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(queue, 1, &cmd);

  WGPUBufferMapCallbackInfo info = {};
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.callback = on_mapped;
  info.userdata1 = ctx;
  wgpuBufferMapAsync(readback_buffer, WGPUMapMode_Read, 0, 256, info);
}

int main()
{
  printf("[blender thread] main() on main runtime thread: %d\n", emscripten_is_main_runtime_thread());
  em_proxying_queue *q = emscripten_proxy_get_system_queue();
  pthread_t gpu_thread = emscripten_main_runtime_thread_id();

  emscripten_proxy_sync_with_ctx(q, gpu_thread, gpu_init, nullptr);
  if (!device) {
    printf("RESULT: FAIL no device\n");
    return 1;
  }
  /* A blocking "main loop", like WM_main. */
  for (int frame = 0; frame < 3; frame++) {
    emscripten_proxy_sync(q, gpu_thread, gpu_draw_frame, nullptr);
    emscripten_thread_sleep(16);
  }
  emscripten_proxy_sync_with_ctx(q, gpu_thread, gpu_readback, nullptr);
  printf("RESULT: readback %d %d %d %d (expected 64 128 191 255)\n",
         readback_pixel[0], readback_pixel[1], readback_pixel[2], readback_pixel[3]);
  return 0;
}
