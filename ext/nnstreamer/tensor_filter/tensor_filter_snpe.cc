/* SPDX-License-Identifier: LGPL-2.1-only */
/**
 * NNStreamer tensor_filter, sub-plugin for SNPE
 * Copyright (C) 2024 Yongjoo Ahn <yongjoo1.ahn@samsung.com>
 */
/**
 * @file      tensor_filter_snpe.cc
 * @date      15 Jan 2024
 * @brief     NNStreamer tensor-filter sub-plugin for SNPE (Qualcomm Neural Processing SDK)
 * @see       http://github.com/nnstreamer/nnstreamer
              https://developer.qualcomm.com/software/qualcomm-neural-processing-sdk
 * @author    Yongjoo Ahn <yongjoo1.ahn@samsung.com>
 * @bug       No known bugs except for NYI items
 *
 * This is the per-NN-framework plugin (SNPE) for tensor_filter.
 */

#include <iostream>
#include <string>
#include <vector>

#include <glib.h>
#include <nnstreamer_cppplugin_api_filter.hh>
#include <nnstreamer_log.h>
#include <nnstreamer_plugin_api_util.h>
#include <nnstreamer_util.h>

#include <DlContainer/DlContainer.h>
#include <DlSystem/DlEnums.h>
#include <DlSystem/DlError.h>
#include <DlSystem/DlVersion.h>
#include <DlSystem/IUserBuffer.h>
#include <DlSystem/RuntimeList.h>
#include <DlSystem/UserBufferMap.h>
#include <SNPE/SNPE.h>
#include <SNPE/SNPEBuilder.h>
#include <SNPE/SNPEUtil.h>

#define SNPE_FRAMEWORK_NAME "snpe"

#if SNPE_VERSION_MAJOR != 2
#error "This code targets only SNPE 2.x"
#endif

namespace nnstreamer
{
namespace tensor_filter_snpe
{
extern "C" {
void init_filter_snpe (void) __attribute__ ((constructor));
void fini_filter_snpe (void) __attribute__ ((destructor));
}

/** @brief tensor-filter-subplugin concrete class for SNPE */
class snpe_subplugin final : public tensor_filter_subplugin
{
  private:
  static snpe_subplugin *registeredRepresentation;
  static const char *name;
  static const accl_hw hw_list[];
  static const int num_hw = 1;

  bool configured;
  char *model_path; /**< The model *.dlc file */
  void cleanup (); /**< cleanup function */
  GstTensorsInfo inputInfo; /**< Input tensors metadata */
  GstTensorsInfo outputInfo; /**< Output tensors metadata */

  /** snpe handles */
  Snpe_SNPE_Handle_t snpe_h;
  Snpe_UserBufferMap_Handle_t inputMap_h;
  Snpe_UserBufferMap_Handle_t outputMap_h;
  std::vector<Snpe_IUserBuffer_Handle_t> user_buffers;

  public:
  static void init_filter_snpe ();
  static void fini_filter_snpe ();

  snpe_subplugin ();
  ~snpe_subplugin ();

