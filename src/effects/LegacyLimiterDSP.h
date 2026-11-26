// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LegacyLimiter {

enum class Mode { SoftLimit, HardLimit, SoftClip, HardClip };

inline float LinearGain(double db)
{
   return static_cast<float>(std::pow(10.0, db / 20.0));
}

inline float SoftClip(float sample, float threshold)
{
   // The central line and sine shoulders of the Nyquist soft-clip table.
   constexpr double pi = 3.14159265358979323846;
   constexpr double knee = 1.0 - 1.0 / pi;
   constexpr double scale = 2.0 / pi;
   constexpr double peak = (0.5 + knee) * scale;
   const auto x = std::abs(sample) * peak / threshold;
   if (x <= 1.0 / pi)
      return sample;
   if (x >= 1.0)
      return std::copysign(threshold, sample);
   const auto shape = scale * (0.5 + knee * std::sin((x - 1.0 / pi) / (scale * knee)));
   return std::copysign(static_cast<float>(threshold * shape / peak), sample);
}

struct Shape {
   Mode mode;
   std::size_t hardHop;
   std::size_t softHop;
   std::size_t hardPadding;

   static Shape From(Mode mode, double rate, double holdMs)
   {
      const auto samples = rate * std::clamp(holdMs, 1.0, 50.0) / 3000;
      return {mode, static_cast<std::size_t>(std::max(1L, std::lround(samples))),
         static_cast<std::size_t>(std::max(1.0, std::floor(samples))),
         static_cast<std::size_t>(std::max(1L, std::lround(3 * samples)))};
   }

   std::size_t Latency() const
   {
      switch (mode) {
      case Mode::SoftLimit: return 3 * (hardHop + softHop);
      case Mode::HardLimit: return 3 * hardHop;
      default: return 0;
      }
   }

   bool operator==(const Shape &other) const
   {
      return mode == other.mode && hardHop == other.hardHop &&
         softHop == other.softHop && hardPadding == other.hardPadding;
   }
};

class PeakEnvelope
{
public:
   explicit PeakEnvelope(std::size_t maximumHop) : mAudio(3 * maximumHop + 1) {}

   void Reset(std::size_t hop, std::size_t padding)
   {
      mHop = hop;
      mPadding = padding;
      mIndex = 0;
      mPeak = mStartupPeak = 0;
      mPeaks.fill(0);
      mControls.fill(0);
   }

   float Push(float sample, float threshold, bool soft)
   {
      mAudio[mIndex % mAudio.size()] = sample;
      mPeak = std::max(mPeak, std::abs(sample));
      const auto hopIndex = mIndex / mHop;
      if (mIndex % mHop == mHop - 1) {
         mPeaks[hopIndex % mPeaks.size()] = mPeak;
         mPeak = 0;
         if (hopIndex == (soft ? 1u : 2u))
            mStartupPeak = *std::max_element(mPeaks.begin(), mPeaks.end());
         if (hopIndex >= 3)
            mControls[hopIndex % mControls.size()] =
               *std::max_element(mPeaks.begin(), mPeaks.end());
      }

      const auto latency = 3 * mHop;
      if (mIndex++ < latency)
         return 0;
      const auto output = mIndex - 1 - latency;
      const auto delayed = mAudio[output % mAudio.size()];
      if (output < mPadding) {
         const auto gain = soft ? std::min(0.9f, threshold * (2.0f - mStartupPeak))
                                : threshold / std::max(threshold, mStartupPeak);
         return delayed * gain;
      }
      const auto position = output - mPadding;
      const auto control = 3 + position / mHop;
      const auto blend = static_cast<float>(position % mHop) / mHop;
      const auto first = mControls[control % mControls.size()];
      const auto second = mControls[(control + 1) % mControls.size()];
      // Nyquist interpolates the peak envelope before converting it to
      // hard-limit gain, but interpolates gain in the soft-limit stage.
      if (!soft)
         return delayed * threshold / std::max(threshold, first + blend * (second - first));
      const auto firstGain = std::min(1.0f, threshold * (2.0f - first));
      const auto secondGain = std::min(1.0f, threshold * (2.0f - second));
      return delayed * (firstGain + blend * (secondGain - firstGain));
   }

private:
   std::vector<float> mAudio;
   std::size_t mHop{}, mPadding{};
   std::uint64_t mIndex{};
   std::array<float, 4> mPeaks{}, mControls{};
   float mPeak{}, mStartupPeak{};
};

