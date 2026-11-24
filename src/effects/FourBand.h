/**********************************************************************

   Audacity: A Digital Audio Editor
   Audacity(R) is copyright (c) 1999-2016 Audacity Team.
   License: GPL v2 or later.  See License.txt.

   FourBand.h
   Based on BassTreble by Steve Daulton.

**********************************************************************/

#ifndef __AUDACITY_EFFECT_FOUR_BAND__
#define __AUDACITY_EFFECT_FOUR_BAND__

#include "StatelessPerTrackEffect.h"
#include "ShuttleAutomation.h"

struct EffectFourBandSettings
{
   double mBass { 0.0 };
   double mLowMid { 0.0 };
   double mHighMid { 0.0 };
   double mTreble { 0.0 };
   double mGain { 0.0 };
   bool mLink { false };
   double mBassHz { 250.0 };
   double mLowMidHz { 500.0 };
   double mHighMidHz { 2000.0 };
   double mTrebleHz { 4000.0 };
   double mBassSlope { 0.4 };
   double mLowMidQ { 1.0 };
   double mHighMidQ { 1.0 };
   double mTrebleSlope { 0.4 };
};

class EffectFourBand final : public EffectWithSettings<
   EffectFourBandSettings, StatelessPerTrackEffect>
{
public:
   static const ComponentInterfaceSymbol Symbol;

   EffectFourBand();

   ComponentInterfaceSymbol GetSymbol() const override;
   TranslatableString GetDescription() const override;
   EffectType GetType() const override;
   RealtimeSince RealtimeSupport() const override;

   std::unique_ptr<EffectEditor> MakeEditor(
      ShuttleGui& S, EffectInstance& instance, EffectSettingsAccess& access,
      const EffectOutputs* outputs) const override;

   bool CheckWhetherSkipEffect(const EffectSettings& settings) const override;
   std::shared_ptr<EffectInstance> MakeInstance() const override;

private:
   struct Editor;
   struct Instance;

   const EffectParameterMethods& Parameters() const override;

   static constexpr EffectParameter Bass{
      &EffectFourBandSettings::mBass, L"Bass",
      EffectFourBandSettings{}.mBass, -60.0, 30.0, 1 };
   static constexpr EffectParameter LowMid{
      &EffectFourBandSettings::mLowMid, L"Low Mid",
      EffectFourBandSettings{}.mLowMid, -60.0, 30.0, 1 };
   static constexpr EffectParameter HighMid{
      &EffectFourBandSettings::mHighMid, L"High Mid",
      EffectFourBandSettings{}.mHighMid, -60.0, 30.0, 1 };
   static constexpr EffectParameter Treble{
      &EffectFourBandSettings::mTreble, L"Treble",
      EffectFourBandSettings{}.mTreble, -60.0, 30.0, 1 };
   static constexpr EffectParameter Gain{
      &EffectFourBandSettings::mGain, L"Gain",
      EffectFourBandSettings{}.mGain, -60.0, 30.0, 1 };
   static constexpr EffectParameter Link{
      &EffectFourBandSettings::mLink, L"Link Sliders",
      EffectFourBandSettings{}.mLink, false, true, 1 };
   static constexpr EffectParameter BassHz{
      &EffectFourBandSettings::mBassHz, L"Bass Frequency",
      EffectFourBandSettings{}.mBassHz, 10.0, 40000.0, 1 };
   static constexpr EffectParameter LowMidHz{
      &EffectFourBandSettings::mLowMidHz, L"Low Mid Frequency",
      EffectFourBandSettings{}.mLowMidHz, 10.0, 40000.0, 1 };
   static constexpr EffectParameter HighMidHz{
      &EffectFourBandSettings::mHighMidHz, L"High Mid Frequency",
      EffectFourBandSettings{}.mHighMidHz, 10.0, 40000.0, 1 };
   static constexpr EffectParameter TrebleHz{
      &EffectFourBandSettings::mTrebleHz, L"Treble Frequency",
      EffectFourBandSettings{}.mTrebleHz, 10.0, 40000.0, 1 };
   static constexpr EffectParameter BassSlope{
      &EffectFourBandSettings::mBassSlope, L"Bass Slope",
      EffectFourBandSettings{}.mBassSlope, 0.01, 1.0, 1 };
   static constexpr EffectParameter LowMidQ{
      &EffectFourBandSettings::mLowMidQ, L"Low Mid Q",
      EffectFourBandSettings{}.mLowMidQ, 0.1, 20.0, 1 };
   static constexpr EffectParameter HighMidQ{
      &EffectFourBandSettings::mHighMidQ, L"High Mid Q",
      EffectFourBandSettings{}.mHighMidQ, 0.1, 20.0, 1 };
   static constexpr EffectParameter TrebleSlope{
      &EffectFourBandSettings::mTrebleSlope, L"Treble Slope",
      EffectFourBandSettings{}.mTrebleSlope, 0.01, 1.0, 1 };
};

#endif