  tensor_filter_subplugin &getEmptyInstance ();
  void configure_instance (const GstTensorFilterProperties *prop);
  void invoke (const GstTensorMemory *input, GstTensorMemory *output);
  void getFrameworkInfo (GstTensorFilterFrameworkInfo &info);
  int getModelInfo (model_info_ops ops, GstTensorsInfo &in_info, GstTensorsInfo &out_info);
  int eventHandler (event_ops ops, GstTensorFilterFrameworkEventData &data);
};

/**
 * @brief Constructor for snpe_subplugin.
 */
snpe_subplugin::snpe_subplugin ()
    : tensor_filter_subplugin (), configured (false), model_path (nullptr),
      snpe_h (nullptr), inputMap_h (nullptr), outputMap_h (nullptr), user_buffers ()
{
  gst_tensors_info_init (std::addressof (inputInfo));
  gst_tensors_info_init (std::addressof (outputInfo));
}

/**
 * @brief Destructor for snpe subplugin.
 */
snpe_subplugin::~snpe_subplugin ()
{
  cleanup ();
}

/**
 * @brief Method to get empty object.
 */
tensor_filter_subplugin &
snpe_subplugin::getEmptyInstance ()
{
  return *(new snpe_subplugin ());
}

/**
 * @brief Method to cleanup snpe subplugin.
 */
void
snpe_subplugin::cleanup ()
{
  g_free (model_path);
  model_path = nullptr;

  if (!configured)
    return;

  if (inputMap_h)
    Snpe_UserBufferMap_Delete (inputMap_h);

  if (outputMap_h)
    Snpe_UserBufferMap_Delete (outputMap_h);

  for (auto &ub : user_buffers)
    if (ub)
      Snpe_IUserBuffer_Delete (ub);

  user_buffers.clear ();

  if (snpe_h)
    Snpe_SNPE_Delete (snpe_h);

  snpe_h = nullptr;
  inputMap_h = nullptr;
  outputMap_h = nullptr;

  gst_tensors_info_free (std::addressof (inputInfo));
  gst_tensors_info_free (std::addressof (outputInfo));

  configured = false;
}

/**
 * @brief Method to prepare/configure SNPE instance.
 */
void
snpe_subplugin::configure_instance (const GstTensorFilterProperties *prop)
{
  /* Already configured */
  if (configured)
    cleanup ();

  Snpe_DlVersion_Handle_t lib_version_h = NULL;
  Snpe_RuntimeList_Handle_t runtime_list_h = NULL;
  Snpe_DlContainer_Handle_t container_h = NULL;
  Snpe_SNPEBuilder_Handle_t snpebuilder_h = NULL;
  Snpe_StringList_Handle_t inputstrListHandle = NULL;
  Snpe_StringList_Handle_t outputstrListHandle = NULL;
  std::vector<Snpe_UserBufferEncoding_ElementType_t> inputTypeVec;
  std::vector<Snpe_UserBufferEncoding_ElementType_t> outputTypeVec;

  auto _clean_handles = [&] () {
    if (lib_version_h)
      Snpe_DlVersion_Delete (lib_version_h);
    if (runtime_list_h)
      Snpe_RuntimeList_Delete (runtime_list_h);
    if (container_h)
      Snpe_DlContainer_Delete (container_h);
    if (snpebuilder_h)
      Snpe_SNPEBuilder_Delete (snpebuilder_h);
    if (inputstrListHandle)
      Snpe_StringList_Delete (inputstrListHandle);
    if (outputstrListHandle)
      Snpe_StringList_Delete (outputstrListHandle);
  };

  /* default runtime is CPU */
  Snpe_Runtime_t runtime = SNPE_RUNTIME_CPU;

  /* lambda function to handle tensor */
  auto handleTensor = [&] (const char *tensorName, GstTensorInfo *info,
                          Snpe_UserBufferMap_Handle_t bufferMapHandle,
                          Snpe_UserBufferEncoding_ElementType_t type) {
    Snpe_IBufferAttributes_Handle_t bufferAttributesOpt
        = Snpe_SNPE_GetInputOutputBufferAttributes (snpe_h, tensorName);
    if (!bufferAttributesOpt)
      throw std::runtime_error ("Error obtaining buffer attributes");

    auto default_type = Snpe_IBufferAttributes_GetEncodingType (bufferAttributesOpt);

    /* parse tensor data type with user given element type */
    switch (type) {
      case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UNKNOWN:
        /* If the type is not provided by user, use default type */
        type = default_type;
        if (default_type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT) {
          info->type = _NNS_FLOAT32;
        } else if (default_type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8) {
          info->type = _NNS_UINT8;
        } else {
          throw std::invalid_argument ("Unsupported data type");
        }
        break;
      case SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT:
        info->type = _NNS_FLOAT32;
        break;
      case SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8:
        info->type = _NNS_UINT8;
        if (default_type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT) {
          throw std::invalid_argument (
              "ERROR: Quantization parameters are not present in model. Use TF8 type.");
        }
        break;
      default:
        throw std::invalid_argument ("Unsupported data type");
    }

    /* parse tensor dimension */
    auto shapeHandle = Snpe_IBufferAttributes_GetDims (bufferAttributesOpt);
    auto rank = Snpe_TensorShape_Rank (shapeHandle);
    const size_t *sdims = Snpe_TensorShape_GetDimensions (shapeHandle);
    for (size_t j = 0; j < rank; j++) {
      info->dimension[rank - 1 - j] = sdims[j];
    }

    /* calculate strides */
    std::vector<size_t> strides (rank);
    strides[rank - 1] = gst_tensor_get_element_size (info->type);
    for (size_t j = rank - 1; j > 0; j--) {
      strides[j - 1] = strides[j] * sdims[j];
    }

    auto stride_h = Snpe_TensorShape_CreateDimsSize (strides.data (), strides.size ());
    Snpe_TensorShape_Delete (shapeHandle);
    Snpe_IBufferAttributes_Delete (bufferAttributesOpt);

    /* assign user_buffermap */
    size_t bufsize = gst_tensor_info_get_size (info);
    Snpe_UserBufferEncoding_Handle_t ube_h = NULL;
    if (type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8) {
      Snpe_IBufferAttributes_Handle_t bufferAttributesOpt
          = Snpe_SNPE_GetInputOutputBufferAttributes (snpe_h, tensorName);
      Snpe_UserBufferEncoding_Handle_t ubeTfNHandle
          = Snpe_IBufferAttributes_GetEncoding_Ref (bufferAttributesOpt);
      uint64_t stepEquivalentTo0 = Snpe_UserBufferEncodingTfN_GetStepExactly0 (ubeTfNHandle);
      float quantizedStepSize
          = Snpe_UserBufferEncodingTfN_GetQuantizedStepSize (ubeTfNHandle);
      ube_h = Snpe_UserBufferEncodingTfN_Create (stepEquivalentTo0, quantizedStepSize, 8);
      Snpe_IBufferAttributes_Delete (bufferAttributesOpt);
      Snpe_UserBufferEncodingTfN_Delete (ubeTfNHandle);
    } else if (type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT) {
      ube_h = Snpe_UserBufferEncodingFloat_Create ();
    }
    auto iub = Snpe_Util_CreateUserBuffer (NULL, bufsize, stride_h, ube_h);
    this->user_buffers.push_back (iub);

    if (type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8)
      Snpe_UserBufferEncodingTfN_Delete (ube_h);
    else if (type == SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT)
      Snpe_UserBufferEncodingFloat_Delete (ube_h);
    Snpe_TensorShape_Delete (stride_h);

    Snpe_UserBufferMap_Add (bufferMapHandle, tensorName, iub);
  };

  auto parse_custom_prop = [&runtime, &outputstrListHandle, &inputTypeVec,
                               &outputTypeVec] (const char *custom_prop) {
    if (!custom_prop)
      return;

    gchar **options = g_strsplit (custom_prop, ",", -1);

    for (guint op = 0; op < g_strv_length (options); ++op) {
      gchar **option = g_strsplit (options[op], ":", -1);

      if (g_strv_length (option) > 1) {
        g_strstrip (option[0]);
        g_strstrip (option[1]);

        if (g_ascii_strcasecmp (option[0], "Runtime") == 0) {
          if (g_ascii_strcasecmp (option[1], "CPU") == 0) {
            runtime = SNPE_RUNTIME_CPU;
          } else if (g_ascii_strcasecmp (option[1], "GPU") == 0) {
            runtime = SNPE_RUNTIME_GPU;
          } else if (g_ascii_strcasecmp (option[1], "DSP") == 0) {
            runtime = SNPE_RUNTIME_DSP;
          } else if (g_ascii_strcasecmp (option[1], "NPU") == 0
                     || g_ascii_strcasecmp (option[1], "AIP") == 0) {
            runtime = SNPE_RUNTIME_AIP_FIXED8_TF;
          } else {
            nns_logw ("Unknown runtime (%s), set CPU as default.", options[op]);
          }
        } else if (g_ascii_strcasecmp (option[0], "OutputTensor") == 0) {
          /* the tensor name may contain ':' */
          gchar *_ot_str = g_strjoinv (":", &option[1]);
          gchar **names = g_strsplit (_ot_str, ";", -1);
          guint num_names = g_strv_length (names);
          outputstrListHandle = Snpe_StringList_Create ();
          for (guint i = 0; i < num_names; ++i) {
            if (g_strcmp0 (names[i], "") == 0) {
              throw std::invalid_argument ("Given tensor name is invalid.");
            }

            nns_logi ("Add output tensor name of %s", names[i]);
            if (Snpe_StringList_Append (outputstrListHandle, names[i]) != SNPE_SUCCESS) {
              const std::string err_msg = "Failed to append output tensor name: "
                                          + (const std::string) names[i];
              throw std::runtime_error (err_msg);
            }
          }
          g_free (_ot_str);
          g_strfreev (names);
        } else if (g_ascii_strcasecmp (option[0], "OutputType") == 0) {
          gchar **types = g_strsplit (option[1], ";", -1);
          guint num_types = g_strv_length (types);
          for (guint i = 0; i < num_types; ++i) {
            if (g_ascii_strcasecmp (types[i], "FLOAT32") == 0) {
              outputTypeVec.push_back (SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT);
            } else if (g_ascii_strcasecmp (types[i], "TF8") == 0) {
              outputTypeVec.push_back (SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8);
            } else {
              nns_logw ("Ignore unknown output type (%s)", types[i]);
            }
          }
          g_strfreev (types);
        } else if (g_ascii_strcasecmp (option[0], "InputType") == 0) {
          gchar **types = g_strsplit (option[1], ";", -1);
          guint num_types = g_strv_length (types);
          for (guint i = 0; i < num_types; ++i) {
            if (g_ascii_strcasecmp (types[i], "FLOAT32") == 0) {
              inputTypeVec.push_back (SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT);
            } else if (g_ascii_strcasecmp (types[i], "TF8") == 0) {
              inputTypeVec.push_back (SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8);
            } else {
              nns_logw ("Ignore unknown input type (%s)", types[i]);
            }
          }
          g_strfreev (types);
        } else {
          nns_logw ("Unknown option (%s).", options[op]);
        }
      }

      g_strfreev (option);
    }

    g_strfreev (options);
  };

  configured = true;
  try {
    /* Log SNPE version */
    lib_version_h = Snpe_Util_GetLibraryVersion ();
    if (!lib_version_h)
      throw std::runtime_error ("Failed to get SNPE library version");

    nns_logi ("SNPE Version: %s", Snpe_DlVersion_ToString (lib_version_h));

    int32_t ver_major = Snpe_DlVersion_GetMajor (lib_version_h);
    if (ver_major < 2) {
      const std::string err_msg = "Invalid SNPE version, version 2.x is supported but has "
                                  + std::to_string (ver_major) + ".x.";
      nns_loge ("%s", err_msg.c_str ());
      throw std::runtime_error (err_msg);
    }

    /* parse custom properties */
    parse_custom_prop (prop->custom_properties);

    /* Check the given Runtime is available */
    std::string runtime_str = std::string (Snpe_RuntimeList_RuntimeToString (runtime));
    if (Snpe_Util_IsRuntimeAvailable (runtime) == 0)
      throw std::runtime_error ("Given runtime " + runtime_str + " is not available");

    nns_logi ("Given runtime %s is available", runtime_str.c_str ());

    /* set runtimelist config */
    runtime_list_h = Snpe_RuntimeList_Create ();
    if (Snpe_RuntimeList_Add (runtime_list_h, runtime) != SNPE_SUCCESS)
      throw std::runtime_error ("Failed to add given runtime to Snpe_RuntimeList");

    /* Load network (dlc file) */
    if (!g_file_test (prop->model_files[0], G_FILE_TEST_IS_REGULAR)) {
      const std::string err_msg
          = "Given file " + (std::string) prop->model_files[0] + " is not valid";
      throw std::invalid_argument (err_msg);
    }

    model_path = g_strdup (prop->model_files[0]);
    container_h = Snpe_DlContainer_Open (model_path);
    if (!container_h)
      throw std::runtime_error ("Failed to open the model file " + std::string (model_path));

    /* Build SNPE handle */
    snpebuilder_h = Snpe_SNPEBuilder_Create (container_h);
    if (!snpebuilder_h)
      throw std::runtime_error ("Failed to create SNPE builder");

    if (Snpe_SNPEBuilder_SetRuntimeProcessorOrder (snpebuilder_h, runtime_list_h) != SNPE_SUCCESS)
      throw std::runtime_error ("Failed to set runtime processor order");

    /* set UserBuffer mode */
    if (Snpe_SNPEBuilder_SetUseUserSuppliedBuffers (snpebuilder_h, true) != SNPE_SUCCESS)
      throw std::runtime_error ("Failed to set use user supplied buffers");

    /* Set Output Tensors (if given by custom prop) */
    if (outputstrListHandle) {
      if (Snpe_SNPEBuilder_SetOutputTensors (snpebuilder_h, outputstrListHandle) != SNPE_SUCCESS) {
        throw std::runtime_error ("Failed to set output tensors");
      }
    }

    snpe_h = Snpe_SNPEBuilder_Build (snpebuilder_h);
    if (!snpe_h)
      throw std::runtime_error ("Failed to build SNPE handle");

    /* set inputTensorsInfo and inputMap */
    inputMap_h = Snpe_UserBufferMap_Create ();
    inputstrListHandle = Snpe_SNPE_GetInputTensorNames (snpe_h);
    if (!inputMap_h || !inputstrListHandle)
      throw std::runtime_error ("Error while setting Input tensors");

    inputInfo.num_tensors = Snpe_StringList_Size (inputstrListHandle);
    for (size_t i = 0; i < inputInfo.num_tensors; i++) {
      GstTensorInfo *info
          = gst_tensors_info_get_nth_info (std::addressof (inputInfo), i);
      const char *inputName = Snpe_StringList_At (inputstrListHandle, i);
      info->name = g_strdup (inputName);

      auto inputType = SNPE_USERBUFFERENCODING_ELEMENTTYPE_UNKNOWN;

      /* set input type from custom prop if it is provided */
      if (inputTypeVec.size () > i)
        inputType = inputTypeVec[i];
      handleTensor (inputName, info, inputMap_h, inputType);
    }

    /* set outputTensorsInfo and outputMap */
    outputMap_h = Snpe_UserBufferMap_Create ();

    /* Get default output tensor names (if not provided by custom prop) */
    if (outputstrListHandle == NULL)
      outputstrListHandle = Snpe_SNPE_GetOutputTensorNames (snpe_h);

    if (!outputMap_h || !outputstrListHandle)
      throw std::runtime_error ("Error while setting Output tensors");

    outputInfo.num_tensors = Snpe_StringList_Size (outputstrListHandle);
    for (size_t i = 0; i < outputInfo.num_tensors; i++) {
      GstTensorInfo *info
          = gst_tensors_info_get_nth_info (std::addressof (outputInfo), i);
      const char *outputName = Snpe_StringList_At (outputstrListHandle, i);
      info->name = g_strdup (outputName);

      /* set output type from custom prop if it is provided */
      auto outputType = SNPE_USERBUFFERENCODING_ELEMENTTYPE_UNKNOWN;
      if (outputTypeVec.size () > i) {
        outputType = outputTypeVec[i];
      }
      handleTensor (outputName, info, outputMap_h, outputType);
    }

    _clean_handles ();
  } catch (const std::exception &e) {
    _clean_handles ();
    cleanup ();
    /* throw exception upward */
    throw;
  }
}

/**
 * @brief Method to execute the model.
 */
void
snpe_subplugin::invoke (const GstTensorMemory *input, GstTensorMemory *output)
{
  if (!input)
    throw std::runtime_error ("Invalid input buffer, it is NULL.");
  if (!output)
    throw std::runtime_error ("Invalid output buffer, it is NULL.");

  for (unsigned int i = 0; i < inputInfo.num_tensors; i++) {
    GstTensorInfo *info = gst_tensors_info_get_nth_info (std::addressof (inputInfo), i);
    auto iub = Snpe_UserBufferMap_GetUserBuffer_Ref (inputMap_h, info->name);
    Snpe_IUserBuffer_SetBufferAddress (iub, input[i].data);
  }

  for (unsigned int i = 0; i < outputInfo.num_tensors; i++) {
    GstTensorInfo *info = gst_tensors_info_get_nth_info (std::addressof (outputInfo), i);
    auto iub = Snpe_UserBufferMap_GetUserBuffer_Ref (outputMap_h, info->name);
    Snpe_IUserBuffer_SetBufferAddress (iub, output[i].data);
  }

  Snpe_SNPE_ExecuteUserBuffers (snpe_h, inputMap_h, outputMap_h);
}

/**
 * @brief Method to get the information of SNPE subplugin.
 */
void
snpe_subplugin::getFrameworkInfo (GstTensorFilterFrameworkInfo &info)
{
  info.name = name;
  info.allow_in_place = FALSE;
  info.allocate_in_invoke = FALSE;
  info.run_without_model = FALSE;
  info.verify_model_path = TRUE;
  info.hw_list = hw_list;
  info.num_hw = num_hw;
  info.accl_auto = ACCL_CPU;
  info.accl_default = ACCL_CPU;
}

/**
 * @brief Method to get the model information.
 */
int
snpe_subplugin::getModelInfo (
    model_info_ops ops, GstTensorsInfo &in_info, GstTensorsInfo &out_info)
{
  if (ops == GET_IN_OUT_INFO) {
    gst_tensors_info_copy (std::addressof (in_info), std::addressof (inputInfo));
    gst_tensors_info_copy (std::addressof (out_info), std::addressof (outputInfo));
    return 0;
  }

  return -ENOENT;
}

/**
 * @brief Method to handle events.
 */
int
snpe_subplugin::eventHandler (event_ops ops, GstTensorFilterFrameworkEventData &data)
{
  UNUSED (ops);
  UNUSED (data);

  return -ENOENT;
}

snpe_subplugin *snpe_subplugin::registeredRepresentation = nullptr;
const char *snpe_subplugin::name = SNPE_FRAMEWORK_NAME;
const accl_hw snpe_subplugin::hw_list[] = { ACCL_CPU };

/** @brief Initialize this object for tensor_filter subplugin runtime register */
void
snpe_subplugin::init_filter_snpe (void)
{
  registeredRepresentation
      = tensor_filter_subplugin::register_subplugin<snpe_subplugin> ();
  nnstreamer_filter_set_custom_property_desc (SNPE_FRAMEWORK_NAME, "Runtime",
      "Designate hardware resource {'CPU' (default), 'GPU', 'DSP', 'NPU', 'AIP'}",
      "OutputTensor", "Tensor names for the output, separated by ';'. E.g., 'concat:0;concat_1:0'",
      "InputType", "Set the data type of the input {'float32 (default)', 'tf8'}", "OutputType",
      "Set the data type of the output {'float32 (default)', 'tf8'}", NULL);
}

/** @brief Destruct the subplugin */
void
snpe_subplugin::fini_filter_snpe (void)
{
  assert (registeredRepresentation != nullptr);
  tensor_filter_subplugin::unregister_subplugin (registeredRepresentation);
}

/**
 * @brief Register the sub-plugin for SNPE.
 */
void
init_filter_snpe ()
{
  snpe_subplugin::init_filter_snpe ();
}

/**
 * @brief Destruct the sub-plugin for SNPE.
 */
void
fini_filter_snpe ()
{
  snpe_subplugin::fini_filter_snpe ();
}

} /* namespace tensor_filter_snpe */
} /* namespace nnstreamer */
