/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ToolBar.h"
#include "Observer.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <wx/timer.h>

namespace BasicUI { class ProgressDialog; }

class wxButton;
class wxGauge;
class wxStaticText;

class OperationsToolBar final : public ToolBar
{
public:
   explicit OperationsToolBar(AudacityProject &project);
   ~OperationsToolBar() override;

   static Identifier ID();
   static OperationsToolBar *Get(AudacityProject &project);

   void StartMixRender(bool replaceSources = false);
   bool IsRunning() const { return mRender || mEffectProgress || !mRenderQueue.empty(); }
   std::unique_ptr<BasicUI::ProgressDialog> StartEffectProgress(
      const TranslatableString &title, const TranslatableString &message,
      unsigned flags);
   void Cancel();

   bool ShownByDefault() const override { return false; }
   DockID DefaultDockID() const override { return BotDockID; }
   void Populate() override;
   int GetInitialWidth() override { return 340; }
   int GetMinToolbarHeight() override { return toolbarTriple; }
   int GetMinToolbarWidth() override;
   wxSize GetDockedSize() override;
   void Repaint(wxDC *) override {}
   void EnableDisableButtons() override;
   void RegenerateTooltips() override {}

private:
   void OnTimer(wxTimerEvent &);
   void UpdateDisplay();
   bool CanServiceRender() const;
   void StartNextRender();
   void AdvanceRenderQueue();

   struct EffectProgress;
   EffectProgress *mEffectProgress{};
   struct Render;
   void ValidateRenderSources(const Render &render);
   void ShowRenderQueue();
   void UpdateQueueDisplay();
   void CancelQueuedRender(std::uint64_t id);
   struct QueueDialog;
   std::unique_ptr<QueueDialog> mQueueDialog;
   std::uint64_t mNextRenderId{};
   Observer::Subscription mUndoSubscription, mTrackSubscription;
   std::unique_ptr<Render> mRender;
   std::deque<std::unique_ptr<Render>> mRenderQueue;
   wxTimer mTimer{ this };
   wxStaticText *mStatus{};
   wxGauge *mProgress{};
   wxStaticText *mTime{};
   wxButton *mQueue{};
   wxButton *mCancel{};
   wxButton *mStop{};
};
