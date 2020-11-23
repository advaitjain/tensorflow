/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/compiler/xla/service/space_to_batch_converter.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <queue>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "absl/algorithm/algorithm.h"
#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/dfs_hlo_visitor_with_default.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/bitmap.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/stream_executor/lib/statusor.h"

namespace xla {

namespace {

namespace m = match;

// ConvolutionVisitor traverses the HLO computation and rewrites Convolution
// operations with small batch counts into convolutions with larger batch
// counts by moving space to batch.
class ConvolutionVisitor {
 public:
  // Top-level function to begin space-to-batch conversion.
  Status PerformSpaceToBatchOnConvolution(HloInstruction* convolution);

  // Struct containing details about a convolution.
  struct ConvDetails {
    int64 spatial_dimension_to_split, inherent_low_padding,
        inherent_high_padding, stride, spatial_size, base_dilation_factor,
        halo_size, high_padding_for_conv, low_padding_for_conv,
        kernel_spatial_dim_size, input_dim_size;
  };

  // Return a struct containing various necessary information pieces for
  // performing space-to-batch on a convolution.
  ConvDetails GetConvolutionDetails(HloInstruction* convolution,
                                    ConvolutionDimensionNumbers& dim_numbers);

  // Function that determines if space-to-batch can be propagated into the
  // consumer. Such propagation is only possible when all required operands are
  // space-to-batch'ed.
  bool CanPropagate(HloInstruction* consumer, HloInstruction* producer);

  // This function checks if the HLO instrution supports propagation.
  bool SupportedOpForPropagation(HloInstruction* consumer,
                                 HloInstruction* producer);

  // Method that checks validity of Broadcast propagation.
  bool IsBroadcastPropagatable(HloInstruction* broadcast,
                               HloInstruction* old_other_op);

  // Propagates space-to-batch on the op, and returns a bool that indicates if
  // the users of the op need to be propagated through.
  StatusOr<bool> Propagate(HloInstruction* consumer, HloInstruction* producer);

  // Perform space-to-batch propagation on the convolution. Assumes the
  // activations were already space-to-batched.
  Status PropagateOnConv(HloInstruction* convolution);

  // Perform space-to-batch propagation on the backprop filter convolution.
  // Assumes the activations and kernel were already space-to-batched.
  Status PropagateOnBackpropFilterConv(HloInstruction* convolution);

  // Method that checks validity of space-to-batch on a given convolution.
  bool IsConvSuitableForSpaceToBatch(HloInstruction* convolution);

  // Once a convolution has been space-to-batch'ed, this function will
  // transitively propagate the space-to-batch-ness on rest of the graph.
  Status PropagateOnUsers(HloInstruction* old_conv);

  // Generates masked output with valid data. This is useful when larger shapes
  // are generated due to space-to-batch.
  StatusOr<HloInstruction*> SelectValidPortion(
      HloInstruction* new_instr, HloInstruction* old_instr,
      HloInstruction* select_val, int64 new_batch_dim, int64 new_space_dim,
      int64 old_batch_dim, int64 old_space_dim);

  // Performs tranposition so that space dimension follows the batch dimension.
  StatusOr<HloInstruction*> BringSpaceNextToBatch(
      HloInstruction* activations, ConvolutionDimensionNumbers& dim_numbers,
      int64& spatial_dimension_to_split, int64& activations_batch_dim);

  // Function that converts spaced-to-batch shape back to the original.
  StatusOr<HloInstruction*> BatchToSpace(HloInstruction* old_instr);

  // Duplicates elements at boundaries.
  StatusOr<HloInstruction*> HaloDuplicateWithSlice(
      HloInstruction* activations, int64 spatial_dimension_to_split,
      int64 activations_batch_dim, int64 old_batch_size, int64 low_padding,
      int64 high_padding, int64 halo_size, int64 original_split_dim_size,
      HloInstruction* pad_val = nullptr);

  // Runs the visitor on a computation.
  StatusOr<bool> Run();

  // Returns whether any convolution ops were rewritten.
  const bool changed() const { return changed_; }

  ~ConvolutionVisitor() = default;

  explicit ConvolutionVisitor(int64 limit_on_batch_size,
                              HloComputation* computation);

  int64 get_chosen_spatial_dim(HloInstruction* convolution) {
    return convolution->convolution_dimension_numbers()
               .input_spatial_dimensions_size() -
           1;
  }

  int64 DimLookUp(absl::Span<const int64> permute_dims, int64 id) {
    return permute_dims[id];
  }

  HloInstruction* DoesConvolutionFeedReduceWindow(HloInstruction* instr,
                                                  int64 depth);

 private:
  // Current HloComputation instance the ConvolutionVisitor is traversing.
  HloComputation* computation_;

  absl::flat_hash_set<HloInstruction*> convs_to_visit_;
  std::vector<HloInstruction*> conv_visitor_list_;
  HloInstructionSet non_propagatable_instrs_;
  // Map from a given spaced-to-batch instruction to its batched-to-space
  // version.
  absl::flat_hash_map<HloInstruction*, HloInstruction*> batch_to_space_map_;

  // Map from old (non space-to-batch) instructions to space-to-batch'ed
  // instructions.
  absl::flat_hash_map<HloInstruction*, HloInstruction*> old_to_new_instrs_;

  // Map from instruction to dimensions of the shape (first is batch, second is
  // space). This is with respect to the old instruction.
  absl::flat_hash_map<HloInstruction*, std::pair<int64, int64>>
      instr_to_dim_map_;

  // Map from space-to-batch'ed instruction to its permute dims.
  absl::flat_hash_map<HloInstruction*, std::vector<int64>>
      instr_to_dim_permute_map_;

  // Whether rewrite has occurred.
  bool changed_ = false;

  // Limit on batch size to apply this technique on.
  int64 limit_on_batch_size_;

  // We choose the new batch size to be a constant so that space-to-batch
  // propagation through several convolutional layers is consistent.
  static constexpr int64 kNewBatchSize = 8;

