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

#define OPENGL 1
#include "data/glsl/glsl_ubos_cpp.h"

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

    GL.glDeleteBuffers(1, &m_UBO);
    GL.glDeleteBuffers(1, &m_MathBuffer);
    GL.glDeleteBuffers(1, &m_SampleBuffer);

    GL.glDeleteTextures(1, &m_ReadbackTex);
    GL.glDeleteFramebuffers(1, &m_ReadbackFBO);
  }

  void ResetReplay()
  {
    CHECK_DEVICE_THREAD();
    if(!m_ResourcesDirty)
    {
      GLMarkerRegion region("ResetReplay");
      // replay the action to get back to 'normal' state for this event, and mark that we need to
      // replay back to pristine state next time we need to fetch data.
      m_pDriver->GetReplay()->ReplayLog(m_EventID, eReplay_OnlyDraw);
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

    DebugSampleUBO uniformParams = {};

    const bool buffer = (texType & DebugAPIWrapper::Buffer_Texture) != 0;
    const bool uintTex = (texType & DebugAPIWrapper::UInt_Texture) != 0;
    const bool sintTex = (texType & DebugAPIWrapper::SInt_Texture) != 0;

    // fetch the right type of descriptor depending on if we're buffer or not
    bool valid = true;
    rdcstr access = StringFormat::Fmt("performing %s operation", ToStr(opcode).c_str());
    const Descriptor &imageDescriptor = buffer ? GetDescriptor(access, ShaderBindIndex(), valid)
                                               : GetDescriptor(access, imageBind, valid);
    const Descriptor &bufferViewDescriptor = buffer
                                                 ? GetDescriptor(access, imageBind, valid)
                                                 : GetDescriptor(access, ShaderBindIndex(), valid);

    // fetch the sampler (if there's no sampler, this will silently return dummy data without
    // marking invalid
    const SamplerDescriptor &samplerDescriptor = GetSamplerDescriptor(access, samplerBind, valid);

    // if any descriptor lookup failed, return now
    if(!valid)
    {
      hasResult = false;
      return false;
    }

    GLMarkerRegion markerRegion("QueueSampleGather");

    GLResource texture = m_pDriver->GetResourceManager()->GetLiveResource(imageDescriptor.resource);
    GLResource bufTexture =
        m_pDriver->GetResourceManager()->GetLiveResource(bufferViewDescriptor.resource);
    GLResource sampler = m_pDriver->GetResourceManager()->GetLiveResource(samplerDescriptor.object);

    // NULL texture : return 0,0,0,0
    if(!buffer && (texture.name == 0))
    {
      memset(&output.value, 0, sizeof(output.value));
      hasResult = true;
      return true;
    }

    WrappedOpenGL::TextureData &texDetails =
        m_pDriver->m_Textures[m_pDriver->GetResourceManager()->GetLiveID(imageDescriptor.resource)];

    SamplingProgramConfig config;

    config.resType = SamplingProgramConfig::Float;
    if(uintTex)
      config.resType = SamplingProgramConfig::UInt;
    else if(sintTex)
      config.resType = SamplingProgramConfig::SInt;

    // how many co-ordinates should there be
    int coords = 0, gradCoords = 0;
    if(buffer)
    {
      config.dim = SamplingProgramConfig::TexBuffer;
      coords = gradCoords = 1;
    }
    else
    {
      switch(texDetails.curType)
      {
        case eGL_TEXTURE_1D:
          coords = 1;
          gradCoords = 1;
          config.dim = SamplingProgramConfig::Tex1D;
          break;
        case eGL_TEXTURE_2D:
          coords = 2;
          gradCoords = 2;
          config.dim = SamplingProgramConfig::Tex2D;
          break;
        case eGL_TEXTURE_3D:
          coords = 3;
          gradCoords = 3;
          config.dim = SamplingProgramConfig::Tex3D;
          break;
        case eGL_TEXTURE_CUBE_MAP:
          coords = 3;
          gradCoords = 3;
          config.dim = SamplingProgramConfig::TexCube;
          break;
        case eGL_TEXTURE_1D_ARRAY:
          coords = 2;
          gradCoords = 1;
          config.dim = SamplingProgramConfig::Tex1DArray;
          break;
        case eGL_TEXTURE_2D_ARRAY:
          coords = 3;
          gradCoords = 2;
          config.dim = SamplingProgramConfig::Tex2DArray;
          break;
        case eGL_TEXTURE_CUBE_MAP_ARRAY:
          coords = 4;
          gradCoords = 3;
          config.dim = SamplingProgramConfig::TexCubeArray;
          break;
        case eGL_TEXTURE_RECTANGLE:
          coords = 2;
          gradCoords = 2;
          config.dim = SamplingProgramConfig::Tex2DRect;
          break;
        case eGL_TEXTURE_BUFFER:
          coords = 1;
          gradCoords = 1;
          config.dim = SamplingProgramConfig::TexBuffer;
          break;
        case eGL_TEXTURE_2D_MULTISAMPLE:
          coords = 2;
          gradCoords = 2;
          config.dim = SamplingProgramConfig::Tex2DMS;
          break;
        case eGL_TEXTURE_2D_MULTISAMPLE_ARRAY:
          coords = 3;
          gradCoords = 2;
          config.dim = SamplingProgramConfig::Tex2DMSArray;
          break;
        default: RDCERR("Invalid texture type %s", ToStr(texDetails.curType).c_str()); return false;
      }
    }

    GLint firstMip = 0, numMips = 1;
    if(texture.name)
    {
      GL.glGetTextureParameterivEXT(texture.name, texDetails.curType, eGL_TEXTURE_BASE_LEVEL,
                                    &firstMip);
      GL.glGetTextureParameterivEXT(texture.name, texDetails.curType, eGL_TEXTURE_MAX_LEVEL,
                                    &numMips);
    }

    // handle query opcodes now
    switch(opcode)
    {
      case rdcspv::Op::ImageQueryLevels:
      {
        output.value.u32v[0] = numMips;
        hasResult = true;
        return true;
      }
      case rdcspv::Op::ImageQuerySamples:
      {
        output.value.u32v[0] = (uint32_t)RDCMAX(1, texDetails.samples);
        hasResult = true;
        return true;
      }
      case rdcspv::Op::ImageQuerySize:
      case rdcspv::Op::ImageQuerySizeLod:
      {
        uint32_t mip = firstMip;

        if(opcode == rdcspv::Op::ImageQuerySizeLod)
          mip += uintComp(lane.GetSrc(operands.lod), 0);

        RDCEraseEl(output.value);

        int i = 0;
        setUintComp(output, i++, RDCMAX(1, texDetails.width >> mip));
        if(coords >= 2)
          setUintComp(output, i++, RDCMAX(1, texDetails.height >> mip));
        if(texDetails.curType == eGL_TEXTURE_3D)
          setUintComp(output, i++, RDCMAX(1, texDetails.depth >> mip));

        if(texDetails.curType == eGL_TEXTURE_1D_ARRAY)
          setUintComp(output, i++, texDetails.height);
        else if(texDetails.curType == eGL_TEXTURE_2D_ARRAY)
          setUintComp(output, i++, texDetails.depth);
        else if(texDetails.curType == eGL_TEXTURE_CUBE_MAP ||
                texDetails.curType == eGL_TEXTURE_CUBE_MAP_ARRAY)
          setUintComp(output, i++, texDetails.depth / 6);

        if(buffer)
        {
          uint64_t size = bufferViewDescriptor.byteSize;
          GLenum format = MakeGLFormat(bufferViewDescriptor.format);

          setUintComp(
              output, 0,
              uint32_t(size / GetByteSize(1, 1, 1, GetBaseFormat(format), GetDataType(format))));
        }

        hasResult = true;
        return true;
      }
      default: break;
    }

    bool lodBiasRestore = false;
    float lodBiasRestoreValue = 0.0f;

    if(operands.flags & rdcspv::ImageOperands::Bias)
    {
      const ShaderVariable &biasVar = lane.GetSrc(operands.bias);

      // silently cast parameters to 32-bit floats
      float bias = floatComp(biasVar, 0);

      if(bias != 0.0f)
      {
        // bias can only be used with implicit lod operations, but we want to do everything with
        // explicit lod operations. So we instead push the bias into the sampler itself, which is
        // entirely equivalent.

        lodBiasRestore = true;
        if(sampler.name)
        {
          GL.glGetSamplerParameterfv(sampler.name, eGL_TEXTURE_LOD_BIAS, &lodBiasRestoreValue);
          GL.glSamplerParameterf(sampler.name, eGL_TEXTURE_LOD_BIAS, lodBiasRestoreValue + bias);
        }
        else
        {
          GL.glGetTextureParameterfvEXT(texture.name, texDetails.curType, eGL_TEXTURE_LOD_BIAS,
                                        &lodBiasRestoreValue);
          float val = lodBiasRestoreValue + bias;
          GL.glTextureParameterfvEXT(texture.name, texDetails.curType, eGL_TEXTURE_LOD_BIAS, &val);
        }
      }
    }

    switch(opcode)
    {
      case rdcspv::Op::ImageFetch: config.op = SamplingProgramConfig::Fetch; break;
      case rdcspv::Op::ImageQueryLod: config.op = SamplingProgramConfig::QueryLod; break;
      case rdcspv::Op::ImageSampleExplicitLod:
      case rdcspv::Op::ImageSampleImplicitLod:
      case rdcspv::Op::ImageSampleProjExplicitLod:
      case rdcspv::Op::ImageSampleProjImplicitLod: config.op = SamplingProgramConfig::Sample; break;
      case rdcspv::Op::ImageSampleDrefExplicitLod:
      case rdcspv::Op::ImageSampleDrefImplicitLod:
      case rdcspv::Op::ImageSampleProjDrefExplicitLod:
      case rdcspv::Op::ImageSampleProjDrefImplicitLod:
        config.op = SamplingProgramConfig::SampleDref;
        break;
      case rdcspv::Op::ImageGather: config.op = SamplingProgramConfig::Gather; break;
      case rdcspv::Op::ImageDrefGather: config.op = SamplingProgramConfig::GatherDref; break;
      default:
      {
        RDCERR("Unsupported opcode %s", ToStr(opcode).c_str());
        hasResult = false;
        return false;
      }
    }

    // proj opcodes have an extra q parameter, but we do the divide ourselves and 'demote' these to
    // non-proj variants
    bool proj = false;
    switch(opcode)
    {
      case rdcspv::Op::ImageSampleProjExplicitLod:
      case rdcspv::Op::ImageSampleProjImplicitLod:
      case rdcspv::Op::ImageSampleProjDrefExplicitLod:
      case rdcspv::Op::ImageSampleProjDrefImplicitLod:
      {
        proj = true;
        break;
      }
      default: break;
    }

    bool useCompare = false;
    switch(opcode)
    {
      case rdcspv::Op::ImageDrefGather:
      case rdcspv::Op::ImageSampleDrefExplicitLod:
      case rdcspv::Op::ImageSampleDrefImplicitLod:
      case rdcspv::Op::ImageSampleProjDrefExplicitLod:
      case rdcspv::Op::ImageSampleProjDrefImplicitLod:
      {
        useCompare = true;
        break;
      }
      default: break;
    }

    bool gatherOp = false;

    switch(opcode)
    {
      case rdcspv::Op::ImageFetch:
      {
        // co-ordinates after the used ones are read as 0s. This allows us to then read an implicit
        // 0 for array layer when we promote accesses to arrays.
        uniformParams.texel_uvw.x = uintComp(uv, 0);
        if(coords >= 2)
          uniformParams.texel_uvw.y = uintComp(uv, 1);
        if(coords >= 3)
          uniformParams.texel_uvw.z = uintComp(uv, 2);

        if(!buffer && operands.flags & rdcspv::ImageOperands::Lod)
          uniformParams.texel_lod = uintComp(lane.GetSrc(operands.lod), 0);
        else
          uniformParams.texel_lod = 0;

        if(operands.flags & rdcspv::ImageOperands::Sample)
          uniformParams.sampleIdx = uintComp(lane.GetSrc(operands.sample), 0);

        break;
      }
      case rdcspv::Op::ImageGather:
      case rdcspv::Op::ImageDrefGather:
      {
        gatherOp = true;

        // silently cast parameters to 32-bit floats
        for(int i = 0; i < coords; i++)
          uniformParams.uvwa.fv[i] = floatComp(uv, i);

        if(useCompare)
          uniformParams.compare = floatComp(compare, 0);

        config.gatherChannel = (uint32_t)gatherChannel;

        if(operands.flags & rdcspv::ImageOperands::ConstOffsets)
        {
          ShaderVariable constOffsets = lane.GetSrc(operands.constOffsets);

          config.useGatherOffs = true;

          // should be an array of ivec2
          RDCASSERT(constOffsets.members.size() == 4);

          // sign extend variables lower than 32-bits
          for(int i = 0; i < 4; i++)
          {
            if(constOffsets.members[i].type == VarType::SByte)
            {
              constOffsets.members[i].value.s32v[0] = constOffsets.members[i].value.s8v[0];
              constOffsets.members[i].value.s32v[1] = constOffsets.members[i].value.s8v[1];
            }
            else if(constOffsets.members[i].type == VarType::SShort)
            {
              constOffsets.members[i].value.s32v[0] = constOffsets.members[i].value.s16v[0];
              constOffsets.members[i].value.s32v[1] = constOffsets.members[i].value.s16v[1];
            }
          }

          config.gatherOffsets[0] = constOffsets.members[0].value.s32v[0];
          config.gatherOffsets[1] = constOffsets.members[0].value.s32v[1];
          config.gatherOffsets[2] = constOffsets.members[1].value.s32v[0];
          config.gatherOffsets[3] = constOffsets.members[1].value.s32v[1];
          config.gatherOffsets[4] = constOffsets.members[2].value.s32v[0];
          config.gatherOffsets[5] = constOffsets.members[2].value.s32v[1];
          config.gatherOffsets[6] = constOffsets.members[3].value.s32v[0];
          config.gatherOffsets[7] = constOffsets.members[3].value.s32v[1];
        }

        break;
      }
      case rdcspv::Op::ImageQueryLod:
      case rdcspv::Op::ImageSampleExplicitLod:
      case rdcspv::Op::ImageSampleImplicitLod:
      case rdcspv::Op::ImageSampleProjExplicitLod:
      case rdcspv::Op::ImageSampleProjImplicitLod:
      case rdcspv::Op::ImageSampleDrefExplicitLod:
      case rdcspv::Op::ImageSampleDrefImplicitLod:
      case rdcspv::Op::ImageSampleProjDrefExplicitLod:
      case rdcspv::Op::ImageSampleProjDrefImplicitLod:
      {
        // silently cast parameters to 32-bit floats
        for(int i = 0; i < coords; i++)
          uniformParams.uvwa.fv[i] = floatComp(uv, i);

        if(proj)
        {
          // coords shouldn't be 4 because that's only valid for cube arrays which can't be
          // projected
          RDCASSERT(coords < 4);

          // do the divide ourselves rather than severely complicating the sample shader (as proj
          // variants need non-arrayed textures)
          float q = floatComp(uv, coords);

          uniformParams.uvwa.fv[0] /= q;
          uniformParams.uvwa.fv[1] /= q;
          uniformParams.uvwa.fv[2] /= q;
        }

        if(operands.flags & rdcspv::ImageOperands::MinLod)
        {
          const ShaderVariable &minLodVar = lane.GetSrc(operands.minLod);

          // silently cast parameters to 32-bit floats
          uniformParams.minlod = floatComp(minLodVar, 0);
        }

        if(useCompare)
        {
          // silently cast parameters to 32-bit floats
          uniformParams.compare = floatComp(compare, 0);
        }

        if(operands.flags & rdcspv::ImageOperands::Lod)
        {
          const ShaderVariable &lodVar = lane.GetSrc(operands.lod);

          // silently cast parameters to 32-bit floats
          uniformParams.lod = floatComp(lodVar, 0);
          config.useGrad = false;
        }
        else if(operands.flags & rdcspv::ImageOperands::Grad)
        {
          ShaderVariable ddx = lane.GetSrc(operands.grad.first);
          ShaderVariable ddy = lane.GetSrc(operands.grad.second);

          config.useGrad = true;

          // silently cast parameters to 32-bit floats
          RDCASSERTEQUAL(ddx.type, ddy.type);
          for(int i = 0; i < gradCoords; i++)
          {
            uniformParams.ddx_uvw.fv[i] = floatComp(ddx, i);
            uniformParams.ddy_uvw.fv[i] = floatComp(ddy, i);
          }
        }

        if(opcode == rdcspv::Op::ImageSampleImplicitLod ||
           opcode == rdcspv::Op::ImageSampleProjImplicitLod || opcode == rdcspv::Op::ImageQueryLod)
        {
          // use grad to sub in for the implicit lod
          config.useGrad = true;

          // silently cast parameters to 32-bit floats
          RDCASSERTEQUAL(ddxCalc.type, ddyCalc.type);
          for(int i = 0; i < gradCoords; i++)
          {
            uniformParams.ddx_uvw.fv[i] = floatComp(ddxCalc, i);
            uniformParams.ddy_uvw.fv[i] = floatComp(ddyCalc, i);
          }
        }

        break;
      }
      default: break;
    }

    if(operands.flags & rdcspv::ImageOperands::ConstOffset)
    {
      ShaderVariable constOffset = lane.GetSrc(operands.constOffset);

      // sign extend variables lower than 32-bits
      for(uint8_t c = 0; c < constOffset.columns; c++)
      {
        if(constOffset.type == VarType::SByte)
          constOffset.value.s32v[c] = constOffset.value.s8v[c];
        else if(constOffset.type == VarType::SShort)
          constOffset.value.s32v[c] = constOffset.value.s16v[c];
      }

      // pass offsets as uniform where possible - when the feature (widely available) on gather
      // operations. On non-gather operations we are forced to use const offsets and must specialise
      // the pipeline.
      if(gatherOp)
      {
        uniformParams.dynoffset.x = constOffset.value.s32v[0];
        if(gradCoords >= 2)
          uniformParams.dynoffset.y = constOffset.value.s32v[1];
        if(gradCoords >= 3)
          uniformParams.dynoffset.z = constOffset.value.s32v[2];
      }
      else
      {
        config.fetchOffset.x = constOffset.value.s32v[0];
        if(gradCoords >= 2)
          config.fetchOffset.y = constOffset.value.s32v[1];
        if(gradCoords >= 3)
          config.fetchOffset.z = constOffset.value.s32v[2];
      }
    }
    else if(operands.flags & rdcspv::ImageOperands::Offset)
    {
      ShaderVariable offset = lane.GetSrc(operands.offset);

      // sign extend variables lower than 32-bits
      for(uint8_t c = 0; c < offset.columns; c++)
      {
        if(offset.type == VarType::SByte)
          offset.value.s32v[c] = offset.value.s8v[c];
        else if(offset.type == VarType::SShort)
          offset.value.s32v[c] = offset.value.s16v[c];
      }

      // if the app's shader used a dynamic offset, we can too!
      uniformParams.dynoffset.x = offset.value.s32v[0];
      if(gradCoords >= 2)
        uniformParams.dynoffset.y = offset.value.s32v[1];
      if(gradCoords >= 3)
        uniformParams.dynoffset.z = offset.value.s32v[2];
    }

    GLuint prog = m_pDriver->GetReplay()->MakeShaderDebugSampleProg(config);

    if(prog == 0)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 "Failed to compile graphics program for sampling operation");
      return false;
    }

    m_pDriver->GetReplay()->UseReplayContext();

    GLRenderState rs;
    rs.FetchState(m_pDriver);

    // do this 'lazily' so we are already inside the state push and pop
    if(m_UBO == 0)
    {
      GL.glGenBuffers(1, &m_UBO);
      GL.glBindBuffer(eGL_UNIFORM_BUFFER, m_UBO);
      GL.glNamedBufferDataEXT(m_UBO, 2048, NULL, eGL_DYNAMIC_DRAW);

      GL.glGenFramebuffers(1, &m_ReadbackFBO);
      GL.glBindFramebuffer(eGL_FRAMEBUFFER, m_ReadbackFBO);

      GL.glGenTextures(1, &m_ReadbackTex);
      GL.glBindTexture(eGL_TEXTURE_2D, m_ReadbackTex);

      GL.glBindBuffer(eGL_PIXEL_UNPACK_BUFFER, 0);
      GL.glTextureImage2DEXT(m_ReadbackTex, eGL_TEXTURE_2D, 0, eGL_RGBA32F, 1, 1, 0, eGL_RGBA,
                             eGL_FLOAT, NULL);
      GL.glTextureParameteriEXT(m_ReadbackTex, eGL_TEXTURE_2D, eGL_TEXTURE_MAX_LEVEL, 0);
      GL.glTextureParameteriEXT(m_ReadbackTex, eGL_TEXTURE_2D, eGL_TEXTURE_MIN_FILTER, eGL_NEAREST);
      GL.glTextureParameteriEXT(m_ReadbackTex, eGL_TEXTURE_2D, eGL_TEXTURE_MAG_FILTER, eGL_NEAREST);
      GL.glTextureParameteriEXT(m_ReadbackTex, eGL_TEXTURE_2D, eGL_TEXTURE_WRAP_S, eGL_CLAMP_TO_EDGE);
      GL.glTextureParameteriEXT(m_ReadbackTex, eGL_TEXTURE_2D, eGL_TEXTURE_WRAP_T, eGL_CLAMP_TO_EDGE);
      GL.glFramebufferTexture2D(eGL_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, eGL_TEXTURE_2D,
                                m_ReadbackTex, 0);
    }

    if(m_SampleOffset >= m_SampleBufferSize || m_SampleBuffer == 0)
    {
      m_SampleBufferSize = m_SampleBufferSize * 2 + 1024 * mathOpResultByteSize;

      GLuint oldBuf = m_SampleBuffer;
      GLsizeiptr oldSize = m_SampleBufferSize;

      // resize the buffer up
      GL.glGenBuffers(1, &m_SampleBuffer);
      GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, m_SampleBuffer);
      GL.glNamedBufferDataEXT(m_SampleBuffer, m_SampleBufferSize, NULL, eGL_DYNAMIC_DRAW);

      if(oldBuf)
        GL.glNamedCopyBufferSubDataEXT(oldBuf, m_SampleBuffer, 0, 0, oldSize);
      GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, 0);
    }

    GL.glUseProgram(prog);

    GL.glActiveTexture(eGL_TEXTURE0);
    if(texture.name)
      GL.glBindTexture(texDetails.curType, texture.name);
    if(bufTexture.name)
      GL.glBindTexture(eGL_TEXTURE_BUFFER, bufTexture.name);
    if(sampler.name)
      GL.glBindSampler(0, sampler.name);

    GL.glBindBufferBase(eGL_UNIFORM_BUFFER, 0, m_UBO);
    DebugSampleUBO *cdata =
        (DebugSampleUBO *)GL.glMapBufferRange(eGL_UNIFORM_BUFFER, 0, sizeof(DebugSampleUBO),
                                              GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

    memcpy(cdata, &uniformParams, sizeof(uniformParams));
    GL.glUnmapBuffer(eGL_UNIFORM_BUFFER);

    // set UVW/DDX/DDY for vertex shader
    GL.glUniform4fv(GL.glGetUniformLocation(prog, "in_uvwa"), 1, &uniformParams.uvwa.x);
    GL.glUniform4fv(GL.glGetUniformLocation(prog, "in_ddx"), 1, &uniformParams.ddx_uvw.x);
    GL.glUniform4fv(GL.glGetUniformLocation(prog, "in_ddy"), 1, &uniformParams.ddy_uvw.x);

    GL.glBindFramebuffer(eGL_FRAMEBUFFER, m_ReadbackFBO);

    float pixel[4] = {};
    GL.glClearBufferfv(eGL_COLOR, 0, pixel);

    if(HasExt[EXT_depth_bounds_test])
      GL.glDisable(eGL_DEPTH_BOUNDS_TEST_EXT);
    GL.glDisable(eGL_DEPTH_TEST);
    GL.glDisable(eGL_STENCIL_TEST);
    GL.glDisable(eGL_CULL_FACE);
    if(HasExt[ARB_texture_multisample_no_array] || HasExt[ARB_texture_multisample])
      GL.glDisable(eGL_SAMPLE_MASK);
    GL.glDisable(eGL_SCISSOR_TEST);
    GL.glDisable(eGL_BLEND);
    GL.glViewport(0, 0, 1, 1);
    GL.glDrawArrays(eGL_TRIANGLES, 0, 3);

    RDCASSERT(m_SampleOffset + sampleGatherOpResultByteSize <= m_SampleBufferSize, m_SampleOffset,
              sampleGatherOpResultByteSize, m_SampleBufferSize);

    GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, m_SampleBuffer);
    GL.glReadPixels(0, 0, 1, 1, eGL_RGBA, eGL_FLOAT, (void *)m_SampleOffset);
    GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, 0);
    m_SampleOffset += sampleGatherOpResultByteSize;

    hasResult = false;

    if(lodBiasRestore)
    {
      if(sampler.name)
        GL.glSamplerParameterf(sampler.name, eGL_TEXTURE_LOD_BIAS, lodBiasRestoreValue);
      else
        GL.glTextureParameterfvEXT(texture.name, texDetails.curType, eGL_TEXTURE_LOD_BIAS,
                                   &lodBiasRestoreValue);
    }

    rs.ApplyState(m_pDriver);

    return true;
  }

  virtual bool QueueCalculateMathOp(rdcspv::GLSLstd450 op,
                                    const rdcarray<ShaderVariable> &params) override
  {
    CHECK_DEVICE_THREAD();
    RDCASSERT(params.size() <= 3, params.size());

    RDCASSERTEQUAL(params[0].type, VarType::Float);

    GLMarkerRegion markerRegion("QueueCalculateMathOp");

    m_pDriver->GetReplay()->UseReplayContext();

    GLRenderState rs;
    rs.FetchState(m_pDriver);

    RDCCOMPILE_ASSERT(SPV_OpSin == (int)rdcspv::GLSLstd450::Sin, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpCos == (int)rdcspv::GLSLstd450::Cos, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpTan == (int)rdcspv::GLSLstd450::Tan, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAsin == (int)rdcspv::GLSLstd450::Asin, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAcos == (int)rdcspv::GLSLstd450::Acos, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAtan == (int)rdcspv::GLSLstd450::Atan, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpSinh == (int)rdcspv::GLSLstd450::Sinh, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpCosh == (int)rdcspv::GLSLstd450::Cosh, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpTanh == (int)rdcspv::GLSLstd450::Tanh, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAsinh == (int)rdcspv::GLSLstd450::Asinh,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAcosh == (int)rdcspv::GLSLstd450::Acosh,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAtanh == (int)rdcspv::GLSLstd450::Atanh,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpExp == (int)rdcspv::GLSLstd450::Exp, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpLog == (int)rdcspv::GLSLstd450::Log, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpExp2 == (int)rdcspv::GLSLstd450::Exp2, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpLog2 == (int)rdcspv::GLSLstd450::Log2, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpSqrt == (int)rdcspv::GLSLstd450::Sqrt, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpInverseSqrt == (int)rdcspv::GLSLstd450::InverseSqrt,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpNormalize == (int)rdcspv::GLSLstd450::Normalize,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpAtan2 == (int)rdcspv::GLSLstd450::Atan2,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpPow == (int)rdcspv::GLSLstd450::Pow, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpFma == (int)rdcspv::GLSLstd450::Fma, "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpLength == (int)rdcspv::GLSLstd450::Length,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpDistance == (int)rdcspv::GLSLstd450::Distance,
                      "Shader defines are mismatched");
    RDCCOMPILE_ASSERT(SPV_OpRefract == (int)rdcspv::GLSLstd450::Refract,
                      "Shader defines are mismatched");

    GLuint mathProg = m_pDriver->GetReplay()->GetShaderDebugMathProg();

    GL.glUniform1i(GL.glGetUniformLocation(mathProg, "outputs"), 0);

    if(m_MathOffset >= m_MathBufferSize || m_MathBuffer == 0)
    {
      m_MathBufferSize = m_MathBufferSize * 2 + 1024 * mathOpResultByteSize;

      GLuint oldBuf = m_MathBuffer;
      GLsizeiptr oldSize = m_MathBufferSize;

      // resize the buffer up
      GL.glGenBuffers(1, &m_MathBuffer);
      GL.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, m_MathBuffer);
      GL.glNamedBufferDataEXT(m_MathBuffer, m_MathBufferSize, NULL, eGL_DYNAMIC_DRAW);

      if(oldBuf)
        GL.glNamedCopyBufferSubDataEXT(oldBuf, m_MathBuffer, 0, 0, oldSize);
    }

    GL.glBindBufferRange(eGL_SHADER_STORAGE_BUFFER, 0, m_MathBuffer, (GLintptr)m_MathOffset,
                         (GLsizeiptr)mathOpResultByteSize);

    m_MathOffset += mathOpResultByteSize;

    GL.glUseProgram(mathProg);

    const char *names[] = {"a", "b", "c"};

    // push the parameters
    for(size_t i = 0; i < params.size(); i++)
    {
      RDCASSERTEQUAL(params[i].type, params[0].type);
      GL.glUniform4fv(GL.glGetUniformLocation(mathProg, names[i]), 1, params[i].value.f32v.data());
    }

    // push the operation afterwards
    GL.glUniform1ui(GL.glGetUniformLocation(mathProg, "op"), (uint32_t)op);

    GL.glDispatchCompute(1, 1, 1);

    rs.ApplyState(m_pDriver);

    return true;
  }

  virtual bool GetQueuedResults(rdcarray<ShaderVariable *> &mathOpResults,
                                rdcarray<ShaderVariable *> &sampleGatherResults) override
  {
    CHECK_DEVICE_THREAD();

    bytebuf gpuResults;
    gpuResults.resize(m_MathBufferSize + m_SampleBufferSize);
    if(m_MathBuffer)
    {
      GL.glBindBuffer(eGL_COPY_READ_BUFFER, m_MathBuffer);
      GL.glGetBufferSubData(eGL_COPY_READ_BUFFER, 0, m_MathBufferSize, gpuResults.data());
    }
    if(m_SampleBuffer)
    {
      GL.glBindBuffer(eGL_COPY_READ_BUFFER, m_SampleBuffer);
      GL.glGetBufferSubData(eGL_COPY_READ_BUFFER, 0, m_SampleBufferSize,
                            gpuResults.data() + m_MathBufferSize);
    }

    m_MathOffset = m_SampleOffset = 0;

    uintptr_t bufferEnd = (uintptr_t)gpuResults.end();

    byte *gpuMathOpResults = gpuResults.data();
    for(ShaderVariable *result : mathOpResults)
    {
      size_t countBytes = VarTypeByteSize(result->type) * result->columns;
      RDCASSERT((uintptr_t)gpuMathOpResults + countBytes <= bufferEnd, (uintptr_t)gpuMathOpResults,
                countBytes, bufferEnd);
      RDCASSERT(countBytes <= mathOpResultByteSize, countBytes, mathOpResultByteSize);
      memcpy(result->value.u32v.data(), gpuMathOpResults, countBytes);
      gpuMathOpResults += mathOpResultByteSize;
    }

    byte *gpuSampleGatherOpResults = gpuResults.data() + m_MathBufferSize;
    for(ShaderVariable *result : sampleGatherResults)
    {
      float *retf = (float *)gpuSampleGatherOpResults;
      uint32_t *retu = (uint32_t *)gpuSampleGatherOpResults;
      int32_t *reti = (int32_t *)gpuSampleGatherOpResults;

      size_t countBytes = 16;
      RDCASSERT((uintptr_t)gpuSampleGatherOpResults + countBytes <= bufferEnd,
                (uintptr_t)gpuSampleGatherOpResults, countBytes, bufferEnd);
      RDCASSERT(countBytes <= sampleGatherOpResultByteSize, countBytes, sampleGatherOpResultByteSize);
      // convert full precision results, we did all sampling at 32-bit precision
      ShaderVariable &output = *result;
      for(uint8_t c = 0; c < 4; c++)
      {
        if(VarTypeCompType(output.type) == CompType::Float)
          setFloatComp(output, c, retf[c]);
        else if(VarTypeCompType(output.type) == CompType::SInt)
          setIntComp(output, c, reti[c]);
        else
          setUintComp(output, c, retu[c]);
      }
      gpuSampleGatherOpResults += sampleGatherOpResultByteSize;
    }

    return true;
  }

  GLuint m_UBO = 0;
  GLuint m_ReadbackTex = 0;
  GLuint m_ReadbackFBO = 0;

  GLuint m_MathBuffer = 0;
  size_t m_MathBufferSize = 0;
  GLuint m_SampleBuffer = 0;
  size_t m_SampleBufferSize = 0;

  size_t m_MathOffset = 0, m_SampleOffset = 0;

  const size_t mathOpResultByteSize = sizeof(Vec4f) * 2;
  const size_t sampleGatherOpResultByteSize = sizeof(Vec4f);

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
        m_pDriver->GetReplay()->ReplayLog(m_EventID, eReplay_WithoutDraw);
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
        m_pDriver->GetReplay()->ReplayLog(m_EventID, eReplay_WithoutDraw);
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
