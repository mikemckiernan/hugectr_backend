// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "memory"
#include "thread"
#include "vector"
#include "map"
#include "cstdlib"
#include "dlfcn.h"
#include "dirent.h"
#include "cuda_runtime_api.h"
#include "triton/backend/backend_common.h"
#include "inference/hugectrmodel.hpp"
#include "inference/inference_utils.hpp"
#include "inference/embedding_interface.hpp"

namespace triton { namespace backend { namespace hugectr {

enum MEMORY_TYPE { GPU, CPU,PIN};

//
// Simple backend that demonstrates the TRITONBACKEND API for a
// blocking backend. A blocking backend completes execution of the
// inference before returning from TRITONBACKED_ModelInstanceExecute.
//
// HugeCTR Backend supports any model that tranined by HugeCTR, which
// has exactly 3 input and exactly 1 output. The input and output should 
// define the name as "DES","CATCOLUMN" and "ROWINDEX", datatype as FP32, 
// UINT32  and INT32, the shape and datatype of the input and output must
// match. The backend  responds with the output tensor contains the prediction result
// 

#define GUARDED_RESPOND_IF_ERROR(RESPONSES, IDX, X)                     \
  do {                                                                  \
    if ((RESPONSES)[IDX] != nullptr) {                                  \
      TRITONSERVER_Error* err__ = (X);                                  \
      if (err__ != nullptr) {                                           \
        LOG_IF_ERROR(                                                   \
            TRITONBACKEND_ResponseSend(                                 \
                (RESPONSES)[IDX], TRITONSERVER_RESPONSE_COMPLETE_FINAL, \
                err__),                                                 \
            "failed to send error response");                           \
        (RESPONSES)[IDX] = nullptr;                                     \
        TRITONSERVER_ErrorDelete(err__);                                \
      }                                                                 \
    }                                                                   \
  } while (false)

//An internal exception to carry the HugeCTR CUDA error code
#define CK_CUDA_THROW_(x)                                                                          \
  do {                                                                                             \
    cudaError_t retval =  (x);                                                                      \
    if (retval != cudaSuccess) {                                                                   \
      throw std::runtime_error(std::string("Runtime error: ") + (cudaGetErrorString(retval)) + \
                                       " " + __FILE__ + ":" + std::to_string(__LINE__) + " \n");   \
    }                                                                                              \
  } while (0)

#define RESPOND_AND_RETURN_IF_ERROR(REQUEST, X)                         \
  do {                                                                  \
    TRITONSERVER_Error* rarie_err__ = (X);                              \
    if (rarie_err__ != nullptr) {                                       \
      TRITONBACKEND_Response* rarie_response__ = nullptr;               \
      LOG_IF_ERROR(                                                     \
          TRITONBACKEND_ResponseNew(&rarie_response__, REQUEST),        \
          "failed to create response");                                 \
      if (rarie_response__ != nullptr) {                                \
        LOG_IF_ERROR(                                                   \
            TRITONBACKEND_ResponseSend(                                 \
                rarie_response__, TRITONSERVER_RESPONSE_COMPLETE_FINAL, \
                rarie_err__),                                           \
            "failed to send error response");                           \
      }                                                                 \
      TRITONSERVER_ErrorDelete(rarie_err__);                            \
      return;                                                           \
    }                                                                   \
  } while (false)


class CudaAllocator {
 public:
  void *allocate(size_t size,MEMORY_TYPE type=MEMORY_TYPE::GPU) const {
    void *ptr;
    if (type==MEMORY_TYPE::GPU){
        CK_CUDA_THROW_(cudaMalloc(&ptr, size));
    }
    else{ 
        CK_CUDA_THROW_(cudaMallocHost(&ptr, size));
    }
    
    return ptr;
  }
  void deallocate(void *ptr,MEMORY_TYPE type=MEMORY_TYPE::GPU) const {
    if (type==MEMORY_TYPE::GPU){
        CK_CUDA_THROW_(cudaFree(ptr));;
    }
    else
    { 
        CK_CUDA_THROW_(cudaFreeHost(ptr));
    }
 }
};


template <typename T>
class HugeCTRBuffer:public std::enable_shared_from_this<HugeCTRBuffer<T>>  {
private:
    std::vector<size_t> reserved_buffers_;
    size_t total_num_elements_;
    CudaAllocator allocator_;
    void *ptr_=nullptr;
    size_t total_size_in_bytes_=0;
    MEMORY_TYPE type;
public:
    static std::shared_ptr<HugeCTRBuffer> create(MEMORY_TYPE m_type=MEMORY_TYPE::GPU) {
        return std::shared_ptr<HugeCTRBuffer>(new HugeCTRBuffer(m_type));
    }
    HugeCTRBuffer(MEMORY_TYPE m_type) : ptr_(nullptr), total_size_in_bytes_(0),type(m_type) {}
    ~HugeCTRBuffer() 
    {
        if (allocated()) {
        allocator_.deallocate(ptr_,type);
        }
    }
    bool allocated() const { return total_size_in_bytes_ != 0 && ptr_ != nullptr; }  
    void allocate() 
    {
    if (ptr_ != nullptr) {
      std::cerr <<"WrongInput:Memory has already been allocated.";
      
    }
    size_t offset = 0;
    for (const size_t buffer : reserved_buffers_) {
        size_t size=buffer;
        if (size % 32 != 0) {
            size += (32 - size % 32);
        }
        offset += size;
    }
    reserved_buffers_.clear();
    total_size_in_bytes_ = offset;

    if (total_size_in_bytes_ != 0) 
    {
        ptr_ = allocator_.allocate(total_size_in_bytes_,type);
    }
    }

    size_t get_buffer_size()
    {
      return total_size_in_bytes_;
    }

    void *get_ptr()  
    { 
        return reinterpret_cast<T*>(ptr_) ; 
    }
    
    size_t get_num_elements_from_dimensions(const std::vector<size_t> &dimensions) 
    {
        size_t elements = 1;
        for (size_t dim : dimensions) {
        elements *= dim;
    }
        return elements;
    }

    void reserve(const std::vector<size_t> &dimensions) {
      if (allocated()) {
        std::cerr << "IllegalCall: Buffer is finalized.";
      }
      size_t num_elements = get_num_elements_from_dimensions(dimensions);
      size_t size_in_bytes = num_elements * sizeof(T);

      reserved_buffers_.push_back(size_in_bytes);
      total_num_elements_ += num_elements;
    }

};

class ModelBackend{
  public:
  static TRITONSERVER_Error* Create(
      TRITONBACKEND_Backend* triton_backend_, ModelBackend** backend,std::vector<std::string>modelnames,std::vector<std::string>modelconfigs,bool supportlonglongkey);

  // Get the handle to the TRITONBACKEND model.
  TRITONBACKEND_Backend* TritonBackend() { return triton_backend_; }

  //HugeCTR Int32 PS
  HugeCTR::HugectrUtility<unsigned int>* HugeCTRParameterServerInt32(){return EmbeddingTable_int32;}

  //HugeCTR Int64 PS
  HugeCTR::HugectrUtility<long long>* HugeCTRParameterServerInt64(){return EmbeddingTable_int64;}

