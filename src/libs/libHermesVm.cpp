#include "common/abi.h"
#include "libs/libBgsRpc.h"
#include "libs/libComposite.h"
#include "libs/libHermesVm.h"
#include "libs/libRnpsAppMgr.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <set>

#include <SDL3/SDL.h>
#include <string>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <unordered_map>
#include <utility>
#include <vector>

#include "kernel/fileSystem.h"

#include <nlohmann/json.hpp>

#include "stb_image.h"
#include "stb_truetype.h"

// ps5-vsh: RNPS startup params (--rnps-param k=v), file-global so both the
// early RnpsBridge stubs (defined before namespace Libs opens) and the
// HermesVmHost setter can reach them. Empty = legacy proxy behavior.
namespace RnpsLaunchParams {

std::mutex                                   g_mutex;
std::unordered_map<std::string, std::string> g_params;

inline void Set(const std::vector<std::pair<std::string, std::string>>& params) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_params.clear();
	for (const auto& kv : params) {
		g_params[kv.first] = kv.second;
	}
}

inline bool Get(const std::string& key, std::string* out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto                  it = g_params.find(key);
	if (it == g_params.end()) {
		return false;
	}
	if (out != nullptr) {
		*out = it->second;
	}
	return true;
}

inline bool Empty() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_params.empty();
}

} // namespace RnpsLaunchParams

#ifdef KYTY_USE_HERMES
// Real VM dispatch (wired via kyty_target_link_hermes in
// 3rdparty/hermes-pin.cmake, which also provides this define, so these
// headers are never needed when the build runs with KYTY_USE_HERMES=OFF).
#include <hermes/hermes.h>
#include <jsi/jsi.h>

// ps5-vsh: TurboModule stub bridge. RN 0.70 Hermes apps reach every native
// module through global.__turboModuleProxy(name). One generic HostObject
// covers all modules: any method call logs (module.method + argc) and
// returns undefined, letting bundle execution advance past native requires.
// Real semantics get filled per module as errors demand them.
namespace RnpsBridge {

// ps5-vsh: everything generic resolves to the __kytyAny Proxy (installed
// as a prelude global before bundle evaluation). Unlike HostObjects, the
// proxy is simultaneously gettable, callable, and constructible, so
// polyfill-style calls and TurboModule methods never throw on shape alone.
// Specific shims (StyleSheet, AppRegistry, timers) keep exact behavior;
// this is the long tail.
inline facebook::jsi::Value GetAnyProxy(facebook::jsi::Runtime& rt) {
	try {
		facebook::jsi::Value any = rt.global().getProperty(rt, "__kytyAny");
		if (any.isObject()) {
			return any;
		}
	} catch (...) {
	}
	facebook::jsi::Object fallback(rt);
	fallback.setProperty(rt, "__kytyStub", true);
	return fallback;
}

// ps5-vsh: named method stub — same shape as __kytyAny (callable via Proxy
// apply) but logs the qualified `label(...)` on call so bundle triage keeps
// the module.method identity instead of collapsing to generic any.called.
inline facebook::jsi::Value MakeMethodStub(facebook::jsi::Runtime& rt,
                                           const std::string& label) {
	try {
		facebook::jsi::Value any = rt.global().getProperty(rt, "__kytyAny");
		if (any.isObject()) {
			// Wrap: a HostFunction that logs the label then forwards to the
			// generic proxy by returning it (preserves thenable/callable).
			auto fn = facebook::jsi::Function::createFromHostFunction(
			    rt, facebook::jsi::PropNameID::forAscii(rt, label.c_str()), 8,
			    [label](facebook::jsi::Runtime& rt2,
			            const facebook::jsi::Value&,
			            const facebook::jsi::Value* args,
			            size_t count) -> facebook::jsi::Value {
				    std::string summary;
				    for (size_t i = 0; i < count && i < 3; i++) {
					    try {
						    const auto& v = args[i];
						    if (v.isString()) {
							    std::string s = v.toString(rt2).utf8(rt2);
							    if (s.size() > 60) {
								    s.resize(60);
							    }
							    summary += " str:" + s;
						    } else if (v.isNumber() || v.isBool()) {
							    summary += " num/bool";
			    } else if (v.isNull() || v.isUndefined()) {
						    summary += " null/undef";
					    } else if (v.isObject()) {
						    try {
							    facebook::jsi::Object o =
								    const_cast<facebook::jsi::Value&>(v)
								        .asObject(rt2);
							    facebook::jsi::Array names =
								    o.getPropertyNames(rt2);
							    size_t n = names.size(rt2);
							    std::string keys, fns;
							    for (size_t k = 0; k < n && k < 12; k++) {
								    try {
									    std::string kk =
									        names.getValueAtIndex(rt2, k)
									            .toString(rt2)
									            .utf8(rt2);
									    if (k > 0) {
										    keys += ",";
									    }
									    if (kk.size() > 40) {
										    kk.resize(40);
									    }
									    keys += kk;
									    try {
										    facebook::jsi::Value pv =
											    o.getProperty(rt2, kk.c_str());
										    if (pv.isObject() &&
										        pv.asObject(rt2).isFunction(
										            rt2)) {
											    if (!fns.empty()) {
												    fns += ",";
											    }
											    fns += kk;
										    }
									    } catch (...) {
									    }
								    } catch (...) {
								    }
							    }
							    summary += " obj{keys=" + keys +
							        " fns=" + fns + "}";
						    } catch (...) {
							    summary += " obj";
						    }
					    } else {
							    summary += " ?";
						    }
					    } catch (...) {
						    summary += " <unprintable>";
					    }
				    }
				    LOGF("\t [RNPS host] %s(...) argc=%zu%s -> any-proxy\n",
				         label.c_str(), count, summary.c_str());
				    (void)args;
				    try {
					    facebook::jsi::Value any2 =
					        rt2.global().getProperty(rt2, "__kytyAny");
					    if (any2.isObject()) {
						    return any2;
					    }
				    } catch (...) {
				    }
				    return facebook::jsi::Value::undefined();
			    });
			return fn;
		}
	} catch (...) {
	}
	return GetAnyProxy(rt);
}

// ps5-vsh: Paper UIManager (mount surface RE'd from ReactNative.PUI +
// NPXS40062 bundle refs: getEnforcing/requireNativeComponent/getConstants
// first, then createView/updateView/setChildren/manageChildren,
// dispatchViewManagerCommand, measure*. No Fabric (1 dormant token).
// ps5-vsh: Paper UIManager (mount surface RE'd from ReactNative.PUI +
// NPXS40062 bundle refs: getEnforcing/requireNativeComponent/getConstants
// first, then createView/updateView/setChildren/manageChildren,
// dispatchViewManagerCommand, measure*. No Fabric (1 dormant token).
//
// Bring-up renderer: createView/updateView/setChildren maintain a view tree
// (tag/parent/name/rect/color); RnRasterize paints it CPU-side into the
// composite RN surface (solid rects; text/icons later). RnTestcard draws
// three boxes without any JS trigger (KYTY_RN_TESTCARD=1) to verify the
// present path end to end.
struct RnViewRect {
	uint32_t    tag    = 0;
	uint32_t    parent = 0;
	std::string name;
	double      x = 0, y = 0, w = 0, h = 0;
	bool        hasRect = false;
	uint32_t    rgba    = 0;
	bool        hasColor = false;
};
static std::mutex                                        g_rnViewMutex;
static std::unordered_map<uint32_t, RnViewRect>          g_rnViews;
static std::unordered_map<uint32_t, std::vector<uint32_t>> g_rnKids;

static uint32_t RnColorForName(const std::string& name, uint32_t tag) {
	// Deterministic palette so unstyled views are distinguishable.
	// u32 ABGR (byte order R,G,B,A in memory).
	static const uint32_t kPal[] = {0xffde862e, 0xff84ac10, 0xff5352ee, 0xff57cafe,
	                                0xffcd275f, 0xffa4a301, 0xffa79583, 0xff3e2f22};
	if (name == "RCTRawText" || name == "RCTText" || name == "RCTVirtualText") {
		return 0xfffaf6f5;
	}
	if (name.find("Image") != std::string::npos) {
		return 0xffffa054;
	}
	return kPal[tag % 8];
}

static double RnNumProp(facebook::jsi::Runtime& rt, const facebook::jsi::Object& props,
                        const char* key, bool* ok) {
	try {
		facebook::jsi::Value v = props.getProperty(rt, key);
		if (v.isNumber()) {
			if (ok != nullptr) {
				*ok = true;
			}
			return v.asNumber();
		}
	} catch (...) {
	}
	if (ok != nullptr) {
		*ok = false;
	}
	return 0;
}

static void RnRecordView(uint32_t tag, const std::string& name, uint32_t parent,
                         facebook::jsi::Runtime& rt, const facebook::jsi::Object* props) {
	RnViewRect v;
	v.tag    = tag;
	v.name   = name;
	v.parent = parent;
	if (props != nullptr) {
		bool ok = false;
		const double w = RnNumProp(rt, *props, "width", &ok);
		if (ok && w > 0 && w < 4096) {
			v.w       = w;
			v.hasRect = true;
		}
		const double h = RnNumProp(rt, *props, "height", &ok);
		if (ok && h > 0 && h < 4096) {
			v.h = h;
		}
		const double x = RnNumProp(rt, *props, "left", &ok);
		if (ok) {
			v.x = x;
		}
		const double y = RnNumProp(rt, *props, "top", &ok);
		if (ok) {
			v.y = y;
		}
		const double bg = RnNumProp(rt, *props, "backgroundColor", &ok);
		if (ok) {
			const uint32_t u = static_cast<uint32_t>(bg);
			// RN packs 0xAARRGGBB-ish; normalize to ABGR u32 (R,G,B,A bytes).
			const uint32_t a = (u >> 24) & 0xffu;
			if (a == 0xffu || a == 0x00u) {
				const uint32_t r = (u >> 16) & 0xffu;
				const uint32_t g = (u >> 8) & 0xffu;
				const uint32_t b = u & 0xffu;
				v.rgba     = 0xff000000u | (b << 16) | (g << 8) | r;
				v.hasColor = true;
			}
		}
	}
	if (!v.hasColor) {
		v.rgba     = RnColorForName(name, tag);
		v.hasColor = true;
	}
	std::lock_guard<std::mutex> lock(g_rnViewMutex);
	auto it = g_rnViews.find(tag);
	if (it != g_rnViews.end()) {
		// Merge: updateView carries partial props; preserve rect/parent.
		if (!v.name.empty() && v.name != "<none>") {
			it->second.name = v.name;
		}
		if (v.hasRect || v.w > 0) {
			it->second.x       = v.x;
			it->second.y       = v.y;
			it->second.w       = v.w;
			it->second.h       = v.h;
			it->second.hasRect = v.hasRect;
		}
		if (v.hasColor) {
			it->second.rgba     = v.rgba;
			it->second.hasColor = true;
		}
		if (parent != 0) {
			it->second.parent = parent;
			g_rnKids[parent].push_back(tag);
		}
		return;
	}
	if (parent != 0) {
		g_rnKids[parent].push_back(tag);
	}
	g_rnViews[tag] = v;
}

static void RnFillRect(uint32_t* px, uint32_t W, uint32_t H, double fx, double fy, double fw,
                       double fh, uint32_t rgba) {
	int x0 = static_cast<int>(fx);
	int y0 = static_cast<int>(fy);
	int x1 = static_cast<int>(fx + fw);
	int y1 = static_cast<int>(fy + fh);
	if (x0 < 0) {
		x0 = 0;
	}
	if (y0 < 0) {
		y0 = 0;
	}
	if (x1 > static_cast<int>(W)) {
		x1 = static_cast<int>(W);
	}
	if (y1 > static_cast<int>(H)) {
		y1 = static_cast<int>(H);
	}
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			px[static_cast<size_t>(y) * W + x] = rgba;
		}
	}
}

static void RnRasterize() {
	const uint64_t addr = Libs::Composite::CompositeHost::RnSurfaceAddr();
	if (addr == 0) {
		return;
	}
	auto* px = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(addr));
	constexpr uint32_t W = 1280, H = 720;
	// PS-shell backdrop (ABGR u32: deep blue, subtle vertical shade).
	for (uint32_t y = 0; y < H; y++) {
		const uint32_t shade = 0xff241814u + (y * 0x00000008u);
		for (uint32_t x = 0; x < W; x++) {
			px[static_cast<size_t>(y) * W + x] = shade;
		}
	}
	std::lock_guard<std::mutex> lock(g_rnViewMutex);
	// Grid fallback for views without layout: tile by creation order.
	uint32_t i = 0;
	for (const auto& kv : g_rnViews) {
		const RnViewRect& v = kv.second;
		if (v.hasRect && v.w > 0 && v.h > 0) {
			RnFillRect(px, W, H, v.x, v.y, v.w, v.h, v.rgba);
		} else {
			const double cw = 200, ch = 140, gap = 24;
			const uint32_t col = i % 5, row = (i / 5) % 3;
			RnFillRect(px, W, H, 80 + col * (cw + gap), 120 + row * (ch + gap), cw, ch,
			           v.rgba);
		}
		i++;
	}
	Libs::Composite::CompositeHost::RnPresent();
	LOGF("\t [RNPS host] RnRasterize: %u views presented\n", i);
}

static void RnTestcard() {
	{
		std::lock_guard<std::mutex> lock(g_rnViewMutex);
		g_rnViews.clear();
		g_rnKids.clear();
		RnViewRect a {};
		a.tag = 1;
		a.name = "TESTCARD-A";
		a.rgba = 0xff3c4ce7;
		a.hasColor = true;
		RnViewRect b {};
		b.tag = 2;
		b.name = "TESTCARD-B";
		b.rgba = 0xff71cc2e;
		b.hasColor = true;
		RnViewRect c {};
		c.tag = 3;
		c.name = "TESTCARD-C";
		c.rgba = 0xffdb9834;
		c.hasColor = true;
		g_rnViews[1] = a;
		g_rnViews[2] = b;
		g_rnViews[3] = c;
	}
	RnRasterize();
}

// ps5-vsh: bring-up home grid (KYTY_RN_HOMEGRID=1). NOT Sony's UI: draws FW
// titles (param.json names + icon0.png) in a grid through the same RN
// surface/present path the faithful views will reuse. Exercises titles,
// PNG decode, TTF text, layout, raster, present end to end.
struct RnGridTitle {
	std::string          id;
	std::string          name;
	std::vector<uint8_t> icon; // RGBA, iconW*iconH
	int                  iconW = 0, iconH = 0;
};

static std::vector<uint32_t> RnDecodePngFile(const std::string& path, int* w, int* h) {
	int            x = 0, y = 0, comp = 0;
	stbi_uc*       px = stbi_load(path.c_str(), &x, &y, &comp, 4);
	std::vector<uint32_t> out;
	if (px != nullptr && x > 0 && y > 0) {
		out.resize(static_cast<size_t>(x) * y);
		std::memcpy(out.data(), px, out.size() * 4);
		if (w != nullptr) {
			*w = x;
		}
		if (h != nullptr) {
			*h = y;
		}
		stbi_image_free(px);
	}
	return out;
}

static void RnBlitIcon(uint32_t* fb, uint32_t W, uint32_t H, int dx, int dy, int dw, int dh,
                       const uint8_t* src, int sw, int sh) {
	if (src == nullptr || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
		return;
	}
	for (int y = 0; y < dh; y++) {
		const int sy = y * sh / dh;
		const int fy = dy + y;
		if (fy < 0 || fy >= static_cast<int>(H)) {
			continue;
		}
		for (int x = 0; x < dw; x++) {
			const int sx = x * sw / dw;
			const int fx = dx + x;
			if (fx < 0 || fx >= static_cast<int>(W)) {
				continue;
			}
			const uint8_t* p = src + (static_cast<size_t>(sy) * sw + sx) * 4;
			const uint32_t a = p[3];
			if (a == 0) {
				continue;
			}
			uint32_t* d = &fb[static_cast<size_t>(fy) * W + fx];
			if (a == 255) {
				*d = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
				       (static_cast<uint32_t>(p[2]) << 16) | 0xff000000u;
			} else {
				const uint32_t dr = (*d) & 0xffu, dg = (*d >> 8) & 0xffu, db = (*d >> 16) & 0xffu;
				const uint32_t r = (p[0] * a + dr * (255 - a)) / 255;
				const uint32_t g = (p[1] * a + dg * (255 - a)) / 255;
				const uint32_t b = (p[2] * a + db * (255 - a)) / 255;
				*d               = r | (g << 8) | (b << 16) | 0xff000000u;
			}
		}
	}
}

static std::vector<uint32_t> RnDecodeUtf8(const std::string& s) {
	std::vector<uint32_t> out;
	for (size_t i = 0; i < s.size();) {
		const uint8_t c = static_cast<uint8_t>(s[i]);
		if (c < 0x80) {
			out.push_back(c);
			i++;
		} else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) {
			out.push_back(((c & 0x1f) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3f));
			i += 2;
		} else if ((c & 0xf0) == 0xe0 && i + 2 < s.size()) {
			out.push_back(((c & 0x0f) << 12) |
			              ((static_cast<uint8_t>(s[i + 1]) & 0x3f) << 6) |
			              (static_cast<uint8_t>(s[i + 2]) & 0x3f));
			i += 3;
		} else {
			i++;
		}
	}
	return out;
}

static void RnDrawText(uint32_t* fb, uint32_t W, uint32_t H, stbtt_fontinfo* font, float scale,
                       int x, int y, const std::string& text, uint32_t rgba) {
	if (font == nullptr) {
		return;
	}
	const uint8_t r = rgba & 0xffu, g = (rgba >> 8) & 0xffu, b = (rgba >> 16) & 0xffu;
	int           cx = x;
	for (const uint32_t cp : RnDecodeUtf8(text)) {
		if (cp < 32) {
			continue;
		}
		int adv = 0, lsb = 0;
		stbtt_GetCodepointHMetrics(font, static_cast<int>(cp), &adv, &lsb);
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		stbtt_GetCodepointBitmapBox(font, static_cast<int>(cp), scale, scale, &x0, &y0, &x1,
		                            &y1);
		const int gw = x1 - x0, gh = y1 - y0;
		if (gw > 0 && gh > 0 && gw < 256 && gh < 256) {
			std::vector<uint8_t> bmp(static_cast<size_t>(gw) * gh);
			stbtt_MakeCodepointBitmap(font, bmp.data(), gw, gh, gw, scale, scale,
			                          static_cast<int>(cp));
			for (int yy = 0; yy < gh; yy++) {
				for (int xx = 0; xx < gw; xx++) {
					const uint8_t cov = bmp[static_cast<size_t>(yy) * gw + xx];
					if (cov == 0) {
						continue;
					}
					const int fx = cx + x0 + xx, fy = y + y0 + yy;
					if (fx < 0 || fy < 0 || fx >= static_cast<int>(W) ||
					    fy >= static_cast<int>(H)) {
						continue;
					}
					uint32_t* d  = &fb[static_cast<size_t>(fy) * W + fx];
					const uint32_t dr = (*d) & 0xffu, dg = (*d >> 8) & 0xffu,
					               db = (*d >> 16) & 0xffu;
					const uint32_t nr = (r * cov + dr * (255 - cov)) / 255;
					const uint32_t ng = (g * cov + dg * (255 - cov)) / 255;
					const uint32_t nb = (b * cov + db * (255 - cov)) / 255;
					*d = nr | (ng << 8) | (nb << 16) | 0xff000000u;
				}
			}
		}
		cx += static_cast<int>(adv * scale);
	}
}

static std::atomic<size_t> g_homeFocus = 0;
static std::atomic<size_t> g_homeCount  = 0;
static std::string         g_homeBanner;

static void RnHomeGridDraw(size_t focus);
static std::vector<RnGridTitle>     g_homeTitles;
static bool                         g_homeScanned = false;
static std::mutex                   g_homeTitlesMutex;
static void RnHomeGridPump();

static void RnHomeGrid() {
	RnHomeGridDraw(g_homeFocus.load());
	RnHomeGridPump();
}

