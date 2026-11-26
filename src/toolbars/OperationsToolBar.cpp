/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OperationsToolBar.h"

#include "AudacityException.h"
#include "EffectBase.h"
#include "MixAndRender.h"
#include "ProjectHistory.h"
#include "ProjectRate.h"
#include "QualitySettings.h"
#include "Sequence.h"
#include "TimeTrack.h"
#include "ToolManager.h"
#include "ToolDock.h"
#include "Track.h"
#include "WaveTrack.h"
#include "XMLWriter.h"

#include "../CommonCommandFlags.h"
#include "../ProjectWindow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <wx/button.h>
#include <wx/evtloop.h>
#include <wx/eventfilter.h>
#include <wx/utils.h>
#include <wx/gauge.h>
#include <wx/stattext.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto renderBudget = std::chrono::milliseconds{12};
constexpr int nextSliceMs = 1;
constexpr int blockedPollMs = 100;
constexpr int progressRange = 10000;
constexpr size_t samplesPerStep = 16384;
constexpr size_t workerSamplesPerStep = 65536;

wxString FormatTime(long long seconds)
{
   if (seconds >= 3600)
      return wxString::Format("%lld:%02lld:%02lld",
         seconds / 3600, seconds / 60 % 60, seconds % 60);
   return wxString::Format("%lld:%02lld", seconds / 60, seconds % 60);
}

// Compare persisted source state while ignoring the selection changes
// that come with editing another track. This never reads the sample data.
class RenderSourceState final : public XMLWriter
{
public:
   using XMLWriter::WriteAttr;

   void WriteAttr(const wxString &name, bool value) override
   {
      if (name != wxT("isSelected"))
         XMLWriter::WriteAttr(name, value);
   }

   void Write(const wxString &data) override { text += data; }

   wxString text;
};

wxString SourceState(const Track &track)
{
   RenderSourceState state;
   track.WriteXML(state);
   return std::move(state.text);
}

const wxEventTypeTag<wxThreadEvent> workerReady{ wxNewEventType() };

// Exactly one request can be in flight. Its captures can refer to the worker
// stack because Dispatch waits until the UI has finished using them.
class RenderWorker
{
public:
   ~RenderWorker()
   {
      {
         std::lock_guard lock{mutex};
         stopping = true;
      }
      condition.notify_one();
      if (thread.joinable())
         thread.join();
   }

   void Start(MixRenderJob &job, wxEvtHandler &owner)
   {
      thread = std::thread([this, &job, &owner] {
         const Sequence::ReadDispatcher dispatch = [&](auto action) {
            std::unique_lock lock{mutex};
            if (stopping)
               throw Stopped{};
            auto event = std::make_unique<wxThreadEvent>(workerReady);
            requestError = {};
            request = std::move(action);
            wxQueueEvent(&owner, event.release());
            condition.wait(lock, [&] { return stopping || !request; });
            if (stopping)
               throw Stopped{};
            if (requestError)
               std::rethrow_exception(requestError);
         };

         try {
            const Sequence::ScopedReadDispatcher reads{dispatch};
            while (job.ProcessNext(workerSamplesPerStep, dispatch))
               elapsed.store(job.Elapsed());
         }
         catch (const Stopped &) {}
         catch (...) { failure = std::current_exception(); }
         done.store(true);
      });
   }

   void ServiceRequest()
   {
      std::unique_lock lock{mutex};
      if (!request)
         return;
      // The worker is waiting on the condition variable. Keep the request
      // alive until the callable and all its borrowed buffers are finished.
      try { request(); }
      catch (...) { requestError = std::current_exception(); }
      request = {};
      lock.unlock();
      condition.notify_one();
   }

   std::atomic<double> elapsed{};
   std::atomic<bool> done{};
   std::exception_ptr failure; // Published by done.

private:
   struct Stopped {};
   std::mutex mutex;
   std::condition_variable condition;
   std::function<void()> request;
   std::exception_ptr requestError;
   bool stopping{};
   std::thread thread;
};

}

