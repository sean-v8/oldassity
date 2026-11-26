/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OperationsToolBar.h"

#include "AudacityException.h"
#include "MixAndRender.h"
#include "ProjectHistory.h"
#include "ProjectRate.h"
#include "QualitySettings.h"
#include "TimeTrack.h"
#include "ToolManager.h"
#include "ToolDock.h"
#include "Track.h"
#include "WaveTrack.h"

#include "../CommonCommandFlags.h"
#include "../ProjectWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/stattext.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto renderBudget = std::chrono::milliseconds{12};
constexpr int nextSliceMs = 1;
constexpr int blockedPollMs = 100;
constexpr int progressRange = 10000;
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
   Clock::duration processingTime{};
   Clock::time_point previousSliceEnd{};
   Clock::time_point lastDisplay{};
   Clock::time_point started{};
   Clock::time_point finished{};
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
   : ToolBar(project, XO("Operations"), ID(), true)
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
      wxDefaultPosition, wxSize(100, -1), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
   mProgress = safenew wxGauge(this, wxID_ANY, progressRange, wxDefaultPosition,
      wxSize(32, 12));
   mProgress->SetName(XO("Operation progress").Translation());
   mCancel = safenew wxButton(this, wxID_ANY, XO("Cancel").Translation(),
      wxDefaultPosition, wxSize(-1, toolbarSingle), wxBU_EXACTFIT);
   mCancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Cancel(); });

   Add(mStatus, 2, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   Add(mProgress, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   Add(mCancel, 0, wxALIGN_CENTER_VERTICAL);

   mStatus->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &) {
      if (auto *dock = GetDock()) {
         const auto width = dock->GetClientSize().x - GetPosition().x - 3;
         if (width >= GetMinToolbarWidth()) {
            SetSize(width, toolbarSingle);
            Layout();
            Updated();
         }
      }
   });
   UpdateDisplay();
}

int OperationsToolBar::GetMinToolbarWidth()
{
   // Leave room for the grabber, resize grip and padding in the base toolbar.
   return 160 + (mCancel ? mCancel->GetBestSize().x : 0);
}

void OperationsToolBar::EnableDisableButtons()
{
   if (mCancel)
      mCancel->Enable(IsRunning());
}

void OperationsToolBar::UpdateDisplay()
{
   auto label = !mRender ? XO("No active operation") :
      mRender->complete ? XO("Ready to add mix") : XO("Mix and Render");
   const auto formatTime = [](long long seconds) {
      if (seconds >= 3600)
         return wxString::Format("%lld:%02lld:%02lld",
            seconds / 3600, seconds / 60 % 60, seconds % 60);
      return wxString::Format("%lld:%02lld", seconds / 60, seconds % 60);
   };
   wxString elapsedText;
   if (mRender) {
      const auto end = mRender->complete ? mRender->finished : Clock::now();
      elapsedText = formatTime(std::chrono::duration_cast<std::chrono::seconds>(
         end - mRender->started).count());
      label = XO("%s · %s elapsed").Format(label.Translation(), elapsedText);
   }
   if (mRender && !mRender->complete) {
      const auto duration = mRender->job->Duration();
      const auto elapsed = mRender->job->Elapsed();
      const auto time = std::chrono::duration<double>(mRender->processingTime).count();
      if (elapsed > 0 && duration > elapsed && time >= 0.5) {
         const auto remaining = std::ceil(time * (duration - elapsed) / elapsed);
         if (remaining >= 0 && remaining <= std::numeric_limits<int>::max()) {
            const auto seconds = static_cast<int>(remaining);
            label = XO("Mix and Render · %s elapsed · %s left").Format(
               elapsedText, formatTime(seconds));
         }
      }
   }
   mStatus->SetLabel(label.Translation());
   mStatus->SetToolTip(XO("%s\nDouble-click to widen to the dock edge.")
      .Format(label).Translation());
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

   render->started = Clock::now();
   mRender = std::move(render);
   UpdateDisplay();
   ToolManager::Get(mProject).Expose(ID(), true);
   mTimer.StartOnce(nextSliceMs);
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
      mRender->previousSliceEnd = {};
      mTimer.StartOnce(blockedPollMs);
      return;
   }

   GuardedCall<void>([&] {
      auto &render = *mRender;
      if (!render.complete) {
         const auto start = Clock::now();
         const auto deadline = start + renderBudget;
         do {
            render.complete = !render.job->ProcessNext(samplesPerStep);
         } while (!render.complete && Clock::now() < deadline);

         const auto now = Clock::now();
         const auto previousEnd = render.previousSliceEnd;
         render.processingTime += now - (previousEnd == Clock::time_point{} ? start : previousEnd);
         render.previousSliceEnd = now;
         if (render.complete)
            render.finished = now;
         const auto duration = render.job->Duration();
         const auto fraction = duration > 0.0
            ? std::clamp(render.job->Elapsed() / duration, 0.0, 1.0) : 0.0;
         if (render.complete || now - render.lastDisplay >= std::chrono::milliseconds{100}) {
            render.progress = render.complete ? progressRange :
               static_cast<int>(progressRange * fraction);
            render.lastDisplay = now;
            UpdateDisplay();
         }
      }

      if (!render.complete || AudioIOBusyPred(mProject)) {
         mTimer.StartOnce(render.complete ? blockedPollMs : nextSliceMs);
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