// Bring-up input pump: arrows move focus, Enter logs the selected title.
// Detached thread, SDL keyboard state, edge-triggered with hold repeat.
static void RnHomeGridPump() {
	static std::atomic<bool> s_started = false;
	if (s_started.exchange(true)) {
		return;
	}
	LOGF("\t [RNPS host] home grid pump started\n");
	std::thread([] {
		// ps5-vsh: deterministic sweep (KYTY_RN_SWEEP=1) for headless
		// verification: advance focus on a timer, no input needed.
		const bool sweep = std::getenv("KYTY_RN_SWEEP") != nullptr;
		unsigned int sweepMs   = 0;
		for (;;) {
			{ static unsigned long long it = 0;
			if ((++it % 84) == 0) { std::fprintf(stderr, "pump iter %llu\n", it); std::fflush(stderr); } }
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
			{ static unsigned long long lastBeat = 0;
			unsigned long long nowB = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			if (nowB - lastBeat > 5000) { lastBeat = nowB;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
				int dbg = 0;
				if ((GetAsyncKeyState(0x25) & 0x8000) != 0) { dbg |= 1; }
				if ((GetAsyncKeyState(0x27) & 0x8000) != 0) { dbg |= 2; }
				if ((GetAsyncKeyState(0x26) & 0x8000) != 0) { dbg |= 4; }
				if ((GetAsyncKeyState(0x28) & 0x8000) != 0) { dbg |= 8; }
				if ((GetAsyncKeyState(0x0D) & 0x8000) != 0) { dbg |= 16; }
				LOGF("\t [RNPS host] home grid pump alive keys=%d\n", dbg);
#else
				LOGF("\t [RNPS host] home grid pump alive\n");
#endif
			} }
			if (sweep) {
				sweepMs += 60;
				if (sweepMs >= 700) {
					sweepMs = 0;
					const size_t count = g_homeCount.load();
					if (count > 0) {
						g_homeFocus.store((g_homeFocus.load() + 1) % count);
						LOGF("\t [RNPS host] home grid sweep @%zu\n", g_homeFocus.load());
						RnHomeGridDraw(g_homeFocus.load());
					}
				}
				continue;
			}
		// NOTE (ps5-vsh): no SDL_PumpEvents/SDL_PollEvent here. The pump
		// runs on a worker thread while the main thread owns the SDL
		// window; pumping SDL from this thread deadlocks (first pass
		// never returns: exactly one heartbeat, then silence). Input
		// uses Win32 async key state (focus-independent, no SDL needed).
			static unsigned long long lastFireMs = 0;
			auto nowMs = [] {
			    return static_cast<unsigned long long>(
			        std::chrono::duration_cast<std::chrono::milliseconds>(
			            std::chrono::steady_clock::now().time_since_epoch())
			            .count());
			};
		uint32_t pressed = 0;
		static uint32_t heldMask = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			auto vkd = [](int vk) -> bool {
			    return (GetAsyncKeyState(vk) & 0x8000) != 0;
			};
			uint32_t cur = 0;
			if (vkd(0x25)) { cur |= 1u; }
			if (vkd(0x27)) { cur |= 2u; }
			if (vkd(0x26)) { cur |= 4u; }
			if (vkd(0x28)) { cur |= 8u; }
			if (vkd(0x0D)) { cur |= 16u; }
		if (cur != 0 && cur != heldMask) { pressed = cur; }
		else if (cur != 0 && cur == heldMask && nowMs() - lastFireMs >= 350) { pressed = cur; }
		heldMask = cur;
#endif
		// Headless command channel: _TempData/home_cmd.txt with one of
		// left/right/up/down/enter. Consumed via atomic rename so a
		// command can never fire twice (plain read+delete races AV/
		// indexer locks: delete fails silently, stale file refires in
		// the next grid). Works where synthetic input cannot reach
		// the process (service sessions, no input desktop).
		{
			const char* busy = "_TempData/home_cmd.busy";
			std::remove(busy);
			bool have = (std::rename("_TempData/home_cmd.txt", busy) == 0);
			if (have) {
				std::string word;
				if (std::ifstream cmd{busy}) {
					cmd >> word;
				}
				std::remove(busy);
				uint32_t cm = 0;
				if (word == "left") { cm = 1u; }
				else if (word == "right") { cm = 2u; }
				else if (word == "up") { cm = 4u; }
				else if (word == "down") { cm = 8u; }
				else if (word == "enter") { cm = 16u; }
				if (cm != 0) {
					LOGF("\t [RNPS host] home grid cmd %s\n", word.c_str());
					pressed |= cm;
				}
			}
		}
			uint32_t mask = pressed;
			const bool fire = (mask != 0);
			if (!fire) {
			    continue;
			}
			lastFireMs = nowMs();
			const size_t count = g_homeCount.load();
			if (count == 0) {
				continue;
			}
			if ((mask & 16u) != 0) {
				std::string launched;
				{
					std::lock_guard<std::mutex> tlock(g_homeTitlesMutex);
					const size_t idx = g_homeFocus.load();
					if (idx < g_homeTitles.size()) {
						launched = g_homeTitles[idx].id;
					}
				}
				if (launched.empty()) {
					continue;
				}
				LOGF("\t [RNPS host] home grid launch %s\n", launched.c_str());
				{
					std::ofstream f("_TempData/home_launch.txt", std::ios::trunc);
					if (f.good()) {
						f << launched;
					}
				}
				g_homeBanner = "Launching " + launched + " ...";
				RnHomeGridDraw(g_homeFocus.load());
				std::this_thread::sleep_for(std::chrono::milliseconds(1200));
				std::quick_exit(42);
			}
			size_t f   = g_homeFocus.load();
			constexpr size_t kPerRow = 6;
			if ((mask & 1u) != 0) {
				f = (f == 0) ? count - 1 : f - 1;
			}
			if ((mask & 2u) != 0) {
				f = (f + 1) % count;
			}
			if ((mask & 4u) != 0) {
				f = (f + count - (kPerRow % count)) % count;
			}
			if ((mask & 8u) != 0) {
				f = (f + kPerRow) % count;
			}
			if (f != g_homeFocus.load()) {
				g_homeFocus.store(f);
				LOGF("\t [RNPS host] home grid focus @%zu\n", f);
				RnHomeGridDraw(f);
			}
		}
	}).detach();
}

static void RnHomeGridDraw(size_t focus) {
	// Titles: FW system apps (param.json titleName + icon0.png), scanned once.
	std::lock_guard<std::mutex> tlock(g_homeTitlesMutex);
	if (!g_homeScanned) {
		g_homeScanned = true;
		const std::filesystem::path appRoot =
		    Libs::LibKernel::FileSystem::GetRealFilename("/system_ex/app");
		LOGF("\t [RNPS host] home grid appRoot: %s\n", appRoot.string().c_str());
		std::vector<RnGridTitle> found;
		{
			std::error_code ec;
			for (const auto& de : std::filesystem::directory_iterator(appRoot, ec)) {
				if (!de.is_directory(ec)) {
					continue;
				}
				RnGridTitle t;
				t.id   = de.path().filename().string();
				t.name = t.id;
				const auto pj = de.path() / "sce_sys" / "param.json";
				{
					std::ifstream f(pj);
					if (f.good()) {
						try {
							auto j = nlohmann::json::parse(f);
							const auto& loc =
							    j.value("localizedParameters", nlohmann::json::object());
							const auto& en =
							    loc.value("en-US", nlohmann::json::object());
							const std::string nm = en.value("titleName", "");
							if (!nm.empty()) {
								t.name = nm;
							}
						} catch (...) {
						}
					}
				}
				const auto ip = de.path() / "sce_sys" / "icon0.png";
				{
					std::error_code ec2;
					if (std::filesystem::exists(ip, ec2)) {
						auto px = RnDecodePngFile(ip.string(), &t.iconW, &t.iconH);
						if (!px.empty()) {
							t.icon.assign(reinterpret_cast<uint8_t*>(px.data()),
							              reinterpret_cast<uint8_t*>(px.data()) + px.size() * 4);
						}
					}
				}
				if (t.id.rfind("NPXS", 0) == 0 || t.id.rfind("CUSA", 0) == 0) {
					found.push_back(std::move(t));
				}
			}
		}
		std::sort(found.begin(), found.end(),
		          [](const RnGridTitle& a, const RnGridTitle& b) { return a.id < b.id; });
		LOGF("\t [RNPS host] home grid: %zu titles\n", found.size());
		g_homeTitles = std::move(found);
		g_homeCount.store(g_homeTitles.size());
	}
	const std::vector<RnGridTitle>& titles = g_homeTitles;
	// Font (Sony DFHEI5; silent without it).
	static std::vector<uint8_t> s_fontBytes;
	static stbtt_fontinfo        s_font {};
	static bool                  s_fontOk = false;
	static bool                  s_fontTried = false;
	if (!s_fontTried) {
		s_fontTried = true;
		const auto fp = Libs::LibKernel::FileSystem::GetRealFilename(
		    "/common/font2/DFHEI5-SONY2022.ttf");
		LOGF("\t [RNPS host] home grid font path: %s\n", fp.string().c_str());
		std::ifstream f(fp, std::ios::binary);
		if (f.good()) {
			s_fontBytes.assign(std::istreambuf_iterator<char>(f),
			                   std::istreambuf_iterator<char>());
			if (!s_fontBytes.empty() &&
			    stbtt_InitFont(&s_font, s_fontBytes.data(),
			                   stbtt_GetFontOffsetForIndex(s_fontBytes.data(), 0)) != 0) {
				s_fontOk = true;
			}
		}
		LOGF("\t [RNPS host] home grid font: %s (%zu bytes)\n",
		     s_fontOk ? "ok" : "missing", s_fontBytes.size());
	}
	const float scale = s_fontOk ? stbtt_ScaleForPixelHeight(&s_font, 22.0f) : 0.0f;
	// Raster.
	const uint64_t addr = Libs::Composite::CompositeHost::RnSurfaceAddr();
	if (addr == 0) {
		return;
	}
	auto*          px = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(addr));
	constexpr uint32_t W = 1280, H = 720;
	for (uint32_t y = 0; y < H; y++) {
		const uint32_t shade = 0xff30201cu + ((y >> 4) * 0x10000u);
		for (uint32_t x = 0; x < W; x++) {
			px[static_cast<size_t>(y) * W + x] = shade;
		}
	}
	// Top bar.
	RnFillRect(px, W, H, 0, 0, W, 64, 0xff201310u);
	if (s_fontOk) {
		std::time_t tt = std::time(nullptr);
		std::tm     tm {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		localtime_s(&tm, &tt);
#else
		tm = *std::localtime(&tt);
#endif
		char clock[32] = {};
		std::snprintf(clock, sizeof(clock), "%d:%02d", tm.tm_hour, tm.tm_min);
		RnDrawText(px, W, H, &s_font, scale, 1150, 20, clock, 0xfff5f6fa);
		RnDrawText(px, W, H, &s_font, scale, 40, 20, "KY TY SHELL", 0xfff5f6fa);
	}
	// Tiles.
	const int tw = 168, th = 168, gap = 28, x0 = 80, y0 = 120, perRow = 6;
	for (size_t i = 0; i < titles.size(); i++) {
		const int cx = x0 + static_cast<int>(i % perRow) * (tw + gap);
		const int cy = y0 + static_cast<int>(i / perRow) * (th + 44 + gap);
		if (cy + th + 30 >= static_cast<int>(H)) {
			break;
		}
		const RnGridTitle& t = titles[i];
		const bool         focused = (i == focus);
		RnFillRect(px, W, H, cx - 4, cy - 4, tw + 8, th + 8,
		           focused ? 0xfff5f6fa : 0xff2a2f3a);
		if (!t.icon.empty()) {
			RnBlitIcon(px, W, H, cx, cy, tw, th, t.icon.data(), t.iconW, t.iconH);
		} else {
			const uint32_t pal[] = {0xff3c4ce7, 0xff71cc2e, 0xffdb9834, 0xff9b59b6,
			                        0xff1abc9c, 0xfff39c12};
			RnFillRect(px, W, H, cx, cy, tw, th, pal[i % 6]);
		}
		if (s_fontOk) {
			RnDrawText(px, W, H, &s_font, scale * 0.8f, cx, cy + th + 8,
			           t.name.size() > 22 ? t.name.substr(0, 22) : t.name, 0xfff5f6fa);
		}
	}
	if (s_fontOk && !g_homeBanner.empty()) {
		RnFillRect(px, W, H, 320, 300, 640, 120, 0xff101320u);
		RnFillRect(px, W, H, 320, 300, 640, 4, 0xfff5f6fa);
		RnDrawText(px, W, H, &s_font, scale, 360, 348, g_homeBanner, 0xfff5f6fa);
	}
	Libs::Composite::CompositeHost::RnPresent();
	LOGF("\t [RNPS host] home grid presented (%zu titles)\n", titles.size());
	if (std::getenv("KYTY_RN_DUMPFRAME") != nullptr) {
		const uint64_t addr = Libs::Composite::CompositeHost::RnSurfaceAddr();
		if (addr != 0) {
			const char* output = std::getenv("KYTY_RN_DUMPFRAME_PATH");
			FILE* f = std::fopen(output != nullptr && output[0] != '\0' ? output : "frame0.rgba", "wb");
			if (f != nullptr) {
				std::fwrite(reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), 1,
				            1280u * 720u * 4u, f);
				std::fclose(f);
			}
		}
	}
}

inline facebook::jsi::Object BuildUIManager(facebook::jsi::Runtime& rt) {
	facebook::jsi::Object ui(rt);
	auto ack = [](const char* label) {
		return [label](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		               const facebook::jsi::Value* args,
		               size_t count) -> facebook::jsi::Value {
			(void)args;
			(void)count;
			(void)rt2;
			LOGF("\t [RNPS host] UIManager.%s() -> ack\n", label);
			return facebook::jsi::Value::undefined();
		};
	};
	ui.setProperty(rt, "getConstants",
	               facebook::jsi::Function::createFromHostFunction(
	                   rt, facebook::jsi::PropNameID::forAscii(rt, "getConstants"), 0,
	                   [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
	                      const facebook::jsi::Value* args,
	                      size_t count) -> facebook::jsi::Value {
		                   (void)args;
		                   (void)count;
		                   // voldemort rnhost parity: RN needs the SHAPE, not an
		                   // empty object. attachDefaultEventTypes branches on
		                   // ViewManagerNames / LazyViewManagersEnabled, and with
		                   // LazyViewManagersEnabled it pulls each RCT* config on
		                   // demand via getConstantsForViewManager.
		                   facebook::jsi::Object c(rt2);
		                   c.setProperty(rt2, "ViewManagerNames",
		                                 facebook::jsi::Array(rt2, 0));
		                   c.setProperty(rt2, "LazyViewManagersEnabled", true);
		                   c.setProperty(rt2, "genericBubblingEventTypes",
		                                 facebook::jsi::Object(rt2));
		                   c.setProperty(rt2, "genericDirectEventTypes",
		                                 facebook::jsi::Object(rt2));
		                   LOGF("\t [RNPS host] UIManager.getConstants() -> "
		                        "{ViewManagerNames:[], LazyViewManagersEnabled:true}\n");
		                   return c;
	                   }));
	ui.setProperty(
	    rt, "getConstantsForViewManager",
	    facebook::jsi::Function::createFromHostFunction(
	        rt, facebook::jsi::PropNameID::forAscii(rt, "getConstantsForViewManager"), 1,
	        [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
	           const facebook::jsi::Value* args,
	           size_t count) -> facebook::jsi::Value {
		        (void)args;
		        (void)count;
		        LOGF("\t [RNPS host] UIManager.getConstantsForViewManager() -> {}\n");
		        return facebook::jsi::Object(rt2);
	        }));
	static std::atomic<uint32_t> s_nextTag {1000};
	ui.setProperty(
	    rt, "createView",
	    facebook::jsi::Function::createFromHostFunction(
	        rt, facebook::jsi::PropNameID::forAscii(rt, "createView"), 4,
	        [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
	           const facebook::jsi::Value* args,
	           size_t count) -> facebook::jsi::Value {
		        std::string view =
		            count >= 2 ? args[1].toString(rt2).utf8(rt2) : "<none>";
		        uint32_t tag = 0;
		        if (count >= 1 && args[0].isNumber()) {
			        tag = static_cast<uint32_t>(args[0].asNumber());
		        }
		        if (tag == 0) {
			        tag = s_nextTag.fetch_add(1);
		        }
		        LOGF("\t [RNPS host] UIManager.createView(%u, %s) -> %u\n", tag,
		             view.c_str(), tag);
		        (void)count;
		        try {
			        const facebook::jsi::Object* props = nullptr;
			        facebook::jsi::Object       held(rt2);
			        if (count >= 4 && args[3].isObject()) {
				        held  = args[3].asObject(rt2);
				        props = &held;
			        }
			        // Parent arrives via setChildren; rootTag (args[2])
			        // is not the parent.
			        RnRecordView(tag, view, 0, rt2, props);
		        } catch (...) {
		        }
		        return facebook::jsi::Value(static_cast<double>(tag));
	        }));
	ui.setProperty(rt, "updateView",
	               facebook::jsi::Function::createFromHostFunction(
	                   rt, facebook::jsi::PropNameID::forAscii(rt, "updateView"), 3,
	                   [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
	                      const facebook::jsi::Value* args,
	                      size_t count) -> facebook::jsi::Value {
		                   uint32_t tag = 0;
		                   if (count >= 1 && args[0].isNumber()) {
			                   tag = static_cast<uint32_t>(args[0].asNumber());
		                   }
		                   std::string view =
		                       count >= 2 ? args[1].toString(rt2).utf8(rt2) : "<none>";
		                   LOGF("\t [RNPS host] UIManager.updateView(%u, %s)\n", tag,
		                        view.c_str());
		                   try {
			                   const facebook::jsi::Object* props = nullptr;
			                   facebook::jsi::Object       held(rt2);
			                   if (count >= 3 && args[2].isObject()) {
				                   held  = args[2].asObject(rt2);
				                   props = &held;
			                   }
			                   if (tag != 0) {
				                   RnRecordView(tag, view, 0, rt2, props);
			                   }
		                   } catch (...) {
		                   }
		                   return facebook::jsi::Value::undefined();
	                   }));
	ui.setProperty(rt, "setChildren",
	               facebook::jsi::Function::createFromHostFunction(
	                   rt, facebook::jsi::PropNameID::forAscii(rt, "setChildren"), 2,
	                   [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
	                      const facebook::jsi::Value* args,
	                      size_t count) -> facebook::jsi::Value {
		                   uint32_t container = 0;
		                   if (count >= 1 && args[0].isNumber()) {
			                   container = static_cast<uint32_t>(args[0].asNumber());
		                   }
		                   LOGF("\t [RNPS host] UIManager.setChildren(%u) -> rasterize\n",
		                        container);
		                   try {
			                   if (count >= 2 && args[1].isObject()) {
				                   facebook::jsi::Array kids =
				                       args[1].asObject(rt2).asArray(rt2);
				                   const size_t n = kids.size(rt2);
				                   std::lock_guard<std::mutex> lock(g_rnViewMutex);
				                   auto& vec = g_rnKids[container];
				                   vec.clear();
				                   for (size_t i = 0; i < n; i++) {
					                   facebook::jsi::Value v = kids.getValueAtIndex(rt2, i);
					                   if (v.isNumber()) {
						                   vec.push_back(static_cast<uint32_t>(v.asNumber()));
					                   }
				                   }
			                   }
		                   } catch (...) {
		                   }
		                   RnRasterize();
		                   return facebook::jsi::Value::undefined();
	                   }));
	ui.setProperty(rt, "manageChildren",
	               facebook::jsi::Function::createFromHostFunction(
	                   rt, facebook::jsi::PropNameID::forAscii(rt, "manageChildren"),
	                   6, ack("manageChildren")));
	ui.setProperty(
	    rt, "dispatchViewManagerCommand",
	    facebook::jsi::Function::createFromHostFunction(
	        rt,
	        facebook::jsi::PropNameID::forAscii(rt, "dispatchViewManagerCommand"),
	        3, ack("dispatchViewManagerCommand")));
	auto measure = [](bool inWindow, const char* label) {
		return [inWindow, label](facebook::jsi::Runtime& rt2,
		                         const facebook::jsi::Value&,
		                         const facebook::jsi::Value* args,
		                         size_t count) -> facebook::jsi::Value {
			LOGF("\t [RNPS host] UIManager.%s() -> zeros\n", label);
			if (count >= 2 && args[count - 1].isObject() &&
			    args[count - 1].asObject(rt2).isFunction(rt2)) {
				if (inWindow) {
					facebook::jsi::Value v[] = {
					    facebook::jsi::Value(0.0), facebook::jsi::Value(0.0),
					    facebook::jsi::Value(1920.0),
					    facebook::jsi::Value(1080.0)};
					args[count - 1].asObject(rt2).asFunction(rt2).call(
					    rt2, static_cast<const facebook::jsi::Value*>(v),
					    static_cast<size_t>(4));
				} else {
					facebook::jsi::Value v[] = {
					    facebook::jsi::Value(0.0), facebook::jsi::Value(0.0),
					    facebook::jsi::Value(0.0), facebook::jsi::Value(0.0),
					    facebook::jsi::Value(0.0), facebook::jsi::Value(0.0)};
					args[count - 1].asObject(rt2).asFunction(rt2).call(
					    rt2, static_cast<const facebook::jsi::Value*>(v),
					    static_cast<size_t>(6));
				}
			}
			return facebook::jsi::Value::undefined();
		};
	};
	ui.setProperty(
	    rt, "measure",
	    facebook::jsi::Function::createFromHostFunction(
	        rt, facebook::jsi::PropNameID::forAscii(rt, "measure"), 2,
	        measure(false, "measure")));
	ui.setProperty(
	    rt, "measureInWindow",
	    facebook::jsi::Function::createFromHostFunction(
	        rt, facebook::jsi::PropNameID::forAscii(rt, "measureInWindow"), 2,
	        measure(true, "measureInWindow")));
	return ui;
}

