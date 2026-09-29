#include "d3d11_metadata.h"
#include <map>
#include <mutex>
#include "3rdparty/md5/md5.h"
#include "common/timing.h"
#include "core/core.h"
#include "driver/dx/official/d3dcompiler.h"
#include "driver/shaders/dxbc/dxbc_container.h"
#include "hooks/hooks.h"
#include "d3d11_device.h"
#include "d3d11_replay.h"
#include "d3d11_resources.h"

#ifndef RIDERDUCK_DIAG_SHADER_METADATA_DEFAULT
#define RIDERDUCK_DIAG_SHADER_METADATA_DEFAULT 0
#endif

namespace D3D11Metadata
{
namespace
{
thread_local uint32_t depth = 0;
const rdcstr &EnvironmentOverride()
{
  static const rdcstr value = Process::GetEnvVariable("RIDERDUCK_DIAG_SHADER_METADATA");
  return value;
}
struct Store
{
  std::mutex mutex;
  std::map<rdcstr, rdcstr> blobs;
  std::map<rdcstr, uint64_t> observations;
  std::map<rdcstr, uint64_t> names;
  double observationMS = 0;
  size_t summaryBytes = 0;
  uint64_t dropped = 0, nameSequence = 0;
};
Store &Data()
{
  static Store store;
  return store;
}
rdcstr Quote(const rdcstr &s)
{
  rdcstr result = "\"";
  for(unsigned char c : s)
  {
    if(c == '\\' || c == '"')
    {
      result += '\\';
      result += char(c);
    }
    else if(c < 32)
      result += StringFormat::Fmt("\\u%04x", uint32_t(c));
    else
      result += char(c);
  }
  return result + "\"";
}
rdcstr Hash(const void *bytes, size_t size)
{
  MD5_CTX context;
  unsigned char hash[16];
  MD5_Init(&context);
  const byte *data = (const byte *)bytes;
  while(size)
  {
    unsigned long part = (unsigned long)RDCMIN(size, size_t(0x40000000));
    MD5_Update(&context, data, part);
    size -= part;
    data += part;
  }
  MD5_Final(hash, &context);
  rdcstr result;
  for(unsigned char c : hash)
    result += StringFormat::Fmt("%02x", uint32_t(c));
  return result;
}
uint32_t U32(const byte *p)
{
  uint32_t value;
  memcpy(&value, p, 4);
  return value;
}
bool HasChunk(const bytebuf &bytes, const char *tag)
{
  if(bytes.size() < 32 || memcmp(bytes.data(), "DXBC", 4) != 0)
    return false;
  uint32_t count = U32(bytes.data() + 28);
  if(count > (bytes.size() - 32) / 4)
    return false;
  for(uint32_t i = 0; i < count; i++)
  {
    uint32_t offset = U32(bytes.data() + 32 + i * 4);
    if(offset <= bytes.size() - 8 && U32(bytes.data() + offset + 4) <= bytes.size() - offset - 8 &&
       memcmp(bytes.data() + offset, tag, 4) == 0)
      return true;
  }
  return false;
}
rdcstr Variables(const rdcarray<DXBC::CBufferVariable> &variables)
{
  rdcstr result = "[";
  for(const auto &v : variables)
  {
    if(result.size() > 1)
      result += ",";
    result += "{\"name\":" + Quote(v.name) + ",\"offset\":" + ToStr(v.offset) +
              ",\"rows\":" + ToStr(v.type.rows) + ",\"columns\":" + ToStr(v.type.cols) +
              ",\"elements\":" + ToStr(v.type.elements) + ",\"type\":" + Quote(v.type.name) +
              ",\"members\":" + Variables(v.type.members) + "}";
  }
  return result + "]";
}
rdcstr Summarise(const void *bytes, size_t size)
{
  const byte *data = (const byte *)bytes;
  bool valid = size >= 32 && memcmp(data, "DXBC", 4) == 0;
  bool rdef = false, debug = false;
  rdcstr chunks = "[";
  if(valid)
  {
    uint32_t count = U32(data + 28);
    valid = count <= (size - 32) / 4 && U32(data + 24) == size;
    for(uint32_t i = 0; valid && i < count; i++)
    {
      uint32_t offset = U32(data + 32 + 4 * i);
      if(offset > size - 8 || U32(data + offset + 4) > size - offset - 8)
      {
        valid = false;
        break;
      }
      rdcstr tag((const char *)data + offset, 4);
      if(chunks.size() > 1)
        chunks += ",";
      chunks += Quote(tag);
      rdef |= tag == "RDEF";
      debug |= tag == "SDBG" || tag == "SPDB" || tag == "ILDB" || tag == "PDBI";
    }
  }
  rdcstr blocks = "[";
  if(valid && rdef)
  {
    bytebuf copy;
    copy.assign(data, size);
    DXBC::DXBCContainer container(copy, {}, GraphicsAPI::D3D11, ~0U, ~0U);
    for(const DXBC::CBuffer &cb : container.GetReflection()->CBuffers)
    {
      if(blocks.size() > 1)
        blocks += ",";
      blocks += "{\"name\":" + Quote(cb.name) + ",\"slot\":" + ToStr(cb.reg) +
                ",\"byte_size\":" + ToStr(cb.descriptor.byteSize) +
                ",\"variable_count\":" + ToStr(cb.variables.size()) +
                ",\"variables\":" + Variables(cb.variables) + "}";
    }
  }
  return "{\"byte_size\":" + ToStr(size) + ",\"valid_dxbc\":" + (valid ? "true" : "false") +
         ",\"has_rdef\":" + (rdef ? "true" : "false") +
         ",\"has_debug\":" + (debug ? "true" : "false") + ",\"chunks\":" + chunks +
         "],\"cbuffers\":" + blocks + "]}";
}
// Called with store.mutex held. Bound metadata rather than retaining source/bytecode payloads.
rdcstr Blob(Store &store, const void *bytes, size_t size)
{
  if(!bytes || !size)
    return {};
  rdcstr hash = Hash(bytes, size);
  if(store.blobs.find(hash) == store.blobs.end())
  {
    if(store.summaryBytes >= 16 * 1024 * 1024 || store.blobs.size() >= 32768)
    {
      store.dropped++;
      return {};
    }
    rdcstr summary = Summarise(bytes, size);
    store.summaryBytes += summary.size() + hash.size();
    store.blobs[hash] = summary;
  }
  return hash;
}
void Observation(const char *api, const void *bytes, size_t size, ResourceId id,
                 const void *output = NULL, size_t outputSize = 0, UINT flags = 0,
                 int compilerVersion = 0)
{
  PerformanceTimer timer;
  Store &store = Data();
  std::lock_guard<std::mutex> lock(store.mutex);
  rdcstr hash = Blob(store, bytes, size), next = Blob(store, output, outputSize);
  if(hash.empty())
    return;
  rdcstr row = "{\"phase\":" + Quote(api) + ",\"resource\":" + Quote(ToStr(id)) +
               ",\"input_md5\":" + Quote(hash) + ",\"output_md5\":" + Quote(next) +
               ",\"strip_flags\":" + ToStr(flags) +
               ",\"thread\":" + ToStr(Threading::GetCurrentID()) +
               ",\"compiler_version\":" + ToStr(compilerVersion) + "}";
  if(store.observations.size() < 65536 || store.observations.count(row))
    store.observations[row]++;
  else
    store.dropped++;
  store.observationMS += timer.GetMilliseconds();
}
void Write(const rdcstr &path, const char *mode, const rdcstr &reflection = "[]")
{
  Store &store = Data();
  rdcstr json;
  {
    std::lock_guard<std::mutex> lock(store.mutex);
    json = "{\"version\":1,\"mode\":" + Quote(mode) +
           ",\"build_default_enabled\":" + (RIDERDUCK_DIAG_SHADER_METADATA_DEFAULT ? "true" : "false") +
           ",\"environment_override\":" + Quote(EnvironmentOverride()) +
           ",\"scope\":\"Process observations after diagnostic hooks were enabled; zero compiler "
           "calls do not prove offline compilation. Serialise-input records are candidates; only "
           "replay deserialisation proves inclusion in the RDC.\","
           "\"hash_algorithm\":\"MD5 of complete bytecode (diagnostic identity, not a security "
           "check)\","
           "\"observation_ms\":" +
           ToStr(store.observationMS) + ",\"summary_bytes\":" + ToStr(store.summaryBytes) +
           ",\"dropped_records\":" + ToStr(store.dropped) + ",\"blobs\":{";
    bool first = true;
    for(const auto &blob : store.blobs)
    {
      if(!first)
        json += ",";
      first = false;
      json += Quote(blob.first) + ":" + blob.second;
    }
    json += "},\"observations\":[";
    first = true;
    for(const auto &row : store.observations)
    {
      if(!first)
        json += ",";
      first = false;
      json += "{\"count\":" + ToStr(row.second) + ",\"event\":" + row.first + "}";
    }
    json += "],\"buffer_names\":[";
    first = true;
    for(const auto &row : store.names)
    {
      if(!first)
        json += ",";
      first = false;
      json += "{\"count\":" + ToStr(row.second) + ",\"event\":" + row.first + "}";
    }
    json += "],\"replay_reflection\":" + reflection + "}";
  }
  FILE *file = FileIO::fopen(path, FileIO::WriteBinary);
  if(!file)
  {
    RDCWARN("Shader metadata diagnostic cannot write %s", path.c_str());
    return;
  }
  size_t written = fwrite(json.data(), 1, json.size(), file);
  int closed = fclose(file);
  if(written != json.size() || closed != 0)
    RDCWARN("Incomplete shader metadata report: %s", path.c_str());
}
}
bool Enabled()
{
  static const bool enabled = EnvironmentOverride() == "1" ||
                             (EnvironmentOverride() != "0" && RIDERDUCK_DIAG_SHADER_METADATA_DEFAULT != 0);
  return enabled;
}
Suppress::Suppress()
{
  depth++;
}
Suppress::~Suppress()
{
  depth--;
}
void Shader(const char *phase, ResourceId id, const void *bytes, size_t size)
{
  if(Enabled() && depth == 0)
    Observation(phase, bytes, size, id);
}
void BufferName(ResourceId id, const rdcstr &name, const char *encoding, bool replay)
{
  if(!Enabled())
    return;
  Store &store = Data();
  std::lock_guard<std::mutex> lock(store.mutex);
  rdcstr row = "{\"resource\":" + Quote(ToStr(id)) + ",\"name\":" + Quote(name) +
               ",\"encoding\":" + Quote(encoding) +
               ",\"phase\":" + Quote(replay ? "replay_name" : "set_private_data") +
               ",\"sequence\":" + ToStr(++store.nameSequence) + "}";
  if(store.names.size() < 65536 || store.names.count(row))
    store.names[row]++;
  else
    store.dropped++;
}
void BeginReplay()
{
  if(!Enabled())
    return;
  Store &store = Data();
  std::lock_guard<std::mutex> lock(store.mutex);
  store.blobs.clear();
  store.observations.clear();
  store.names.clear();
  store.summaryBytes = 0;
  store.observationMS = 0;
  store.dropped = 0;
  store.nameSequence = 0;
}
void WriteCapture(const rdcstr &path)
{
  if(Enabled() && !path.empty())
    Write(path + ".shader-metadata.json", "capture");
}
void WriteReplay(const rdcstr &path, WrappedID3D11Device *device)
{
  if(!Enabled() || path.empty())
    return;
  rdcstr reflection = "[";
  for(const auto &entry : WrappedShader::m_ShaderList)
  {
    if(ResourceIDGen::IsReplayOnlyID(entry.first))
      continue;
    ShaderReflection &shader = entry.second->GetDetails();
    if(reflection.size() > 1)
      reflection += ",";
    reflection += "{\"resource\":" + Quote(ToStr(entry.first)) + ",\"bytecode_md5\":" +
                  Quote(Hash(shader.rawBytes.data(), shader.rawBytes.size())) + ",\"cbuffers\":[";
    bool first = true;
    for(const ConstantBlock &cb : shader.constantBlocks)
    {
      if(!first)
        reflection += ",";
      first = false;
      const char *source = HasChunk(shader.rawBytes, "RDEF") ? "shader_reflection"
                           : entry.second->GetDXBC()->GetDebugInfo()
                               ? "debug_information_or_instruction_fallback"
                               : "instruction_fallback";
      reflection += "{\"name\":" + Quote(cb.name) + ",\"slot\":" + ToStr(cb.fixedBindNumber) +
                    ",\"variable_count\":" + ToStr(cb.variables.size()) +
                    ",\"name_source\":" + Quote(source) + "}";
    }
    reflection += "]}";
  }
  // Resource names are independent of shader-local cbuffer names.
  for(const ResourceDescription &resource : device->GetReplay()->GetResources())
    if(resource.type == ResourceType::Buffer)
      BufferName(resource.resourceId, resource.name,
                 resource.autogeneratedName ? "generated_fallback" : "application", true);
  Write(path + ".shader-metadata.replay.json", "replay", reflection + "]");
}

// A separate trampoline per DLL is necessary: several compiler versions may coexist.
template <int Version>
struct Compiler
{
  static HookedFunction<decltype(&D3DCompile)> compile;
  static HookedFunction<decltype(&D3DCompile2)> compile2;
  static HookedFunction<decltype(&D3DCompileFromFile)> fromFile;
  static HookedFunction<decltype(&D3DStripShader)> strip;
  static HRESULT WINAPI Compile(LPCVOID src, SIZE_T size, LPCSTR name,
                                const D3D_SHADER_MACRO *defines, ID3DInclude *include, LPCSTR entry,
                                LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code,
                                ID3DBlob **errors)
  {
    bool record = depth == 0;
    Suppress guard;
    HRESULT hr =
        compile()(src, size, name, defines, include, entry, target, flags1, flags2, code, errors);
    if(record && SUCCEEDED(hr) && code && *code)
      Observation("D3DCompile", (*code)->GetBufferPointer(), (*code)->GetBufferSize(), {}, NULL, 0,
                  0, Version);
    return hr;
  }
  static HRESULT WINAPI Compile2(LPCVOID src, SIZE_T size, LPCSTR name,
                                 const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                                 LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                 UINT secondaryFlags, LPCVOID secondary, SIZE_T secondarySize,
                                 ID3DBlob **code, ID3DBlob **errors)
  {
    bool record = depth == 0;
    Suppress guard;
    HRESULT hr = compile2()(src, size, name, defines, include, entry, target, flags1, flags2,
                            secondaryFlags, secondary, secondarySize, code, errors);
    if(record && SUCCEEDED(hr) && code && *code)
      Observation("D3DCompile2", (*code)->GetBufferPointer(), (*code)->GetBufferSize(), {}, NULL, 0,
                  0, Version);
    return hr;
  }
  static HRESULT WINAPI FromFile(LPCWSTR file, const D3D_SHADER_MACRO *defines,
                                 ID3DInclude *include, LPCSTR entry, LPCSTR target, UINT flags1,
                                 UINT flags2, ID3DBlob **code, ID3DBlob **errors)
  {
    bool record = depth == 0;
    Suppress guard;
    HRESULT hr = fromFile()(file, defines, include, entry, target, flags1, flags2, code, errors);
    if(record && SUCCEEDED(hr) && code && *code)
      Observation("D3DCompileFromFile", (*code)->GetBufferPointer(), (*code)->GetBufferSize(), {},
                  NULL, 0, 0, Version);
    return hr;
  }
  static HRESULT WINAPI Strip(LPCVOID data, SIZE_T size, UINT flags, ID3DBlob **stripped)
  {
    bool record = depth == 0;
    Suppress guard;
    HRESULT hr = strip()(data, size, flags, stripped);
    if(record && SUCCEEDED(hr) && stripped && *stripped)
      Observation("D3DStripShader", data, size, {}, (*stripped)->GetBufferPointer(),
                  (*stripped)->GetBufferSize(), flags, Version);
    return hr;
  }
  static void Register()
  {
    rdcstr dll = StringFormat::Fmt("d3dcompiler_%d.dll", Version);
    LibraryHooks::RegisterLibraryHook(dll.c_str(), NULL);
    compile.Register(dll.c_str(), "D3DCompile", (void *)&Compile);
    compile2.Register(dll.c_str(), "D3DCompile2", (void *)&Compile2);
    fromFile.Register(dll.c_str(), "D3DCompileFromFile", (void *)&FromFile);
    strip.Register(dll.c_str(), "D3DStripShader", (void *)&Strip);
  }
};
template <int V>
HookedFunction<decltype(&D3DCompile)> Compiler<V>::compile;
template <int V>
HookedFunction<decltype(&D3DCompile2)> Compiler<V>::compile2;
template <int V>
HookedFunction<decltype(&D3DCompileFromFile)> Compiler<V>::fromFile;
template <int V>
HookedFunction<decltype(&D3DStripShader)> Compiler<V>::strip;
struct MetadataHooks : LibraryHook
{
  void RegisterHooks() override
  {
    if(!Enabled() || RiderDuck::Inst().IsReplayApp())
      return;
    Compiler<33>::Register();
    Compiler<34>::Register();
    Compiler<35>::Register();
    Compiler<36>::Register();
    Compiler<37>::Register();
    Compiler<38>::Register();
    Compiler<39>::Register();
    Compiler<40>::Register();
    Compiler<41>::Register();
    Compiler<42>::Register();
    Compiler<43>::Register();
    Compiler<44>::Register();
    Compiler<45>::Register();
    Compiler<46>::Register();
    Compiler<47>::Register();
  }
} hooks;
}