  // Depth for searching reduce window
  static constexpr int64 kReduceWindowSearchDepth = 10;
};

ConvolutionVisitor::ConvolutionVisitor(int64 limit_on_batch_size,
                                       HloComputation* computation) {
  computation_ = computation;
  limit_on_batch_size_ = limit_on_batch_size;
  for (HloInstruction* inst : computation->MakeInstructionPostOrder()) {
    if (inst->opcode() != HloOpcode::kConvolution) {
      continue;
    }

    auto convolution = inst;
    // Perform legality checks.
    if (!IsConvSuitableForSpaceToBatch(convolution)) {
      VLOG(1) << "Conv not suitable for space-to-batch "
              << convolution->ToString();
      continue;
    }
    VLOG(1) << "Conv added to space-to-batch worklist "
            << convolution->ToString();
    convs_to_visit_.insert(convolution);
    conv_visitor_list_.push_back(convolution);
  }
}

bool ConvolutionVisitor::IsConvSuitableForSpaceToBatch(
    HloInstruction* convolution) {
  ConvolutionDimensionNumbers dim_numbers =
      convolution->convolution_dimension_numbers();

  // If there are no spatial dims, we return.
  if (dim_numbers.input_spatial_dimensions_size() < 1) {
    return false;
  }

  // Batch in batch_group_count has different semantics (it isn't true batch).
  // Consider supporting this case in future if needed.
  if (convolution->batch_group_count() != 1) {
    return false;
  }

  if (convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .window_dilation() != 1) {
    return false;
  }

  const ConvDetails c = GetConvolutionDetails(convolution, dim_numbers);

  const int64 low_pad = convolution->window()
                            .dimensions(get_chosen_spatial_dim(convolution))
                            .padding_low();

  // TODO(b/168316428): Support base dilations more generically.
  if (c.base_dilation_factor != 1) {
    if (c.stride != 1) {
      return false;
    }
    // For low pad of 0, only support a pointwise kernel.
    if (low_pad == 0) {
      if (c.kernel_spatial_dim_size != 1) {
        return false;
      }
    } else if (c.kernel_spatial_dim_size != c.base_dilation_factor + 1 ||
               low_pad != c.base_dilation_factor - 1) {
      // Only support dilations such that base dilation factor and low pad are
      // compatible with kernel_spatial_dim_size to be compatible with
      // HaloDuplicateWithSlice.
      return false;
    }
  }

  int64 activations_batch_dim = dim_numbers.input_batch_dimension();

  const int64 old_batch_size =
      convolution->operand(0)->shape().dimensions(activations_batch_dim);

  if (old_batch_size > limit_on_batch_size_) {
    return false;
  }
  // We currently only cater to evenly divisible cases.
  if (kNewBatchSize % old_batch_size != 0) {
    return false;
  }

  VLOG(1) << "spatial size " << c.spatial_size;

  const int64 num_splits = kNewBatchSize / old_batch_size;
  // If the ratio is not within the 2X range, we can't Halo Pad from the next
  // split.
  if (c.halo_size > CeilOfRatio(c.spatial_size, num_splits)) {
    return false;
  }
  VLOG(1) << "Legal space-to-batch convolution " << convolution->ToString();
  return true;
}

StatusOr<HloInstruction*> ConvolutionVisitor::HaloDuplicateWithSlice(
    HloInstruction* activations, int64 spatial_dimension_to_split,
    int64 activations_batch_dim, int64 old_batch_size, int64 low_padding,
    int64 high_padding, int64 halo_size, int64 original_split_dim_size,
    HloInstruction* pad_val) {
  const int64 rank = activations->shape().rank();
  const int64 spatial_split_size =
      activations->shape().dimensions(spatial_dimension_to_split);
  const int64 batch_size =
      activations->shape().dimensions(activations_batch_dim);

  CHECK_LE(std::abs(halo_size - low_padding), spatial_split_size);
  VLOG(1) << "In HaloDuplicateWithSlice with activations "
          << activations->ToString() << " batch_size " << batch_size
          << " spatial_split_size " << spatial_split_size << " low_padding "
          << low_padding << " halo size " << halo_size;

  HloInstruction* first_slice = nullptr;

  std::vector<int64> strides(rank, 1);
  HloInstruction* padding =
      pad_val == nullptr
          ? computation_->AddInstruction(HloInstruction::CreateConstant(
                LiteralUtil::Zero(activations->shape().element_type())))
          : pad_val;

  if (low_padding > 0) {
    std::vector<int64> start_indices(rank, 0),
        end_indices(activations->shape().dimensions().begin(),
                    activations->shape().dimensions().end());
    start_indices[spatial_dimension_to_split] =
        spatial_split_size - low_padding;
    end_indices[activations_batch_dim] = batch_size - 1;
    end_indices[spatial_dimension_to_split] = spatial_split_size;

    TF_ASSIGN_OR_RETURN(first_slice, MakeSliceHlo(activations, start_indices,
                                                  end_indices, strides));
    VLOG(1) << "first slice " << first_slice->ToString();
    PaddingConfig padding_config =
        MakeNoPaddingConfig(first_slice->shape().dimensions_size());
    padding_config.mutable_dimensions(activations_batch_dim)
        ->set_edge_padding_low(1);

    TF_ASSIGN_OR_RETURN(first_slice,
                        MakePadHlo(first_slice, padding, padding_config));
  }

  HloInstruction* halo_region = nullptr;
  if (halo_size - low_padding > 0) {
    std::vector<int64> start_indices_halo(rank, 0),
        end_indices_halo(activations->shape().dimensions().begin(),
                         activations->shape().dimensions().end());

    start_indices_halo[activations_batch_dim] = 1;
    end_indices_halo[spatial_dimension_to_split] = halo_size - low_padding;

    TF_ASSIGN_OR_RETURN(halo_region,
                        MakeSliceHlo(activations, start_indices_halo,
                                     end_indices_halo, strides));
    VLOG(1) << "halo_region " << halo_region->ToString();
    PaddingConfig padding_config_halo =
        MakeNoPaddingConfig(halo_region->shape().dimensions_size());
    padding_config_halo.mutable_dimensions(activations_batch_dim)
        ->set_edge_padding_high(1);
    TF_ASSIGN_OR_RETURN(halo_region,
                        MakePadHlo(halo_region, padding, padding_config_halo));
  }

  if (halo_size == 0 && low_padding != 0) {
    std::vector<int64> start_indices_activations_cut(rank, 0),
        end_indices_activations_cut(activations->shape().dimensions().begin(),
                                    activations->shape().dimensions().end());
    // When no halo is needed, we must slice out activations.
    if (low_padding > 0) {
      end_indices_activations_cut[spatial_dimension_to_split] =
          spatial_split_size - low_padding;
    } else {
      start_indices_activations_cut[spatial_dimension_to_split] =
          0 - low_padding;
      end_indices_activations_cut[spatial_dimension_to_split] =
          spatial_split_size;
    }

    TF_ASSIGN_OR_RETURN(activations,
                        MakeSliceHlo(activations, start_indices_activations_cut,
                                     end_indices_activations_cut, strides));
  }

  if (first_slice != nullptr) {
    TF_ASSIGN_OR_RETURN(activations, MakeConcatHlo({first_slice, activations},
                                                   spatial_dimension_to_split));
  }

  if (halo_region != nullptr) {
    TF_ASSIGN_OR_RETURN(activations, MakeConcatHlo({activations, halo_region},
                                                   spatial_dimension_to_split));
  }
  VLOG(1) << "HaloDuplicated activations " << activations->ToString();
  return activations;
}

StatusOr<HloInstruction*> ConvolutionVisitor::BringSpaceNextToBatch(
    HloInstruction* activations, ConvolutionDimensionNumbers& dim_numbers,
    int64& spatial_dimension_to_split, int64& activations_batch_dim) {
  ConvolutionDimensionNumbers new_dim_numbers = dim_numbers;
  if (spatial_dimension_to_split != activations_batch_dim + 1) {
    int64 pushed_counter = 0;
    std::vector<int64> transpose_dims;
    int64 new_batch_dim, new_spatial_dim;
    for (int i = 0; i < activations->shape().rank(); ++i) {
      if (i == activations_batch_dim) {
        continue;
      }
      if (i == spatial_dimension_to_split) {
        transpose_dims.push_back(activations_batch_dim);
        new_batch_dim = pushed_counter;
        pushed_counter++;
        new_spatial_dim = pushed_counter;
      }

      if (i == dim_numbers.input_feature_dimension()) {
        new_dim_numbers.set_input_feature_dimension(pushed_counter);
      } else {
        for (int j = 0; j < dim_numbers.input_spatial_dimensions_size(); ++j) {
          if (i == dim_numbers.input_spatial_dimensions(j)) {
            new_dim_numbers.set_input_spatial_dimensions(j, pushed_counter);
            break;
          }
        }
      }
      transpose_dims.push_back(i);
      pushed_counter++;
    }

    activations_batch_dim = new_batch_dim;
    spatial_dimension_to_split = new_spatial_dim;
    TF_ASSIGN_OR_RETURN(activations,
                        MakeTransposeHlo(activations, transpose_dims));
  }

  new_dim_numbers.set_input_batch_dimension(activations_batch_dim);
  dim_numbers = new_dim_numbers;

  return activations;
}

StatusOr<bool> ConvolutionVisitor::Run() {
  for (auto conv : conv_visitor_list_) {
    if (convs_to_visit_.count(conv) > 0) {
      TF_CHECK_OK(PerformSpaceToBatchOnConvolution(conv));
    }
  }
  conv_visitor_list_.clear();
  convs_to_visit_.clear();
  // Iterate through all instructions that we could not propagate through, and
  // turn their operands from batch-to-space as needed.
  for (auto instr : non_propagatable_instrs_) {
    VLOG(1) << "Could not eventually propagate through " << instr->ToString();
    absl::flat_hash_map<int64, HloInstruction*> operand_map;
    for (int64 i = 0; i < instr->operand_count(); ++i) {
      if (old_to_new_instrs_.count(instr->mutable_operand(i))) {
        TF_ASSIGN_OR_RETURN(operand_map[i],
                            BatchToSpace(instr->mutable_operand(i)));
      }
    }
    for (auto entry : operand_map) {
      TF_CHECK_OK(instr->ReplaceOperandWith(entry.first, entry.second));
    }
  }
  non_propagatable_instrs_.clear();
  return changed_;
}

bool IsTrivialElementwise(HloInstruction* hlo) {
  if (hlo->opcode() == HloOpcode::kFusion || hlo->opcode() == HloOpcode::kRng ||
      hlo->opcode() == HloOpcode::kCopy ||
      hlo->opcode() == HloOpcode::kConstant ||
      hlo->opcode() == HloOpcode::kIota) {
    return false;
  }
  return hlo->IsElementwise();
}

bool ConvolutionVisitor::CanPropagate(HloInstruction* consumer,
                                      HloInstruction* producer) {
  if (IsTrivialElementwise(consumer)) {
    VLOG(2) << "Doing propagation check on elementwise op: "
            << consumer->ToString();

    HloInstruction* pivot_operand = nullptr;
    for (int64 i = 0; i < consumer->operand_count(); ++i) {
      auto old_producer = consumer->mutable_operand(i);
      const bool broadcast_or_constant =
          (old_producer->opcode() == HloOpcode::kConstant) ||
          (old_producer->opcode() == HloOpcode::kBroadcast &&
           IsBroadcastPropagatable(old_producer, producer));

      if (!old_to_new_instrs_.contains(old_producer) &&
          !broadcast_or_constant) {
        VLOG(1) << "Cannot propagate on elementwise op " << consumer->ToString()
                << " because operand " << old_producer->ToString()
                << " isn't ready ";
        return false;
      } else {
        if (broadcast_or_constant) {
          VLOG(2) << "Skipping on " << old_producer->ToString();
          continue;
        }

        CHECK(old_to_new_instrs_.contains(old_producer));

        CHECK(instr_to_dim_map_.contains(old_producer));
        if (pivot_operand == nullptr) {
          pivot_operand = old_producer;
          VLOG(2) << "Elementwise op: pivot " << old_producer->ToString();
        } else {
          if (instr_to_dim_map_[pivot_operand] !=
              instr_to_dim_map_[old_producer]) {
            VLOG(2) << "Elementwise op: checking for shape equivalence "
                    << consumer->ToString()
                    << " failed due to changed batch space ordering ";
            return false;
          }
          auto pivot_new_instr = old_to_new_instrs_[pivot_operand];
          auto pivot_permute_dims = instr_to_dim_permute_map_[pivot_new_instr];
          auto new_instr = old_to_new_instrs_[old_producer];
          auto permute_dims = instr_to_dim_permute_map_[new_instr];
          for (int j = 0; j < pivot_permute_dims.size(); ++j) {
            // Ensure the dimension mapping is the same.
            if (pivot_permute_dims[j] != permute_dims[j]) {
              VLOG(2) << "Elementwise op: checking for shape equivalence "
                      << consumer->ToString()
                      << " failed due to permuted dimensions ";
              return false;
            }

            // Make sure all other dimensions are of the same size.
            if (pivot_new_instr->shape().dimensions(j) !=
                new_instr->shape().dimensions(j)) {
              if (!(consumer->IsElementwiseBinary() &&
                    j == instr_to_dim_map_[pivot_operand].second)) {
                VLOG(2) << "Elementwise op: checking for shape equivalence "
                        << consumer->ToString()
                        << " failed due to changed shape sizes ";
                return false;
              }
            }
          }
        }
      }
    }
  }

  if (consumer->opcode() == HloOpcode::kConvolution) {
    VLOG(1) << "Checking if conv is supported for propagation";
    if (IsConvSuitableForSpaceToBatch(consumer)) {
      for (int64 i = 0; i < consumer->operand_count(); ++i) {
        auto old_producer = consumer->mutable_operand(i);
        if (i == 0 && !old_to_new_instrs_.contains(old_producer)) {
          return false;
        }
      }
      return true;
    }

    // Check for space-to-depth readiness here. Note this is not done in
    // SupportedOpForPropagation because the readiness is dependent upon
    // space-to-batchedness of the operands.

    // We currently only support stride of 1.
    if (consumer->window()
            .dimensions(get_chosen_spatial_dim(consumer))
            .stride() != 1) {
      return false;
    }

    // All operands must be space-to-batched.
    for (int64 i = 0; i < consumer->operand_count(); ++i) {
      auto old_producer = consumer->mutable_operand(i);
      if (!old_to_new_instrs_.contains(old_producer)) {
        return false;
      }
      if (!instr_to_dim_permute_map_.contains(
              old_to_new_instrs_[old_producer])) {
        return false;
      }
    }

    auto first_operand = old_to_new_instrs_[consumer->mutable_operand(0)];
    auto dim_map_val_op_0 = instr_to_dim_map_[consumer->mutable_operand(0)];
    auto second_operand = old_to_new_instrs_[consumer->mutable_operand(1)];
    auto dim_map_val_op_1 = instr_to_dim_map_[consumer->mutable_operand(1)];

    auto permute_dims_first_operand = instr_to_dim_permute_map_[first_operand];
    auto permute_dims_second_operand =
        instr_to_dim_permute_map_[second_operand];

    const int64 new_batch_dim_operand_0 =
        DimLookUp(permute_dims_first_operand, dim_map_val_op_0.first);
    const int64 new_space_dim_operand_0 =
        DimLookUp(permute_dims_first_operand, dim_map_val_op_0.second);

    const int64 new_batch_dim_operand_1 =
        DimLookUp(permute_dims_second_operand, dim_map_val_op_1.first);
    const int64 new_space_dim_operand_1 =
        DimLookUp(permute_dims_second_operand, dim_map_val_op_1.second);

    if (first_operand->shape().dimensions(new_batch_dim_operand_0) !=
        second_operand->shape().dimensions(new_batch_dim_operand_1)) {
      return false;
    }

    const int64 rhs_dilation = consumer->window()
                                   .dimensions(get_chosen_spatial_dim(consumer))
                                   .window_dilation();
    if (first_operand->shape().dimensions(new_space_dim_operand_0) !=
        rhs_dilation *
            second_operand->shape().dimensions(new_space_dim_operand_1)) {
      return false;
    }

    VLOG(2) << "Backprop filter conv ready for propagation";

    return true;
  }

  if (consumer->opcode() == HloOpcode::kReduceWindow ||
      consumer->opcode() == HloOpcode::kReduce) {
    for (int64 i = 0; i < consumer->operand_count(); ++i) {
      auto old_producer = consumer->mutable_operand(i);
      if (i == 0 && !old_to_new_instrs_.contains(old_producer)) {
        return false;
      }
    }
  }

  if (consumer->opcode() == HloOpcode::kSelectAndScatter) {
    // We currently only support adds in the scatter.
    auto scatter_comp = consumer->scatter();
    if (!Match(scatter_comp->root_instruction(),
               m::AddAnyOrder(m::Parameter(0), m::Parameter(1)))) {
      return false;
    }

    for (int64 i = 0; i < consumer->operand_count(); ++i) {
      auto old_producer = consumer->mutable_operand(i);
      if (i < 2 && !old_to_new_instrs_.contains(old_producer)) {
        return false;
      }
    }

    auto first_operand = old_to_new_instrs_[consumer->mutable_operand(0)];
    auto dim_map_val_op_0 = instr_to_dim_map_[consumer->mutable_operand(0)];
    auto second_operand = old_to_new_instrs_[consumer->mutable_operand(1)];

    auto permute_dims_first_operand = instr_to_dim_permute_map_[first_operand];
    auto permute_dims_second_operand =
        instr_to_dim_permute_map_[second_operand];

    // The permuting must match.
    if (permute_dims_first_operand != permute_dims_second_operand) {
      return false;
    }

    const int64 old_batch_dim = dim_map_val_op_0.first;
    const int64 old_space_dim = dim_map_val_op_0.second;

    const int64 new_batch_dim =
        DimLookUp(permute_dims_first_operand, old_batch_dim);
    const int64 new_space_dim =
        DimLookUp(permute_dims_first_operand, old_space_dim);

    if (first_operand->shape().dimensions(new_batch_dim) !=
        second_operand->shape().dimensions(new_batch_dim)) {
      return false;
    }

    const int64 stride = consumer->window().dimensions(old_space_dim).stride();
    const int64 pad_high =
        consumer->window().dimensions(old_space_dim).padding_high();
    const int64 pad_low =
        consumer->window().dimensions(old_space_dim).padding_low();

    if ((first_operand->shape().dimensions(new_space_dim) + pad_high +
         pad_low) /
            stride !=
        second_operand->shape().dimensions(new_space_dim)) {
      return false;
    }
    VLOG(1) << "Can propagate through select and scatter";
    return true;
  }
  return true;
}

bool ConvolutionVisitor::IsBroadcastPropagatable(HloInstruction* broadcast,
                                                 HloInstruction* old_other_op) {
  CHECK_EQ(broadcast->opcode(), HloOpcode::kBroadcast);
  CHECK(instr_to_dim_map_.contains(old_other_op));

  auto result = instr_to_dim_map_[old_other_op];
  const int64 batch_dim = result.first;
  const int64 space_dim = result.second;
  auto broadcast_dims = broadcast->dimensions();
  return !absl::c_linear_search(broadcast_dims, batch_dim) &&
         !absl::c_linear_search(broadcast_dims, space_dim);
}

bool ConvolutionVisitor::SupportedOpForPropagation(HloInstruction* consumer,
                                                   HloInstruction* producer) {
  if (IsTrivialElementwise(consumer)) {
    for (int64 i = 0; i < consumer->operand_count(); ++i) {
      if (consumer->operand(i)->opcode() == HloOpcode::kBroadcast) {
        if (!IsBroadcastPropagatable(consumer->mutable_operand(i), producer)) {
          VLOG(2) << "Could not propagate through broadcast";
          return false;
        }
      }
    }
    return true;
  }

  if (consumer->opcode() == HloOpcode::kConvolution) {
    return true;
  }

  if (consumer->opcode() == HloOpcode::kReduce) {
    // Support only the trivial case where both batch and split spatial dim are
    // being reduced

    auto reduce_dims = consumer->dimensions();
    auto result = instr_to_dim_map_[consumer->mutable_operand(0)];
    const int64 batch_dim = result.first;
    const int64 space_dim = result.second;
    VLOG(1) << "Checking if reduce is supported batch_dim " << batch_dim
            << "  space_dim " << space_dim << " reduce "
            << consumer->ToString();
    return absl::c_linear_search(reduce_dims, batch_dim) &&
           absl::c_linear_search(reduce_dims, space_dim);
  }

  if (consumer->opcode() == HloOpcode::kReduceWindow ||
      consumer->opcode() == HloOpcode::kSelectAndScatter) {
    auto first_operand = consumer->mutable_operand(0);
    auto window = consumer->window();
    if (instr_to_dim_map_.count(first_operand) <= 0) {
      VLOG(1) << "Dim map not found on windowed operand. Window dim count "
              << window.dimensions().size();
      return false;
    }
    // Disallow windowing on on the batch dim
    auto result = instr_to_dim_map_[first_operand];
    const int64 old_batch_dim = result.first;
    const int64 old_space_dim = result.second;
    if (window.dimensions(old_batch_dim).size() != 1) {
      return false;
    }

    // Only allow no-low-padding cases.
    if (window.dimensions(old_space_dim).padding_low() != 0) {
      return false;
    }

    // Only allow small high pads.
    if (window.dimensions(old_space_dim).padding_high() >
        window.dimensions(old_space_dim).size()) {
      return false;
    }

    // Operand 0 must have been propagated through
    if (old_to_new_instrs_.count(first_operand) <= 0) {
      return false;
    }

    auto new_operand = old_to_new_instrs_[first_operand];
    auto permute_dims = instr_to_dim_permute_map_[new_operand];
    const int64 new_space_dim = DimLookUp(permute_dims, old_space_dim);

    // Make sure that the stride lines up.
    if (window.dimensions(old_space_dim).size() != 1) {
      if (new_operand->shape().dimensions(new_space_dim) %
              window.dimensions(old_space_dim).stride() !=
          0) {
        return false;
      }
    }

    return true;
  }

  return false;
}

StatusOr<bool> ConvolutionVisitor::Propagate(HloInstruction* consumer,
                                             HloInstruction* producer) {
  auto computation = consumer->parent();
  if (IsTrivialElementwise(consumer)) {
    auto dim_map_val = instr_to_dim_map_[producer];
    auto new_consumer = computation->AddInstruction(consumer->Clone());
    if (consumer->IsElementwiseBinary()) {
      for (int64 i = 0; i < 2; ++i) {
        if (consumer->operand(i)->opcode() == HloOpcode::kBroadcast) {
          break;
        }
        CHECK(old_to_new_instrs_.contains(consumer->mutable_operand(i)));
        if (i == 1) {
          // Choose the larger shape to be used as the producer.
          if (old_to_new_instrs_[consumer->mutable_operand(0)]
                  ->shape()
                  .dimensions() >
              old_to_new_instrs_[consumer->mutable_operand(1)]
                  ->shape()
                  .dimensions()) {
            producer = consumer->mutable_operand(0);
          } else {
            producer = consumer->mutable_operand(1);
          }
        }
      }
    }

    for (int64 i = 0; i < consumer->operand_count(); ++i) {
      if (consumer->operand(i)->opcode() == HloOpcode::kBroadcast) {
        CHECK(old_to_new_instrs_.contains(producer));
        auto new_producer = old_to_new_instrs_[producer];
        auto permute_dims = instr_to_dim_permute_map_[new_producer];
        std::vector<int64> broadcast_dims;
        for (auto j : consumer->operand(i)->dimensions()) {
          broadcast_dims.push_back(DimLookUp(permute_dims, j));
        }
        auto new_broadcast = MakeBroadcastHlo(
            consumer->mutable_operand(i)->mutable_operand(0), broadcast_dims,
            new_producer->shape().dimensions());
        VLOG(1) << "Created broadcast " << new_broadcast->ToString();
        TF_CHECK_OK(
            new_consumer->ReplaceOperandWithDifferentShape(i, new_broadcast));
      } else {
        CHECK(old_to_new_instrs_.contains(consumer->mutable_operand(i)));
        HloInstruction* operand_to_use = nullptr;

        auto result = instr_to_dim_map_[producer];
        const int64 old_batch_dim = result.first;
        const int64 old_space_dim = result.second;
        const int64 old_batch_size =
            producer->shape().dimensions(old_batch_dim);
        HloInstruction* new_instr =
            old_to_new_instrs_[consumer->mutable_operand(i)];
        HloInstruction* pivot_new_instr = old_to_new_instrs_[producer];

        auto permute_dims = instr_to_dim_permute_map_[new_instr];
        const int64 batch_dim = DimLookUp(permute_dims, old_batch_dim);
        const int64 space_dim = DimLookUp(permute_dims, old_space_dim);
        const int64 batch_size = new_instr->shape().dimensions(batch_dim);

        if (new_instr->shape().dimensions(space_dim) !=
            pivot_new_instr->shape().dimensions(space_dim)) {
          // Because we do not propagate through transposes, the batch should
          // always be followed by the split space dimension.
          CHECK_EQ(batch_dim + 1, space_dim);

          // Reshape to 1D, pad to the producer's size, reshape back to 2D.
          std::vector<int64> new_dimensions(
              new_instr->shape().dimensions().begin(),
              new_instr->shape().dimensions().end());
          new_dimensions[space_dim] *= (batch_size / old_batch_size);
          new_dimensions[batch_dim] = old_batch_size;

          TF_ASSIGN_OR_RETURN(HloInstruction * reshape,
                              MakeReshapeHlo(new_dimensions, new_instr));

          const int64 pivot_space_size =
              pivot_new_instr->shape().dimensions(space_dim) * batch_size /
              old_batch_size;

          CHECK_GT(pivot_space_size, new_dimensions[space_dim]);

          PaddingConfig padding_config =
              MakeNoPaddingConfig(reshape->shape().dimensions_size());
          padding_config.mutable_dimensions(space_dim)->set_edge_padding_high(
              pivot_space_size - new_dimensions[space_dim]);
          padding_config.mutable_dimensions(space_dim)->set_edge_padding_low(0);
          HloInstruction* padding =
              computation_->AddInstruction(HloInstruction::CreateConstant(
                  LiteralUtil::Zero(reshape->shape().element_type())));

          TF_ASSIGN_OR_RETURN(HloInstruction * padded_operand,
                              MakePadHlo(reshape, padding, padding_config));

          TF_ASSIGN_OR_RETURN(
              operand_to_use,
              MakeReshapeHlo(pivot_new_instr->shape().dimensions(),
                             padded_operand));

        } else {
          operand_to_use = old_to_new_instrs_[consumer->mutable_operand(i)];
        }
        TF_CHECK_OK(
            new_consumer->ReplaceOperandWithDifferentShape(i, operand_to_use));
      }
    }
    auto old_type = new_consumer->mutable_shape()->element_type();
    *(new_consumer->mutable_shape()) = old_to_new_instrs_[producer]->shape();

    // The element type needs to be retained.
    new_consumer->mutable_shape()->set_element_type(old_type);

    old_to_new_instrs_[consumer] = new_consumer;
    instr_to_dim_map_[consumer] = dim_map_val;
    CHECK(instr_to_dim_permute_map_.contains(old_to_new_instrs_[producer]));
    instr_to_dim_permute_map_[new_consumer] = std::vector<int64>(
        instr_to_dim_permute_map_[old_to_new_instrs_[producer]]);

    VLOG(2) << " new_consumer " << new_consumer->ToString()
            << " old_to_new_instrs_[producer] "
            << old_to_new_instrs_[producer]->ToString() << " permute dims "
            << instr_to_dim_permute_map_.count(new_consumer);

    return true;
  }

  if (consumer->opcode() == HloOpcode::kConvolution) {
    if (IsConvSuitableForSpaceToBatch(consumer)) {
      TF_CHECK_OK(PropagateOnConv(consumer));
      return true;
    } else {
      TF_CHECK_OK(PropagateOnBackpropFilterConv(consumer));
      return false;
    }
  }

  if (consumer->opcode() == HloOpcode::kReduce) {
    auto new_consumer = computation->AddInstruction(consumer->Clone());
    auto first_operand = old_to_new_instrs_[consumer->mutable_operand(0)];

    auto dim_map_val = instr_to_dim_map_[consumer->mutable_operand(0)];
    const int64 old_batch_dim = dim_map_val.first;
    const int64 old_space_dim = dim_map_val.second;
    auto permute_dims = instr_to_dim_permute_map_[first_operand];
    const int64 new_batch_dim = DimLookUp(permute_dims, old_batch_dim);
    const int64 new_space_dim = DimLookUp(permute_dims, old_space_dim);

    TF_ASSIGN_OR_RETURN(
        first_operand,
        SelectValidPortion(first_operand, consumer->mutable_operand(0),
                           consumer->mutable_operand(1), new_batch_dim,
                           new_space_dim, old_batch_dim, old_space_dim));

    std::vector<int64> changed_dims(new_consumer->dimensions().size());
    for (int64 i = 0; i < new_consumer->dimensions().size(); ++i) {
      changed_dims[i] = DimLookUp(permute_dims, new_consumer->dimensions(i));
    }
    *(new_consumer->mutable_dimensions()) = changed_dims;
    // Replace operand 0.
    TF_CHECK_OK(
        new_consumer->ReplaceOperandWithDifferentShape(0, first_operand));
    // We do not set instr_to_dim_permute_map_ here because no further
    // propagation is needed here.
    old_to_new_instrs_[consumer] = new_consumer;
    instr_to_dim_map_[consumer] = dim_map_val;

    // Since the resultant ordering of dimension is the same as before, no
    // further propagation is needed.
    return false;
  }

  if (consumer->opcode() == HloOpcode::kReduceWindow ||
      consumer->opcode() == HloOpcode::kSelectAndScatter) {
    bool is_select_and_scatter =
        consumer->opcode() == HloOpcode::kSelectAndScatter;
    auto first_operand = old_to_new_instrs_[consumer->mutable_operand(0)];

    auto init_val = is_select_and_scatter ? consumer->mutable_operand(2)
                                          : consumer->mutable_operand(1);
    auto dim_map_val = instr_to_dim_map_[consumer->mutable_operand(0)];
    const int64 old_batch_dim = dim_map_val.first;
    const int64 old_space_dim = dim_map_val.second;
    auto permute_dims = instr_to_dim_permute_map_[first_operand];
    const int64 new_batch_dim = DimLookUp(permute_dims, old_batch_dim);
    const int64 new_space_dim = DimLookUp(permute_dims, old_space_dim);

    TF_ASSIGN_OR_RETURN(
        first_operand,
        SelectValidPortion(first_operand, consumer->mutable_operand(0),
                           init_val, new_batch_dim, new_space_dim,
                           old_batch_dim, old_space_dim));

    // Calculate the required halo size
    auto new_shape = first_operand->shape();
    auto old_shape = consumer->mutable_operand(0)->shape();

    const int64 new_batch_size = new_shape.dimensions(new_batch_dim);
    const int64 new_space_size = new_shape.dimensions(new_space_dim);
    const int64 stride = consumer->window().dimensions(old_space_dim).stride();
    const int64 window_size =
        consumer->window().dimensions(old_space_dim).size();
    const int64 last_overlap_point = ((new_space_size - 1) / stride) * stride;
    VLOG(1) << "last_overlap_point " << last_overlap_point << " window_size "
            << window_size << " new_space_size " << new_space_size;

    const int64 halo_size = last_overlap_point + window_size - new_space_size;
    if (halo_size > 0) {
      TF_ASSIGN_OR_RETURN(first_operand,
                          HaloDuplicateWithSlice(first_operand, new_space_dim,
                                                 new_batch_dim, new_batch_size,
                                                 /*low_padding=*/0,
                                                 /*high_padding=*/0, halo_size,
                                                 new_space_size, init_val));
    }

    Window new_win;
    for (int64 i = 0; i < consumer->window().dimensions().size(); ++i) {
      auto dim = DimLookUp(permute_dims, i);
      new_win.add_dimensions();
      new_win.mutable_dimensions(i)->set_stride(
          consumer->window().dimensions(dim).stride());
      new_win.mutable_dimensions(i)->set_size(
          consumer->window().dimensions(dim).size());
      if (i == old_space_dim) {
        new_win.mutable_dimensions(i)->set_padding_high(0);
        new_win.mutable_dimensions(i)->set_padding_low(0);
      } else {
        new_win.mutable_dimensions(i)->set_padding_high(
            consumer->window().dimensions(dim).padding_high());
        new_win.mutable_dimensions(i)->set_padding_low(
            consumer->window().dimensions(dim).padding_low());
      }
      new_win.mutable_dimensions(i)->set_window_dilation(
          consumer->window().dimensions(dim).window_dilation());
      new_win.mutable_dimensions(i)->set_base_dilation(
          consumer->window().dimensions(dim).base_dilation());
      new_win.mutable_dimensions(i)->set_window_reversal(
          consumer->window().dimensions(dim).window_reversal());
    }

    new_shape = first_operand->shape();

    HloInstruction* new_consumer = nullptr;
    if (is_select_and_scatter) {
      auto second_operand = old_to_new_instrs_[consumer->mutable_operand(1)];

      auto select_comp = consumer->select();

      auto scatter_comp = consumer->scatter();
      TF_ASSIGN_OR_RETURN(
          auto new_select_and_scatter_shape,
          ShapeInference::InferSelectAndScatterShape(
              new_shape, select_comp->ComputeProgramShape(), new_win,
              second_operand->shape(), init_val->shape(),
              scatter_comp->ComputeProgramShape()));
      new_consumer =
          computation_->AddInstruction(HloInstruction::CreateSelectAndScatter(
              new_select_and_scatter_shape, first_operand, select_comp, new_win,
              second_operand, init_val, scatter_comp));
      // Replace operand 0.
      TF_CHECK_OK(
          new_consumer->ReplaceOperandWithDifferentShape(0, first_operand));
      // Replace operand 1.
      TF_CHECK_OK(
          new_consumer->ReplaceOperandWithDifferentShape(1, second_operand));
      VLOG(2) << "New select and scatter " << new_consumer->ToString();

      // If the window size was larger than the stride, there could be overlaps.
      // Such cases require updates from both overlaps to be applied.
      if (halo_size > 0) {
        const int64 rank = new_consumer->shape().rank();

        const int64 batch_size =
            new_consumer->shape().dimensions(new_batch_dim);

        std::vector<int64> start_indices(rank, 0),
            end_indices(new_consumer->shape().dimensions().begin(),
                        new_consumer->shape().dimensions().end()),
            strides(rank, 1);
        start_indices[new_space_dim] = new_space_size;
        end_indices[new_space_dim] = new_space_size + halo_size;
        end_indices[new_batch_dim] = batch_size - 1;

        // This is the slice from halo padding.
        TF_ASSIGN_OR_RETURN(
            HloInstruction * bottom,
            MakeSliceHlo(new_consumer, start_indices, end_indices, strides));

        std::vector<int64> start_indices_top(rank, 0),
            end_indices_top(new_consumer->shape().dimensions().begin(),
                            new_consumer->shape().dimensions().end());
        end_indices_top[new_space_dim] = halo_size;
        // The first batch has correct data.
        start_indices_top[new_batch_dim] = 1;

        // This is the original area from where halo pad was extracted.
        TF_ASSIGN_OR_RETURN(HloInstruction * top,
                            MakeSliceHlo(new_consumer, start_indices_top,
                                         end_indices_top, strides));

        HloInstruction* default_fill =
            MakeBroadcastHlo(init_val, {}, top->shape().dimensions());

        // Compare to see if the bottom area was changed.
        TF_ASSIGN_OR_RETURN(
            HloInstruction * bottom_compare,
            MakeCompareHlo(ComparisonDirection::kNe, bottom, default_fill));

        // Take out only the changed values.
        TF_ASSIGN_OR_RETURN(
            HloInstruction * bottom_taken,
            MakeSelectHlo(bottom_compare, bottom, default_fill));

        // Compare to see if the top area was changed.
        TF_ASSIGN_OR_RETURN(
            HloInstruction * top_compare,
            MakeCompareHlo(ComparisonDirection::kNe, top, default_fill));

        // Take out only the changed values.
        TF_ASSIGN_OR_RETURN(HloInstruction * top_taken,
                            MakeSelectHlo(top_compare, top, bottom_taken));

        // This makes checks if the area was updated by both overlaps.
        TF_ASSIGN_OR_RETURN(
            HloInstruction * both_compare,
            MakeBinaryHlo(HloOpcode::kAnd, top_compare, bottom_compare));

        // If it was, add them up.
        TF_ASSIGN_OR_RETURN(HloInstruction * both_added,
                            MakeBinaryHlo(HloOpcode::kAdd, top, bottom));

        // Pad the final result to the original shape.
        TF_ASSIGN_OR_RETURN(HloInstruction * final_selection,
                            MakeSelectHlo(both_compare, both_added, top_taken));

        PaddingConfig padding_config =
            MakeNoPaddingConfig(final_selection->shape().dimensions_size());
        padding_config.mutable_dimensions(new_batch_dim)
            ->set_edge_padding_low(1);
        padding_config.mutable_dimensions(new_space_dim)
            ->set_edge_padding_high(new_space_size);
        HloInstruction* padding =
            computation_->AddInstruction(HloInstruction::CreateConstant(
                LiteralUtil::Zero(final_selection->shape().element_type())));

        TF_ASSIGN_OR_RETURN(
            final_selection,
            MakePadHlo(final_selection, padding, padding_config));

        tensorflow::core::Bitmap b(batch_size * (new_space_size + halo_size));
        for (int k = 0; k < batch_size * (new_space_size + halo_size); ++k) {
          const int64 space_index = k % (new_space_size + halo_size);
          const int64 batch_index = (k / (new_space_size + halo_size));
          if (batch_index < 1 || space_index >= halo_size) {
            b.set(k);
          } else {
            b.clear(k);
          }
        }

        auto arg_literal = LiteralUtil::CreateR1(b);
        VLOG(4) << "Slice mask created: arg literal " << arg_literal.ToString();
        HloInstruction* slice_mask = computation_->AddInstruction(
            HloInstruction::CreateConstant(std::move(arg_literal)));

        std::vector<int64> slice_mask_reshape_dims(2);
        slice_mask_reshape_dims[0] = batch_size;
        slice_mask_reshape_dims[1] = (new_space_size + halo_size);

        TF_ASSIGN_OR_RETURN(
            HloInstruction * slice_mask_reshaped,
            MakeReshapeHlo(slice_mask_reshape_dims, slice_mask));

        // Broadcast the mask in all dimensions.
        HloInstruction* shape_mask = MakeBroadcastHlo(
            slice_mask_reshaped, {new_batch_dim, new_space_dim},
            final_selection->shape().dimensions());

        TF_ASSIGN_OR_RETURN(
            new_consumer,
            MakeSelectHlo(shape_mask, new_consumer, final_selection));
      }

      auto previous_shape =
          old_to_new_instrs_[consumer->mutable_operand(0)]->shape();
      std::vector<int64> start_indices(previous_shape.rank(), 0),
          end_indices(previous_shape.dimensions().begin(),
                      previous_shape.dimensions().end()),
          strides(previous_shape.rank(), 1);

      TF_ASSIGN_OR_RETURN(
          new_consumer,
          MakeSliceHlo(new_consumer, start_indices, end_indices, strides));

    } else {
      auto reduce_comp = consumer->to_apply();
      TF_ASSIGN_OR_RETURN(auto new_reduce_window_shape,
                          ShapeInference::InferReduceWindowShape(
                              new_shape, init_val->shape(), new_win));
      new_consumer =
          computation_->AddInstruction(HloInstruction::CreateReduceWindow(
              new_reduce_window_shape, first_operand, init_val, new_win,
              reduce_comp));
      // Replace operand 0.
      TF_CHECK_OK(
          new_consumer->ReplaceOperandWithDifferentShape(0, first_operand));
      VLOG(1) << "New reduce window " << new_consumer->ToString();
    }

    old_to_new_instrs_[consumer] = new_consumer;
    instr_to_dim_map_[consumer] = dim_map_val;

    instr_to_dim_permute_map_[new_consumer] = std::vector<int64>(
        instr_to_dim_permute_map_[old_to_new_instrs_[consumer->mutable_operand(
            0)]]);

    return true;
  }

  LOG(FATAL) << "Trying to propagate through an unsupported instruction "
             << consumer->ToString();
  return true;
}

StatusOr<HloInstruction*> ConvolutionVisitor::SelectValidPortion(
    HloInstruction* new_instr, HloInstruction* old_instr,
    HloInstruction* select_val, int64 new_batch_dim, int64 new_space_dim,
    int64 old_batch_dim, int64 old_space_dim) {
  auto new_shape = new_instr->shape();
  auto old_shape = old_instr->shape();
  VLOG(1) << "In SelectValidPortion new_batch_dim " << new_batch_dim
          << " new_space_dim " << new_space_dim << " old_batch_dim "
          << old_batch_dim << " old_space_dim " << old_space_dim;
  const int64 new_batch_size = new_shape.dimensions(new_batch_dim);
  const int64 new_space_size = new_shape.dimensions(new_space_dim);
  const int64 old_batch_size = old_shape.dimensions(old_batch_dim);
  const int64 old_space_size = old_shape.dimensions(old_space_dim);
  CHECK_EQ(new_batch_size % old_batch_size, 0);
  const int64 num_splits = new_batch_size / old_batch_size;
  // Build a constant PRED to decide which elements in the split dimension
  // are from halo.
  tensorflow::core::Bitmap b(new_batch_size * new_space_size);
  for (int k = 0; k < new_batch_size * new_space_size; ++k) {
    const int64 space_index = k % new_space_size;
    const int64 batch_index = (k / new_space_size) % num_splits;
    if (batch_index * new_space_size + space_index < old_space_size) {
      b.set(k);
    } else {
      b.clear(k);
    }
  }

  auto arg_literal = LiteralUtil::CreateR1(b);
  VLOG(4) << "Slice mask created: arg literal " << arg_literal.ToString();
  HloInstruction* slice_mask = computation_->AddInstruction(
      HloInstruction::CreateConstant(std::move(arg_literal)));

  std::vector<int64> slice_mask_reshape_dims(2);
  slice_mask_reshape_dims[0] = new_batch_size;
  slice_mask_reshape_dims[1] = new_space_size;

  TF_ASSIGN_OR_RETURN(HloInstruction * slice_mask_reshaped,
                      MakeReshapeHlo(slice_mask_reshape_dims, slice_mask));

  // Broadcast the mask in all dimensions of the activations.
  HloInstruction* shape_mask =
      MakeBroadcastHlo(slice_mask_reshaped, {new_batch_dim, new_space_dim},
                       new_instr->shape().dimensions());

  VLOG(1) << "Shape mask made " << shape_mask->ToString();

  HloInstruction* zeroes =
      MakeBroadcastHlo(select_val, {}, new_instr->shape().dimensions());

  TF_ASSIGN_OR_RETURN(new_instr, MakeSelectHlo(shape_mask, new_instr, zeroes));

  return new_instr;
}

StatusOr<HloInstruction*> ConvolutionVisitor::BatchToSpace(
    HloInstruction* old_instr) {
  if (batch_to_space_map_.count(old_instr)) {
    return batch_to_space_map_[old_instr];
  }
  auto result = instr_to_dim_map_[old_instr];
  const int64 old_batch_dim = result.first;
  const int64 old_space_dim = result.second;

  const int64 old_batch_size = old_instr->shape().dimensions(old_batch_dim);
  CHECK(old_to_new_instrs_.contains(old_instr));
  auto new_instr = old_to_new_instrs_[old_instr];
  VLOG(2) << "old_batch_dim " << old_batch_dim << " old_space_dim "
          << old_space_dim << " new_instr " << new_instr->ToString()
          << " permute dims " << instr_to_dim_permute_map_.count(new_instr);
  CHECK(instr_to_dim_permute_map_.contains(new_instr));
  auto permute_dims = instr_to_dim_permute_map_[new_instr];
  const int64 batch_dim = DimLookUp(permute_dims, old_batch_dim);
  const int64 space_dim = DimLookUp(permute_dims, old_space_dim);
  const int64 batch_size = new_instr->shape().dimensions(batch_dim);

  std::vector<int64> new_dimensions(new_instr->shape().dimensions().begin(),
                                    new_instr->shape().dimensions().end());
  new_dimensions[space_dim] *= (batch_size / old_batch_size);
  new_dimensions[batch_dim] = old_batch_size;
  // Reshape the output of the new conv into the old convolutions shape.
  TF_ASSIGN_OR_RETURN(HloInstruction * reshape,
                      MakeReshapeHlo(new_dimensions, new_instr));

  const int64 rank = old_instr->shape().rank();
  std::vector<int64> start_indices(rank, 0),
      end_indices(new_dimensions.begin(), new_dimensions.end()),
      strides(rank, 1);
  end_indices[space_dim] = old_instr->shape().dimensions(old_space_dim);

  // This slicing is getting rid of the padding we added to evenly divide space.
  TF_ASSIGN_OR_RETURN(
      HloInstruction * output_slice,
      MakeSliceHlo(reshape, start_indices, end_indices, strides));
  VLOG(1) << "Batch to space slice " << output_slice->ToString();
  std::vector<int64> transpose_dims(permute_dims);
  TF_ASSIGN_OR_RETURN(HloInstruction * output_transpose,
                      MakeTransposeHlo(output_slice, transpose_dims));

  old_instr->SetupDerivedInstruction(output_transpose);

  batch_to_space_map_[old_instr] = output_transpose;
  return output_transpose;
}

Status ConvolutionVisitor::PropagateOnUsers(HloInstruction* old_conv) {
  std::queue<std::pair<HloInstruction*, HloInstruction*>> propagation_worklist;

  if (old_conv->user_count() == 0) {
    TF_ASSIGN_OR_RETURN(HloInstruction * batch_to_space,
                        BatchToSpace(old_conv));
    VLOG(1) << "Replacing the root instruction to "
            << batch_to_space->ToString();
    TF_CHECK_OK(computation_->ReplaceInstruction(old_conv, batch_to_space));
    VLOG(1) << "Replacement successful";
    return Status::OK();
  }

  int64 iteration_count = 0;
  propagation_worklist.push(
      std::make_pair(old_conv, old_conv->mutable_operand(0)));

  while (!propagation_worklist.empty()) {
    auto top = propagation_worklist.front();
    auto node = top.first;
    auto parent = top.second;
    VLOG(1) << "Traversing for propagation operating on " << node->ToString();
    propagation_worklist.pop();

    // Don't work on the same node again.
    if (old_to_new_instrs_.count(node) > 0 && iteration_count != 0) {
      continue;
    }

    bool needs_further_propagation = true;
    if (iteration_count != 0) {
      // Do the space-to-batch propagation on this node.
      TF_ASSIGN_OR_RETURN(needs_further_propagation, Propagate(node, parent));
    }
    iteration_count++;
    // If this is the root, no room for further propagation.
    if (node->parent()->root_instruction() == node) {
      // The below case does not need going back to space.
      if (!needs_further_propagation) {
        VLOG(1) << "Replacing the root instruction to "
                << old_to_new_instrs_[node]->ToString();
        TF_CHECK_OK(
            computation_->ReplaceInstruction(node, old_to_new_instrs_[node]));
        continue;
      }

      TF_ASSIGN_OR_RETURN(HloInstruction * batch_to_space, BatchToSpace(node));
      VLOG(1) << "Replacing the root instruction to "
              << batch_to_space->ToString();
      TF_CHECK_OK(computation_->ReplaceInstruction(node, batch_to_space));
    } else {
      if (!needs_further_propagation) {
        TF_CHECK_OK(
            computation_->ReplaceInstruction(node, old_to_new_instrs_[node]));
        continue;
      }
      // Insert all users into the queue, as long as the ops are supported and
      // the op is ready for propagation. If the op is unsupported, do
      // batch-to-space. If not ready, mark as non-propagatable.
      for (auto user : node->users()) {
        if (!SupportedOpForPropagation(user, node)) {
          VLOG(1) << "Unsupported op found " << user->ToString();
          TF_ASSIGN_OR_RETURN(HloInstruction * batch_to_space,
                              BatchToSpace(node));
          for (int64 i = 0; i < user->operand_count(); ++i) {
            if (user->operand(i) == node) {
              TF_CHECK_OK(user->ReplaceOperandWith(i, batch_to_space));
            }
          }
          continue;
        }
        // If the instruction is ready for propagation, add it to the queue.
        if (CanPropagate(user, node)) {
          non_propagatable_instrs_.erase(user);
          propagation_worklist.push(std::make_pair(user, node));
        } else {
          // Mark it as non-propagatable for now, for later revisiting.
          non_propagatable_instrs_.insert(user);
        }
      }
    }
  }
  return Status::OK();
}

Status ConvolutionVisitor::PropagateOnConv(HloInstruction* convolution) {
  auto activations_old = convolution->mutable_operand(0);

  CHECK(old_to_new_instrs_.contains(activations_old));
  auto activations_new = old_to_new_instrs_[activations_old];
  auto permute_dims = instr_to_dim_permute_map_[activations_new];

  auto original_conv_dims = convolution->convolution_dimension_numbers();

  const int64 old_space_dim = original_conv_dims.input_spatial_dimensions(
      get_chosen_spatial_dim(convolution));
  const int64 old_split_dim_size =
      convolution->mutable_operand(0)->shape().dimensions(old_space_dim);

  auto permuted_conv_dims_numbers = original_conv_dims;

  int64 activations_batch_dim =
      DimLookUp(permute_dims, original_conv_dims.input_batch_dimension());
  int64 activations_feature_dim =
      DimLookUp(permute_dims, original_conv_dims.input_feature_dimension());
  permuted_conv_dims_numbers.set_input_batch_dimension(activations_batch_dim);
  permuted_conv_dims_numbers.set_input_feature_dimension(
      activations_feature_dim);

  for (int64 i = 0; i < original_conv_dims.input_spatial_dimensions_size();
       ++i) {
    permuted_conv_dims_numbers.set_input_spatial_dimensions(
        i, DimLookUp(permute_dims,
                     original_conv_dims.input_spatial_dimensions(i)));
  }

  const int64 old_batch_dim = original_conv_dims.input_batch_dimension();
  const int64 old_batch_size =
      activations_old->shape().dimensions(old_batch_dim);

  ConvDetails c =
      GetConvolutionDetails(convolution, permuted_conv_dims_numbers);

  VLOG(1) << "Propagating on conv activations_batch_dim "
          << activations_batch_dim << " spatial_dimension_to_split "
          << c.spatial_dimension_to_split << " old_batch_size "
          << old_batch_size;
  TF_ASSIGN_OR_RETURN(activations_new,
                      BringSpaceNextToBatch(
                          activations_new, permuted_conv_dims_numbers,
                          c.spatial_dimension_to_split, activations_batch_dim));

  auto select_val = computation_->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(activations_new->shape().element_type())));

