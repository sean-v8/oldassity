/**********************************************************************

   Audacity: A Digital Audio Editor
   Audacity(R) is copyright (c) 1999-2016 Audacity Team.
   License: GPL v2 or later.  See License.txt.

   FourBand.cpp
   Based on BassTreble by Steve Daulton.

**********************************************************************/

#include "FourBand.h"

#include "EffectEditor.h"
#include "LoadEffects.h"
#include "ShuttleGui.h"
#include "WaveTrack.h"
#include "wxPanelWrapper.h"
#include "../widgets/valnum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/weakref.h>

const EffectParameterMethods& EffectFourBand::Parameters() const
{
   static CapturedParameters<EffectFourBand,
      Bass, LowMid, HighMid, Treble, Gain, Link,
      BassHz, LowMidHz, HighMidHz, TrebleHz,
      BassSlope, LowMidQ, HighMidQ, TrebleSlope
   > parameters;
   return parameters;
}

const ComponentInterfaceSymbol EffectFourBand::Symbol{ XO("Four Band") };

namespace
{
BuiltinEffectsModule::Registration<EffectFourBand> reg;

enum class FilterType { LowShelf, Bell, HighShelf };

struct Band
{
   double hz;
   double shape;
   double gain;
};

struct Biquad
{
   void Configure(FilterType type, const Band& next, double sampleRate)
   {
      if (next.hz == band.hz && next.shape == band.shape &&
          next.gain == band.gain)
         return;

      band = next;
      active = band.gain != 0.0 && band.hz < sampleRate / 2.0;
      if (!active)
      {
         x1 = x2 = y1 = y2 = 0.0;
         return;
      }

      constexpr double twoPi = 6.28318530717958647693;
      const auto w = twoPi * band.hz / sampleRate;
      const auto c = std::cos(w);
      const auto s = std::sin(w);
      const auto a = std::pow(10.0, band.gain / 40.0);
      double a0;

      if (type == FilterType::Bell)
      {
         const auto alpha = s / (2.0 * band.shape);
         b0 = 1.0 + alpha * a;
         b1 = -2.0 * c;
         b2 = 1.0 - alpha * a;
         a0 = 1.0 + alpha / a;
         a1 = b1;
         a2 = 1.0 - alpha / a;
      }
      else
      {
         // Shelf slope S, with the same response as Bass and Treble.
         const auto b = s * std::sqrt(
            (a * a + 1.0) / band.shape - (a - 1.0) * (a - 1.0));
         const auto sum = a + 1.0;
         const auto diff = a - 1.0;
         if (type == FilterType::LowShelf)
         {
            b0 = a * (sum - diff * c + b);
            b1 = 2.0 * a * (diff - sum * c);
            b2 = a * (sum - diff * c - b);
            a0 = sum + diff * c + b;
            a1 = -2.0 * (diff + sum * c);
            a2 = sum + diff * c - b;
         }
         else
         {
            b0 = a * (sum + diff * c + b);
            b1 = -2.0 * a * (diff + sum * c);
            b2 = a * (sum + diff * c - b);
            a0 = sum - diff * c + b;
            a1 = 2.0 * (diff - sum * c);
            a2 = sum - diff * c - b;
         }
      }

      b0 /= a0;
      b1 /= a0;
      b2 /= a0;
      a1 /= a0;
      a2 /= a0;
   }

   double Process(double input)
   {
      if (!active)
         return input;

      const auto output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1;
      x1 = input;
      y2 = y1;
      y1 = output;
      return output;
   }

   Band band{};
   bool active = false;
   double b0{}, b1{}, b2{}, a1{}, a2{};
   double x1{}, x2{}, y1{}, y2{};
};

struct FilterState
{
   double sampleRate{};
   std::array<Biquad, 4> filters{};
};

size_t ProcessFilters(const EffectFourBandSettings& settings, FilterState& state,
   const float* input, float* output, size_t length)
{
   auto& filters = state.filters;
   filters[0].Configure(FilterType::LowShelf,
      { settings.mBassHz, settings.mBassSlope, settings.mBass }, state.sampleRate);
   filters[1].Configure(FilterType::Bell,
      { settings.mLowMidHz, settings.mLowMidQ, settings.mLowMid }, state.sampleRate);
   filters[2].Configure(FilterType::Bell,
      { settings.mHighMidHz, settings.mHighMidQ, settings.mHighMid }, state.sampleRate);
   filters[3].Configure(FilterType::HighShelf,
      { settings.mTrebleHz, settings.mTrebleSlope, settings.mTreble }, state.sampleRate);

   const auto volume = DB_TO_LINEAR(settings.mGain);
   for (size_t i = 0; i < length; ++i)
   {
      double sample = input[i];
      for (auto& filter : filters)
         sample = filter.Process(sample);
      output[i] = static_cast<float>(sample * volume);
   }
   return length;
}
}

