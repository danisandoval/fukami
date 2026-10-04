// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stable, PCSX2-header-free ABI for the optional live GS backend.
//
// This interface is intentionally owned by RRV-Recomp.  The implementation is
// built against PCSX2 2.8.2 (fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3), but consumers
// only load this dylib/shared library and must not link PCSX2 headers or types.
#ifndef RRV_PCSX2_GS_BRIDGE_H
#define RRV_PCSX2_GS_BRIDGE_H

#include <stdint.h>

#if defined(_WIN32) && defined(RRV_PCSX2_GS_BRIDGE_BUILD)
#define RRV_PCSX2_GS_BRIDGE_API __declspec(dllexport)
#elif defined(_WIN32)
#define RRV_PCSX2_GS_BRIDGE_API __declspec(dllimport)
#else
#define RRV_PCSX2_GS_BRIDGE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RRV_PCSX2_GS_BRIDGE_ABI_VERSION = 5,
    RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT = 19,
    RRV_PCSX2_GS_BRIDGE_LOCAL_MEMORY_BYTES = 4 * 1024 * 1024,
    RRV_PCSX2_GS_RENDER_MODE_FIELD = 0,
    RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME = 1,
    RRV_PCSX2_GS_RENDER_MODE_CAP_FIELD = 1u << RRV_PCSX2_GS_RENDER_MODE_FIELD,
    RRV_PCSX2_GS_RENDER_MODE_CAP_FULL_FRAME = 1u << RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME,
    RRV_PCSX2_GS_RENDERER_UNKNOWN = 0,
    RRV_PCSX2_GS_RENDERER_SOFTWARE = 1,
    RRV_PCSX2_GS_RENDERER_METAL = 2,
    RRV_PCSX2_GS_RENDERER_AUTO = 3,
    // Linux/Steam Deck (Gate 5). AUTO also resolves to Vulkan off macOS.
    RRV_PCSX2_GS_RENDERER_VULKAN = 4,
    // `presentation_mode` is a request, not a fallback preference. Direct
    // presentation must fail closed when these requirements cannot be met.
    RRV_PCSX2_GS_PRESENTATION_LEGACY_CPU_SNAPSHOT = 0,
    RRV_PCSX2_GS_PRESENTATION_DIRECT_GPU = 1,
    // Native objects stay opaque to portable callers. The macOS adapter is the
    // only bridge code which interprets SDL's view/layer as AppKit/Metal objects.
    RRV_PCSX2_GS_SURFACE_NONE = 0,
    RRV_PCSX2_GS_SURFACE_MACOS_VIEW_METAL_LAYER = 1,
    // Linux (Gate 5), from SDL_GetWindowWMInfo. X11: native_view = Display*,
    // native_layer = (void*)(uintptr_t)Window. Wayland: native_view =
    // wl_display*, native_layer = wl_surface*. Maps to PCSX2 WindowInfo.
    // Steam Deck Game Mode (gamescope) runs SDL on XWayland, i.e. X11.
    RRV_PCSX2_GS_SURFACE_LINUX_X11 = 2,
    RRV_PCSX2_GS_SURFACE_LINUX_WAYLAND = 3,
    // The SDL platform shell retains both handles from create through destroy.
    // The bridge borrows them and never transfers or exposes their ownership.
    RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES = 1u << 0,
    // The caller has prepared the SDL-owned surface on the platform main
    // thread. PCSX2 may synchronously marshal attach/detach to that thread; it
    // never calls back into RRV while the bridge's serialized execution lock is
    // held.
    RRV_PCSX2_GS_SURFACE_FLAG_MAIN_THREAD_PREPARED = 1u << 1,
    // Optional owner-thread sideband. Set only by prepare_owner_surface(),
    // after validation on main. The main thread must service the lifecycle
    // pump while waiting for owner create/destroy (PCSX2 attaches there).
    RRV_PCSX2_GS_SURFACE_FLAG_OWNER_THREAD = 1u << 2,
    RRV_PCSX2_GS_CAP_HARDWARE_GS = 1u << 0,
    RRV_PCSX2_GS_CAP_METAL_BACKEND = 1u << 1,
    RRV_PCSX2_GS_CAP_DIRECT_PRESENT = 1u << 2,
    RRV_PCSX2_GS_CAP_ON_DEMAND_CAPTURE = 1u << 3,
    RRV_PCSX2_GS_CAP_LEGACY_CPU_SNAPSHOT = 1u << 4,
    RRV_PCSX2_GS_CAP_VULKAN_BACKEND = 1u << 5,
};

