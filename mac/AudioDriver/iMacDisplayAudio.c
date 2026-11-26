// iMac Display audio driver: a Core Audio server plug-in (user space, no kernel code).
//
// It has two devices:
//   "iMac Speakers"        output only, visible. Apps (or the whole system) play into it.
//   "iMac Speakers (tap)"  input only, hidden. The iMac Display app records from it and gets
//                          exactly what was played into "iMac Speakers", to send to the iMac.
// "iMac Speakers" exists only while the app is recording from the tap, that is, while an iMac is
// connected. Like the speakers of a real monitor, it appears with the display and goes with it.
// The driver never changes the samples. Volume and mute of "iMac Speakers" are only stored here;
// the app reads them and tells the iMac, which applies them at its own speakers.
//
// Both devices run on the same pretend clock (the Mac's own time base at 48 kHz), so what is
// written at one moment can be read back by position without any resampling.
#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    kObj_PlugIn = kAudioObjectPlugInObject,   // 1
    kObj_Out = 2, kObj_OutStream = 3, kObj_OutVolume = 4, kObj_OutMute = 5,
    kObj_Tap = 6, kObj_TapStream = 7,
};

#define kSampleRate 48000.0
#define kChannels 2
#define kBytesPerFrame (kChannels * sizeof(Float32))
#define kZeroTimeStampPeriod 16384      // frames between the clock's time stamps
#define kRingFrames 65536               // shared sample store, 1.4 s
#define kMinDecibels (-64.0f)

#define kOutUID CFSTR("iMacDisplay:speakers")
#define kTapUID CFSTR("iMacDisplay:speakers-tap")
#define kModelUID CFSTR("iMacDisplay:audio")
#define kManufacturer CFSTR("iMac Display")

typedef struct {
    UInt32 ioCount;         // clients doing IO
    UInt64 anchorHostTime;  // when IO started
    UInt64 timeStamps;      // zero time stamps handed out so far
    SInt64 ringOffset;      // device sample time + ringOffset = position in the shared timeline
} DeviceClock;

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = NULL;
static UInt32 gRefCount = 0;
static Float64 gHostTicksPerFrame = 0;
static UInt64 gEpochHostTime = 0;       // start of the shared timeline
static DeviceClock gOut, gTap;
static Float32 gVolume = 0.5f;
static UInt32 gMute = 0;
static Float32 *gRing = NULL;           // kRingFrames interleaved stereo frames
static Boolean gOutPublished = false;   // "iMac Speakers" is in the device list (the app is listening)

// Tells macOS that the list of devices changed. Not from inside a driver call: later, on another thread.
static void announceDeviceList(void) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        const AudioObjectPropertyAddress changed[2] = {
            {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        };
        if (gHost) gHost->PropertiesChanged(gHost, kObj_PlugIn, 2, changed);
    });
}

static DeviceClock *clockOf(AudioObjectID device) { return device == kObj_Out ? &gOut : device == kObj_Tap ? &gTap : NULL; }
static Boolean isDevice(AudioObjectID o) { return o == kObj_Out || o == kObj_Tap; }
static Boolean isStream(AudioObjectID o) { return o == kObj_OutStream || o == kObj_TapStream; }