class TurboModuleStub : public facebook::jsi::HostObject {
public:
	explicit TurboModuleStub(std::string module) : module_(std::move(module)) {}

	facebook::jsi::Value get(facebook::jsi::Runtime& rt,
	                         const facebook::jsi::PropNameID& name) override {
		const std::string method = name.utf8(rt);
		const std::string module = module_;
		// ps5-vsh: route-config mutual exclusion (react-navigation throws
		// if a route declares both screen and getScreen; stub sources must
		// not fabricate the missing twin). Path reads null (parsers accept
		// string-or-null; real values never traverse these traps).
		if (method == "screen" || method == "getScreen") {
			return facebook::jsi::Value::undefined();
		}
		if (method == "path") {
			return facebook::jsi::Value::null();
		}
		LOGF("\t [RNPS host] %s.%s -> any-proxy\n", module.c_str(), method.c_str());
		return GetAnyProxy(rt);
	}

private:
	std::string module_;
};

// ps5-vsh: SystemInfo TurboModule with real values (testkit dump: isDevKit
// true, 13.60). NPXS40003 reads getConstants().isDevKit.value and
// GetSystemSoftwareVersionSync().sysVer to gate setup — proxies stall it.
class SystemInfoStub : public facebook::jsi::HostObject {
public:
	facebook::jsi::Value get(facebook::jsi::Runtime& rt,
	                         const facebook::jsi::PropNameID& name) override {
		const std::string method = name.utf8(rt);
		if (method == "getConstants") {
			auto fn = facebook::jsi::Function::createFromHostFunction(
			    rt, facebook::jsi::PropNameID::forAscii(rt, "getConstants"), 0,
			    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
			       const facebook::jsi::Value*, size_t)
			        -> facebook::jsi::Value {
				    LOGF("\t [RNPS host] SystemInfo.getConstants() -> {isDevKit:true}\n");
				    facebook::jsi::Object out(rt2);
				    facebook::jsi::Object dev(rt2);
				    dev.setProperty(rt2, "value", true);
				    out.setProperty(rt2, "isDevKit", std::move(dev));
				    return out;
			    });
			return fn;
		}
		if (method == "GetSystemSoftwareVersionSync") {
			auto fn = facebook::jsi::Function::createFromHostFunction(
			    rt,
			    facebook::jsi::PropNameID::forAscii(
			        rt, "GetSystemSoftwareVersionSync"),
			    0,
			    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
			       const facebook::jsi::Value*, size_t)
			        -> facebook::jsi::Value {
				    LOGF("\t [RNPS host] SystemInfo.GetSystemSoftwareVersionSync() -> {sysVer:13.60}\n");
				    facebook::jsi::Object out(rt2);
				    out.setProperty(
				        rt2, "sysVer",
				        facebook::jsi::String::createFromAscii(rt2, "13.60"));
				    return out;
			    });
			return fn;
		}
		LOGF("\t [RNPS host] SystemInfo.%s -> any-proxy\n", method.c_str());
		return GetAnyProxy(rt);
	}
};

// ps5-vsh: legacy NativeModules bridge. Bundles do
// NativeModules.BlobModule (etc.); every module resolves to a
// TurboModuleStub so execution advances past native requires.
class NativeModulesStub : public facebook::jsi::HostObject {
public:
	facebook::jsi::Value get(facebook::jsi::Runtime& rt,
	                         const facebook::jsi::PropNameID& name) override {
		const std::string module = name.utf8(rt);
		if (module == "screen" || module == "getScreen") {
			return facebook::jsi::Value::undefined();
		}
		if (module == "path") {
			return facebook::jsi::Value::null();
		}
		if (module == "UIManager") {
			LOGF("\t [RNPS host] NativeModules.UIManager -> real UIManager\n");
			return ::RnpsBridge::BuildUIManager(rt);
		}
		if (module == "UIManager") {
			LOGF("\t [RNPS host] NativeModules.UIManager -> real UIManager\n");
			return BuildUIManager(rt);
		}
		// ps5-vsh: VshNMUserModule carries the logged-in user identity.
		// Static RE (NPXS40002): require(1).NativeModules.VshNMUserModule
		// .launchedUser flows into Account.isSignedIn, which invariants on
		// typeof userId === 'number'. Mirrors user_list.dat uid 1000.
		if (module == "VshNMUserModule") {
			LOGF("\t [RNPS host] NativeModules.VshNMUserModule -> uid 1000\n");
			facebook::jsi::Object user(rt);
			user.setProperty(rt, "launchedUser", facebook::jsi::Value(1000.0));
			user.setProperty(rt, "loginUserId", facebook::jsi::Value(1000.0));
			user.setProperty(rt, "__kytyStub", true);
			return user;
		}
		if (module == "LinkingPS") {
			LOGF("\t [RNPS host] NativeModules.LinkingPS -> home nav\n");
			facebook::jsi::Object linking(rt);
			linking.setProperty(
			    rt, "initialURL",
			    facebook::jsi::String::createFromUtf8(
			        rt, "homeui:navigateToHome?intro=hidden"));
			linking.setProperty(rt, "__kytyStub", true);
			return linking;
		}
		// ps5-vsh: CSI entitlement module — NPXS40003 (control-center) gates
		// its post-init setup on `Get({keys}) -> {results:[{value}]}`.
		// Generic proxy results stall it (no length, no string values), so
		// serve a realistic resolved promise: results[0].value = "1".
		if (module == "VshNMPsnEntitlementCSIModule") {
			LOGF("\t [RNPS host] NativeModules.VshNMPsnEntitlementCSIModule -> CSI stub\n");
			class CsiStub : public facebook::jsi::HostObject {
			public:
				facebook::jsi::Value get(
				    facebook::jsi::Runtime& rt,
				    const facebook::jsi::PropNameID& name) override {
					const std::string method = name.utf8(rt);
					if (method == "screen" || method == "getScreen") {
						return facebook::jsi::Value::undefined();
					}
					if (method == "path") {
						return facebook::jsi::Value::null();
					}
					// ps5-vsh: rnps-home polls url.indexOf('intro=hidden')
					// (homeui:navigateToHome) every tick and never renders
					// without it. Cold boot skips intro -> show home.
					if (method == "initialURL") {
						LOGF("\t [RNPS host] CSI.initialURL -> home nav\n");
						return facebook::jsi::String::createFromUtf8(
						    rt, "homeui:navigateToHome?intro=hidden");
					}
					LOGF("\t [RNPS host] CSI.%s -> %s\n", method.c_str(),
					     (method == "Get" ||
					      method == "getEntitlementsByIds" ||
					      method == "getEntitlementsBySkuIds" ||
					      method == "getEntitlementsByTitleIds")
					         ? "entitlement promise"
					         : ((method == "params" &&
					            !RnpsLaunchParams::Empty())
					                ? "launch params"
					                : "any-proxy"));
					// ps5-vsh: startup params (--rnps-param k=v). Bundle reads
					// CSI.params.appconfig(.feature/.param) for launch routing.
					// Empty store preserves legacy proxy behavior exactly.
					if (method == "params" &&
					    !RnpsLaunchParams::Empty()) {
						class CsiParams : public facebook::jsi::HostObject {
						public:
							facebook::jsi::Value get(
							    facebook::jsi::Runtime& rt,
							    const facebook::jsi::PropNameID& name) override {
								const std::string prop = name.utf8(rt);
								LOGF("\t [RNPS host] CSI.params.%s\n", prop.c_str());
								if (prop == "appconfig") {
									class CsiAppConfig : public facebook::jsi::
									    HostObject {
									public:
										facebook::jsi::Value get(
										    facebook::jsi::Runtime& rt,
										    const facebook::jsi::PropNameID& name)
										    override {
											const std::string p = name.utf8(rt);
											std::string       v;
											if (RnpsLaunchParams::Get(p, &v) ||
											    RnpsLaunchParams::Get(
											        std::string("appconfig.") + p, &v)) {
												LOGF("\t [RNPS host] CSI.params.appconfig.%s=\"%s\"\n",
												     p.c_str(), v.c_str());
												// ps5-vsh: one-shot caller stack so the
												// launch-param consumer is attributable
												// in hbcdump (addresses -> fn IDs).
												static bool logged = false;
												if (!logged) {
													logged = true;
													try {
														facebook::jsi::Value st =
														    rt.evaluateJavaScript(
														        std::make_shared<
														            facebook::jsi::
														                StringBuffer>(
														            std::string(
														                "(new Error().stack)||''")),
														        "kyty-stack");
														try {
															LOGF("\t [RNPS host] appconfig consumer stack:\n%.1200s\n",
															     st.toString(rt)
															         .utf8(rt)
															         .c_str());
														} catch (...) {
														}
													} catch (...) {
													}
												}
												return facebook::jsi::String::
												    createFromAscii(rt, v);
											}
											LOGF("\t [RNPS host] CSI.params.appconfig.%s -> any-proxy\n",
											     p.c_str());
											return GetAnyProxy(rt);
										}
									};
									return facebook::jsi::Object::
									    createFromHostObject(
									        rt, std::make_shared<CsiAppConfig>());
								}
								std::string v;
								if (RnpsLaunchParams::Get(prop, &v)) {
									LOGF("\t [RNPS host] CSI.params.%s=\"%s\"\n",
									     prop.c_str(), v.c_str());
									return facebook::jsi::String::createFromAscii(
									    rt, v);
								}
								return GetAnyProxy(rt);
							}
						};
						return facebook::jsi::Object::createFromHostObject(
						    rt, std::make_shared<CsiParams>());
					}
					if (method == "Get" ||
					    method == "getEntitlementsByIds" ||
					    method == "getEntitlementsBySkuIds" ||
					    method == "getEntitlementsByTitleIds") {
						auto fn = facebook::jsi::Function::createFromHostFunction(
						    rt,
						    facebook::jsi::PropNameID::forAscii(rt, method.c_str()),
						    1,
						    [](facebook::jsi::Runtime& rt2,
						       const facebook::jsi::Value&,
						       const facebook::jsi::Value* args,
						       size_t count) -> facebook::jsi::Value {
							    LOGF("\t [RNPS host] CSI.Get() -> keyed thenable\n");
							    try {
								    // ps5-vsh: sync thenable (not a real Promise:
								    // our one-shot harness never drains the
								    // microtask queue, so real Promise.resolve
								    // callbacks never run). Request keys are echoed:
								    // one result item per requested key (cap 64),
								    // each logging its reads as CSI.rd.<path>.
								    std::vector<std::string> reqKeys;
								    if (count >= 1 && args[0].isObject()) {
									    try {
										    facebook::jsi::Object req =
										        args[0].asObject(rt2);
										    facebook::jsi::Value kv =
										        req.getProperty(rt2, "keys");
										    if (kv.isObject()) {
											    facebook::jsi::Object ko =
											        kv.asObject(rt2);
											    facebook::jsi::Value lenV;
											    try {
												    lenV = ko.getProperty(rt2, "length");
											    } catch (...) {
											    }
											    uint32_t n = 0;
											    if (lenV.isNumber()) {
												    const double d = lenV.asNumber();
												    if (d > 0 && d < 64) {
													    n = static_cast<uint32_t>(d);
												    }
											    }
											    if (n > 64) {
												    n = 64;
											    }
											    for (uint32_t i = 0; i < n; i++) {
												    try {
													    facebook::jsi::Value ev =
													        ko.getProperty(
													            rt2,
													            facebook::jsi::PropNameID::forAscii(
													                rt2,
													                std::to_string(i).c_str()));
													    reqKeys.push_back(
													        ev.toString(rt2).utf8(rt2));
												    } catch (...) {
													    reqKeys.emplace_back("");
												    }
											    }
										    }
									    } catch (...) {
									    }
								    }
								    if (reqKeys.empty()) {
									    reqKeys.emplace_back("");
								    }
								    std::string keyPreview;
								    for (size_t ki = 0;
								         ki < reqKeys.size() && ki < 4; ki++) {
									    if (ki > 0) {
										    keyPreview += ",";
									    }
									    keyPreview += reqKeys[ki].substr(0, 48);
								    }
								    LOGF("\t [RNPS host] CSI.Get(keys=%zu [%s])\n",
								         reqKeys.size(), keyPreview.c_str());
								    class CsiLogObj : public facebook::jsi::
								        HostObject {
								    public:
									    explicit CsiLogObj(std::string tag, std::string key)
									        : tag_(std::move(tag)), key_(std::move(key)) {}
									    facebook::jsi::Value get(
									        facebook::jsi::Runtime& rt,
									        const facebook::jsi::PropNameID& name)
									        override {
										    const std::string p = name.utf8(rt);
										    LOGF("\t [RNPS host] CSI.rd.%s.%s\n",
										         tag_.c_str(), p.c_str());
										    if (p == "key") {
											    return facebook::jsi::String::
											        createFromAscii(rt, key_);
										    }
										    if (p == "value" ||
										        p == "entitlementId" ||
										        p == "skuId" ||
										        p == "titleId" ||
										        p == "status" ||
										        p == "type") {
											    return facebook::jsi::String::
											        createFromAscii(rt, "1");
										    }
										    if (p == "length" || p == "size" ||
										        p == "count") {
											    return facebook::jsi::Value(1.0);
										    }
										    return GetAnyProxy(rt);
									    }
								    private:
									    std::string tag_;
									    std::string key_;
								    };
								    facebook::jsi::Array results(
								        rt2, reqKeys.size() == 0 ? 1 : reqKeys.size());
								    for (size_t i = 0; i < reqKeys.size(); i++) {
									    facebook::jsi::Object item =
									        facebook::jsi::Object::
									            createFromHostObject(
									                rt2, std::make_shared<CsiLogObj>(
									                         "k" + std::to_string(i),
									                         reqKeys[i]));
									    results.setValueAtIndex(rt2, i, item);
								    }
								    auto resultsShared = std::make_shared<
								        facebook::jsi::Array>(
								        std::move(results));
								    class CsiPayload : public facebook::jsi::
								        HostObject {
								    public:
									    explicit CsiPayload(
									        std::shared_ptr<facebook::jsi::Array>
									            results)
									        : results_(std::move(results)) {}
									    facebook::jsi::Value get(
									        facebook::jsi::Runtime& rt,
									        const facebook::jsi::PropNameID& name)
									        override {
										    const std::string p = name.utf8(rt);
										    LOGF("\t [RNPS host] CSI.rd.payload.%s\n",
										         p.c_str());
										    if (p == "results") {
											    return facebook::jsi::Value(
											        rt, *results_);
										    }
										    return GetAnyProxy(rt);
									    }
								    private:
									    std::shared_ptr<facebook::jsi::Array>
									        results_;
								    };
								    facebook::jsi::Object payloadObj =
								        facebook::jsi::Object::
								            createFromHostObject(
								                rt2, std::make_shared<CsiPayload>(
								                         resultsShared));
								    auto payloadShared = std::make_shared<
								        facebook::jsi::Object>(
								        std::move(payloadObj));
								    facebook::jsi::Object thenable(rt2);
								    auto thenFn = facebook::jsi::Function::
								        createFromHostFunction(
								            rt2,
								            facebook::jsi::PropNameID::forAscii(
								                rt2, "then"),
								            2,
								            [payloadShared](
								                facebook::jsi::Runtime& rt3,
								                const facebook::jsi::Value&,
								                const facebook::jsi::Value* targs,
								                size_t tcount)
								                -> facebook::jsi::Value {
									            LOGF("\t [RNPS host] CSI.thenable.then() -> fire onF(payload)\n");
									            try {
										            facebook::jsi::Value stk =
										                rt3.global()
										                    .getPropertyAsObject(
										                        rt3, "Error");
										            (void)stk;
										            facebook::jsi::Value stackVal =
										                rt3.evaluateJavaScript(
										                    std::make_shared<
										                        facebook::jsi::StringBuffer>(
										                        std::string(
										                            "(new Error().stack)||''")),
										                    "kyty-stack");
										            try {
											            LOGF("\t [RNPS host] CSI onF JS stack:\n%.1500s\n",
											                 stackVal.toString(rt3)
											                     .utf8(rt3)
											                     .c_str());
										            } catch (...) {
										            }
									            } catch (...) {
									            }
									            if (tcount >= 1 &&
									                targs[0].isObject() &&
									                targs[0]
									                    .asObject(rt3)
									                    .isFunction(rt3)) {
										            facebook::jsi::Value pv(
										                rt3, *payloadShared);
										            try {
											            targs[0]
											                .asObject(rt3)
											                .asFunction(rt3)
											                .call(
											                    rt3,
											                    static_cast<
											                        const facebook::jsi::
											                            Value*>(
											                        &pv),
											                    static_cast<size_t>(
											                        1));
										            } catch (
										                const facebook::jsi::
										                    JSError& e) {
											            LOGF("\t [RNPS host] CSI onF threw: %s (swallowed)\n",
											                 e.getMessage()
											                     .c_str());
										            }
									            }
									            try {
										            facebook::jsi::Value any =
										                rt3.global().getProperty(
										                    rt3, "__kytyAny");
										            if (any.isObject()) {
											            return any;
										            }
									            } catch (...) {
									            }
									            return facebook::jsi::Value::
									                undefined();
								            });
								    thenable.setProperty(rt2, "then",
								                         std::move(thenFn));
								    auto catchFn = facebook::jsi::Function::
								        createFromHostFunction(
								            rt2,
								            facebook::jsi::PropNameID::forAscii(
								                rt2, "catch"),
								            1,
								            [](facebook::jsi::Runtime& rt3,
								               const facebook::jsi::Value&,
								               const facebook::jsi::Value*,
								               size_t)
								                -> facebook::jsi::Value {
									            LOGF("\t [RNPS host] CSI.thenable.catch() -> swallow\n");
									            try {
										            facebook::jsi::Value any =
										                rt3.global().getProperty(
										                    rt3, "__kytyAny");
										            if (any.isObject()) {
											            return any;
										            }
									            } catch (...) {
									            }
									            return facebook::jsi::Value::
									                undefined();
								            });
								    thenable.setProperty(rt2, "catch",
								                         std::move(catchFn));
								    return thenable;
							    } catch (...) {
								    return GetAnyProxy(rt2);
							    }
						    });
						return fn;
					}
					// ps5-vsh: npEnv (13.60 rnps-home reads CSI.npEnv.value
					// during setup). Serve the devkit env tag with a proxy
					// fallback for any other shape, so .value and method
					// reads both keep working.
					if (method == "npEnv") {
						LOGF("\t [RNPS host] CSI.npEnv -> {value:\"e1-np\"}\n");
						class NpEnvObj : public facebook::jsi::HostObject {
						public:
							facebook::jsi::Value get(
							    facebook::jsi::Runtime& rt,
							    const facebook::jsi::PropNameID& name) override {
								if (name.utf8(rt) == "value") {
									return facebook::jsi::String::createFromAscii(
									    rt, "e1-np");
								}
								return GetAnyProxy(rt);
							}
						};
						return facebook::jsi::Object::createFromHostObject(
						    rt, std::make_shared<NpEnvObj>());
					}
					return GetAnyProxy(rt);
				}
			};
			return facebook::jsi::Object::createFromHostObject(
			    rt, std::make_shared<CsiStub>());
		}
	}
};

