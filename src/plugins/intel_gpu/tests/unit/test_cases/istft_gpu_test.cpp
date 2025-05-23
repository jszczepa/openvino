// Copyright (C) 2018-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <intel_gpu/primitives/data.hpp>
#include <intel_gpu/primitives/input_layout.hpp>
#include <intel_gpu/primitives/istft.hpp>

#include "test_utils.h"

using namespace cldnn;
using namespace ::tests;

namespace {

constexpr float REL_EPS = 2e-3f;
constexpr float ABS_EPS = 1e-5f;

namespace helpers {
// TODO: Move to common place.

// Converts float vector to another type vector.
template <typename T>
std::vector<T> ConverFloatVector(const std::vector<float>& vec) {
    std::vector<T> ret;
    ret.reserve(vec.size());
    for (const auto& val : vec) {
        ret.push_back(T(val));
    }
    return ret;
}

// Allocates tensoer with given shape and data.
template <typename TDataType>
memory::ptr AllocateTensor(ov::PartialShape shape, const std::vector<TDataType>& data) {
    const layout lo = {shape, ov::element::from<TDataType>(), cldnn::format::bfyx};
    EXPECT_EQ(lo.get_linear_size(), data.size());
    memory::ptr tensor = get_test_engine().allocate_memory(lo);
    set_values<TDataType>(tensor, data);
    return tensor;
}

template <typename T>
void CompareTypedBuffers(const memory::ptr& output, const memory::ptr& expectedOutput, cldnn::stream& stream) {
    mem_lock<T> output_ptr(output, stream);
    mem_lock<T> wanted_output_ptr(expectedOutput, stream);

    ASSERT_EQ(output->get_layout(), expectedOutput->get_layout());
    ASSERT_EQ(output_ptr.size(), wanted_output_ptr.size());
    for (size_t i = 0; i < output_ptr.size(); ++i)
        ASSERT_TRUE(are_equal(wanted_output_ptr[i], output_ptr[i], REL_EPS, ABS_EPS)) << "at index " << i;
}

void CompareBuffers(const memory::ptr& output, const memory::ptr& expectedOutput, cldnn::stream& stream) {
    ASSERT_EQ(output->get_layout(), expectedOutput->get_layout());
    auto type = output->get_layout().data_type;

    switch (type) {
    case data_types::f32:
        helpers::CompareTypedBuffers<float>(output, expectedOutput, stream);
        break;

    default:
        GTEST_FAIL() << "Unsupported data type: " << type;
        break;
    }
}

}  // namespace helpers

struct ISTFTTestParams {
    ov::PartialShape signalShape;
    ov::PartialShape windowShape;
    ov::PartialShape outputShape;
    int64_t frameSize;
    int64_t frameStep;
    bool center;
    bool normalized;
    int length;
    std::vector<float> signalData;
    std::vector<float> windowData;
    std::vector<float> expectedOutput;
    std::string testcaseName;
};

class istft_test : public ::testing::TestWithParam<ISTFTTestParams> {
public:
    static std::string getTestCaseName(const testing::TestParamInfo<ISTFTTestParams>& obj) {
        auto param = obj.param;
        std::ostringstream result;
        result << "signalShape=" << param.signalShape;
        result << "_windowShape=" << param.windowShape;
        result << "_outputShape=" << param.outputShape;
        result << "_frameSize=" << param.frameSize;
        result << "_frameStep=" << param.frameStep;
        result << "_center=" << param.center;
        result << "_normalizedr=" << param.normalized;
        result << "_length=" << param.length;
        result << "_" << param.testcaseName;
        return result.str();
    }

    struct ISTFTInferenceParams {
        bool center;
        bool normalized;
        memory::ptr signal;
        memory::ptr window;
        memory::ptr frameSize;
        memory::ptr frameStep;
        memory::ptr length;
        memory::ptr expectedOutput;
    };