    //HugeCTR EmbeddingTable
  TRITONSERVER_Error* HugeCTREmbedding_backend();
 private:
  TRITONBACKEND_Backend* triton_backend_;
  std::vector<std::string> model_config_path;
  std::vector<std::string> model_name;
  HugeCTR::HugectrUtility<unsigned int>* EmbeddingTable_int32;
  HugeCTR::HugectrUtility<long long>* EmbeddingTable_int64;
  bool support_int64_key_=false;
  ModelBackend(
       TRITONBACKEND_Backend* triton_backend_,
      std::vector<std::string>modelnames,std::vector<std::string>modelconfigs,bool supportlonglongkey );


};

TRITONSERVER_Error*
ModelBackend::Create(TRITONBACKEND_Backend* triton_backend_, ModelBackend** backend,std::vector<std::string>modelnames,std::vector<std::string>modelconfigs,bool supportlonglongkey)
{

  *backend = new ModelBackend(
       triton_backend_,modelconfigs, modelnames,supportlonglongkey);
  return nullptr;  // success
}

ModelBackend::ModelBackend(
   TRITONBACKEND_Backend* triton_backend,
   std::vector<std::string>modelnames, std::vector<std::string>modelconfigs,bool supportlonglongkey )
    : triton_backend_(triton_backend),
      model_config_path(modelconfigs),model_name(modelnames),support_int64_key_(supportlonglongkey)
    
{

    //current much model initialization work handled by TritonModel
}


//HugeCTR EmbeddingTable
TRITONSERVER_Error* 
ModelBackend::HugeCTREmbedding_backend(){
     LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("**********Backend Parameter Server creating ") ).c_str());
    HugeCTR::INFER_TYPE type= HugeCTR::INFER_TYPE::TRITON;
    if (support_int64_key_)
    {
      LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Backend Long Long type key Parameter Server creating... ") ).c_str());
      EmbeddingTable_int64=HugeCTR::HugectrUtility<long long>::Create_Parameter_Server(type,model_config_path,model_name);
    }
    else
    {
      LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Backend regular int key type Parameter Server creating ") ).c_str());
      EmbeddingTable_int32 =HugeCTR::HugectrUtility<unsigned int>::Create_Parameter_Server(type,model_config_path,model_name);
    }
    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("**********Backend Create Parameter Server sucessully ") ).c_str());
    return nullptr;
}

//
// ModelState
//
// State associated with a model that is using this backend. An object
// of this class is created and associated with each
// TRITONBACKEND_Model.
//
class ModelState {
 public:
  static TRITONSERVER_Error* Create(
      TRITONBACKEND_Model* triton_model, ModelState** state,HugeCTR::HugectrUtility<unsigned int>* EmbeddingTable_int32, HugeCTR::HugectrUtility<long long>* EmbeddingTable_int64);

  // Get the handle to the TRITONBACKEND model.
  TRITONBACKEND_Model* TritonModel() { return triton_model_; }

  // Get the HugeCTR model batch size.
  int64_t BatchSize() { return max_batch_size_; }

  // Get the HugeCTR model slots size.
  int64_t SlotNum() { return slot_num_; }

  // Get the HugeCTR model max nnz.
  int64_t MaxNNZ() { return max_nnz_; }

  // Get the HugeCTR model dense size.
  int64_t DeseNum() { return dese_num_; }

  // Get the HugeCTR model cat feature size.
  int64_t CatNum() {return cat_num_;}

  // Get the HugeCTR model Embedding size.
  int64_t EmbeddingSize() { return embedding_size_; }

  // Get the HugeCTR cache size per.
  float CacheSizePer() {return cache_size_per;}

  // Support GPU cache for embedding.
  bool GPUCache() { return support_gpu_cache_; }

  //Support int64 embedding key
  bool SupportLongEmbeddingKey() { return support_int64_key_; }
  
  // Get the current HugeCTR model json config.
  std::string HugeCTRJsonConfig() {return hugectr_config_;}

  // Get the handle to the Hugectr_backend  Configuration.
  common::TritonJson::Value& ModelConfig() { return model_config_; }

  // Get the name and version of the model.
  const std::string& Name() const { return name_; }
  uint64_t Version() const { return version_; }

  // Does this model support batching in the first dimension. This
  // function should not be called until after the model is completely
  // loaded.
  TRITONSERVER_Error* SupportsFirstDimBatching(bool* supports);

  // Validate that model configuration is supported by this backend.
  TRITONSERVER_Error* ValidateModelConfig();

  // Parse that model configuration is supported by this backend.
  TRITONSERVER_Error* ParseModelConfig();

  //HugeCTR EmbeddingTable
  TRITONSERVER_Error* HugeCTREmbedding();

  //HugeCTR Int32 PS
  HugeCTR::HugectrUtility<unsigned int>* HugeCTRParameterServerInt32(){return EmbeddingTable_int32;}

  //HugeCTR Int64 PS
  HugeCTR::HugectrUtility<long long>* HugeCTRParameterServerInt64(){return EmbeddingTable_int64;}

 private:
  ModelState(
      TRITONSERVER_Server* triton_server, TRITONBACKEND_Model* triton_model,
      const char* name, const uint64_t version,
      common::TritonJson::Value&& model_config,
      HugeCTR::HugectrUtility<unsigned int>* EmbeddingTable_int32,HugeCTR::HugectrUtility<long long>* EmbeddingTable_int64 );

  TRITONSERVER_Server* triton_server_;
  TRITONBACKEND_Model* triton_model_;
  const std::string name_;
  const uint64_t version_;
  int64_t max_batch_size_=64;
  int64_t slot_num_=10;
  int64_t dese_num_=50;
  int64_t cat_num_=50;
  int64_t embedding_size_=64;
  int64_t max_nnz_=3;
  float cache_size_per=0.5;
  std::string hugectr_config_;
  common::TritonJson::Value model_config_;
  std::vector<std::string> model_config_path;
  std::vector<std::string> model_name;

  bool support_int64_key_=false;
  bool support_gpu_cache_=false;
  bool supports_batching_initialized_;
  bool supports_batching_;

  HugeCTR::HugectrUtility<unsigned int>* EmbeddingTable_int32;
  HugeCTR::HugectrUtility<long long>* EmbeddingTable_int64;

};

TRITONSERVER_Error*
ModelState::Create(TRITONBACKEND_Model* triton_model, ModelState** state, HugeCTR::HugectrUtility<unsigned int>* EmbeddingTable_int32, HugeCTR::HugectrUtility<long long>* EmbeddingTable_int64 )
{
  TRITONSERVER_Message* config_message;
  RETURN_IF_ERROR(TRITONBACKEND_ModelConfig(
      triton_model, 1 /* config_version */, &config_message));

  // We can get the model configuration as a json string from
  // config_message, parse it with our favorite json parser to create
  // DOM that we can access when we need to example the
  // configuration. We use TritonJson, which is a wrapper that returns
  // nice errors (currently the underlying implementation is
  // rapidjson... but others could be added). You can use any json
  // parser you prefer.
  const char* buffer;
  size_t byte_size;
  RETURN_IF_ERROR(
      TRITONSERVER_MessageSerializeToJson(config_message, &buffer, &byte_size));

  common::TritonJson::Value model_config;
  TRITONSERVER_Error* err = model_config.Parse(buffer, byte_size);
  RETURN_IF_ERROR(TRITONSERVER_MessageDelete(config_message));
  RETURN_IF_ERROR(err);

  const char* model_name;
  RETURN_IF_ERROR(TRITONBACKEND_ModelName(triton_model, &model_name));

  uint64_t model_version;
  RETURN_IF_ERROR(TRITONBACKEND_ModelVersion(triton_model, &model_version));

  TRITONSERVER_Server* triton_server;
  RETURN_IF_ERROR(TRITONBACKEND_ModelServer(triton_model, &triton_server));

  *state = new ModelState(
      triton_server, triton_model, model_name, model_version,
      std::move(model_config),EmbeddingTable_int32, EmbeddingTable_int64);
  return nullptr;  // success
}

