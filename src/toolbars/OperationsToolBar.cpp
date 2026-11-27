/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OperationsToolBar.h"

#include "AudacityException.h"
#include "EffectBase.h"
#include "MixAndRender.h"
#include "RealtimeEffectList.h"
#include "ProjectHistory.h"
#include "ProjectRate.h"
#include "QualitySettings.h"
#include "TimeTrack.h"
#include "ToolManager.h"
#include "ToolDock.h"
#include "Track.h"
#include "UndoManager.h"
#include "WaveTrack.h"
#include "XMLWriter.h"

#include "../CommonCommandFlags.h"
#include "../ProjectWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
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

}

struct OperationsToolBar::Render {
   std::uint64_t id{};
   // The mixer borrows tracks and the time-warp envelope. Keep private copies
   // alive until after the mixer and its detached output have been destroyed.
   TrackListHolder sources{ TrackList::Create(nullptr) };
   TrackListHolder timeTrack;
   std::unique_ptr<MixRenderJob> job;
   std::vector<std::pair<TrackId, wxString>> originals;
   wxString warpState;
   bool replaceSources{};
   bool effectsEnabled{true};
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
   bool validateSources{true};
   TranslatableString label;
};

struct OperationsToolBar::QueueDialog final : wxDialog
{
   struct Item {
      std::uint64_t id;
      wxString label;
      int progress;
      bool cancellable;
   };