struct OperationsToolBar::Render {
   // The mixer borrows tracks and the time-warp envelope. Keep private copies
   // alive until after the mixer and its detached output have been destroyed.
   TrackListHolder sources{ TrackList::Create(nullptr) };
   TrackListHolder timeTrack;
   std::unique_ptr<MixRenderJob> job;
   std::vector<std::pair<TrackId, wxString>> originals;
   wxString warpState;
   bool replaceSources{};
   double rate{};
   sampleFormat format{};
   size_t sourceCount{};
   int waveColor{};
   int progress{};
   Clock::duration processingTime{};
   Clock::time_point previousSliceEnd{};
   Clock::time_point lastDisplay{};
   Clock::time_point started{};
   Clock::time_point finished{};
   bool complete{};
   std::unique_ptr<RenderWorker> worker; // Destroy and join before the job.
};

struct OperationsToolBar::EffectProgress final
   : BasicUI::ProgressDialog, wxEventFilter
{
   EffectProgress(OperationsToolBar &toolbar, const TranslatableString &title,
      const TranslatableString &message, unsigned flags)
      : toolbar{toolbar}, title{title}, message{message}, flags{flags}
   {
      projectWindow = &ProjectWindow::Get(toolbar.mProject);
      toolbarWindow = wxGetTopLevelParent(&toolbar);
      // Keep Cancel reachable in the docked toolbar or its floating frame.
      // wxWidgets 3.1 can exempt only one top-level window.
      disabler = std::make_unique<wxWindowDisabler>(toolbarWindow);
      wxEvtHandler::AddFilter(this);
   }

   ~EffectProgress() override
   {
      wxEvtHandler::RemoveFilter(this);
      toolbar.mEffectProgress = nullptr;
      toolbar.UpdateDisplay();
   }

   int FilterEvent(wxEvent &event) override
   {
      const auto type = event.GetEventType();
      if (type == wxEVT_MENU || type == wxEVT_MENU_OPEN)
         return Event_Ignore;
      auto *window = dynamic_cast<wxWindow *>(event.GetEventObject());
      if (!window)
         return Event_Skip;
      const auto *top = wxGetTopLevelParent(window);
      if (top != projectWindow && top != toolbarWindow)
         return Event_Skip;
      if (window == toolbar.mCancel || window == toolbar.mStop)
         return Event_Skip;
      if (type == wxEVT_CLOSE_WINDOW || type == wxEVT_CONTEXT_MENU ||
          event.GetEventCategory() == wxEVT_CATEGORY_USER_INPUT ||
          event.IsCommandEvent())
         return Event_Ignore;
      return Event_Skip;
   }

   BasicUI::ProgressResult Poll(unsigned long long numerator,
      unsigned long long denominator, const TranslatableString &newMessage) override
   {
      if (!newMessage.empty())
         message = newMessage;
      fraction = denominator
         ? std::min(1.0, static_cast<double>(numerator) / denominator) : 0.0;
      const auto now = Clock::now();
      if (now - lastPaint >= std::chrono::milliseconds{50}) {
         lastPaint = now;
         toolbar.UpdateDisplay();
         if (auto *loop = wxEventLoopBase::GetActive(); loop && !loop->IsYielding())
            loop->YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT);
      }
      return result;
   }

   void SetMessage(const TranslatableString &value) override { message = value; }
   void SetDialogTitle(const TranslatableString &value) override { title = value; }
   void Reinit() override
   {
      started = Clock::now();
      lastPaint = {};
      fraction = 0.0;
      result = BasicUI::ProgressResult::Success;
   }

   OperationsToolBar &toolbar;
   TranslatableString title;
   TranslatableString message;
   unsigned flags;
   Clock::time_point started{Clock::now()};
   Clock::time_point lastPaint{};
   double fraction{};
   BasicUI::ProgressResult result{BasicUI::ProgressResult::Success};
   wxWindow *projectWindow{};
   wxWindow *toolbarWindow{};
   std::unique_ptr<wxWindowDisabler> disabler;
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
   Bind(workerReady, [this](wxThreadEvent &) {
      if (mRender && mRender->worker && CanServiceRender())
         mRender->worker->ServiceRequest();
   });
}