ModelState::ModelState(
    TRITONSERVER_Server* triton_server, TRITONBACKEND_Model* triton_model,
    const char* name, const uint64_t version,
    common::TritonJson::Value&& model_config, HugeCTR::HugectrUtility<unsigned int>* EmbeddingTable_int32,HugeCTR::HugectrUtility<long long>* EmbeddingTable_int64 )
    : triton_server_(triton_server), triton_model_(triton_model), name_(name),
      version_(version), model_config_(std::move(model_config)),
      supports_batching_initialized_(false), supports_batching_(false),
      EmbeddingTable_int32(EmbeddingTable_int32),EmbeddingTable_int64(EmbeddingTable_int64)
{

    //current much model initialization work handled by TritonModel
}


//HugeCTR EmbeddingTable
TRITONSERVER_Error* 
ModelState::HugeCTREmbedding(){
     LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("**********Parameter Server creating ") ).c_str());
    HugeCTR::INFER_TYPE type= HugeCTR::INFER_TYPE::TRITON;
    if (support_int64_key_)
    {
      LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Long Long type key Parameter Server creating... ") ).c_str());
      EmbeddingTable_int64=HugeCTR::HugectrUtility<long long>::Create_Parameter_Server(type,model_config_path,model_name);
    }
    else
    {
      LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("regular int key type Parameter Server creating ") ).c_str());
      EmbeddingTable_int32 =HugeCTR::HugectrUtility<unsigned int>::Create_Parameter_Server(type,model_config_path,model_name);
    }
    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("**********Create Parameter Server sucessully ") ).c_str());
    return nullptr;
}

TRITONSERVER_Error*
ModelState::SupportsFirstDimBatching(bool* supports)
{
  // We can't determine this during model initialization because
  // TRITONSERVER_ServerModelBatchProperties can't be called until the
  // model is loaded. So we just cache it here.
  if (!supports_batching_initialized_) {
    uint32_t flags = 0;
    RETURN_IF_ERROR(TRITONSERVER_ServerModelBatchProperties(
        triton_server_, name_.c_str(), version_, &flags, nullptr /* voidp */));
    supports_batching_ = ((flags & TRITONSERVER_BATCH_FIRST_DIM) != 0);
    supports_batching_initialized_ = true;
  }

  *supports = supports_batching_;
  return nullptr;  // success
}

TRITONSERVER_Error*
ModelState::ValidateModelConfig()
{

  // We have the json DOM for the model configuration...
  common::TritonJson::WriteBuffer buffer;
  RETURN_IF_ERROR(model_config_.PrettyWrite(&buffer));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("model configuration:\n") + buffer.Contents()).c_str());

  common::TritonJson::Value inputs, outputs;
  RETURN_IF_ERROR(model_config_.MemberAsArray("input", &inputs));
  RETURN_IF_ERROR(model_config_.MemberAsArray("output", &outputs));

  // There must be 3 input and 1 output.
  RETURN_ERROR_IF_FALSE(
      inputs.ArraySize() == 3, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("expected 1 input, got ") +
          std::to_string(inputs.ArraySize()));
  RETURN_ERROR_IF_FALSE(
      outputs.ArraySize() == 1, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("expected 1 output, got ") +
          std::to_string(outputs.ArraySize()));

  common::TritonJson::Value input, output;
  RETURN_IF_ERROR(inputs.IndexAsObject(0, &input));
  RETURN_IF_ERROR(outputs.IndexAsObject(0, &output));

  // Input and output must have same datatype
  std::string input_dtype, output_dtype;
  RETURN_IF_ERROR(input.MemberAsString("data_type", &input_dtype));
  RETURN_IF_ERROR(output.MemberAsString("data_type", &output_dtype));

  RETURN_ERROR_IF_FALSE(
      input_dtype == output_dtype, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("expected input and output datatype to match, got ") +
          input_dtype + " and " + output_dtype);

  // Input and output must have same shape
  std::vector<int64_t> input_shape, output_shape;
  RETURN_IF_ERROR(backend::ParseShape(input, "dims", &input_shape));
  RETURN_IF_ERROR(backend::ParseShape(output, "dims", &output_shape));

  RETURN_ERROR_IF_FALSE(
      input_shape == output_shape, TRITONSERVER_ERROR_INVALID_ARG,
      std::string("expected input and output shape to match, got ") +
          backend::ShapeToString(input_shape) + " and " +
          backend::ShapeToString(output_shape));

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelState::ParseModelConfig()
{
  common::TritonJson::WriteBuffer buffer;
  RETURN_IF_ERROR(model_config_.PrettyWrite(&buffer));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("model configuration:\n") + buffer.Contents()).c_str());

      //Get HugeCTR model configuration
  
  common::TritonJson::Value parameters; 
  if (model_config_.Find("parameters", &parameters)) {
    common::TritonJson::Value slots;
    if (parameters.Find("slots", &slots)) {
      std::string slots_str;
      (slots.MemberAsString(
          "string_value", &slots_str));
      slot_num_=std::stoi(slots_str );
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("slots set to : ") + std::to_string(slot_num_))
              .c_str());
    }
    common::TritonJson::Value dense;
    if (parameters.Find("des_feature_num", &dense)) {
      std::string dese_str;
      (dense.MemberAsString(
          "string_value", &dese_str));
      dese_num_=std::stoi(dese_str );
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("desene num to : ") + std::to_string(dese_num_))
              .c_str());
    }

    common::TritonJson::Value catfea;
    if (parameters.Find("cat_feature_num", &catfea)) {
      std::string cat_str;
      (catfea.MemberAsString(
          "string_value", &cat_str));
      cat_num_=std::stoi(cat_str );
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("cat_feature num to : ") + std::to_string(cat_num_))
              .c_str());
    }

    common::TritonJson::Value embsize;
    if (parameters.Find("embedding_vector_size", &embsize)) {
      std::string embsize_str;
      (embsize.MemberAsString(
          "string_value", &embsize_str));
      embedding_size_=std::stoi(embsize_str );
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("embedding size is : ") + std::to_string(embedding_size_))
              .c_str());
    }
    common::TritonJson::Value nnz;
    if (parameters.Find("max_nnz", &nnz)) {
      std::string nnz_str;
      (nnz.MemberAsString(
          "string_value", &nnz_str));
      max_nnz_=std::stoi(nnz_str );
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("maxnnz is: ") + std::to_string(max_nnz_))
              .c_str());
    }
    common::TritonJson::Value hugeconfig;
    if (parameters.Find("config", &hugeconfig)) {
      std::string config_str;
      (hugeconfig.MemberAsString(
          "string_value", &config_str));
      hugectr_config_=config_str;
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("Hugectr model config path : ") + hugectr_config_)
              .c_str());
    }
    common::TritonJson::Value gpucache;
    if (parameters.Find("gpucache", &gpucache)) {
      std::string gpu_cache;
      (gpucache.MemberAsString(
          "string_value", &gpu_cache));
      if ((gpu_cache)=="true")
      support_gpu_cache_=true;
      std::cout<<"support gpu cache is "<<support_gpu_cache_<<std::endl;
    }
    common::TritonJson::Value gpucacheper;
    if (parameters.Find("gpucacheper", &gpucacheper)) {
      std::string gpu_cache_per;
      (gpucacheper.MemberAsString(
          "string_value", &gpu_cache_per));
      cache_size_per=std::atof(gpu_cache_per.c_str());
      std::cout<<"gpu cache per is "<<cache_size_per<<std::endl;
    }
    
    common::TritonJson::Value embeddingkey;
    if (parameters.Find("embeddingkey_long_type", &embeddingkey)) {
      std::string embeddingkey_str;
      (embeddingkey.MemberAsString(
          "string_value", &embeddingkey_str));
      if ((embeddingkey_str)=="true")
      support_int64_key_=true;
      std::cout<<"Support long embedding key "<<support_int64_key_<<std::endl;
    }
  }
  model_config_.MemberAsInt("max_batch_size", &max_batch_size_);
  std::cout<<"max_batch_size is "<<max_batch_size_<<std::endl;
  return nullptr;
}

