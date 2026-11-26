// SPDX-License-Identifier: GPL-2.0-or-later
#include "LegacyLimiterDSP.h"

#include "EffectEditor.h"
#include "LoadEffects.h"
#include "ShuttleAutomation.h"
#include "ShuttleGui.h"
#include "StatelessPerTrackEffect.h"
#include "../widgets/valnum.h"

#include <optional>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/valgen.h>

namespace {

using LegacyLimiter::Channel;
using LegacyLimiter::Mode;
using LegacyLimiter::Shape;

const EnumValueSymbol modes[] = {
   { wxT("SoftLimit"), XO("Soft Limit") },
   { wxT("HardLimit"), XO("Hard Limit") },
   { wxT("SoftClip"), XO("Soft Clip") },
   { wxT("HardClip"), XO("Hard Clip") },
};

struct LimiterSettings
{
   int mode{};
   double leftGain{}, rightGain{}, threshold{-3}, hold{10};
   bool makeup{};

   Shape GetShape(double rate) const
   {
      return Shape::From(static_cast<Mode>(mode), rate, hold);
   }
};

class EffectLegacyLimiter final
   : public EffectWithSettings<LimiterSettings, StatelessPerTrackEffect>
{
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   {
      return XO("Legacy peak limiting and soft or hard clipping");
   }
   ManualPageID ManualPage() const override { return L"Limiter"; }
   EffectType GetType() const override { return EffectTypeProcess; }
   RealtimeSince RealtimeSupport() const override { return RealtimeSince::After_3_1; }

   std::shared_ptr<EffectInstance> MakeInstance() const override;
   std::unique_ptr<EffectEditor> MakeEditor(ShuttleGui &gui, EffectInstance &,
      EffectSettingsAccess &access, const EffectOutputs *) const override;

private:
   struct Editor;
   struct Instance;

   static constexpr EnumParameter Type{
      &LimiterSettings::mode, L"Type", 0, 0, 3, 1, modes, 4 };
   static constexpr EffectParameter LeftGain{
      &LimiterSettings::leftGain, L"Input Gain Left", 0.0, 0.0, 10.0, 1 };
   static constexpr EffectParameter RightGain{
      &LimiterSettings::rightGain, L"Input Gain Right", 0.0, 0.0, 10.0, 1 };
   static constexpr EffectParameter Threshold{
      &LimiterSettings::threshold, L"Limit to", -3.0, -10.0, 0.0, 1 };
   static constexpr EffectParameter Hold{
      &LimiterSettings::hold, L"Hold", 10.0, 1.0, 50.0, 1 };
   static constexpr EffectParameter Makeup{
      &LimiterSettings::makeup, L"Apply Make-up Gain", false, false, true, 1 };

   const EffectParameterMethods &Parameters() const override
   {
      static CapturedParameters<EffectLegacyLimiter,
         Type, LeftGain, RightGain, Threshold, Hold, Makeup> parameters;
      return parameters;
   }
};

// Give the port its own plug-in identity so existing Nyquist presets and
// macros continue to find the original offline Limiter.
const ComponentInterfaceSymbol EffectLegacyLimiter::Symbol{ XO("Limiter (Realtime)") };
BuiltinEffectsModule::Registration<EffectLegacyLimiter> reg;

struct EffectLegacyLimiter::Instance final
   : PerTrackEffect::Instance, EffectInstanceWithBlockSize
{
   explicit Instance(const EffectLegacyLimiter &effect) : PerTrackEffect::Instance{effect} {}

   struct StereoProcessor {
      StereoProcessor(const LimiterSettings &settings, double rate, bool realtime)
         : rate{rate}, channels{Channel{rate, settings.GetShape(rate), realtime},
                                Channel{rate, settings.GetShape(rate), realtime}} {}

      void Process(const LimiterSettings &settings, const float *const *in,
         float *const *out, size_t length, bool bypass = false)
      {
         const auto shape = settings.GetShape(rate);
         const auto threshold = LegacyLimiter::LinearGain(settings.threshold);
         const auto left = LegacyLimiter::LinearGain(settings.leftGain);
         const auto right = LegacyLimiter::LinearGain(settings.rightGain);
         channels[0].Process(shape, in[0], out[0], length, left, threshold, settings.makeup, bypass);
         channels[1].Process(shape, in[1], out[1], length, right, threshold, settings.makeup, bypass);
      }

      double rate;
      std::array<Channel, 2> channels;
   };

   unsigned GetAudioInCount() const override { return 2; }
   unsigned GetAudioOutCount() const override { return 2; }

   bool ProcessInitialize(EffectSettings &settings, double rate, ChannelNames) override
   {
      offline.emplace(GetSettings(settings), rate, false);
      return true;
   }

   bool ProcessFinalize() noexcept override
   {
      offline.reset();
      return true;
   }

   size_t ProcessBlock(EffectSettings &settings, const float *const *in,
      float *const *out, size_t length) override
   {
      offline->Process(GetSettings(settings), in, out, length);
      return length;
   }

   bool RealtimeInitialize(EffectSettings &settings, double) override
   {
      SetBlockSize(512);
      realtime.clear();
      bypassed = !settings.extra.GetActive();
      return true;
   }

   bool RealtimeAddProcessor(EffectSettings &settings, EffectOutputs *, unsigned,
      float rate) override
   {
      realtime.emplace_back(GetSettings(settings), rate, true);
      return true;
   }

   bool RealtimeFinalize(EffectSettings &) noexcept override
   {
      realtime.clear();
      return true;
   }

   size_t RealtimeProcess(size_t group, EffectSettings &settings,
      const float *const *in, float *const *out, size_t length) override
   {
      if (group >= realtime.size())
         return 0;
      realtime[group].Process(GetSettings(settings), in, out, length, bypassed);
      return length;
   }

   size_t GetRealtimeLookahead(double rate) const override
   {
      return Channel::Maximum(rate).Latency();
   }

   void RealtimeReset(const EffectSettings &settings) noexcept override
   {
      for (auto &group : realtime)
         for (auto &channel : group.channels)
            channel.Reset(GetSettings(settings).GetShape(group.rate));
   }

   bool RealtimeSuspend() override { bypassed = true; return true; }
   bool RealtimeResume() override { bypassed = false; return true; }

   SampleCount GetLatency(const EffectSettings &settings, double rate) const override
   {
      return realtime.empty() ? GetSettings(settings).GetShape(rate).Latency()
                              : Channel::Maximum(rate).Latency();
   }

   std::optional<StereoProcessor> offline;
   std::vector<StereoProcessor> realtime;
   bool bypassed{};
};

struct EffectLegacyLimiter::Editor final : EffectEditor
{
   Editor(const EffectLegacyLimiter &effect, EffectSettingsAccess &access, ShuttleGui &gui)
      : EffectEditor{effect, access}, parent{gui.GetParent()}, settings{GetSettings(access.Get())}
   {
      gui.StartMultiColumn(2, wxEXPAND);
      auto *type = gui.Validator<wxGenericValidator>(&settings.mode)
         .AddChoice(XXO("Type"), Msgids(modes, 4));
      BindTo(*type, wxEVT_CHOICE, &Editor::OnChange);
      AddNumber(gui, XXO("Input Gain (dB) mono/Left"), settings.leftGain, LeftGain);
      AddNumber(gui, XXO("Input Gain (dB) Right channel"), settings.rightGain, RightGain);
      AddNumber(gui, XXO("Limit to (dB)"), settings.threshold, Threshold);
      AddNumber(gui, XXO("Hold (ms)"), settings.hold, Hold);
      auto *makeup = gui.Validator<wxGenericValidator>(&settings.makeup)
         .AddCheckBox(XXO("Apply Make-up Gain"), settings.makeup);
      BindTo(*makeup, wxEVT_CHECKBOX, &Editor::OnChange);
      gui.EndMultiColumn();
   }