struct EffectFourBand::Instance final
   : PerTrackEffect::Instance, EffectInstanceWithBlockSize
{
   explicit Instance(const PerTrackEffect& effect)
      : PerTrackEffect::Instance{ effect }
   {}

   bool ProcessInitialize(EffectSettings&, double sampleRate, ChannelNames) override
   {
      mOffline = FilterState{ sampleRate };
      return true;
   }

   size_t ProcessBlock(EffectSettings& settings, const float* const* input,
      float* const* output, size_t length) override
   {
      return ProcessFilters(GetSettings(settings), mOffline, input[0], output[0], length);
   }

   bool RealtimeInitialize(EffectSettings&, double) override
   {
      SetBlockSize(512);
      mRealtime.clear();
      return true;
   }

   bool RealtimeAddProcessor(EffectSettings&, EffectOutputs*, unsigned,
      float sampleRate) override
   {
      mRealtime.push_back(FilterState{ sampleRate });
      return true;
   }

   bool RealtimeFinalize(EffectSettings&) noexcept override
   {
      mRealtime.clear();
      return true;
   }

   size_t RealtimeProcess(size_t group, EffectSettings& settings,
      const float* const* input, float* const* output, size_t length) override
   {
      if (group >= mRealtime.size())
         return 0;

      return ProcessFilters(
         GetSettings(settings), mRealtime[group], input[0], output[0], length);
   }

   unsigned GetAudioInCount() const override { return 1; }
   unsigned GetAudioOutCount() const override { return 1; }

   FilterState mOffline;
   std::vector<FilterState> mRealtime;
};

struct EffectFourBand::Editor final : EffectEditor
{
   using Setting = double EffectFourBandSettings::*;

   Editor(const EffectUIServices& services, EffectSettingsAccess& access)
      : EffectEditor{ services, access }, mSettings{ GetSettings(access.Get()) }
   {}

   bool ValidateUI() override
   {
      if (!mParent->Validate() || !mParent->TransferDataFromWindow())
         return false;

      mAccess.ModifySettings([this](EffectSettings& settings) {
         GetSettings(settings) = mSettings;
         return nullptr;
      });
      return true;
   }

   bool UpdateUI() override
   {
      mSettings = GetSettings(mAccess.Get());
      for (auto& control : mControls)
      {
         UpdateText(control);
         UpdateSlider(control);
      }
      mLink->SetValue(mSettings.mLink);
      EnableApply(mParent, mParent->Validate());
      return true;
   }

   bool HasOptions() const override { return true; }

   void ShowOptions() override
   {
      if (!mParent->Validate() || !mParent->TransferDataFromWindow())
         return;

      auto options = mSettings;
      wxDialogWrapper dialog{ mParent, wxID_ANY, XO("Four Band Options") };
      ShuttleGui S{ &dialog, eIsCreating };

      const auto addField = [&S, &options](const auto& parameter,
                             const TranslatableString& label, int precision) {
         S.Validator<FloatingPointValidator<double>>(
            precision, &(options.*parameter.mem), NumValidatorStyle::DEFAULT,
            parameter.min, parameter.max).AddTextBox(label, L"", 10);
      };

      S.SetBorder(5);
      S.StartStatic(XO("Bass shelf"));
      S.StartMultiColumn(2, wxEXPAND);
      addField(BassHz, XO("Frequency (Hz):"), 2);
      addField(BassSlope, XO("Shelf slope (S):"), 3);
      S.EndMultiColumn();
      S.EndStatic();

      S.StartStatic(XO("Low mid bell"));
      S.StartMultiColumn(2, wxEXPAND);
      addField(LowMidHz, XO("Frequency (Hz):"), 2);
      addField(LowMidQ, XO("Width (Q):"), 3);
      S.EndMultiColumn();
      S.EndStatic();

      S.StartStatic(XO("High mid bell"));
      S.StartMultiColumn(2, wxEXPAND);
      addField(HighMidHz, XO("Frequency (Hz):"), 2);
      addField(HighMidQ, XO("Width (Q):"), 3);
      S.EndMultiColumn();
      S.EndStatic();

      S.StartStatic(XO("Treble shelf"));
      S.StartMultiColumn(2, wxEXPAND);
      addField(TrebleHz, XO("Frequency (Hz):"), 2);
      addField(TrebleSlope, XO("Shelf slope (S):"), 3);
      S.EndMultiColumn();
      S.EndStatic();

      S.AddStandardButtons(eOkButton | eCancelButton);
      dialog.Fit();
      dialog.CentreOnParent();

      if (dialog.ShowModal() == wxID_OK)
      {
         mSettings = options;
         if (ValidateUI())
            Publish(EffectSettingChanged{});
      }
   }

