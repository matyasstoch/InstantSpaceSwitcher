#include "include/ISS.h"
#include "event_serialize.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGEventTypes.h>
#include <assert.h>
#include <dlfcn.h>
#include <float.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const CGEventField kCGSEventTypeField = (CGEventField)55;
static const CGEventField kCGEventGestureHIDType = (CGEventField)110;
static const CGEventField kCGEventGestureSwipeMotion = (CGEventField)123;
static const CGEventField kCGEventGestureSwipeProgress = (CGEventField)124;
static const CGEventField kCGEventGestureSwipePositionX = (CGEventField)125;
static const CGEventField kCGEventGestureSwipeVelocityX = (CGEventField)129;
static const CGEventField kCGEventGestureSwipeVelocityY = (CGEventField)130;
static const CGEventField kCGEventGesturePhase = (CGEventField)132;
static const CGEventField kCGEventGesturePhaseAlias = (CGEventField)134;
static const CGEventField kCGEventGestureZoomDeltaY = (CGEventField)138;
static const CGEventField kCGEventSourceUnixProcessIDAlias = (CGEventField)169;

// See IOHIDEventType enum in IOHIDFamily
static const uint32_t kIOHIDEventTypeDockSwipe = 23;

typedef uint32_t CGSEventType;
enum {
    kCGSEventScrollWheel = 22,
    kCGSEventZoom = 28,
    kCGSEventGesture = 29,
    kCGSEventDockControl = 30,
    kCGSEventFluidTouchGesture = 31,
};

typedef CF_ENUM(uint8_t, CGSGesturePhase) {
    kCGSGesturePhaseNone = 0,
    kCGSGesturePhaseBegan = 1,
    kCGSGesturePhaseChanged = 2,
    kCGSGesturePhaseEnded = 4,
    kCGSGesturePhaseCancelled = 8,
    kCGSGesturePhaseMayBegin = 128,
};

// Limited subset of motion constants observed in synthetic Dock swipe traces.
typedef CF_ENUM(uint16_t, CGGestureMotion) {
    kCGGestureMotionHorizontal = 1,
};

typedef int32_t CGSConnectionID;
typedef uint64_t CGSSpaceID;

extern CFArrayRef CGSCopyManagedDisplaySpaces(CGSConnectionID connection, CFStringRef display) __attribute__((weak_import));
extern CFStringRef CGSCopyActiveMenuBarDisplayIdentifier(CGSConnectionID connection) __attribute__((weak_import));
extern CGSConnectionID CGSMainConnectionID(void) __attribute__((weak_import));
extern CGSSpaceID CGSGetActiveSpace(CGSConnectionID connection) __attribute__((weak_import));
extern CFArrayRef CGSCopySpacesForWindows(CGSConnectionID connection, int mask, CFArrayRef windowIDs) __attribute__((weak_import));

static CFMachPortRef globalTap = NULL;
static CFRunLoopSourceRef globalSource = NULL;

// Overlay detection state
static bool overlayDetectionEnabled = false;

// Swipe override state
static bool swipeOverrideEnabled = false;
static bool swipeTracking = false;
static bool swipeFired = false;

// Our macOS 27 swipes re-enter the tap with sourcePid == 0, so count them out instead
static unsigned int syntheticEventsToPassThrough = 0;

// Gesture speed state
static double gestureSpeed = 2000.0;

static ISSSwitchCallback switchCallback = NULL;

// Predictions dictionary: DisplayID (CFStringRef) -> Index (CFNumberRef)
static CFMutableDictionaryRef predictionsDict = NULL;

// While our own gestures are still being animated, CGS reports an
// intermediate space and the Dock fires space-change notifications for each
// hop. Trusting either during a burst makes us overshoot past the last space,
// which on macOS 27 wedges the Dock (black screen) until it times out. Keep
// predictions authoritative until the Dock has been quiet for this long.
static const CFTimeInterval kPredictionTTL = 1.0;
static CFAbsoluteTime lastGestureTime = 0;

static bool gestures_in_flight(void) {
    return CFAbsoluteTimeGetCurrent() - lastGestureTime < kPredictionTTL;
}

static bool get_prediction(const char *displayID, unsigned int *outIndex) {
    if (!displayID || !predictionsDict) {
        return false;
    }
    if (!gestures_in_flight()) {
        CFDictionaryRemoveAllValues(predictionsDict);
        return false;
    }
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    const void *value = CFDictionaryGetValue(predictionsDict, key);
    CFRelease(key);

    if (value) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, outIndex);
        return true;
    }
    return false;
}

