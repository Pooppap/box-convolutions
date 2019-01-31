#include <torch/extension.h>
#include <ATen/AccumulateType.h>
#include <TH/THGeneral.h>

#include <box_convolution.h>

at::Tensor integral_image(at::Tensor input);

#define CHECK_CONTIGUOUS(x) AT_CHECK(x.is_contiguous(), #x " must be contiguous")

at::Tensor box_convolution_forward(
    at::Tensor input_integrated,
    at::Tensor x_min, at::Tensor x_max,
    at::Tensor y_min, at::Tensor y_max, const bool normalize) {

    CHECK_CONTIGUOUS(input_integrated);
    AT_CHECK(input_integrated.dim() == 4, "box conv input must have 4 dimensions");
    AT_CHECK(
        x_min.dim() == 2 and x_max.dim() == 2 and y_min.dim() == 2 and y_max.dim() == 2, 
        "all box conv parameters must have 2 dimensions");
    AT_CHECK(
        x_min.size(0) == x_max.size(0) and x_min.size(0) == y_min.size(0) and 
        x_min.size(0) == y_max.size(0) and x_min.size(0) == input_integrated.size(1), 
        "all box conv parameters must have as many rows as there are input channels");
    AT_CHECK(
        x_min.size(1) == x_max.size(1) and x_min.size(1) == y_min.size(1) and 
        x_min.size(1) == y_max.size(1),
        "all box conv parameters must have equal number of columns");

    // Split x_min, x_max, y_min, y_max into integer and fractional parts
    auto intOptions = x_min.options().is_variable(false).dtype(caffe2::TypeMeta::Make<int>());
    auto xMinInt = at::empty(x_min.sizes(), intOptions);
    auto xMaxInt = at::empty(x_min.sizes(), intOptions);
    auto yMinInt = at::empty(x_min.sizes(), intOptions);
    auto yMaxInt = at::empty(x_min.sizes(), intOptions);

    auto fracOptions = x_min.options().is_variable(false);
    auto xMinFrac = at::empty(x_min.sizes(), fracOptions);
    auto xMaxFrac = at::empty(x_min.sizes(), fracOptions);
    auto yMinFrac = at::empty(x_min.sizes(), fracOptions);
    auto yMaxFrac = at::empty(x_min.sizes(), fracOptions);

    // inverse box areas for normalization
    at::Tensor area;

    if (x_min.is_cuda()) {
        THError("NYI: gpu::splitParameters");

        if (normalize) {
            THError("NYI: gpu::computeArea");
        }
    } else {
        cpu::splitParameters(
            x_min   , x_max   , y_min   , y_max   ,
            xMinInt , xMaxInt , yMinInt , yMaxInt ,
            xMinFrac, xMaxFrac, yMinFrac, yMaxFrac);

        if (normalize) {
            area = cpu::computeArea(x_min, x_max, y_min, y_max);
        }
    }

    const int batchSize = input_integrated.size(0);
    const int nInputPlanes = input_integrated.size(1);
    const int numFilters = x_min.size(1);
    const int h = input_integrated.size(2) - 1;
    const int w = input_integrated.size(3) - 1;

    // Output will be 1 pixel smaller and have `num_filters` channels per each input channel
    auto output = at::empty(
        {batchSize, nInputPlanes, numFilters, h, w}, input_integrated.options());
    CHECK_CONTIGUOUS(output);

    // Actually fill `output`
    if (input_integrated.is_cuda()) {
        THError("NYI: gpu::boxConvUpdateOutput");
    } else {
        if (normalize) {
            cpu::boxConvUpdateOutput<true>(
                xMinInt , xMaxInt , yMinInt , yMaxInt ,
                xMinFrac, xMaxFrac, yMinFrac, yMaxFrac,
                area, input_integrated, output);
        } else {
            cpu::boxConvUpdateOutput<false>(
                xMinInt , xMaxInt , yMinInt , yMaxInt ,
                xMinFrac, xMaxFrac, yMinFrac, yMaxFrac,
                area, input_integrated, output);
        }
    }

    return output.reshape({batchSize, nInputPlanes * numFilters, h, w});
}

