/*******************************************************************************
 * Copyright 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#include "ngraph_builder.h"
#include "ngraph_utils.h"

#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb_text.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/edgeset.h"

namespace ngraph_bridge {
unique_ptr<ng::Function> Builder::TranslateGraph(
    const std::vector<tf::TensorShape>& inputs, const tf::Graph* input_graph) {
  // Do a topological sort
  // GetReversePostOrder will give us topological sort.
  vector<tf::Node*> ordered;
  tf::GetReversePostOrder(*input_graph, &ordered);

  // Now start building the nGraph.
  vector<tf::Node*> tf_params;
  vector<tf::Node*> tf_consts;
  vector<tf::Node*> tf_ret_vals;
  vector<tf::Node*> tf_ops;

  for (auto n : ordered) {
    if (n->IsSink() || n->IsSource() || n->IsControlFlow()) {
      continue;
    }

    if (n->IsVariable()) {
      std::cout << "Variable Node: " << n->type_string() << endl;
    }
    if (n->type_string() == "_Arg") {
      tf_params.push_back(n);
    } else if (n->type_string() == "_Retval") {
      tf_ret_vals.push_back(n);
    } else {
      tf_ops.push_back(n);
    }
  }

  unordered_map<string, shared_ptr<ng::Node>> ng_op_map;
  vector<shared_ptr<ng::op::Parameter>> ng_parameter_list;
  cout << "Parameters\n";
  for (auto parm : tf_params) {
    tf::DataType dtype;
    if (tf::GetNodeAttr(parm->attrs(), "T", &dtype) != tf::Status::OK()) {
      cout << "Error: No data type defined for _Arg!\n";
      return nullptr;
    }
    int index;
    if (tf::GetNodeAttr(parm->attrs(), "index", &index) != tf::Status::OK()) {
      cout << "Error: No index defined for _Arg!\n";
      return nullptr;
    }

    const tf::TensorShape& tf_shape = inputs[index];
    cout << "Param: " << index << " Name: " << parm->name()
         << " Rank: " << tf_shape.dims() << " Shape: " << tf_shape << endl;
    // TODO: Convert tf::Type to ng::type
    ng::Shape ng_shape(tf_shape.dims());
    for (int i = 0; i < tf_shape.dims(); ++i) {
      ng_shape[i] = tf_shape.dim_size(i);
    }

    auto ng_param = make_shared<ng::op::Parameter>(ng::element::f32, ng_shape);
    ng_op_map[parm->name()] = ng_param;
    ng_parameter_list.push_back(ng_param);
  }

  cout << "Ops\n";
  for (auto op : tf_ops) {
    cout << "Op: " << op->name() << "(" << op->type_string() << ")" << endl;
    for (const tf::Edge* edge : op->in_edges()) {
      cout << "\tSrc: " << edge->src()->name() << "("
           << edge->src()->type_string() << ")" << endl;
    }

    // Now create the ng ops from tf ops
    if (op->type_string() == "Const") {
      // Data type first
      tf::DataType dtype;
      if (tf::GetNodeAttr(op->attrs(), "dtype", &dtype) != tf::Status::OK()) {
        cout << "Error: No data type defined for Constant!\n";
        return nullptr;
      }

      // TODO: Create a type based on the DataType
      tf::TensorShapeProto shape_proto;
      vector<float> const_values;
      if (!ValuesFromConstNode<float>(op->def(), &shape_proto, &const_values)) {
        cout << "Error: Cannot get values from Constant!" << endl;
      }

      cout << "Constant: " << endl;
      for (auto val : const_values) {
        cout << val << " ";
      }
      cout << endl;

      tf::TensorShape const_shape(shape_proto);
      ng::Shape ng_shape(const_shape.dims());
      for (int i = 0; i < const_shape.dims(); ++i) {
        ng_shape[i] = const_shape.dim_size(i);
      }

      // vector<float> float_t(ng::shape_size(ng_shape), 0);
      auto ng_node = make_shared<ng::op::Constant>(ng::element::f32, ng_shape,
                                                   const_values);
      ng_op_map[op->name()] = ng_node;

    } else if ((op->type_string() == "Mul") || (op->type_string() == "Add")) {
      if (op->num_inputs() != 2) {
        cout << "Error: Number of inputs is not 2 for elementwise op\n";
        return nullptr;
      }
      tf::Node* tf_lhs;
      if (op->input_node(0, &tf_lhs) != tf::Status::OK()) {
        cout << "Cannot get the input node 0";
        return nullptr;
      }

      tf::Node* tf_rhs;
      if (op->input_node(1, &tf_rhs) != tf::Status::OK()) {
        cout << "Cannot get the input node 1";
        return nullptr;
      }

      auto ng_lhs = ng_op_map.find(tf_lhs->name())->second;
      auto ng_rhs = ng_op_map.find(tf_rhs->name())->second;

      cout << "NG LHS node: " << ng_lhs->get_name() << endl;
      cout << "NG RHS node: " << ng_rhs->get_name() << endl;

      shared_ptr<ng::Node> ng_op;

      if (op->type_string() == "Mul") {
        ng_op = make_shared<ngraph::op::Multiply>(ng_lhs, ng_rhs);
      } else if (op->type_string() == "Add") {
        ng_op = make_shared<ngraph::op::Add>(ng_lhs, ng_rhs);
      }

      // Add to the map
      ng_op_map[op->name()] = ng_op;
    }
  }

  cout << "Return Values\n";
  vector<shared_ptr<ng::Node>> ng_node_list;

  for (auto n : tf_ret_vals) {
    tf::Node* tf_node;
    if (n->input_node(0, &tf_node) != tf::Status::OK()) {
      cout << "Error: Cannot find the source of the return node!\n";
      return nullptr;
    }

    // Get the corresponding nGraph node
    auto item = ng_op_map.find(tf_node->name());
    if (item != ng_op_map.end()) {
      ng_node_list.push_back(item->second);
    } else {
      cout << "Error: Cannot find return node! " << tf_node->name() << endl;
    }
  }

  // Now create the nGraph function
  auto ng_func = unique_ptr<ng::Function>(
      new ng::Function(ng_node_list, ng_parameter_list));

  return move(ng_func);
}

}  // namespace ngraph_bridge