static void set_prediction(const char *displayID, unsigned int index) {
    if (!displayID || !predictionsDict) {
        return;
    }
    
    CFStringRef key = CFStringCreateWithCString(NULL, displayID, kCFStringEncodingUTF8);
    CFNumberRef val = CFNumberCreate(NULL, kCFNumberIntType, &index);
    CFDictionarySetValue(predictionsDict, key, val);
    CFRelease(key);
    CFRelease(val);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo);
static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay);
static bool iss_perform_switch_gesture(ISSDirection direction, double velocity);
static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction);
static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction);

static ISSDirection iss_direction_from_gesture_value(double value) {
    // Map the intercepted gesture value to the app's logical direction.
    return value > 0.0 ? ISSDirectionRight : ISSDirectionLeft;
}

// Perform a swipe-override switch: get space info, compute target, switch,
// and notify the handler with the target index.
static void swipe_override_switch(ISSDirection dir) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        iss_perform_switch_gesture(dir, gestureSpeed);
        return;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
    unsigned int target = dir == ISSDirectionLeft ? current - 1 : current + 1;

    if (iss_switch_with_info(&info, dir)) {
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
    }
}

static CGEventRef eventTapCallback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;

    // Re-enable if the system disabled our tap for being too slow
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (globalTap) CGEventTapEnable(globalTap, true);
        // Anything we posted while disabled never reaches us, so stop waiting for it
        syntheticEventsToPassThrough = 0;
        return event;
    }

    CGSEventType eventType =
        (CGSEventType)CGEventGetIntegerValueField(event, kCGSEventTypeField);

    // Our own events come back within milliseconds; a count left over after the
    // burst would let real trackpad swipes slip past the override
    if (syntheticEventsToPassThrough > 0 && !gestures_in_flight()) {
        syntheticEventsToPassThrough = 0;
    }

    if (syntheticEventsToPassThrough > 0 &&
        (eventType == kCGSEventDockControl || eventType == kCGSEventGesture)) {
        syntheticEventsToPassThrough--;
        return event;
    }

    if (!swipeOverrideEnabled) return event;

    // Pass through synthetic events (non-HID source). Real gesture events
    // from the trackpad have sourcePid == 0 (HID kernel).
    if (eventType == kCGSEventDockControl || eventType == kCGSEventGesture) {
        pid_t sourcePid = (pid_t)CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
        if (sourcePid != 0) return event;
    }

    if (eventType == kCGSEventDockControl) {
        uint32_t hidType =
            (uint32_t)CGEventGetIntegerValueField(event, kCGEventGestureHIDType);
        if (hidType != kIOHIDEventTypeDockSwipe) return event;

        uint16_t motion =
            (uint16_t)CGEventGetIntegerValueField(event, kCGEventGestureSwipeMotion);
        if (motion != kCGGestureMotionHorizontal) return event;

        CGSGesturePhase phase =
            (CGSGesturePhase)CGEventGetIntegerValueField(event, kCGEventGesturePhase);

        switch (phase) {
        case kCGSGesturePhaseBegan:
            if (iss_is_expose_active()) return event;
            swipeTracking = true;
            swipeFired = false;
            return NULL;

        case kCGSGesturePhaseChanged: {
            if (!swipeTracking) return event;
            if (!swipeFired) {
                double progress =
                    CGEventGetDoubleValueField(event, kCGEventGestureSwipeProgress);
                if (progress != 0.0) {
                    ISSDirection dir = iss_direction_from_gesture_value(progress);
                    swipeFired = true;
                    swipe_override_switch(dir);
                }
            }
            return NULL;
        }

        case kCGSGesturePhaseEnded: {
            if (!swipeTracking) return event;
            if (!swipeFired) {
                double velocity =
                    CGEventGetDoubleValueField(event, kCGEventGestureSwipeVelocityX);
                if (velocity != 0.0) {
                    ISSDirection dir = iss_direction_from_gesture_value(velocity);
                    swipeFired = true;
                    swipe_override_switch(dir);
                }
            }
            swipeTracking = false;
            swipeFired = false;
            if (iss_requires_event_augmentation()) {
                // On macOS 27 the Dock needs the real ended event to close its gesture
                // state; zero the motion so it doesn't switch a second time
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeProgress, 0.0);
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityX, 0.0);
                CGEventSetDoubleValueField(event, kCGEventGestureSwipeVelocityY, 0.0);
                return event;
            }
            return NULL;
        }

        case kCGSGesturePhaseCancelled:
            swipeTracking = false;
            swipeFired = false;
            return NULL;

        default:
            return swipeTracking ? NULL : event;
        }
    }

    // Suppress companion gesture events during active swipe tracking
    if (eventType == kCGSEventGesture && swipeTracking) {
        return NULL;
    }

    return event;
}