//
// ModelInstanceState
//
// State associated with a model instance. An object of this class is
// created and associated with each TRITONBACKEND_ModelInstance.
//
class ModelInstanceState {
 public:
  static TRITONSERVER_Error* Create(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance,
      ModelInstanceState** state);
    
    ~ModelInstanceState();

  // Get the handle to the TRITONBACKEND model instance.
  TRITONBACKEND_ModelInstance* TritonModelInstance()
  {
    return triton_model_instance_;
  }

  // Get the name, kind and device ID of the instance.
  const std::string& Name() const { return name_; }
  TRITONSERVER_InstanceGroupKind Kind() const { return kind_; }
  int32_t DeviceId() const { return device_id_; }

  // Get the state of the model that corresponds to this instance.
  ModelState* StateForModel() const { return model_state_; }

  // Get the prediction result that corresponds to this instance.
  void ProcessRequest(int64_t numofsamples);

  //Create Embedding_cache
  void Create_EmbeddingCache();

  //Create Embedding_cache
  void LoadHugeCTRModel();

  std::shared_ptr<HugeCTRBuffer<float>> GetDeseBuffer() {return dense_value_buf;}

  std::shared_ptr<HugeCTRBuffer<unsigned int>> GetCatColBuffer_int32() {return cat_column_index_buf_int32;}

  std::shared_ptr<HugeCTRBuffer<long long>> GetCatColBuffer_int64() {return cat_column_index_buf_int64;}

  std::shared_ptr<HugeCTRBuffer<int>> GetRowBuffer() {return row_ptr_buf;}

  std::shared_ptr<HugeCTRBuffer<float>> GetPredictBuffer() {return prediction_buf;}

 private:
ModelInstanceState(
      ModelState* model_state,
      TRITONBACKEND_ModelInstance* triton_model_instance, const char* name,
      const TRITONSERVER_InstanceGroupKind kind, const int32_t device_id);

    ModelState* model_state_;
    TRITONBACKEND_ModelInstance* triton_model_instance_;
    const std::string name_;
    const TRITONSERVER_InstanceGroupKind kind_;
    const int32_t device_id_;
    //common::TritonJson::Value model_config_;

    //HugeCTR Model buffer for input and output
    //There buffers will be shared for all the requests
    std::shared_ptr<HugeCTRBuffer<float>> dense_value_buf;
    std::shared_ptr<HugeCTRBuffer<unsigned int>> cat_column_index_buf_int32;
    std::shared_ptr<HugeCTRBuffer<long long>> cat_column_index_buf_int64;
    std::shared_ptr<HugeCTRBuffer<int>> row_ptr_buf;
    std::shared_ptr<HugeCTRBuffer<float>> prediction_buf;
    
    HugeCTR::embedding_interface* Embedding_cache;

    HugeCTR::HugeCTRModel* hugectrmodel_;

};

TRITONSERVER_Error*
ModelInstanceState::Create(
    ModelState* model_state, TRITONBACKEND_ModelInstance* triton_model_instance,
    ModelInstanceState** state)
{
  const char* instance_name;
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelInstanceName(triton_model_instance, &instance_name));

  TRITONSERVER_InstanceGroupKind instance_kind;
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelInstanceKind(triton_model_instance, &instance_kind));

  int32_t device_id;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceDeviceId(triton_model_instance, &device_id));

  int32_t instance_id;
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelInstanceDeviceId(triton_model_instance, &instance_id));

  *state = new ModelInstanceState(
      model_state, triton_model_instance, instance_name, instance_kind,
      device_id);
  return nullptr;  // success
}

ModelInstanceState::ModelInstanceState(
    ModelState* model_state, TRITONBACKEND_ModelInstance* triton_model_instance,
    const char* name, const TRITONSERVER_InstanceGroupKind kind,
    const int32_t device_id)
    : model_state_(model_state), triton_model_instance_(triton_model_instance),
      name_(model_state->Name()), kind_(kind), device_id_(device_id)
{

  	LOG_MESSAGE(
        TRITONSERVER_LOG_INFO,+
                (std::string("Triton Model Instance Initialization on device ")+std::to_string(device_id)).c_str());
    cudaError_t cuerr = cudaSetDevice(device_id);
    if (cuerr != cudaSuccess) {
        std::cerr << "failed to set CUDA device to " << device_id << ": "
            << cudaGetErrorString(cuerr);
    }
  
    //Alloc the cuda memory
    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Dense Feature buffer allocation: ")).c_str());
    dense_value_buf=HugeCTRBuffer<float>::create();
    std::vector<size_t> dense_value_dims = {static_cast<size_t>(model_state_->BatchSize() * model_state_->DeseNum()) }; 
    dense_value_buf->reserve(dense_value_dims);
    dense_value_buf->allocate();

    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Categorical Feature buffer allocation: ")).c_str());
    if(model_state_->SupportLongEmbeddingKey()){
      cat_column_index_buf_int64=HugeCTRBuffer<long long>::create(MEMORY_TYPE::PIN);
      std::vector<size_t> cat_column_index_dims = {static_cast<size_t>(model_state_->BatchSize()* model_state_->CatNum()) }; 
      cat_column_index_buf_int64->reserve(cat_column_index_dims);
      cat_column_index_buf_int64->allocate();

    }
    else{
      cat_column_index_buf_int32=HugeCTRBuffer<unsigned int>::create(MEMORY_TYPE::PIN);
      std::vector<size_t> cat_column_index_dims = {static_cast<size_t>(model_state_->BatchSize() * model_state_->CatNum()) }; 
      cat_column_index_buf_int32->reserve(cat_column_index_dims);
      cat_column_index_buf_int32->allocate();
    }
    
    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Categorical Row Index buffer allocation: ")).c_str());
    row_ptr_buf=HugeCTRBuffer<int>::create();
    std::vector<size_t> row_ptrs_dims = {static_cast<size_t>(model_state_->BatchSize() * model_state_->SlotNum()+1 ) }; 
    row_ptr_buf->reserve(row_ptrs_dims);
    row_ptr_buf->allocate();

    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("Predict resulte buffer allocation: ")).c_str());
    prediction_buf=HugeCTRBuffer<float>::create();
    std::vector<size_t> prediction_dims = {static_cast<size_t>(model_state_->BatchSize()) }; 
    prediction_buf->reserve(prediction_dims);
    prediction_buf->allocate();
     LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("******Creating Embedding Cache ")).c_str());
    Create_EmbeddingCache();
    LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("******Loading Hugectr Model ")).c_str());
    LoadHugeCTRModel();

}

