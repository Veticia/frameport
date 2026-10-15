// xr_sim_probe: the smallest D3D11 OpenXR client FramePort uses to check a headset-free OpenXR runtime
// (Monado's simulated HMD, docs/PC_ANDROID.md "Testing without a headset") on Windows.
//
// Creates an instance with XR_KHR_D3D11_enable, picks the D3D11 adapter by the LUID the runtime asks for,
// runs a session (begins it on READY), submits a projection layer every frame (each eye cleared to a
// colour that cycles), and after <seconds> asks for the exit. Every step prints one "probe: ..." line;
// the last line is "probe: result ok frames=N focused=0|1" or "probe: result fail ...".
// Optional: --dump <file.bmp> saves the left eye image of frame 60 (what the client drew).
//
// Build (VS Build Tools x64 prompt), with the official OpenXR loader release (openxr_loader_windows):
//   cl /nologo /EHsc /O2 /I<loader>\include xr_sim_probe.cpp /link <loader>\x64\lib\openxr_loader.lib d3d11.lib dxgi.lib
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(call)                                                                  \
	do {                                                                             \
		XrResult r_ = (call);                                                        \
		if (XR_FAILED(r_)) {                                                         \
			std::printf("probe: result fail %s = %d\n", #call, (int)r_);            \
			std::fflush(stdout);                                                     \
			std::exit(2);                                                            \
		}                                                                            \
	} while (0)

static const char *state_name(XrSessionState s)
{
	switch (s) {
	case XR_SESSION_STATE_IDLE: return "IDLE";
	case XR_SESSION_STATE_READY: return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING: return "EXITING";
	default: return "UNKNOWN";
	}
}

static void save_bmp(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const char *path)
{
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;
	d.ArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	ID3D11Texture2D *staging = nullptr;
	if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) {
		std::printf("probe: dump failed (staging)\n");
		return;
	}
	ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
		staging->Release();
		return;
	}
	FILE *f = std::fopen(path, "wb");
	if (f) {
		uint32_t w = d.Width, h = d.Height, row = w * 3, pad = (4 - row % 4) % 4;
		uint32_t size = 54 + (row + pad) * h;
		uint8_t hdr[54] = {'B', 'M'};
		std::memcpy(hdr + 2, &size, 4);
		hdr[10] = 54;
		hdr[14] = 40;
		std::memcpy(hdr + 18, &w, 4);
		std::memcpy(hdr + 22, &h, 4);
		hdr[26] = 1;
		hdr[28] = 24;
		std::fwrite(hdr, 1, 54, f);
		std::vector<uint8_t> line(row + pad, 0);
		for (int y = (int)h - 1; y >= 0; y--) {
			const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < w; x++) { // RGBA/BGRA 8-bit formats only: good enough for a probe
				bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
				line[x * 3 + 0] = src[x * 4 + (bgra ? 0 : 2)];
				line[x * 3 + 1] = src[x * 4 + 1];
				line[x * 3 + 2] = src[x * 4 + (bgra ? 2 : 0)];
			}
			std::fwrite(line.data(), 1, line.size(), f);
		}
		std::fclose(f);
		std::printf("probe: dumped left eye %ux%u to %s\n", w, h, path);
	}
	ctx->Unmap(staging, 0);
	staging->Release();
}