static bool cgs_symbols_available(void) {
    return (&CGSMainConnectionID != NULL) &&
           (&CGSGetActiveSpace != NULL) &&
           (&CGSCopyManagedDisplaySpaces != NULL);
}

static bool extract_space_info_from_display(CFDictionaryRef displayDict,
                                            CGSSpaceID activeSpace,
                                            bool hasActiveSpace,
                                            ISSSpaceInfo *outInfo) {
    if (!displayDict || !outInfo) {
        return false;
    }

    memset(outInfo->displayID, 0, sizeof(outInfo->displayID));
    CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
    if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID()) {
        CFStringGetCString(identifier, outInfo->displayID, sizeof(outInfo->displayID), kCFStringEncodingUTF8);
    }

    const void *spacesValue = CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
    if (!spacesValue || CFGetTypeID(spacesValue) != CFArrayGetTypeID()) {
        return false;
    }

    // Try to get current space from display dict (more accurate per-display)
    CGSSpaceID displayActiveSpace = 0;
    const void *currentSpaceValue = CFDictionaryGetValue(displayDict, CFSTR("Current Space"));
    if (currentSpaceValue && CFGetTypeID(currentSpaceValue) == CFDictionaryGetTypeID()) {
        CFDictionaryRef currentSpaceDict = (CFDictionaryRef)currentSpaceValue;
        CFNumberRef currentSpaceID = (CFNumberRef)CFDictionaryGetValue(currentSpaceDict, CFSTR("id64"));
        if (currentSpaceID && CFGetTypeID(currentSpaceID) == CFNumberGetTypeID()) {
            CFNumberGetValue(currentSpaceID, kCFNumberSInt64Type, &displayActiveSpace);
        }
    }
    
    // Use display-specific active space if available, otherwise use global
    CGSSpaceID targetActiveSpace = displayActiveSpace != 0 ? displayActiveSpace : activeSpace;
    bool hasTargetActiveSpace = displayActiveSpace != 0 || hasActiveSpace;

    CFArrayRef spaces = (CFArrayRef)spacesValue;
    const CFIndex spaceCount = CFArrayGetCount(spaces);

    unsigned int totalSpaces = 0;
    unsigned int activeIndex = 0;
    bool foundActive = false;

    for (CFIndex i = 0; i < spaceCount; i++) {
        const void *spaceValue = CFArrayGetValueAtIndex(spaces, i);
        if (!spaceValue || CFGetTypeID(spaceValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef spaceDict = (CFDictionaryRef)spaceValue;
        CFNumberRef idNumber = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
        if (!idNumber || CFGetTypeID(idNumber) != CFNumberGetTypeID()) {
            continue;
        }

        CGSSpaceID candidate = 0;
        if (CFNumberGetValue(idNumber, kCFNumberSInt64Type, &candidate)) {
            if (!foundActive && hasTargetActiveSpace && candidate == targetActiveSpace) {
                activeIndex = totalSpaces;
                foundActive = true;
            }
            totalSpaces++;
        }
    }

    if (totalSpaces == 0 || (hasTargetActiveSpace && !foundActive)) {
        return false;
    }

    outInfo->spaceCount = totalSpaces;
    outInfo->currentIndex = foundActive ? activeIndex : 0;
    return true;
}

static bool load_space_info_for_display(ISSSpaceInfo *info, bool useCursorDisplay) {
    if (!cgs_symbols_available()) {
        fprintf(stderr, "ISS: required CGS symbols missing\n");
        return false;
    }

    CGSConnectionID connection = CGSMainConnectionID();
    if (connection == 0) {
        fprintf(stderr, "ISS: CGSMainConnectionID returned 0\n");
        return false;
    }

    CGSSpaceID activeSpace = 0;
    bool hasActiveSpace = false;
    if (&CGSGetActiveSpace != NULL) {
        activeSpace = CGSGetActiveSpace(connection);
        if (activeSpace != 0) {
            hasActiveSpace = true;
        } else {
            fprintf(stderr, "ISS: CGSGetActiveSpace returned 0\n");
            return false;
        }
    }

    // Get display identifier based on mode
    CFStringRef activeDisplayIdentifier = NULL;
    
    if (useCursorDisplay) {
        // Get display where cursor is located
        CGEventRef tempEvent = CGEventCreate(NULL);
        if (tempEvent) {
            CGPoint cursorLocation = CGEventGetLocation(tempEvent);
            CFRelease(tempEvent);

            CGDirectDisplayID cursorDisplay = 0;
            uint32_t cursorDisplayCount = 0;

            if (CGGetDisplaysWithPoint(cursorLocation, 1, &cursorDisplay, &cursorDisplayCount) == kCGErrorSuccess && cursorDisplayCount > 0) {
                CFUUIDRef displayUUID = CGDisplayCreateUUIDFromDisplayID(cursorDisplay);
                if (displayUUID) {
                    activeDisplayIdentifier = CFUUIDCreateString(NULL, displayUUID);
                    CFRelease(displayUUID);
                }
            }
        }
    } else {
        // Get menubar display
        if (&CGSCopyActiveMenuBarDisplayIdentifier != NULL) {
            activeDisplayIdentifier = CGSCopyActiveMenuBarDisplayIdentifier(connection);
        }
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, activeDisplayIdentifier);
    if (!displays && activeDisplayIdentifier) {
        displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    }
    if (!displays) {
        if (activeDisplayIdentifier) {
            CFRelease(activeDisplayIdentifier);
        }
        return false;
    }

    const CFIndex displayCount = CFArrayGetCount(displays);
    CFDictionaryRef targetDisplay = NULL;
    CFDictionaryRef fallbackDisplay = NULL;

    for (CFIndex i = 0; i < displayCount; i++) {
        const void *displayValue = CFArrayGetValueAtIndex(displays, i);
        if (!displayValue || CFGetTypeID(displayValue) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFDictionaryRef displayDict = (CFDictionaryRef)displayValue;

        if (!fallbackDisplay) {
            fallbackDisplay = displayDict;
        }

        if (!activeDisplayIdentifier || targetDisplay) {
            continue;
        }

        CFStringRef identifier = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
        if (identifier && CFGetTypeID(identifier) == CFStringGetTypeID() && CFEqual(identifier, activeDisplayIdentifier)) {
            targetDisplay = displayDict;
        }
    }

    if (!targetDisplay) {
        targetDisplay = fallbackDisplay;
    }

    bool success = false;
    if (targetDisplay) {
        success = extract_space_info_from_display(targetDisplay, activeSpace, hasActiveSpace, info);
    }

    if (activeDisplayIdentifier) {
        CFRelease(activeDisplayIdentifier);
    }
    CFRelease(displays);

    return success;
}

static bool iss_should_block_switch(const ISSSpaceInfo *info, ISSDirection direction) {
    if (!info) {
        return false;
    }
    if (info->spaceCount == 0) {
        return true;
    }

    unsigned int predicted;
    unsigned int current = get_prediction(info->displayID, &predicted) ? predicted : info->currentIndex;

    if (direction == ISSDirectionLeft) {
        return current == 0;
    }

    return current + 1 >= info->spaceCount;
}

bool iss_can_move(ISSSpaceInfo info, ISSDirection direction) {
    return !iss_should_block_switch(&info, direction);
}

/** @brief Minimal signed swipe progress for every gesture phase.
 *
 * A small nonzero value minimizes visible travel while retaining direction;
 * velocity still commits the gesture.
 */
double iss_swipe_progress_for_phase(CGSGesturePhase phase, ISSDirection direction) {
    (void)phase;
    const bool isRight = (direction == ISSDirectionRight);
    const double magnitude = 0.000016;
    return isRight ? magnitude : -magnitude;
}

// The fling on the ended phase is what commits the switch on macOS 27. The payload
// field is 16.16 fixed point (max ~32767), so the speed setting isn't reused here.
static const double kModernFlingVelocity = 9999.0;

// Cached natural scrolling preference; -1 means re-read it
static int naturalScrolling = -1;

// Posted by System Settings when the natural scrolling checkbox is toggled
static void natural_scrolling_changed(CFNotificationCenterRef center, void *observer,
                                      CFNotificationName name, const void *object,
                                      CFDictionaryRef userInfo) {
    (void)center;
    (void)observer;
    (void)name;
    (void)object;
    (void)userInfo;
    CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
    naturalScrolling = -1;
}

// On macOS 27 right is negative, and the Dock reads the posted sign through the
// natural scrolling preference, so it flips again with natural scrolling off.
static double iss_modern_swipe_sign(ISSDirection direction) {
    if (naturalScrolling < 0) {
        Boolean valid = false;
        Boolean natural = CFPreferencesGetAppBooleanValue(CFSTR("com.apple.swipescrolldirection"),
                                                          kCFPreferencesAnyApplication, &valid);
        naturalScrolling = (!valid || natural) ? 1 : 0;
    }
    const double sign = (direction == ISSDirectionRight) ? -1.0 : 1.0;
    return naturalScrolling ? sign : -sign;
}

static bool iss_post_modern_dock_swipe(CGSGesturePhase phase, ISSDirection direction) {
    const double sign = iss_modern_swipe_sign(direction);

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    // Near-zero progress (the smallest non-zero 16.16 value) leaves nothing to animate.
    // Full progress switches too, but the previous space flashes before it lands.
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, sign * 0.000016);
    // Fields a real trackpad swipe carries (from FasterSwiper's traces).
    CGEventSetIntegerValueField(ev, kCGEventGesturePhaseAlias, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureZoomDeltaY, 3.0);
    CGEventSetDoubleValueField(ev, kCGEventSourceUnixProcessIDAlias, (double)mach_absolute_time());
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipePositionX, 0.1);
    // Match FasterSwiper: only the Ended event carries velocity.
    if (phase == kCGSGesturePhaseEnded) {
        CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, sign * kModernFlingVelocity);
    }

    CGEventRef augmented = iss_augment_dock_swipe_event(ev);
    CFRelease(ev);
    if (!augmented) {
        return false;
    }

    // The Dock ignores a DockControl event without a companion gesture event
    CGEventRef companion = CGEventCreate(NULL);
    if (!companion) {
        CFRelease(augmented);
        return false;
    }
    CGEventSetIntegerValueField(companion, kCGSEventTypeField, kCGSEventGesture);

    if (globalTap) {
        syntheticEventsToPassThrough += 2;
    }
    CGEventPost(kCGSessionEventTap, augmented);
    CGEventPost(kCGSessionEventTap, companion);
    CFRelease(augmented);
    CFRelease(companion);
    return true;
}

