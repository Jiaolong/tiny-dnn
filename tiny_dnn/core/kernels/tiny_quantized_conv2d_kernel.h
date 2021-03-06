/*
    Copyright (c) 2016, Taiga Nomi, Edgar Riba
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#include "tiny_dnn/core/params/conv_params.h"
#include "tiny_dnn/core/kernels/tiny_quantization_kernel.h"

namespace tiny_dnn {
namespace core {
namespace kernels {

void tiny_quantized_conv2d_kernel(const conv_params& params,
                                  const vec_t&       in,
                                  const vec_t&       W,
                                  const vec_t&       bias,
                                  vec_t&             a,
                                  const bool layer_parallelize) {
    // image quantization
    float min_input(in[0]);
    float max_input(in[0]);
    for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.in_padded.height_*params.in_padded.height_; ins++) {
            cnn_size_t idx = params.in_padded.get_index(0, 0, inc);
            min_input = std::min(min_input, (&in[idx])[ins]);
            max_input = std::max(max_input, (&in[idx])[ins]);
        }
    }
    std::vector<uint8_t> in_quantized =
        float_tensor_to_quantized<uint8_t>(in, min_input, max_input);
    // filter quantization
    float min_filter(W[0]);
    float max_filter(W[0]);
    for (cnn_size_t inc = 0; inc < params.in_padded.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.weight.height_*params.weight.height_; ins++) {
            cnn_size_t idx = params.in_padded.get_index(0, 0, inc);
            min_filter = std::min(min_filter, (&W[idx])[ins]);
            max_filter = std::max(max_filter, (&W[idx])[ins]);
        }
    }
    if (min_filter == max_filter) {
      max_filter = W[0] + 1e-3f;
      min_filter = W[0] - 1e-3f;
    }
    std::vector<uint8_t> W_quantized =
        float_tensor_to_quantized<uint8_t>(W, min_filter, max_filter);
    // bias quantization
    float min_bias(0);
    float max_bias(0);
    std::vector<uint8_t> bias_quantized;
    if (params.has_bias) {
        for (cnn_size_t inc = 0; inc < params.out.depth_; inc++) {
            min_bias = std::min(min_bias, bias[inc]);
            max_bias = std::max(max_bias, bias[inc]);
        }
        if (min_bias == max_bias) {
          max_bias = bias[0] + 1e-3f;
          min_bias = bias[0] - 1e-3f;
        }
        bias_quantized =
            float_tensor_to_quantized<uint8_t>(bias, min_bias, max_bias);
    }
    // output range
    float min_output_value;
    float max_output_value;
    quantization_range_for_multiplication<uint8_t, uint8_t, int32_t>(
        min_input, max_input, min_filter, max_filter, &min_output_value,
        &max_output_value);

    std::vector<int32_t> a_quantized(a.size(), static_cast<int32_t>(0));

    // calculating offset
    const int32_t offset_input =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_input, max_input);
    const int32_t offset_filter =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_filter, max_filter);
    const int32_t zero_in_total_space =
        float_to_quantized<int32_t>(0.0f, min_output_value, max_output_value);

    for_i(layer_parallelize, params.out.depth_, [&](int o) {
        for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
            if (!params.tbl.is_connected(o, inc)) continue;

            cnn_size_t idx = 0;
            idx = params.in.depth_ * o + inc;
            idx = params.weight.get_index(0, 0, idx);
            const uint8_t *pw = &W_quantized[idx];

            idx = params.in_padded.get_index(0, 0, inc);
            const uint8_t *pi = &in_quantized[idx];

            idx = params.out.get_index(0, 0, o);
            int32_t *pa_quantized = &a_quantized[idx];

            for (cnn_size_t y = 0; y < params.out.height_; y++) {
                for (cnn_size_t x = 0; x < params.out.width_; x++) {
                    const uint8_t * ppw = pw;
                    const uint8_t * ppi = pi + params.in_padded.width_ *
                                        (y * params.h_stride) +
                                         x * params.w_stride;
                    int32_t sum = 0;

                    // should be optimized for small kernel(3x3,5x5)
                    for (cnn_size_t wy = 0; wy < params.weight.height_; wy++) {
                        for (cnn_size_t wx = 0; wx < params.weight.width_; wx++) {
                            idx = wy * params.in_padded.width_ + wx;
                            sum += (static_cast<int32_t>(*ppw++) - offset_filter)
                                    * (static_cast<int32_t>(ppi[idx]) - offset_input);
                        }
                    }
                    pa_quantized[y * params.out.width_ + x] += sum;
                }
            }
        }
        if (params.has_bias) {
            int32_t * pa_quantized  = &a_quantized[params.out.get_index(0, 0, o)];
            int32_t * paa_quantized = pa_quantized + params.out.width_ * params.out.height_;
            std::for_each(pa_quantized, paa_quantized, [&](int32_t& f) {
                f += (bias_quantized[o] - zero_in_total_space);
            });
        }
    });

    float min_output_requantized;
    float max_output_requantized;
    std::vector<uint8_t> a_requantized(a_quantized.size(), static_cast<uint8_t>(0));

    // Requantize from 32bits to 8 bits for next layer
    quantize_down_and_shrink_range<int32_t, uint8_t>(a_quantized, min_output_value, max_output_value,
    &min_output_requantized, &max_output_requantized, &a_requantized);

    // dequantize to flaot, this could be removed within concatenated quantized network
    a = quantized_tensor_to_float<uint8_t>(a_requantized, min_output_requantized, max_output_requantized);
}

void tiny_quantized_conv2d_back_kernel(const conv_params& params,
                                       const vec_t& prev_out,
                                       const vec_t& W,
                                       vec_t&       dW,
                                       vec_t&       db,
                                       vec_t&       curr_delta,
                                       vec_t*       prev_delta) {
    // previous output quantization
    float min_prev_out(prev_out[0]);
    float max_prev_out(prev_out[0]);
    for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.in_padded.height_*params.in_padded.height_; ins++) {
            cnn_size_t idx = params.in_padded.get_index(0, 0, inc);
            min_prev_out = std::min(min_prev_out, (&prev_out[idx])[ins]);
            max_prev_out = std::max(min_prev_out, (&prev_out[idx])[ins]);
        }
    }
    std::vector<uint8_t> prev_out_quantized =
        float_tensor_to_quantized<uint8_t>(prev_out, min_prev_out, max_prev_out);

    // filter quantization
    float min_filter(W[0]);
    float max_filter(W[0]);
    for (cnn_size_t inc = 0; inc < params.in_padded.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.weight.height_*params.weight.height_; ins++) {
            cnn_size_t idx = params.in_padded.get_index(0, 0, inc);
            min_filter = std::min(min_filter, (&W[idx])[ins]);
            max_filter = std::max(max_filter, (&W[idx])[ins]);
        }
    }
    if (min_filter == max_filter) {
      max_filter = W[0] + 1e-3f;
      min_filter = W[0] - 1e-3f;
    }
    std::vector<uint8_t> W_quantized =
        float_tensor_to_quantized<uint8_t>(W, min_filter, max_filter);

    // current delta quantization
    float min_curr_delta(curr_delta[0]);
    float max_curr_delta(curr_delta[0]);
    for (cnn_size_t inc = 0; inc < params.out.depth_; inc++) {
        for (cnn_size_t ins = 0; ins < params.out.height_*params.out.height_; ins++) {
            cnn_size_t idx = params.out.get_index(0, 0, inc);
            min_curr_delta = std::min(min_curr_delta, (&curr_delta[idx])[ins]);
            max_curr_delta = std::max(max_curr_delta, (&curr_delta[idx])[ins]);
        }
    }
    std::vector<uint8_t> curr_delta_quantized =
        float_tensor_to_quantized<uint8_t>(curr_delta, min_curr_delta, max_curr_delta);

    // output range for previous delta
    float min_prev_delta_value;
    float max_prev_delta_value;
    quantization_range_for_multiplication<uint8_t, uint8_t, int32_t>(
        min_curr_delta, max_curr_delta, min_filter, max_filter, &min_prev_delta_value,
        &max_prev_delta_value);

    std::vector<int32_t> prev_delta_quantized(prev_delta->size(), static_cast<int32_t>(0));

    // output range for dW
    float min_dW_value;
    float max_dW_value;
    quantization_range_for_multiplication<uint8_t, uint8_t, int32_t>(
        min_curr_delta, max_curr_delta, min_prev_out, max_prev_out, &min_dW_value,
        &max_dW_value);

    std::vector<int32_t> dW_quantized(dW.size(), static_cast<int32_t>(0));

    // calculating offset
    const int32_t offset_prev_out =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_prev_out, max_prev_out);
    const int32_t offset_filter =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_filter, max_filter);
    const int32_t offset_curr_delta =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_curr_delta, max_curr_delta);
    // const int32_t zero_in_prev_delta =
    //    float_to_quantized<int32_t>(0.0f, min_prev_delta_value, max_prev_delta_value);

    // propagate delta to previous layer
    for_i(params.in.depth_, [&](int inc) {
        for (cnn_size_t outc = 0; outc < params.out.depth_; outc++) {
            if (!params.tbl.is_connected(outc, inc)) continue;

            cnn_size_t idx = 0;
            idx = params.in.depth_ * outc + inc;
            idx = params.weight.get_index(0, 0, idx);
            const uint8_t *pw = &W_quantized[idx];

            idx = params.out.get_index(0, 0, outc);
            const uint8_t *pdelta_src = &curr_delta_quantized[idx];

            idx = params.in_padded.get_index(0, 0, inc);
            int32_t *pdelta_quantized_dst = &(prev_delta_quantized)[idx];

            for (cnn_size_t y = 0; y < params.out.height_; y++) {
                for (cnn_size_t x = 0; x < params.out.width_; x++) {
                    const uint8_t * ppw = pw;

                    idx = y * params.out.width_ + x;
                    const uint8_t ppdelta_src = pdelta_src[idx];

                    int32_t * ppdelta_quantized_dst = pdelta_quantized_dst +
                          y * params.h_stride * params.in_padded.width_ +
                          x * params.w_stride;

                    for (cnn_size_t wy = 0; wy < params.weight.height_; wy++) {    // NOLINT
                        for (cnn_size_t wx = 0; wx < params.weight.width_; wx++) { // NOLINT
                            idx = wy * params.in_padded.width_ + wx;
                            ppdelta_quantized_dst[idx] += (static_cast<int32_t>(*ppw++) - offset_filter)
                                                * (static_cast<int32_t>(ppdelta_src) - offset_curr_delta);
                        }
                    }
                }
            }
        }
    });

    float min_prev_delta_requantized;
    float max_prev_delta_requantized;
    std::vector<uint8_t> prev_delta_requantized(prev_delta_quantized.size(), static_cast<uint8_t>(0));

    // Requantize from 32bits to 8 bits for next layer
    quantize_down_and_shrink_range<int32_t, uint8_t>(prev_delta_quantized, min_prev_delta_value, max_prev_delta_value,
    &min_prev_delta_requantized, &max_prev_delta_requantized, &prev_delta_requantized);

    // dequantize to flaot, this could be removed within concatenated quantized network
    vec_t prev_delta_vec = quantized_tensor_to_float<uint8_t>(prev_delta_requantized, min_prev_delta_requantized, max_prev_delta_requantized);
    prev_delta = &prev_delta_vec;

    // Accumulate dw
    for_i(params.in.depth_, [&](int inc) {
        for (cnn_size_t outc = 0; outc < params.out.depth_; outc++) {
            if (!params.tbl.is_connected(outc, inc)) continue;

            for (cnn_size_t wy = 0; wy < params.weight.height_; wy++) {
                for (cnn_size_t wx = 0; wx < params.weight.width_; wx++) {
                    int32_t dst = int32_t(0);

                    cnn_size_t idx = 0;
                    idx = params.in_padded.get_index(wx, wy, inc);
                    const uint8_t * prevo = &prev_out_quantized[idx];

                    idx = params.out.get_index(0, 0, outc);
                    const uint8_t * delta = &curr_delta_quantized[idx];

                    for (cnn_size_t y = 0; y < params.out.height_; y++) {
                        for (cnn_size_t x = 0; x < params.out.width_; x++) {
                            dst += (static_cast<int32_t>(*(prevo + y * params.in_padded.width_ + x)) - offset_prev_out) *
                                   (static_cast<int32_t>(*(delta + y * params.out.width_ + x)) - offset_curr_delta);
                        }
                    }

                    idx = params.in.depth_ * outc + inc;
                    dW_quantized[params.weight.get_index(wx, wy, idx)] += dst;
                }
            }
        }
    });

    float min_dW_requantized;
    float max_dW_requantized;
    std::vector<uint8_t> dW_requantized(dW_quantized.size(), static_cast<uint8_t>(0));

    // requantize from 32bits to 8 bits for next layer
    quantize_down_and_shrink_range<int32_t, uint8_t>(dW_quantized, min_dW_value, max_dW_value,
    &min_dW_requantized, &max_dW_requantized, &dW_requantized);

    // dequantize to flaot, this could be removed within concatenated quantized network
    dW = quantized_tensor_to_float<uint8_t>(dW_requantized, min_dW_requantized, max_dW_requantized);

    // Accumulate db
    if (params.has_bias) {
        //vec_t& db = *in_grad[2];

        for (cnn_size_t outc = 0; outc < params.out.depth_; outc++) {
            cnn_size_t idx = params.out.get_index(0, 0, outc);
            const float_t * delta = &curr_delta[idx];
            const float_t * deltaa = delta + params.out.width_ *
                                             params.out.height_;
            db[outc] += std::accumulate(delta, deltaa, float_t(0));
        }
    }
}

void tiny_quantized_conv2d_kernel(const conv_params& params,
                                  const vec_t&       in,
                                  const vec_t&       W,
                                  const vec_t&       bias,
                                  const vec_t&       in_r,
                                  const vec_t&       W_r,
                                  const vec_t&       b_r,
                                  vec_t&             a,
                                  vec_t&             a_r,
                                  const bool layer_parallelize) {
    // filter range
    float min_filter(W_r[0]);
    float max_filter(W_r[1]);
    if (W_r[0] == W_r[1]) {
      max_filter = W_r[1] + 1e-3f;
      min_filter = W_r[0] - 1e-3f;
    }
    // bias range
    float min_bias(b_r[0]);
    float max_bias(b_r[1]);
    if (params.has_bias) {
        if (min_bias == max_bias) {
          max_bias = b_r[1] + 1e-3f;
          min_bias = b_r[0] - 1e-3f;
        }
    }
    // output range
    float min_output_value;
    float max_output_value;
    quantization_range_for_multiplication<uint8_t, uint8_t, int32_t>(
        in_r[0], in_r[1], min_filter, max_filter, &min_output_value,
        &max_output_value);
    // data type restore
    std::vector<uint8_t> in_quantized, W_quantized, bias_quantized;
    for (size_t i = 0; i < in.size(); i++) {
       in_quantized.push_back(static_cast<uint8_t>(in[i]));
    }
    for (size_t i = 0; i < W.size(); i++) {
        W_quantized.push_back(static_cast<uint8_t>(W[i]));
    }
    for (size_t i = 0; i < bias.size(); i++) {
        bias_quantized.push_back(static_cast<uint8_t>(bias[i]));
    }

    std::vector<int32_t> a_quantized(a.size(), static_cast<int32_t>(0));

    // calculating offset
    const int32_t offset_input =
        float_to_quantized_unclamped<uint8_t>(0.0f, in_r[0], in_r[1]);
    const int32_t offset_filter =
        float_to_quantized_unclamped<uint8_t>(0.0f, min_filter, max_filter);
    const int32_t zero_in_total_space =
        float_to_quantized<int32_t>(0.0f, min_output_value, max_output_value);

    for_i(layer_parallelize, params.out.depth_, [&](int o) {
        for (cnn_size_t inc = 0; inc < params.in.depth_; inc++) {
            if (!params.tbl.is_connected(o, inc)) continue;

            cnn_size_t idx = 0;
            idx = params.in.depth_ * o + inc;
            idx = params.weight.get_index(0, 0, idx);
            const uint8_t *pw = &W_quantized[idx];

            idx = params.in_padded.get_index(0, 0, inc);
            const uint8_t *pi = &in_quantized[idx];

            idx = params.out.get_index(0, 0, o);
            int32_t *pa_quantized = &a_quantized[idx];

            for (cnn_size_t y = 0; y < params.out.height_; y++) {
                for (cnn_size_t x = 0; x < params.out.width_; x++) {
                    const uint8_t * ppw = pw;
                    const uint8_t * ppi = pi + params.in_padded.width_ *
                                        (y * params.h_stride) +
                                         x * params.w_stride;
                    int32_t sum = 0;

                    // should be optimized for small kernel(3x3,5x5)
                    for (cnn_size_t wy = 0; wy < params.weight.height_; wy++) {
                        for (cnn_size_t wx = 0; wx < params.weight.width_; wx++) {
                            idx = wy * params.in_padded.width_ + wx;
                            sum += (static_cast<int32_t>(*ppw++) - offset_filter)
                                    * (static_cast<int32_t>(ppi[idx]) - offset_input);
                        }
                    }
                    pa_quantized[y * params.out.width_ + x] += sum;
                }
            }
        }
        if (params.has_bias) {
            int32_t * pa_quantized  = &a_quantized[params.out.get_index(0, 0, o)];
            int32_t * paa_quantized = pa_quantized + params.out.width_ * params.out.height_;
            std::for_each(pa_quantized, paa_quantized, [&](int32_t& f) {
                f += (bias[o] - zero_in_total_space);
            });
        }
    });

    float min_output_requantized;
    float max_output_requantized;
    std::vector<uint8_t> a_requantized(a_quantized.size(), static_cast<uint8_t>(0));

    // Requantize from 32bits to 8 bits for next layer
    quantize_down_and_shrink_range<int32_t, uint8_t>(a_quantized, min_output_value, max_output_value,
        &min_output_requantized, &max_output_requantized, &a_requantized);
    // store directly in float datatype
    for (size_t i = 0; i < a_requantized.size(); i++) {
        a[i] = static_cast<float>(a_requantized[i]);
    }
    a_r[0] = min_output_requantized;
    a_r[1] = max_output_requantized;
}

}  // namespace kernels
}  // namespace core
}  // namespace tiny_dnn