// ps5-vsh: shared RN namespace builder. Returned by base_dll(...) calls
// AND installed as base_dll's own properties, so every require path
// (call result, member read) yields StyleSheet/AppRegistry/Platform.
// Anything ELSE read off it falls back to a TurboModuleStub, so unknown
// members (e.g. future .BlobModule-style reads) never throw.
class RnNamespaceStub : public facebook::jsi::HostObject {
public:
	void add(facebook::jsi::Runtime& rt, const char* name, facebook::jsi::Object obj) {
		known_[name] = std::make_shared<facebook::jsi::Object>(std::move(obj));
		(void)rt;
	}

	facebook::jsi::Value get(facebook::jsi::Runtime& rt,
	                         const facebook::jsi::PropNameID& name) override {
		const std::string prop = name.utf8(rt);
		const auto        it   = known_.find(prop);
		if (it != known_.end() && it->second != nullptr) {
			return facebook::jsi::Value(rt, *it->second);
		}
		if (prop == "screen" || prop == "getScreen") {
			return facebook::jsi::Value::undefined();
		}
		if (prop == "path") {
			return facebook::jsi::Value::null();
		}
		if (prop == "VshNMUserModule") {
			LOGF("\t [RNPS host] RNns.VshNMUserModule -> uid 1000\n");
			facebook::jsi::Object user(rt);
			user.setProperty(rt, "launchedUser", facebook::jsi::Value(1000.0));
			user.setProperty(rt, "loginUserId", facebook::jsi::Value(1000.0));
			user.setProperty(rt, "__kytyStub", true);
			return user;
		}
		// ps5-vsh: LinkingPS boot routing (static RE, NPXS40002 4.03 +
		// 13.60 HBC): getLocationScene() reads LinkingPS.initialURL,
		// parses .path, and selects the boot flow (languageChange /
		// switchUser / autoLogin / initialLoad). Cold boot skips intro.
		if (prop == "LinkingPS") {
			LOGF("\t [RNPS host] RNns.LinkingPS -> home nav\n");
			facebook::jsi::Object linking(rt);
			linking.setProperty(
			    rt, "initialURL",
			    facebook::jsi::String::createFromUtf8(
			        rt, "homeui:navigateToHome?intro=hidden"));
			linking.setProperty(rt, "__kytyStub", true);
			return linking;
		}
		// ps5-vsh: ES-module interop shape — __esModule must be a truthy
		// scalar and default must stay Proxy-shaped (callable +
		// constructible + gettable). A HostFunction method-stub here breaks
		// Object.create(default) at bundle top-level.
		if (prop == "__esModule") {
			return facebook::jsi::Value(true);
		}
		if (prop == "default") {
			return GetAnyProxy(rt);
		}
		// ps5-vsh: route legacy NativeModules reads through the real stub
		// (CSI entitlement data, UIManager, VshNMUserModule) instead of a
		// generic proxy — bundles reach it as RNns.NativeModules.X.
		if (prop == "NativeModules") {
			LOGF("\t [RNPS host] RNns.NativeModules -> NativeModulesStub\n");
			return facebook::jsi::Object::createFromHostObject(
			    rt, std::make_shared<NativeModulesStub>());
		}
		// ps5-vsh: leaf verb — bundle calls RNns.enable(cfg) directly and
		// never reads sub-props off it, so a named HostFunction is safe
		// (namespaces like TurboModuleRegistry must stay Proxy-shaped).
		if (prop == "enable") {
			LOGF("\t [RNPS host] RNns.enable -> method-stub\n");
			return MakeMethodStub(rt, "RNns.enable");
		}
		// ps5-vsh: TurboModuleRegistry with real SystemInfo (isDevKit +
		// sysVer gate NPXS40003 setup). get/getEnforcing return the
		// SystemInfo stub for SystemInfoTurboModule, generic stubs else.
		if (prop == "TurboModuleRegistry") {
			LOGF("\t [RNPS host] RNns.TurboModuleRegistry -> TmRegistryStub\n");
			class TmRegistryStub : public facebook::jsi::HostObject {
			public:
				facebook::jsi::Value get(
				    facebook::jsi::Runtime& rt,
				    const facebook::jsi::PropNameID& name) override {
					const std::string method = name.utf8(rt);
					if (method != "get" && method != "getEnforcing") {
						return GetAnyProxy(rt);
					}
					auto fn = facebook::jsi::Function::createFromHostFunction(
					    rt,
					    facebook::jsi::PropNameID::forAscii(rt, method.c_str()),
					    1,
					    [](facebook::jsi::Runtime& rt2,
					       const facebook::jsi::Value&,
					       const facebook::jsi::Value* args,
					       size_t count) -> facebook::jsi::Value {
						    std::string mod = count >= 1
						        ? args[0].toString(rt2).utf8(rt2)
						        : "<none>";
						    if (mod == "SystemInfoTurboModule") {
							    LOGF("\t [RNPS host] TmRegistry.%s(\"%s\") -> SystemInfo stub\n",
							         "get", mod.c_str());
							    return facebook::jsi::Object::
							        createFromHostObject(
							            rt2, std::make_shared<SystemInfoStub>());
						    }
						    // ps5-vsh: clock modules — NPXS40003 reads
						    // keyCurrentSecureTime/NetworkTime/LocalTime for
						    // time-gated setup; proxies yield NaN and stall
						    // comparisons, so serve host epoch millis.
						    if (mod == "SystemSrtcTurboModule" ||
						        mod == "SystemRtcTurboModule") {
							    LOGF("\t [RNPS host] TmRegistry.%s(\"%s\") -> clock stub\n",
							         "get", mod.c_str());
							    const double nowMs = static_cast<double>(
							        std::chrono::duration_cast<
							            std::chrono::milliseconds>(
							            std::chrono::system_clock::now()
							                .time_since_epoch())
							            .count());
							    facebook::jsi::Object clock(rt2);
							    auto constantsFn = facebook::jsi::Function::
							        createFromHostFunction(
							            rt2,
							            facebook::jsi::PropNameID::forAscii(
							                rt2, "getConstants"),
							            0,
							            [nowMs](
							                facebook::jsi::Runtime& rt3,
							                const facebook::jsi::Value&,
							                const facebook::jsi::Value*,
							                size_t) -> facebook::jsi::Value {
								            LOGF("\t [RNPS host] clock.getConstants() -> now=%f\n",
								                 nowMs);
								            facebook::jsi::Object out(rt3);
								            out.setProperty(
								                rt3, "keyCurrentSecureTime", nowMs);
								            out.setProperty(
								                rt3, "keyCurrentNetworkTime", nowMs);
								            out.setProperty(
								                rt3, "keyCurrentLocalTime", nowMs);
								            return out;
							            });
							    clock.setProperty(rt2, "getConstants",
							                      std::move(constantsFn));
							    return clock;
						    }
						    LOGF("\t [RNPS host] TmRegistry.%s(\"%s\") -> stub\n",
						         "get", mod.c_str());
						    (void)args;
						    return facebook::jsi::Object::
						        createFromHostObject(
						            rt2, std::make_shared<TurboModuleStub>(mod));
					    });
					return fn;
				}
			};
			return facebook::jsi::Object::createFromHostObject(
			    rt, std::make_shared<TmRegistryStub>());
		}
		LOGF("\t [RNPS host] RNns.%s -> any-proxy\n", prop.c_str());
		return GetAnyProxy(rt);
	}

private:
	std::unordered_map<std::string, std::shared_ptr<facebook::jsi::Object>> known_;
};

// ps5-vsh: headless mount driver state. jsi::Function values cannot persist
// across calls without GC roots, so we track component NAMEs + a mounted set
// plus an incrementing fake root tag. Runtimes are per-Execute and each app
// runs once, so process-static state cleared at prelude is acceptable. No
// actual render happens here (renderer arrives next round); runApplication
// only logs STARTUP, records the mount, and returns the root tag.
struct AppRegistryState {
	std::mutex                 mutex;
	std::vector<std::string>   components; // registerComponent names in order
	std::vector<std::string>   mounted;    // runApplication names in order
	int                        nextRootTag = 1;
};

inline AppRegistryState& GetAppRegistryState() {
	static AppRegistryState s;
	return s;
}

inline void ResetAppRegistryForRun() {
	AppRegistryState& s = GetAppRegistryState();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.components.clear();
	s.mounted.clear();
	s.nextRootTag = 1;
}

// ps5-vsh: TTPoolInstance app pool (observed HW behavior: App Pool capacity
// 4, `instance promoted from TTPoolInstance [guid]`, then `MountApp
// runApplication`). Instances are promoted per title at boot; MountApp marks
// the title mounted and hands out a root tag. No renderer yet: mount is
// recorded + logged, same contract as AppRegistry.runApplication.
struct TTPoolInstance {
	std::string guid;
	std::string titleId;
	std::string appName;
	std::string appVersion;
	bool        running = false;
};

struct TTPoolState {
	std::mutex                 mutex;
	std::vector<TTPoolInstance> instances; // capacity 4, FIFO evict
	uint64_t                   guidSeq = 1;
};

inline TTPoolState& GetTTPoolState() {
	static TTPoolState s;
	return s;
}

inline void ResetTTPoolForRun() {
	TTPoolState& s = GetTTPoolState();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.instances.clear();
}

inline std::string TTPoolPromote(const std::string& titleId, const std::string& appName,
                                 const std::string& appVersion) {
	TTPoolState& s = GetTTPoolState();
	std::lock_guard<std::mutex> lock(s.mutex);
	for (auto& in : s.instances) {
		if (in.titleId == titleId) {
			in.running = true;
			LOGF("\t [RNPS host] TTPoolInstance [%s] already promoted for %s\n",
			     in.guid.c_str(), titleId.c_str());
			return in.guid;
		}
	}
	// guid in 8-4-4-4-12 shape from FNV over title+salt (local FNV-1a64:
	// Detail::Fnv1a64 is declared later in this TU, so hash inline here).
	uint64_t h = 14695981039346656037ull;
	for (const char c : titleId) {
		h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
		h *= 1099511628211ull;
	}
	h ^= (s.guidSeq++ * 0x9E3779B97F4A7C15ull);
	char guid[40];
	std::snprintf(guid, sizeof(guid), "%08x-%04x-4%03x-%04x-%012llx",
	              static_cast<unsigned>(h & 0xffffffffu),
	              static_cast<unsigned>((h >> 32) & 0xffffu),
	              static_cast<unsigned>((h >> 48) & 0xfffu),
	              static_cast<unsigned>((h >> 20) & 0xffffu),
	              static_cast<unsigned long long>(h * 0x100000001B3ull) &
	                  0xffffffffffffull);
	while (s.instances.size() >= 4) {
		s.instances.erase(s.instances.begin());
	}
	TTPoolInstance in;
	in.guid       = guid;
	in.titleId    = titleId;
	in.appName    = appName;
	in.appVersion = appVersion;
	in.running    = true;
	s.instances.push_back(in);
	LOGF("\t [RNPS host] TTPoolInstance [%s].AppManifest : applicationName : %s\n",
	     guid, appName.c_str());
	LOGF("\t [RNPS host] TTPoolInstance [%s].AppManifest : applicationVersion : %s\n",
	     guid, appVersion.c_str());
	LOGF("\t [RNPS host] TTPoolInstance [%s].AppManifest : titleId : %s\n", guid,
	     titleId.c_str());
	LOGF("\t [RNPS host] RNPS.%s.%s : instance promoted from TTPoolInstance [%s]\n",
	     appName.c_str(), titleId.c_str(), guid);
	return in.guid;
}

inline int TTPoolMountApp(const std::string& titleId, const std::string& appName) {
	AppRegistryState& s = GetAppRegistryState();
	std::lock_guard<std::mutex> lock(s.mutex);
	const int rootTag = s.nextRootTag++;
	s.mounted.push_back(titleId.empty() ? appName : titleId);
	LOGF("\t [RNPS host] RNPS.%s.%s : MountApp runApplication rootTag=%d\n",
	     appName.c_str(), titleId.c_str(), rootTag);
	return rootTag;
}

// ps5-vsh: SceneQ + focus chain (observed HW order: SceneQ Loaded:
// Render.<title> : AppScene, then OnFocusActiveSceneChanged [prev] ->
// [new]). MountApp loads the title scene; focus moves to it from whatever
// was focused (BootScene at cold boot). Host-side state only: gives the
// future renderer a scene graph to target and makes scene flow log-visible.
struct SceneQState {
	std::mutex              mutex;
	std::vector<std::string> loaded; // Render.<title> scene names in order
	std::string             focused = "BootScene : Scene";
};

inline SceneQState& GetSceneQState() {
	static SceneQState s;
	return s;
}

inline void SceneLoadOnMount(const std::string& titleId) {
	SceneQState& s = GetSceneQState();
	std::lock_guard<std::mutex> lock(s.mutex);
	const std::string scene = "Render." + titleId + " : AppScene";
	s.loaded.push_back(scene);
	LOGF("\t [RNPS host] SceneQ Loaded: %s\n", scene.c_str());
	const std::string prev = s.focused;
	s.focused              = scene;
	LOGF("\t [RNPS host] OnFocusActiveSceneChanged [%s] -> [%s]\n", prev.c_str(),
	     scene.c_str());
	// ps5-vsh: every mounted AppScene gets a compositor canvas (same table
	// the guest canvas path uses); the renderer target for future frames.
	const uint64_t canvas =
	    Libs::Composite::CompositeHost::CanvasForScene(scene);
	LOGF("\t [RNPS host] scene %s -> canvas %" PRIu64 "\n", scene.c_str(), canvas);
}

inline void ResetSceneQForRun() {
	SceneQState& s = GetSceneQState();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.loaded.clear();
	s.focused = "BootScene : Scene";
}

inline facebook::jsi::Object BuildRnNamespace(facebook::jsi::Runtime& rt) {
	facebook::jsi::Object rn(rt);
	{
		facebook::jsi::Object styleSheet(rt);
		auto createFn = facebook::jsi::Function::createFromHostFunction(
		    rt, facebook::jsi::PropNameID::forAscii(rt, "create"), 1,
		    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		       const facebook::jsi::Value* args,
		       size_t count) -> facebook::jsi::Value {
			    LOGF("\t [RNPS host] StyleSheet.create() -> stub\n");
			    facebook::jsi::Object out(rt2);
			    out.setProperty(rt2, "__kytyStyles", true);
			    if (count >= 1 && args[0].isObject()) {
				    return facebook::jsi::Value(
				        rt2, facebook::jsi::Object(rt2));
			    }
			    return facebook::jsi::Value(rt2, std::move(out));
		    });
		styleSheet.setProperty(rt, "create", std::move(createFn));
		rn.setProperty(rt, "StyleSheet", std::move(styleSheet));
	}
	{
		facebook::jsi::Object appReg(rt);
		auto regFn = facebook::jsi::Function::createFromHostFunction(
		    rt, facebook::jsi::PropNameID::forAscii(rt, "registerComponent"), 2,
		    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		       const facebook::jsi::Value* args,
		       size_t count) -> facebook::jsi::Value {
			    std::string name = "<none>";
			    try {
				    if (count >= 1 && !args[0].isUndefined() && !args[0].isNull()) {
					    name = args[0].toString(rt2).utf8(rt2);
				    }
			    } catch (...) {
				    name = "<unprintable>";
			    }
			    if (!name.empty() && name != "<none>" && name != "<unprintable>") {
				    AppRegistryState& s = GetAppRegistryState();
				    std::lock_guard<std::mutex> lock(s.mutex);
				    bool dup = false;
				    for (const auto& c : s.components) {
					    if (c == name) {
						    dup = true;
						    break;
					    }
				    }
				    if (!dup) {
					    s.components.push_back(name);
				    }
			    }
			    LOGF("\t [RNPS host] AppRegistry.registerComponent(\"%s\")\n",
			         name.c_str());
			    return facebook::jsi::Value::undefined();
		    });
					appReg.setProperty(rt, "registerComponent", std::move(regFn));
					auto runFn = facebook::jsi::Function::createFromHostFunction(
					    rt,
					    facebook::jsi::PropNameID::forAscii(rt, "runApplication"),
					    2,
					    [](facebook::jsi::Runtime& rt2,
					       const facebook::jsi::Value&,
					       const facebook::jsi::Value* args,
					       size_t count) -> facebook::jsi::Value {
						    std::string name = "<none>";
						    try {
							    if (count >= 1 && !args[0].isUndefined() &&
							        !args[0].isNull()) {
								    name = args[0].toString(rt2).utf8(rt2);
							    }
						    } catch (...) {
							    name = "<unprintable>";
						    }
						    // Null-safe params key dump (names only, capped).
						    std::string keys;
						    size_t        nkeys = 0;
						    try {
							    if (count >= 2 && args[1].isObject()) {
								    facebook::jsi::Array names =
								        args[1].asObject(rt2).getPropertyNames(rt2);
								    nkeys = names.size(rt2);
								    for (size_t i = 0; i < nkeys && i < 8; i++) {
									    try {
										    std::string k =
										        names.getValueAtIndex(rt2, i)
										            .toString(rt2)
										            .utf8(rt2);
										    if (i > 0) {
											    keys += ",";
										    }
										    if (k.size() > 48) {
											    k.resize(48);
										    }
										    keys += k;
									    } catch (...) {
										    keys += "<unprintable>";
										    break;
									    }
								    }
							    }
						    } catch (...) {
							    keys.clear();
							    nkeys = 0;
						    }
						    int rootTag = 0;
						    {
							    AppRegistryState& s = GetAppRegistryState();
							    std::lock_guard<std::mutex> lock(s.mutex);
							    rootTag          = s.nextRootTag++;
							    s.mounted.push_back(name);
						    }
						    LOGF("\t [RNPS host] AppRegistry.runApplication(\"%s\") "
						         "paramsKeys=[%s](%zu) rootTag=%d -- STARTUP REACHED\n",
						         name.c_str(), keys.c_str(), nkeys, rootTag);
						    return facebook::jsi::Value(static_cast<double>(rootTag));
					    });
					appReg.setProperty(rt, "runApplication", std::move(runFn));
					auto keysFn = facebook::jsi::Function::createFromHostFunction(
					    rt, facebook::jsi::PropNameID::forAscii(rt, "getAppKeys"), 0,
					    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
					       const facebook::jsi::Value*, size_t)
					        -> facebook::jsi::Value {
						    // Honest pre-mount: no renderer yet, so empty list.
						    LOGF("\t [RNPS host] AppRegistry.getAppKeys() -> []\n");
						    (void)rt2;
						    return facebook::jsi::Array(rt2, 0);
					    });
					appReg.setProperty(rt, "getAppKeys", std::move(keysFn));
					rn.setProperty(rt, "AppRegistry", std::move(appReg));
	}
	{
		facebook::jsi::Object platform(rt);
		platform.setProperty(rt, "OS",
		                     facebook::jsi::String::createFromAscii(rt, "ps5"));
		platform.setProperty(rt, "Version", 13);
		auto selectFn = facebook::jsi::Function::createFromHostFunction(
		    rt, facebook::jsi::PropNameID::forAscii(rt, "select"), 1,
		    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		       const facebook::jsi::Value* args,
		       size_t count) -> facebook::jsi::Value {
			    // ps5-vsh: RN Platform.select(specs): ps5/native/default
			    // first defined wins (RN picks per-OS branch).
			    static constexpr const char* kKeys[] = {
			        "ps5", "native", "default", "ios", "android"};
			    if (count >= 1 && args[0].isObject()) {
				    facebook::jsi::Object specs = args[0].asObject(rt2);
				    for (const char* key : kKeys) {
					    facebook::jsi::Value v = specs.getProperty(rt2, key);
					    if (!v.isUndefined()) {
						    LOGF("\t [RNPS host] Platform.select() -> \"%s\" branch\n",
						         key);
						    return v;
					    }
				    }
			    }
			    LOGF("\t [RNPS host] Platform.select() -> undefined (no branch)\n");
			    return facebook::jsi::Value::undefined();
		    });
		platform.setProperty(rt, "select", std::move(selectFn));
		rn.setProperty(rt, "Platform", std::move(platform));
	}
	// ps5-vsh: startup-path RN surface (all log-and-succeed stubs; real
	// semantics per module as errors demand). Dimensions/PixelRatio feed
	// layout; AppRegistry.runApplication marks startup reached.
	{
		auto dimsGet = facebook::jsi::Function::createFromHostFunction(
		    rt, facebook::jsi::PropNameID::forAscii(rt, "get"), 1,
		    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		       const facebook::jsi::Value* args,
		       size_t count) -> facebook::jsi::Value {
			    std::string which = count >= 1
			        ? args[0].toString(rt2).utf8(rt2)
			        : "window";
			    LOGF("\t [RNPS host] Dimensions.get(\"%s\") -> 1920x1080\n",
			         which.c_str());
			    facebook::jsi::Object metrics(rt2);
			    metrics.setProperty(rt2, "width", 1920.0);
			    metrics.setProperty(rt2, "height", 1080.0);
			    metrics.setProperty(rt2, "scale", 1.0);
			    metrics.setProperty(rt2, "fontScale", 1.0);
			    return metrics;
		    });
		facebook::jsi::Object dims(rt);
		dims.setProperty(rt, "get", std::move(dimsGet));
		rn.setProperty(rt, "Dimensions", std::move(dims));
	}
	{
		facebook::jsi::Object pr(rt);
		auto getFn = facebook::jsi::Function::createFromHostFunction(
		    rt, facebook::jsi::PropNameID::forAscii(rt, "get"), 0,
		    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		       const facebook::jsi::Value*, size_t) -> facebook::jsi::Value {
			    LOGF("\t [RNPS host] PixelRatio.get() -> 1\n");
			    return facebook::jsi::Value(1.0);
		    });
		pr.setProperty(rt, "get", std::move(getFn));
		rn.setProperty(rt, "PixelRatio", std::move(pr));
	}
	rn.setProperty(rt, "__kytyRnStub", true);
	// ps5-vsh: wrap with fallback — unknown members resolve to
	// TurboModule stubs instead of throwing, while the five real
	// namespaces above keep exact behavior.
	auto holder = std::make_shared<RnNamespaceStub>();
	for (const char* key :
	     {"StyleSheet", "AppRegistry", "Platform", "Dimensions", "PixelRatio"}) {
		facebook::jsi::Value v = rn.getProperty(rt, key);
		if (v.isObject()) {
			holder->add(rt, key, std::move(v).asObject(rt));
		}
	}
	return facebook::jsi::Object::createFromHostObject(rt, std::move(holder));
}

} // namespace RnpsBridge
#endif

