/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "gl_shaderdebug.h"
#include "driver/shaders/spirv/spirv_debug.h"
#include "driver/shaders/spirv/spirv_editor.h"
#include "driver/shaders/spirv/spirv_op_helpers.h"
#include "maths/formatpacking.h"
#include "replay/common/var_dispatch_helpers.h"
#include "gl_driver.h"
#include "gl_replay.h"

#if ENABLED(RDOC_DEVEL)
#define CHECK_DEVICE_THREAD() \
  RDCASSERTMSG("API Wrapper function called from non-device thread!", IsDeviceThread());
#else
#define CHECK_DEVICE_THREAD()
#endif

class GLAPIWrapper : public rdcspv::DebugAPIWrapper
{
public:
  GLAPIWrapper(WrappedOpenGL *gl, ShaderStage stage, uint32_t eid, ResourceId shadId)
      : m_EventID(eid), m_ShaderID(shadId), deviceThreadID(Threading::GetCurrentID())
  {
    m_pDriver = gl;

    // when we're first setting up, the state is pristine and no replay is needed
    m_ResourcesDirty = false;

    GLReplay *replay = m_pDriver->GetReplay();

    // cache the descriptor access. This should be a superset of all descriptors we need to read from
    m_Access = replay->GetDescriptorAccess(eid);

    // filter to only accesses from the stage we care about, as access lookups will be stage-specific
    m_Access.removeIf([stage](const DescriptorAccess &access) { return access.stage != stage; });

    // fetch all descriptor contents now too
    m_Descriptors.reserve(m_Access.size());
    m_SamplerDescriptors.reserve(m_Access.size());

    // we could collate ranges by descriptor store, but in practice we don't expect descriptors to
    // be scattered across multiple stores. So to keep the code simple for now we do a linear sweep
    ResourceId store;
    rdcarray<DescriptorRange> ranges;

    for(const DescriptorAccess &acc : m_Access)
    {
      if(acc.descriptorStore != store)
      {
        if(store != ResourceId())
        {
          m_Descriptors.append(replay->GetDescriptors(store, ranges));
          m_SamplerDescriptors.append(replay->GetSamplerDescriptors(store, ranges));
        }

        store = replay->GetLiveID(acc.descriptorStore);
        ranges.clear();
      }

      // if the last range is contiguous with this access, append this access as a new range to query
      if(!ranges.empty() && ranges.back().descriptorSize == acc.byteSize &&
         ranges.back().offset + ranges.back().descriptorSize == acc.byteOffset &&
         ranges.back().type == acc.type)
      {
        ranges.back().count++;
        continue;
      }

      DescriptorRange range = acc;
      ranges.push_back(range);
    }

    if(store != ResourceId())
    {
      m_Descriptors.append(replay->GetDescriptors(store, ranges));
      m_SamplerDescriptors.append(replay->GetSamplerDescriptors(store, ranges));
    }
  }

  ~GLAPIWrapper()
  {
    CHECK_DEVICE_THREAD();
    m_pDriver->glFlush();
    m_pDriver->glFinish();

    for(auto it = m_BiasSamplers.begin(); it != m_BiasSamplers.end(); it++)
      m_pDriver->glDeleteSamplers(1, &it->second);
  }

  void ResetReplay()
  {
    CHECK_DEVICE_THREAD();
    if(!m_ResourcesDirty)
    {
      GLMarkerRegion region("ResetReplay");
      // replay the action to get back to 'normal' state for this event, and mark that we need to
      // replay back to pristine state next time we need to fetch data.
      m_pDriver->ReplayLog(0, m_EventID, eReplay_OnlyDraw);
    }
    m_ResourcesDirty = true;
  }

  virtual void AddDebugMessage(MessageCategory cat, MessageSeverity sev, MessageSource src,
                               rdcstr desc) override
  {
    CHECK_DEVICE_THREAD();
    m_pDriver->AddDebugMessage(cat, sev, src, desc);
  }

  virtual bool SimulateThreaded() override { return false; }

  virtual ResourceId GetShaderID() override { return m_ShaderID; }

