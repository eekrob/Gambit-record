#include "video/VideoProcessor.hpp"
#include <stdexcept>

namespace evidence {
void VideoProcessor::ensure(std::uint32_t width, std::uint32_t height) {
  if (processor_ && width == width_ && height == height_) return; reset(); width_ = width & ~1u; height_ = height & ~1u;
  if (FAILED(device_.device()->QueryInterface(IID_PPV_ARGS(&video_device_))) || FAILED(device_.context()->QueryInterface(IID_PPV_ARGS(&video_context_)))) throw std::runtime_error("D3D11 video processor unavailable");
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{}; desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; desc.InputWidth = width_; desc.InputHeight = height_; desc.OutputWidth = width_; desc.OutputHeight = height_; desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  desc.InputFrameRate = {60, 1}; desc.OutputFrameRate = {60, 1};
  if (FAILED(video_device_->CreateVideoProcessorEnumerator(&desc, &enumerator_)) || FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) throw std::runtime_error("cannot create D3D11 video processor");
}
Microsoft::WRL::ComPtr<ID3D11Texture2D> VideoProcessor::convert_bgra_to_nv12(ID3D11Texture2D* source, std::uint32_t width, std::uint32_t height) {
  ensure(width, height);
  D3D11_TEXTURE2D_DESC out_desc{}; out_desc.Width = width_; out_desc.Height = height_; out_desc.MipLevels = 1; out_desc.ArraySize = 1; out_desc.Format = DXGI_FORMAT_NV12; out_desc.SampleDesc.Count = 1; out_desc.Usage = D3D11_USAGE_DEFAULT; out_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> output; if (FAILED(device_.device()->CreateTexture2D(&out_desc, nullptr, &output))) throw std::runtime_error("cannot create NV12 texture");
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{}; in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; in_desc.Texture2D.MipSlice = 0;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov_desc{}; ov_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view; Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
  const HRESULT input_hr = video_device_->CreateVideoProcessorInputView(source, enumerator_.Get(), &in_desc, &input_view);
  const HRESULT output_hr = SUCCEEDED(input_hr) ? video_device_->CreateVideoProcessorOutputView(output.Get(), enumerator_.Get(), &ov_desc, &output_view) : E_ABORT;
  if (FAILED(input_hr) || FAILED(output_hr)) throw std::runtime_error("cannot create video processor views (input HRESULT=" + std::to_string(static_cast<unsigned long>(input_hr)) + ", output HRESULT=" + std::to_string(static_cast<unsigned long>(output_hr)) + ")");
  D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input_view.Get(); RECT rect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
  video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &rect); video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &rect); video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &rect);
  if (FAILED(video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(), 0, 1, &stream))) throw std::runtime_error("BGRA to NV12 conversion failed"); return output;
}
void VideoProcessor::reset() { processor_.Reset(); enumerator_.Reset(); video_context_.Reset(); video_device_.Reset(); width_ = height_ = 0; }
}