int main(int argc, char **argv)
{
	double seconds = 10.0;
	const char *dump = nullptr;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--dump") && i + 1 < argc)
			dump = argv[++i];
		else
			seconds = std::atof(argv[i]);
	}
	setvbuf(stdout, nullptr, _IONBF, 0);

	const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr_sim_probe");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = exts;
	XrInstance inst = XR_NULL_HANDLE;
	CHECK(xrCreateInstance(&ici, &inst));
	XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
	CHECK(xrGetInstanceProperties(inst, &ip));
	std::printf("probe: runtime %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
	            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys;
	CHECK(xrGetSystem(inst, &sgi, &sys));
	XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
	CHECK(xrGetSystemProperties(inst, sys, &sp));
	std::printf("probe: system %s\n", sp.systemName);

	PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
	CHECK(xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
	XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	CHECK(getReq(inst, sys, &req));

	IDXGIFactory1 *factory = nullptr;
	CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 ad;
		adapter->GetDesc1(&ad);
		if (std::memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) {
			std::printf("probe: adapter %ls (luid %08lx:%08lx)\n", ad.Description, ad.AdapterLuid.HighPart,
			            ad.AdapterLuid.LowPart);
			break;
		}
		adapter->Release();
		adapter = nullptr;
	}
	if (!adapter) {
		std::printf("probe: result fail no DXGI adapter with the runtime's LUID\n");
		return 2;
	}
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
	                             nullptr, &ctx))) {
		std::printf("probe: result fail D3D11CreateDevice\n");
		return 2;
	}

	XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	gb.device = dev;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &gb;
	sci.systemId = sys;
	XrSession session;
	CHECK(xrCreateSession(inst, &sci, &session));
	std::printf("probe: session created\n");

	XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1;
	XrSpace space;
	CHECK(xrCreateReferenceSpace(session, &rsci, &space));

	uint32_t nviews = 0;
	CHECK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nviews, nullptr));
	std::vector<XrViewConfigurationView> vcv(nviews, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	CHECK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nviews, &nviews,
	                                        vcv.data()));

	uint32_t nfmt = 0;
	CHECK(xrEnumerateSwapchainFormats(session, 0, &nfmt, nullptr));
	std::vector<int64_t> fmts(nfmt);
	CHECK(xrEnumerateSwapchainFormats(session, nfmt, &nfmt, fmts.data()));
	int64_t fmt = fmts[0];
	for (int64_t f : fmts)
		if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM) {
			fmt = f;
			break;
		}

	struct Eye {
		XrSwapchain sc;
		std::vector<XrSwapchainImageD3D11KHR> imgs;
		std::vector<ID3D11RenderTargetView *> rtv;
		uint32_t w, h;
	};
	std::vector<Eye> eyes(nviews);
	for (uint32_t i = 0; i < nviews; i++) {
		Eye &e = eyes[i];
		e.w = vcv[i].recommendedImageRectWidth;
		e.h = vcv[i].recommendedImageRectHeight;
		XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
		sc.format = fmt;
		sc.sampleCount = 1;
		sc.width = e.w;
		sc.height = e.h;
		sc.faceCount = 1;
		sc.arraySize = 1;
		sc.mipCount = 1;
		CHECK(xrCreateSwapchain(session, &sc, &e.sc));
		uint32_t n = 0;
		CHECK(xrEnumerateSwapchainImages(e.sc, 0, &n, nullptr));
		e.imgs.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
		CHECK(xrEnumerateSwapchainImages(e.sc, n, &n, (XrSwapchainImageBaseHeader *)e.imgs.data()));
		for (auto &img : e.imgs) {
			D3D11_RENDER_TARGET_VIEW_DESC rd{};
			rd.Format = (DXGI_FORMAT)fmt;
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			ID3D11RenderTargetView *v = nullptr;
			dev->CreateRenderTargetView(img.texture, &rd, &v);
			e.rtv.push_back(v);
		}
	}
	std::printf("probe: %u views %ux%u format %lld\n", nviews, eyes[0].w, eyes[0].h, (long long)fmt);

	bool running = false, quit = false, focused = false, exit_requested = false;
	uint64_t frames = 0, rendered = 0;
	auto t0 = std::chrono::steady_clock::now();
	while (!quit) {
		XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				auto *s = (XrEventDataSessionStateChanged *)&ev;
				double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				std::printf("probe: state %s at %.2f s (frames %llu)\n", state_name(s->state), t,
				            (unsigned long long)frames);
				if (s->state == XR_SESSION_STATE_FOCUSED)
					focused = true;
				if (s->state == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					CHECK(xrBeginSession(session, &bi));
					running = true;
				} else if (s->state == XR_SESSION_STATE_STOPPING) {
					CHECK(xrEndSession(session));
					running = false;
				} else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
					quit = true;
				}
			} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
				quit = true;
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (!running) {
			if (elapsed > seconds + 20) { // never got READY, or never left
				std::printf("probe: result fail timeout (frames %llu)\n", (unsigned long long)frames);
				return 3;
			}
			Sleep(10);
			continue;
		}
		if (elapsed > seconds && !exit_requested) {
			std::printf("probe: requesting exit after %.2f s, frames %llu\n", elapsed, (unsigned long long)frames);
			xrRequestExitSession(session);
			exit_requested = true;
		}

		XrFrameState fs{XR_TYPE_FRAME_STATE};
		CHECK(xrWaitFrame(session, nullptr, &fs));
		CHECK(xrBeginFrame(session, nullptr));
		std::vector<XrCompositionLayerProjectionView> pv(nviews, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
		XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		std::vector<XrCompositionLayerBaseHeader *> layers;
		if (fs.shouldRender) {
			std::vector<XrView> views(nviews, {XR_TYPE_VIEW});
			XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
			li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			li.displayTime = fs.predictedDisplayTime;
			li.space = space;
			XrViewState vs{XR_TYPE_VIEW_STATE};
			uint32_t got = 0;
			CHECK(xrLocateViews(session, &li, &vs, nviews, &got, views.data()));
			for (uint32_t i = 0; i < nviews; i++) {
				Eye &e = eyes[i];
				uint32_t idx;
				CHECK(xrAcquireSwapchainImage(e.sc, nullptr, &idx));
				XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = XR_INFINITE_DURATION;
				CHECK(xrWaitSwapchainImage(e.sc, &wi));
				float phase = (float)((frames % 120) / 120.0);
				float col[4] = {i == 0 ? phase : 0.1f, 0.2f, i == 1 ? phase : 0.1f, 1.0f};
				ctx->ClearRenderTargetView(e.rtv[idx], col);
				if (dump && i == 0 && rendered == 60)
					save_bmp(dev, ctx, e.imgs[idx].texture, dump);
				CHECK(xrReleaseSwapchainImage(e.sc, nullptr));
				pv[i].pose = views[i].pose;
				pv[i].fov = views[i].fov;
				pv[i].subImage.swapchain = e.sc;
				pv[i].subImage.imageRect = {{0, 0}, {(int32_t)e.w, (int32_t)e.h}};
			}
			layer.space = space;
			layer.viewCount = nviews;
			layer.views = pv.data();
			layers.push_back((XrCompositionLayerBaseHeader *)&layer);
			rendered++;
		}
		XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
		fe.displayTime = fs.predictedDisplayTime;
		fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fe.layerCount = (uint32_t)layers.size();
		fe.layers = layers.data();
		CHECK(xrEndFrame(session, &fe));
		frames++;
		if (frames % 120 == 0)
			std::printf("probe: frames %llu rendered %llu at %.2f s\n", (unsigned long long)frames,
			            (unsigned long long)rendered, elapsed);
	}
	for (auto &e : eyes)
		xrDestroySwapchain(e.sc);
	xrDestroySpace(space);
	xrDestroySession(session);
	xrDestroyInstance(inst);
	std::printf("probe: result ok frames=%llu rendered=%llu focused=%d\n", (unsigned long long)frames,
	            (unsigned long long)rendered, focused ? 1 : 0);
	return 0;
}