static AudioStreamBasicDescription streamFormat(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = kBytesPerFrame;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = kBytesPerFrame;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

// ---- properties ----------------------------------------------------------------

static Boolean hasProperty(AudioObjectID obj, const AudioObjectPropertyAddress *a) {
    switch (a->mSelector) {   // every object has these
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
            return obj >= kObj_PlugIn && obj <= kObj_TapStream;
    }
    if (obj == kObj_PlugIn) {
        switch (a->mSelector) {
            case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyDeviceList:
            case kAudioPlugInPropertyTranslateUIDToDevice: case kAudioPlugInPropertyResourceBundle:
                return true;
        }
    } else if (isDevice(obj)) {
        switch (a->mSelector) {
            case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList:
            case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyPreferredChannelsForStereo: case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod:
                return true;
        }
    } else if (isStream(obj)) {
        switch (a->mSelector) {
            case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency: case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                return true;
        }
    } else if (obj == kObj_OutVolume) {
        switch (a->mSelector) {
            case kAudioControlPropertyScope: case kAudioControlPropertyElement:
            case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
            case kAudioLevelControlPropertyConvertDecibelsToScalar:
                return true;
        }
    } else if (obj == kObj_OutMute) {
        switch (a->mSelector) {
            case kAudioControlPropertyScope: case kAudioControlPropertyElement: case kAudioBooleanControlPropertyValue:
                return true;
        }
    }
    return false;
}

static Boolean isSettable(AudioObjectID obj, const AudioObjectPropertyAddress *a) {
    if (isDevice(obj)) return a->mSelector == kAudioDevicePropertyNominalSampleRate;
    if (isStream(obj)) return a->mSelector == kAudioStreamPropertyIsActive || a->mSelector == kAudioStreamPropertyVirtualFormat ||
                              a->mSelector == kAudioStreamPropertyPhysicalFormat;
    if (obj == kObj_OutVolume) return a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue;
    if (obj == kObj_OutMute) return a->mSelector == kAudioBooleanControlPropertyValue;
    return false;
}

static Float32 scalarToDecibels(Float32 s) { return s <= 0 ? kMinDecibels : s >= 1 ? 0 : kMinDecibels * (1 - s); }
static Float32 decibelsToScalar(Float32 d) { return d <= kMinDecibels ? 0 : d >= 0 ? 1 : 1 - d / kMinDecibels; }

// One function answers both "how big" (out == NULL) and "give me the value".
#define RET(type, value) do { \
        if (out) { if (inSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)out = (value); } \
        *outSize = sizeof(type); return kAudioHardwareNoError; } while (0)

