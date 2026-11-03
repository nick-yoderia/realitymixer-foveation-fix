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
// Fix: on Reality Mixer's OpenVR thread (whenever it calls into OpenVR there, at
// most every 250 ms), copy the painted top mip into a texture of our own, on our
// own D3D11 device, and submit that instead; SteamVR then rebuilds the mips from
// it. Log: %TEMP%\rmfix.log.
#include <windows.h>
#include <d3d11_4.h>
#include <openvr.h>  // OpenVR 1.26.7: IVROverlay_027 and IVRSystem_022, as Reality Mixer uses
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

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

// Everything below runs on our own D3D11 device, never on Reality Mixer's: submitting
// a texture makes SteamVR lock that texture's device, and Reality Mixer holds its
// device's lock on another thread while editing boxes, so re-submitting Reality
// Mixer's own texture from its OpenVR thread deadlocked it.
struct Face {
	vr::Texture_t mine{};                // our copy, submitted in place of Reality Mixer's texture
	ID3D11Texture2D *tex = nullptr;      // = mine.handle, on our device
	ID3D11Texture2D *painted = nullptr;  // SteamVR's copy that Reality Mixer paints into, opened on our device
	ID3D11Resource *paintedRM = nullptr; // the same, on Reality Mixer's device (only to notice changes)
	D3D11_TEXTURE2D_DESC desc{};         // Reality Mixer's texture
};
static vr::IVROverlay *g_ov;
static std::mutex g_mx;
static std::map<vr::VROverlayHandle_t, Face> g_faces;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static DWORD g_thread;  // the thread that submitted the textures (Reality Mixer's OpenVR thread)
static double g_last, g_lastStats;
static long long g_resubmits;

template <class T> static void Rel(T *&p) { if (p) p->Release(); p = nullptr; }
static void Clear(Face &f) { Rel(f.tex); Rel(f.painted); Rel(f.paintedRM); f.mine.handle = nullptr; }

// Our device, on the same GPU as Reality Mixer's.
static bool MakeDevice(ID3D11Device *rm) {
	if (g_dev) return true;
	IDXGIDevice *dd = nullptr; IDXGIAdapter *ad = nullptr;
	if (FAILED(rm->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) || FAILED(dd->GetAdapter(&ad))) { Rel(dd); return false; }
	HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
	Rel(ad); Rel(dd);
	if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%08lx", hr); return false; }
	ID3D11Multithread *mt = nullptr;
	if (SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
	return true;
}

void OnSetTexture(vr::VROverlayHandle_t h, const vr::Texture_t *t) {
	if (!t || t->eType != vr::TextureType_DirectX || !t->handle) return;
	std::lock_guard<std::mutex> lk(g_mx);
	auto it = g_faces.find(h);
	if (it != g_faces.end() && it->second.mine.handle == t->handle) return;  // our own re-submit
	char key[256] = ""; vr::EVROverlayError e;
	g_ov->GetOverlayKey(h, key, sizeof key, &e);
	if (strncmp(key, "Key_RealityMixer_b", 18)) return;  // box faces only
	Face &f = g_faces[h];
	Clear(f);
	((ID3D11Texture2D *)t->handle)->GetDesc(&f.desc);
	f.mine = *t;
	f.mine.handle = nullptr;  // set once our copy exists
	g_thread = GetCurrentThreadId();
}

void OnGetTexture(vr::VROverlayHandle_t h, void **native, vr::ETextureType *type) {
	if (!native || !*native || !type || *type != vr::TextureType_DirectX) return;
	std::lock_guard<std::mutex> lk(g_mx);
	auto it = g_faces.find(h);
	if (it == g_faces.end()) return;
	Face &f = it->second;
	ID3D11Resource *res = nullptr;
	((ID3D11ShaderResourceView *)*native)->GetResource(&res);
	if (res == f.paintedRM) { res->Release(); return; }
	Clear(f);
	f.paintedRM = res;
	char key[256] = ""; vr::EVROverlayError e;
	g_ov->GetOverlayKey(h, key, sizeof key, &e);
	// Open SteamVR's copy on our device through its shared handle, and make our own texture.
	ID3D11Device *rm = nullptr; IDXGIResource *dr = nullptr; HANDLE sh = nullptr;
	res->GetDevice(&rm);
	bool ok = MakeDevice(rm)
		&& SUCCEEDED(res->QueryInterface(__uuidof(IDXGIResource), (void **)&dr)) && SUCCEEDED(dr->GetSharedHandle(&sh)) && sh
		&& SUCCEEDED(g_dev->OpenSharedResource(sh, __uuidof(ID3D11Texture2D), (void **)&f.painted));
	Rel(dr); Rel(rm);
	if (ok) {
		D3D11_TEXTURE2D_DESC d = f.desc;
		d.MipLevels = 1; d.ArraySize = 1; d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		ok = SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &f.tex));
	}
	if (!ok) { Log("can't fix %s", key); Clear(f); f.paintedRM = res; res->AddRef(); return; }
	f.mine.handle = f.tex;
	Log("fixing %s", key);
}

void Tick() {
	if (!g_ov || GetCurrentThreadId() != g_thread) return;
	double t = Now();
	if (t - g_last < 0.25) return;
	g_last = t;
	std::lock_guard<std::mutex> lk(g_mx);
	for (auto it = g_faces.begin(); it != g_faces.end();) {
		Face &f = it->second;
		if (!f.tex) { ++it; continue; }
		g_ctx->CopySubresourceRegion(f.tex, 0, 0, 0, 0, f.painted, 0, nullptr);  // top mip
		vr::EVROverlayError r = g_ov->SetOverlayTexture(it->first, &f.mine);
		if (r == vr::VROverlayError_InvalidHandle || r == vr::VROverlayError_UnknownOverlay) { Clear(f); it = g_faces.erase(it); continue; }
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