ModelInstanceState::~ModelInstanceState()
{
  // release all the buffers
}

void ModelInstanceState::Create_EmbeddingCache()
{
  if(model_state_->SupportLongEmbeddingKey())
  {
    Embedding_cache=HugeCTR::embedding_interface::Create_Embedding_Cache(model_state_->HugeCTRParameterServerInt64(),
    device_id_,
    model_state_->GPUCache(),
    model_state_->CacheSizePer(),
    model_state_->HugeCTRJsonConfig(),
    name_);
  }
  else
  {
    Embedding_cache=HugeCTR::embedding_interface::Create_Embedding_Cache(model_state_->HugeCTRParameterServerInt32(),
      device_id_,
      model_state_->GPUCache(),
      model_state_->CacheSizePer(),
      model_state_->HugeCTRJsonConfig(),
      name_);
  }
  LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("******Creating Embedding Cache successfully")).c_str());
}

void ModelInstanceState::LoadHugeCTRModel(){
  HugeCTR::INFER_TYPE type=HugeCTR::INFER_TYPE::TRITON;
  std::cout<<"model config is "<<model_state_->HugeCTRJsonConfig()<<std::endl;
  hugectrmodel_=HugeCTR::HugeCTRModel::load_model(type,model_state_->HugeCTRJsonConfig(),device_id_,Embedding_cache);
  LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("******Loading Hugectr model successfully")).c_str());
}

void ModelInstanceState::ProcessRequest(int64_t numofsamples)
{
  if(model_state_->SupportLongEmbeddingKey()){
    hugectrmodel_->predict((float*)dense_value_buf->get_ptr(),cat_column_index_buf_int64->get_ptr(),(int*)row_ptr_buf->get_ptr(),(float*)prediction_buf->get_ptr(),numofsamples);
  }
  else{
    hugectrmodel_->predict((float*)dense_value_buf->get_ptr(),cat_column_index_buf_int32->get_ptr(),(int*)row_ptr_buf->get_ptr(),(float*)prediction_buf->get_ptr(),numofsamples);
  }
}


/////////////

extern "C" {

// Implementing TRITONBACKEND_Initialize is optional. The backend
// should initialize any global state that is intended to be shared
// across all models and model instances that use the backend.
TRITONSERVER_Error*
TRITONBACKEND_Initialize(TRITONBACKEND_Backend* backend)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_BackendName(backend, &cname));
  std::string name(cname);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_Initialize: ") + name).c_str());

  // We should check the backend API version that Triton supports
  // vs. what this backend was compiled against.
  uint32_t api_version_major, api_version_minor;
  RETURN_IF_ERROR(
      TRITONBACKEND_ApiVersion(&api_version_major, &api_version_minor));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("Triton TRITONBACKEND API version: ") +
       std::to_string(api_version_major) + "." +
       std::to_string(api_version_minor))
          .c_str());
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("'") + name + "' TRITONBACKEND API version: " +
       std::to_string(TRITONBACKEND_API_VERSION_MAJOR) + "." +
       std::to_string(TRITONBACKEND_API_VERSION_MINOR))
          .c_str());

  if ((api_version_major != TRITONBACKEND_API_VERSION_MAJOR) ||
      (api_version_minor < TRITONBACKEND_API_VERSION_MINOR)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "triton backend API version does not support this backend");
  }

  // The backend configuration may contain information needed by the
  // backend, such a command-line arguments. This backend doesn't use
  // any such configuration but we print whatever is available.
  TRITONSERVER_Message* backend_config_message;
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendConfig(backend, &backend_config_message));

  TRITONBACKEND_ArtifactType artifact_type;
  const char* clocation;
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendArtifacts(backend, &artifact_type, &clocation));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("backedn Repository location: ") + clocation).c_str());

  const char* buffer;
  size_t byte_size;
  RETURN_IF_ERROR(TRITONSERVER_MessageSerializeToJson(
      backend_config_message, &buffer, &byte_size));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("backend configuration:\n") + buffer).c_str());

  // If we have any global backend state we create and set it here. We
  // don't need anything for this backend but for demonstration
  // purposes we just create something...
  std::string* state = new std::string("backend state");
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendSetState(backend, reinterpret_cast<void*>(state)));


  common::TritonJson::Value backend_config;
  TRITONSERVER_Error* err = backend_config.Parse(buffer, byte_size);
  RETURN_IF_ERROR(err);
  common::TritonJson::Value cmdline;;
  std::vector<std::string> param_values;
  std::vector<std::string> param_keys;
  bool supportlonglongkey=false;
  if (backend_config.Find("cmdline", &cmdline)) {
    RETURN_IF_ERROR(cmdline.Members(&param_keys));
    for (const auto& param_key : param_keys){
      std::string value_string;
      if(param_key!="supportlonglong")
      {
      RETURN_IF_ERROR(cmdline.MemberAsString(
                        param_key.c_str(), &value_string));
      param_values.push_back(value_string);
      }
      else{
        supportlonglongkey=true;
      }
    }
  }
  std::vector<std::string>::iterator it;
   for(it=param_keys.begin();it!=param_keys.end();)
    {
        if(*it =="supportlonglong")
        {
            it=param_keys.erase(it);
        }
        else
        { ++it;}
           
    }

  ModelBackend* model_backend;
  RETURN_IF_ERROR(ModelBackend::Create(backend,&model_backend,param_values,param_keys,supportlonglongkey));
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendSetState(backend, reinterpret_cast<void*>(model_backend)));

  // One of the primary things to do in ModelInitialize is to examine
  // the model configuration to ensure that it is something that this
  // backend can support. If not, returning an error from this
  // function will prevent the model from loading.
  //RETURN_IF_ERROR(model_state->ValidateModelConfig());

  RETURN_IF_ERROR(model_backend->HugeCTREmbedding_backend());


  return nullptr;  // success
}