static OSStatus retList(const AudioObjectID *ids, UInt32 count, UInt32 inSize, UInt32 *outSize, void *out) {
    if (out) {
        UInt32 fit = inSize / sizeof(AudioObjectID);
        if (count > fit) count = fit;
        if (count) memcpy(out, ids, count * sizeof(AudioObjectID));
    }
    *outSize = count * sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

static OSStatus property(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qualSize, const void *qual,
                         UInt32 inSize, UInt32 *outSize, void *out) {
    const AudioObjectPropertyScope scope = a->mScope;
    if (obj == kObj_PlugIn) {
        // The tap is always there; "iMac Speakers" only while the app listens.
        static const AudioObjectID devices[] = {kObj_Tap, kObj_Out};
        pthread_mutex_lock(&gMutex); const Boolean published = gOutPublished; pthread_mutex_unlock(&gMutex);
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
            case kAudioObjectPropertyClass: RET(AudioClassID, kAudioPlugInClassID);
            case kAudioObjectPropertyOwner: RET(AudioObjectID, kAudioObjectUnknown);
            case kAudioObjectPropertyManufacturer: RET(CFStringRef, kManufacturer);
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: return retList(devices, published ? 2 : 1, inSize, outSize, out);
            case kAudioPlugInPropertyTranslateUIDToDevice: {
                AudioObjectID found = kAudioObjectUnknown;
                if (qualSize == sizeof(CFStringRef) && qual && *(CFStringRef *)qual) {
                    CFStringRef uid = *(CFStringRef *)qual;
                    if (CFStringCompare(uid, kOutUID, 0) == kCFCompareEqualTo) found = published ? kObj_Out : kAudioObjectUnknown;
                    else if (CFStringCompare(uid, kTapUID, 0) == kCFCompareEqualTo) found = kObj_Tap;
                }
                RET(AudioObjectID, found);
            }
            case kAudioPlugInPropertyResourceBundle: RET(CFStringRef, CFSTR(""));
        }
    } else if (isDevice(obj)) {
        const Boolean isOut = obj == kObj_Out;
        const AudioObjectID stream = isOut ? kObj_OutStream : kObj_TapStream;
        const Boolean streamScope = scope == kAudioObjectPropertyScopeGlobal ||
                                    scope == (isOut ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput);
        static const AudioObjectID controls[] = {kObj_OutVolume, kObj_OutMute};
        const Boolean controlScope = isOut && (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput);
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
            case kAudioObjectPropertyClass: RET(AudioClassID, kAudioDeviceClassID);
            case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_PlugIn);
            case kAudioObjectPropertyName: RET(CFStringRef, isOut ? CFSTR("iMac Speakers") : CFSTR("iMac Speakers (tap)"));
            case kAudioObjectPropertyManufacturer: RET(CFStringRef, kManufacturer);
            case kAudioObjectPropertyOwnedObjects: {
                AudioObjectID owned[3]; UInt32 n = 0;
                if (streamScope) owned[n++] = stream;
                if (controlScope) { owned[n++] = kObj_OutVolume; owned[n++] = kObj_OutMute; }
                return retList(owned, n, inSize, outSize, out);
            }
            case kAudioDevicePropertyDeviceUID: RET(CFStringRef, isOut ? kOutUID : kTapUID);
            case kAudioDevicePropertyModelUID: RET(CFStringRef, kModelUID);
            case kAudioDevicePropertyTransportType: RET(UInt32, kAudioDeviceTransportTypeVirtual);
            case kAudioDevicePropertyRelatedDevices: return retList(&obj, 1, inSize, outSize, out);
            case kAudioDevicePropertyClockDomain: RET(UInt32, 0);
            case kAudioDevicePropertyDeviceIsAlive: {
                pthread_mutex_lock(&gMutex); UInt32 alive = !isOut || gOutPublished; pthread_mutex_unlock(&gMutex);
                RET(UInt32, alive);
            }
            case kAudioDevicePropertyDeviceIsRunning: {
                pthread_mutex_lock(&gMutex); UInt32 running = clockOf(obj)->ioCount > 0; pthread_mutex_unlock(&gMutex);
                RET(UInt32, running);
            }
            // Only "iMac Speakers" may be picked as the Mac's output; the tap is never a default device.
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                RET(UInt32, isOut && scope != kAudioObjectPropertyScopeInput ? 1 : 0);
            case kAudioDevicePropertyLatency: RET(UInt32, 0);
            case kAudioDevicePropertyStreams: return retList(&stream, streamScope ? 1 : 0, inSize, outSize, out);
            case kAudioObjectPropertyControlList: return retList(controls, isOut ? 2 : 0, inSize, outSize, out);
            case kAudioDevicePropertySafetyOffset: RET(UInt32, 0);
            case kAudioDevicePropertyNominalSampleRate: RET(Float64, kSampleRate);
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange r = {kSampleRate, kSampleRate};
                RET(AudioValueRange, r);
            }
            case kAudioDevicePropertyIsHidden: RET(UInt32, isOut ? 0 : 1);
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                if (out) { if (inSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError; ((UInt32 *)out)[0] = 1; ((UInt32 *)out)[1] = 2; }
                *outSize = 2 * sizeof(UInt32); return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyPreferredChannelLayout: {
                const UInt32 size = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
                if (out) {
                    if (inSize < size) return kAudioHardwareBadPropertySizeError;
                    AudioChannelLayout *l = out;
                    memset(l, 0, size);
                    l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                    l->mNumberChannelDescriptions = kChannels;
                    l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                    l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                }
                *outSize = size; return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyZeroTimeStampPeriod: RET(UInt32, kZeroTimeStampPeriod);
        }
    } else if (isStream(obj)) {
        const Boolean isOut = obj == kObj_OutStream;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
            case kAudioObjectPropertyClass: RET(AudioClassID, kAudioStreamClassID);
            case kAudioObjectPropertyOwner: RET(AudioObjectID, isOut ? kObj_Out : kObj_Tap);
            case kAudioObjectPropertyOwnedObjects: return retList(NULL, 0, inSize, outSize, out);
            case kAudioStreamPropertyIsActive: RET(UInt32, 1);
            case kAudioStreamPropertyDirection: RET(UInt32, isOut ? 0 : 1);
            case kAudioStreamPropertyTerminalType: RET(UInt32, isOut ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeDigitalAudioInterface);
            case kAudioStreamPropertyStartingChannel: RET(UInt32, 1);
            case kAudioStreamPropertyLatency: RET(UInt32, 0);
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: RET(AudioStreamBasicDescription, streamFormat());
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription r = {streamFormat(), {kSampleRate, kSampleRate}};
                RET(AudioStreamRangedDescription, r);
            }
        }
    } else if (obj == kObj_OutVolume) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioLevelControlClassID);
            case kAudioObjectPropertyClass: RET(AudioClassID, kAudioVolumeControlClassID);
            case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_Out);
            case kAudioObjectPropertyOwnedObjects: return retList(NULL, 0, inSize, outSize, out);
            case kAudioControlPropertyScope: RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement: RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
            case kAudioLevelControlPropertyScalarValue: {
                pthread_mutex_lock(&gMutex); Float32 v = gVolume; pthread_mutex_unlock(&gMutex);
                RET(Float32, v);
            }
            case kAudioLevelControlPropertyDecibelValue: {
                pthread_mutex_lock(&gMutex); Float32 v = gVolume; pthread_mutex_unlock(&gMutex);
                RET(Float32, scalarToDecibels(v));
            }
            case kAudioLevelControlPropertyDecibelRange: {
                AudioValueRange r = {kMinDecibels, 0};
                RET(AudioValueRange, r);
            }
            // These two convert the value the caller passed in the output buffer.
            case kAudioLevelControlPropertyConvertScalarToDecibels:
                if (out) { if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError; *(Float32 *)out = scalarToDecibels(*(Float32 *)out); }
                *outSize = sizeof(Float32); return kAudioHardwareNoError;
            case kAudioLevelControlPropertyConvertDecibelsToScalar:
                if (out) { if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError; *(Float32 *)out = decibelsToScalar(*(Float32 *)out); }
                *outSize = sizeof(Float32); return kAudioHardwareNoError;
        }
    } else if (obj == kObj_OutMute) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioBooleanControlClassID);
            case kAudioObjectPropertyClass: RET(AudioClassID, kAudioMuteControlClassID);
            case kAudioObjectPropertyOwner: RET(AudioObjectID, kObj_Out);
            case kAudioObjectPropertyOwnedObjects: return retList(NULL, 0, inSize, outSize, out);
            case kAudioControlPropertyScope: RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
            case kAudioControlPropertyElement: RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
            case kAudioBooleanControlPropertyValue: {
                pthread_mutex_lock(&gMutex); UInt32 m = gMute; pthread_mutex_unlock(&gMutex);
                RET(UInt32, m);
            }
        }
    } else {
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---- the plug-in interface --------------------------------------------------------

static AudioServerPlugInDriverInterface gInterface;
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

static HRESULT IMD_QueryInterface(void *driver, REFIID iid, LPVOID *outInterface) {
    if (driver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, iid);
    Boolean ok = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);
    if (!ok) { *outInterface = NULL; return E_NOINTERFACE; }
    pthread_mutex_lock(&gMutex); gRefCount++; pthread_mutex_unlock(&gMutex);
    *outInterface = gDriverRef;
    return S_OK;
}
static ULONG IMD_AddRef(void *driver) {
    pthread_mutex_lock(&gMutex); ULONG n = ++gRefCount; pthread_mutex_unlock(&gMutex); return n;
}
static ULONG IMD_Release(void *driver) {
    pthread_mutex_lock(&gMutex); ULONG n = gRefCount ? --gRefCount : 0; pthread_mutex_unlock(&gMutex); return n;
}

