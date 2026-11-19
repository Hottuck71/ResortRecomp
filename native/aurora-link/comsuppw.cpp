// Fills two gaps when linking aurora with GCC against the MSVC-built Dawn. Written for this project.
//
// comsuppw: aurora asks for this MSVC library for a system information query. MinGW's comdef.h
// already supplies what that query needs, so this file only has to become a library of that name.
//
// DawnInstanceDescriptor: the one C++ symbol aurora uses from Dawn. MSVC and GCC name C++ functions
// differently, so GCC can't find it in the DLL. It only sets the structure's type.
#include <cstddef>
#include <dawn/native/DawnNative.h>

namespace dawn::native {
DawnInstanceDescriptor::DawnInstanceDescriptor() { sType = wgpu::SType::DawnInstanceDescriptor; }

// Dawn's DLL reads this structure with MSVC's layout. scripts/48_build_aurora.ps1 pins it.
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(DawnInstanceDescriptor, additionalRuntimeSearchPathsCount) == 16);
static_assert(offsetof(DawnInstanceDescriptor, additionalRuntimeSearchPaths) == 24);
static_assert(offsetof(DawnInstanceDescriptor, platform) == 32);
static_assert(offsetof(DawnInstanceDescriptor, backendValidationLevel) == 40);
static_assert(offsetof(DawnInstanceDescriptor, beginCaptureOnStartup) == 44);
static_assert(offsetof(DawnInstanceDescriptor, loggingCallbackInfo) == 48);
static_assert(sizeof(DawnInstanceDescriptor) == 80);
}