   explicit QueueDialog(OperationsToolBar &toolbar)
      : wxDialog(&toolbar, wxID_ANY,
           XO("Operations Queue").Translation(), wxDefaultPosition, wxDefaultSize,
           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
      , mToolbar{toolbar}
   {
      auto *layout = safenew wxBoxSizer(wxVERTICAL);
      mList = safenew wxScrolledWindow(this, wxID_ANY);
      mList->SetScrollRate(0, 12);
      mList->SetSizer(safenew wxBoxSizer(wxVERTICAL));
      layout->Add(mList, 1, wxEXPAND | wxALL, 8);

      auto *close = safenew wxButton(this, wxID_CLOSE, XO("Close").Translation());
      close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Hide(); });
      Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { Hide(); });
      layout->Add(close, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 8);
      SetSizer(layout);
      SetSize(FromDIP(wxSize(560, 280)));
   }

   void UpdateItems(const std::vector<Item> &items)
   {
      if (!IsShown())
         return;
      if (!mList->GetSizer()->GetItemCount() || items.size() != mRows.size() ||
          !std::equal(items.begin(), items.end(), mRows.begin(),
             [](const Item &item, const Row &row) { return item.id == row.id; }))
      {
         auto *layout = mList->GetSizer();
         layout->Clear(true);
         mRows.clear();
         if (items.empty())
            layout->Add(safenew wxStaticText(mList, wxID_ANY,
               XO("No active or pending operations").Translation()), 0, wxALL, 4);

         for (const auto &item : items) {
            auto *line = safenew wxBoxSizer(wxHORIZONTAL);
            auto *label = safenew wxStaticText(mList, wxID_ANY, wxString{},
               wxDefaultPosition, FromDIP(wxSize(140, -1)),
               wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
            auto *progress = safenew wxGauge(mList, wxID_ANY, progressRange,
               wxDefaultPosition, FromDIP(wxSize(100, -1)));
            auto *cancel = safenew wxButton(mList, wxID_ANY, XO("Cancel").Translation());
            cancel->Bind(wxEVT_BUTTON, [this, id = item.id](wxCommandEvent &) {
               // Updating the list may delete this button.
               CallAfter([this, id] { mToolbar.CancelQueuedRender(id); });
            });
            line->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            line->Add(progress, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            line->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);
            layout->Add(line, 0, wxEXPAND | wxALL, 4);
            mRows.push_back({item.id, label, progress, cancel});
         }
         mList->Layout();
         mList->FitInside();
      }

      for (size_t i = 0; i < items.size(); ++i) {
         const auto &item = items[i];
         const auto &row = mRows[i];
         if (row.label->GetLabel() != item.label) {
            row.label->SetLabel(item.label);
            row.label->SetToolTip(item.label);
         }
         row.progress->SetValue(item.progress);
         row.cancel->Enable(item.cancellable);
      }
   }

private:
   struct Row {
      std::uint64_t id;
      wxStaticText *label;
      wxGauge *progress;
      wxButton *cancel;
   };
   OperationsToolBar &mToolbar;
   wxScrolledWindow *mList{};
   std::vector<Row> mRows;
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
      if (toolbar.mQueueDialog)
         toolbar.mQueueDialog->Enable();
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
      if (window == toolbar.mCancel || window == toolbar.mStop || window == toolbar.mQueue)
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
   : ToolBar(project, XO("Operations"), ID())
{
   Bind(wxEVT_TIMER, &OperationsToolBar::OnTimer, this, mTimer.GetId());
   mUndoSubscription = UndoManager::Get(project).Subscribe([this](UndoRedoMessage event) {
      if (mRender && (event.type == UndoRedoMessage::Pushed ||
             event.type == UndoRedoMessage::Modified ||
             event.type == UndoRedoMessage::UndoOrRedo ||
             event.type == UndoRedoMessage::Reset))
         mRender->validateSources = true;
   });
   mTrackSubscription = TrackList::Get(project).Subscribe([this](const TrackListEvent &event) {
      if (mRender && (event.mType == TrackListEvent::TRACK_DATA_CHANGE ||
             event.mType == TrackListEvent::DELETION || event.mType == TrackListEvent::ADDITION))
         mRender->validateSources = true;
   });
}

OperationsToolBar::~OperationsToolBar()
{
   mTimer.Stop();
   mQueueDialog.reset();
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

   mTime = safenew wxStaticText(this, wxID_ANY, wxString{});
   mQueue = safenew wxButton(this, wxID_ANY, XO("Queue").Translation(),
      wxDefaultPosition, wxSize(-1, toolbarSingle), wxBU_EXACTFIT);
   mQueue->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { ShowRenderQueue(); });

   auto *rows = safenew wxBoxSizer(wxVERTICAL);
   rows->Add(mStatus, 1, wxEXPAND);
   auto *controls = safenew wxBoxSizer(wxHORIZONTAL);
   controls->Add(mTime, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   controls->Add(mProgress, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   controls->Add(mQueue, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   controls->Add(mStop, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
   controls->Add(mCancel, 0, wxALIGN_CENTER_VERTICAL);
   rows->Add(controls, 0, wxEXPAND);
   Add(rows, 1, wxEXPAND);

   UpdateDisplay();
}

wxSize OperationsToolBar::GetDockedSize()
{
   return {GetMinToolbarWidth(), GetMinToolbarHeight()};
}

int OperationsToolBar::GetMinToolbarWidth()
{
   // Leave room for the grabber, resize grip and padding in the base toolbar.
   return 220 + (mCancel ? mCancel->GetBestSize().x : 0);
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
   mQueue->Enable(IsRunning());
   if (relayout)
      Layout();
}

void OperationsToolBar::UpdateDisplay()
{
   TranslatableString label;
   wxString timing;
   int progress{};
   if (mEffectProgress) {
      const auto &effect = *mEffectProgress;
      label = effect.message.empty() ? effect.title : effect.message;
      progress = static_cast<int>(effect.fraction * progressRange);
      if (!(effect.flags & BasicUI::ProgressHideTime)) {
         const auto elapsed = std::chrono::duration<double>(Clock::now() - effect.started).count();
         timing = FormatTime(static_cast<long long>(elapsed));
         if (elapsed >= 0.5 && effect.fraction > 0.0) {
            const auto remaining = std::ceil(elapsed * (1.0 - effect.fraction) / effect.fraction);
            if (remaining <= std::numeric_limits<int>::max())
               timing += wxT("<") + FormatTime(static_cast<long long>(remaining));
         }
      }
   }
   else if (mRender) {
      const auto &render = *mRender;
      label = render.complete ? XO("Waiting to add result: %s").Format(render.label)
                              : render.label;
      progress = render.progress;
      const auto end = render.complete ? render.finished : Clock::now();
      timing = FormatTime(std::chrono::duration_cast<std::chrono::seconds>(
         end - render.started).count());
      const auto time = std::chrono::duration<double>(render.processingTime).count();
      if (!render.complete && time >= 0.5) {
         const auto elapsed = render.job->Elapsed();
         const auto duration = render.job->Duration();
         if (elapsed > 0.0 && elapsed < duration) {
            const auto remaining = std::ceil(time * (duration - elapsed) / elapsed);
            if (remaining <= std::numeric_limits<int>::max())
               timing += wxT("<") + FormatTime(static_cast<long long>(remaining));
         }
      }
   }
   auto message = label.Translation();
   message.Replace(wxT("\n"), wxT(" "));
   message.Trim();
   mStatus->SetLabel(message);
   mStatus->SetToolTip(XO("%s\nDouble-click to widen to the dock edge.")
      .Format(label).Translation());
   mProgress->SetValue(progress);
   if (mTime->GetLabel() != timing) {
      mTime->SetLabel(timing);
      Layout();
   }
   const auto queueLabel = mRenderQueue.empty() ? XO("Queue")
      : XO("Queue (%d)").Format(static_cast<int>(mRenderQueue.size()));
   if (mQueue->GetLabel() != queueLabel.Translation()) {
      mQueue->SetLabel(queueLabel.Translation());
      Layout();
   }
   EnableDisableButtons();
   UpdateQueueDisplay();
}

void OperationsToolBar::ShowRenderQueue()
{
   if (!mQueueDialog)
      mQueueDialog = std::make_unique<QueueDialog>(*this);
   mQueueDialog->Show();
   UpdateQueueDisplay();
   mQueueDialog->Raise();
}

void OperationsToolBar::UpdateQueueDisplay()
{
   if (!mQueueDialog || !mQueueDialog->IsShown())
      return;

   std::vector<QueueDialog::Item> items;
   if (mEffectProgress) {
      const auto &effect = *mEffectProgress;
      const auto &label = effect.message.empty() ? effect.title : effect.message;
      items.push_back({0, label.Translation(),
         static_cast<int>(effect.fraction * progressRange), false});
   }
   if (mRender) {
      auto status = mRender->complete ? XO("Waiting to add result: %s") :
         mEffectProgress ? XO("Paused: %s") : XO("Active: %s");
      items.push_back({mRender->id, status.Format(mRender->label).Translation(),
         mRender->progress, !mEffectProgress});
   }
   for (const auto &render : mRenderQueue)
      items.push_back({render->id, XO("Pending: %s").Format(render->label).Translation(),
         0, !mEffectProgress});
   mQueueDialog->UpdateItems(items);
}

void OperationsToolBar::CancelQueuedRender(std::uint64_t id)
{
   if (mEffectProgress)
      return;
   if (mRender && mRender->id == id) {
      AdvanceRenderQueue();
      return;
   }
   const auto pending = std::find_if(mRenderQueue.begin(), mRenderQueue.end(),
      [id](const auto &render) { return render->id == id; });
   if (pending != mRenderQueue.end())
      mRenderQueue.erase(pending);
   UpdateDisplay();
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
   render->id = ++mNextRenderId;
   render->replaceSources = replaceSources;
   render->sourceCount = selected.size();
   render->waveColor = (*selected.begin())->GetWaveColorIndex();
   wxString sourceNames;
   for (const auto *track : selected) {
      if (replaceSources)
         render->originals.emplace_back(track->GetId(), SourceState(*track));
      if (!sourceNames.empty())
         sourceNames += wxT(", ");
      sourceNames += track->GetName();
      render->sources->Append(std::move(*track->Duplicate()));
   }

   if (const auto *time = *tracks.Any<const TimeTrack>().begin()) {
      if (replaceSources)
         render->warpState = SourceState(*time);
      render->timeTrack = time->Duplicate();
   }

   render->label = (replaceSources ? XO("Mix and Render: %s")
      : XO("Mix and Render to New Track: %s")).Format(sourceNames);
   render->effectsEnabled = RealtimeEffectList::Get(mProject).AreProjectEffectsEnabled();
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
      WaveTrackFactory::Get(mProject), render.rate, render.format, 0.0, 0.0,
      render.effectsEnabled);
   render.started = Clock::now();
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

void OperationsToolBar::ValidateRenderSources(const Render &render)
{
   auto &tracks = TrackList::Get(mProject);
   const auto *time = *tracks.Any<const TimeTrack>().begin();
   const bool sourcesMatch = std::all_of(render.originals.begin(), render.originals.end(),
      [&](const auto &source) {
         const auto *track = tracks.FindById(source.first);
         return track && track->IsLeader() && SourceState(*track) == source.second;
      });
   if (!sourcesMatch || (time ? SourceState(*time) : wxString{}) != render.warpState)
      throw SimpleMessageBoxException{ExceptionType::BadUserAction,
         XO("A render source changed. The project audio was kept."), XO("Mix and Render")};
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
      if (render.replaceSources && render.validateSources) {
         ValidateRenderSources(render);
         render.validateSources = false;
      }
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

      if (render.replaceSources && wxWindow::GetCapture()) {
         mTimer.StartOnce(blockedPollMs);
         return;
      }

      auto &tracks = TrackList::Get(mProject);
      std::vector<Track *> originals;
      Track *insertionPoint{};
      if (render.replaceSources) {
         ValidateRenderSources(render);
         for (const auto &source : render.originals)
            originals.push_back(tracks.FindById(source.first));

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