typedef struct RrvPcsx2GsBridge RrvPcsx2GsBridge;

// Every structure starts with struct_size so later ABI revisions can append
// fields without changing the existing binary contract. A zero snapshot size
// asks PCSX2 to choose its native presentation size.
typedef struct RrvPcsx2GsBridgeSurface {
    uint32_t struct_size;
    uint32_t kind;  // RRV_PCSX2_GS_SURFACE_*
    uint32_t flags; // RRV_PCSX2_GS_SURFACE_FLAG_*
    uint32_t reserved;
    void* native_view;  // opaque; SDL_MetalView for the macOS direct-present kind
    void* native_layer; // opaque; result of SDL_Metal_GetLayer for that view
    uint32_t width_pixels;
    uint32_t height_pixels;
    float backing_scale;
    uint32_t reserved2;
} RrvPcsx2GsBridgeSurface;

typedef struct RrvPcsx2GsBridgeConfig {
    uint32_t struct_size;
    uint32_t snapshot_width;
    uint32_t snapshot_height;
    uint32_t sw_threads; // 0: PCSX2 Software renderer default.
    // rrv::gs::GsRenderMode's stable uint32_t value. The C ABI intentionally
    // does not expose the C++ policy header.
    uint32_t gs_render_mode;
    uint32_t presentation_mode; // RRV_PCSX2_GS_PRESENTATION_*
    // Explicit portable request. UNKNOWN retains the diagnostic environment
    // selector; product targets request METAL/AUTO through this field.
    uint32_t renderer_kind; // RRV_PCSX2_GS_RENDERER_*
    uint32_t reserved;
    // Required before GSopen when presentation_mode is DIRECT_GPU.
    RrvPcsx2GsBridgeSurface surface;
} RrvPcsx2GsBridgeConfig;

// Renderer-owned facts returned during create, including when create rejects a
// requested mode before GSopen. renderer_name is a stable bridge-owned string
// literal valid for the bridge library's lifetime.
typedef struct RrvPcsx2GsBridgeCapabilities {
    uint32_t struct_size;
    uint32_t supported_render_modes; // RRV_PCSX2_GS_RENDER_MODE_CAP_* bitset
    uint32_t renderer_kind;          // RRV_PCSX2_GS_RENDERER_*
    uint32_t capability_flags;       // RRV_PCSX2_GS_CAP_* bitset
    const char* renderer_name;
} RrvPcsx2GsBridgeCapabilities;

typedef struct RrvPcsx2GsBridgeFrame {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba; // valid until the next bridge call on this handle.
    uint64_t sequence;
} RrvPcsx2GsBridgeFrame;

// A capture is always an explicit diagnostic request. Tags identify the guest
// boundary selected by the caller; the bridge never schedules a later capture
// implicitly and never turns one request into continuous readback.
typedef struct RrvPcsx2GsBridgeCaptureRequest {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t guest_tick;
    uint64_t field_index;
    uint64_t present_index;
} RrvPcsx2GsBridgeCaptureRequest;

typedef struct RrvPcsx2GsBridgeCaptureResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t guest_tick;
    uint64_t field_index;
    uint64_t present_index;
    RrvPcsx2GsBridgeFrame frame;
} RrvPcsx2GsBridgeCaptureResult;

typedef struct RrvPcsx2GsBridgeStats {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t presented_frames;
    uint64_t direct_gpu_presents;
    uint64_t requested_captures;
    uint64_t completed_captures;
    uint64_t synchronous_cpu_readbacks;
    uint64_t unexpected_readbacks;
    uint64_t cpu_waits;
} RrvPcsx2GsBridgeStats;

// The 19 values are RRV's GSRegisters POD order, not PCSX2's padded layout:
// PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1,
// DISPFB2, DISPLAY2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, CSR, IMR, BUSDIR,
// SIGLBLID.  The bridge copies/scatters them into PCSX2's 0x2000 GSPrivRegSet.
typedef uint64_t RrvPcsx2GsPrivRegs[RRV_PCSX2_GS_BRIDGE_PRIV_REG_COUNT];

RRV_PCSX2_GS_BRIDGE_API uint32_t rrv_pcsx2_gs_bridge_version(void);