  TF_ASSIGN_OR_RETURN(
      activations_new,
      SelectValidPortion(activations_new, activations_old, select_val,
                         activations_batch_dim, c.spatial_dimension_to_split,
                         old_batch_dim, old_space_dim));
  // Create the new convolution dim numbers.
  auto new_dim_numbers = permuted_conv_dims_numbers;

  VLOG(1) << "spatial size " << c.spatial_size;

  const int64 num_splits = kNewBatchSize / old_batch_size;

  const int64 output_offsets = convolution->shape().dimensions(
      permuted_conv_dims_numbers.output_spatial_dimensions(
          get_chosen_spatial_dim(convolution)));
  const int64 output_offsets_per_split =
      CeilOfRatio(output_offsets, num_splits);

  int64 spatial_split_size =
      CeilOfRatio(output_offsets_per_split, c.base_dilation_factor) * c.stride;

  // Keep increasing the split size so that overall size isn't smaller than the
  // original spatial dimension. Unlike for the first space-to-batch'ed
  // convolution, while propagating, we can use the last halo_size as available
  // spatial size.
  while (spatial_split_size * num_splits + c.halo_size - c.spatial_size < 0) {
    spatial_split_size += c.stride;
  }

  int64 slice_size = spatial_split_size + c.halo_size;