namespace Libs {

// ps5-vsh: Hermes VM foundation (host-executed, REAL-service style: like
// avPlayer runs FFmpeg on the host, the Hermes bytecode runs on the host once
// the real VM plugs in here).
//
// No Hermes source is vendored in 3rdparty/ (checked: no hermes/jsi/js
// entries), so this turn does NOT FetchContent anything. This file is the
// host-side execution scaffold the VM will plug into:
//   - VmContext owns the decrypted bytecode + JSI bridge state + GC stat
//     counters, keyed by a host handle returned to the guest.
//   - Create validates the RNPSHEDR-stripped Hermes magic and fails closed on
//     encrypted/unstripped input; Execute is an honest NO-VM-YET placeholder
//     (0x80940002), never a fake success.
// Guest buffers are identity-mapped on the host (same assumption as the
// LibcInternal realloc forward), so guest pointers are read directly after a
// null check. Never crashes on null guest pointers: every entry point null-
// checks before dereferencing.

namespace HermesVm {

LIB_VERSION("HermesVm", 1, "HermesVm", 1, 1);

// Fail-closed errors. 0x80940001 matches the libRnpsBundle VerifyAndDecrypt
// fail-closed placeholder (encrypted input must not pretend success).
// 0x80940002 is the honest NO-VM-YET placeholder for Execute/JsiEvaluate.
constexpr int HERMES_VM_ERROR_ENCRYPTED = static_cast<int>(0x80940001u);
constexpr int HERMES_VM_ERROR_NO_VM     = static_cast<int>(0x80940002u);
// JS exception escaping HermesVmExecute with the VM wired in (KYTY_USE_HERMES).
constexpr int HERMES_VM_ERROR_EXEC_FAILED = static_cast<int>(0x80940003u);

// Hermes HBC magic (RNPSHEDR-stripped bytecode must start with this), also
// used by Loader::HasHermesBytecode for bundle scanning. Upstream defines it
// as the u64 word 0x1F1903C103BC1FC6 (BytecodeFileFormat.h), stored
// little-endian, so on-disk bytes read C6-1F-BC-03-C1-03-19-1F. Proven with
// scratch hermesc v0.11.0 output 2026-10-05 (a big-endian order here rejected
// genuine HBC).
constexpr uint8_t kHermesMagic[8] = {0xC6, 0x1F, 0xBC, 0x03, 0xC1, 0x03, 0x19, 0x1F};
// RNPS bundle magic: seeing this means the caller passed an unstripped
// (still encrypted / header-prefixed) bundle instead of decrypted bytecode.
constexpr char kRnpsMagic[] = "RNPSHEDR";

// Guest-visible stats, mirroring the hermes_{heapSize,allocatedBytes,...}
// stat names found in libhermes-0.11.sprx strings.
struct HermesVmStats {
	uint64_t heapSize;       // mirrors hermes_heapSize
	uint64_t allocatedBytes; // mirrors hermes_allocatedBytes
	uint64_t gcCollections;  // GC run count (mirrors hermes_gc.* counters)
	uint64_t execCount;      // HermesVmExecute calls on this context
};

struct VmContext {
	std::vector<uint8_t> bytecode; // decrypted Hermes HBC owned by the host
	uint32_t             bytecodeVersion = 0; // HBC version word (offset 8)
	uint64_t             sourceHash      = 0; // FNV-1a64 over bytecode (runtime cache key)
	bool                 hasJsiRuntime   = false; // true for JsiCreateRuntime handles
	uint64_t             heapSize        = 0;
	uint64_t             allocatedBytes  = 0;
	uint64_t             gcCollections   = 0;
	uint64_t             execCount       = 0;
	// Evaluate cache: same bytecode hash + same entry/source-hash repeats the
	// cached result code without re-running validation/logging (real-runtime
	// style memoization; Execute itself stays an honest NO-VM-YET).
	std::string cachedEntry;
	int       cachedResult     = 0;
	bool      hasCachedResult  = false;
	uint64_t  cachedEvalHash   = 0; // FNV-1a64 of last JsiEvaluate source
	int       cachedEvalResult = 0;
	bool      hasCachedEval    = false;
	// ps5-vsh: resident service mode. When > 0, HermesVmExecute keeps the
	// runtime alive after bundle evaluation and pumps the live tick loop
	// for this many seconds instead of returning immediately (one-shot
	// evaluate -> quick_exit never lets the service observe show/login
	// events). Set only by HermesVmHost::RunHbcBufferLive.
	uint64_t liveSeconds = 0;
	// ps5-vsh: one-shot guard for the KYTY_RNPS_REAL_REGISTRY drive step.
	// runApplication must fire once, after the host bundle registered "Main";
	// re-driving on every Execute would stack root views (and previously the
	// step could also run before any registry existed).
	bool appDriven = false;
};

namespace Detail {

std::mutex                                             g_vmMutex;
std::unordered_map<uint64_t, VmContext>                g_vmContexts;
uint64_t                                               g_vmNextHandle = 1;

// ps5-vsh: app identity for the TTPool mount path (fed by
// HermesVmHost::SetRnpsAppInfo from the emulator RNPS block, which owns
// the manifest; one app per process run).
std::mutex  g_appInfoMutex;
std::string g_appTitleId;
std::string g_appName;
std::string g_appVersion;

uint64_t RegisterContext(VmContext&& ctx) {
	std::lock_guard<std::mutex> lock(g_vmMutex);
	const uint64_t              handle = g_vmNextHandle++;
	g_vmContexts.emplace(handle, std::move(ctx));
	return handle;
}

VmContext* FindContext(uint64_t handle) {
	std::lock_guard<std::mutex> lock(g_vmMutex);
	const auto                  it = g_vmContexts.find(handle);
	return it != g_vmContexts.end() ? &it->second : nullptr;
}

bool HasHermesMagic(const uint8_t* data, uint64_t size) {
	return data != nullptr && size >= sizeof(kHermesMagic) &&
	       std::memcmp(data, kHermesMagic, sizeof(kHermesMagic)) == 0;
}

bool HasRnpsMagic(const uint8_t* data, uint64_t size) {
	return data != nullptr && size >= sizeof(kRnpsMagic) - 1 &&
	       std::memcmp(data, kRnpsMagic, sizeof(kRnpsMagic) - 1) == 0;
}

uint32_t ReadBytecodeVersion(const uint8_t* data, uint64_t size) {
	// HBC header: 8-byte magic followed by a little-endian u32 version.
	if (data == nullptr || size < sizeof(kHermesMagic) + sizeof(uint32_t)) {
		return 0;
	}
	uint32_t version = 0;
	std::memcpy(&version, data + sizeof(kHermesMagic), sizeof(version));
	return version;
}

uint64_t Fnv1a64(const uint8_t* data, uint64_t size) {
	// FNV-1a 64-bit: runtime cache key over bytecode / JSI source.
	constexpr uint64_t kOffset = 14695981039346656037ull;
	constexpr uint64_t kPrime  = 1099511628211ull;
	uint64_t           hash   = kOffset;
	for (uint64_t i = 0; i < size; i++) {
		hash ^= static_cast<uint64_t>(data[i]);
		hash *= kPrime;
	}
	return hash;
}

uint64_t Fnv1a64Str(const char* data, uint64_t size) {
	return Fnv1a64(reinterpret_cast<const uint8_t*>(data), size);
}

// HBC pre-validation: version-word check + section/file-size sanity from the
// header. Everything is LOGF-dumped so hbcdump triage has the header words.
// Returns false only on fail-closed input (zero/short version, insane length).
bool ValidateHbcHeader(const uint8_t* data, uint64_t size) {
	if (data == nullptr || size < sizeof(kHermesMagic) + sizeof(uint32_t)) {
		LOGF("\t HBC header too short (size = %" PRIu64 "), failing validation\n", size);
		return false;
	}
	uint32_t version = 0;
	std::memcpy(&version, data + sizeof(kHermesMagic), sizeof(version));
	LOGF("\t HBC version word = %" PRIu32 " (0x%08" PRIx32 ", hermes-0.11 emits 84)\n",
	     version, version);
	if (version == 0 || version > 512) {
		LOGF("\t HBC version word out of plausible range, failing validation\n");
		return false;
	}
	// Hermes FileHeader layout: magic[8] + version[4] + sourceHash[20] +
	// fileLength[4] at offset 32. When present, fileLength must cover the
	// buffer and stay sane (<= 256 MiB) — otherwise the section table that
	// follows cannot be trusted.
	constexpr uint64_t kFileLengthOffset = 8 + 4 + 20;
	constexpr uint32_t kMaxHbcBytes      = 256u * 1024u * 1024u;
	if (size >= kFileLengthOffset + sizeof(uint32_t)) {
		uint32_t fileLength = 0;
		std::memcpy(&fileLength, data + kFileLengthOffset, sizeof(fileLength));
		LOGF("\t HBC fileLength word = %" PRIu32 " (buffer size = %" PRIu64 ")\n",
		     fileLength, size);
		if (fileLength < size || fileLength > kMaxHbcBytes) {
			LOGF("\t HBC fileLength insane (smaller than buffer or > 256MiB), "
			     "failing validation\n");
			return false;
		}
	} else {
		LOGF("\t HBC header short: no fileLength word (size = %" PRIu64
		     "), skipping section-size sanity\n",
		     size);
	}
	return true;
}

} // namespace Detail

#ifdef KYTY_USE_HERMES
// ps5-vsh: resident service pump. Re-fires JS-side __kytyListeners each
// second with a fresh event so the bundle observes service events past
// afterAppLoad; per-tick heartbeat proves residency. Bounded by seconds.
inline void LivePump(facebook::hermes::HermesRuntime& rt, uint64_t seconds) {
	// ps5-vsh: BGS loopback channel for the resident service. Connect once;
	// each tick polls the transport and forwards arrivals into the
	// RnpsAppMgr ShellUI event queue (the same queue guest HasEvent/
	// GetEventForShellUI drain), proving transport -> dispatch -> channel
	// end to end while the runtime stays alive.
	uint64_t bgs = 0;
	try {
		bgs = Libs::BgsRpc::BgsRpcHost::Connect("rnps://control-center");
	} catch (...) {
		LOGF("\t [RNPS host] BGS connect failed\n");
	}
	// ps5-vsh: one-time survey for bundle-exposed entry points (render /
	// show / start / mount style globals). Log-only: identifies the native
	// trigger surface without invoking unknown side effects.
	try {
		(void)rt.evaluateJavaScript(
		    std::make_shared<facebook::jsi::StringBuffer>(std::string(
		        "(function(){try{var ks=Object.keys(globalThis);var hit=[];"
		        "for(var i=0;i<ks.length;i++){var k=ks[i];"
		        "if(/render|show|start|mount|launch|boot|runApp|open|main|entry|control/i.test(k))"
		        "{var t='?';try{t=typeof globalThis[k];}catch(_){}hit.push(k+':'+t);}}"
		        "__kytyLog('RNPS live globals enterish=['+hit.slice(0,24).join(',')+'] total='+ks.length);"
		        "var fns=[]; for(var i=0;i<ks.length;i++){try{var kn=ks[i]; if(typeof globalThis[kn]==='function'){fns.push(kn);}}catch(_){}}"
		        "__kytyLog('RNPS live global-fns=['+fns.slice(0,48).join(',')+']');"
		        "var probes=['initializeIpcClient','waitConnectionAsync','showControlCenter','showQuickMenu','showHomeScreen','getInstance','runApplication','registerComponent','enable','getStartupParams'];"
		        "var found=[];"
		        "for(var i=0;i<ks.length;i++){try{var o=globalThis[ks[i]];"
		        "if(o&&(typeof o==='object'||typeof o==='function')&&ks[i]!=='__kytyAny'&&!o.__kytyStub&&!o.__kytyRnStub){for(var j=0;j<probes.length;j++){try{if(typeof o[probes[j]]==='function'){found.push(ks[i]+'.'+probes[j]);}}catch(_){}}}}catch(_){}}"
		        "__kytyLog('RNPS live host-callables=['+found.slice(0,24).join(',')+']');}"
		        "catch(e){try{__kytyLog('live survey failed '+e);}catch(_){}}})()")),
		    "kyty-survey");
	} catch (const facebook::jsi::JSError& e) {
		LOGF("\t [RNPS host] live survey failed: %s\n", e.getMessage().c_str());
	}
	for (uint64_t tick = 1; tick <= seconds; tick++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const std::string prog =
		    std::to_string(static_cast<unsigned long long>(tick)) + "/" +
		    std::to_string(static_cast<unsigned long long>(seconds));
		try {
			(void)rt.evaluateJavaScript(
			    std::make_shared<facebook::jsi::StringBuffer>(std::string(
			        "(function(){try{var L=globalThis.__kytyListeners||[];"
			        "try{__kytyLog('RNPS live tick " +
			        prog + " listeners='+L.length);}catch(_){}"
			        "for(var i=0;i<L.length;i++){try{var arg=globalThis.__kytyAny;"				"try{if(globalThis.__kytyTickN===undefined){globalThis.__kytyTickN=0;}}catch(_){}"
				"globalThis.__kytyTickN++; try{__kytyLog('RNPS live rich-fire tick='+globalThis.__kytyTickN);}catch(_){}"
				"if(globalThis.__kytyTickN===3){arg={type:'show',screen:'home',rootTag:1,userId:1000,loginUserId:1000,entitlements:[{entitlementId:'1',status:'active'}],presence:'online',titles:[{titleId:'NPXS40087',name:'Test'}]};}"
				"L[i].cb(arg);}"
			        "catch(e){try{__kytyLog('live listener threw '+e);}catch(_){}}}"
			        "}catch(e){try{__kytyLog('live tick failed '+e);}catch(_){}}})()")),
			    "kyty-tick");
		} catch (const facebook::jsi::JSError& e) {
			LOGF("\t [RNPS host] live tick %s failed: %s (stopping)\n",
			     prog.c_str(), e.getMessage().c_str());
			break;
		}
		LOGF("\t [RNPS host] live tick %s done\n", prog.c_str());
		// ps5-vsh: BGS dispatch leg. Transport arrivals feed the ShellUI
		// event channel; depth proves the native show/login path is live.
		if (bgs != 0) {
			try {
				const std::string msg = Libs::BgsRpc::BgsRpcHost::Poll(bgs);
				if (!msg.empty()) {
					Libs::RnpsAppMgr::RnpsAppMgrHost::EnqueueShellUIEvent(msg);
					LOGF("\t [RNPS host] BGS dispatch '%s' -> ShellUI (pending=%zu)\n",
					     msg.c_str(),
					     Libs::RnpsAppMgr::RnpsAppMgrHost::PendingShellUIEvents());
				}
			} catch (...) {
				LOGF("\t [RNPS host] BGS dispatch failed\n");
			}
			// ps5-vsh: consumer leg. A Shell-side drain would call
			// HasEvent/GetEventForShellUI; the resident pump consumes one
			// per tick so the pop path is exercised end to end.
			try {
				std::string consumed;
				if (Libs::RnpsAppMgr::RnpsAppMgrHost::TryConsumeShellUIEvent(
				        &consumed)) {
					LOGF("\t [RNPS host] ShellUI consumed '%s'\n", consumed.c_str());
				}
			} catch (...) {
				LOGF("\t [RNPS host] ShellUI consume failed\n");
			}
		}
	}
	if (bgs != 0) {
		try {
			Libs::BgsRpc::BgsRpcHost::Close(bgs);
		} catch (...) {
		}
	}
}
#endif

namespace Vm {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HermesVmCreate(const uint8_t* bytecode, uint64_t size,
                                        uint64_t* outHandle) {
	PRINT_NAME();
	LOGF("\t bytecode = %p size = %" PRIu64 " outHandle = %p\n",
	     static_cast<const void*>(bytecode), size,
	     static_cast<void*>(outHandle));
	if (bytecode == nullptr || size == 0 || outHandle == nullptr) {
		LOGF("\t null guest pointer, failing closed with 0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	if (Detail::HasRnpsMagic(bytecode, size)) {
		LOGF("\t RNPSHEDR prefix present: bundle not stripped/decrypted, failing closed "
		     "with 0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	if (!Detail::HasHermesMagic(bytecode, size)) {
		LOGF("\t bad Hermes magic (likely still encrypted), failing closed with "
		     "0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	VmContext ctx;
	ctx.bytecode.assign(bytecode, bytecode + size);
	ctx.bytecodeVersion = Detail::ReadBytecodeVersion(bytecode, size);
	ctx.sourceHash      = Detail::Fnv1a64(bytecode, size);
	if (!Detail::ValidateHbcHeader(bytecode, size)) {
		LOGF("\t HBC pre-validation failed, failing closed with 0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	ctx.heapSize        = size;
	ctx.allocatedBytes  = size;
	LOGF("\t Hermes bytecode: size = %" PRIu64 " version = %" PRIu32
	     " fnv1a64 = 0x%016" PRIx64 "\n",
	     size, ctx.bytecodeVersion, ctx.sourceHash);
	*outHandle = Detail::RegisterContext(std::move(ctx));
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HermesVmDestroy(uint64_t handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", handle);
	std::lock_guard<std::mutex> lock(Detail::g_vmMutex);
	if (Detail::g_vmContexts.erase(handle) == 0) {
		LOGF("\t unknown handle, failing closed with 0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	return 0;
}

// ps5-vsh: real-registry gate. Unreachable in the co-hosted configuration
// (HermesVmExecute is never called and the per-call runtime destroys the
// stash before any later bundle can read it), so require the exact value
// "1" rather than mere presence to make accidental enablement impossible.
static bool RnpsRealRegistryEnabled() {
	const char* v = std::getenv("KYTY_RNPS_REAL_REGISTRY");
	return v != nullptr && std::string(v) == "1";
}

// ps5-vsh: does a bundle entry name refer to RNPS's base_dll bundle? Entry
// strings come from the guest (RNPS) and are not ours to fix, so match loosely
// and case-insensitively. Used to key the base_dll registry stash on the bundle
// that actually produced it, instead of whichever bundle happened to evaluate
// first (host.ps.bundle evaluates first and its completion is NOT base_dll).
static bool RnpsEntryLooksLikeBaseDll(const std::string& entry) {
	std::string lower;
	lower.reserve(entry.size());
	for (char c : entry) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return lower.find("base_dll") != std::string::npos ||
	       lower.find("base-dll") != std::string::npos ||
	       lower.find("basedll") != std::string::npos;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HermesVmExecute(uint64_t handle, const char* entry) {
	PRINT_NAME();
	if (entry == nullptr) {
		LOGF("\t handle = 0x%016" PRIx64 " null entry pointer, failing closed "
		     "with 0x80940001\n",
		     handle);
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	VmContext* ctx = Detail::FindContext(handle);
	if (ctx == nullptr) {
		LOGF("\t handle = 0x%016" PRIx64 " unknown handle, failing closed with "
		     "0x80940001\n",
		     handle);
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	// Evaluate cache: same bytecode hash + same entry repeats the cached
	// result code without re-logging the bytecode dump (real-runtime style).
	if (ctx->hasCachedResult && ctx->cachedEntry == entry) {
		return ctx->cachedResult;
	}
#ifdef KYTY_USE_HERMES
	// Real dispatch: ctx->bytecode (decrypted HBC) runs on the host VM.
	// Link/execution proven in scratch _Build/hermes-only 2026-10-05
	// (hermes.lib + jsi.lib; hello.hbc executed, print observed).
	// NOTE: one runtime per Execute; a future turn should reuse a runtime
	// per context. This TU needs /EHsc when KYTY_USE_HERMES=1 (jsi throws).
	ctx->execCount++;
	try {
		auto runtime = facebook::hermes::makeHermesRuntime();
		if (!runtime) {
			LOGF("\t handle = 0x%016" PRIx64 " makeHermesRuntime failed, "
			     "returning 0x80940002\n",
			     handle);
			return HERMES_VM_ERROR_NO_VM;
		}
		// ps5-vsh: RNPS host globals. On HW the NPXS40000 daemon sets up
		// the JS environment before app bundles evaluate: bundles take
		// their bundleRegistryLoad* path, whose result feeds the require
		// table. Our loader returns a RICH RN namespace (StyleSheet,
		// AppRegistry, Platform + TurboModule namespaces).
		{
			// ps5-vsh: base_dll intentionally ABSENT (undefined) so bundles
			// take their bundleRegistryLoad* path like on HW. The loader
			// DEFINES global.base_dll (side effect; its result is discarded)
			// as a callable carrying the RN namespace, since the bundle
			// BOTH calls base_dll(...) AND reads .StyleSheet/.AppRegistry.
			auto loaderStub = [](
			                      facebook::jsi::Runtime& rt,
			                      const facebook::jsi::Value&,
			                      const facebook::jsi::Value* args,
			                      size_t count) -> facebook::jsi::Value {
				std::string name =
				    count >= 1 ? args[0].toString(rt).utf8(rt) : "<none>";
				// ps5-vsh: if the base_dll evaluation already ran, its registry
				// function is stashed; publish it verbatim rather than the
				// synthetic RN namespace (which shadowed the real module graph
				// and is why host.ps.bundle never loaded the real app).
				if (RnpsRealRegistryEnabled()) {
					try {
						facebook::jsi::Value real =
						    rt.global().getProperty(rt, "__kytyRealBaseDll");
						if (!real.isObject() || !real.getObject(rt).isFunction(rt)) {
							// Legacy slot empty: scan the entry-keyed results for
							// the bundle whose entry names base_dll.
							auto results =
							    rt.global().getProperty(rt, "__kytyBundleResults");
							if (results.isObject()) {
								auto         obj   = results.getObject(rt);
								auto         names = obj.getPropertyNames(rt);
								const size_t n     = names.size(rt);
								for (size_t i = 0; i < n; i++) {
									std::string key;
									try {
										key = names.getValueAtIndex(rt, i)
										          .toString(rt)
										          .utf8(rt);
									} catch (...) {
										continue;
									}
									if (!RnpsEntryLooksLikeBaseDll(key)) {
										continue;
									}
									auto cand = obj.getProperty(rt, key.c_str());
									if (cand.isObject() &&
									    cand.getObject(rt).isFunction(rt)) {
										real = std::move(cand);
										LOGF("\t [RNPS host] base_dll registry "
										     "found under entry-keyed "
										     "results[%s]\n",
										     key.c_str());
										break;
									}
								}
							}
						}
						if (real.isObject()) {
							auto realObj = real.getObject(rt);
							if (realObj.isFunction(rt)) {
								LOGF("\t [RNPS host] bundleRegistry*(\"%s\") -> REAL "
								     "base_dll registry\n",
								     name.c_str());
								rt.global().setProperty(rt, "base_dll", realObj);
								return facebook::jsi::Value(std::move(realObj));
							}
						}
					} catch (const facebook::jsi::JSError& e) {
						LOGF("\t [RNPS host] real registry lookup failed: %s\n",
						     e.getMessage().c_str());
					}
				}
				LOGF("\t [RNPS host] bundleRegistry*(\"%s\") -> define global.base_dll\n",
				     name.c_str());
				facebook::jsi::Object ns = ::RnpsBridge::BuildRnNamespace(rt);
				auto baseFn = facebook::jsi::Function::createFromHostFunction(
				    rt,
				    facebook::jsi::PropNameID::forAscii(rt, "base_dll"), 8,
				    [](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
				       const facebook::jsi::Value* args2,
				       size_t count2) -> facebook::jsi::Value {
					    std::string a0 = "<none>";
					    if (count2 >= 1) {
						    try {
							    if (args2[0].isString()) {
								    a0 = args2[0].toString(rt2).utf8(rt2);
							    } else if (args2[0].isNumber()) {
								    a0 = "num:" +
								        std::to_string(
								            static_cast<long long>(args2[0].asNumber()));
							    } else if (args2[0].isObject()) {
								    a0 = "<object>";
							    } else if (args2[0].isUndefined()) {
								    a0 = "<undefined>";
							    } else {
								    a0 = "<other>";
							    }
							    if (a0.size() > 160) {
								    a0.resize(160);
							    }
						    } catch (...) {
							    a0 = "<unprintable>";
						    }
					    }
					    LOGF("\t [RNPS host] base_dll(...) called argc=%zu a0=%s -> RN namespace\n",
					         count2, a0.c_str());
					    (void)args2;
					    return ::RnpsBridge::BuildRnNamespace(rt2);
				    });
				baseFn.setProperty(rt, "StyleSheet",
				                   ns.getProperty(rt, "StyleSheet"));
				baseFn.setProperty(rt, "AppRegistry",
				                   ns.getProperty(rt, "AppRegistry"));
				baseFn.setProperty(rt, "Platform", ns.getProperty(rt, "Platform"));
				baseFn.setProperty(rt, "Dimensions",
				                   ns.getProperty(rt, "Dimensions"));
				baseFn.setProperty(rt, "PixelRatio",
				                   ns.getProperty(rt, "PixelRatio"));
				baseFn.setProperty(rt, "__kytyRnStub", true);
				rt.global().setProperty(rt, "base_dll", std::move(baseFn));
				return ns;
			};
			for (const char* name :
			     {"bundleRegistryLoader", "bundleRegistryLoad"}) {
				auto fn = facebook::jsi::Function::createFromHostFunction(
				    *runtime, facebook::jsi::PropNameID::forAscii(*runtime, name),
				    3, loaderStub);
				runtime->global().setProperty(*runtime, name, std::move(fn));
			}
#ifdef KYTY_USE_HERMES
			// ps5-vsh: TurboModule + RNPS namespace shims (see RnpsBridge).
			{
				auto turboProxy = facebook::jsi::Function::createFromHostFunction(
				    *runtime,
				    facebook::jsi::PropNameID::forAscii(*runtime, "__turboModuleProxy"),
				    1,
				    [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
				       const facebook::jsi::Value* args,
				       size_t count) -> facebook::jsi::Value {
				    std::string name = count >= 1
				        ? args[0].toString(rt).utf8(rt)
				        : "<none>";
				    if (name == "UIManager") {
					    LOGF("\t [RNPS host] __turboModuleProxy(\"UIManager\") -> real UIManager\n");
					    return ::RnpsBridge::BuildUIManager(rt);
				    }
				    LOGF("\t [RNPS host] __turboModuleProxy(\"%s\") -> stub\n",
				         name.c_str());
					    return facebook::jsi::Object::createFromHostObject(
					        rt, std::make_shared<::RnpsBridge::TurboModuleStub>(name));
				    });
			runtime->global().setProperty(*runtime, "__turboModuleProxy",
			                              std::move(turboProxy));
			}
			for (const char* ns : {"RNPS", "PSM", "LoginMgr", "UrlConfig"}) {
				facebook::jsi::Object obj(*runtime);
				obj.setProperty(*runtime, "__kytyStub", true);
				auto getAppInfoFn =
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "getAppInfo"),
				        0,
				        [](facebook::jsi::Runtime& rt2,
				           const facebook::jsi::Value&,
				           const facebook::jsi::Value*, size_t)
				            -> facebook::jsi::Value {
					        LOGF("\t [RNPS host] getAppInfo() -> manifest stub\n");
					        facebook::jsi::Object info(rt2);
					        info.setProperty(
					            rt2, "titleId",
					            facebook::jsi::String::createFromAscii(rt2, "NPXS40024"));
					        info.setProperty(
					            rt2, "applicationName",
					            facebook::jsi::String::createFromAscii(
					                rt2, "rnps-capture-menu"));
					        info.setProperty(
					            rt2, "applicationVersion",
					            facebook::jsi::String::createFromAscii(rt2, "13.0.0+11517"));
					        info.setProperty(
					            rt2, "jsBackend",
					            facebook::jsi::String::createFromAscii(rt2, "HERMES"));
					        return info;
				        });
				obj.setProperty(*runtime, "getAppInfo", std::move(getAppInfoFn));
				auto genericMethod = [](const char* label) {
					return [label](facebook::jsi::Runtime& rt2,
					               const facebook::jsi::Value&,
					               const facebook::jsi::Value* args,
					               size_t count) -> facebook::jsi::Value {
						(void)args;
						(void)count;
						LOGF("\t [RNPS host] %s() -> stub object\n", label);
						facebook::jsi::Object out(rt2);
						out.setProperty(rt2, "__kytyStub", true);
						return out;
					};
				};
				obj.setProperty(
				    *runtime, "TTPoolInstance",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "TTPoolInstance"),
				        1, genericMethod("TTPoolInstance")));
				obj.setProperty(
				    *runtime, "SceneQ",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime, facebook::jsi::PropNameID::forAscii(*runtime, "SceneQ"),
				        1, genericMethod("SceneQ")));
				obj.setProperty(
				    *runtime, "GetIpabadHandle",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "GetIpabadHandle"),
				        1, genericMethod("GetIpabadHandle")));
				obj.setProperty(
				    *runtime, "appUninstall2",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "appUninstall2"),
				        1, genericMethod("appUninstall2")));
				// ps5-vsh: user identity (mirrors user_list.dat uid 1000 +
				// MgrIntGetUserList FakeUserId). Callers REQUIRE typeof
				// userId === 'number' (invariant "argument type mismatch").
				auto userIdFn = [](const char* label) {
					return [label](facebook::jsi::Runtime& rt2,
					               const facebook::jsi::Value&,
					               const facebook::jsi::Value*, size_t)
					            -> facebook::jsi::Value {
						LOGF("\t [RNPS host] %s() -> 1000\n", label);
						(void)rt2;
						return facebook::jsi::Value(1000.0);
					};
				};
				obj.setProperty(
				    *runtime, "getUserId",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "getUserId"),
				        0, userIdFn("getUserId")));
				obj.setProperty(
				    *runtime, "getLoginUserId",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "getLoginUserId"),
				        0, userIdFn("getLoginUserId")));
				obj.setProperty(
				    *runtime, "getCurrentUserId",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "getCurrentUserId"),
				        0, userIdFn("getCurrentUserId")));
				runtime->global().setProperty(*runtime, ns, std::move(obj));
			}
			// ps5-vsh: legacy NativeModules bridge (verified need:
			// NPXS40062 throws "Cannot read property 'BlobModule' of
			// undefined"). Every module resolves to a TurboModuleStub.
			{
				facebook::jsi::Object nativeModules =
				    facebook::jsi::Object::createFromHostObject(
				        *runtime,
				        std::make_shared<::RnpsBridge::NativeModulesStub>());
				runtime->global().setProperty(*runtime, "NativeModules",
				                              std::move(nativeModules));
			}
			// ps5-vsh: timers + console. Startup code calls setTimeout et
			// al as bare functions; missing timers throw exactly
			// "undefined is not a function". Fire callbacks synchronously
			// with TWO guards: max 4 fires per timer id AND a global
			// budget (polling loops mint fresh ids each iteration —
			// per-id caps alone recurse forever, cf. NPXS40002).
			{
				struct TimerState {
					std::unordered_map<uint64_t, int> fires;
					// ps5-vsh: trampoline queue (constant C++ depth).
					// Reentrant scheduling (polling loops!) enqueues instead
					// of recursing; the pump drains iteratively. Caps the
					// native-stack blowup seen in NPXS40002's setTimeout loop.
					std::vector<facebook::jsi::Function> pending;
					bool                                 pumping = false;
					uint64_t                             next = 1;
					int                                  totalFired = 0;
				};
				static constexpr int kMaxTimerFires = 512;
				auto timers = std::make_shared<TimerState>();
				auto pumpTimers = [timers](facebook::jsi::Runtime& rt) {
					if (timers->pumping) {
						return;
					}
					timers->pumping = true;
					while (!timers->pending.empty() &&
					       timers->totalFired < kMaxTimerFires) {
						facebook::jsi::Function cb =
						    std::move(timers->pending.back());
						timers->pending.pop_back();
						timers->totalFired++;
						try {
							cb.call(rt, nullptr, 0);
						} catch (const facebook::jsi::JSError& e) {
							LOGF("\t [RNPS host] timer callback threw: %s (swallowed)\n",
							     e.getMessage().c_str());
						}
					}
					if (!timers->pending.empty()) {
						LOGF("\t [RNPS host] timer budget exhausted (%d fired, %zu queued) "
						     "-> swallow rest\n",
						     timers->totalFired, timers->pending.size());
						timers->pending.clear();
					}
					timers->pumping = false;
				};
				auto setTimer = [timers, pumpTimers](bool interval) {
					return [timers, pumpTimers, interval](
					           facebook::jsi::Runtime& rt,
					           const facebook::jsi::Value&,
					           const facebook::jsi::Value* args,
					           size_t count) -> facebook::jsi::Value {
						const uint64_t id = timers->next++;
						int&           n  = timers->fires[id];
						const bool fire =
						    n < 4 && timers->totalFired < kMaxTimerFires;
						LOGF("\t [RNPS host] %s(...) id=%llu -> %s\n",
						     interval ? "setInterval" : "setTimeout",
						     static_cast<unsigned long long>(id),
						     fire ? "enqueue" : "swallow");
						if (fire && count >= 1 && args[0].isObject() &&
						    args[0].asObject(rt).isFunction(rt)) {
							n++;
							timers->pending.push_back(
							    args[0].asObject(rt).asFunction(rt));
							pumpTimers(rt);
						} else if (!fire) {
							n++;
						}
						return facebook::jsi::Value(static_cast<double>(id));
					};
				};
				auto clearTimer = [timers](facebook::jsi::Runtime& rt,
				                           const facebook::jsi::Value&,
				                           const facebook::jsi::Value*,
				                           size_t) -> facebook::jsi::Value {
					(void)timers;
					(void)rt;
					return facebook::jsi::Value::undefined();
				};
				runtime->global().setProperty(
				    *runtime, "setTimeout",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "setTimeout"),
				        2, setTimer(false)));
				runtime->global().setProperty(
				    *runtime, "setInterval",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "setInterval"),
				        2, setTimer(true)));
				runtime->global().setProperty(
				    *runtime, "clearTimeout",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "clearTimeout"),
				        1, clearTimer));
				runtime->global().setProperty(
				    *runtime, "clearInterval",
				    facebook::jsi::Function::createFromHostFunction(
				        *runtime,
				        facebook::jsi::PropNameID::forAscii(*runtime, "clearInterval"),
				        1, clearTimer));
				facebook::jsi::Object consoleObj(*runtime);
				auto consoleFn = [](const char* level) {
					return [level](facebook::jsi::Runtime& rt,
					               const facebook::jsi::Value&,
					               const facebook::jsi::Value* args,
					               size_t count) -> facebook::jsi::Value {
						std::string msg;
						for (size_t i = 0; i < count && i < 4; i++) {
							try {
								if (i > 0) {
									msg += " ";
								}
								msg += args[i].toString(rt).utf8(rt);
							} catch (...) {
								msg += "<unprintable>";
							}
						}
						if (msg.size() > 300) {
							msg.resize(300);
						}
						LOGF("\t [RNPS jsconsole.%s] %s\n", level, msg.c_str());
						return facebook::jsi::Value::undefined();
					};
				};
				for (const char* m : {"log", "warn", "error", "info", "debug"}) {
					consoleObj.setProperty(
					    *runtime, m,
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime, facebook::jsi::PropNameID::forAscii(*runtime, m),
					        4, consoleFn(m)));
				}
				runtime->global().setProperty(*runtime, "console", std::move(consoleObj));
			}
			// ps5-vsh: RN runtime globals batch. Startup touches these as
			// bare globals; missing ones throw "undefined is not a
			// function". All log-and-succeed stubs; real semantics later.
			{
				auto logStubFn = [](const char* label) {
					return [label](facebook::jsi::Runtime& rt,
					               const facebook::jsi::Value&,
					               const facebook::jsi::Value* args,
					               size_t count) -> facebook::jsi::Value {
						(void)args;
						(void)count;
						(void)rt;
						LOGF("\t [RNPS host] %s() -> stub\n", label);
						facebook::jsi::Object out(rt);
						out.setProperty(rt, "__kytyStub", true);
						return out;
					};
				};
				auto addFn = [&](const char* name, int params,
				                 facebook::jsi::HostFunctionType fn) {
					runtime->global().setProperty(
					    *runtime, name,
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime, facebook::jsi::PropNameID::forAscii(*runtime, name),
					        static_cast<unsigned int>(params), std::move(fn)));
				};
				addFn("requestAnimationFrame",
				      1,
				      [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
				         const facebook::jsi::Value* args,
				         size_t count) -> facebook::jsi::Value {
					      thread_local int depth = 0;
					      static int totalFrames = 0;
					      constexpr int kMaxFrames = 600;
					      static uint64_t nextId = 1;
					      const uint64_t id = nextId++;
					      if (++depth > 32 || totalFrames >= kMaxFrames) {
						      depth--;
						      LOGF("\t [RNPS host] requestAnimationFrame() id=%llu -> swallow (depth/budget)\n",
						           static_cast<unsigned long long>(id));
						      return facebook::jsi::Value(
						          static_cast<double>(id));
					      }
					      totalFrames++;
					      LOGF("\t [RNPS host] requestAnimationFrame() id=%llu frame=%d -> fire-now\n",
					           static_cast<unsigned long long>(id),
					           totalFrames);
					      if (count >= 1 && args[0].isObject() &&
					          args[0].asObject(rt).isFunction(rt)) {
						      const double ts = static_cast<double>(
						          std::chrono::duration_cast<
						              std::chrono::milliseconds>(
						              std::chrono::steady_clock::now()
						                  .time_since_epoch())
						              .count());
						      facebook::jsi::Value argv[] = {
						          facebook::jsi::Value(ts)};
						      try {
							      args[0].asObject(rt).asFunction(rt).call(
							          rt,
							          static_cast<const facebook::jsi::Value*>(
							              argv),
							          static_cast<size_t>(1));
						      } catch (const facebook::jsi::JSError& e) {
							      LOGF("\t [RNPS host] rAF callback threw: %s (swallowed)\n",
							           e.getMessage().c_str());
						      }
					      }
					      depth--;
					      return facebook::jsi::Value(
					          static_cast<double>(id));
				      });
				addFn("cancelAnimationFrame", 1, logStubFn("cancelAnimationFrame"));
				addFn("queueMicrotask", 1,
				      [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
				         const facebook::jsi::Value* args,
				         size_t count) -> facebook::jsi::Value {
					      // ps5-vsh: reentrancy depth guard (same overflow
					      // class as the timer trampoline).
					      thread_local int depth = 0;
					      if (++depth > 32) {
						      depth--;
						      LOGF("\t [RNPS host] queueMicrotask() depth cap -> swallow\n");
						      return facebook::jsi::Value::undefined();
					      }
					      LOGF("\t [RNPS host] queueMicrotask() -> run-now\n");
					      if (count >= 1 && args[0].isObject() &&
					          args[0].asObject(rt).isFunction(rt)) {
						      args[0].asObject(rt).asFunction(rt).call(rt, nullptr, 0);
					      }
					      depth--;
					      return facebook::jsi::Value::undefined();
				      });
				addFn("fetch", 1, logStubFn("fetch"));
				addFn("alert", 1, logStubFn("alert"));
				addFn("requireNativeComponent", 1,
				      [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
				         const facebook::jsi::Value* args,
				         size_t count) -> facebook::jsi::Value {
					      std::string name = count >= 1
					          ? args[0].toString(rt).utf8(rt)
					          : "<none>";
					      LOGF("\t [RNPS host] requireNativeComponent(\"%s\") -> any-proxy\n",
					           name.c_str());
					      return ::RnpsBridge::GetAnyProxy(rt);
				      });
				// ps5-vsh: ErrorUtils (RN error handling reads
				// ErrorUtils.getGlobalHandler() at startup).
				{
					facebook::jsi::Object eu(*runtime);
					auto ggFn = facebook::jsi::Function::createFromHostFunction(
					    *runtime,
					    facebook::jsi::PropNameID::forAscii(*runtime, "getGlobalHandler"),
					    0,
					    [](facebook::jsi::Runtime& rt2,
					       const facebook::jsi::Value&,
					       const facebook::jsi::Value*, size_t)
					        -> facebook::jsi::Value {
						    LOGF("\t [RNPS host] ErrorUtils.getGlobalHandler() -> noop fn\n");
						    auto noop = facebook::jsi::Function::createFromHostFunction(
						        rt2,
						        facebook::jsi::PropNameID::forAscii(rt2, "noop"), 2,
						        [](facebook::jsi::Runtime& rt3,
						           const facebook::jsi::Value&,
						           const facebook::jsi::Value*, size_t)
						            -> facebook::jsi::Value {
							        LOGF("\t [RNPS host] global-error-handler invoked (swallowed)\n");
							        (void)rt3;
							        return facebook::jsi::Value::undefined();
						        });
						    return noop;
					    });
					eu.setProperty(*runtime, "getGlobalHandler", std::move(ggFn));
					auto sgFn = facebook::jsi::Function::createFromHostFunction(
					    *runtime,
					    facebook::jsi::PropNameID::forAscii(*runtime, "setGlobalHandler"),
					    1,
					    [](facebook::jsi::Runtime& rt2,
					       const facebook::jsi::Value&,
					       const facebook::jsi::Value*, size_t)
					        -> facebook::jsi::Value {
						    LOGF("\t [RNPS host] ErrorUtils.setGlobalHandler() -> ack\n");
						    (void)rt2;
						    return facebook::jsi::Value::undefined();
					    });
					eu.setProperty(*runtime, "setGlobalHandler", std::move(sgFn));
					runtime->global().setProperty(*runtime, "ErrorUtils", std::move(eu));
				}
				addFn("setImmediate", 1,
				      [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
				         const facebook::jsi::Value* args,
				         size_t count) -> facebook::jsi::Value {
					      thread_local int depth = 0;
					      if (++depth > 32) {
						      depth--;
						      LOGF("\t [RNPS host] setImmediate() depth cap -> swallow\n");
						      return facebook::jsi::Value(1.0);
					      }
					      LOGF("\t [RNPS host] setImmediate() -> fire-now\n");
					      if (count >= 1 && args[0].isObject() &&
					          args[0].asObject(rt).isFunction(rt)) {
						      args[0].asObject(rt).asFunction(rt).call(rt, nullptr, 0);
					      }
					      depth--;
					      return facebook::jsi::Value(1.0);
				      });
				addFn("clearImmediate", 1, logStubFn("clearImmediate"));
				runtime->global().setProperty(*runtime, "__DEV__", false);
				{
					facebook::jsi::Object navigator(*runtime);
					navigator.setProperty(
					    *runtime, "product",
					    facebook::jsi::String::createFromAscii(*runtime, "ReactNative"));
				runtime->global().setProperty(*runtime, "navigator",
				                              std::move(navigator));
			}
			// ps5-vsh: `global` === globalThis (RN polyfill expectation).
			// Hermes exposes globalThis; alias it so `global.X` works.
			// Same for window/self: bundles gate on browser-ish scope
			// ("Are you sure this is running in the browser?").
			{
				facebook::jsi::Value gthis =
				    runtime->global().getProperty(*runtime, "globalThis");
				if (!gthis.isUndefined()) {
					runtime->global().setProperty(*runtime, "global", gthis);
					runtime->global().setProperty(*runtime, "window", gthis);
					runtime->global().setProperty(*runtime, "self", gthis);
				} else {
					facebook::jsi::Object self(*runtime);
					self.setProperty(*runtime, "__kytyStub", true);
					runtime->global().setProperty(*runtime, "global",
					                              std::move(self));
				}
			}
				{
					facebook::jsi::Object pe(*runtime);
					auto nowFn = facebook::jsi::Function::createFromHostFunction(
					    *runtime, facebook::jsi::PropNameID::forAscii(*runtime, "now"),
					    0,
					    [](facebook::jsi::Runtime&, const facebook::jsi::Value&,
					       const facebook::jsi::Value*, size_t) -> facebook::jsi::Value {
						    return facebook::jsi::Value(
						        static_cast<double>(
						            std::chrono::duration_cast<std::chrono::milliseconds>(
						                std::chrono::steady_clock::now().time_since_epoch())
						                .count()));
					    });
					pe.setProperty(*runtime, "now", std::move(nowFn));
					auto markFn = [](const char* label) {
						return [label](facebook::jsi::Runtime& rt2,
						               const facebook::jsi::Value&,
						               const facebook::jsi::Value* args,
						               size_t count) -> facebook::jsi::Value {
							std::string name = count >= 1
							    ? args[0].toString(rt2).utf8(rt2)
							    : "<none>";
							LOGF("\t [RNPS host] performance.%s(\"%s\")\n", label,
							     name.c_str());
							(void)rt2;
							return facebook::jsi::Value::undefined();
						};
					};
					pe.setProperty(
					    *runtime, "mark",
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime, facebook::jsi::PropNameID::forAscii(*runtime, "mark"),
					        1, markFn("mark")));
					pe.setProperty(
					    *runtime, "measure",
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime,
					        facebook::jsi::PropNameID::forAscii(*runtime, "measure"), 1,
					        markFn("measure")));
					pe.setProperty(
					    *runtime, "clearMarks",
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime,
					        facebook::jsi::PropNameID::forAscii(*runtime, "clearMarks"),
					        1, markFn("clearMarks")));
					pe.setProperty(
					    *runtime, "clearMeasures",
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime,
					        facebook::jsi::PropNameID::forAscii(*runtime, "clearMeasures"),
					        1, markFn("clearMeasures")));
					// ps5-vsh: getEntriesByName/Type return [] (forEach-able,
					// empty timeline at boot). Called as bare env binding.
					auto entriesFn = [](const char* label) {
						return [label](facebook::jsi::Runtime& rt2,
						               const facebook::jsi::Value&,
						               const facebook::jsi::Value* args,
						               size_t count) -> facebook::jsi::Value {
							std::string name = count >= 1
							    ? args[0].toString(rt2).utf8(rt2)
							    : "<none>";
							LOGF("\t [RNPS host] performance.%s(\"%s\") -> []\n",
							     label, name.c_str());
							return facebook::jsi::Array(rt2, 0);
						};
					};
					pe.setProperty(
					    *runtime, "getEntriesByName",
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime,
					        facebook::jsi::PropNameID::forAscii(*runtime, "getEntriesByName"),
					        1, entriesFn("getEntriesByName")));
					pe.setProperty(
					    *runtime, "getEntriesByType",
					    facebook::jsi::Function::createFromHostFunction(
					        *runtime,
					        facebook::jsi::PropNameID::forAscii(*runtime, "getEntriesByType"),
					        1, entriesFn("getEntriesByType")));
					runtime->global().setProperty(*runtime, "performance",
					                              std::move(pe));
				}
				{
					facebook::jsi::Object appState(*runtime);
					appState.setProperty(
					    *runtime, "currentState",
					    facebook::jsi::String::createFromAscii(*runtime, "active"));
				runtime->global().setProperty(*runtime, "AppState",
				                              std::move(appState));
				}
				// ps5-vsh: web-API polyfills as plain JS constructor functions
				// (Hermes 0.12 rejects `return class` expressions and HostFunctions
				// are NOT constructible). Installed per-name via __kytyStubClass.
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(std::string(
				        "globalThis.__kytyStubClass = function(name) {"
				        "  function K() {"
				        "    try { __kytyLog('new ' + name + '()'); } catch (e) {}"
				        "    this.__kytyStub = true; this.__kytyClass = name;"
				        "  }"
				        "  return K;"
				        "}")),
				        "kyty-stubs");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"AbortController\"] = __kytyStubClass(\"AbortController\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"AbortSignal\"] = __kytyStubClass(\"AbortSignal\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"Headers\"] = __kytyStubClass(\"Headers\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"Request\"] = __kytyStubClass(\"Request\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"Response\"] = __kytyStubClass(\"Response\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"FormData\"] = __kytyStubClass(\"FormData\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"URLSearchParams\"] = __kytyStubClass(\"URLSearchParams\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"Blob\"] = __kytyStubClass(\"Blob\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"File\"] = __kytyStubClass(\"File\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"FileReader\"] = __kytyStubClass(\"FileReader\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"XMLHttpRequest\"] = __kytyStubClass(\"XMLHttpRequest\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"WebSocket\"] = __kytyStubClass(\"WebSocket\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"EventSource\"] = __kytyStubClass(\"EventSource\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"MessageChannel\"] = __kytyStubClass(\"MessageChannel\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"MessagePort\"] = __kytyStubClass(\"MessagePort\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"TextEncoder\"] = __kytyStubClass(\"TextEncoder\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"TextDecoder\"] = __kytyStubClass(\"TextDecoder\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"Event\"] = __kytyStubClass(\"Event\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"CustomEvent\"] = __kytyStubClass(\"CustomEvent\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"CloseEvent\"] = __kytyStubClass(\"CloseEvent\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"MessageEvent\"] = __kytyStubClass(\"MessageEvent\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"ErrorEvent\"] = __kytyStubClass(\"ErrorEvent\");")),
				        "kyty-stub-one");
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis[\"ProgressEvent\"] = __kytyStubClass(\"ProgressEvent\");")),
				        "kyty-stub-one");
				// ps5-vsh: real URL parser as plain constructor function.
				(void)runtime->evaluateJavaScript(
				        std::make_shared<facebook::jsi::StringBuffer>(std::string(
				        "globalThis.__kytyURL = function(href) {"
				        "  this.href = String(href || '');"
				        "  var m = this.href.match(/^(?:([a-zA-Z][a-zA-Z0-9+.-]*:))?(?:\\/\\/([^\\\\/?#]*))?([^?#]*)(\\\\?[^#]*)?/);"
				        "  if (m) { this.protocol = m[1] || ''; this.host = m[2] || ''; this.hostname = m[2] || ''; this.pathname = m[3] || '/'; this.search = m[4] || ''; }"
				        "  else { this.protocol = ''; this.host = ''; this.hostname = ''; this.pathname = this.href; this.search = ''; }"
				        "}")),
				        "kyty-url");
				{
				facebook::jsi::Value cls =
				    runtime->global().getProperty(*runtime, "__kytyURL");
				if (cls.isObject() &&
				    cls.asObject(*runtime).isFunction(*runtime)) {
				runtime->global().setProperty(*runtime, "URL", cls);
				} else {
				LOGF("\t [RNPS host] __kytyURL missing!\n");
				}
				}
					// crypto.getRandomValues (host RNG) + atob/btoa.
					{
						facebook::jsi::Object crypto(*runtime);
						auto grv = facebook::jsi::Function::createFromHostFunction(
						    *runtime,
						    facebook::jsi::PropNameID::forAscii(*runtime, "getRandomValues"),
						    1,
						    [](facebook::jsi::Runtime& rt2,
						       const facebook::jsi::Value&,
						       const facebook::jsi::Value* args,
						       size_t count) -> facebook::jsi::Value {
							    LOGF("\t [RNPS host] crypto.getRandomValues() -> host RNG\n");
							    if (count >= 1 && args[0].isObject()) {
								    try {
									    facebook::jsi::Object arr =
									        args[0].asObject(rt2);
									    static uint64_t s = 0x243F6A8885A308D3ull;
									    for (uint32_t i = 0;; i++) {
										    facebook::jsi::Value lenV =
										        arr.getProperty(rt2, "length");
										    if (!lenV.isNumber()) {
											    break;
										    }
										    const double len = lenV.asNumber();
										    if (i >= static_cast<uint32_t>(len)) {
											    break;
										    }
										    s ^= s << 13;
										    s ^= s >> 7;
										    s ^= s << 17;
										    arr.setProperty(
										        rt2,
										        facebook::jsi::PropNameID::forAscii(
										            rt2, std::to_string(i).c_str()),
										        facebook::jsi::Value(
										            static_cast<double>(s & 0xFFu)));
										    if (i > 65536) {
											    break;
										    }
									    }
									    return args[0].asObject(rt2);
								    } catch (...) {
								    }
							    }
							    return facebook::jsi::Value::undefined();
						    });
						crypto.setProperty(*runtime, "getRandomValues", std::move(grv));
						crypto.setProperty(*runtime, "randomUUID",
						                   facebook::jsi::String::createFromAscii(
						                       *runtime, "00000000-0000-4000-8000-000000000000"));
						runtime->global().setProperty(*runtime, "crypto",
						                              std::move(crypto));
					}
				}
			}
			// ps5-vsh: minimal react-native surface. Bundles call
			// StyleSheet.create(...) first; the real RN renderer comes
			// later. create() is pure JS semantics: return input unchanged.
			{
				facebook::jsi::Object styleSheet(*runtime);
				auto createFn = facebook::jsi::Function::createFromHostFunction(
				    *runtime,
				    facebook::jsi::PropNameID::forAscii(*runtime, "create"), 1,
				    [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
				       const facebook::jsi::Value* args,
				       size_t count) -> facebook::jsi::Value {
					    LOGF("\t [RNPS host] StyleSheet.create() -> passthrough\n");
					    if (count >= 1 && !args[0].isUndefined()) {
						    // Return a fresh object to avoid aliasing host values.
						    facebook::jsi::Object out(rt);
						    out.setProperty(rt, "__kytyStyles", true);
						    return out;
					    }
					    return facebook::jsi::Value::undefined();
				    });
				styleSheet.setProperty(*runtime, "create", std::move(createFn));
				styleSheet.setProperty(*runtime, "__kytyStub", true);
				runtime->global().setProperty(*runtime, "StyleSheet",
				                              std::move(styleSheet));
			}
