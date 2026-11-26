/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  RealtimeEffectStateUI.cpp

  Dmitry Vedenko

**********************************************************************/
#include "RealtimeEffectStateUI.h"

#include <cassert>
#include <utility>
#include <vector>

#include "EffectAutomationParameters.h"
#include "EffectEditor.h"
#include "EffectUI.h"
#include "EffectUIServices.h"
#include "RealtimeEffectList.h"
#include "RealtimeEffectState.h"

#include "EffectBase.h"
#include "EffectManager.h"
#include "UndoManager.h"
#include "ProjectHistory.h"
#include "ProjectWindow.h"
#include "Track.h"
#include "WaveTrack.h"

namespace
{
const RealtimeEffectState::RegisteredFactory realtimeEffectStateUIFactory { [](auto& state)
   { return std::make_unique<RealtimeEffectStateUI>(state); }
};

std::optional<wxString> DescribeSettings(
   const RealtimeEffectState &state, const EffectSettings &settings)
{
   const auto *effect = state.GetEffect();
   CommandParameters parameters;
   wxString description;
   if (!effect || !effect->SaveSettings(settings, parameters) ||
       !parameters.GetParameters(description))
      return {};
   description.Prepend(settings.extra.GetActive() ? "1:" : "0:");
   return description;
}

// Track history keeps the effect identities so mixer controls and editor
// windows can survive track replacements. Save an independent settings value
// alongside those identities for each project history entry.
struct RealtimeEffectSettingsRestorer final : UndoStateExtension {
   explicit RealtimeEffectSettingsRestorer(AudacityProject &project)
   {
      const auto capture = [this](RealtimeEffectState &state, bool) {
         if (std::as_const(state).GetEffect())
            settings.emplace_back(state.shared_from_this(), state.GetSettings());
      };
      RealtimeEffectList::Get(project).Visit(capture);
      for (auto *track : TrackList::Get(project).Any<WaveTrack>())
         RealtimeEffectList::Get(*track).Visit(capture);
   }

   void RestoreUndoRedoState(AudacityProject &) override
   {
      for (const auto &[state, snapshot] : settings) {
         const auto previous = DescribeSettings(*state, state->GetSettings());
         const auto restored = DescribeSettings(*state, snapshot);
         if (previous && restored && *previous == *restored)
            continue;
         // Give the editor a chance to commit native state before replacing
         // its settings. Hide without a project to suppress a new undo entry.
         RealtimeEffectStateUI::Get(*state).Hide();
         state->RestoreSettings(snapshot);
      }
   }

   std::vector<std::pair<std::shared_ptr<RealtimeEffectState>, EffectSettings>> settings;
};

UndoRedoExtensionRegistry::Entry realtimeEffectSettingsHistory {
   [](AudacityProject &project) {
      return std::make_shared<RealtimeEffectSettingsRestorer>(project);
   }
};
} // namespace


BEGIN_EVENT_TABLE(RealtimeEffectStateUI, wxEvtHandler)
   EVT_CLOSE(RealtimeEffectStateUI::OnClose)
END_EVENT_TABLE()

RealtimeEffectStateUI::RealtimeEffectStateUI(RealtimeEffectState& state)
    : mRealtimeEffectState(state)
{
}

RealtimeEffectStateUI::~RealtimeEffectStateUI()
{
   Hide();
}

bool RealtimeEffectStateUI::IsShown() const noexcept
{
   return mEffectUIHost != nullptr;
}