  VLOG(1) << "spatial_split_size " << spatial_split_size << " slice_size "
          << slice_size;

  const int64 new_batch_size =
      activations_new->shape().dimensions(activations_batch_dim);
  const int64 new_space_size =
      activations_new->shape().dimensions(c.spatial_dimension_to_split);
  // In the below case, we cannot use the activations directly for Halo
  // Duplication. We must reshape them.
  if (spatial_split_size > new_space_size) {
    std::vector<int64> new_dimensions(
        activations_new->shape().dimensions().begin(),
        activations_new->shape().dimensions().end());
    const int64 reshaped_space_size =
        new_space_size * new_batch_size / old_batch_size;
    new_dimensions[c.spatial_dimension_to_split] = reshaped_space_size;
    new_dimensions[activations_batch_dim] = old_batch_size;

    // Reshape the output of the new conv into the old convolutions shape.
    TF_ASSIGN_OR_RETURN(HloInstruction * reshaped_activations,
                        MakeReshapeHlo(new_dimensions, activations_new));

    PaddingConfig padding_config =
        MakeNoPaddingConfig(reshaped_activations->shape().dimensions_size());
    padding_config.mutable_dimensions(c.spatial_dimension_to_split)
        ->set_edge_padding_high(spatial_split_size * new_batch_size -
                                reshaped_space_size);
    padding_config.mutable_dimensions(c.spatial_dimension_to_split)
        ->set_edge_padding_low(0);
    HloInstruction* padding =
        computation_->AddInstruction(HloInstruction::CreateConstant(
            LiteralUtil::Zero(reshaped_activations->shape().element_type())));

    TF_ASSIGN_OR_RETURN(
        reshaped_activations,
        MakePadHlo(reshaped_activations, padding, padding_config));

    std::vector<int64> reshape_back_dims(
        reshaped_activations->shape().dimensions().begin(),
        reshaped_activations->shape().dimensions().end());

    reshape_back_dims[c.spatial_dimension_to_split] = spatial_split_size;
    reshape_back_dims[activations_batch_dim] = new_batch_size;

    TF_ASSIGN_OR_RETURN(
        reshaped_activations,
        MakeReshapeHlo(reshape_back_dims, reshaped_activations));

    TF_ASSIGN_OR_RETURN(
        activations_new,
        HaloDuplicateWithSlice(
            reshaped_activations, c.spatial_dimension_to_split,
            activations_batch_dim, old_batch_size,
            /*low_padding=*/c.base_dilation_factor != 1 &&
                    c.inherent_low_padding != 0
                ? c.base_dilation_factor - 1
                : c.inherent_low_padding,
            c.inherent_high_padding, slice_size - spatial_split_size,
            old_split_dim_size));
  } else {
    // If the ideal spatial_split_size was smaller than the incoming spatial
    // dimension size, we don't need reshaping. Instead, we determine the
    // additional space available, and adjust the required slice size (and
    // thereby the halo size).
    if (spatial_split_size < new_space_size) {
      const int64 additional_space_present = spatial_split_size % c.stride;
      spatial_split_size = new_space_size;
      slice_size =
          spatial_split_size + std::max(c.kernel_spatial_dim_size - c.stride -
                                            additional_space_present,
                                        static_cast<int64>(0));
    }

    TF_ASSIGN_OR_RETURN(
        activations_new,
        HaloDuplicateWithSlice(activations_new, c.spatial_dimension_to_split,
                               activations_batch_dim, old_batch_size,
                               /*low_padding=*/c.base_dilation_factor != 1 &&
                                       c.inherent_low_padding != 0
                                   ? c.base_dilation_factor - 1
                                   : c.inherent_low_padding,
                               c.inherent_high_padding,
                               slice_size - spatial_split_size,
                               old_split_dim_size));
  }