#endif
		// evaluateJavaScript detects HBC magic and runs bytecode directly
		// (no recompile); entry names the call for logging only.
		//
		// ps5-vsh: universal stub oracle. A Proxy-based __kytyAny that
		// absorbs ANY get/call/construct (logging each) lets bundle
		// execution advance past unknown natives while recording exactly
		// what the bundle needs. Loader results route through it.
		try {
			// ps5-vsh: headless mount driver starts clean per run.
			::RnpsBridge::ResetAppRegistryForRun();
			auto logFn = facebook::jsi::Function::createFromHostFunction(
			    *runtime,
			    facebook::jsi::PropNameID::forAscii(*runtime, "__kytyLog"), 1,
			    [](facebook::jsi::Runtime& rt, const facebook::jsi::Value&,
			       const facebook::jsi::Value* args,
			       size_t count) -> facebook::jsi::Value {
				    if (count >= 1) {
					    try {
						    LOGF("\t [RNPS js] %s\n",
						         args[0].toString(rt).utf8(rt).c_str());
					    } catch (...) {
						    LOGF("\t [RNPS js] <unprintable>\n");
					    }
				    }
				    return facebook::jsi::Value::undefined();
			    });
			runtime->global().setProperty(*runtime, "__kytyLog", std::move(logFn));
			// ps5-vsh: live-listener registry (JS-side array = GC-safe roots).
			// Resident mode re-fires these per tick; one-shot mode ignores it.
			try {
				(void)runtime->evaluateJavaScript(
				    std::make_shared<facebook::jsi::StringBuffer>(
				        std::string("globalThis.__kytyListeners=[];")),
				    "kyty-listeners");
			} catch (...) {
			}
			(void)runtime->evaluateJavaScript(
			    std::make_shared<facebook::jsi::StringBuffer>(std::string(
		        "globalThis.__kytyAny = new Proxy(function(){}, {"
		        "get: function(t, p) { "
		        "  if (p === Symbol.toPrimitive || p === 'toString' || p === 'valueOf') return function(){ return '[kytyAny]'; };"
		        "  if (p === 'prototype') { var po = {}; po.isReactComponent = {}; return po; } "
		        "  if (p === 'screen' || p === 'getScreen') return undefined; "
		        "  if (p === 'path') return null; "
		        "  if (p === 'VshNMUserModule') return {launchedUser: 1000, loginUserId: 1000, __kytyStub: true}; "
		        "  if (p === 'then') return function(onF, onR) { try { __kytyLog('any.then attach'); } catch (e) {} try { if (typeof onF === 'function') { var r = onF(globalThis.__kytyAny); if (r && typeof r.then === 'function') return r; } } catch (e) { try { __kytyLog('any.then cb threw ' + e); } catch (_) {} try { if (typeof onR === 'function') onR(e); } catch (_) {} } return globalThis.__kytyAny; };"
		        "  if (p === 'catch') return function(onR) { try { __kytyLog('any.catch attach (swallow)'); } catch (e) {} return globalThis.__kytyAny; };"
		        "  if (p === 'finally') return function(cb) { try { if (typeof cb === 'function') cb(); } catch (e) {} return globalThis.__kytyAny; };"
		        "  if (p === 'subscribe') return function() { try { __kytyLog('any.subscribe attach fire-now'); } catch (e) {} try { for (var i = 0; i < arguments.length; i++) { if (typeof arguments[i] === 'function') { try { (globalThis.__kytyListeners||(globalThis.__kytyListeners=[])).push({ev:'subscribe',cb:arguments[i]}); } catch (_) {} arguments[i](globalThis.__kytyAny); } } } catch (e) {} return {unsubscribe: function(){}, remove: function(){}}; };"
		        "  if (p === 'addListener' || p === 'addEventListener' || p === 'on' || p === 'once') return function(ev, cb) { try { var es = String(ev); try { if (ev && (typeof ev === 'object' || typeof ev === 'function')) { var ek = Object.keys(ev); es += ' type=' + typeof ev + ' keys=' + ek.slice(0,10).join(','); } } catch (_) {} var ths = '?'; try { ths = (this && this.constructor && this.constructor.name) ? this.constructor.name : typeof this; } catch (_) {} __kytyLog('any.' + String(p) + ' ev=' + es + ' on=' + ths + ' fire-now'); } catch (e) {} var fn = (typeof cb === 'function') ? cb : ((arguments.length >= 3 && typeof arguments[2] === 'function') ? arguments[2] : null); try { if (fn) { try { (globalThis.__kytyListeners||(globalThis.__kytyListeners=[])).push({ev:String(ev),cb:fn}); } catch (_) {} fn(globalThis.__kytyAny); } } catch (e) {} return {remove: function(){}, unsubscribe: function(){}}; };"
		        "  var defs = t.__kytyDefs; if (defs && (p in defs)) return defs[p];"
		        "  if (p === 'length' || p === 'size' || p === 'count') return 1;"
		        "  if (p === 'map' || p === 'forEach' || p === 'filter') return function(cb) { try { __kytyLog('any.' + String(p) + ' fan-out'); } catch (e) {} var r; try { if (typeof cb === 'function') r = cb(globalThis.__kytyAny, 0, globalThis.__kytyAny); } catch (e) {} if (String(p) === 'forEach') return undefined; return [r]; };"
		        "  if (p === 'find' || p === 'findIndex') return function(cb) { try { __kytyLog('any.' + String(p) + ' fan-out'); } catch (e) {} var r; try { if (typeof cb === 'function') r = cb(globalThis.__kytyAny, 0, globalThis.__kytyAny); } catch (e) {} if (String(p) === 'findIndex') return 0; return (r === undefined || r === null || r === false) ? globalThis.__kytyAny : r; };"
		        "  if (p === 'some' || p === 'every' || p === 'includes' || p === 'contains') return function() { try { var a = arguments[0]; if (typeof a === 'function') { try { a(globalThis.__kytyAny, 0, globalThis.__kytyAny); } catch (e) {} } } catch (e) {} return true; };"
		        "  if (p === 'indexOf' || p === 'lastIndexOf' || p === 'search') return function() { try { __kytyLog('any.' + String(p) + ' -> 1 (found)'); } catch (e) {} return 1; };"
			        "  try { __kytyLog('any.get ' + String(p)); } catch (e) {}"
			        "  return globalThis.__kytyAny; },"
			        "set: function(t, p, v) { if (p === 'prototype') { t[p] = v; return true; } ((t.__kytyDefs || (t.__kytyDefs = {}))[p]) = v; return true; },"
			        "defineProperty: function(t, p, d) { if (p === 'prototype') { Object.defineProperty(t, p, d); return true; } ((t.__kytyDefs || (t.__kytyDefs = {}))[p]) = (d && ('value' in d)) ? d.value : undefined; return true; },"
			        "has: function(t, p) { try { __kytyLog('any.has ' + String(p)); } catch (e) {} return true; },"
			        "getOwnPropertyDescriptor: function(t, p) { var defs = t.__kytyDefs; if (p !== 'prototype' && defs && (p in defs)) return {configurable: true, enumerable: true, value: defs[p]}; if (p === 'prototype' || p === 'length' || p === 'name' || p === 'arguments' || p === 'caller') return Object.getOwnPropertyDescriptor(t, p); return {configurable: true, enumerable: true, value: globalThis.__kytyAny}; },"
		        "apply: function(t, th, a) { try { var s = 'any.called argc=' + a.length; for (var i = 0; i < a.length && i < 5; i++) { var v = a[i], r = typeof v; if (r === 'string') s += ' a' + i + '=[str]' + v.slice(0, 80); else if (r === 'number' || r === 'boolean') s += ' a' + i + '=' + v; else if (v === null || v === undefined) s += ' a' + i + '=' + v; else if (r === 'object' || r === 'function') { try { var ks = Object.keys(v); var fns = []; for (var k = 0; k < ks.length && k < 12; k++) { try { if (typeof v[ks[k]] === 'function') fns.push(ks[k]); } catch (_) {} } s += ' a' + i + '=[' + r + ' keys=' + ks.slice(0,12).join(',') + ' fns=' + fns.join(',') + ']'; } catch (_) { s += ' a' + i + '=[' + r + ']'; } } else s += ' a' + i + '=[' + r + ']'; } try { if (a.length > 0 && typeof a[0] === 'string' && (a[0].indexOf('intro=') >= 0 || a[0].indexOf('navigateToHome') >= 0 || a[0].indexOf('homeui:') >= 0)) { try { s += ' STACK=' + String(new Error().stack).slice(0, 600); } catch (_) {} } } catch (_) {} __kytyLog(s); } catch (e) {} return globalThis.__kytyAny; },"
		        "construct: function(t, a) { try { var s = 'any.constructed argc=' + a.length; for (var i = 0; i < a.length && i < 5; i++) { var v = a[i], r = typeof v; if (r === 'string') s += ' a' + i + '=[str]' + v.slice(0, 80); else if (r === 'number' || r === 'boolean') s += ' a' + i + '=' + v; else s += ' a' + i + '=[' + r + ']'; } __kytyLog(s); } catch (e) {} return globalThis.__kytyAny; }"
			        "});")),
			    "kyty-prelude");
		} catch (const facebook::jsi::JSError& e) {
			LOGF("\t prelude failed: %s\n", e.getMessage().c_str());
		}
		std::string bytecodeStr(reinterpret_cast<const char*>(ctx->bytecode.data()),
		                        ctx->bytecode.size());
		{
			LOGF("\t [RNPS host] evaluating bundle %s (%zu bytes)\n", entry,
			     ctx->bytecode.size());
			auto bundleResult = runtime->evaluateJavaScript(
			    std::make_shared<facebook::jsi::StringBuffer>(
			        std::move(bytecodeStr)),
			    entry);
			// ps5-vsh: base_dll's completion value IS its module registry
			// (__webpack_require__), and host.ps.bundle's module 0 is
			// `e.exports = this.base_dll`, so RNPS's bundleRegistryLoad must
			// publish THAT. Record every completion under its entry name so the
			// loader can publish the real registry once base_dll has actually
			// evaluated (KYTY_RNPS_REAL_REGISTRY). Keying on the entry (rather
			// than "first completion wins") matters: host.ps.bundle evaluates
			// first and its completion is a webpack runtime, NOT base_dll.
			if (bundleResult.isObject() && RnpsRealRegistryEnabled()) {
				facebook::jsi::Runtime& rtR = *runtime;
				try {
					auto results =
					    rtR.global().getProperty(rtR, "__kytyBundleResults");
					if (!results.isObject()) {
						rtR.global().setProperty(rtR, "__kytyBundleResults",
						                         facebook::jsi::Object(rtR));
						results = rtR.global().getProperty(rtR, "__kytyBundleResults");
					}
					results.getObject(rtR).setProperty(
					    rtR, facebook::jsi::PropNameID::forUtf8(rtR, entry), bundleResult);
					if (RnpsEntryLooksLikeBaseDll(entry)) {
						rtR.global().setProperty(rtR, "__kytyRealBaseDll", bundleResult);
						LOGF("\t [RNPS host] bundle %s completion stashed as "
						     "__kytyRealBaseDll\n",
						     entry);
					} else {
						LOGF("\t [RNPS host] bundle %s completion recorded "
						     "(entry-keyed)\n",
						     entry);
					}
				} catch (const facebook::jsi::JSError& e) {
					LOGF("\t [RNPS host] stash failed: %s\n", e.getMessage().c_str());
				}
			}
		}
		// voldemort rnhost parity (tools/rnhost/run.js:337): once host has
		// registered "Main", DRIVE the real AppRegistry through the real batched
		// bridge. Nothing renders until this happens - our previous stub only
		// logged STARTUP REACHED, so no createView was ever issued and no pixel
		// could ever be produced.
		if (RnpsRealRegistryEnabled() && !ctx->appDriven) {
			try {
				auto v = runtime->evaluateJavaScript(
				    std::make_shared<facebook::jsi::StringBuffer>(std::string(
				        "var __bb=globalThis.__fbBatchedBridge;"
				        "if(__bb&&typeof __bb.callFunctionReturnFlushedQueue"
				        "==='function'){"
				        "var __q=__bb.callFunctionReturnFlushedQueue("
				        "'AppRegistry','runApplication',"
				        "['Main',{rootTag:1,initialProps:{},fabric:false}]);"
				        "globalThis.__kytyLastQueue=__q;'driven';"
				        "}else{'no __fbBatchedBridge';}")),
				    "drive.js");
				const std::string r = v.toString(*runtime).utf8(*runtime);
				LOGF("\t [RNPS host] drive runApplication -> %s\n", r.c_str());
				// Retry on later bundle evaluations until the bridge is
				// actually up, then never again (one root view).
				if (r == "driven") {
					ctx->appDriven = true;
				}
			} catch (const std::exception& e) {
				LOGF("\t [RNPS host] drive runApplication THREW: %s\n", e.what());
			}
		}
		// ps5-vsh: TTPool promote + MountApp runApplication (observed HW
		// order: instance promoted from TTPoolInstance, then MountApp
		// mounts). Runs for every successful evaluation, one-shot or
		// resident; identity comes from SetRnpsAppInfo (emulator RNPS block).
		{
			std::string tid, tname, tver;
			{
				std::lock_guard<std::mutex> lock(Detail::g_appInfoMutex);
				tid   = Detail::g_appTitleId;
				tname = Detail::g_appName;
				tver  = Detail::g_appVersion;
			}
			if (!tid.empty()) {
				::RnpsBridge::ResetTTPoolForRun();
				::RnpsBridge::ResetSceneQForRun();
				::RnpsBridge::TTPoolPromote(tid, tname.empty() ? tid : tname, tver);
				::RnpsBridge::TTPoolMountApp(tid, tname.empty() ? tid : tname);
				::RnpsBridge::SceneLoadOnMount(tid);
			}
			// ps5-vsh: bring-up testcard (KYTY_RN_TESTCARD=1). Draws three
			// boxes + presents immediately, independent of bundle behavior.
			if (std::getenv("KYTY_RN_TESTCARD") != nullptr) {
				LOGF("\t [RNPS host] testcard requested\n");
				::RnpsBridge::RnTestcard();
			}
			// ps5-vsh: bring-up home grid (KYTY_RN_HOMEGRID=1). FW titles +
			// icons + clock through the same surface/present path.
			if (std::getenv("KYTY_RN_HOMEGRID") != nullptr) {
				LOGF("\t [RNPS host] home grid requested\n");
				::RnpsBridge::RnHomeGrid();
			}
		}
