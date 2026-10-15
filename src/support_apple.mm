// macOS support helpers for SWE1Racer (Objective-C++)
//
// SDL's Metal view (SDL_cocoametalview) resizes the drawable of its backing
// CAMetalLayer every time the NSView's frame changes (-updateDrawableSize).
// RT64 owns the swap chain and sizes the drawable itself from the window's
// size, so SDL doing it too races with RT64 and can hand Metal a drawable
// whose size disagrees with what RT64 just configured. We neutralize SDL's
// version by swapping its implementation with a no-op at load time.
//
// Every lookup is guarded: if the SDL build in use names the class or method
// differently (or strips it), this quietly does nothing.

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

namespace {

void swe1r_noop_updateDrawableSize(id /*self*/, SEL /*_cmd*/) {
    // Intentionally empty: RT64 manages the CAMetalLayer drawable size.
}

void swe1r_disable_sdl_metal_view_resize() {
    Class view_class = objc_getClass("SDL_cocoametalview");
    if (view_class == nil) {
        return;
    }

    SEL target_sel = sel_registerName("updateDrawableSize");
    Method target_method = class_getInstanceMethod(view_class, target_sel);
    if (target_method == nullptr) {
        return;
    }

    const char* type_encoding = method_getTypeEncoding(target_method);
    if (type_encoding == nullptr) {
        return;
    }

    // Replace the implementation in place. class_replaceMethod returns the
    // previous IMP (which we discard) or NULL if it had to add the method;
    // either way the view now ignores drawable-size updates.
    class_replaceMethod(view_class, target_sel,
                        (IMP)swe1r_noop_updateDrawableSize, type_encoding);
}

} // namespace

// Runs when the executable is loaded, before main(), so the swizzle is in
// place before SDL creates any Metal view.
__attribute__((constructor))
static void swe1r_apple_support_init() {
    @autoreleasepool {
        swe1r_disable_sdl_metal_view_resize();
    }
}