  // We will generate output such that batch is followed by the split spatial
  // dimension.
  const int64 rank = (convolution->shape().rank());
  std::vector<int64> transpose_dims(rank);
  int dim_count = 0;
  std::map<int64, int64> dim_map;

  for (int j = 0;
       j < permuted_conv_dims_numbers.output_spatial_dimensions_size(); ++j) {
    if (j == get_chosen_spatial_dim(convolution)) {
      dim_map[permuted_conv_dims_numbers.output_batch_dimension()] = dim_count;
      new_dim_numbers.set_output_batch_dimension(dim_count++);
    }
    dim_map[permuted_conv_dims_numbers.output_spatial_dimensions(j)] =
        dim_count;
    new_dim_numbers.set_output_spatial_dimensions(j, dim_count);
    dim_count++;
  }

  dim_map[permuted_conv_dims_numbers.output_feature_dimension()] = dim_count;
  new_dim_numbers.set_output_feature_dimension(dim_count);

  int p = 0;
  for (const auto& entry : dim_map) {
    transpose_dims[p] = entry.second;
    p++;
  }

  auto new_window = convolution->window();
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_padding_high(c.high_padding_for_conv);
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_padding_low(c.low_padding_for_conv);
  TF_ASSIGN_OR_RETURN(
      HloInstruction * new_conv,
      MakeConvolveHlo(
          activations_new, /*rhs=*/convolution->mutable_operand(1),
          convolution->feature_group_count(), convolution->batch_group_count(),
          new_window, new_dim_numbers, convolution->precision_config(),
          /*preferred_element_type=*/convolution->shape().element_type()));
  convolution->SetupDerivedInstruction(new_conv);

