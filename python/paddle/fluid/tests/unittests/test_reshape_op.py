#   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import unittest
import numpy as np
from op_test import OpTest


class TestReshapeOp(OpTest):
    def setUp(self):
        self.op_type = "reshape"
        self.inputs = {'X': np.random.random((10, 20)).astype("float32")}
        self.attrs = {'shape': [10 * 20]}
        self.outputs = {'Out': self.inputs['X'].reshape(self.attrs['shape'])}

    def test_check_output(self):
        self.check_output()

    def test_check_grad(self):
        self.check_grad(["X"], "Out")


class TestReshapeOpDimInfer(OpTest):
    def setUp(self):
        self.op_type = "reshape"
        self.inputs = {'X': np.random.random((10, 20)).astype("float32")}
        self.attrs = {'shape': [4, -1, 5]}
        self.outputs = {'Out': self.inputs['X'].reshape(self.attrs['shape'])}

    def test_check_output(self):
        self.check_output()

    def test_check_grad(self):
        self.check_grad(["X"], "Out")


if __name__ == '__main__':
    unittest.main()