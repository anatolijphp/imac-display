// Exercises the audio driver without installing it: loads the bundle's binary, walks every
// property of every object the way coreaudiod does, then plays a ramp into "iMac Speakers" and
// checks that the tap device returns it once and silence afterwards.
//   test-driver path/to/iMacDisplayAudio.driver/Contents/MacOS/iMacDisplayAudio
#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int notifications = 0;
static OSStatus hostChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress a[]) { notifications += n; return 0; }
static OSStatus hostCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) { return 1; }
static OSStatus hostWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { return 0; }
static OSStatus hostDelete(AudioServerPlugInHostRef h, CFStringRef k) { return 0; }
static OSStatus hostRequest(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) { return 0; }
static const AudioServerPlugInHostInterface host = {hostChanged, hostCopy, hostWrite, hostDelete, hostRequest};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test-driver <driver binary>\n"); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "cannot load %s: %s\n", argv[1], dlerror()); return 2; }
    void *(*create)(CFAllocatorRef, CFUUIDRef) = dlsym(lib, "IMDAudio_Create");
    if (!create) { fprintf(stderr, "no IMDAudio_Create\n"); return 2; }
    AudioServerPlugInDriverRef d = create(NULL, kAudioServerPlugInTypeUUID);
    CHECK(d != NULL, "factory returned NULL");
    CHECK(create(NULL, IUnknownUUID) == NULL, "factory accepted a wrong type");
    AudioServerPlugInDriverInterface *i = *d;

    void *again = NULL;
    CHECK(i->QueryInterface(d, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &again) == 0 && again == d, "QueryInterface");
    CHECK(i->Initialize(d, &host) == 0, "Initialize");

    // Every property the driver says it has must give a size and then a value of that size.
    static const AudioObjectPropertySelector selectors[] = {
        kAudioObjectPropertyBaseClass, kAudioObjectPropertyClass, kAudioObjectPropertyOwner, kAudioObjectPropertyName,
        kAudioObjectPropertyManufacturer, kAudioObjectPropertyOwnedObjects, kAudioPlugInPropertyDeviceList,
        kAudioPlugInPropertyResourceBundle, kAudioDevicePropertyDeviceUID, kAudioDevicePropertyModelUID,
        kAudioDevicePropertyTransportType, kAudioDevicePropertyRelatedDevices, kAudioDevicePropertyClockDomain,
        kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning, kAudioDevicePropertyDeviceCanBeDefaultDevice,
        kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioDevicePropertyLatency, kAudioDevicePropertyStreams,
        kAudioObjectPropertyControlList, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyNominalSampleRate,
        kAudioDevicePropertyAvailableNominalSampleRates, kAudioDevicePropertyIsHidden, kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyPreferredChannelLayout, kAudioDevicePropertyZeroTimeStampPeriod, kAudioDevicePropertyIcon,
        kAudioStreamPropertyIsActive, kAudioStreamPropertyDirection, kAudioStreamPropertyTerminalType,
        kAudioStreamPropertyStartingChannel, kAudioStreamPropertyLatency, kAudioStreamPropertyVirtualFormat,
        kAudioStreamPropertyPhysicalFormat, kAudioStreamPropertyAvailableVirtualFormats, kAudioStreamPropertyAvailablePhysicalFormats,
        kAudioControlPropertyScope, kAudioControlPropertyElement, kAudioLevelControlPropertyScalarValue,
        kAudioLevelControlPropertyDecibelValue, kAudioLevelControlPropertyDecibelRange, kAudioBooleanControlPropertyValue,
    };
    int answered = 0;
    for (AudioObjectID obj = 1; obj <= 8; obj++) {
        for (size_t s = 0; s < sizeof selectors / sizeof *selectors; s++) {
            for (int sc = 0; sc < 3; sc++) {
                AudioObjectPropertyAddress a = {selectors[s], sc == 0 ? kAudioObjectPropertyScopeGlobal : sc == 1 ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
                if (!i->HasProperty(d, obj, 0, &a)) continue;
                UInt32 size = 0xFFFFFFFF, used = 0;
                char buf[256];
                memset(buf, 0, sizeof buf);
                OSStatus rc = i->GetPropertyDataSize(d, obj, 0, &a, 0, NULL, &size);
                CHECK(rc == 0 && size <= sizeof buf, "size of %.4s on object %u: rc %d size %u", (char *)&(UInt32){CFSwapInt32HostToBig(a.mSelector)}, obj, (int)rc, size);
                rc = i->GetPropertyData(d, obj, 0, &a, 0, NULL, sizeof buf, &used, buf);
                CHECK(rc == 0 && used == size, "value of %.4s on object %u: rc %d used %u size %u", (char *)&(UInt32){CFSwapInt32HostToBig(a.mSelector)}, obj, (int)rc, used, size);
                Boolean settable = 2;
                CHECK(i->IsPropertySettable(d, obj, 0, &a, &settable) == 0 && settable <= 1, "IsPropertySettable");
                answered++;
            }
        }
    }
    printf("properties answered: %d\n", answered);
    AudioObjectPropertyAddress none = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK(!i->HasProperty(d, 8, 0, &none) && !i->HasProperty(d, 99, 0, &none), "an object that does not exist has properties");

    // UID lookup, names, hidden flag.
    CFStringRef uid = CFSTR("iMacDisplay:speakers-tap");
    AudioObjectID found = 0; UInt32 used = 0;
    AudioObjectPropertyAddress tr = {kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    i->GetPropertyData(d, 1, 0, &tr, sizeof uid, &uid, sizeof found, &used, &found);
    CHECK(found == 6, "tap UID gives object %u", found);
    UInt32 hidden = 9;
    AudioObjectPropertyAddress hid = {kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    i->GetPropertyData(d, 2, 0, &hid, 0, NULL, sizeof hidden, &used, &hidden); CHECK(hidden == 0, "speakers hidden");
    i->GetPropertyData(d, 6, 0, &hid, 0, NULL, sizeof hidden, &used, &hidden); CHECK(hidden == 1, "tap not hidden");

    // Volume and mute: set, read back, host told.
    AudioObjectPropertyAddress vol = {kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Float32 v = 0.25f, back = 0;
    CHECK(i->SetPropertyData(d, 4, 0, &vol, 0, NULL, sizeof v, &v) == 0, "set volume");
    i->GetPropertyData(d, 4, 0, &vol, 0, NULL, sizeof back, &used, &back);
    CHECK(back == 0.25f && notifications == 2, "volume %f, notifications %d", back, notifications);
    AudioObjectPropertyAddress mute = {kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 m = 1, mb = 0;
    CHECK(i->SetPropertyData(d, 5, 0, &mute, 0, NULL, sizeof m, &m) == 0, "set mute");
    i->GetPropertyData(d, 5, 0, &mute, 0, NULL, sizeof mb, &used, &mb);
    CHECK(mb == 1 && notifications == 3, "mute %u, notifications %d", mb, notifications);
    Float64 rate = 44100;
    AudioObjectPropertyAddress sr = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK(i->SetPropertyData(d, 2, 0, &sr, 0, NULL, sizeof rate, &rate) != 0, "44.1 kHz was accepted");

    // Loopback: play a ramp at sample 1000 of the speakers, read the tap from sample 0.
    CHECK(i->StartIO(d, 2, 1) == 0 && i->StartIO(d, 6, 1) == 0, "StartIO");
    Float64 st; UInt64 ht, seed;
    CHECK(i->GetZeroTimeStamp(d, 2, 1, &st, &ht, &seed) == 0 && st == 0 && seed == 1, "zero time stamp");
    Boolean will = false, inPlace = false;
    i->WillDoIOOperation(d, 2, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace); CHECK(will, "speakers refuse WriteMix");
    i->WillDoIOOperation(d, 6, 1, kAudioServerPlugInIOOperationReadInput, &will, &inPlace); CHECK(will, "tap refuses ReadInput");
    i->WillDoIOOperation(d, 6, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace); CHECK(!will, "tap accepts WriteMix");

    enum { N = 512, R = 4096 };
    Float32 ramp[N * 2], got[R * 2];
    for (int f = 0; f < N; f++) { ramp[f * 2] = (f + 1) / 1000.0f; ramp[f * 2 + 1] = -(f + 1) / 1000.0f; }
    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof cycle);
    cycle.mOutputTime.mSampleTime = 1000;
    CHECK(i->DoIOOperation(d, 2, 3, 1, kAudioServerPlugInIOOperationWriteMix, N, &cycle, ramp, NULL) == 0, "write");
    cycle.mInputTime.mSampleTime = 0;
    memset(got, 0x7f, sizeof got);
    CHECK(i->DoIOOperation(d, 6, 7, 1, kAudioServerPlugInIOOperationReadInput, R, &cycle, got, NULL) == 0, "read");
    int start = -1, wrong = 0;
    for (int f = 0; f < R; f++) if (got[f * 2] != 0) { start = f; break; }
    if (start >= 0) for (int f = 0; f < N && start + f < R; f++) wrong += got[(start + f) * 2] != ramp[f * 2] || got[(start + f) * 2 + 1] != ramp[f * 2 + 1];
    for (int f = 0; f < R; f++) if (start < 0 || f < start || f >= start + N) wrong += got[f * 2] != 0 || got[f * 2 + 1] != 0;
    printf("ramp written at 1000 came back at %d (the two devices started a few frames apart), %d wrong samples\n", start, wrong);
    CHECK(start >= 990 && start <= 1010 && wrong == 0, "loopback");
    memset(got, 0x7f, sizeof got);
    i->DoIOOperation(d, 6, 7, 1, kAudioServerPlugInIOOperationReadInput, R, &cycle, got, NULL);
    int nonzero = 0;
    for (int f = 0; f < R * 2; f++) nonzero += got[f] != 0;
    CHECK(nonzero == 0, "second read of the same stretch is not silent (%d samples)", nonzero);

    // Wrap-around at the end of the ring.
    cycle.mOutputTime.mSampleTime = 65536.0 * 3 - 100;
    i->DoIOOperation(d, 2, 3, 1, kAudioServerPlugInIOOperationWriteMix, N, &cycle, ramp, NULL);
    cycle.mInputTime.mSampleTime = 65536.0 * 3 - 100 + (1000 - start);
    i->DoIOOperation(d, 6, 7, 1, kAudioServerPlugInIOOperationReadInput, N, &cycle, got, NULL);
    CHECK(memcmp(got, ramp, sizeof ramp) == 0, "wrap-around");

    CHECK(i->StopIO(d, 2, 1) == 0 && i->StopIO(d, 6, 1) == 0, "StopIO");
    printf(failures ? "%d FAILURES\n" : "driver self-test passed\n", failures);
    return failures != 0;
}