static bool iss_post_dock_swipe(CGSGesturePhase phase, ISSDirection direction, double velocity) {
    if (iss_requires_event_augmentation()) {
        return iss_post_modern_dock_swipe(phase, direction);
    }

    const bool isRight = (direction == ISSDirectionRight);
    const double progress = iss_swipe_progress_for_phase(phase, direction);

    // Velocity of gesture based on speed setting
    const double vel = isRight ? velocity : -velocity;

    CGEventRef ev = CGEventCreate(NULL);
    if (!ev) {
        return false;
    }
    CGEventSetIntegerValueField(ev, kCGSEventTypeField, kCGSEventDockControl);
    CGEventSetIntegerValueField(ev, kCGEventGestureHIDType, kIOHIDEventTypeDockSwipe);
    CGEventSetIntegerValueField(ev, kCGEventGesturePhase, phase);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeProgress, progress);
    CGEventSetIntegerValueField(ev, kCGEventGestureSwipeMotion, kCGGestureMotionHorizontal);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityX, vel);
    CGEventSetDoubleValueField(ev, kCGEventGestureSwipeVelocityY, vel);
    CGEventPost(kCGSessionEventTap, ev);
    CFRelease(ev);
    return true;
}

static bool iss_perform_switch_gesture(ISSDirection direction, double velocity) {
    // Send three gesture events--began, changed, and ended
    // If we only send two then mission control doesn't work.
    lastGestureTime = CFAbsoluteTimeGetCurrent();
    return iss_post_dock_swipe(kCGSGesturePhaseBegan,   direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseChanged, direction, velocity)
        && iss_post_dock_swipe(kCGSGesturePhaseEnded,   direction, velocity);
}