std::vector<at::Tensor> box_convolution_backward(
    at::Tensor input_integrated,
    at::Tensor x_min, at::Tensor x_max,
    at::Tensor y_min, at::Tensor y_max,
    at::Tensor grad_output, at::Tensor output,
    const float max_input_h, const float max_input_w, const bool normalize,
    const bool input_needs_grad,
    const bool x_min_needs_grad, const bool x_max_needs_grad,
    const bool y_min_needs_grad, const bool y_max_needs_grad) {

    CHECK_CONTIGUOUS(grad_output);
    AT_CHECK(grad_output.dim() == 4, "grad_output for box_convolution must have 4 dimensions")
    AT_CHECK(
        grad_output.size(0) == input_integrated.size(0) and
        grad_output.size(1) == input_integrated.size(1) * x_min.size(1) and
        grad_output.size(2) == input_integrated.size(2) - 1 and
        grad_output.size(2) == input_integrated.size(2) - 1,
        "box_convolution: sizes of grad_output and input_integrated don't match");
    AT_CHECK(
        x_min.dim() == 2 and x_max.dim() == 2 and y_min.dim() == 2 and y_max.dim() == 2, 
        "all box conv parameters must have 2 dimensions");
    AT_CHECK(
        x_min.size(0) == x_max.size(0) and x_min.size(0) == y_min.size(0) and 
        x_min.size(0) == y_max.size(0) and x_min.size(0) == input_integrated.size(1), 
        "all box conv parameters must have as many rows as there are input channels");
    AT_CHECK(
        x_min.size(1) == x_max.size(1) and x_min.size(1) == y_min.size(1) and 
        x_min.size(1) == y_max.size(1),
        "all box conv parameters must have equal number of columns");

    const int batchSize = input_integrated.size(0);
    const int nInputPlanes = input_integrated.size(1);
    const int numFilters = x_min.size(1);
    const int h = input_integrated.size(2) - 1;
    const int w = input_integrated.size(3) - 1;

    grad_output = grad_output.reshape({batchSize, nInputPlanes, numFilters, h, w});

    // Return value
    // TODO change `nullTensor` to Python `None`
    at::Tensor nullTensor = at::empty({0}, at::TensorOptions().is_variable(true));
    at::Tensor gradInput = nullTensor;

    // Allocate memory for splitting x_min, x_max, y_min, y_max into integer and fractional parts
    auto intOptions = x_min.options().is_variable(false).dtype(caffe2::TypeMeta::Make<int>());
    auto xMinInt = at::empty(x_min.sizes(), intOptions);
    auto xMaxInt = at::empty(x_min.sizes(), intOptions);
    auto yMinInt = at::empty(x_min.sizes(), intOptions);
    auto yMaxInt = at::empty(x_min.sizes(), intOptions);

    auto fracOptions = x_min.options().is_variable(false);
    auto xMinFrac = at::empty(x_min.sizes(), fracOptions);
    auto xMaxFrac = at::empty(x_min.sizes(), fracOptions);
    auto yMinFrac = at::empty(x_min.sizes(), fracOptions);
    auto yMaxFrac = at::empty(x_min.sizes(), fracOptions);


    if (input_needs_grad) {
        at::Tensor grad_output_integrated = integral_image(grad_output);

        at::Tensor tmpArray = at::empty(
            {batchSize, nInputPlanes, numFilters, h, w}, grad_output.options());
        CHECK_CONTIGUOUS(tmpArray);

        at::Tensor area; // box area for normalization

        if (gradInput.is_cuda()) {
            if (normalize) {
                THError("NYI: gpu::computeArea");
            }

            THError("NYI: gpu::boxConvUpdateGradInput");
        } else {
            if (normalize) {
                area = cpu::computeArea(x_min, x_max, y_min, y_max);

                cpu::boxConvUpdateGradInput<true>(
                    x_min, x_max, y_min, y_max,
                    xMinInt , xMaxInt , yMinInt , yMaxInt ,
                    xMinFrac, xMaxFrac, yMinFrac, yMaxFrac,
                    area, grad_output_integrated, tmpArray);
            } else {
                cpu::boxConvUpdateGradInput<false>(
                    x_min, x_max, y_min, y_max,
                    xMinInt , xMaxInt , yMinInt , yMaxInt ,
                    xMinFrac, xMaxFrac, yMinFrac, yMaxFrac,
                    area, grad_output_integrated, tmpArray);
            }
        }

        gradInput = tmpArray.sum(2);
    } // if (input_needs_grad)

    bool paramNeedsGrad[4] = {x_min_needs_grad, x_max_needs_grad, y_min_needs_grad, y_max_needs_grad};
    at::Tensor gradParam[4] = {nullTensor, nullTensor, nullTensor, nullTensor};

    at::Tensor tmpArray;
    at::Tensor area; // box area for normalization
    
    bool someParamNeedsGrad = false;
    for (bool needsGrad : paramNeedsGrad) {
        someParamNeedsGrad |= needsGrad;
    }

    if (someParamNeedsGrad) {
        tmpArray = at::empty({batchSize, nInputPlanes, numFilters, h, w}, x_min.options());

        if (x_min.is_cuda()) {
            THError("NYI: gpu::splitParameters");

            if (normalize) {
                THError("NYI: gpu::computeArea");
            }
        } else {
            cpu::splitParametersAccGradParameters(
                x_min   , x_max   , y_min   , y_max   ,
                xMinInt , xMaxInt , yMinInt , yMaxInt ,
                xMinFrac, xMaxFrac, yMinFrac, yMaxFrac);

            if (normalize) {
                area = cpu::computeArea(x_min, x_max, y_min, y_max);
            }
        }

        for (int paramIdx = 0; paramIdx < 4; ++paramIdx) {
            if (paramNeedsGrad[paramIdx]) {
                const Parameter paramId = static_cast<Parameter>(paramIdx);

                if (input_integrated.is_cuda()) {
                    THError("NYI: gpu::boxConvAccGradParameters");
                } else {cpu::boxConvAccGradParameters(
                        xMinInt , xMaxInt , yMinInt , yMaxInt ,
                        xMinFrac, xMaxFrac, yMinFrac, yMaxFrac,
                        input_integrated, tmpArray, paramId);
                }

                tmpArray.mul_(grad_output);

                gradParam[paramIdx] = 
                    tmpArray.reshape({batchSize, nInputPlanes, numFilters, h*w}).sum({0, 3});

                if (normalize) {
                    gradParam[paramIdx].mul_(area);
                }
            }
        }

        if (normalize) { // add the second summand
            output = output.reshape({batchSize, nInputPlanes, numFilters, h, w});
            
            tmpArray = grad_output.mul(output);
            tmpArray = tmpArray.reshape({batchSize, nInputPlanes, numFilters, h*w}).sum({0, 3});

            for (int paramIdx = 0; paramIdx < 4; ++paramIdx) {
                if (paramNeedsGrad[paramIdx]) {
                    const Parameter paramId = static_cast<Parameter>(paramIdx);

                    // multiply by area derivative and divide by area
                    const bool needXDeriv = paramId == Parameter::xMin or paramId == Parameter::xMax;
                    const bool needYDeriv = not needXDeriv;

                    if (x_min.is_cuda()) {
                        THError("NYI: gpu::computeArea");
                    } else {
                        area = cpu::computeArea(x_min, x_max, y_min, y_max, needXDeriv, needYDeriv);
                    }

                    const bool minus = paramId == Parameter::xMax or paramId == Parameter::yMax;
                    gradParam[paramIdx].addcmul_(tmpArray, area, minus ? -1.0 : 1.0);
                }
            }
        }

        // account for reparametrization
        for (int paramIdx = 0; paramIdx < 4; ++paramIdx) {
            if (paramNeedsGrad[paramIdx]) {
                const Parameter paramId = static_cast<Parameter>(paramIdx);
                const double scale = paramId == Parameter::xMin or paramId == Parameter::xMax
                                     ? max_input_h : max_input_w;

                gradParam[paramIdx].mul_(scale);
            }
        }
    } // if (someParamNeedsGrad)

    return {gradInput, gradParam[0], gradParam[1], gradParam[2], gradParam[3]};
}

void clip_parameters(
    at::Tensor x_min, at::Tensor x_max,
    at::Tensor y_min, at::Tensor y_max,
    const float max_input_h, const float max_input_w, const bool exact) {

    CHECK_CONTIGUOUS(x_min); // and assume other parameter tensors have same layout

    const float minWidth  = exact ? 1.001f : 2.001f;
    const float minHeight = exact ? 1.001f : 2.001f;

    if (x_min.is_cuda()) {
        THError("NYI: gpu::clip_parameters");
    } else {
        cpu::clipParameters(x_min, x_max, minHeight, max_input_h);
        cpu::clipParameters(y_min, y_max, minWidth , max_input_w);
    }
}