// Implementing TRITONBACKEND_Finalize is optional unless state is set
// using TRITONBACKEND_BackendSetState. The backend must free this
// state and perform any other global cleanup.
TRITONSERVER_Error*
TRITONBACKEND_Finalize(TRITONBACKEND_Backend* backend)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_BackendState(backend, &vstate));
  std::string* state = reinterpret_cast<std::string*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_Finalize: state is '") + *state + "'")
          .c_str());

  delete state;

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInitialize is optional. The backend
// should initialize any state that is intended to be shared across
// all instances of the model.
TRITONSERVER_Error*
TRITONBACKEND_ModelInitialize(TRITONBACKEND_Model* model)
{
  
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_ModelName(model, &cname));
  std::string name(cname);

  uint64_t version;
  RETURN_IF_ERROR(TRITONBACKEND_ModelVersion(model, &version));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_ModelInitialize: ") + name + " (version " +
       std::to_string(version) + ")")
          .c_str());

  // Can get location of the model artifacts. Normally we would need
  // to check the artifact type to make sure it was something we can
  // handle... but we are just going to log the location so we don't
  // need the check. We would use the location if we wanted to load
  // something from the model's repo.
  TRITONBACKEND_ArtifactType artifact_type;
  const char* clocation;
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelRepository(model, &artifact_type, &clocation));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("Repository location: ") + clocation).c_str());

  // The model can access the backend as well... here we can access
  // the backend global state.
  TRITONBACKEND_Backend* backend;
  RETURN_IF_ERROR(TRITONBACKEND_ModelBackend(model, &backend));

  TRITONSERVER_Message* backend_config_message;
  RETURN_IF_ERROR(
      TRITONBACKEND_BackendConfig(backend, &backend_config_message));

  const char* buffer;
  size_t byte_size;
  RETURN_IF_ERROR(TRITONSERVER_MessageSerializeToJson(
      backend_config_message, &buffer, &byte_size));
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("backend configuration in mode:\n") + buffer).c_str());

  void* vbackendstate;
  RETURN_IF_ERROR(TRITONBACKEND_BackendState(backend, &vbackendstate));
  ModelBackend* backend_state = reinterpret_cast<ModelBackend*>(vbackendstate);

  /*LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("backend state is '") + *backend_state + "'").c_str());*/

  // With each model we create a ModelState object and associate it
  // with the TRITONBACKEND_Model.
  ModelState* model_state;
  ModelState::Create(model, &model_state,backend_state->HugeCTRParameterServerInt32(),backend_state->HugeCTRParameterServerInt64());
  RETURN_IF_ERROR(
      TRITONBACKEND_ModelSetState(model, reinterpret_cast<void*>(model_state)));

  // One of the primary things to do in ModelInitialize is to examine
  // the model configuration to ensure that it is something that this
  // backend can support. If not, returning an error from this
  // function will prevent the model from loading.
  //RETURN_IF_ERROR(model_state->ValidateModelConfig());

  RETURN_IF_ERROR(model_state->ParseModelConfig());

  //RETURN_IF_ERROR(model_state->HugeCTREmbedding());

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelFinalize is optional unless state
// is set using TRITONBACKEND_ModelSetState. The backend must free
// this state and perform any other cleanup.
TRITONSERVER_Error*
TRITONBACKEND_ModelFinalize(TRITONBACKEND_Model* model)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO, "TRITONBACKEND_ModelFinalize: delete model state");

  delete model_state;

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInstanceInitialize is optional. The
// backend should initialize any state that is required for a model
// instance.
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceInitialize(TRITONBACKEND_ModelInstance* instance)
{
  const char* cname;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceName(instance, &cname));
  std::string name(cname);

  int32_t device_id;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceDeviceId(instance, &device_id));

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("TRITONBACKEND_ModelInstanceInitialize: ") + name +
       " (device " + std::to_string(device_id) + ")")
          .c_str());

  // The instance can access the corresponding model as well... here
  // we get the model and from that get the model's state.
  TRITONBACKEND_Model* model;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceModel(instance, &model));

  void* vmodelstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelState(model, &vmodelstate));
  ModelState* model_state = reinterpret_cast<ModelState*>(vmodelstate);

  // With each instance we create a ModelInstanceState object and
  // associate it with the TRITONBACKEND_ModelInstance.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(
      ModelInstanceState::Create(model_state, instance, &instance_state));
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceSetState(
      instance, reinterpret_cast<void*>(instance_state)));

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInstanceFinalize is optional unless
// state is set using TRITONBACKEND_ModelInstanceSetState. The backend
// must free this state and perform any other cleanup.
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceFinalize(TRITONBACKEND_ModelInstance* instance)
{
  void* vstate;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(instance, &vstate));
  ModelInstanceState* instance_state =
      reinterpret_cast<ModelInstanceState*>(vstate);

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      "TRITONBACKEND_ModelInstanceFinalize: delete instance state");

  delete instance_state;

  return nullptr;  // success
}