// Optional exports resolved by the dedicated-owner candidate. ABI 5 layouts
// and the default inline path are unchanged. All GS calls including create,
// resize and destroy must run serially on that one owner. The caller retains
// SDL handles until destroy completes and must not hold a lock needed by a
// main-thread lifecycle callback while pumping.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_prepare_owner_surface(
    RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity);
RRV_PCSX2_GS_BRIDGE_API void rrv_pcsx2_gs_bridge_owner_thread_init(void);
typedef void (*RrvPcsx2GsBridgeOwnerCommand)(void* context);
RRV_PCSX2_GS_BRIDGE_API void rrv_pcsx2_gs_bridge_owner_command(
    RrvPcsx2GsBridgeOwnerCommand command, void* context);
// Main-thread-only, bounded ~1 ms service of PCSX2's attach/detach dispatch.
// Use only while awaiting create/destroy, never to wait for ordinary GS work.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_pump_main_thread(void);

RRV_PCSX2_GS_BRIDGE_API RrvPcsx2GsBridge* rrv_pcsx2_gs_bridge_create(
    const RrvPcsx2GsBridgeConfig* config,
    RrvPcsx2GsBridgeCapabilities* capabilities,
    char* error, uint32_t error_capacity);
RRV_PCSX2_GS_BRIDGE_API void rrv_pcsx2_gs_bridge_destroy(RrvPcsx2GsBridge* bridge);

// Optional ABI-5 extension. Synchronous CSR soft reset, dispatched on the same
// owner as submit/vsync. Corresponds to pinned MTGS::ResetGS(false), not a GIF
// producer reset. Clears renderer/parser state, preserves GS local memory.
// Privileged registers continue to arrive via ordered producer snapshots.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_reset_gs(
    RrvPcsx2GsBridge* bridge, char* error, uint32_t error_capacity);

// path_id is the original RRV GifPathId (1, 2, or 3).  It is retained for
// boundary diagnostics.  The packet itself is already post-arbitration, so
// all three paths deliberately enter PCSX2's staged GSgifTransfer() API.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_submit(
    RrvPcsx2GsBridge* bridge, uint32_t path_id, const uint8_t* gif_bytes,
    uint32_t size_bytes, char* error, uint32_t error_capacity);

// Read guest-visible GS state after submit/vsync.  In particular, PCSX2's GIF
// decoder updates CSR SIGNAL/FINISH and SIGLBLID; callers must mirror those
// values into their own EE-visible GS register bank when using this backend.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_readback(
    RrvPcsx2GsBridge* bridge, uint64_t* csr, uint64_t* siglblid,
    char* error, uint32_t error_capacity);

// Reads an already-issued GS local-to-host transfer from PCSX2 local memory.
// The producer must have submitted BITBLTBUF/TRXPOS/TRXREG then TRXDIR=1 in
// guest order before this call. `byte_count` is the exact guest-visible byte
// count; the bridge pads its internal FIFO read to QWC and never writes those
// padding bytes to `dst`. The ABI retains the three descriptors for caller
// provenance/trace identity; this function never submits or acknowledges them.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_read_local_memory(
    RrvPcsx2GsBridge* bridge, uint8_t* dst, uint32_t byte_count,
    uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg,
    char* error, uint32_t error_capacity);

// Copy the complete, native-order 4 MiB GS local-memory image.  These calls
// are synchronous and must share the bridge's single serialized GS execution
// context with GIF submission and field transitions.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_snapshot_local_memory(
    RrvPcsx2GsBridge* bridge, uint8_t* dst, uint32_t byte_count,
    char* error, uint32_t error_capacity);
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_restore_local_memory(
    RrvPcsx2GsBridge* bridge, const uint8_t* src, uint32_t byte_count,
    char* error, uint32_t error_capacity);

// Scatter regs and submit exactly one GS field transition. Snapshot capture is
// deliberately separate and host-paced through rrv_pcsx2_gs_bridge_snapshot.
// `field_index` is a monotonic diagnostic/correlation value. `field_parity` is
// authoritative (must be 0 or 1): the bridge mirrors it into CSR.FIELD before
// the register scatter, then derives PCSX2's inverted GSvsync argument from
// that CSR bit, matching its MTGS convention at the pinned revision.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_vsync(
    RrvPcsx2GsBridge* bridge, const RrvPcsx2GsPrivRegs regs,
    uint64_t field_index, uint32_t field_parity, char* error, uint32_t error_capacity);