#ifdef KYTY_USE_HERMES
		// ps5-vsh: resident service mode keeps this same runtime alive and
		// pumps live ticks (see RunHbcBufferLive). One-shot path skips it.
		if (ctx->liveSeconds > 0) {
			LOGF("\t [RNPS host] resident mode: pumping %llu live ticks on this runtime\n",
			     static_cast<unsigned long long>(ctx->liveSeconds));
			LivePump(*runtime, ctx->liveSeconds);
		}
#endif
		ctx->cachedEntry     = entry;
		ctx->cachedResult    = 0;
		ctx->hasCachedResult = true;
		return 0;
	} catch (const facebook::jsi::JSError& e) {
		LOGF("\t handle = 0x%016" PRIx64 " entry = \"%s\" threw: %s, "
		     "returning 0x80940003\n",
		     handle, entry, e.getMessage().c_str());
		try {
			const std::string stack = e.getStack();
			LOGF("\t stack (first 2048 chars):\n%.2048s\n", stack.c_str());
		} catch (...) {
			LOGF("\t (no stack available)\n");
		}
		ctx->cachedEntry     = entry;
		ctx->cachedResult    = HERMES_VM_ERROR_EXEC_FAILED;
		ctx->hasCachedResult = true;
		return HERMES_VM_ERROR_EXEC_FAILED;
	}
