/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2020 Aksel Alpay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cassert>

#include "hipSYCL/runtime/dag_submitted_ops.hpp"
#include "hipSYCL/runtime/dag_node.hpp"
#include "hipSYCL/runtime/hints.hpp"

namespace hipsycl {
namespace rt {

namespace {

void erase_completed_nodes(std::vector<dag_node_ptr> &ops) {
  ops.erase(std::remove_if(
                ops.begin(), ops.end(),
                [&](dag_node_ptr node) -> bool { return node->is_complete(); }),
            ops.end());
}
}


void dag_submitted_ops::update_with_submission(dag_node_ptr single_node) {
  std::lock_guard lock{_lock};

  erase_completed_nodes(_ops);

  assert(single_node->is_submitted());
  _ops.push_back(single_node);
}

void dag_submitted_ops::wait_for_all() {
  std::vector<dag_node_ptr> current_ops;
  {
    std::lock_guard lock{_lock};
    current_ops = _ops;
  }
  
  for(dag_node_ptr node : current_ops) {
    assert(node->is_submitted());
    node->wait();
  }
}

void dag_submitted_ops::wait_for_group(std::size_t node_group) {
  HIPSYCL_DEBUG_INFO << "dag_submitted_ops: Waiting for node group "
                     << node_group << std::endl;
  
  std::vector<dag_node_ptr> current_ops;
  {
    std::lock_guard lock{_lock};  
    current_ops = _ops;
  }

  // TODO We can optimize this process by
  // 1.) In dag_node::wait(), when the event turns complete the first time,
  // recursively mark all requirements as complete as well.
  // 2.) Reverse the iteration order here - this will cause us to handle the
  // newest nodes first, which usually will depend on older nodes.
  // Since nodes cache their state when they complete and because of 1), 
  // the wait() on most of the older nodes will become trivial and not 
  // require any backend interaction at all.
  for(dag_node_ptr node : current_ops) {
    assert(node->is_submitted());
    if (hints::node_group *g =
            node->get_execution_hints().get_hint<hints::node_group>()) {
      if (g->get_id() == node_group) {
        HIPSYCL_DEBUG_INFO
            << "dag_submitted_ops: Waiting for node group; current node: "
            << node.get() << std::endl;
        node->wait();
      }
    }
  }
}

std::vector<dag_node_ptr> dag_submitted_ops::get_group(std::size_t node_group) {
  
  std::vector<dag_node_ptr> ops;
  {
    std::lock_guard lock{_lock};
    for(dag_node_ptr node : _ops) {
      assert(node->is_submitted());
      if (hints::node_group *g =
              node->get_execution_hints().get_hint<hints::node_group>()) {
        if (g->get_id() == node_group) {
          ops.push_back(node);
        }
      }
    }
  }
  return ops;
}

}
}