// Optional M2 diagnostic exports. They exist only when the bridge is built
// with RRV_PCSX2_GS_CANONICAL_RECEIPTS; the runtime resolves them dynamically
// so the stable ABI and production bridge remain unchanged in mode A.
#if defined(RRV_PCSX2_GS_CANONICAL_RECEIPTS)
// Fixed, non-canonical sideband evidence for a selector-edge live receipt
// boundary.  It is deliberately not part of receipt schema 1 or any epoch
// digest: it proves the arm placement, rather than becoming workload input.
// The bridge writes these values on its serialized synchronous GS path.
typedef struct RrvPcsx2GsBridgeReceiptBoundaryOrdering {
    uint32_t struct_size;
    uint32_t arm_scheduled;
    uint32_t armed_after_successful_vsync;
    uint32_t first_post_arm_consumer_event_type;
    uint64_t mixed_vsync_consumer_ack_ordinal;
    uint64_t arm_consumer_ack_ordinal;
    uint64_t bridge_vsync_return_consumer_ack_ordinal;
    uint64_t first_post_arm_consumer_ack_ordinal;
    uint64_t first_post_arm_transfer_ack_ordinal;
} RrvPcsx2GsBridgeReceiptBoundaryOrdering;

RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_receipt_arm(
    RrvPcsx2GsBridge* bridge, char* error, uint32_t error_capacity);
// Schedule a one-shot arm after the next *successful* GSvsync has closed the
// current field.  The actual arm executes before the synchronous bridge
// vsync wrapper returns, so the following transfer starts an empty collector.
// This does not alter receipt_arm(), whose immediate behavior is retained for
// the accepted immutable replay fixture.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_receipt_arm_after_vsync(
    RrvPcsx2GsBridge* bridge, char* error, uint32_t error_capacity);
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_receipt_boundary_ordering(
    RrvPcsx2GsBridge* bridge,
    RrvPcsx2GsBridgeReceiptBoundaryOrdering* ordering,
    char* error, uint32_t error_capacity);
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_receipt_set_epoch_identity(
    RrvPcsx2GsBridge* bridge, uint64_t guest_field_id, uint64_t gs_field_epoch_id,
    char* error, uint32_t error_capacity);
#endif

// Changes the drawable size of an already-created direct surface. Calls which
// touch a bridge handle (submit, vsync, resize, capture, destroy) are strictly
// serialized by RRV; neither side may destroy or mutate the supplied objects
// concurrently. For the macOS surface kind, create and resize must additionally
// run on the platform main thread unless prepare_owner_surface() enabled the
// dedicated-owner sideband. AppKit preparation remains on main in both modes.
// `destroy` remains the explicit shutdown operation.
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_resize(
    RrvPcsx2GsBridge* bridge, uint32_t width_pixels, uint32_t height_pixels,
    float backing_scale, char* error, uint32_t error_capacity);

// ---- Optional ABI-5 extension: host overlay (the Fukami in-game menu) -----
//
// The host registers one frame callback. On a direct-present bridge every
// rrv_pcsx2_gs_bridge_vsync() calls it on the serialized GS owner (the same
// thread as submit/vsync), immediately before GSvsync presents, with a table
// of Dear ImGui entry points that is valid only during that call. PCSX2 draws
// the resulting ImGui windows over the presented picture (GSDeviceMTL::
// EndPresent -> RenderImGui). With no callback registered (headless, a host
// without a menu, or a snapshot-only bridge) the bridge does no overlay work.
// Nothing here reads or writes GS registers, local memory, CSR or the GIF and
// field streams; the set_* options change presentation only.
enum {
    // Keys for RrvPcsx2GsBridgeUi.add_key (mapped to ImGuiKey_* in the bridge).
    RRV_PCSX2_GS_UI_KEY_TAB = 1,
    RRV_PCSX2_GS_UI_KEY_LEFT,
    RRV_PCSX2_GS_UI_KEY_RIGHT,
    RRV_PCSX2_GS_UI_KEY_UP,
    RRV_PCSX2_GS_UI_KEY_DOWN,
    RRV_PCSX2_GS_UI_KEY_PAGE_UP,
    RRV_PCSX2_GS_UI_KEY_PAGE_DOWN,
    RRV_PCSX2_GS_UI_KEY_HOME,
    RRV_PCSX2_GS_UI_KEY_END,
    RRV_PCSX2_GS_UI_KEY_SPACE,
    RRV_PCSX2_GS_UI_KEY_ENTER,
    RRV_PCSX2_GS_UI_KEY_ESCAPE,
    RRV_PCSX2_GS_UI_KEY_SHIFT,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_UP,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_DOWN,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_LEFT,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_RIGHT,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_DOWN,  // A / Cross: activate
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT, // B / Circle: cancel
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_LEFT,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_UP,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_L1,
    RRV_PCSX2_GS_UI_KEY_GAMEPAD_R1,
    RRV_PCSX2_GS_UI_KEY_COUNT,
};