  old_to_new_instrs_[convolution] = new_conv;
  VLOG(1) << "Space-to-batched convolution " << new_conv->ToString();

  instr_to_dim_map_[convolution] =
      std::make_pair(original_conv_dims.output_batch_dimension(),
                     original_conv_dims.output_spatial_dimensions(
                         get_chosen_spatial_dim(convolution)));

  instr_to_dim_permute_map_[new_conv] = std::vector<int64>(transpose_dims);

  convs_to_visit_.erase(convolution);
  return Status::OK();
}

Status ConvolutionVisitor::PropagateOnBackpropFilterConv(
    HloInstruction* convolution) {
  auto activations_old = convolution->mutable_operand(0);

  CHECK(old_to_new_instrs_.contains(activations_old));
  auto activations_new = old_to_new_instrs_[activations_old];
  const int64 new_spatial_dimension =
      activations_new->shape().dimensions_size();

  auto kernel_old = convolution->mutable_operand(1);

  CHECK(old_to_new_instrs_.contains(kernel_old));
  auto kernel_new = old_to_new_instrs_[kernel_old];

  auto permute_dims = instr_to_dim_permute_map_[activations_new];

  auto original_conv_dims = convolution->convolution_dimension_numbers();

  const int64 old_space_dim = original_conv_dims.input_spatial_dimensions(
      get_chosen_spatial_dim(convolution));
  const int64 old_split_dim_size =
      activations_old->shape().dimensions(old_space_dim);

  const int64 old_kernel_space_dim =
      original_conv_dims.kernel_spatial_dimensions(
          get_chosen_spatial_dim(convolution));
  const int64 old_kernel_split_dim_size =
      kernel_old->shape().dimensions(old_kernel_space_dim);

  auto permuted_conv_dims_numbers = original_conv_dims;

  // Note the inversion here : batch and feature are inverted in backprop
  // filters.
  int64 activations_batch_dim =
      DimLookUp(permute_dims, original_conv_dims.input_feature_dimension());
  int64 activations_feature_dim =
      DimLookUp(permute_dims, original_conv_dims.input_batch_dimension());

  const int64 previous_spatial_dim_count =
      original_conv_dims.input_spatial_dimensions_size();
  for (int64 i = 0; i < previous_spatial_dim_count; ++i) {
    permuted_conv_dims_numbers.set_input_spatial_dimensions(
        i, DimLookUp(permute_dims,
                     original_conv_dims.input_spatial_dimensions(i)));
    permuted_conv_dims_numbers.set_kernel_spatial_dimensions(
        i, DimLookUp(permute_dims,
                     original_conv_dims.kernel_spatial_dimensions(i)));
  }

  permuted_conv_dims_numbers.add_input_spatial_dimensions(
      new_spatial_dimension);
  permuted_conv_dims_numbers.add_kernel_spatial_dimensions(
      new_spatial_dimension);
  permuted_conv_dims_numbers.add_output_spatial_dimensions(
      new_spatial_dimension);

  // For the output, make the last dimension size 1.
  const int64 previous_chosen_spatial_dim_in_output =
      permuted_conv_dims_numbers.output_spatial_dimensions(
          get_chosen_spatial_dim(convolution));
  permuted_conv_dims_numbers.set_output_spatial_dimensions(
      get_chosen_spatial_dim(convolution), new_spatial_dimension);
  permuted_conv_dims_numbers.set_output_spatial_dimensions(
      previous_spatial_dim_count, previous_chosen_spatial_dim_in_output);

  const int64 kernel_input_feature_dim = DimLookUp(
      permute_dims, original_conv_dims.kernel_input_feature_dimension());

  const int64 kernel_output_feature_dim = DimLookUp(
      permute_dims, original_conv_dims.kernel_output_feature_dimension());

  permuted_conv_dims_numbers.set_kernel_input_feature_dimension(
      kernel_input_feature_dim);
  permuted_conv_dims_numbers.set_kernel_output_feature_dimension(
      kernel_output_feature_dim);

  int64 spatial_dimension_to_split =
      permuted_conv_dims_numbers.input_spatial_dimensions(
          get_chosen_spatial_dim(convolution));

  const int64 kernel_spatial_dimension_to_split =
      permuted_conv_dims_numbers.kernel_spatial_dimensions(
          get_chosen_spatial_dim(convolution));

  const int64 old_batch_dim = original_conv_dims.input_feature_dimension();
  const int64 old_batch_size =
      activations_old->shape().dimensions(old_batch_dim);

  const int64 new_split_dim_size =
      activations_new->shape().dimensions(spatial_dimension_to_split);

  permuted_conv_dims_numbers.set_input_batch_dimension(activations_feature_dim);
  permuted_conv_dims_numbers.set_input_feature_dimension(activations_batch_dim);

  VLOG(1) << "Propagating on conv activations_batch_dim "
          << activations_batch_dim << " spatial_dimension_to_split "
          << spatial_dimension_to_split << " old_batch_size " << old_batch_size;
  TF_ASSIGN_OR_RETURN(
      activations_new,
      BringSpaceNextToBatch(activations_new, permuted_conv_dims_numbers,
                            spatial_dimension_to_split, activations_batch_dim));

  // Need to set batch dimension again because BringSpaceNextToBatch changes it.
  permuted_conv_dims_numbers.set_input_batch_dimension(activations_feature_dim);

  auto select_val = computation_->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(activations_new->shape().element_type())));

  // Select activations correctly by masking additional space.
  TF_ASSIGN_OR_RETURN(
      activations_new,
      SelectValidPortion(activations_new, activations_old, select_val,
                         activations_batch_dim, spatial_dimension_to_split,
                         old_batch_dim, old_space_dim));

  // Select kernel correctly by masking additional space.
  TF_ASSIGN_OR_RETURN(
      kernel_new,
      SelectValidPortion(
          kernel_new, kernel_old, select_val,
          /*new_batch_dim=*/kernel_input_feature_dim,
          kernel_spatial_dimension_to_split,
          /*old_batch_dim=*/original_conv_dims.kernel_input_feature_dimension(),
          old_kernel_space_dim));

  // Create the new convolution dim numbers.
  auto new_dim_numbers = permuted_conv_dims_numbers;

  VLOG(2) << "New dim numbers " << new_dim_numbers.DebugString();

  const int64 inherent_low_padding =
      convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .padding_low();

  const int64 inherent_high_padding =
      convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .padding_high();

  const int64 rhs_dilation =
      convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .window_dilation();

  std::vector<HloInstruction*> activations_chunks;

  // Insert slices for low padding.
  for (int64 i = 0; i < inherent_low_padding; ++i) {
    HloInstruction* activations_to_use = nullptr;
    if (i == 0) {
      activations_to_use = activations_new;
    } else {
      activations_to_use = activations_chunks.back();
    }
    TF_ASSIGN_OR_RETURN(
        HloInstruction * activations_slice,
        HaloDuplicateWithSlice(activations_to_use, spatial_dimension_to_split,
                               activations_batch_dim, old_batch_size,
                               /*low_padding=*/1,
                               /*high_padding=*/0,
                               /*halo_size=*/0, old_split_dim_size));
    activations_chunks.push_back(activations_slice);
  }

  const int64 expanded_kernel =
      old_kernel_split_dim_size * rhs_dilation - (rhs_dilation - 1);
  const int64 overlap_count =
      old_split_dim_size - expanded_kernel + 1 +
      (inherent_low_padding < 0 ? inherent_low_padding : 0) +
      (inherent_high_padding < 0 ? inherent_high_padding : 0);
  VLOG(1) << "overlap_count " << overlap_count;

  // Insert original activations.
  for (int64 i = 0; i < overlap_count; ++i) {
    HloInstruction* activations_to_use = nullptr;
    HloInstruction* activations_slice = nullptr;
    if (i == 0) {
      activations_to_use = activations_new;
      if (inherent_low_padding < 0) {
        TF_ASSIGN_OR_RETURN(activations_slice,
                            HaloDuplicateWithSlice(
                                activations_to_use, spatial_dimension_to_split,
                                activations_batch_dim, old_batch_size,
                                /*low_padding=*/inherent_low_padding,
                                /*high_padding=*/0,
                                /*halo_size=*/0, old_split_dim_size));
      } else {
        activations_slice = activations_to_use;
      }
    } else {
      activations_to_use = activations_chunks.back();

      TF_ASSIGN_OR_RETURN(
          activations_slice,
          HaloDuplicateWithSlice(activations_to_use, spatial_dimension_to_split,
                                 activations_batch_dim, old_batch_size,
                                 /*low_padding=*/-1,
                                 /*high_padding=*/0,
                                 /*halo_size=*/0, old_split_dim_size));
    }

    activations_chunks.push_back(activations_slice);
  }

  // Insert slices for high padding.
  for (int64 i = 0; i < inherent_high_padding; ++i) {
    HloInstruction* activations_to_use = nullptr;
    activations_to_use = activations_chunks.back();

    TF_ASSIGN_OR_RETURN(
        HloInstruction * activations_slice,
        HaloDuplicateWithSlice(activations_to_use, spatial_dimension_to_split,
                               activations_batch_dim, old_batch_size,
                               /*low_padding=*/-1, /*high_padding=*/0,
                               /*halo_size=*/0, old_split_dim_size));
    activations_chunks.push_back(activations_slice);
  }

  for (int64 i = 0; i < activations_chunks.size(); ++i) {
    std::vector<int64> input_sizes(
        activations_chunks[i]->shape().dimensions().begin(),
        activations_chunks[i]->shape().dimensions().end());
    // Insert 1-sized dimension at the end
    input_sizes.push_back(1);
    TF_ASSIGN_OR_RETURN(activations_chunks[i],
                        MakeReshapeHlo(input_sizes, activations_chunks[i]));
  }

  TF_ASSIGN_OR_RETURN(
      activations_new,
      MakeConcatHlo(absl::MakeSpan(activations_chunks), new_spatial_dimension));

  // Reshape the kernel with additional spatial dim.
  std::vector<int64> kernel_sizes(kernel_new->shape().dimensions().begin(),
                                  kernel_new->shape().dimensions().end());
  // Insert 1-sized dimension at the end
  kernel_sizes.push_back(1);
  TF_ASSIGN_OR_RETURN(kernel_new, MakeReshapeHlo(kernel_sizes, kernel_new));

  auto new_window = convolution->window();
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_padding_high(-(rhs_dilation - 1));
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_padding_low(0);
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_size(new_split_dim_size / rhs_dilation);

  // Set the window for the additional spatial dim. This is a vanilla window.
  auto window_dim = new_window.add_dimensions();
  window_dim->set_base_dilation(1);
  window_dim->set_size(1);
  window_dim->set_stride(1);
  window_dim->set_padding_low(0);
  window_dim->set_padding_high(0);
  window_dim->set_window_reversal(false);
  window_dim->set_window_dilation(1);

  TF_ASSIGN_OR_RETURN(
      HloInstruction * new_conv,
      MakeConvolveHlo(
          activations_new, kernel_new, convolution->feature_group_count(),
          convolution->batch_group_count(), new_window, new_dim_numbers,
          convolution->precision_config(),
          /*preferred_element_type=*/convolution->shape().element_type()));
  convolution->SetupDerivedInstruction(new_conv);

  std::vector<int64> output_sizes(new_conv->shape().dimensions().begin(),
                                  new_conv->shape().dimensions().end());

  output_sizes.erase(output_sizes.begin() +
                     new_dim_numbers.output_spatial_dimensions(
                         get_chosen_spatial_dim(convolution)));

  TF_ASSIGN_OR_RETURN(new_conv, MakeReshapeHlo(output_sizes, new_conv));

  old_to_new_instrs_[convolution] = new_conv;
  VLOG(1) << "Space-to-featured convolution " << new_conv->ToString();

  instr_to_dim_map_[convolution] =
      std::make_pair(original_conv_dims.output_batch_dimension(),
                     original_conv_dims.output_spatial_dimensions(
                         get_chosen_spatial_dim(convolution)));

  // We do not set instr_to_dim_permute_map_ here because no further
  // propagation is needed here.

  return Status::OK();
}

