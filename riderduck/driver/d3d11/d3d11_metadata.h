#pragma once

#include "api/replay/rdcstr.h"
#include "api/replay/resourceid.h"

struct ShaderReflection;
class WrappedID3D11Device;

namespace D3D11Metadata
{
bool Enabled();
// Use around RiderDuck's own compiler calls. Never change compiler parameters/results.
struct Suppress
{
  Suppress();
  ~Suppress();
};
void Shader(const char *phase, ResourceId id, const void *bytes, size_t length);
void BufferName(ResourceId id, const rdcstr &name, const char *encoding, bool replay);
void BeginReplay();
void WriteCapture(const rdcstr &capturePath);
void WriteReplay(const rdcstr &capturePath, WrappedID3D11Device *device);
}