/** @brief Walks a CGWindowListCopyWindowInfo result
 *
 * Used for trying to determine if Exposé or Mission Control is active.
 *
 * @param windowList The window list to scan
 * @param outLayer18Count The count of layer-18 windows
 * @param outLayer20Count The count of layer-20 windows
 */
static void scan_dock_window_list(CFArrayRef windowList,
                                  int *outLayer18Count,
                                  int *outLayer20Count) {
    *outLayer18Count = 0;
    *outLayer20Count = 0;
    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(info, CFSTR("kCGWindowOwnerName"));
        if (!owner || !CFEqual(owner, CFSTR("Dock"))) continue;
        int layer = 0;
        CFNumberRef layerNum = (CFNumberRef)CFDictionaryGetValue(info, CFSTR("kCGWindowLayer"));
        if (layerNum) {
            CFNumberGetValue(layerNum, kCFNumberIntType, &layer);
        }
        if (layer == 18) {
            (*outLayer18Count)++;
            continue;
        }
        if (layer == 20) {
            (*outLayer20Count)++;
        }
    }
}

// Testable helpers
bool iss_is_expose_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // App Exposé: layer-18 present, at least one layer-20, AND count(layer=20) <= count(layer=18)
    return layer18Count > 0 && layer20Count > 0 && layer20Count <= layer18Count;
}