HloInstruction* ConvolutionVisitor::DoesConvolutionFeedReduceWindow(
    HloInstruction* instr, int64 depth = kReduceWindowSearchDepth) {
  if (depth == 0) {
    return nullptr;
  }

  for (auto user : instr->users()) {
    if (user->opcode() == HloOpcode::kReduceWindow) {
      return user;
    }
    // Stop the search if these ops are encountered.
    if (user->opcode() == HloOpcode::kConvolution ||
        user->opcode() == HloOpcode::kPad ||
        user->opcode() == HloOpcode::kTranspose) {
      continue;
    }
    auto ret = DoesConvolutionFeedReduceWindow(user, depth - 1);
    if (ret != nullptr) {
      return ret;
    }
  }
  return nullptr;
}

ConvolutionVisitor::ConvDetails ConvolutionVisitor::GetConvolutionDetails(
    HloInstruction* convolution, ConvolutionDimensionNumbers& dim_numbers) {
  auto activations = convolution->mutable_operand(0);

  auto kernel = convolution->mutable_operand(1);
  const auto& kernel_shape = kernel->shape();
  const int64 kernel_spatial_dim_size =
      kernel_shape.dimensions(dim_numbers.kernel_spatial_dimensions(
          get_chosen_spatial_dim(convolution)));

  const int64 spatial_dimension_to_split =
      dim_numbers.input_spatial_dimensions(get_chosen_spatial_dim(convolution));

  const int64 input_dim_size =
      activations->shape().dimensions(spatial_dimension_to_split);

  const int64 inherent_low_padding =
      convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .padding_low();
  const int64 inherent_high_padding =
      convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .padding_high();

  const int64 stride = convolution->window()
                           .dimensions(get_chosen_spatial_dim(convolution))
                           .stride();

  const int64 base_dilation_factor =
      convolution->window()
          .dimensions(get_chosen_spatial_dim(convolution))
          .base_dilation();

  const int64 spatial_size =
      input_dim_size + (base_dilation_factor > 1 ? 0 : inherent_low_padding) +
      inherent_high_padding;

  const int64 halo_size =
      std::max(kernel_spatial_dim_size - stride - (base_dilation_factor - 1),
               static_cast<int64>(0));
  const int64 high_padding_for_conv = base_dilation_factor == 1 ? 0
                                      : inherent_low_padding == 0
                                          ? base_dilation_factor - 1
                                          : 0;
  const int64 low_padding_for_conv =
      base_dilation_factor == 1 ? 0 : inherent_low_padding;

  return ConvDetails{spatial_dimension_to_split,
                     inherent_low_padding,
                     inherent_high_padding,
                     stride,
                     spatial_size,
                     base_dilation_factor,
                     halo_size,
                     high_padding_for_conv,
                     low_padding_for_conv,
                     kernel_spatial_dim_size,
                     input_dim_size};
}