    template <ov::element::Type_t ET>
    ISTFTInferenceParams PrepareInferenceParams(const ISTFTTestParams& testParam) {
        using T = typename ov::element_type_traits<ET>::value_type;
        ISTFTInferenceParams ret;

        ret.center = testParam.center;
        ret.normalized = testParam.normalized;

        ret.signal = helpers::AllocateTensor<T>(testParam.signalShape, helpers::ConverFloatVector<T>(testParam.signalData));
        ret.window = helpers::AllocateTensor<T>(testParam.windowShape, helpers::ConverFloatVector<T>(testParam.windowData));
        ret.expectedOutput = helpers::AllocateTensor<T>(testParam.outputShape, helpers::ConverFloatVector<T>(testParam.expectedOutput));

        ret.frameStep = helpers::AllocateTensor<int64_t>({}, {testParam.frameStep});
        ret.frameSize = helpers::AllocateTensor<int64_t>({}, {testParam.frameSize});
        if (testParam.length > 0)
            ret.length = helpers::AllocateTensor<int64_t>({}, {testParam.length});

        return ret;
    }

    void Execute(const ISTFTInferenceParams& params) {
        // Prepare the network.
        auto stream = get_test_stream_ptr(get_test_default_config(engine_));

        auto scalar_layout = params.frameSize->get_layout();
        scalar_layout.set_partial_shape({});

        topology topology;
        topology.add(input_layout("signal", params.signal->get_layout()));
        topology.add(input_layout("window", params.window->get_layout()));
        topology.add(input_layout("frameSize", scalar_layout));
        topology.add(input_layout("frameStep", scalar_layout));
        std::vector<input_info> inputs = {
            input_info("signal"),
            input_info("window"),
            input_info("frameSize"),
            input_info("frameStep"),
        };
        if (params.length) {
            topology.add(input_layout("length", scalar_layout));
            inputs.push_back(input_info("length"));
        }
        topology.add(ISTFT("istft", inputs, params.center, params.normalized));

        cldnn::network::ptr network = get_network(engine_, topology, get_test_default_config(engine_), stream, false);

        network->set_input_data("signal", params.signal);
        network->set_input_data("window", params.window);
        network->set_input_data("frameSize", params.frameSize);
        network->set_input_data("frameStep", params.frameStep);
        if (params.length) {
            network->set_input_data("length", params.length);
        }

        // Run and check results.
        auto outputs = network->execute();

        auto output = outputs.at("istft").get_memory();

        helpers::CompareBuffers(output, params.expectedOutput, get_test_stream());
    }

private:
    engine& engine_ = get_test_engine();
};

std::vector<ISTFTTestParams> generateTestParams() {
    std::vector<ISTFTTestParams> params;
#define TEST_DATA(signalShape,                       \
                  windowShape,                       \
                  outputShape,                       \
                  frameSize,                         \
                  frameStep,                         \
                  center,                            \
                  normalized,                        \
                  length,                            \
                  signalData,                        \
                  windowData,                        \
                  expectedOutput,                    \
                  testcaseName)                      \
    params.push_back(ISTFTTestParams{signalShape,    \
                                     windowShape,    \
                                     outputShape,    \
                                     frameSize,      \
                                     frameStep,      \
                                     center,         \
                                     normalized,     \
                                     length,         \
                                     signalData,     \
                                     windowData,     \
                                     expectedOutput, \
                                     testcaseName});

#include "unit_test_utils/tests_data/istft_data.h"
#undef TEST_DATA

    return params;
}

}  // namespace

#define ISTFT_TEST_P(precision)                                                      \
    TEST_P(istft_test, ref_comp_##precision) {                                       \
        Execute(PrepareInferenceParams<ov::element::Type_t::precision>(GetParam())); \
    }

ISTFT_TEST_P(f32);

INSTANTIATE_TEST_SUITE_P(istft_test_suit, istft_test, testing::ValuesIn(generateTestParams()), istft_test::getTestCaseName);