typedef struct RrvPcsx2GsBridgeUi {
    uint32_t struct_size;
    uint32_t reserved;
    float display_width;  // drawable pixels
    float display_height; // drawable pixels
    float scale;          // PCSX2's ImGui scale (backing scale x OSD scale)
    float reserved2;
    // Input. Events queue into Dear ImGui and take effect on its next frame.
    void (*add_key)(uint32_t key, int down);
    void (*add_mouse_position)(float x_pixels, float y_pixels);
    void (*add_mouse_button)(uint32_t button, int down); // 0 left, 1 right, 2 middle
    void (*add_mouse_wheel)(float x, float y);
    void (*clear_input)(void);
    // 1 while Esc / B belong to ImGui: a combo (or other popup) is open or a
    // widget is being edited (a slider activated from the keyboard/pad).
    int (*consumes_cancel)(void);
    // Layout. begin_menu draws a dimmed backdrop and a centered window.
    // `appearing` != 0 on the first frame after the host opened the menu.
    // Like ImGui::Begin/End, call end_menu() whatever begin_menu() returned.
    int (*begin_menu)(const char* title, int appearing);
    void (*end_menu)(void);
    void (*section)(const char* label);
    void (*text)(const char* text);
    void (*text_disabled)(const char* text);
    void (*text_wrapped)(const char* text);
    void (*text_warning)(const char* text);
    void (*separator)(void);
    void (*same_line)(void);
    void (*spacing)(void);
    void (*push_id)(int id);
    void (*pop_id)(void);
    void (*begin_disabled)(int disabled);
    void (*end_disabled)(void);
    void (*tooltip)(const char* text); // for the previous item
    void (*default_focus)(void);       // previous item gets nav focus on appearing
    // Labelled rows (label left, control right). Return 1 when the value
    // changed / the button was activated this frame. A row's identity is the
    // enclosing push_id() (the label is display text only), so wrap each row
    // in push_id/pop_id; buttons are identified by their label.
    int (*checkbox)(const char* label, int* value);
    int (*combo)(const char* label, int* index, const char* const* items, int count);
    int (*slider_int)(const char* label, int* value, int min, int max);
    int (*slider_float)(const char* label, float* value, float min, float max, const char* format);
    int (*button)(const char* label);
    // Presentation-only options, applied on this owner thread between fields.
    // set_aspect takes auto|4:3|16:9|16:10|21:9|stretch; returns 0 if invalid.
    int (*set_aspect)(const char* value);
    void (*set_integer_scaling)(int enabled);
    void (*set_fxaa)(int enabled);
    int (*set_cas)(int sharpness); // 0 = off, 1..100
} RrvPcsx2GsBridgeUi;

typedef void (*RrvPcsx2GsBridgeOverlayFrame)(void* context, const RrvPcsx2GsBridgeUi* ui);
// Registers the host overlay, or clears it with frame == NULL. Thread-safe.
// Clearing waits for an in-flight frame callback, so the host may release
// `context` once this returns.
RRV_PCSX2_GS_BRIDGE_API void rrv_pcsx2_gs_bridge_set_overlay(
    RrvPcsx2GsBridgeOverlayFrame frame, void* context);

RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_snapshot(
    RrvPcsx2GsBridge* bridge, RrvPcsx2GsBridgeFrame* frame,
    char* error, uint32_t error_capacity);

RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_capture(
    RrvPcsx2GsBridge* bridge, const RrvPcsx2GsBridgeCaptureRequest* request,
    RrvPcsx2GsBridgeCaptureResult* result, char* error, uint32_t error_capacity);
RRV_PCSX2_GS_BRIDGE_API int rrv_pcsx2_gs_bridge_get_stats(
    RrvPcsx2GsBridge* bridge, RrvPcsx2GsBridgeStats* stats,
    char* error, uint32_t error_capacity);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RRV_PCSX2_GS_BRIDGE_H