// Implementing TRITONBACKEND_ModelInstanceExecute is required.
TRITONSERVER_Error*
TRITONBACKEND_ModelInstanceExecute(
    TRITONBACKEND_ModelInstance* instance, TRITONBACKEND_Request** requests,
    const uint32_t request_count)
{
  // Triton will not call this function simultaneously for the same
  // 'instance'. But since this backend could be used by multiple
  // instances from multiple models the implementation needs to handle
  // multiple calls to this function at the same time (with different
  // 'instance' objects). Suggested practice for this is to use only
  // function-local and model-instance-specific state (obtained from
  // 'instance'), which is what we do here.
  ModelInstanceState* instance_state;
  RETURN_IF_ERROR(TRITONBACKEND_ModelInstanceState(
      instance, reinterpret_cast<void**>(&instance_state)));
  ModelState* model_state = instance_state->StateForModel();

  // This backend specifies BLOCKING execution policy. That means that
  // we should not return from this function until execution is
  // complete. Triton will automatically release 'instance' on return
  // from this function so that it is again available to be used for
  // another call to TRITONBACKEND_ModelInstanceExecute.

  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("model ") + model_state->Name() + ", instance " +
       instance_state->Name() + ", executing " + std::to_string(request_count) +
       " requests")
          .c_str());

  bool supports_batching = false;
  RETURN_IF_ERROR(model_state->SupportsFirstDimBatching(&supports_batching));

  // 'responses' is initialized with the response objects below and
  // if/when an error response is sent the corresponding entry in
  // 'responses' is set to nullptr to indicate that that response has
  // already been sent.
  std::vector<TRITONBACKEND_Response*> responses;
  responses.reserve(request_count); 
  // Create a single response object for each request. If something
  // goes wrong when attempting to create the response objects just
  // fail all of the requests by returning an error.
  for (uint32_t r = 0; r < request_count; ++r) {
    TRITONBACKEND_Request* request = requests[r];

    TRITONBACKEND_Response* response;
    RETURN_IF_ERROR(TRITONBACKEND_ResponseNew(&response, request));
    responses.push_back(response);
  }

  // The way we collect these batch timestamps is not entirely
  // accurate. Normally, in a performant backend you would execute all
  // the requests at the same time, and so there would be a single
  // compute-start / compute-end time-range. But here we execute each
  // request separately so there is no single range. As a result we
  // just show the entire execute time as being the compute time as
  // well.
  uint64_t min_exec_start_ns = std::numeric_limits<uint64_t>::max();
  uint64_t max_exec_end_ns = 0;
  uint64_t total_batch_size = 0;

  // After this point we take ownership of 'requests', which means
  // that a response must be sent for every request. If something does
  // go wrong in processing a particular request then we send an error
  // response just for the specific request.

  // For simplicity we just process each request separately... in
  // general a backend should try to operate on the entire batch of
  // requests at the same time for improved performance.
  for (uint32_t r = 0; r < request_count; ++r) {
    uint64_t exec_start_ns = 0;
    SET_TIMESTAMP(exec_start_ns);
    min_exec_start_ns = std::min(min_exec_start_ns, exec_start_ns);

    TRITONBACKEND_Request* request = requests[r];
    const char* request_id = "";
    GUARDED_RESPOND_IF_ERROR(
        responses, r, TRITONBACKEND_RequestId(request, &request_id));

    uint64_t correlation_id = 0;
    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_RequestCorrelationId(request, &correlation_id));

    // Triton ensures that there is only a single input since that is
    // what is specified in the model configuration, so normally there
    // would be no reason to check it but we do here to demonstate the
    // API.
    uint32_t input_count = 0;
    GUARDED_RESPOND_IF_ERROR(
        responses, r, TRITONBACKEND_RequestInputCount(request, &input_count));

    uint32_t requested_output_count = 0;
    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_RequestOutputCount(request, &requested_output_count));

    // If an error response was sent for the above then display an
    // error message and move on to next request.
    if (responses[r] == nullptr) {
      LOG_MESSAGE(
          TRITONSERVER_LOG_ERROR,
          (std::string("request ") + std::to_string(r) +
           ": failed to read request input/output counts, error response sent")
              .c_str());
      continue;
    }

    LOG_MESSAGE(
        TRITONSERVER_LOG_INFO,
        (std::string("request ") + std::to_string(r) + ": id = \"" +
         request_id + "\", correlation_id = " + std::to_string(correlation_id) +
         ", input_count = " + std::to_string(input_count) +
         ", requested_output_count = " + std::to_string(requested_output_count))
            .c_str());

    const char* des_input_name;
    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_RequestInputName(request, 0 /* index */, &des_input_name));

    const char* catcol_input_name;
    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_RequestInputName(request, 1 /* index */, &catcol_input_name));

    const char* row_input_name;
    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_RequestInputName(request, 2 /* index */, &row_input_name));

    TRITONBACKEND_Input* des_input = nullptr;
    GUARDED_RESPOND_IF_ERROR(
        responses, r, TRITONBACKEND_RequestInput(request, des_input_name, &des_input));

    TRITONBACKEND_Input* catcol_input = nullptr;
    GUARDED_RESPOND_IF_ERROR(
        responses, r, TRITONBACKEND_RequestInput(request, catcol_input_name, &catcol_input));
    
    TRITONBACKEND_Input* row_input = nullptr;
    GUARDED_RESPOND_IF_ERROR(
        responses, r, TRITONBACKEND_RequestInput(request, row_input_name, &row_input));

    // We also validated that the model configuration specifies only a
    // single output, but the request is not required to request any
    // output at all so we only produce an output if requested.
    const char* requested_output_name = nullptr;
    if (requested_output_count > 0) {
      GUARDED_RESPOND_IF_ERROR(
          responses, r,
          TRITONBACKEND_RequestOutputName(
              request, 0 /* index */, &requested_output_name));
    }

    // If an error response was sent while getting the input or
    // requested output name then display an error message and move on
    // to next request.
    if (responses[r] == nullptr) {
      LOG_MESSAGE(
          TRITONSERVER_LOG_ERROR,
          (std::string("request ") + std::to_string(r) +
           ": failed to read input or requested output name, error response "
           "sent")
              .c_str());
      continue;
    }

    TRITONSERVER_DataType des_datatype;
    TRITONSERVER_DataType cat_datatype;
    TRITONSERVER_DataType row_datatype;

    const int64_t* input_shape;
    uint32_t des_dims_count;
    uint32_t cat_dims_count;
    uint32_t row_dims_count;
    uint64_t des_byte_size;
    uint64_t cat_byte_size;
    uint64_t row_byte_size;
    uint32_t input_buffer_count;
   

    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_InputProperties(
            catcol_input, nullptr /* input_name */, &cat_datatype, &input_shape,
            &cat_dims_count, &cat_byte_size, &input_buffer_count));
     LOG_MESSAGE(
        TRITONSERVER_LOG_INFO,
        
        (std::string("\tinput ") + catcol_input_name +
         ": datatype = " + TRITONSERVER_DataTypeString(cat_datatype) +
         ", shape = " + backend::ShapeToString(input_shape, cat_dims_count) +
         ", byte_size = " + std::to_string(cat_byte_size) +
         ", buffer_count = " + std::to_string(input_buffer_count))
            .c_str());

    GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_InputProperties(
            row_input, nullptr /* input_name */, &row_datatype, &input_shape,
            &row_dims_count, &row_byte_size, &input_buffer_count));
     LOG_MESSAGE(
        TRITONSERVER_LOG_INFO,
        (std::string("\tinput ") + row_input_name +
         ": datatype = " + TRITONSERVER_DataTypeString(row_datatype) +
         ", shape = " + backend::ShapeToString(input_shape, row_dims_count) +
         ", byte_size = " + std::to_string(row_byte_size) +
         ", buffer_count = " + std::to_string(input_buffer_count))
            .c_str());
     
     GUARDED_RESPOND_IF_ERROR(
        responses, r,
        TRITONBACKEND_InputProperties(
            des_input, nullptr /* input_name */, &des_datatype, &input_shape,
            &des_dims_count, &des_byte_size, &input_buffer_count));
    LOG_MESSAGE(
        TRITONSERVER_LOG_INFO,
        (std::string("\tinput ") + des_input_name +
         ": datatype = " + TRITONSERVER_DataTypeString(des_datatype) +
         ", shape = " + backend::ShapeToString(input_shape, des_dims_count) +
         ", byte_size = " + std::to_string(des_byte_size) +
         ", buffer_count = " + std::to_string(input_buffer_count))
            .c_str());

    
    if (responses[r] == nullptr) {
      LOG_MESSAGE(
          TRITONSERVER_LOG_ERROR,
          (std::string("request ") + std::to_string(r) +
           ": failed to read input properties, error response sent")
              .c_str());
      continue;
    }

   
    LOG_MESSAGE(
        TRITONSERVER_LOG_INFO,
        (std::string("\trequested_output ") + requested_output_name).c_str());

    // For statistics we need to collect the total batch size of all
    // the requests. If the model doesn't support batching then each
    // request is necessarily batch-size 1. If the model does support
    // batching then the first dimension of the shape is the batch
    // size.
    if (supports_batching && (des_dims_count > 0)) {
      total_batch_size += input_shape[0];
    } else {
      total_batch_size++;
    }

    // We only need to produce an output if it was requested.
    if (requested_output_count > 0) {
      // This backend simply copies the input tensor to the output
      // tensor. The input tensor contents are available in one or
      // more contiguous buffers. To do the copy we:
      //
      //   1. Create an output tensor in the response.
      //
      //   2. Allocate appropriately sized buffer in the output
      //      tensor.
      //
      //   3. Iterate over the input tensor buffers, pass to the HugeCTR predict and copy the
      //      result into the output buffer.
      TRITONBACKEND_Response* response = responses[r];

      // Step 1. Input and output have same datatype and shape...
      TRITONBACKEND_Output* output;
      int64_t numofdes=(des_byte_size/sizeof(float));
      int64_t numofsample=numofdes/(instance_state->StateForModel()->DeseNum());
      if ((numofsample>instance_state->StateForModel()->BatchSize()) ) {
          GUARDED_RESPOND_IF_ERROR(
              responses, r,
              TRITONSERVER_ErrorNew(
                  TRITONSERVER_ERROR_UNSUPPORTED,
                  "The number of Input sample greater than max batch size"));
        }
      int64_t* out_putshape=&numofsample;
      GUARDED_RESPOND_IF_ERROR(
          responses, r,
          TRITONBACKEND_ResponseOutput(
              response, &output, requested_output_name, des_datatype,
              out_putshape, 1));
      if (responses[r] == nullptr) {
        LOG_MESSAGE(
            TRITONSERVER_LOG_ERROR,
            (std::string("request ") + std::to_string(r) +
             ": failed to create response output, error response sent")
                .c_str());
        continue;
      }

      // Step 2. Get the output buffer. We request a buffer in GPU
      // memory but we have to handle any returned type. 
      void* output_buffer;
      TRITONSERVER_MemoryType output_memory_type = TRITONSERVER_MEMORY_GPU;
      int64_t output_memory_type_id = 0;
      GUARDED_RESPOND_IF_ERROR(
          responses, r,
          TRITONBACKEND_OutputBuffer(
              output, &output_buffer, instance_state->GetDeseBuffer()->get_buffer_size(), &output_memory_type,
              &output_memory_type_id));
      if ((responses[r] == nullptr) ) {
        GUARDED_RESPOND_IF_ERROR(
            responses, r,
            TRITONSERVER_ErrorNew(
                TRITONSERVER_ERROR_UNSUPPORTED,
                "failed to create output buffer in GPU memory"));
        LOG_MESSAGE(
            TRITONSERVER_LOG_ERROR,
            (std::string("request ") + std::to_string(r) +
             ": failed to create output buffer in CPU memory, error response "
             "sent")
                .c_str());
        continue;
      }

      // Step 3. Copy input -> DEvice Buffer. 
      size_t output_buffer_offset = 0;
      for (uint32_t b = 0; b < input_buffer_count; ++b) {

        const void* des_buffer=nullptr;
        uint64_t buffer_byte_size = des_byte_size;
        TRITONSERVER_MemoryType input_memory_type = TRITONSERVER_MEMORY_GPU;
        int64_t input_memory_type_id = 0;
        GUARDED_RESPOND_IF_ERROR(
            responses, r,
            TRITONBACKEND_InputBuffer(
                des_input, b, &des_buffer, &buffer_byte_size, &input_memory_type,
                &input_memory_type_id));
        CK_CUDA_THROW_(cudaMemcpy(instance_state->GetDeseBuffer()->get_ptr(), des_buffer, des_byte_size, cudaMemcpyHostToDevice));

        const void* cat_buffer=nullptr;
        GUARDED_RESPOND_IF_ERROR(
            responses, r,
            TRITONBACKEND_InputBuffer(
                catcol_input, b, &cat_buffer, &cat_byte_size, &input_memory_type,
                &input_memory_type_id));
        if (instance_state->StateForModel()->SupportLongEmbeddingKey())
        {
          CK_CUDA_THROW_(cudaMemcpy(instance_state->GetCatColBuffer_int64()->get_ptr(), cat_buffer, cat_byte_size, cudaMemcpyHostToHost));
        }
        else{
          CK_CUDA_THROW_(cudaMemcpy(instance_state->GetCatColBuffer_int32()->get_ptr(), cat_buffer, cat_byte_size, cudaMemcpyHostToHost));
        }
        
        const void* row_buffer=nullptr;
        GUARDED_RESPOND_IF_ERROR(
            responses, r,
            TRITONBACKEND_InputBuffer(
                row_input, b, &row_buffer, &row_byte_size, &input_memory_type,
                &input_memory_type_id));
        CK_CUDA_THROW_(cudaMemcpy(instance_state->GetRowBuffer()->get_ptr(), row_buffer, row_byte_size, cudaMemcpyHostToDevice));
        

        if ((responses[r] == nullptr) ) {
          GUARDED_RESPOND_IF_ERROR(
              responses, r,
              TRITONSERVER_ErrorNew(
                  TRITONSERVER_ERROR_UNSUPPORTED,
                  "failed to get input buffer in GPU memory"));
        }
         // Step 3. Pass device buffer to Predict  
        LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("******Process request")).c_str());
        instance_state->ProcessRequest(numofsample);
        LOG_MESSAGE(TRITONSERVER_LOG_INFO,(std::string("******process request finish")).c_str());
        output_buffer_offset += buffer_byte_size; 
        CK_CUDA_THROW_(cudaMemcpy(output_buffer, instance_state->GetPredictBuffer()->get_ptr(), numofsample*sizeof(float), cudaMemcpyDeviceToHost));
      }
      
      if (responses[r] == nullptr) {
        LOG_MESSAGE(
            TRITONSERVER_LOG_ERROR,
            (std::string("request ") + std::to_string(r) +
             ": failed to get input buffer in CPU memory, error response "
             "sent")
                .c_str());
        continue;
      }
    }

    // To demonstrate response parameters we attach some here. Most
    // responses do not use parameters but they provide a way for
    // backends to communicate arbitrary information along with the
    // response.
    LOG_IF_ERROR(
        TRITONBACKEND_ResponseSetStringParameter(
            responses[r], "param0", "an example string parameter"),
        "failed setting string parameter");
    LOG_IF_ERROR(
        TRITONBACKEND_ResponseSetIntParameter(responses[r], "param1", 42),
        "failed setting integer parameter");
    LOG_IF_ERROR(
        TRITONBACKEND_ResponseSetBoolParameter(responses[r], "param2", false),
        "failed setting boolean parameter");

    // If we get to this point then there hasn't been any error and
    // the response is complete and we can send it. This is the last
    // (and only) response that we are sending for the request so we
    // must mark it FINAL. If there is an error when sending all we
    // can do is log it.
    LOG_IF_ERROR(
        TRITONBACKEND_ResponseSend(
            responses[r], TRITONSERVER_RESPONSE_COMPLETE_FINAL,
            nullptr /* success */),
        "failed sending response");
    

    uint64_t exec_end_ns = 0;
    SET_TIMESTAMP(exec_end_ns);
    max_exec_end_ns = std::max(max_exec_end_ns, exec_end_ns);

    // Report statistics for the successful request. For an instance
    // using the CPU we don't associate any device with the
    // statistics, otherwise we associate the instance's device.
    LOG_IF_ERROR(
        TRITONBACKEND_ModelInstanceReportStatistics(
            instance_state->TritonModelInstance(), request, true /* success */,
            exec_start_ns, exec_start_ns, exec_end_ns, exec_end_ns),
        "failed reporting request statistics");
  }

  // Done with requests...

  // There are two types of statistics that we can report... the
  // statistics for the entire batch of requests that we just executed
  // and statistics for each individual request. Statistics for each
  // individual request were reported above inside the loop as each
  // request was processed (or for failed requests we report that
  // failure below). Here we report statistics for the entire batch of
  // requests.
  LOG_IF_ERROR(
      TRITONBACKEND_ModelInstanceReportBatchStatistics(
          instance_state->TritonModelInstance(), total_batch_size,
          min_exec_start_ns, min_exec_start_ns, max_exec_end_ns,
          max_exec_end_ns),
      "failed reporting batch request statistics");

  // We could have released each request as soon as we sent the
  // corresponding response. But for clarity we just release them all
  // here. Note that is something goes wrong when releasing a request
  // all we can do is log it... there is no response left to use to
  // report an error.
  for (uint32_t r = 0; r < request_count; ++r) {
    TRITONBACKEND_Request* request = requests[r];

    // Before releasing, record failed requests as those where
    // responses[r] is nullptr. The timestamps are ignored in this
    // case.
    if (responses[r] == nullptr) {
      LOG_IF_ERROR(
          TRITONBACKEND_ModelInstanceReportStatistics(
              instance_state->TritonModelInstance(), request,
              false /* success */, 0, 0, 0, 0),
          "failed reporting request statistics");
    }

    LOG_IF_ERROR(
        TRITONBACKEND_RequestRelease(request, TRITONSERVER_REQUEST_RELEASE_ALL),
        "failed releasing request");
  }

  return nullptr;  // success
}

}  // extern "C"

}}}  // namespace triton::backend::identity