static OSStatus IMD_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    if (driver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = host;
    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    Float64 hostTicksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    gHostTicksPerFrame = hostTicksPerSecond / kSampleRate;
    gEpochHostTime = mach_absolute_time();
    if (!gRing) gRing = calloc(kRingFrames, kBytesPerFrame);
    return gRing ? kAudioHardwareNoError : kAudioHardwareUnspecifiedError;
}

static OSStatus IMD_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *outID) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus IMD_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID device) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus IMD_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID device, const AudioServerPlugInClientInfo *c) { return kAudioHardwareNoError; }
static OSStatus IMD_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID device, const AudioServerPlugInClientInfo *c) { return kAudioHardwareNoError; }
static OSStatus IMD_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID device, UInt64 action, void *info) { return kAudioHardwareNoError; }
static OSStatus IMD_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID device, UInt64 action, void *info) { return kAudioHardwareNoError; }

static Boolean IMD_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    return d == gDriverRef && a && hasProperty(obj, a);
}
static OSStatus IMD_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *outSettable) {
    if (d != gDriverRef || !a || !outSettable) return kAudioHardwareIllegalOperationError;
    if (!hasProperty(obj, a)) return kAudioHardwareUnknownPropertyError;
    *outSettable = isSettable(obj, a);
    return kAudioHardwareNoError;
}
static OSStatus IMD_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                        UInt32 qualSize, const void *qual, UInt32 *outSize) {
    if (d != gDriverRef || !a || !outSize) return kAudioHardwareIllegalOperationError;
    if (!hasProperty(obj, a)) return kAudioHardwareUnknownPropertyError;
    return property(obj, a, qualSize, qual, 0, outSize, NULL);
}
static OSStatus IMD_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qualSize, const void *qual, UInt32 inSize, UInt32 *outSize, void *outData) {
    if (d != gDriverRef || !a || !outSize || !outData) return kAudioHardwareIllegalOperationError;
    if (!hasProperty(obj, a)) return kAudioHardwareUnknownPropertyError;
    return property(obj, a, qualSize, qual, inSize, outSize, outData);
}

