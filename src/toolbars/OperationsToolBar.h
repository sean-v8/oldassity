/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ToolBar.h"

#include <memory>
#include <wx/timer.h>

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

   void StartMixRender();
   bool IsRunning() const { return mRender != nullptr; }
   void Cancel();

   bool ShownByDefault() const override { return false; }
   DockID DefaultDockID() const override { return BotDockID; }
   void Populate() override;
   void Repaint(wxDC *) override {}
   void EnableDisableButtons() override;
   void RegenerateTooltips() override {}

private:
   void OnTimer(wxTimerEvent &);
   void UpdateDisplay();

   struct Render;
   std::unique_ptr<Render> mRender;
   wxTimer mTimer{ this };
   wxStaticText *mStatus{};
   wxGauge *mProgress{};
   wxButton *mCancel{};
};
