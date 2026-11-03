// rmfix: an openvr_api.dll for Reality Mixer's folder that fixes its boxes in
// SteamVR 2.19's foveated Steam Link stream. Everything is forwarded to the real
// DLL (renamed openvr_api_real.dll); only the box textures get extra handling.
//
// Why: Reality Mixer hands SteamVR a blank texture per box face, then asks for
// SteamVR's own copy (GetOverlayTexture) and paints into that copy's top mip
// only. SteamVR makes the smaller mips when a texture is submitted, so they stay
// blank. The new foveated stream draws the low-res periphery from those mips, so
// the boxes fade out everywhere except in the sharp area.
//
// Fix: on Reality Mixer's own thread (whenever it calls into OpenVR there, at most
// every 250 ms), copy the painted top mip back into Reality Mixer's texture and
// submit it again; SteamVR then rebuilds the mips from it. Log: %TEMP%\rmfix.log.
#include <windows.h>
#include <d3d11.h>
#include <openvr.h>  // OpenVR 1.26.7: IVROverlay_027 and IVRSystem_022, as Reality Mixer uses
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

namespace rmfix {
static FILE *g_log;
static auto g_t0 = std::chrono::steady_clock::now();
static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }
static void Log(const char *fmt, ...) {
	if (!g_log) return;
	va_list a; va_start(a, fmt);
	fprintf(g_log, "%9.3f ", Now()); vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log);
	va_end(a);
}

struct Face {
	vr::Texture_t mine{};                          // Reality Mixer's texture, as it submitted it
	ID3D11ShaderResourceView *painted = nullptr;   // SteamVR's copy that Reality Mixer paints into
};
static vr::IVROverlay *g_ov;
static std::map<vr::VROverlayHandle_t, Face> g_faces;
static DWORD g_thread;  // the thread that submitted the textures (owns the D3D11 context)
static double g_last, g_lastStats;
static long long g_resubmits;

static void Drop(std::map<vr::VROverlayHandle_t, Face>::iterator it) {
	if (it->second.painted) it->second.painted->Release();
	((IUnknown *)it->second.mine.handle)->Release();
	g_faces.erase(it);
}

void OnSetTexture(vr::VROverlayHandle_t h, const vr::Texture_t *t) {
	if (!t || t->eType != vr::TextureType_DirectX || !t->handle) return;
	char key[256] = ""; vr::EVROverlayError e;
	g_ov->GetOverlayKey(h, key, sizeof key, &e);
	if (strncmp(key, "Key_RealityMixer_b", 18)) return;  // box faces only
	auto it = g_faces.find(h);
	if (it != g_faces.end()) {
		if (it->second.mine.handle == t->handle) return;  // our own re-submit
		Drop(it);
	}
	((IUnknown *)t->handle)->AddRef();
	g_faces[h].mine = *t;
	g_thread = GetCurrentThreadId();
}

void OnGetTexture(vr::VROverlayHandle_t h, void **native, vr::ETextureType *type) {
	auto it = g_faces.find(h);
	if (it == g_faces.end() || !native || !*native || !type || *type != vr::TextureType_DirectX) return;
	auto *srv = (ID3D11ShaderResourceView *)*native;
	if (it->second.painted == srv) return;
	if (it->second.painted) it->second.painted->Release();
	srv->AddRef();
	it->second.painted = srv;
	char key[256] = ""; vr::EVROverlayError e;
	g_ov->GetOverlayKey(h, key, sizeof key, &e);
	Log("fixing %s", key);
}