static OSStatus IMD_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qualSize, const void *qual, UInt32 inSize, const void *inData) {
    if (d != gDriverRef || !a || !inData) return kAudioHardwareIllegalOperationError;
    if (!hasProperty(obj, a)) return kAudioHardwareUnknownPropertyError;
    if (!isSettable(obj, a)) return kAudioHardwareUnsupportedOperationError;
    AudioObjectPropertyAddress changed[2];
    UInt32 changedCount = 0;
    if (isDevice(obj)) {           // only 48 kHz exists
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        if (*(const Float64 *)inData != kSampleRate) return kAudioHardwareIllegalOperationError;
    } else if (isStream(obj)) {
        if (a->mSelector != kAudioStreamPropertyIsActive) {   // only one format exists
            if (inSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            const AudioStreamBasicDescription *f = inData, want = streamFormat();
            if (f->mSampleRate != want.mSampleRate || f->mFormatID != want.mFormatID || f->mChannelsPerFrame != want.mChannelsPerFrame ||
                f->mBitsPerChannel != want.mBitsPerChannel) return kAudioDeviceUnsupportedFormatError;
        }
    } else if (obj == kObj_OutVolume) {
        if (inSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32 *)inData;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = decibelsToScalar(v);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        pthread_mutex_lock(&gMutex);
        Boolean differs = v != gVolume;
        gVolume = v;
        pthread_mutex_unlock(&gMutex);
        if (differs) {
            changed[0] = (AudioObjectPropertyAddress){kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            changed[1] = (AudioObjectPropertyAddress){kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            changedCount = 2;
        }
    } else if (obj == kObj_OutMute) {
        if (inSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        UInt32 m = *(const UInt32 *)inData != 0;
        pthread_mutex_lock(&gMutex);
        Boolean differs = m != gMute;
        gMute = m;
        pthread_mutex_unlock(&gMutex);
        if (differs) {
            changed[0] = (AudioObjectPropertyAddress){kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            changedCount = 1;
        }
    }
    if (changedCount && gHost) gHost->PropertiesChanged(gHost, obj, changedCount, changed);
    return kAudioHardwareNoError;
}

// ---- IO ------------------------------------------------------------------------------

static OSStatus IMD_StartIO(AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 clientID) {
    DeviceClock *c = clockOf(device);
    if (d != gDriverRef || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    if (c->ioCount == 0) {
        c->timeStamps = 0;
        c->anchorHostTime = mach_absolute_time();
        // Where this device's sample 0 lies on the shared timeline, in whole frames.
        c->ringOffset = (SInt64)llround((Float64)(c->anchorHostTime - gEpochHostTime) / gHostTicksPerFrame);
        if (device == kObj_Tap && gRing) memset(gRing, 0, kRingFrames * kBytesPerFrame);   // nothing old for a new listener
    }
    c->ioCount++;
    Boolean appeared = false;
    if (device == kObj_Tap && !gOutPublished) { gOutPublished = true; appeared = true; }   // the app listens: show "iMac Speakers"
    pthread_mutex_unlock(&gMutex);
    if (appeared) announceDeviceList();
    return kAudioHardwareNoError;
}

static OSStatus IMD_StopIO(AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 clientID) {
    DeviceClock *c = clockOf(device);
    if (d != gDriverRef || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    if (c->ioCount) c->ioCount--;
    Boolean gone = false;
    if (device == kObj_Tap && c->ioCount == 0 && gOutPublished) { gOutPublished = false; gone = true; }   // nobody listens: hide it
    pthread_mutex_unlock(&gMutex);
    if (gone) announceDeviceList();
    return kAudioHardwareNoError;
}

static OSStatus IMD_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 clientID,
                                     Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    DeviceClock *c = clockOf(device);
    if (d != gDriverRef || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gMutex);
    const Float64 ticksPerPeriod = gHostTicksPerFrame * kZeroTimeStampPeriod;
    const UInt64 now = mach_absolute_time();
    const UInt64 next = c->anchorHostTime + (UInt64)((Float64)(c->timeStamps + 1) * ticksPerPeriod);
    if (next <= now) c->timeStamps++;
    *outSampleTime = (Float64)c->timeStamps * kZeroTimeStampPeriod;
    *outHostTime = c->anchorHostTime + (UInt64)((Float64)c->timeStamps * ticksPerPeriod);
    *outSeed = 1;
    pthread_mutex_unlock(&gMutex);
    return kAudioHardwareNoError;
}

static OSStatus IMD_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 clientID, UInt32 operation,
                                      Boolean *outWillDo, Boolean *outInPlace) {
    Boolean will = (device == kObj_Out && operation == kAudioServerPlugInIOOperationWriteMix) ||
                   (device == kObj_Tap && operation == kAudioServerPlugInIOOperationReadInput);
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return kAudioHardwareNoError;
}
static OSStatus IMD_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 clientID, UInt32 operation,
                                     UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) { return kAudioHardwareNoError; }
static OSStatus IMD_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID device, UInt32 clientID, UInt32 operation,
                                   UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) { return kAudioHardwareNoError; }

// Position on the shared timeline, wrapped into the ring.
static UInt32 ringIndex(SInt64 position) {
    SInt64 m = position % kRingFrames;
    return (UInt32)(m < 0 ? m + kRingFrames : m);
}

static OSStatus IMD_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID device, AudioObjectID stream, UInt32 clientID, UInt32 operation,
                                  UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *mainBuffer, void *secondaryBuffer) {
    if (d != gDriverRef || !cycle || !mainBuffer || !gRing) return kAudioHardwareBadObjectError;
    if (frames > kRingFrames) return kAudioHardwareBadPropertySizeError;
    Float32 *buf = mainBuffer;
    if (device == kObj_Out && operation == kAudioServerPlugInIOOperationWriteMix) {
        // What the Mac plays, already mixed. Store it at its place in time.
        UInt32 at = ringIndex((SInt64)cycle->mOutputTime.mSampleTime + gOut.ringOffset);
        UInt32 first = kRingFrames - at < frames ? kRingFrames - at : frames;
        memcpy(gRing + at * kChannels, buf, first * kBytesPerFrame);
        if (first < frames) memcpy(gRing, buf + first * kChannels, (frames - first) * kBytesPerFrame);
    } else if (device == kObj_Tap && operation == kAudioServerPlugInIOOperationReadInput) {
        // The app asks for a stretch of the past. Hand it over and wipe it, so nothing is ever heard twice
        // and silence follows when nobody plays.
        UInt32 at = ringIndex((SInt64)cycle->mInputTime.mSampleTime + gTap.ringOffset);
        UInt32 first = kRingFrames - at < frames ? kRingFrames - at : frames;
        memcpy(buf, gRing + at * kChannels, first * kBytesPerFrame);
        memset(gRing + at * kChannels, 0, first * kBytesPerFrame);
        if (first < frames) {
            memcpy(buf + first * kChannels, gRing, (frames - first) * kBytesPerFrame);
            memset(gRing, 0, (frames - first) * kBytesPerFrame);
        }
    }
    return kAudioHardwareNoError;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    IMD_QueryInterface, IMD_AddRef, IMD_Release,
    IMD_Initialize, IMD_CreateDevice, IMD_DestroyDevice, IMD_AddDeviceClient, IMD_RemoveDeviceClient,
    IMD_PerformDeviceConfigurationChange, IMD_AbortDeviceConfigurationChange,
    IMD_HasProperty, IMD_IsPropertySettable, IMD_GetPropertyDataSize, IMD_GetPropertyData, IMD_SetPropertyData,
    IMD_StartIO, IMD_StopIO, IMD_GetZeroTimeStamp, IMD_WillDoIOOperation, IMD_BeginIOOperation, IMD_DoIOOperation, IMD_EndIOOperation,
};

// The factory named in Info.plist (CFPlugInFactories).
void *IMDAudio_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID) {
    return CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID) ? gDriverRef : NULL;
}