class Processor
{
public:
   Processor(std::size_t maximumHop, std::size_t latency)
      : mHard{maximumHop}, mSoft{maximumHop}, mDelay(latency + 1), mLatency{latency} {}

   void Reset(Shape shape)
   {
      mShape = shape;
      mHard.Reset(shape.hardHop, shape.hardPadding);
      mSoft.Reset(shape.softHop, 3 * shape.softHop);
      mCount = 0;
   }

   float Push(float sample, float threshold)
   {
      float result{};
      switch (mShape.mode) {
      case Mode::SoftLimit:
         result = mHard.Push(sample, 1.0f, false);
         if (mCount >= 3 * mShape.hardHop)
            result = std::clamp(mSoft.Push(result, threshold, true), -threshold, threshold);
         break;
      case Mode::HardLimit:
         result = mHard.Push(sample, threshold, false);
         break;
      case Mode::SoftClip:
         result = SoftClip(sample, threshold);
         break;
      case Mode::HardClip:
         result = std::clamp(sample, -threshold, threshold);
         break;
      }

      const auto padding = mLatency - mShape.Latency();
      const auto index = mCount++;
      mDelay[index % mDelay.size()] = result;
      return index < padding ? 0.0f : mDelay[(index - padding) % mDelay.size()];
   }

private:
   PeakEnvelope mHard, mSoft;
   std::vector<float> mDelay;
   std::size_t mLatency;
   Shape mShape{};
   std::uint64_t mCount{};
};

// Realtime changes to Type and Hold warm up a second preallocated processor
// before crossfading. Every mode has the same host-visible latency.
class Channel
{
public:
   Channel(double rate, Shape initial, bool realtime)
      : mLatency{realtime ? Maximum(rate).Latency() : initial.Latency()}
      , mProcessors{Processor{Maximum(rate).hardHop, mLatency},
                   Processor{Maximum(rate).hardHop, mLatency}}
      , mDry(mLatency + 1)
      , mFadeLength{static_cast<std::size_t>(std::max(1.0, std::round(rate * 0.005)))}
      , mActiveShape{initial}, mPendingShape{initial}
   {
      Reset(initial);
   }

   static Shape Maximum(double rate) { return Shape::From(Mode::SoftLimit, rate, 50); }
   std::size_t Latency() const { return mLatency; }

   void Reset(Shape shape)
   {
      mActive = 0;
      mActiveShape = shape;
      mProcessors[0].Reset(shape);
      mTransition = false;
      mDryCount = 0;
   }

   void Process(Shape shape, const float *input, float *output, std::size_t count,
      float gain, float threshold, bool makeup, bool bypass = false)
   {
      if (!mTransition && shape.Latency() <= mLatency && !(shape == mActiveShape)) {
         mPendingShape = shape;
         mProcessors[1 - mActive].Reset(shape);
         mTransition = true;
         mTransitionPosition = 0;
      }
      const auto outputGain = makeup ? 0.999f / threshold : 1.0f;
      for (std::size_t i = 0; i < count; ++i) {
         const auto sample = input[i] * gain;
         auto result = mProcessors[mActive].Push(sample, threshold);
         if (mTransition) {
            const auto next = mProcessors[1 - mActive].Push(sample, threshold);
            if (mTransitionPosition >= mLatency) {
               const auto blend = static_cast<float>(mTransitionPosition - mLatency + 1) /
                  mFadeLength;
               result += blend * (next - result);
            }
            if (++mTransitionPosition == mLatency + mFadeLength) {
               mTransition = false;
               mActive = 1 - mActive;
               mActiveShape = mPendingShape;
            }
         }
         if (mLatency) {
            const auto index = mDryCount++;
            mDry[index % mDry.size()] = input[i];
            const auto dry = index < mLatency ? 0.0f : mDry[(index - mLatency) % mDry.size()];
            const auto step = 1.0f / mFadeLength;
            mWet = bypass ? std::max(0.0f, mWet - step) : std::min(1.0f, mWet + step);
            output[i] = dry + mWet * (result * outputGain - dry);
         }
         else
            output[i] = bypass ? input[i] : result * outputGain;
      }
   }

private:
   std::size_t mLatency;
   std::array<Processor, 2> mProcessors;
   std::vector<float> mDry;
   std::uint64_t mDryCount{};
   float mWet{1.0f};
   std::size_t mFadeLength;
   Shape mActiveShape, mPendingShape;
   std::size_t mActive{}, mTransitionPosition{};
   bool mTransition{};
};

} // namespace LegacyLimiter