OperationsToolBar::~OperationsToolBar()
{
   mTimer.Stop();
   mRender.reset();
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
   mCancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
      if (mEffectProgress && !(mEffectProgress->flags & BasicUI::ProgressShowCancel))
         mEffectProgress->result = BasicUI::ProgressResult::Stopped;
      else if (mEffectProgress)
         Cancel();
      else
         AdvanceRenderQueue();
   });
   mStop = safenew wxButton(this, wxID_ANY, XO("Stop").Translation(),
      wxDefaultPosition, wxSize(-1, toolbarSingle), wxBU_EXACTFIT);
   mStop->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
      if (mEffectProgress)
         mEffectProgress->result = BasicUI::ProgressResult::Stopped;
   });

   Add(mStatus, 2, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   Add(mProgress, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   Add(mStop, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
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
   return 160 + (mCancel ? mCancel->GetBestSize().x : 0) +
      (mStop && mStop->IsShown() ? mStop->GetBestSize().x + 4 : 0);
}

void OperationsToolBar::EnableDisableButtons()
{
   if (!mCancel)
      return;
   const auto flags = mEffectProgress ? mEffectProgress->flags : 0;
   const bool stop = flags & BasicUI::ProgressShowStop;
   const bool cancel = flags & BasicUI::ProgressShowCancel;
   const auto label = (stop && !cancel ? XO("Stop") : XO("Cancel")).Translation();
   bool relayout = mStop->Show(stop && cancel);
   relayout |= mCancel->Show(!mEffectProgress || stop || cancel);
   if (mCancel->GetLabel() != label) {
      mCancel->SetLabel(label);
      relayout = true;
   }
   mCancel->Enable(IsRunning());
   if (relayout)
      Layout();
}

void OperationsToolBar::UpdateDisplay()
{
   if (mEffectProgress) {
      const auto &progress = *mEffectProgress;
      auto label = progress.message.empty() ? progress.title : progress.message;
      if (!(progress.flags & BasicUI::ProgressHideTime)) {
         const auto elapsed = std::chrono::duration<double>(
            Clock::now() - progress.started).count();
         const auto elapsedText = FormatTime(static_cast<long long>(elapsed));
         const auto remaining = progress.fraction > 0.0
            ? std::ceil(elapsed * (1.0 - progress.fraction) / progress.fraction)
            : std::numeric_limits<double>::infinity();
         if (elapsed >= 0.5 && remaining <= std::numeric_limits<int>::max())
            label = XO("%s · %s<%s").Format(label.Translation(), elapsedText,
               FormatTime(static_cast<long long>(remaining)));
         else
            label = XO("%s · %s").Format(label.Translation(), elapsedText);
      }
      mStatus->SetLabel(label.Translation());
      mStatus->SetToolTip(label.Translation());
      mProgress->SetValue(static_cast<int>(progress.fraction * progressRange));
      EnableDisableButtons();
      return;
   }
   auto label = !mRender ? XO("No active operation") :
      mRender->complete ? XO("Ready to add mix") : XO("Mix and Render");
   wxString elapsedText;
   if (mRender) {
      const auto end = mRender->complete ? mRender->finished : Clock::now();
      elapsedText = FormatTime(std::chrono::duration_cast<std::chrono::seconds>(
         end - mRender->started).count());
      label = XO("%s · %s").Format(label.Translation(), elapsedText);
   }
   if (mRender && !mRender->complete) {
      const auto duration = mRender->job->Duration();
      const auto elapsed = mRender->worker
         ? mRender->worker->elapsed.load() : mRender->job->Elapsed();
      const auto time = std::chrono::duration<double>(mRender->processingTime).count();
      if (elapsed > 0 && duration > elapsed && time >= 0.5) {
         const auto remaining = std::ceil(time * (duration - elapsed) / elapsed);
         if (remaining >= 0 && remaining <= std::numeric_limits<int>::max()) {
            const auto seconds = static_cast<int>(remaining);
            label = XO("Mix and Render · %s<%s").Format(
               elapsedText, FormatTime(seconds));
         }
      }
   }
   mStatus->SetLabel(label.Translation());
   auto tooltip = XO("%s\nDouble-click to widen to the dock edge.").Format(label);
   if (!mRenderQueue.empty())
      tooltip = XO("%s\nQueued renders: %d").Format(tooltip.Translation(),
         static_cast<int>(mRenderQueue.size()));
   mStatus->SetToolTip(tooltip.Translation());
   mProgress->SetValue(mRender ? mRender->progress : 0);
   EnableDisableButtons();
}

std::unique_ptr<BasicUI::ProgressDialog> OperationsToolBar::StartEffectProgress(
   const TranslatableString &title, const TranslatableString &message, unsigned flags)
{
   if (mEffectProgress || !CanServiceRender() || !mStatus ||
       (flags & BasicUI::ProgressConfirmStopOrCancel))
      return {};

   ToolManager::Get(mProject).Expose(ID(), true);
   auto progress = std::make_unique<EffectProgress>(*this, title, message, flags);
   mEffectProgress = progress.get();
   UpdateDisplay();
   return progress;
}

void OperationsToolBar::StartMixRender(bool replaceSources)
{
   auto &tracks = TrackList::Get(mProject);
   auto selected = tracks.Selected<WaveTrack>();
   if (selected.empty())
      return;

   auto render = std::make_unique<Render>();
   render->replaceSources = replaceSources;
   render->sourceCount = selected.size();
   render->waveColor = (*selected.begin())->GetWaveColorIndex();
   for (const auto *track : selected) {
      if (replaceSources)
         render->originals.emplace_back(track->GetId(), SourceState(*track));
      render->sources->Append(std::move(*track->Duplicate()));
   }

   if (const auto *time = *tracks.Any<const TimeTrack>().begin()) {
      if (replaceSources)
         render->warpState = SourceState(*time);
      render->timeTrack = time->Duplicate();
   }

   render->rate = ProjectRate::Get(mProject).GetRate();
   render->format = QualitySettings::SampleFormatChoice();
   mRenderQueue.push_back(std::move(render));
   ToolManager::Get(mProject).Expose(ID(), true);
   if (!mRender && !mTimer.IsRunning())
      mTimer.StartOnce(nextSliceMs);
   UpdateDisplay();
}

void OperationsToolBar::StartNextRender()
{
   mRender = std::move(mRenderQueue.front());
   mRenderQueue.pop_front();
   auto &render = *mRender;
   const auto *warp = render.timeTrack
      ? static_cast<const TimeTrack &>(**render.timeTrack->begin()).GetEnvelope()
      : nullptr;
   render.job = MixRenderJob::Begin(
      render.sources->Any<const WaveTrack>(), Mixer::WarpOptions{warp},
      TrackList::Get(mProject).MakeUniqueTrackName(XO("Mix").Translation()),
      WaveTrackFactory::Get(mProject), render.rate, render.format, 0.0, 0.0);
   render.started = Clock::now();
   if (gPrefs->ReadBool(wxT("/AudioIO/MixRenderWorker"), false)) {
      render.worker = std::make_unique<RenderWorker>();
      try {
         render.worker->Start(*render.job, *this);
      }
      catch (const std::system_error &) {
         // The existing UI-sliced path is available if thread creation fails.
         render.worker.reset();
      }
   }
   UpdateDisplay();
}

void OperationsToolBar::AdvanceRenderQueue()
{
   mTimer.Stop();
   if (mRender)
      mRender.reset();
   else if (!mRenderQueue.empty())
      mRenderQueue.pop_front();
   if (!mRenderQueue.empty())
      mTimer.StartOnce(nextSliceMs);
   UpdateDisplay();
}

void OperationsToolBar::Cancel()
{
   if (mEffectProgress) {
      mEffectProgress->result = BasicUI::ProgressResult::Cancelled;
      return;
   }
   mRenderQueue.clear();
   AdvanceRenderQueue();
}

bool OperationsToolBar::CanServiceRender() const
{
   const auto &window = ProjectWindow::Get(mProject);
   return !mEffectProgress && !window.IsBeingDeleted() && window.IsEnabled() &&
      !mProject.mbBusyImporting;
}

void OperationsToolBar::OnTimer(wxTimerEvent &)
{
   if (!mRender && mRenderQueue.empty())
      return;

   auto &window = ProjectWindow::Get(mProject);
   if (window.IsBeingDeleted()) {
      Cancel();
      return;
   }
   // An existing modal operation may yield timer events mid-command.
   if (!CanServiceRender()) {
      if (mRender)
         mRender->previousSliceEnd = {};
      mTimer.StartOnce(blockedPollMs);
      return;
   }

   GuardedCall<void>([&] {
      if (!mRender)
         StartNextRender();
      auto &render = *mRender;
      if (render.worker && !render.complete) {
         render.worker->ServiceRequest();
         render.complete = render.worker->done.load();
         if (render.complete && render.worker->failure)
            std::rethrow_exception(render.worker->failure);
         const auto now = Clock::now();
         render.processingTime = now - render.started;
         if (render.complete)
            render.finished = now;
         const auto duration = render.job->Duration();
         const auto fraction = duration > 0
            ? std::clamp(render.worker->elapsed.load() / duration, 0.0, 1.0) : 0.0;
         render.progress = render.complete ? progressRange
            : static_cast<int>(progressRange * fraction);
         UpdateDisplay();
      }
      else if (!render.complete) {
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
         mTimer.StartOnce(render.complete || render.worker ? blockedPollMs : nextSliceMs);
         return;
      }

      if (render.replaceSources && wxWindow::GetCapture()) {
         mTimer.StartOnce(blockedPollMs);
         return;
      }

      auto &tracks = TrackList::Get(mProject);
      std::vector<Track *> originals;
      Track *insertionPoint{};
      if (render.replaceSources) {
         for (const auto &[id, state] : render.originals) {
            auto *track = tracks.FindById(id);
            if (track && track->IsLeader() && SourceState(*track) == state)
               originals.push_back(track);
         }
         const auto *time = *tracks.Any<const TimeTrack>().begin();
         if (originals.size() != render.originals.size() ||
             (time ? SourceState(*time) : wxString{}) != render.warpState) {
            throw SimpleMessageBoxException{ExceptionType::BadUserAction,
               XO("A render source changed. The project audio was kept."),
               XO("Mix and Render")};
         }

         // Place the result immediately after the last source in current
         // project order, then remove the original tracks.
         for (auto it = tracks.begin(); it != tracks.end(); ++it) {
            if (std::find(originals.begin(), originals.end(), *it) != originals.end())
               insertionPoint = *std::next(it);
         }
      }

      auto result = render.job->Finish();
      auto &mix = static_cast<WaveTrack &>(**result->begin());
      mix.SetSelected(std::any_of(originals.begin(), originals.end(),
         [](const Track *track) { return track->GetSelected(); }));
      mix.SetWaveColorIndex(render.waveColor);
      if (render.sourceCount > 1)
         mix.SetName(tracks.MakeUniqueTrackName(XO("Mix").Translation()));
      const auto description = render.sourceCount == 1
         ? XO("Rendered all audio in track '%s'").Format(mix.GetName())
         : (mix.NChannels() > 1
              ? XO("Mixed and rendered %d tracks into one new stereo track")
              : XO("Mixed and rendered %d tracks into one new mono track"))
              .Format(static_cast<int>(render.sourceCount));
      const auto command = render.replaceSources && render.sourceCount == 1
         ? XO("Render") : XO("Mix and Render");
      auto &history = ProjectHistory::Get(mProject);
      try {
         tracks.Insert(insertionPoint, std::move(*result));
         for (auto *original : originals)
            tracks.Remove(*original);
         history.PushState(description, command);
      }
      catch (...) {
         if (render.replaceSources)
            history.RollbackState();
         else if (mix.GetOwner().get() == &tracks)
            tracks.Remove(mix);
         throw;
      }
      AdvanceRenderQueue();
   }, [this](AudacityException *exception) {
      AdvanceRenderQueue();
      if (!exception)
         BasicUI::ShowErrorDialog({}, XO("Mix and Render"), XO("The render failed."), {});
   });
}

namespace {
const EffectBase::ProgressFactory::Scope effectProgressFactory{
   [](AudacityProject &project, const TranslatableString &title,
      const TranslatableString &message, unsigned flags) {
      auto *toolbar = OperationsToolBar::Get(project);
      return toolbar ? toolbar->StartEffectProgress(title, message, flags) : nullptr;
   }
};

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