  virtual void ReadAddress(uint64_t address, uint64_t byteSize, void *dst) override
  {
    RDCERR("Unsupported address operation");
    return;
  }
  virtual void WriteAddress(uint64_t address, uint64_t byteSize, const void *src) override
  {
    RDCERR("Unsupported address operation");
    return;
  }
  virtual bool IsBufferCached(uint64_t address) override
  {
    RDCERR("Unsupported address operation");
    return false;
  }

  virtual uint64_t GetBufferLength(const ShaderBindIndex &bind) override
  {
    rdcspv::DeviceOpResult opResult;
    size_t length = 0;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        bind, [&length](bytebuf *data) { length = data->size(); }, opResult);
    RDCASSERT(succeeded);
    RDCASSERTEQUAL(opResult, rdcspv::DeviceOpResult::Succeeded);
    return length;
  }

  virtual void ReadBufferValue(const ShaderBindIndex &bind, uint64_t offset, uint64_t byteSize,
                               void *dst) override
  {
    rdcspv::DeviceOpResult opResult;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        bind,
        [offset, byteSize, dst](bytebuf *data) {
          if(offset + byteSize <= data->size())
            memcpy(dst, data->data() + (size_t)offset, (size_t)byteSize);
        },
        opResult);
    RDCASSERT(succeeded);
    RDCASSERTEQUAL(opResult, rdcspv::DeviceOpResult::Succeeded);
  }

  virtual void WriteBufferValue(const ShaderBindIndex &bind, uint64_t offset, uint64_t byteSize,
                                const void *src) override
  {
    rdcspv::DeviceOpResult opResult;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        bind,
        [offset, byteSize, src](bytebuf *data) {
          if(offset + byteSize <= data->size())
            memcpy(data->data() + (size_t)offset, src, (size_t)byteSize);
        },
        opResult);
    RDCASSERT(succeeded);
    RDCASSERTEQUAL(opResult, rdcspv::DeviceOpResult::Succeeded);
  }

  // Called from any thread
  // Caller guarantees that if the image data is not cached then we are on the device thread
  virtual rdcspv::DeviceOpResult ReadTexel(const ShaderBindIndex &imageBind,
                                           const ShaderVariable &coord, uint32_t sample,
                                           ShaderVariable &output) override
  {
    rdcspv::DeviceOpResult opResult;
    bool isCached = false;
    {
      SCOPED_READLOCK(imageCacheLock);
      isCached = GetImageDataFromCache(imageBind, opResult) != NULL;
      RDCASSERTNOTEQUAL(opResult, rdcspv::DeviceOpResult::NeedsDevice);
    }

    if(!isCached)
    {
      // Add image data to the cache : cache should not be locked by this thread
      PopulateImage(imageBind);
    }

    {
      SCOPED_READLOCK(imageCacheLock);
      ImageData *result = GetImageDataFromCache(imageBind, opResult);
      if(!result)
      {
        RDCASSERTEQUAL(opResult, rdcspv::DeviceOpResult::Failed);
        return rdcspv::DeviceOpResult::Failed;
      }

      ImageData &data = *result;
      if(data.width == 0)
        return rdcspv::DeviceOpResult::Failed;

      uint32_t coords[4];
      for(int i = 0; i < 4; i++)
        coords[i] = uintComp(coord, i);

      if(coords[0] >= data.width || coords[1] >= data.height || coords[2] >= data.depth)
      {
        if(!IsDeviceThread())
          return rdcspv::DeviceOpResult::NeedsDevice;

        CHECK_DEVICE_THREAD();
        m_pDriver->AddDebugMessage(
            MessageCategory::Execution, MessageSeverity::High, MessageSource::RuntimeWarning,
            StringFormat::Fmt(
                "Out of bounds access to image, coord %u,%u,%u outside of dimensions %ux%ux%u",
                coords[0], coords[1], coords[2], data.width, data.height, data.depth));
        return rdcspv::DeviceOpResult::Failed;
      }

      CompType varComp = VarTypeCompType(output.type);

      set0001(output);

      ShaderVariable input;
      input.columns = data.fmt.compCount;

      // the only 'irregular' format we need to worry about handling for integer types is
      // 10:10:10:2. All others are float/uint
      if(data.fmt.type == ResourceFormatType::R10G10B10A2)
      {
        PixelValue val;
        DecodePixelData(data.fmt, data.texel(coords, sample), val);

        if(data.fmt.compType == CompType::UInt)
          input.type = VarType::UInt;
        else if(data.fmt.compType == CompType::SInt)
          input.type = VarType::SInt;
        else
          input.type = VarType::Float;

        memcpy(input.value.u32v.data(), val.uintValue.data(), val.uintValue.byteSize());

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
        {
          if(data.fmt.compType == CompType::UInt)
            setUintComp(output, c, uintComp(input, c));
          else if(data.fmt.compType == CompType::SInt)
            setIntComp(output, c, intComp(input, c));
          else
            setFloatComp(output, c, input.value.f32v[c]);
        }
      }
      else if(data.fmt.compType == CompType::UInt)
      {
        RDCASSERT(varComp == CompType::UInt, varComp);

        // set up input type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          input.type = VarType::UByte;
        else if(data.fmt.compByteWidth == 2)
          input.type = VarType::UShort;
        else if(data.fmt.compByteWidth == 4)
          input.type = VarType::UInt;
        else if(data.fmt.compByteWidth == 8)
          input.type = VarType::ULong;

        memcpy(input.value.u8v.data(), data.texel(coords, sample), data.texelSize);

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
          setUintComp(output, c, uintComp(input, c));
      }
      else if(data.fmt.compType == CompType::SInt)
      {
        RDCASSERT(varComp == CompType::SInt, varComp);

        // set up input type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          input.type = VarType::SByte;
        else if(data.fmt.compByteWidth == 2)
          input.type = VarType::SShort;
        else if(data.fmt.compByteWidth == 4)
          input.type = VarType::SInt;
        else if(data.fmt.compByteWidth == 8)
          input.type = VarType::SLong;

        memcpy(input.value.u8v.data(), data.texel(coords, sample), data.texelSize);

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
          setIntComp(output, c, intComp(input, c));
      }
      else
      {
        RDCASSERT(varComp == CompType::Float, varComp);

        // do the decode of whatever unorm/float/etc the format is
        FloatVector v = DecodeFormattedComponents(data.fmt, data.texel(coords, sample));

        // set it into f32v
        input.value.f32v[0] = v.x;
        input.value.f32v[1] = v.y;
        input.value.f32v[2] = v.z;
        input.value.f32v[3] = v.w;

        // read as floats
        input.type = VarType::Float;

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
          setFloatComp(output, c, input.value.f32v[c]);
      }
    }

    return rdcspv::DeviceOpResult::Succeeded;
  }

  // Called from any thread
  // Caller guarantees that if the image data is not cached then we are on the device thread
  virtual rdcspv::DeviceOpResult WriteTexel(const ShaderBindIndex &imageBind,
                                            const ShaderVariable &coord, uint32_t sample,
                                            const ShaderVariable &input) override
  {
    rdcspv::DeviceOpResult opResult;
    ImageData *result = NULL;
    {
      SCOPED_READLOCK(imageCacheLock);
      result = GetImageDataFromCache(imageBind, opResult);
      RDCASSERTNOTEQUAL(opResult, rdcspv::DeviceOpResult::NeedsDevice);
    }

    if(!result)
    {
      // Add image data to the cache : cache should not be locked by this thread
      PopulateImage(imageBind);
    }

    {
      SCOPED_READLOCK(imageCacheLock);
      result = GetImageDataFromCache(imageBind, opResult);
      if(!result)
        return rdcspv::DeviceOpResult::Failed;

      ImageData &data = *result;
      if(data.width == 0)
        return rdcspv::DeviceOpResult::Failed;

      uint32_t coords[4];
      for(int i = 0; i < 4; i++)
        coords[i] = uintComp(coord, i);

      if(coords[0] >= data.width || coords[1] >= data.height || coords[2] >= data.depth)
      {
        if(!IsDeviceThread())
          return rdcspv::DeviceOpResult::NeedsDevice;

        CHECK_DEVICE_THREAD();
        m_pDriver->AddDebugMessage(
            MessageCategory::Execution, MessageSeverity::High, MessageSource::RuntimeWarning,
            StringFormat::Fmt(
                "Out of bounds access to image, coord %u,%u,%u outside of dimensions %ux%ux%u",
                coords[0], coords[1], coords[2], data.width, data.height, data.depth));
        return rdcspv::DeviceOpResult::Failed;
      }

      CompType varComp = VarTypeCompType(input.type);

      ShaderVariable output;
      output.columns = data.fmt.compCount;

      // the only 'irregular' format we need to worry about handling for integer types is
      // 10:10:10:2. All others are float/uint
      if(data.fmt.type == ResourceFormatType::R10G10B10A2)
      {
        // image writes are required to write a whole texel so we know we should have 4 components
        RDCASSERTEQUAL(input.columns, 4);

        uint32_t encoded = 0;

        if(data.fmt.compType == CompType::SNorm)
          encoded = ConvertToR10G10B10A2SNorm(Vec4f(input.value.f32v[0], input.value.f32v[1],
                                                    input.value.f32v[2], input.value.f32v[3]));
        else if(data.fmt.compType == CompType::UInt)
          encoded = ConvertToR10G10B10A2(Vec4u(input.value.u32v[0], input.value.u32v[1],
                                               input.value.u32v[2], input.value.u32v[3]));
        else
          encoded = ConvertToR10G10B10A2(Vec4f(input.value.f32v[0], input.value.f32v[1],
                                               input.value.f32v[2], input.value.f32v[3]));

        memcpy(data.texel(coords, sample), &encoded, sizeof(uint32_t));
      }
      else if(data.fmt.compType == CompType::UInt)
      {
        RDCASSERT(varComp == CompType::UInt, varComp);

        // set up output type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          output.type = VarType::UByte;
        else if(data.fmt.compByteWidth == 2)
          output.type = VarType::UShort;
        else if(data.fmt.compByteWidth == 4)
          output.type = VarType::UInt;
        else if(data.fmt.compByteWidth == 8)
          output.type = VarType::ULong;

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
          setUintComp(output, c, uintComp(input, c));

        memcpy(data.texel(coords, sample), output.value.u8v.data(), data.texelSize);
      }
      else if(data.fmt.compType == CompType::SInt)
      {
        RDCASSERT(varComp == CompType::SInt, varComp);

        // set up input type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          output.type = VarType::SByte;
        else if(data.fmt.compByteWidth == 2)
          output.type = VarType::SShort;
        else if(data.fmt.compByteWidth == 4)
          output.type = VarType::SInt;
        else if(data.fmt.compByteWidth == 8)
          output.type = VarType::SLong;

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
          setIntComp(output, c, intComp(input, c));

        memcpy(data.texel(coords, sample), output.value.u8v.data(), data.texelSize);
      }
      else
      {
        RDCASSERT(varComp == CompType::Float, varComp);

        // read as floats
        output.type = VarType::Float;

        for(uint8_t c = 0; c < RDCMIN(output.columns, input.columns); c++)
          setFloatComp(output, c, input.value.f32v[c]);

        FloatVector v;

        // set it into f32v
        v.x = input.value.f32v[0];
        v.y = input.value.f32v[1];
        v.z = input.value.f32v[2];
        v.w = input.value.f32v[3];

        EncodeFormattedComponents(data.fmt, v, data.texel(coords, sample));
      }
    }
    return rdcspv::DeviceOpResult::Succeeded;
  }

  virtual void FillInputValue(ShaderVariable &var, ShaderBuiltin builtin, uint32_t threadIndex,
                              uint32_t location, uint32_t component) override
  {
    CHECK_DEVICE_THREAD();
    if(builtin != ShaderBuiltin::Undefined)
    {
      if(threadIndex < thread_builtins.size())
      {
        auto it = thread_builtins[threadIndex].find(builtin);
        if(it != thread_builtins[threadIndex].end())
        {
          var.value = it->second.value;
          return;
        }
      }

      auto it = global_builtins.find(builtin);
      if(it != global_builtins.end())
      {
        var.value = it->second.value;
        return;
      }

      RDCERR("Couldn't get input for %s", ToStr(builtin).c_str());
      return;
    }

    if(threadIndex < location_inputs.size())
    {
      if(location < location_inputs[threadIndex].size())
      {
        if(var.rows == 1)
        {
          if(component + var.columns > 4)
            RDCERR("Unexpected component %u for column count %u", component, var.columns);

          for(uint8_t c = 0; c < var.columns; c++)
            copyComp(var, c, location_inputs[threadIndex][location], component + c);
        }
        else
        {
          RDCASSERTEQUAL(component, 0);
          for(uint8_t r = 0; r < var.rows; r++)
            for(uint8_t c = 0; c < var.columns; c++)
              copyComp(var, r * var.columns + c, location_inputs[threadIndex][location + c], r);
        }
        return;
      }
    }

    RDCERR("Couldn't get input for %s at thread=%u, location=%u, component=%u", var.name.c_str(),
           threadIndex, location, component);
  }

  uint32_t GetThreadProperty(uint32_t threadIndex, rdcspv::ThreadProperty prop) override
  {
    CHECK_DEVICE_THREAD();
    if(prop >= rdcspv::ThreadProperty::Count)
      return 0;
    if(threadIndex >= thread_props.size())
      return 0;

    return thread_props[threadIndex][(size_t)prop];
  }

  bool QueueSampleGather(rdcspv::ThreadState &lane, rdcspv::Op opcode,
                         DebugAPIWrapper::TextureType texType, const ShaderBindIndex &imageBind,
                         const ShaderBindIndex &samplerBind, const ShaderVariable &uv,
                         const ShaderVariable &ddxCalc, const ShaderVariable &ddyCalc,
                         const ShaderVariable &compare, rdcspv::GatherChannel gatherChannel,
                         const rdcspv::ImageOperandsAndParamDatas &operands, ShaderVariable &output,
                         bool &hasResult) override
  {
    CHECK_DEVICE_THREAD();
    return true;
  }

  virtual bool QueueCalculateMathOp(rdcspv::GLSLstd450 op,
                                    const rdcarray<ShaderVariable> &params) override
  {
    CHECK_DEVICE_THREAD();
    return true;
  }

  virtual bool GetQueuedResults(rdcarray<ShaderVariable *> &mathOpResults,
                                rdcarray<ShaderVariable *> &sampleGatherResults) override
  {
    CHECK_DEVICE_THREAD();
    return false;
  }

  virtual bool QueuedOpsHasSpace() override { return true; }

  // global over all threads
  std::unordered_map<ShaderBuiltin, ShaderVariable> global_builtins;

  // per-thread builtins
  rdcarray<std::unordered_map<ShaderBuiltin, ShaderVariable>> thread_builtins;

  // per-thread custom inputs by location [thread][location]
  rdcarray<rdcarray<ShaderVariable>> location_inputs;

  rdcarray<rdcfixedarray<uint32_t, arraydim<rdcspv::ThreadProperty>()>> thread_props;

  uint64_t GetDeviceThreadID() const { return deviceThreadID; }
  bool IsDeviceThread() const { return Threading::GetCurrentID() == GetDeviceThreadID(); }