Status ConvolutionVisitor::PerformSpaceToBatchOnConvolution(
    HloInstruction* convolution) {
  VLOG(1) << "Handling conv " << convolution->ToString();

  changed_ = false;

  ConvolutionDimensionNumbers dim_numbers =
      convolution->convolution_dimension_numbers();

  ConvDetails c = GetConvolutionDetails(convolution, dim_numbers);

  int64 activations_batch_dim = dim_numbers.input_batch_dimension();

  const int64 old_batch_size =
      convolution->operand(0)->shape().dimensions(activations_batch_dim);

  auto activations = convolution->mutable_operand(0);

  const bool inherent_padding_needed =
      c.inherent_low_padding != 0 || c.inherent_high_padding != 0;

  VLOG(1) << "spatial size " << c.spatial_size;

  const int64 num_splits = kNewBatchSize / old_batch_size;
  auto original_conv = convolution;

  // We'd need transposition of activations here such that batch and space dim
  // that is being split are adjacent (in that order).
  TF_ASSIGN_OR_RETURN(activations,
                      BringSpaceNextToBatch(activations, dim_numbers,
                                            c.spatial_dimension_to_split,
                                            activations_batch_dim));
  // Create the new convolution dim numbers.
  auto new_dim_numbers = dim_numbers;

  const int64 output_spatial_dim = dim_numbers.output_spatial_dimensions(
      get_chosen_spatial_dim(convolution));
  const int64 output_offsets =
      convolution->shape().dimensions(output_spatial_dim);
  const int64 output_offsets_per_split =
      CeilOfRatio(output_offsets, num_splits);

  int64 spatial_split_size =
      CeilOfRatio(output_offsets_per_split, c.base_dilation_factor) * c.stride;
  // Keep increasing the split size so that overall size isn't smaller than the
  // original spatial dimension.
  while (spatial_split_size * num_splits - c.spatial_size < 0) {
    spatial_split_size += c.stride;
  }

  auto reduce_window = DoesConvolutionFeedReduceWindow(convolution);

  if (reduce_window != nullptr) {
    VLOG(2) << "DoesConvolutionFeedReduceWindow " << reduce_window;
    // Take into account the stride of the reduce window while choosing the
    // spatial_split_size. This will guarantee propagation through reduce
    // windows.
    const int64 red_win_stride =
        reduce_window->window().dimensions(output_spatial_dim).stride();
    while ((spatial_split_size / c.stride) % red_win_stride != 0) {
      spatial_split_size += c.stride;
    }
  }

  const int64 slice_size = spatial_split_size + c.halo_size;

  // Pad spatial dim.
  const int64 pad_size = spatial_split_size * num_splits - c.spatial_size;

  VLOG(1) << "spatial_split_size " << spatial_split_size << " stride "
          << c.stride << " slice_size " << slice_size;
  VLOG(1) << "spatial_dimension_to_split " << c.spatial_dimension_to_split
          << " num_splits " << num_splits << " kernel_spatial_dim_size "
          << c.kernel_spatial_dim_size;

  // Because we are splitting the spatial dimension, if convolution needed
  // padding in the spatial dimension, we materialize it.
  if (pad_size != 0 || inherent_padding_needed) {
    PaddingConfig padding_config =
        MakeNoPaddingConfig(activations->shape().dimensions_size());
    padding_config.mutable_dimensions(c.spatial_dimension_to_split)
        ->set_edge_padding_high(c.inherent_high_padding + pad_size);
    padding_config.mutable_dimensions(c.spatial_dimension_to_split)
        ->set_edge_padding_low(
            c.base_dilation_factor == 1 ? c.inherent_low_padding : 0);
    HloInstruction* padding =
        computation_->AddInstruction(HloInstruction::CreateConstant(
            LiteralUtil::Zero(activations->shape().element_type())));
    TF_ASSIGN_OR_RETURN(activations,
                        MakePadHlo(activations, padding, padding_config));
  }
  VLOG(1) << "Initial padded activations shape "
          << activations->shape().ToString();

  // Now we reorganize the activations. E.g. if the shape [B, SPACE] was [1, 16]
  // and 4 splits were needed, we first create [4, 4]. Next, to deal with halo
  // in the spatial dimension, we generate a gather. E.g. if halo size was 2,
  // we'd create a shape of [24] using the gather, and reshape it into [6, 4]
  // (4 being the batch).

  // The benefit of the above mentioned scheme is that it allows for batch
  // growth. Here are some examples of the size increases it causes for a 3x3
  // kernel.
  // with batch=1, [1,16] -> [4,4] ->   [4,6] ->   [1,24] growth of 8.
  // with batch=2, [2,16] -> [8,4] ->   [8,6] ->   [1,48] growth of 16.
  // with batch=3, [3,16] -> [12,4] -> [12,6] -> [1,72] growth of 24.

  std::vector<int64> reshape_dimensions(
      activations->shape().dimensions().begin(),
      activations->shape().dimensions().end());

  reshape_dimensions[c.spatial_dimension_to_split] = spatial_split_size;
  reshape_dimensions[activations_batch_dim] = num_splits * old_batch_size;

  TF_ASSIGN_OR_RETURN(HloInstruction * batch_increased_reshape,
                      MakeReshapeHlo(reshape_dimensions, activations));
  convolution->SetupDerivedInstruction(batch_increased_reshape);

  VLOG(1) << "First reshape done " << batch_increased_reshape->ToString();

  TF_ASSIGN_OR_RETURN(
      activations, HaloDuplicateWithSlice(batch_increased_reshape,
                                          c.spatial_dimension_to_split,
                                          activations_batch_dim, old_batch_size,
                                          /*low_padding=*/0, /*high_padding=*/0,
                                          c.halo_size, c.input_dim_size));

  VLOG(1) << "Batch merge done " << activations->ToString();

  // Now, we rewrite the convolution with a larger batch.

  // We will generate output such that batch is followed by the split spatial
  // dimension.
  const int64 rank = convolution->shape().rank();
  std::vector<int64> transpose_dims(rank);
  int dim_count = 0;
  std::map<int64, int64> dim_map;

  for (int j = 0; j < dim_numbers.output_spatial_dimensions_size(); ++j) {
    if (j == get_chosen_spatial_dim(convolution)) {
      dim_map[dim_numbers.output_batch_dimension()] = dim_count;
      new_dim_numbers.set_output_batch_dimension(dim_count++);
    }
    dim_map[dim_numbers.output_spatial_dimensions(j)] = dim_count;
    new_dim_numbers.set_output_spatial_dimensions(j, dim_count);
    dim_count++;
  }

  dim_map[dim_numbers.output_feature_dimension()] = dim_count;
  new_dim_numbers.set_output_feature_dimension(dim_count);

  int p = 0;
  for (const auto& entry : dim_map) {
    transpose_dims[p] = entry.second;
    p++;
  }
  VLOG(1) << "New dim numbers " << new_dim_numbers.DebugString()
          << " batch dim " << new_dim_numbers.input_batch_dimension();
  auto new_window = convolution->window();
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_padding_high(c.high_padding_for_conv);
  new_window.mutable_dimensions(get_chosen_spatial_dim(convolution))
      ->set_padding_low(c.low_padding_for_conv);
  TF_ASSIGN_OR_RETURN(
      HloInstruction * new_conv,
      MakeConvolveHlo(
          activations, /*rhs=*/convolution->mutable_operand(1),
          convolution->feature_group_count(), convolution->batch_group_count(),
          new_window, new_dim_numbers, convolution->precision_config(),
          /*preferred_element_type=*/convolution->shape().element_type()));
  convolution->SetupDerivedInstruction(new_conv);

  VLOG(1) << "Space-to-batched convolution " << new_conv->ToString();

  const int64 output_split_spatial_dim =
      new_dim_numbers.output_spatial_dimensions(
          get_chosen_spatial_dim(convolution));
  const int64 output_batch_dim = new_dim_numbers.output_batch_dimension();
  VLOG(1) << "output_batch_dim " << output_batch_dim
          << " output_split_spatial_dim " << output_split_spatial_dim;

  auto select_val = computation_->AddInstruction(HloInstruction::CreateConstant(
      LiteralUtil::Zero(new_conv->shape().element_type())));

  TF_ASSIGN_OR_RETURN(
      new_conv, SelectValidPortion(new_conv, original_conv, select_val,
                                   output_batch_dim, output_split_spatial_dim,
                                   dim_numbers.output_batch_dimension(),
                                   dim_numbers.output_spatial_dimensions(
                                       get_chosen_spatial_dim(original_conv))));
  old_to_new_instrs_[original_conv] = new_conv;

  instr_to_dim_map_[original_conv] =
      std::make_pair(dim_numbers.output_batch_dimension(),
                     dim_numbers.output_spatial_dimensions(
                         get_chosen_spatial_dim(original_conv)));

  instr_to_dim_permute_map_[new_conv] = std::vector<int64>(transpose_dims);

  TF_CHECK_OK(PropagateOnUsers(original_conv));

  changed_ = true;

  return Status::OK();
}

}  // namespace

StatusOr<bool> ConvolutionSpaceToBatchConverter::Run(HloModule* module) {
  XLA_VLOG_LINES(2, "ConvolutionSpaceToBatchConverter::Run(), before:\n" +
                        module->ToString());
  bool changed = false;

  for (auto* comp : module->MakeNonfusionComputations()) {
    ConvolutionVisitor visitor(limit_on_batch_size_, comp);
    if (visitor.Run().ValueOrDie()) {
      changed = true;
    }
    VLOG(1) << "Done operating on computation";
  }
  XLA_VLOG_LINES(2, "ConvolutionSpaceToBatchConverter::Run(), after:\n" +
                        module->ToString());
  return changed;
}

}  // namespace xla