#else
	LOGF("\t handle = 0x%016" PRIx64 " bytecode size = %llu version = %" PRIu32
	     " fnv1a64 = 0x%016" PRIx64 " entry = \"%s\" -> no VM yet, returning "
	     "0x80940002\n",
	     handle, static_cast<unsigned long long>(ctx->bytecode.size()),
	     ctx->bytecodeVersion, ctx->sourceHash, entry);
	ctx->execCount++;
	ctx->cachedEntry      = entry;
	ctx->cachedResult     = HERMES_VM_ERROR_NO_VM;
	ctx->hasCachedResult  = true;
	// NEXT: defined(KYTY_USE_HERMES) dispatches ctx->bytecode via
	// facebook/hermes (v0.11.0 tag, HBC version 84) once
	// kyty_target_link_hermes(kyty_emulator) is wired in the dispatch turn.
	return HERMES_VM_ERROR_NO_VM;
#endif
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HermesVmGetStats(uint64_t handle, HermesVmStats* outStats) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 " outStats = %p\n", handle,
	     static_cast<void*>(outStats));
	if (outStats == nullptr) {
		LOGF("\t null guest pointer, failing closed with 0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	VmContext* ctx = Detail::FindContext(handle);
	if (ctx == nullptr) {
		LOGF("\t unknown handle, failing closed with 0x80940001\n");
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	outStats->heapSize       = ctx->heapSize;
	outStats->allocatedBytes = ctx->allocatedBytes;
	outStats->gcCollections  = ctx->gcCollections;
	outStats->execCount      = ctx->execCount;
	return 0;
}

} // namespace Vm

LIB_DEFINE(InitHermesVm_1) {
	// LOCAL: not FW NIDs — internal scaffold keys until the real HermesVm NIDs are dumped.
	LIB_FUNC("LOCAL_HermesVmCreate", Vm::HermesVmCreate);
	// LOCAL: not FW NIDs — internal scaffold keys until the real HermesVm NIDs are dumped.
	LIB_FUNC("LOCAL_HermesVmDestroy", Vm::HermesVmDestroy);
	// LOCAL: not FW NIDs — internal scaffold keys until the real HermesVm NIDs are dumped.
	LIB_FUNC("LOCAL_HermesVmExecute", Vm::HermesVmExecute);
	// LOCAL: not FW NIDs — internal scaffold keys until the real HermesVm NIDs are dumped.
	LIB_FUNC("LOCAL_HermesVmGetStats", Vm::HermesVmGetStats);
}

namespace HermesVmHost {

// RNPS startup params (--rnps-param k=v). Must be set before RunHbcBuffer*.
void SetRnpsLaunchParams(
    const std::vector<std::pair<std::string, std::string>>& params) {
	RnpsLaunchParams::Set(params);
	LOGF("\t [RNPS host] launch params: %zu entr%s\n", params.size(),
	     params.size() == 1 ? "y" : "ies");
	for (const auto& kv : params) {
		LOGF("\t   %s=\"%s\"\n", kv.first.c_str(), kv.second.c_str());
	}
}

// RNPS app identity for the TTPool mount path. Must be set before
// RunHbcBuffer* (the emulator RNPS block owns the manifest).
void SetRnpsAppInfo(const std::string& titleId, const std::string& appName,
                    const std::string& appVersion) {
	std::lock_guard<std::mutex> lock(Detail::g_appInfoMutex);
	Detail::g_appTitleId  = titleId;
	Detail::g_appName     = appName;
	Detail::g_appVersion  = appVersion;
	LOGF("\t [RNPS host] app info: %s (%s) %s\n", appName.c_str(),
	     titleId.c_str(), appVersion.c_str());
}

// Host-side one-shot for the RNPS --game path (see libHermesVm.h).
// Entry name is a static so the evaluate cache keys stably.
int RunHbcBuffer(const uint8_t* bytecode, uint64_t size) {
	static constexpr char kEntry[] = "rnps-main";
	uint64_t              handle   = 0;
	const int rcCreate = Vm::HermesVmCreate(bytecode, size, &handle);
	if (rcCreate != 0) {
		return rcCreate;
	}
	const int rcExec = Vm::HermesVmExecute(handle, kEntry);
	Vm::HermesVmDestroy(handle);
	return rcExec;
}

// Host-side resident service for the RNPS --game path with
// --rnps-resident <secs>: same one-shot evaluation, then the runtime stays
// alive pumping live ticks for `seconds` (event listeners re-fire, service
// stays observable) instead of quick_exit.
int RunHbcBufferLive(const uint8_t* bytecode, uint64_t size, uint64_t seconds) {
	static constexpr char kEntry[] = "rnps-main";
	uint64_t              handle   = 0;
	const int rcCreate = Vm::HermesVmCreate(bytecode, size, &handle);
	if (rcCreate != 0) {
		return rcCreate;
	}
	VmContext* ctx = nullptr;
	{
		// ps5-vsh: same lookup HermesVmExecute uses (mutex-guarded).
		ctx = Detail::FindContext(handle);
	}
	if (ctx == nullptr) {
		Vm::HermesVmDestroy(handle);
		return HERMES_VM_ERROR_ENCRYPTED;
	}
	ctx->liveSeconds = seconds;
	const int rcExec = Vm::HermesVmExecute(handle, kEntry);
	Vm::HermesVmDestroy(handle);
	return rcExec;
}

} // namespace HermesVmHost

} // namespace HermesVm

namespace SceJsi {

// JSI bridge into the Hermes scaffold. Provisional lib/module names (mirror
// libSceJsi-0.70.sprx); real JSI NIDs come from the libSceJsi-0.70.sprx export
// dump next round, so every key below is a clearly-marked LOCAL placeholder.
LIB_VERSION("SceJsi", 1, "SceJsi", 1, 1);

namespace Jsi {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsiCreateRuntime(uint64_t* outHandle) {
	PRINT_NAME();
	LOGF("\t outHandle = %p\n", static_cast<void*>(outHandle));
	if (outHandle == nullptr) {
		LOGF("\t null guest pointer, failing closed with 0x80940001\n");
		return HermesVm::HERMES_VM_ERROR_ENCRYPTED;
	}
	HermesVm::VmContext ctx;
	ctx.hasJsiRuntime = true;
	*outHandle       = HermesVm::Detail::RegisterContext(std::move(ctx));
	LOGF("\t JSI runtime handle = 0x%016" PRIx64 "\n", *outHandle);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsiDestroyRuntime(uint64_t handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", handle);
	std::lock_guard<std::mutex> lock(HermesVm::Detail::g_vmMutex);
	if (HermesVm::Detail::g_vmContexts.erase(handle) == 0) {
		LOGF("\t unknown handle, failing closed with 0x80940001\n");
		return HermesVm::HERMES_VM_ERROR_ENCRYPTED;
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsiEvaluate(uint64_t handle, const char* source, uint64_t sourceLen) {
	PRINT_NAME();
	if (source == nullptr || sourceLen == 0) {
		LOGF("\t handle = 0x%016" PRIx64 " null/empty source, failing closed "
		     "with 0x80940001\n",
		     handle);
		return HermesVm::HERMES_VM_ERROR_ENCRYPTED;
	}
	HermesVm::VmContext* ctx = HermesVm::Detail::FindContext(handle);
	if (ctx == nullptr) {
		LOGF("\t handle = 0x%016" PRIx64 " unknown handle, failing closed with "
		     "0x80940001\n",
		     handle);
		return HermesVm::HERMES_VM_ERROR_ENCRYPTED;
	}
	// Source-hash dedup: same FNV-1a over the source repeats the cached result
	// code without re-logging (real runtime cache behaviour).
	const uint64_t evalHash = HermesVm::Detail::Fnv1a64Str(source, sourceLen);
	if (ctx->hasCachedEval && ctx->cachedEvalHash == evalHash) {
		return ctx->cachedEvalResult;
	}
	LOGF("\t handle = 0x%016" PRIx64 " sourceLen = %" PRIu64
	     " fnv1a64 = 0x%016" PRIx64 " -> routes into HermesVmExecute, no VM yet "
	     "(0x80940002)\n",
	     handle, sourceLen, evalHash);
	ctx->execCount++;
	ctx->cachedEvalHash   = evalHash;
	ctx->cachedEvalResult = HermesVm::HERMES_VM_ERROR_NO_VM;
	ctx->hasCachedEval    = true;
	// NEXT: parse source via Hermes compiler (hermesc) or run precompiled HBC
	// from the RNPS bundle once facebook/hermes is vendored.
	return HermesVm::HERMES_VM_ERROR_NO_VM;
}

} // namespace Jsi

LIB_DEFINE(InitSceJsi_1) {
	// LOCAL: not FW NIDs — real JSI NIDs come from libSceJsi-0.70.sprx export dump next round.
	LIB_FUNC("LOCAL_JsiCreateRuntime", Jsi::JsiCreateRuntime);
	// LOCAL: not FW NIDs — real JSI NIDs come from libSceJsi-0.70.sprx export dump next round.
	LIB_FUNC("LOCAL_JsiDestroyRuntime", Jsi::JsiDestroyRuntime);
	// LOCAL: not FW NIDs — real JSI NIDs come from libSceJsi-0.70.sprx export dump next round.
	LIB_FUNC("LOCAL_JsiEvaluate", Jsi::JsiEvaluate);
}

} // namespace SceJsi

} // namespace Libs

// WIRED (dispatch turn): kyty_target_link_hermes(kyty_emulator) is called
// from the top-level CMakeLists after add_executable(kyty_emulator); the
// helper links libhermes+jsi, adds the proven include dirs, defines
// KYTY_USE_HERMES=1 (real makeHermesRuntime dispatch above), applies /EHsc
// under clang-cl (jsi throws), and stages hermes.dll next to the emulator.
// Proof 2026-10-05: scratch hermes-lib-e2e.exe ran hello.hbc (408 B, HBC
// v84) - print "hello hermes 1+2=3", LINK_RUN_OK, exit 0.
// Still open: per-context runtime reuse, and hbcdump validation against the
// decrypted NPXS40024 bundle.

// Orchestrator registration snippet for src/libs/libs.cpp (add exactly this):
//
//   namespace HermesVm {
//   LIB_DEFINE(InitHermesVm_1);
//   } // namespace HermesVm
//
//   namespace SceJsi {
//   LIB_DEFINE(InitSceJsi_1);
//   } // namespace SceJsi
//
// ... and inside InitAll(Loader::SymbolDatabase* s), alongside the other
// ps5-vsh entries:
//
//   HermesVm::InitHermesVm_1(s);
//   SceJsi::InitSceJsi_1(s);
