/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OperationsToolBar.h"

#include "AudacityException.h"
#include "MixAndRender.h"
#include "ProjectHistory.h"
#include "ProjectRate.h"
#include "QualitySettings.h"
#include "TimeTrack.h"
#include "ToolManager.h"
#include "Track.h"
#include "WaveTrack.h"

#include "../CommonCommandFlags.h"
#include "../ProjectWindow.h"

#include <algorithm>
#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/stattext.h>

namespace {
constexpr int renderDelayMs = 5;
constexpr size_t samplesPerStep = 16384;
}

struct OperationsToolBar::Render {
   // The mixer borrows tracks and the time-warp envelope. Keep private copies
   // alive until after the mixer and its detached output have been destroyed.
   TrackListHolder sources{ TrackList::Create(nullptr) };
   TrackListHolder timeTrack;
   std::unique_ptr<MixRenderJob> job;
   size_t sourceCount{};
   int waveColor{};
   int progress{};
   bool complete{};
};

Identifier OperationsToolBar::ID()
{
   return wxT("Operations");
}

OperationsToolBar *OperationsToolBar::Get(AudacityProject &project)
{
   return static_cast<OperationsToolBar *>(ToolManager::Get(project).GetToolBar(ID()));
}

OperationsToolBar::OperationsToolBar(AudacityProject &project)
   : ToolBar(project, XO("Operations"), ID())
{
   Bind(wxEVT_TIMER, &OperationsToolBar::OnTimer, this, mTimer.GetId());
}

OperationsToolBar::~OperationsToolBar()
{
   mTimer.Stop();
}

void OperationsToolBar::Populate()
{
   mStatus = safenew wxStaticText(this, wxID_ANY, wxString{},
      wxDefaultPosition, wxSize(165, -1), wxST_ELLIPSIZE_END);
   mProgress = safenew wxGauge(this, wxID_ANY, 100, wxDefaultPosition,
      wxSize(82, -1));
   mProgress->SetName(XO("Operation progress").Translation());
   mCancel = safenew wxButton(this, wxID_ANY, XO("Cancel").Translation(),
      wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
   mCancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Cancel(); });

   Add(mStatus, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   Add(mProgress, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   Add(mCancel, 0, wxALIGN_CENTER_VERTICAL);
   UpdateDisplay();
}

void OperationsToolBar::EnableDisableButtons()
{
   if (mCancel)
      mCancel->Enable(IsRunning());
}

void OperationsToolBar::UpdateDisplay()
{
   const auto label = !mRender ? XO("No active operation") :
      mRender->complete ? XO("Ready to add mix") : XO("Mix and Render");
   mStatus->SetLabel(label.Translation());
   mProgress->SetValue(mRender ? mRender->progress : 0);
   EnableDisableButtons();
}

void OperationsToolBar::StartMixRender()
{
   if (mRender) {
      ToolManager::Get(mProject).Expose(ID(), true);
      return;
   }

   auto &tracks = TrackList::Get(mProject);
   auto selected = tracks.Selected<WaveTrack>();
   if (selected.empty())
      return;

   auto render = std::make_unique<Render>();
   render->sourceCount = selected.size();
   render->waveColor = (*selected.begin())->GetWaveColorIndex();
   for (const auto *track : selected)
      render->sources->Append(std::move(*track->Duplicate()));

   const BoundedEnvelope *warp = nullptr;
   if (const auto *time = *tracks.Any<const TimeTrack>().begin()) {
      render->timeTrack = time->Duplicate();
      warp = static_cast<const TimeTrack &>(**render->timeTrack->begin()).GetEnvelope();
   }

   render->job = MixRenderJob::Begin(
      render->sources->Any<const WaveTrack>(), Mixer::WarpOptions{ warp },
      tracks.MakeUniqueTrackName(XO("Mix").Translation()),
      WaveTrackFactory::Get(mProject), ProjectRate::Get(mProject).GetRate(),
      QualitySettings::SampleFormatChoice(), 0.0, 0.0);
   if (!render->job)
      return;

   mRender = std::move(render);
   UpdateDisplay();
   ToolManager::Get(mProject).Expose(ID(), true);
   mTimer.StartOnce(renderDelayMs);
}

void OperationsToolBar::Cancel()
{
   mTimer.Stop();
   if (mRender) {
      mRender.reset();
      UpdateDisplay();
   }
}

void OperationsToolBar::OnTimer(wxTimerEvent &)
{
   if (!mRender)
      return;

   auto &window = ProjectWindow::Get(mProject);
   if (window.IsBeingDeleted()) {
      Cancel();
      return;
   }
   // An existing modal operation may yield timer events mid-command.
   if (!window.IsEnabled() || mProject.mbBusyImporting) {
      mTimer.StartOnce(50);
      return;
   }

   GuardedCall<void>([&] {
      auto &render = *mRender;
      if (!render.complete) {
         render.complete = !render.job->ProcessNext(samplesPerStep);
         const auto duration = render.job->Duration();
         render.progress = render.complete ? 100 : duration > 0.0 ?
            static_cast<int>(100 * std::clamp(render.job->Elapsed() / duration, 0.0, 1.0)) : 0;
         UpdateDisplay();
      }

      if (!render.complete || AudioIOBusyPred(mProject)) {
         mTimer.StartOnce(render.complete ? 50 : renderDelayMs);
         return;
      }

      // Append only after processing and flushing succeed, on the UI thread.
      const auto count = render.sourceCount;
      const auto color = render.waveColor;
      auto result = render.job->Finish();
      Cancel();

      auto &tracks = TrackList::Get(mProject);
      auto &mix = static_cast<WaveTrack &>(**result->begin());
      const bool stereo = mix.NChannels() > 1;
      mix.SetSelected(false);
      mix.SetWaveColorIndex(color);
      if (count > 1)
         mix.SetName(tracks.MakeUniqueTrackName(XO("Mix").Translation()));
      const auto description = count == 1
         ? XO("Rendered all audio in track '%s'").Format(mix.GetName())
         : (stereo ? XO("Mixed and rendered %d tracks into one new stereo track")
                   : XO("Mixed and rendered %d tracks into one new mono track"))
              .Format(static_cast<int>(count));
      tracks.Append(std::move(*result));
      try {
         ProjectHistory::Get(mProject).PushState(description, XO("Mix and Render"));
      }
      catch (...) {
         tracks.Remove(mix);
         throw;
      }
   }, [this](AudacityException *) { Cancel(); });
}

namespace {
RegisteredToolbarFactory factory{
   [](AudacityProject &project) {
      return ToolBar::Holder{ safenew OperationsToolBar{ project } };
   }
};

AttachedToolBarMenuItem menuItem{
   OperationsToolBar::ID(), wxT("ShowOperationsTB"), XXO("&Operations Toolbar"),
   { Registry::OrderingHint::After, "ShowTimeTB" }
};
}