void RealtimeEffectStateUI::Show(AudacityProject& project)
{
   if (mEffectUIHost != nullptr && mEffectUIHost->IsShown())
   {
      // Bring the host to front
      mEffectUIHost->Raise();
      return;
   }

   const auto ID = mRealtimeEffectState.GetID();
   const auto effectPlugin =
      dynamic_cast<EffectBase*>(EffectManager::Get().GetEffect(ID));

   if (effectPlugin == nullptr)
      return;

   const auto client = dynamic_cast<EffectUIServices *>(effectPlugin);

   // Effect has no client interface
   if (client == nullptr)
      return;

   auto& projectWindow = ProjectWindow::Get(project);

   std::shared_ptr<EffectInstance> pInstance;

   auto access = mRealtimeEffectState.GetAccess();

   // EffectUIHost invokes shared_from_this on access
   Destroy_ptr<EffectUIHost> dlg(safenew EffectUIHost(
      &projectWindow, project, *effectPlugin, *client, pInstance, *access,
      mRealtimeEffectState.shared_from_this()));

   if (!dlg->Initialize())
      return;

   mOpeningSettings = DescribeSettings(mRealtimeEffectState, access->Get());
   mParameterChanged = false;

   // Dialog is owned by its parent now!
   mEffectUIHost = dlg.release();

   UpdateTitle();

   client->ShowClientInterface(*effectPlugin,
      projectWindow, *mEffectUIHost, mEffectUIHost->GetEditor(), false);

   // The dialog was modal? That shouldn't have happened
   if (mEffectUIHost == nullptr || !mEffectUIHost->IsShown())
   {
      assert(false);
      mEffectUIHost = {};
      return;
   }

   mEffectUIHost->PushEventHandler(this);
   mpProject = &project;

   mProjectWindowDestroyedSubscription = projectWindow.Subscribe(
      [this, &project](ProjectWindowDestroyedMessage) {
         // This achieves autosave on close of project before other important
         // project state is destroyed
         Hide(&project);
      });

   mParameterChangedSubscription = mEffectUIHost->GetEditor()->Subscribe(
      [this](auto) {
         if (mpProject) {
            mParameterChanged = true;
            UndoManager::Get(*mpProject).MarkUnsaved();
         }
      });
}

void RealtimeEffectStateUI::Hide(AudacityProject* project)
{
   if (mEffectUIHost != nullptr)
   {
      mpProject = project; // May reassign as null to skip autosave in OnClose
      // EffectUIHost calls Destroy in OnClose handler
      mEffectUIHost->Close();
      mEffectUIHost = {};
   }
}

void RealtimeEffectStateUI::Toggle(AudacityProject& project)
{
   if(IsShown())
      Hide(&project);
   else
      Show(project);
}

void RealtimeEffectStateUI::UpdateTrackData(const Track& track)
{
   mTrackName = track.GetName();

   UpdateTitle();
}

RealtimeEffectStateUI& RealtimeEffectStateUI::Get(RealtimeEffectState& state)
{      
   return state.Get<RealtimeEffectStateUI&>(realtimeEffectStateUIFactory);
}

const RealtimeEffectStateUI&
RealtimeEffectStateUI::Get(const RealtimeEffectState& state)
{
   return Get(const_cast<RealtimeEffectState&>(state));
}

void RealtimeEffectStateUI::UpdateTitle()
{
   if (mEffectUIHost == nullptr)
      return;

   if (mEffectName.empty())
   {
      const auto ID = mRealtimeEffectState.GetID();
      const auto effectPlugin = EffectManager::Get().GetEffect(ID);

      if (effectPlugin != nullptr)
         mEffectName = effectPlugin->GetDefinition().GetName();
   }

   const auto title =
      mTrackName.empty() ?
         mEffectName :
         /* i18n-hint: First %s is an effect name, second is a track name */
         XO("%s - %s").Format(mEffectName, mTrackName);

   mEffectUIHost->SetTitle(title);
   mEffectUIHost->SetName(title);
}

void RealtimeEffectStateUI::AutoSave(AudacityProject &project)
{
   ProjectHistory::AutoSave::Call(project);
}

void RealtimeEffectStateUI::SaveClosedEditor(AudacityProject &project)
{
   auto access = mRealtimeEffectState.GetAccess();
   access->Flush();
   const auto settings = DescribeSettings(mRealtimeEffectState, access->Get());
   const auto changed = mOpeningSettings && settings
      ? *mOpeningSettings != *settings : mParameterChanged;

   if (changed) {
      const auto description = mTrackName.empty()
         ? XO("Changed settings for %s").Format(mEffectName)
         : XO("Changed settings for %s on %s").Format(mEffectName, mTrackName);
      ProjectHistory::Get(project).PushState(
         description, XO("Change Realtime Effect Settings"));
   }
   else
      AutoSave(project);
}

void RealtimeEffectStateUI::OnClose(wxCloseEvent & evt)
{
   auto keepStateAlive = mRealtimeEffectState.weak_from_this().lock();
   auto next = GetNextHandler();
   mEffectUIHost->RemoveEventHandler(this);
   auto pProject = mpProject;
   mpProject = nullptr;

   // Native editors may only store their full settings during close.
   if (next)
      next->ProcessEvent(evt);

   mEffectUIHost = {};
   mParameterChangedSubscription.Reset();
   if (pProject)
      SaveClosedEditor(*pProject);
}
