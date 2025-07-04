/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/backends/vulkan/runtime/graph/ops/OperatorRegistry.h>

#include <executorch/backends/vulkan/runtime/graph/ops/impl/Staging.h>

#include <executorch/backends/vulkan/runtime/graph/ops/utils/StagingUtils.h>

#include <executorch/backends/vulkan/runtime/graph/ops/impl/utils/KernelUtils.h>
#include <executorch/backends/vulkan/runtime/graph/ops/impl/utils/TensorUtils.h>

#include <executorch/backends/vulkan/runtime/graph/ops/utils/ShaderNameUtils.h>

namespace vkcompute {

void resize_conv2d_node(
    ComputeGraph* graph,
    const std::vector<ArgGroup>& args,
    const std::vector<ValueRef>& extra_args) {
  vTensorPtr out = graph->get_tensor(args[0].refs[0]);
  vTensorPtr self = graph->get_tensor(args[1].refs[0]);

  size_t ndim = self->sizes().size();
  std::vector<int64_t> new_out_sizes(ndim);
  const bool transposed = graph->get_bool(extra_args[4]);

  // Batch, Channel
  if (ndim == 4) {
    new_out_sizes.at(ndim - 4) = self->sizes().at(ndim - 4);
  }

  TensorRefPtr weight_ref = graph->get_tref(extra_args[0]);
  const auto& weight_sizes = weight_ref->sizes;
  new_out_sizes.at(ndim - 3) =
      transposed ? weight_sizes.at(ndim - 3) : weight_sizes.at(ndim - 4);

  // Height, Width
  const auto& new_out_sizes_hw = calc_out_sizes_hw(
      *graph,
      self->sizes(),
      extra_args[0],
      /*kernel_size_only = */ false,
      {extra_args[1], extra_args[2], extra_args[3], extra_args[5]},
      transposed);
  new_out_sizes.at(ndim - 2) = new_out_sizes_hw.at(0);
  new_out_sizes.at(ndim - 1) = new_out_sizes_hw.at(1);

  out->virtual_resize(new_out_sizes);
}

void resize_conv1d_node(
    ComputeGraph* graph,
    const std::vector<ArgGroup>& args,
    const std::vector<ValueRef>& extra_args) {
  vTensorPtr out = graph->get_tensor(args[0].refs[0]);
  vTensorPtr self = graph->get_tensor(args[1].refs[0]);
  TensorRefPtr weight_ref = graph->get_tref(extra_args[0]);

  int64_t stride_size = graph->get_int_list(extra_args[1])->at(0);
  int64_t padding_size = graph->get_int_list(extra_args[2])->at(0);
  int64_t dilation_size = graph->get_int_list(extra_args[3])->at(0);

  const std::vector<int64_t>& weight_sizes = weight_ref->sizes;

  const std::vector<int64_t>& in_sizes = self->sizes();
  size_t ndim = in_sizes.size();
  std::vector<int64_t> new_out_sizes(ndim);

  int64_t kernel_size = weight_sizes.at(2);
  int64_t in_length = in_sizes.at(2);

  new_out_sizes.at(0) = in_sizes.at(0);
  new_out_sizes.at(1) = weight_sizes.at(0);
  new_out_sizes.at(2) = calc_out_size(
      in_length, kernel_size, stride_size, padding_size, dilation_size, false);

  out->virtual_resize(new_out_sizes);
}

ValueRef prepack_biases(
    ComputeGraph& graph,
    const ValueRef vref,
    const ValueRef weight,
    const bool transposed,
    const utils::StorageType storage_type,
    const utils::GPUMemoryLayout memory_layout) {
  auto sizes = graph.sizes_of(weight);
  const int64_t out_channels = transposed ? sizes.at(1) : sizes.at(0);

  ValueRef v = graph.add_tensor(
      {out_channels}, graph.dtype_of(weight), storage_type, memory_layout);
  vTensorPtr t = graph.get_tensor(v);

  vkapi::ShaderInfo shader = get_nchw_to_tensor_shader(*t);

  graph.prepack_nodes().emplace_back(new PrepackNode(
      graph,
      shader,
      graph.create_global_wg_size(v),
      graph.create_local_wg_size(v),
      vref,
      v,
      {},
      // Specialization constants
      {t->hashed_layout()},
      {graph.sizes_pc_of(v)}));

  return v;
}

enum class Conv2dMethod : uint8_t {
  Depthwise,
  Pointwise,
  SlidingWindow,
  Transposed,
};

vkapi::ShaderInfo get_conv2d_shader(
    ComputeGraph& graph,
    const api::vTensor& t_out,
    const bool prepack_weights,
    const Conv2dMethod method,
    const ValueRef weight,
    const bool clamp_out = false,
    const bool stride_equals_dilation = false,
    const bool stride_1_padding_0 = false) {
  std::string kernel_name;
  kernel_name.reserve(kShaderNameReserve);
  switch (method) {
    case Conv2dMethod::Depthwise:
      kernel_name = "conv2d_dw";
      if (!prepack_weights) {
        if (!stride_equals_dilation) {
          kernel_name += "_sned";
        }
        const auto& weight_sizes = graph.get_tref(weight)->sizes;
        if (weight_sizes.at(2) == 3 && weight_sizes.at(3) == 3) {
          kernel_name += "_output_tile_3x3";
        }
        if (weight_sizes.at(2) == 5 && weight_sizes.at(3) == 5) {
          kernel_name += "_output_tile_5x5";
        }
      }
      break;
    case Conv2dMethod::Pointwise:
      if (prepack_weights) {
        kernel_name = "conv2d";
      } else {
        kernel_name = stride_1_padding_0 ? "conv2d_pw_s1p0" : "conv2d_pw";
      }
      break;
    case Conv2dMethod::SlidingWindow:
      kernel_name = "conv2d";
      break;
    case Conv2dMethod::Transposed:
      kernel_name = "conv_transpose2d";
      break;
  }
  if (prepack_weights) {
    kernel_name += "_prepack_weights";
  } else if (clamp_out) {
    kernel_name += "_clamp";
  }
  add_dtype_suffix(kernel_name, t_out);

  return VK_KERNEL_FROM_STR(kernel_name);
}

std::vector<int64_t> get_final_sizes(
    const std::vector<int64_t>& original_sizes,
    const Conv2dMethod method) {
  int64_t batch_padded = utils::align_up_4(utils::val_at(-4, original_sizes));
  int64_t channels_padded =
      utils::align_up_4(utils::val_at(-3, original_sizes));
  int64_t height = utils::val_at(-2, original_sizes);
  int64_t width = utils::val_at(-1, original_sizes);

  switch (method) {
    case Conv2dMethod::Depthwise:
      return std::vector<int64_t>{4, batch_padded / 4, height * width};
    case Conv2dMethod::Pointwise:
    case Conv2dMethod::SlidingWindow:
      return std::vector<int64_t>{
          4, batch_padded * height / 4, channels_padded * width};
    case Conv2dMethod::Transposed:
      return std::vector<int64_t>{
          4, channels_padded * height / 4, batch_padded * width};
  }
}

ValueRef prepack_weights(
    ComputeGraph& graph,
    const ValueRef vref,
    const Conv2dMethod method) {
  const auto original_sizes = graph.sizes_of(vref);
  const auto final_sizes = get_final_sizes(original_sizes, method);

  ValueRef v = graph.add_tensor(
      final_sizes,
      graph.dtype_of(vref),
      utils::kTexture2D,
      utils::kChannelsPacked);
  vTensorPtr t = graph.get_tensor(v);

  vkapi::ShaderInfo shader =
      get_conv2d_shader(graph, *t, /*prepack_weights = */ true, method, vref);

  graph.prepack_nodes().emplace_back(new PrepackNode(
      graph,
      shader,
      graph.create_global_wg_size(v),
      graph.create_local_wg_size(v),
      vref,
      v,
      {t->sizes_ubo(),
       graph.create_params_buffer(
           utils::make_ivec4(original_sizes, /*reverse = */ true))},
      // Specialization constants
      {SV(t->packed_dim())}));

  return v;
}

void check_conv_args(const api::vTensor& in, const api::vTensor& out) {
  VK_CHECK_COND(check_packed_dim_is(in, WHCN::kChannelsDim));
  VK_CHECK_COND(check_packed_dim_is(out, WHCN::kChannelsDim));
}

struct Conv2dParams final {
  utils::ivec2 overlay_region;
  int in_group_size;
};

struct OutputParams final {
  float out_min;
  float out_max;
};

Conv2dParams create_conv2d_params(
    ComputeGraph& graph,
    const ValueRef weight,
    const Kernel2dParams& p,
    const bool transposed) {
  const auto& overlay_region = utils::make_ivec2({
      p.kernel_size[0] + (p.kernel_size[0] - 1) * (p.dilation[0] - 1),
      p.kernel_size[1] + (p.kernel_size[1] - 1) * (p.dilation[1] - 1),
  });
  const auto weight_sizes = graph.sizes_of(weight);
  const int32_t in_group_size = utils::safe_downcast<int32_t>(
      utils::align_up_4(transposed ? weight_sizes.at(0) : weight_sizes.at(1)));
  return {overlay_region, in_group_size};
}

void check_conv2d_params(const Kernel2dParams& p, const bool transposed) {
  if (transposed) {
    if (p.dilation[0] > 1 || p.dilation[1] > 1) {
      VK_THROW(
          "aten.convolution.default: transposed = true, dilation > 1 is not supported yet!");
    }
  }
}

Conv2dMethod get_conv2d_method(
    ComputeGraph& graph,
    const ValueRef weight,
    const int64_t groups,
    const bool transposed) {
  const auto weight_sizes = graph.sizes_of(weight);
  if (!transposed && weight_sizes.at(0) == groups && weight_sizes.at(1) == 1) {
    return Conv2dMethod::Depthwise;
  }
  if (groups > 1) {
    VK_THROW("aten.convolution.default: groups > 1 is not supported yet!");
  }
  if (transposed) {
    return Conv2dMethod::Transposed;
  }
  if (weight_sizes.at(2) == 1 && weight_sizes.at(3) == 1) {
    return Conv2dMethod::Pointwise;
  }
  return Conv2dMethod::SlidingWindow;
}

utils::uvec2 get_conv2d_dw_dispatch_divisor(
    const std::vector<int64_t>& weight_sizes) {
  if (weight_sizes.at(2) == 3 && weight_sizes.at(3) == 3) {
    return {4u, 2u};
  }
  if (weight_sizes.at(2) == 5 && weight_sizes.at(3) == 5) {
    return {4u, 2u};
  }
  return {4u, 2u};
}

utils::uvec3 create_conv2d_global_wg_size(
    ComputeGraph& graph,
    const Conv2dMethod method,
    const ValueRef out,
    const ValueRef weight_data,
    const bool stride_equals_dilation) {
  if (method == Conv2dMethod::Pointwise) {
    const utils::uvec3 image_extents = graph.logical_limits_of(out);
    return {
        utils::div_up(image_extents[0u], 1u),
        utils::div_up(image_extents[1u], 4u),
        image_extents[2u]};
  } else if (method == Conv2dMethod::Depthwise && stride_equals_dilation) {
    const utils::uvec3 image_extents = graph.create_global_wg_size(out);
    const utils::uvec2 div =
        get_conv2d_dw_dispatch_divisor(graph.get_tref(weight_data)->sizes);
    return {
        utils::div_up(image_extents[0], div[0]),
        utils::div_up(image_extents[1], div[1]),
        image_extents[2]};
  } else {
    return graph.create_global_wg_size(out);
  }
}

void add_conv2d_node(
    ComputeGraph& graph,
    const ValueRef in,
    const ValueRef weight_data,
    const ValueRef bias,
    const ValueRef stride,
    const ValueRef padding,
    const ValueRef dilation,
    const ValueRef transposed,
    const ValueRef output_padding,
    const ValueRef groups,
    const ValueRef out_min,
    const ValueRef out_max,
    const ValueRef out,
    const bool clamp_out) {
  const bool transposed_val = graph.get_bool(transposed);

  float out_min_val = 0.0f;
  float out_max_val = 0.0f;
  if (out_min != kDummyValueRef) {
    out_min_val = graph.extract_scalar<float>(out_min);
  }
  if (out_max != kDummyValueRef) {
    out_max_val = graph.extract_scalar<float>(out_max);
  }

  const int64_t groups_val = graph.get_int(groups);

  const Conv2dMethod method =
      get_conv2d_method(graph, weight_data, groups_val, transposed_val);

  ValueRef arg_weight = prepack_weights(graph, weight_data, method);
  ValueRef arg_bias = prepack_biases(
      graph,
      bias,
      weight_data,
      transposed_val,
      /* storage_type = */ utils::kTexture2D,
      /* memory_layout = */ utils::kWidthPacked);

  vTensorPtr t_in = graph.get_tensor(in);
  vTensorPtr t_out = graph.get_tensor(out);
  if (t_in->sizes().at(0) > 1) {
    VK_THROW("conv2d: input batch size > 1 is not supported yet!");
  }
  check_conv_args(*t_in, *t_out);

  Kernel2dParams kernel_params = create_kernel2d_params(
      graph,
      weight_data,
      /*kernel_size_only = */ false,
      stride,
      padding,
      dilation);
  Conv2dParams extra_params =
      create_conv2d_params(graph, weight_data, kernel_params, transposed_val);

  const bool stride_equals_dilation =
      (kernel_params.stride[0] == kernel_params.dilation[0] &&
       kernel_params.stride[1] == kernel_params.dilation[1]);

  const bool stride_1_padding_0 =
      (kernel_params.stride[0] == 1 && kernel_params.stride[1] == 1 &&
       kernel_params.padding[0] == 0 && kernel_params.padding[1] == 0);

  OutputParams out_params = {out_min_val, out_max_val};

  check_conv2d_params(kernel_params, transposed_val);

  vkapi::ShaderInfo shader = get_conv2d_shader(
      graph,
      *t_out,
      /*prepack_weights = */ false,
      method,
      weight_data,
      clamp_out,
      stride_equals_dilation,
      stride_1_padding_0);

  utils::uvec3 wg_size = create_conv2d_global_wg_size(
      graph, method, out, weight_data, stride_equals_dilation);

  if (method == Conv2dMethod::Depthwise) {
    wg_size = {wg_size[0] * wg_size[1] * wg_size[2], 1, 1};
  } else if (method == Conv2dMethod::Pointwise) {
    wg_size = {wg_size[0] * wg_size[1], wg_size[2], 1};
  }

  utils::uvec3 local_wg_size;
  if (method == Conv2dMethod::Pointwise) {
    uint32_t local_wg_size_y = 1;
    if (wg_size[1] % 8 == 0) {
      local_wg_size_y = 8;
    } else if (wg_size[1] % 4 == 0) {
      local_wg_size_y = 4;
    } else if (wg_size[1] % 2 == 0) {
      local_wg_size_y = 2;
    }
    local_wg_size = {64 / local_wg_size_y, local_wg_size_y, 1};
  } else {
    local_wg_size = graph.create_local_wg_size(wg_size);
  }

  vkapi::ParamsBindList param_buffers;
  std::vector<PushConstantDataInfo> push_constants;
  if (method == Conv2dMethod::Pointwise) {
    const utils::ivec4 kernel_param_stride_pad = {
        kernel_params.stride[0],
        kernel_params.stride[1],
        kernel_params.padding[0],
        kernel_params.padding[1],
    };

    struct Conv2dPWParams final {
      int in_group_size;
      int dummy_padding;
      OutputParams out_params;
    } param{extra_params.in_group_size, 0, out_params};

    push_constants = {
        graph.logical_limits_pc_of(out),
        PushConstantDataInfo(
            &kernel_param_stride_pad, sizeof(kernel_param_stride_pad)),
        PushConstantDataInfo(&param, sizeof(param)),
    };
  } else if (method == Conv2dMethod::Depthwise) {
    const utils::ivec4 kernel_param_size_stride = {
        kernel_params.kernel_size[0],
        kernel_params.kernel_size[1],
        kernel_params.stride[0],
        kernel_params.stride[1]};

    const utils::ivec4 kernel_param_pad_dial = {
        kernel_params.padding[0],
        kernel_params.padding[1],
        kernel_params.dilation[0],
        kernel_params.dilation[1]};

    push_constants = {
        graph.logical_limits_pc_of(out),
        graph.sizes_pc_of(in),
        PushConstantDataInfo(
            &kernel_param_size_stride, sizeof(kernel_param_size_stride)),
        PushConstantDataInfo(
            &kernel_param_pad_dial, sizeof(kernel_param_pad_dial)),
        PushConstantDataInfo(
            &extra_params, sizeof(extra_params), sizeof(utils::ivec4)),
        PushConstantDataInfo(&out_params, sizeof(out_params)),
    };
  } else {
    param_buffers = {
        t_out->logical_limits_ubo(),
        t_in->sizes_ubo(),
        graph.create_params_buffer(kernel_params),
        graph.create_params_buffer(extra_params),
        graph.create_params_buffer(out_params),
    };
  }

  graph.execute_nodes().emplace_back(new DispatchNode(
      graph,
      shader,
      wg_size,
      local_wg_size,
      // Inputs and Outputs
      {{out, vkapi::kWrite}, {{in, arg_weight, arg_bias}, vkapi::kRead}},
      // Shader params buffers
      param_buffers,
      // Push Constants
      push_constants,
      // Specialization Constants
      {},
      // Resize Args
      {weight_data, stride, padding, dilation, transposed, output_padding},
      // Resizing Logic
      resize_conv2d_node));
}

void add_conv1d_node(
    ComputeGraph& graph,
    const ValueRef in,
    const ValueRef weight,
    const ValueRef bias,
    const ValueRef stride,
    const ValueRef padding,
    const ValueRef dilation,
    const ValueRef groups,
    const ValueRef out_min,
    const ValueRef out_max,
    const ValueRef out,
    const bool clamp_out) {
  ValueRef arg_weight = prepack_standard(
      graph,
      weight,
      graph.storage_type_of(out),
      utils::kChannelsPacked,
      /* passthrough = */ false,
      utils::kOptimizedAxisMap);
  ValueRef arg_bias = prepack_biases(
      graph,
      bias,
      weight,
      /*transposed = */ false,
      /*storage_type = */ utils::kTexture3D,
      /*memory_layout = */ utils::kWidthPacked);

  float out_min_val = 0.0f;
  float out_max_val = 0.0f;
  if (out_min != kDummyValueRef) {
    out_min_val = graph.extract_scalar<float>(out_min);
  }
  if (out_max != kDummyValueRef) {
    out_max_val = graph.extract_scalar<float>(out_max);
  }

  vTensorPtr t_in = graph.get_tensor(in);
  vTensorPtr t_weight = graph.get_tensor(arg_weight);
  vTensorPtr t_bias = graph.get_tensor(arg_bias);
  vTensorPtr t_out = graph.get_tensor(out);
  const int64_t groups_val = graph.get_int(groups);

  std::vector<int64_t> in_sizes = t_in->sizes();
  std::vector<int64_t> weight_sizes = t_weight->sizes();
  std::vector<int64_t> out_sizes = t_out->sizes();

  check_conv_args(*t_in, *t_out);

  const int32_t in_channels = in_sizes.at(1);
  const int32_t out_channels = weight_sizes.at(0);
  const int32_t kernel_size = weight_sizes.at(2);
  const int32_t stride_size = graph.get_int_list(stride)->at(0);
  const int32_t padding_size = graph.get_int_list(padding)->at(0);
  const int32_t dilation_size = graph.get_int_list(dilation)->at(0);
  const int32_t in_group_size = static_cast<int64_t>(in_channels / groups_val);
  const int32_t out_group_size =
      static_cast<int64_t>(out_channels / groups_val);

  const utils::uvec3 global_size = {
      // out length
      graph.size_at<uint32_t>(-1, out),
      // out channels
      static_cast<uint32_t>(out_channels),
      // out batches
      utils::div_up_4(graph.size_at<uint32_t>(-3, out))};
  const utils::uvec3 local_size = graph.create_local_wg_size(global_size);

  Kernel1dParams kernel_params = {
      kernel_size,
      stride_size,
      padding_size,
      dilation_size,
      in_group_size,
      out_group_size};

  const OutputParams out_params = {out_min_val, out_max_val};

  std::string kernel_name("conv1d");
  if (clamp_out) {
    kernel_name += "_clamp";
  }
  kernel_name.reserve(kShaderNameReserve);

  add_dtype_suffix(kernel_name, *t_out);

  graph.execute_nodes().emplace_back(new DispatchNode(
      graph,
      VK_KERNEL_FROM_STR(kernel_name),
      global_size,
      local_size,
      // Inputs and Outputs
      {{out, vkapi::kWrite}, {{in, arg_weight, arg_bias}, vkapi::kRead}},
      // Shader params buffers
      {
          t_out->logical_limits_ubo(),
          t_in->sizes_ubo(),
          graph.create_params_buffer(kernel_params),
          graph.create_params_buffer(out_params),
      },
      // Push Constants
      {},
      // Specialization Constants
      {t_out->hashed_layout(),
       t_in->hashed_layout(),
       t_weight->hashed_layout(),
       t_bias->hashed_layout()},
      // Resize Args
      {weight, stride, padding, dilation},
      // Resizing Logic
      resize_conv1d_node));
}

void conv(ComputeGraph& graph, const std::vector<ValueRef>& args) {
  int64_t in_ndim = graph.get_tensor(args[0])->sizes().size();
  if (in_ndim == 4) {
    if (args.size() == 10) {
      // ordinary conv2d
      return add_conv2d_node(
          graph,
          args[0],
          args[1],
          args[2],
          args[3],
          args[4],
          args[5],
          args[6],
          args[7],
          args[8],
          /*out_min = */ kDummyValueRef,
          /*out_max = */ kDummyValueRef,
          args[9],
          false);
    } else {
      // conv2d with clamp
      return add_conv2d_node(
          graph,
          args[0],
          args[1],
          args[2],
          args[3],
          args[4],
          args[5],
          args[6],
          args[7],
          args[8],
          args[9],
          args[10],
          args[11],
          true);
    }
  } else {
    if (args.size() == 10) {
      // ordinary conv1d
      return add_conv1d_node(
          graph,
          args[0],
          args[1],
          args[2],
          args[3],
          args[4],
          args[5],
          args[8],
          /*out_min = */ kDummyValueRef,
          /*out_max = */ kDummyValueRef,
          args[9],
          false);
    } else {
      // conv1d with clamp
      return add_conv1d_node(
          graph,
          args[0],
          args[1],
          args[2],
          args[3],
          args[4],
          args[5],
          args[8],
          args[9],
          args[10],
          args[11],
          true);
    }
  }
}

REGISTER_OPERATORS {
  VK_REGISTER_OP(aten.convolution.default, conv);
  VK_REGISTER_OP(conv_with_clamp.default, conv);
  VK_REGISTER_OP(et_vk.conv_with_clamp.default, conv);
}

} // namespace vkcompute