bool iss_is_mission_control_detected_in_window_list(CFArrayRef windowList) {
    int layer18Count = 0;
    int layer20Count = 0;
    scan_dock_window_list(windowList, &layer18Count, &layer20Count);
    // Mission Control: layer-18 present AND count(layer=20) > count(layer=18)
    return layer18Count > 0 && layer20Count > layer18Count;
}

/// Returns true when App Exposé is active (1-2 layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_expose_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_expose_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

/// Returns true when Mission Control is active (3+ layer-20 windows)
/// This heuristic is empirical and may not work in all cases.
bool iss_is_mission_control_active(void) {
    if (!overlayDetectionEnabled) return false;
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return false;
    bool result = iss_is_mission_control_detected_in_window_list(windowList);
    CFRelease(windowList);
    return result;
}

void iss_set_overlay_detection_enabled(bool enabled) {
    overlayDetectionEnabled = enabled;
}

bool iss_init(void) {
    if (globalTap) {
        return true;
    }

    if (!predictionsDict) {
        predictionsDict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp)
        | (1ULL << kCGSEventGesture) | (1ULL << kCGSEventDockControl);
    globalTap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        eventTapCallback,
        NULL
    );

    if (!globalTap) {
        return false;
    }

    globalSource = CFMachPortCreateRunLoopSource(NULL, globalTap, 0);
    if (!globalSource) {
        CFRelease(globalTap);
        globalTap = NULL;
        return false;
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
    CGEventTapEnable(globalTap, true);

    naturalScrolling = -1;
    CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(),
                                    &naturalScrolling, natural_scrolling_changed,
                                    CFSTR("SwipeScrollDirectionDidChangeNotification"),
                                    NULL, CFNotificationSuspensionBehaviorDeliverImmediately);

    return true;
}

void iss_destroy(void) {
    syntheticEventsToPassThrough = 0;
    if (predictionsDict) {
        CFRelease(predictionsDict);
        predictionsDict = NULL;
    }
    if (globalTap) {
        CFNotificationCenterRemoveObserver(CFNotificationCenterGetDistributedCenter(),
                                           &naturalScrolling,
                                           CFSTR("SwipeScrollDirectionDidChangeNotification"),
                                           NULL);
        CGEventTapEnable(globalTap, false);
        if (globalSource) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), globalSource, kCFRunLoopCommonModes);
            CFRelease(globalSource);
            globalSource = NULL;
        }
        CFRelease(globalTap);
        globalTap = NULL;
    }
}

bool iss_get_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, true);
}