void Tick() {
	if (!g_ov || g_faces.empty() || GetCurrentThreadId() != g_thread) return;
	double t = Now();
	if (t - g_last < 0.25) return;
	g_last = t;
	for (auto it = g_faces.begin(); it != g_faces.end();) {
		Face &f = it->second;
		if (!f.painted) { ++it; continue; }
		ID3D11Resource *src = nullptr, *dst = (ID3D11Resource *)f.mine.handle;
		ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
		f.painted->GetResource(&src);
		dst->GetDevice(&dev); dev->GetImmediateContext(&ctx);
		ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, nullptr);  // top mip
		ctx->Release(); dev->Release(); src->Release();
		vr::EVROverlayError r = g_ov->SetOverlayTexture(it->first, &f.mine);
		if (r == vr::VROverlayError_InvalidHandle || r == vr::VROverlayError_UnknownOverlay) { auto d = it++; Drop(d); continue; }
		g_resubmits++;
		++it;
	}
	if (t - g_lastStats > 60) { g_lastStats = t; Log("%zu faces, %lld re-submits so far", g_faces.size(), g_resubmits); }
}
}  // namespace rmfix

using namespace vr;
#include "wrap_overlay.h"
#include "wrap_system.h"

static HMODULE Real() {
	static HMODULE h = [] {
		wchar_t p[MAX_PATH]; HMODULE self;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Real, &self);
		GetModuleFileNameW(self, p, MAX_PATH);
		wcscpy(wcsrchr(p, (wchar_t)92) + 1, L"openvr_api_real.dll");
		return LoadLibraryW(p);
	}();
	return h;
}

extern "C" __declspec(dllexport) void *VR_GetGenericInterface(const char *ver, EVRInitError *err) {
	auto fn = (void *(*)(const char *, EVRInitError *))GetProcAddress(Real(), "VR_GetGenericInterface");
	void *p = fn(ver, err);
	if (!rmfix::g_log) {
		char t[MAX_PATH]; GetTempPathA(MAX_PATH, t); strcat(t, "rmfix.log");
		rmfix::g_log = fopen(t, "w");
		rmfix::Log("rmfix loaded");
	}
	if (p && !strcmp(ver, "IVROverlay_027")) {
		static WrapOverlay w; w.real = rmfix::g_ov = (IVROverlay *)p;
		return &w;
	}
	if (p && !strcmp(ver, "IVRSystem_022")) {  // called every frame: lets the fix run more often
		static WrapSystem w; w.real = (IVRSystem *)p;
		return &w;
	}
	return p;
}

#pragma comment(linker, "/export:LiquidVR=openvr_api_real.LiquidVR")
#pragma comment(linker, "/export:VRControlPanel=openvr_api_real.VRControlPanel")
#pragma comment(linker, "/export:VRHeadsetView=openvr_api_real.VRHeadsetView")
#pragma comment(linker, "/export:VRPaths=openvr_api_real.VRPaths")
#pragma comment(linker, "/export:VRVirtualDisplay=openvr_api_real.VRVirtualDisplay")
#pragma comment(linker, "/export:VR_GetInitToken=openvr_api_real.VR_GetInitToken")
#pragma comment(linker, "/export:VR_GetRuntimePath=openvr_api_real.VR_GetRuntimePath")
#pragma comment(linker, "/export:VR_GetStringForHmdError=openvr_api_real.VR_GetStringForHmdError")
#pragma comment(linker, "/export:VR_GetVRInitErrorAsEnglishDescription=openvr_api_real.VR_GetVRInitErrorAsEnglishDescription")
#pragma comment(linker, "/export:VR_GetVRInitErrorAsSymbol=openvr_api_real.VR_GetVRInitErrorAsSymbol")
#pragma comment(linker, "/export:VR_InitInternal=openvr_api_real.VR_InitInternal")
#pragma comment(linker, "/export:VR_InitInternal2=openvr_api_real.VR_InitInternal2")
#pragma comment(linker, "/export:VR_IsHmdPresent=openvr_api_real.VR_IsHmdPresent")
#pragma comment(linker, "/export:VR_IsInterfaceVersionValid=openvr_api_real.VR_IsInterfaceVersionValid")
#pragma comment(linker, "/export:VR_IsRuntimeInstalled=openvr_api_real.VR_IsRuntimeInstalled")
#pragma comment(linker, "/export:VR_RuntimePath=openvr_api_real.VR_RuntimePath")
#pragma comment(linker, "/export:VR_ShutdownInternal=openvr_api_real.VR_ShutdownInternal")
