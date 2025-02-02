/*******************************************************************************
 * Copyright 2017-2019 Intel Corporation
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
#include "gtest/gtest.h"

#include "tensorflow/core/common_runtime/dma_helper.h"

#include "ngraph_bridge/ngraph_catalog.h"
#include "ngraph_bridge/ngraph_tensor_manager.h"
#include "ngraph_bridge/ngraph_utils.h"
#include "ngraph_bridge/version.h"
#include "test/test_utilities.h"

using namespace std;

namespace tensorflow {

namespace ngraph_bridge {

namespace testing {

class NGraphTensorManagerTest : public ::testing::Test {
 protected:
  // Utility to Simulate entering variable info in catalog
  void EnterVarInCatalog(const int& ng_encap_graph_id,
                         const string& ng_encap_node_name,
                         const vector<int>& var_inp_indexes,
                         const vector<int>& var_out_indexes,
                         const vector<int>& out_indexes_need_copy) {
    for (int index : var_inp_indexes) {
      string key = NGraphCatalog::CreateNodeKey(ng_encap_graph_id,
                                                ng_encap_node_name, index);
      NGraphCatalog::AddToInputVariableSharedNameMap(key, "abc");
    }

    for (int index : var_out_indexes) {
      string key = NGraphCatalog::CreateNodeKey(ng_encap_graph_id,
                                                ng_encap_node_name, index);
      NGraphCatalog::AddToEncapOutputInfoMap(key, make_tuple("abc", true));
    }

    unordered_set<int> indexes_need_copy;
    for (int index : out_indexes_need_copy) {
      indexes_need_copy.insert(index);
    }
    NGraphCatalog::AddToEncapOutputCopyIndexesMap(
        ng_encap_graph_id, ng_encap_node_name, indexes_need_copy);
  }

  void EnterPrefetchInCatalog(const int& ng_encap_graph_id,
                              const string& ng_encap_node_name,
                              const vector<int>& prefetched_inp_indexes) {
    unordered_set<int> pref_indexes;
    for (int index : prefetched_inp_indexes) {
      pref_indexes.insert(index);
    }
    NGraphCatalog::AddToPrefetchedInputIndexMap(
        ng_encap_graph_id, ng_encap_node_name, pref_indexes);
  }

  // Clears the Catalog
  void ClearCatalog() { NGraphCatalog::ClearCatalog(); }

  // returns {0,1,2, ... , size-1}
  vector<int> FillRange(int size) {
    vector<int> vout(size);
    iota(vout.begin(), vout.end(), 0);
    return vout;
  }
};

TEST(NGraphUtils, FindComplement1) {
  bool yes;
  Status st = IsNgraphTFLogTensorCopiesEnabled(0, yes);

  vector<int> input = {0, 3, 5, 8, 9};
  vector<int> complement = FindComplement(10, input);

  vector<int> expected = {1, 2, 4, 6, 7};
  ASSERT_EQ(expected, complement);

  // test 2
  input = {-1, 3, 5};
  complement = FindComplement(5, input);
  expected = {0, 1, 2, 4};
  ASSERT_EQ(expected, complement);
}

// Tests scenario when the graph has no variables
// and no prefetched inputs
TEST_F(NGraphTensorManagerTest, NoVariablesNoPrefetch) {
  string ng_encap_node_name = "xyz_1";
  int ng_encap_cluster_id = 1;
  int ng_encap_graph_id = 1;
  int number_of_inputs = 5;
  int number_of_outputs = 2;

  NGraphTensorManager tensor_manager(ng_encap_node_name, ng_encap_cluster_id,
                                     ng_encap_graph_id, number_of_inputs,
                                     number_of_outputs);
  // expected
  vector<int> empty;
  vector<int> expected_pipelined_inp_indexes = FillRange(number_of_inputs);
  vector<int> expected_pipelined_out_indexes = FillRange(number_of_outputs);

  // var related
  ASSERT_EQ(empty, tensor_manager.GetInputIndexesFedByVariables());
  ASSERT_EQ(empty, tensor_manager.GetOutputIndexesAssigningVariables());
  ASSERT_EQ(empty, tensor_manager.GetOutputIndexesThatNeedCopy());
  ASSERT_EQ(expected_pipelined_inp_indexes,
            tensor_manager.GetPipelinedInputIndexes());
  ASSERT_EQ(expected_pipelined_out_indexes,
            tensor_manager.GetPipelinedOutputIndexes());

  // prefetched
  ASSERT_EQ(empty, tensor_manager.GetPrefetchedInputIndexes());
}

// Tests scenario when the graph has variables but no prefetched inputs
//   1. For Var build: catalog is populated
//   2. For others: catalog is not populated
TEST_F(NGraphTensorManagerTest, HasVariablesNoPrefetch) {
  string ng_encap_node_name = "xyz_1";
  int ng_encap_cluster_id = 1;
  int ng_encap_graph_id = 1;
  int number_of_inputs = 5;
  int number_of_outputs = 2;

  // expected
  vector<int> expected_pipelined_inp_indexes, expected_pipelined_out_indexes,
      expected_var_inp_indexes, expected_var_out_indexes,
      expected_out_indexes_need_copy, expected_prefetched_inp_indexes;

  if (ngraph_tf_are_variables_enabled()) {
    // expected values
    expected_pipelined_inp_indexes = {1, 3, 4};
    expected_pipelined_out_indexes = {1};
    expected_var_inp_indexes =
        FindComplement(number_of_inputs, expected_pipelined_inp_indexes);
    expected_var_out_indexes =
        FindComplement(number_of_outputs, expected_pipelined_out_indexes);
    expected_out_indexes_need_copy = {1};
    expected_prefetched_inp_indexes = {};

    // enter in catalog
    EnterVarInCatalog(ng_encap_graph_id, ng_encap_node_name,
                      expected_var_inp_indexes, expected_var_out_indexes,
                      expected_out_indexes_need_copy);

  } else {
    expected_pipelined_inp_indexes = FillRange(number_of_inputs);
    expected_pipelined_out_indexes = FillRange(number_of_outputs);

    expected_var_inp_indexes = {};
    expected_var_out_indexes = {};
    expected_out_indexes_need_copy = {};
    expected_prefetched_inp_indexes = {};
  }

  NGraphTensorManager tensor_manager(ng_encap_node_name, ng_encap_cluster_id,
                                     ng_encap_graph_id, number_of_inputs,
                                     number_of_outputs);

  ASSERT_EQ(expected_var_inp_indexes,
            tensor_manager.GetInputIndexesFedByVariables());
  ASSERT_EQ(expected_var_out_indexes,
            tensor_manager.GetOutputIndexesAssigningVariables());
  ASSERT_EQ(expected_out_indexes_need_copy,
            tensor_manager.GetOutputIndexesThatNeedCopy());

  ASSERT_EQ(expected_prefetched_inp_indexes,
            tensor_manager.GetPrefetchedInputIndexes());

  ASSERT_EQ(expected_pipelined_inp_indexes,
            tensor_manager.GetPipelinedInputIndexes());
  ASSERT_EQ(expected_pipelined_out_indexes,
            tensor_manager.GetPipelinedOutputIndexes());

  // clean up
  ClearCatalog();
}

// Tests scenario when the graph has no variables
// but has prefetched inputs
TEST_F(NGraphTensorManagerTest, NoVariablesHasPrefetch) {
  string ng_encap_node_name = "xyz_1";
  int ng_encap_cluster_id = 1;
  int ng_encap_graph_id = 1;
  int number_of_inputs = 5;
  int number_of_outputs = 2;

  // expected
  vector<int> empty;
  vector<int> expected_pipelined_inp_indexes = FillRange(number_of_inputs);
  vector<int> expected_pipelined_out_indexes = FillRange(number_of_outputs);
  vector<int> expected_prefetched_inp_indexes = {1, 3};
  vector<int> expected_pipelined_inp_indexes_prefetched = {
      1, 3};  // as all inputs are pipelined

  EnterPrefetchInCatalog(ng_encap_graph_id, ng_encap_node_name,
                         expected_prefetched_inp_indexes);

  NGraphTensorManager tensor_manager(ng_encap_node_name, ng_encap_cluster_id,
                                     ng_encap_graph_id, number_of_inputs,
                                     number_of_outputs);

  // var related
  ASSERT_EQ(empty, tensor_manager.GetInputIndexesFedByVariables());
  ASSERT_EQ(empty, tensor_manager.GetOutputIndexesAssigningVariables());
  ASSERT_EQ(empty, tensor_manager.GetOutputIndexesThatNeedCopy());
  ASSERT_EQ(expected_pipelined_inp_indexes,
            tensor_manager.GetPipelinedInputIndexes());
  ASSERT_EQ(expected_pipelined_out_indexes,
            tensor_manager.GetPipelinedOutputIndexes());

  // prefetched
  ASSERT_EQ(expected_prefetched_inp_indexes,
            tensor_manager.GetPrefetchedInputIndexes());
  ASSERT_EQ(expected_pipelined_inp_indexes_prefetched,
            tensor_manager.GetPipelinedInputIndexesThatArePrefetched());

  // clean up
  ClearCatalog();
}

// Tests scenario when the graph has variables and prefetched inputs
TEST_F(NGraphTensorManagerTest, VariablesAndPrefetch) {
  string ng_encap_node_name = "xyz_1";
  int ng_encap_cluster_id = 1;
  int ng_encap_graph_id = 1;
  int number_of_inputs = 7;
  int number_of_outputs = 4;

  // expected
  vector<int> expected_pipelined_inp_indexes, expected_pipelined_out_indexes,
      expected_var_inp_indexes, expected_var_out_indexes,
      expected_out_indexes_need_copy, expected_prefetched_inp_indexes,
      expected_pipelined_inp_indexes_prefetched;

  if (ngraph_tf_are_variables_enabled()) {
    // expected values
    expected_pipelined_inp_indexes = {1, 3, 4, 6};
    expected_prefetched_inp_indexes = {3, 6};
    expected_pipelined_inp_indexes_prefetched = {1, 3};
    expected_pipelined_out_indexes = {0, 2};
    expected_var_inp_indexes =
        FindComplement(number_of_inputs, expected_pipelined_inp_indexes);
    expected_var_out_indexes =
        FindComplement(number_of_outputs, expected_pipelined_out_indexes);
    expected_out_indexes_need_copy = {2, 3};
    // enter in catalog
    EnterVarInCatalog(ng_encap_graph_id, ng_encap_node_name,
                      expected_var_inp_indexes, expected_var_out_indexes,
                      expected_out_indexes_need_copy);

  } else {
    expected_pipelined_inp_indexes = FillRange(number_of_inputs);
    expected_pipelined_out_indexes = FillRange(number_of_outputs);
    expected_prefetched_inp_indexes = {3, 6};
    expected_pipelined_inp_indexes_prefetched = {
        3, 6};  // all inputs are pipelined

    expected_var_inp_indexes = {};
    expected_var_out_indexes = {};
    expected_out_indexes_need_copy = {};
  }

  EnterPrefetchInCatalog(ng_encap_graph_id, ng_encap_node_name,
                         expected_prefetched_inp_indexes);

  NGraphTensorManager tensor_manager(ng_encap_node_name, ng_encap_cluster_id,
                                     ng_encap_graph_id, number_of_inputs,
                                     number_of_outputs);

  // var related
  ASSERT_EQ(expected_var_inp_indexes,
            tensor_manager.GetInputIndexesFedByVariables());
  ASSERT_EQ(expected_var_out_indexes,
            tensor_manager.GetOutputIndexesAssigningVariables());
  ASSERT_EQ(expected_out_indexes_need_copy,
            tensor_manager.GetOutputIndexesThatNeedCopy());
  ASSERT_EQ(expected_pipelined_inp_indexes,
            tensor_manager.GetPipelinedInputIndexes());
  ASSERT_EQ(expected_pipelined_out_indexes,
            tensor_manager.GetPipelinedOutputIndexes());

  // prefetched
  ASSERT_EQ(expected_prefetched_inp_indexes,
            tensor_manager.GetPrefetchedInputIndexes());
  ASSERT_EQ(expected_pipelined_inp_indexes_prefetched,
            tensor_manager.GetPipelinedInputIndexesThatArePrefetched());
  // clean up
  ClearCatalog();
}

// check error
TEST_F(NGraphTensorManagerTest, PrefetchNotInPipeline) {
  string ng_encap_node_name = "xyz_1";
  int ng_encap_cluster_id = 1;
  int ng_encap_graph_id = 1;
  int number_of_inputs = 5;
  int number_of_outputs = 2;

  vector<int> prefetched_inp_indexes = {6, 7};
  EnterPrefetchInCatalog(ng_encap_graph_id, ng_encap_node_name,
                         prefetched_inp_indexes);

  ASSERT_THROW(NGraphTensorManager tensor_manager(
                   ng_encap_node_name, ng_encap_cluster_id, ng_encap_graph_id,
                   number_of_inputs, number_of_outputs),
               std::runtime_error);

  // clean up
  ClearCatalog();
}

}  // namespace testing
}  // namespace ngraph_bridge
}  // namespace tensorflow