bool iss_get_menubar_space_info(ISSSpaceInfo *info) {
    if (!info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    return load_space_info_for_display(info, false);
}

static bool iss_switch_with_info(const ISSSpaceInfo *info, ISSDirection direction) {
    if (iss_should_block_switch(info, direction)) {
        return false;
    }
    if (!iss_perform_switch_gesture(direction, gestureSpeed)) {
        return false;
    }

    return true;
}

bool iss_switch(ISSDirection direction) {
    ISSSpaceInfo info;
    if (iss_get_space_info(&info)) {
        unsigned int predicted;
        unsigned int current = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;
        unsigned int target = direction == ISSDirectionLeft ? current - 1 : current + 1;

        if (!iss_switch_with_info(&info, direction)) {
            return false;
        }
        set_prediction(info.displayID, target);
        if (switchCallback) { switchCallback(target); }
        return true;
    }

    return iss_perform_switch_gesture(direction, gestureSpeed);
}

bool iss_switch_to_index(unsigned int targetIndex) {
    ISSSpaceInfo info;
    if (!iss_get_space_info(&info)) {
        return false;
    }

    assert(info.spaceCount > 0);

    bool outOfBounds = targetIndex >= info.spaceCount;
    if (outOfBounds) {
        targetIndex = info.spaceCount - 1;
    }

    unsigned int predicted;
    unsigned int currentIndex = get_prediction(info.displayID, &predicted) ? predicted : info.currentIndex;

    if (currentIndex == targetIndex) {
        return !outOfBounds;
    }

    ISSDirection direction = currentIndex < targetIndex ? ISSDirectionRight : ISSDirectionLeft;
    unsigned int steps = direction == ISSDirectionRight ? (targetIndex - currentIndex) : (currentIndex - targetIndex);

    // Multiply velocity by number of steps for faster multi-space switching
    double velocity = gestureSpeed * steps;

    for (unsigned int i = 0; i < steps; i++) {
        if (!iss_perform_switch_gesture(direction, velocity)) {
            return false;
        }
    }

    set_prediction(info.displayID, targetIndex);
    if (switchCallback) { switchCallback(targetIndex); }
    return !outOfBounds;
}

void iss_set_swipe_override(bool enabled) {
    swipeOverrideEnabled = enabled;
    if (!enabled) {
        swipeTracking = false;
        swipeFired = false;
    }
}

void iss_set_gesture_speed(double speed) {
    gestureSpeed = speed;
}

void iss_reset_predictions(void) {
    // Space-change notifications fire for every intermediate hop of our own
    // gestures; ignore them until the burst has settled.
    if (gestures_in_flight()) return;
    if (predictionsDict) {
        CFDictionaryRemoveAllValues(predictionsDict);
    }
}

void iss_set_switch_callback(ISSSwitchCallback callback) {
    switchCallback = callback;
}

bool iss_get_space_index_for_pid(pid_t pid, unsigned int currentSpaceIndex, unsigned int *outIndex) {
    if (!outIndex || !cgs_symbols_available()) {
        return false;
    }
    if (&CGSCopySpacesForWindows == NULL) {
        return false;
    }

    // Collect CGWindowID values for all normal (layer 0) windows owned by this PID.
    CFArrayRef allWindowInfo = CGWindowListCopyWindowInfo(
        kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!allWindowInfo) {
        return false;
    }

    CFMutableArrayRef windowIDs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!windowIDs) {
        CFRelease(allWindowInfo);
        return false;
    }

    const CFIndex windowCount = CFArrayGetCount(allWindowInfo);
    for (CFIndex i = 0; i < windowCount; i++) {
        CFDictionaryRef winInfo = (CFDictionaryRef)CFArrayGetValueAtIndex(allWindowInfo, i);
        if (!winInfo || CFGetTypeID(winInfo) != CFDictionaryGetTypeID()) {
            continue;
        }

        CFNumberRef ownerPIDNum = (CFNumberRef)CFDictionaryGetValue(winInfo, kCGWindowOwnerPID);
        if (!ownerPIDNum) {
            continue;
        }
        int32_t windowPID = 0;
        CFNumberGetValue(ownerPIDNum, kCFNumberSInt32Type, &windowPID);
        if ((pid_t)windowPID != pid) {
            continue;
        }

        // Only consider normal (layer 0) windows to avoid menu-bar / HUD / overlay windows.
        CFNumberRef layerNum = (CFNumberRef)CFDictionaryGetValue(winInfo, kCGWindowLayer);
        if (layerNum) {
            int32_t layer = 0;
            CFNumberGetValue(layerNum, kCFNumberSInt32Type, &layer);
            if (layer != 0) {
                continue;
            }
        }

        CFNumberRef windowIDNum = (CFNumberRef)CFDictionaryGetValue(winInfo, kCGWindowNumber);
        if (windowIDNum) {
            CFArrayAppendValue(windowIDs, windowIDNum);
        }
    }
    CFRelease(allWindowInfo);

    if (CFArrayGetCount(windowIDs) == 0) {
        CFRelease(windowIDs);
        return false;
    }

    // Build an index->spaceID table from the managed display spaces list so we
    // can map raw CGS space IDs back to zero-based indices.
    CGSConnectionID connection = CGSMainConnectionID();

    CFStringRef displayIdentifier = NULL;
    if (&CGSCopyActiveMenuBarDisplayIdentifier != NULL) {
        displayIdentifier = CGSCopyActiveMenuBarDisplayIdentifier(connection);
    }

    CFArrayRef displays = CGSCopyManagedDisplaySpaces(connection, displayIdentifier);
    if (!displays && displayIdentifier) {
        displays = CGSCopyManagedDisplaySpaces(connection, NULL);
    }
    if (!displays) {
        if (displayIdentifier) CFRelease(displayIdentifier);
        CFRelease(windowIDs);
        return false;
    }

    // Collect the ordered list of space IDs for the active display.
    // We'll use this to convert a CGS space ID → zero-based index.
    CFMutableArrayRef orderedSpaceIDs = CFArrayCreateMutable(NULL, 0, NULL);
    if (!orderedSpaceIDs) {
        CFRelease(displays);
        if (displayIdentifier) CFRelease(displayIdentifier);
        CFRelease(windowIDs);
        return false;
    }

    const CFIndex displayCount = CFArrayGetCount(displays);
    for (CFIndex d = 0; d < displayCount; d++) {
        CFDictionaryRef displayDict = (CFDictionaryRef)CFArrayGetValueAtIndex(displays, d);
        if (!displayDict || CFGetTypeID(displayDict) != CFDictionaryGetTypeID()) {
            continue;
        }

        // Only process the display identified by displayIdentifier (first display if unset).
        if (displayIdentifier) {
            CFStringRef ident = (CFStringRef)CFDictionaryGetValue(displayDict, CFSTR("Display Identifier"));
            if (!ident || !CFEqual(ident, displayIdentifier)) {
                continue;
            }
        }

        CFArrayRef spaces = (CFArrayRef)CFDictionaryGetValue(displayDict, CFSTR("Spaces"));
        if (!spaces || CFGetTypeID(spaces) != CFArrayGetTypeID()) {
            continue;
        }

        const CFIndex spaceCount = CFArrayGetCount(spaces);
        for (CFIndex s = 0; s < spaceCount; s++) {
            CFDictionaryRef spaceDict = (CFDictionaryRef)CFArrayGetValueAtIndex(spaces, s);
            if (!spaceDict || CFGetTypeID(spaceDict) != CFDictionaryGetTypeID()) {
                CFArrayAppendValue(orderedSpaceIDs, (void *)(uintptr_t)0);
                continue;
            }
            CFNumberRef spaceIDNum = (CFNumberRef)CFDictionaryGetValue(spaceDict, CFSTR("id64"));
            int64_t spaceID = 0;
            if (spaceIDNum && CFGetTypeID(spaceIDNum) == CFNumberGetTypeID()) {
                CFNumberGetValue(spaceIDNum, kCFNumberSInt64Type, &spaceID);
            }
            CFArrayAppendValue(orderedSpaceIDs, (void *)(uintptr_t)(uint64_t)spaceID);
        }
        break; // Only need the matching display.
    }

    CFRelease(displays);
    if (displayIdentifier) CFRelease(displayIdentifier);

    // Ask CGS which spaces contain the app's windows (mask 7 = all space types).
    CFArrayRef spacesForWindows = CGSCopySpacesForWindows(connection, 7, windowIDs);
    CFRelease(windowIDs);

    if (!spacesForWindows || CFArrayGetCount(spacesForWindows) == 0) {
        if (spacesForWindows) CFRelease(spacesForWindows);
        CFRelease(orderedSpaceIDs);
        return false;
    }

    // Map each returned space ID to an index and decide what to do:
    //   - If ANY window is already on currentSpaceIndex → app is accessible here, no switch needed.
    //   - Otherwise, return the first index found on another space.
    const CFIndex orderedCount = CFArrayGetCount(orderedSpaceIDs);
    bool foundOnCurrentSpace = false;
    bool foundOther = false;
    unsigned int otherIndex = 0;

    const CFIndex winSpaceCount = CFArrayGetCount(spacesForWindows);
    for (CFIndex ws = 0; ws < winSpaceCount; ws++) {
        CFTypeRef entry = CFArrayGetValueAtIndex(spacesForWindows, ws);
        if (!entry || CFGetTypeID(entry) != CFNumberGetTypeID()) {
            continue;
        }
        int64_t winSpaceID = 0;
        CFNumberGetValue((CFNumberRef)entry, kCFNumberSInt64Type, &winSpaceID);
        if (winSpaceID == 0) {
            continue;
        }

        // Find this space ID in our ordered list.
        for (CFIndex s = 0; s < orderedCount; s++) {
            int64_t candidate = (int64_t)(uintptr_t)CFArrayGetValueAtIndex(orderedSpaceIDs, s);
            if (candidate == winSpaceID) {
                if ((unsigned int)s == currentSpaceIndex) {
                    foundOnCurrentSpace = true;
                } else if (!foundOther) {
                    foundOther = true;
                    otherIndex = (unsigned int)s;
                }
                break;
            }
        }

        // Early exit: once we know the app has a window on the current space we can stop.
        if (foundOnCurrentSpace) {
            break;
        }
    }

    CFRelease(spacesForWindows);
    CFRelease(orderedSpaceIDs);

    // If the app has a window on the current space, macOS will focus it without
    // a space switch — no action needed from us.
    if (foundOnCurrentSpace) {
        return false;
    }

    if (foundOther) {
        *outIndex = otherIndex;
        return true;
    }

    return false;
}