   void PopulateOrExchange(ShuttleGui& S)
   {
      mParent = S.GetParent();
      S.SetBorder(5);
      S.AddSpace(0, 5);

      S.StartStatic(XO("Tone controls"));
      S.StartMultiColumn(3, wxEXPAND);
      S.SetStretchyCol(2);
      AddControl(S, 0, Bass, XXO("Ba&ss (dB):"), XO("Bass"));
      AddControl(S, 1, LowMid, XXO("&Low Mid (dB):"), XO("Low Mid"));
      AddControl(S, 2, HighMid, XXO("&High Mid (dB):"), XO("High Mid"));
      AddControl(S, 3, Treble, XXO("&Treble (dB):"), XO("Treble"));
      S.EndMultiColumn();
      S.EndStatic();

      S.StartStatic(XO("Output"));
      S.StartMultiColumn(3, wxEXPAND);
      S.SetStretchyCol(2);
      AddControl(S, 4, Gain, XXO("&Volume (dB):"), XO("Volume"));
      S.EndMultiColumn();

      mLink = S.AddCheckBox(XXO("L&ink Volume control to Tone controls"), mSettings.mLink);
      mLink->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Commit(); });
      S.EndStatic();
   }

private:
   struct Control
   {
      Setting setting{};
      wxTextCtrl* text{};
      wxSlider* slider{};
   };

   void AddControl(ShuttleGui& S, size_t index, const decltype(Gain)& parameter,
      const TranslatableString& label, const TranslatableString& name)
   {
      auto& control = mControls[index];
      control.setting = parameter.mem;
      control.text = S.Validator<FloatingPointValidator<double>>(
         1, &(mSettings.*control.setting), NumValidatorStyle::DEFAULT,
         parameter.min, parameter.max)
         .AddTextBox(label, L"", 10);
      control.slider = S.Name(name).Style(wxSL_HORIZONTAL)
         .AddSlider({}, 0, parameter.max, parameter.min);

      control.text->Bind(wxEVT_TEXT, [this, index](wxCommandEvent&) {
         auto& changed = mControls[index];
         const auto oldGain = mSettings.*changed.setting;
         if (!EnableApply(mParent, mParent->TransferDataFromWindow()))
            return;

         UpdateSlider(changed);
         LinkVolume(changed.setting, oldGain);
         Commit();
      });

      control.slider->Bind(wxEVT_SLIDER, [this, index](wxCommandEvent& event) {
         auto& changed = mControls[index];
         const auto oldGain = mSettings.*changed.setting;
         mSettings.*changed.setting = event.GetInt();
         UpdateText(changed);
         if (mParent->Validate())
            LinkVolume(changed.setting, oldGain);
         Commit();
      });
   }

   void UpdateText(const Control& control)
   {
      wxEventBlocker blocker{ control.text, wxEVT_TEXT };
      control.text->GetValidator()->TransferToWindow();
   }

   void UpdateSlider(const Control& control)
   {
      control.slider->SetValue(static_cast<int>(mSettings.*control.setting));
   }

   void LinkVolume(Setting setting, double oldGain)
   {
      if (!mLink->GetValue() || setting == &EffectFourBandSettings::mGain)
         return;

      const auto estimate = [](double gain) { return gain > 0.0 ? gain / 2.0 : gain / 4.0; };
      const auto change = estimate(mSettings.*setting) - estimate(oldGain);
      mSettings.mGain = std::clamp(mSettings.mGain - change, Gain.min, Gain.max);
      UpdateText(mControls.back());
      UpdateSlider(mControls.back());
   }

   void Commit()
   {
      mSettings.mLink = mLink->GetValue();
      if (EnableApply(mParent, ValidateUI()))
         Publish(EffectSettingChanged{});
   }

   wxWeakRef<wxWindow> mParent;
   EffectFourBandSettings mSettings;
   std::array<Control, 5> mControls;
   wxCheckBox* mLink{};
};

EffectFourBand::EffectFourBand()
{
   SetLinearEffectFlag(true);
}

ComponentInterfaceSymbol EffectFourBand::GetSymbol() const
{
   return Symbol;
}

TranslatableString EffectFourBand::GetDescription() const
{
   return XO("Four-band equalizer with two shelves and two bells");
}

EffectType EffectFourBand::GetType() const
{
   return EffectTypeProcess;
}

auto EffectFourBand::RealtimeSupport() const -> RealtimeSince
{
   return RealtimeSince::After_3_1;
}

bool EffectFourBand::CheckWhetherSkipEffect(const EffectSettings& settings) const
{
   const auto& values = GetSettings(settings);
   return values.mBass == 0.0 && values.mLowMid == 0.0 &&
      values.mHighMid == 0.0 && values.mTreble == 0.0 && values.mGain == 0.0;
}

std::shared_ptr<EffectInstance> EffectFourBand::MakeInstance() const
{
   return std::make_shared<Instance>(*this);
}

std::unique_ptr<EffectEditor> EffectFourBand::MakeEditor(
   ShuttleGui& S, EffectInstance&, EffectSettingsAccess& access,
   const EffectOutputs*) const
{
   auto result = std::make_unique<Editor>(*this, access);
   result->PopulateOrExchange(S);
   return result;
}
