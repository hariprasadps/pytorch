#include "torch/csrc/jit/passes/onnx/peephole.h"

#include <ATen/optional.h>

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

namespace torch { namespace jit {

bool isRNN(const Node *node) {
  auto k = node->kind();
  return k == onnx::RNN || k == onnx::LSTM || k == onnx::GRU;
}

bool isNopTranspose(const std::vector<int64_t> & perm) {
  for (int64_t i = 0, perm_size = perm.size(); i < perm_size; i++)
    if (perm[i] != i)
      return false;
  return true;
}

// returns a vector `ret` such that transposing by `ret` is equivalent
// to transposing by `t1` and then by `t2`
std::vector<int64_t> composeTransposes(const std::vector<int64_t> & t1,
                                       const std::vector<int64_t> & t2) {
  JIT_ASSERT(t1.size() == t2.size());
  std::vector<int64_t> ret;
  for (size_t i = 0; i < t1.size(); i++) {
    JIT_ASSERT(   t1[i]  < int64_t(t2.size()));
    JIT_ASSERT(t2[t1[i]] < int64_t(t2.size()));
    ret.push_back(t2[t1[i]]);
  }
  return ret;
}

bool isBroadcasting(Node* node) {
  // Broadcasting operators have the following property:
  // They support a 'broadcast' flag, which enables broadcasting
  // on the last argument.  ATM this is not full-Numpy broadcasting,
  // only left-size extension (no size 1 to size n broadcast)
  static std::unordered_set<NodeKind> broadcasting = {
    onnx::Add,
    onnx::Div,
    onnx::Mul,
    onnx::Pow,
    onnx::Sub,
    onnx::Gemm,
  };

  return broadcasting.count(node->kind());
}

// First iterate over the 'from' tensor sizes. Ignore all leading and trailing
// dimensions that are simply one, since they can be trivially broadcasted.
// When iterating over the dimension sizes (with reduced 'from' tensor),
// starting at the trailing dimension, the dimension sizes must either be equal,
// or one of them does not exist. If a broadcast candidate is not found at the
// trailing dimension, search at the leading dimension. If one is found here,
// return the `axis` argument to be emitted to ONNX on the broadcasting operator
//
// Note that this is NOT equivalent to numpy broadcasting semantics, and do
// not represent the generalized broadcasting that Pytorch implements.
// Rather, this is Caffe2-style broadcasting.
//
// Return value is 1) Whether this expand is fusable, 2) the `axis` argument we
// should emit to ONNX. Coming from a Pytorch frontend, this should either not
// be emitted (if we're broadcasting trailing dimensions) or it should be
// emitted as `0` (leading dimensions.)
std::tuple<bool, at::optional<size_t>> fusibleExpandTo(at::IntList from, at::IntList to) {
  if (from.size() > to.size()) {
    return std::make_tuple(false, at::nullopt);
  }
  ssize_t from_dim_start = 0, from_dim_end = from.size() - 1;
  while (from_dim_start < (ssize_t) from.size() && from[from_dim_start] == 1) {
    from_dim_start++;
  }
  while (from_dim_end > from_dim_start && from[from_dim_end] == 1) {
    from_dim_end--;
  }

  ssize_t f = from_dim_end;
  ssize_t t = to.size() - 1;
  bool trailing_expand = true;
  for (; f >= from_dim_start && t >= 0; --f, --t) {
    if (from[f] != to[t]) {
      trailing_expand = false;
      break;
    }
  }

  // In the case that the 'to' tensor has leading ones in the same place that
  // the 'from' tensor does, f will be less than from_dim_start rather than
  // strictly equal. E.x.: to := [5, 1, 768] and from := [1, 1, 768]
  if (trailing_expand && f <= from_dim_start) {
    return std::make_tuple(true, at::nullopt);
  }

  f = from_dim_start;
  t = 0;
  bool leading_expand = true;
  for (; f <= from_dim_end && t < static_cast<ssize_t>(to.size()); ++f, ++t) {
    if (from[f] != to[t]) {
      leading_expand = false;
      break;
    }
  }

  if (leading_expand && f >= from_dim_end) {
    return std::make_tuple(true, 0);
  }

  return std::make_tuple(false, at::nullopt);
}

void fuseBroadcast(std::shared_ptr<Graph>& graph) {
  for(auto n : graph->nodes()) {

    // Can't fuse into nodes that don't support broadcasting
    if (!isBroadcasting(n)) continue;

    // If the node already broadcasts, can't "rebroadcast"
    // TODO: Actually, maybe you can, if there is a broadcast for some
    // dims, and then another broadcast for the rest.  But this will
    // never happen in practice so I didn't implement it.
    if (n->hasAttribute(attr::broadcast) && n->i(attr::broadcast)) continue;
    JIT_ASSERT(!n->hasAttribute(attr::axis));

    auto input_index = n->inputs().size() - 1;
    auto* expanded_rhs = n->input(input_index)->node();

    // The expanded_rhs input isn't actually an expand, so no fusion available
    if (expanded_rhs->kind() != aten::expand) continue;

    auto* unexpanded_rhs = expanded_rhs->input();

    // We need to know what the type pre-expand is.  We should basically
    // always have this information (because expands are only ever traced,
    // not generated from symbolic), but if for some reason we don't
    // have it, we need to skip.
    if (!unexpanded_rhs->isTensor()) continue;

    // Not all broadcasts are supported by ONNX broadcast.
    bool fusible_expand;
    at::optional<size_t> axis;
    std::tie(fusible_expand, axis) = fusibleExpandTo(
        unexpanded_rhs->type()->expect<TensorType>()->sizes(), // from
        expanded_rhs->output()->type()->expect<TensorType>()->sizes()); // to
    if (!fusible_expand)
      continue;

    n->replaceInput(input_index, unexpanded_rhs);
    n->i_(attr::broadcast, 1);
    if (axis) {
      n->i_(attr::axis, axis.value());
    }
    if (!expanded_rhs->hasUses()) {
      expanded_rhs->destroy();
    }
  }
}

void fuseConsecutiveTransposes(std::shared_ptr<Graph>& graph) {
  for(auto n : graph->nodes()) {

    if (n->kind() == onnx::Transpose && n->input()->node()->kind() == onnx::Transpose) {
      auto origInput = n->input();
      n->is_(attr::perm, composeTransposes(origInput->node()->is(attr::perm), n->is(attr::perm)));
      n->replaceInput(0, origInput->node()->input());
      if (origInput->uses().size() == 0) {
        origInput->node()->destroy();
      }
      continue;
    }
  }
}

void eliminateNopTranspose(std::shared_ptr<Graph>& graph) {
  for(auto it = graph->nodes().begin(), end = graph->nodes().end(); it != end; ++it) {
    auto n = *it;
    if (n->kind() == onnx::Transpose) {
      if (isNopTranspose(n->is(attr::perm))) {
        n->replaceAllUsesWith(n->input()->node());
        it.destroyCurrent();
        continue;
      }
    }
  }
}

void fuseTransposeIntoGemm(std::shared_ptr<Graph>& graph) {
  static const std::vector<int64_t> simpleTransPerm({1,0});

  for(auto n : graph->nodes()) {

    if (n->kind() == onnx::Gemm) {
      for (size_t i : {0,1}) {
        auto inp = n->inputs()[i];
        auto trans = i == 0 ? attr::transA : attr::transB;
        if (inp->node()->kind() == onnx::Transpose && inp->node()->is(attr::perm) == simpleTransPerm) {
          n->replaceInput(i, inp->node()->input());
          n->i_(trans, n->hasAttribute(trans) ? !n->i(trans) : 1);
          if (inp->uses().size() == 0) {
            inp->node()->destroy();
          }
        }
      }
    }
  }
}

// Why this is here:
//
//   Pytorch has a "packed" representation of sequences, as well as a
//   "padded" representation. ONNX has only one representation,
//   corresponding to pytorch's "padded". Therefore, we need to remove
//   any use of packed sequences before exporting.
//
// What this does:
//
//   This code uses the observation that
//     RNN(PackPadded(x)) == PackPadded(RNN(x))
//   and converts the first form to the second whenever possible,
//   "pushing" the packing operation past the RNN operation. Then,
//   the removeNopPacking pass removes the packing operations
//   entirely by pairing them with their inverse PadPacked. If the
//   input graph does not pair the operations, export will fail.
void pushPackingPastRnn(std::shared_ptr<Graph>& graph) {
  for (auto it = graph->nodes().begin(); it != graph->nodes().end(); ++it) {
    auto* n = *it;

    if (n->kind() != prim::PackPadded) {
      continue;
    }
    if (n->outputs()[0]->uses().size() != 1) {
      // For now, only handle the case where there is one consumer.
      continue;
    }
    Node * rnn = n->outputs()[0]->uses()[0].user;
    if (!isRNN(rnn)) {
      continue;
    }

    // remove PackPadded from in front of the RNN
    n->outputs()[0]->replaceAllUsesWith(n->inputs()[0]);

    // note there can be multiple uses of the length blob. If we are
    // translating a multi-level RNN it will be an input to each level.
    n->outputs()[1]->replaceFirstUseWith(n->inputs()[1]);

    // and insert new PackPadded after the RNN
    Node * newPackPadded = graph->create(prim::PackPadded, 2);
    newPackPadded->insertAfter(rnn);

    // make things consume from the new PackPadded
    rnn->outputs()[0]->replaceAllUsesWith(newPackPadded->outputs()[0]);
    n->outputs()[1]->replaceAllUsesWith(newPackPadded->outputs()[1]);

    // setup the new PackPadded's inputs
    newPackPadded->addInput(rnn->outputs()[0]);
    newPackPadded->addInput(n->inputs()[1]);

    it.destroyCurrent();
  }
}

void removeNopPacking(std::shared_ptr<Graph>& graph) {
  for (auto it = graph->nodes().begin(); it != graph->nodes().end(); ++it) {
    auto* n = *it;

    if (n->kind() != prim::PadPacked) {
      continue;
    }
    Node* input = n->inputs()[0]->node();
    if (input->kind() != prim::PackPadded) {
      continue;
    }
    if (input->outputs()[0] != n->inputs()[0]) {
      continue;
    }
    if (input->outputs()[1] != n->inputs()[1]) {
      continue;
    }
    n->outputs()[0]->replaceAllUsesWith(input->inputs()[0]);
    n->outputs()[1]->replaceAllUsesWith(input->inputs()[1]);

    n->removeAllInputs();
    it.destroyCurrent();
  }
}

void fixDefaultRNNState(std::shared_ptr<Graph>& graph, Node * n, int input_index) {
  auto initial_state = n->inputs()[input_index];

  // The RNN code in pytorch accepts an optional hidden state. When it
  // is provided, everything works great. When it is not provided, it
  // is default-initialized by constructing a new Variable, which gets
  // traced as a Constant. Recognize that pattern here and replace it
  // with something that doesn't fix the batch size.  Note that for
  // multi-layer RNNs there will be a Slice operation between the
  // Constant and the RNN.
  bool needsFixing =
    initial_state->node()->kind() == onnx::Constant ||
    (initial_state->node()->kind() == onnx::Slice &&
     initial_state->node()->inputs()[0]->node()->kind() == onnx::Constant);

  if (!needsFixing) {
    return;
  }

  Node * shape_of_input = graph->create(onnx::Shape, 1);
  shape_of_input->insertBefore(n);
  shape_of_input->addInput(n->inputs()[0]);

  Node * gather_indices = graph->create(onnx::Constant, 1);
  gather_indices->insertBefore(n);
  gather_indices->t_(attr::value, at::Scalar(1).toTensor());

  Node * batch_size = graph->create(onnx::Gather, 1);
  batch_size->insertBefore(n);
  batch_size->addInput(shape_of_input->outputs()[0]);
  batch_size->addInput(gather_indices->outputs()[0]);

  Node * unsqueezed_batch_size = graph->create(onnx::Unsqueeze, 1);
  unsqueezed_batch_size->insertBefore(n);
  unsqueezed_batch_size->addInput(batch_size->outputs()[0]);
  unsqueezed_batch_size->is_(attr::axes, {0});

  Node * hidden_size = graph->create(onnx::Constant, 1);
  hidden_size->insertBefore(n);
  hidden_size->t_(attr::value, at::CPU(at::kLong).tensor({1}).fill_(n->i(attr::hidden_size))); // at::Scalar(n->i(attr::hidden_size)).toTensor());

  Node * num_directions = graph->create(onnx::Constant, 1);
  num_directions->insertBefore(n);
  num_directions->t_(attr::value, at::Scalar(n->hasAttribute(attr::direction) && n->s(attr::direction) == "bidirectional" ? 2 : 1).toTensor());

  Node * unsqueezed_num_directions = graph->create(onnx::Unsqueeze, 1);
  unsqueezed_num_directions->insertBefore(n);
  unsqueezed_num_directions->addInput(num_directions->outputs()[0]);
  unsqueezed_num_directions->is_(attr::axes, {0});

  Node * concated_dims = graph->create(onnx::Concat, 1);
  concated_dims->insertBefore(n);
  concated_dims->i_(attr::axis, 0);
  concated_dims->addInput(unsqueezed_num_directions->outputs()[0]);
  concated_dims->addInput(unsqueezed_batch_size->outputs()[0]);
  concated_dims->addInput(hidden_size->outputs()[0]);

  Node * constant_fill = graph->create(onnx::ConstantFill, 1);
  constant_fill->insertBefore(n);
  constant_fill->i_(attr::input_as_shape, 1);
  constant_fill->addInput(concated_dims->outputs()[0]);

  n->replaceInput(input_index, constant_fill->outputs()[0]);
  if (initial_state->uses().size() == 0) {
    initial_state->node()->destroy();
  }
}

void fixDefaultRnnHiddenState(std::shared_ptr<Graph>& graph) {
  for (auto it = graph->nodes().begin(); it != graph->nodes().end(); ++it) {
    auto* n = *it;

    if (!isRNN(n)) {
      continue;
    }
    // Hidden state is the sixth input for RNN, LSTM, GRU.
    // See http://pytorch.org/docs/master/nn.html#torch.nn.RNN
    if (n->inputs().size() < 6) {
      continue;
    }
    fixDefaultRNNState(graph, n, 5);
  }
}

void fixDefaultLstmCellState(std::shared_ptr<Graph>& graph) {
  for (auto it = graph->nodes().begin(); it != graph->nodes().end(); ++it) {
    auto* n = *it;

    if (n->kind() != onnx::LSTM) {
      continue;
    }
    // Cell state is the seventh input for LSTM.
    // See http://pytorch.org/docs/master/nn.html#torch.nn.LSTM
    if (n->inputs().size() < 7) {
      continue;
    }
    fixDefaultRNNState(graph, n, 6);
  }
}

// This optimization does ONNX-specific peephole optimizations.
//
// At the moment, here are the optimizations it does:
//  - This optimization fuses expand calls into ONNX operators, because it is
//    easier for non-strided backends to more efficiently do broadcasts if this is
//    local information.  This optimization is not useful for PyTorch as 'expand'
//    is free.
//  - Fusing of consecutive transposes
//  - Elimination of NOP transposes
//  - Fusing of transposes into Gemm
//  - Elimination of PaddedSequences
//
// Before you write an optimization here, ask yourself, "Could I do this
// optimization on ATen operators"?  If so, you should seriously consider
// writing your optimization in jit/passes/peephole.cpp rather than
// here, as it will be generally applicable to the JIT as well.  The
// optimizations here are ONLY applied on ONNX update
void PeepholeOptimizeONNX(std::shared_ptr<Graph>& graph) {
  // TODO: decide on fixpoint strategy
  // TODO: make it easier not to do O(k) iterations over the graph, where
  // k is the number of distinct peephole optimizations
  pushPackingPastRnn(graph);
  removeNopPacking(graph);
  fixDefaultRnnHiddenState(graph);
  fixDefaultLstmCellState(graph);
  fuseBroadcast(graph);
  fuseConsecutiveTransposes(graph);
  eliminateNopTranspose(graph);
  fuseTransposeIntoGemm(graph);
}

}}