private:
  WrappedOpenGL *m_pDriver = NULL;

  bool m_ResourcesDirty = false;
  uint32_t m_EventID;
  ResourceId m_ShaderID;

  rdcarray<DescriptorAccess> m_Access;
  rdcarray<Descriptor> m_Descriptors;
  rdcarray<SamplerDescriptor> m_SamplerDescriptors;

  typedef rdcpair<ResourceId, float> SamplerBiasKey;
  std::map<SamplerBiasKey, GLuint> m_BiasSamplers;

  Threading::RWLock bufferCacheLock;
  std::map<ShaderBindIndex, bytebuf> bufferCache;

  struct ImageData
  {
    uint32_t width = 0, height = 0, depth = 0;
    uint32_t texelSize = 0;
    uint64_t rowPitch = 0, slicePitch = 0, samplePitch = 0;
    ResourceFormat fmt;
    bytebuf bytes;

    byte *texel(const uint32_t *coord, uint32_t sample)
    {
      byte *ret = bytes.data();

      ret += samplePitch * sample;
      ret += slicePitch * coord[2];
      ret += rowPitch * coord[1];
      ret += texelSize * coord[0];

      return ret;
    }
  };

  Threading::RWLock imageCacheLock;
  std::map<ShaderBindIndex, ImageData> imageCache;

  const Descriptor &GetDescriptor(const rdcstr &access, const ShaderBindIndex &index, bool &valid)
  {
    CHECK_DEVICE_THREAD();
    static Descriptor dummy;

    if(index.category == DescriptorCategory::Unknown)
    {
      // invalid index, return a dummy data but don't mark as invalid
      return dummy;
    }

    int32_t a = m_Access.indexOf(index);

    // this should not happen unless the debugging references an array element that we didn't
    // detect dynamically. We could improve this by retrieving a more conservative access set
    // internally so that all descriptors are 'accessed'
    if(a < 0)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 StringFormat::Fmt("Internal error: Binding %s %u[%u] did not "
                                                   "exist in calculated descriptor access when %s.",
                                                   ToStr(index.category).c_str(), index.index,
                                                   index.arrayElement, access.c_str()));
      valid = false;
      return dummy;
    }

    return m_Descriptors[a];
  }

  const SamplerDescriptor &GetSamplerDescriptor(const rdcstr &access, const ShaderBindIndex &index,
                                                bool &valid)
  {
    CHECK_DEVICE_THREAD();
    static SamplerDescriptor dummy;

    if(index.category == DescriptorCategory::Unknown)
    {
      // invalid index, return a dummy data but don't mark as invalid
      return dummy;
    }

    int32_t a = m_Access.indexOf(index);

    // this should not happen unless the debugging references an array element that we didn't
    // detect dynamically. We could improve this by retrieving a more conservative access set
    // internally so that all descriptors are 'accessed'
    if(a < 0)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 StringFormat::Fmt("Internal error: Binding %s %u[%u] did not "
                                                   "exist in calculated descriptor access when %s.",
                                                   ToStr(index.category).c_str(), index.index,
                                                   index.arrayElement, access.c_str()));
      valid = false;
      return dummy;
    }

    return m_SamplerDescriptors[a];
  }

  // Called from any thread
  bool IsBufferCached(const ShaderBindIndex &bind) override
  {
    SCOPED_READLOCK(bufferCacheLock);
    return bufferCache.find(bind) != bufferCache.end();
  }

  // Called from any thread
  bytebuf *GetBufferDataFromCache(const ShaderBindIndex &bind, rdcspv::DeviceOpResult &opResult)
  {
    // Calling function responsible for acquiring bufferCache Read lock
    auto findIt = bufferCache.find(bind);
    if(findIt != bufferCache.end())
    {
      opResult = rdcspv::DeviceOpResult::Succeeded;
      return &findIt->second;
    }

    opResult = rdcspv::DeviceOpResult::Failed;

    // Not in the cache : populate must happen on the device thread
    if(!IsDeviceThread())
      opResult = rdcspv::DeviceOpResult::NeedsDevice;

    return NULL;
  }

  // Called from any thread
  bool BufferFunction(const ShaderBindIndex &bind, const std::function<void(bytebuf *data)> &func,
                      rdcspv::DeviceOpResult &opResult)
  {
    bool isCached = false;
    {
      SCOPED_READLOCK(bufferCacheLock);
      isCached = GetBufferDataFromCache(bind, opResult) != NULL;
      if(opResult == rdcspv::DeviceOpResult::NeedsDevice)
        return false;
    }

    if(!isCached)
    {
      // Add buffer data to the cache : cache should not be locked by this thread
      PopulateBuffer(bind);
    }

    {
      SCOPED_READLOCK(bufferCacheLock);
      bytebuf *result = GetBufferDataFromCache(bind, opResult);
      if(result)
      {
        // Guarantee the buffer cache readlock whilst the function is called
        func(result);
        return true;
      }

      RDCASSERTEQUAL(opResult, rdcspv::DeviceOpResult::Failed);
      opResult = rdcspv::DeviceOpResult::Failed;
      return false;
    }
  }

  // Must be called from the replay manager thread (the debugger thread)
  void PopulateBuffer(const ShaderBindIndex &bind)
  {
    CHECK_DEVICE_THREAD();
    bytebuf data;

    bool valid = true;
    const Descriptor &bufData = GetDescriptor("accessing buffer value", bind, valid);
    if(valid)
    {
      // if the resources might be dirty from side-effects from the action, replay back to right
      // before it.
      if(m_ResourcesDirty)
      {
        GLMarkerRegion region("un-dirtying resources");
        m_pDriver->ReplayLog(0, m_EventID, eReplay_WithoutDraw);
        m_ResourcesDirty = false;
      }

      if(bufData.resource != ResourceId())
      {
        m_pDriver->GetReplay()->GetBufferData(
            m_pDriver->GetResourceManager()->GetLiveID(bufData.resource), bufData.byteOffset,
            bufData.byteSize, data);
      }
    }

    {
      // Insert atomically with all the data filled in : to prevent race conditions
      SCOPED_WRITELOCK(bufferCacheLock);
      auto insertIt = bufferCache.insert(std::make_pair(bind, data));
      RDCASSERT(insertIt.second);
    }
  }

  // Must be called from the replay manager thread (the debugger thread)
  void PopulateImage(const ShaderBindIndex &bind)
  {
    CHECK_DEVICE_THREAD();
    ImageData data;
    bool valid = true;
    const Descriptor &imgData = GetDescriptor("performing image load/store", bind, valid);
    if(valid)
    {
      // if the resources might be dirty from side-effects from the action, replay back to right
      // before it.
      if(m_ResourcesDirty)
      {
        GLMarkerRegion region("un-dirtying resources");
        m_pDriver->ReplayLog(0, m_EventID, eReplay_WithoutDraw);
        m_ResourcesDirty = false;
      }

      if(imgData.type == DescriptorType::TypedBuffer ||
         imgData.type == DescriptorType::ReadWriteTypedBuffer)
      {
        ResourceId buffer = m_pDriver->GetResourceManager()->GetLiveID(imgData.resource);
        uint64_t offset = imgData.byteOffset;
        GLenum format = MakeGLFormat(imgData.format);
        uint64_t byteWidth = imgData.byteSize;

        data.fmt = imgData.format;
        data.texelSize = (uint32_t)GetByteSize(1, 1, 1, GetBaseFormat(format), GetDataType(format));

        // convert to a texel width, rounding down as per spec
        data.width = uint32_t(byteWidth / data.texelSize);
        data.height = 1;
        data.depth = 1;

        data.samplePitch = data.slicePitch = data.rowPitch = data.width * data.texelSize;

        m_pDriver->GetReplay()->GetBufferData(
            m_pDriver->GetResourceManager()->GetLiveID(imgData.resource), offset, data.rowPitch,
            data.bytes);
      }
      else if(imgData.resource != ResourceId())
      {
        ResourceId id = m_pDriver->GetResourceManager()->GetLiveID(imgData.resource);
        const WrappedOpenGL::TextureData &texProps = m_pDriver->m_Textures[id];

        uint32_t mip = imgData.firstMip;

        data.width = RDCMAX(1, texProps.width >> mip);
        data.height = RDCMAX(1, texProps.height >> mip);
        if(texProps.curType == eGL_TEXTURE_3D)
        {
          data.depth = RDCMAX(1, texProps.depth >> mip);
        }
        else
        {
          data.depth = texProps.depth;
        }

        GLenum format = MakeGLFormat(imgData.format);

        data.fmt = imgData.format;
        data.texelSize = (uint32_t)GetByteSize(1, 1, 1, GetBaseFormat(format), GetDataType(format));
        data.rowPitch =
            (uint32_t)GetByteSize(data.width, 1, 1, GetBaseFormat(format), GetDataType(format));
        data.slicePitch =
            GetByteSize(data.width, data.height, 1, GetBaseFormat(format), GetDataType(format));
        data.samplePitch = GetByteSize(data.width, data.height, data.depth, GetBaseFormat(format),
                                       GetDataType(format));

        const uint32_t numSlices = texProps.curType == eGL_TEXTURE_3D ? 1 : data.depth;
        const uint32_t numSamples = (uint32_t)RDCMAX(1, texProps.samples);

        data.bytes.reserve(size_t(data.samplePitch * numSamples));

        // defaults are fine - no interpretation. Maybe we could use the view's typecast?
        const GetTextureDataParams params = GetTextureDataParams();

        for(uint32_t sample = 0; sample < numSamples; sample++)
        {
          for(uint32_t slice = 0; slice < numSlices; slice++)
          {
            bytebuf subBytes;
            m_pDriver->GetReplay()->GetTextureData(id, Subresource(mip, slice, sample), params,
                                                   subBytes);

            // fast path, swap into output if there's only one slice and one sample (common case)
            if(numSlices == 1 && numSamples == 1)
            {
              subBytes.swap(data.bytes);
            }
            else
            {
              data.bytes.append(subBytes);
            }
          }
        }
      }
    }

    {
      // Insert atomically with all the data filled in : to prevent race conditions
      SCOPED_WRITELOCK(imageCacheLock);
      auto insertIt = imageCache.insert(std::make_pair(bind, data));
      RDCASSERT(insertIt.second);
    }
  }

  // Called from any thread
  bool IsImageCached(const ShaderBindIndex &bind) override
  {
    SCOPED_READLOCK(imageCacheLock);
    return imageCache.find(bind) != imageCache.end();
  }

  // Called from any thread
  ImageData *GetImageDataFromCache(const ShaderBindIndex &bind, rdcspv::DeviceOpResult &opResult)
  {
    // Calling function responsible for acquiring imageCache Read lock
    auto findIt = imageCache.find(bind);
    if(findIt != imageCache.end())
    {
      opResult = rdcspv::DeviceOpResult::Succeeded;
      return &findIt->second;
    }

    opResult = rdcspv::DeviceOpResult::Failed;

    // Not in the cache : populate must happen on the device thread
    if(!IsDeviceThread())
      opResult = rdcspv::DeviceOpResult::NeedsDevice;

    return NULL;
  }

  const uint64_t deviceThreadID;
};

ShaderDebugTrace *GLReplay::DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid,
                                        uint32_t idx, uint32_t view)
{
  GLNOTIMP("DebugVertex");
  return new ShaderDebugTrace();
}

ShaderDebugTrace *GLReplay::DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                                       const DebugPixelInputs &inputs)
{
  GLNOTIMP("DebugPixel");
  return new ShaderDebugTrace();
}

ShaderDebugTrace *GLReplay::DebugThread(uint32_t eventId, const rdcfixedarray<uint32_t, 3> &groupid,
                                        const rdcfixedarray<uint32_t, 3> &threadid)
{
  GLNOTIMP("DebugThread");
  return new ShaderDebugTrace();
}

ShaderDebugTrace *GLReplay::DebugMeshThread(uint32_t eventId,
                                            const rdcfixedarray<uint32_t, 3> &groupid,
                                            const rdcfixedarray<uint32_t, 3> &threadid)
{
  GLNOTIMP("DebugMeshThread");
  return new ShaderDebugTrace();
}

rdcarray<ShaderDebugState> GLReplay::ContinueDebug(ShaderDebugger *debugger)
{
  return {};
}

void GLReplay::FreeDebugger(ShaderDebugger *debugger)
{
  delete debugger;
}