   bool ValidateUI() override
   {
      if (!parent->TransferDataFromWindow())
         return false;
      mAccess.ModifySettings([this](EffectSettings &target) {
         GetSettings(target) = settings;
         return nullptr;
      });
      return true;
   }

   bool UpdateUI() override
   {
      const auto guard = valueRestorer(updating, true);
      settings = GetSettings(mAccess.Get());
      return parent->TransferDataToWindow();
   }

   void OnChange(wxCommandEvent &)
   {
      if (!updating && ValidateUI())
         Publish(EffectSettingChanged{});
   }

   template<typename Parameter>
   void AddNumber(ShuttleGui &gui, const TranslatableString &name, double &value,
      const Parameter &parameter)
   {
      auto *text = gui.Validator<FloatingPointValidator<double>>(
         3, &value, NumValidatorStyle::DEFAULT, parameter.min, parameter.max)
         .AddTextBox(name, wxString{}, 8);
      BindTo(*text, wxEVT_TEXT, &Editor::OnChange);
   }

   wxWindow *parent;
   LimiterSettings settings;
   bool updating{};
};

std::shared_ptr<EffectInstance> EffectLegacyLimiter::MakeInstance() const
{
   return std::make_shared<Instance>(*this);
}

std::unique_ptr<EffectEditor> EffectLegacyLimiter::MakeEditor(ShuttleGui &gui,
   EffectInstance &, EffectSettingsAccess &access, const EffectOutputs *) const
{
   return std::make_unique<Editor>(*this, access, gui);
}

} // namespace
