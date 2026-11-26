/**********************************************************************

  Audacity: A Digital Audio Editor

  MeterToolBar.cpp

  Dominic Mazzoni
  Leland Lucius

  See MeterToolBar.h for details

*******************************************************************//*!

\class MeterToolBar
\brief A ToolBar that holds the VU Meter

*//*******************************************************************/



#include "MeterToolBar.h"
#include "widgets/AButton.h"

// For compilers that support precompilation, includes "wx/wx.h".
#include <wx/wxprec.h>

#include <wx/setup.h> // for wxUSE_* macros

#ifndef WX_PRECOMP
#include <wx/tooltip.h>
#endif

#include <wx/gbsizer.h>

#include "AllThemeResources.h"
#include "Decibels.h"
#include "ToolManager.h"
#include "ProjectAudioIO.h"
#include "../widgets/MeterPanel.h"

#if wxUSE_ACCESSIBILITY

class MeterButtonAx : public AButtonAx
{
   wxWeakRef<MeterPanel> mAssociatedMeterPanel{nullptr};
   bool mAccSilent{false};
public:

   MeterButtonAx(AButton& button, MeterPanel& panel)
      : AButtonAx(&button)
      , mAssociatedMeterPanel(&panel)
   { }

   wxAccStatus GetName(int childId, wxString* name) override
   {
      if(childId != wxACC_SELF)
         return wxACC_NOT_IMPLEMENTED;

      if(mAccSilent)
         *name = wxEmptyString; // Jaws reads nothing, and nvda reads "unknown"
      else
      {
         const auto button = static_cast<AButton*>(GetWindow());

         *name = button->GetName();

         if(const auto panel = mAssociatedMeterPanel.get())
         {
            // translations of strings such as " Monitoring " did not
            // always retain the leading space. Therefore a space has
            // been added to ensure at least one space, and stop
            // words from being merged
            if(panel->IsMonitoring())
               *name += wxT(" ") + _(" Monitoring ");
            else if(panel->IsActive())
               *name += wxT(" ") + _(" Active ");

            const auto dbRange = panel->GetDBRange();
            if (dbRange != -1)
               *name += wxT(" ") + wxString::Format(_(" Peak %2.f dB"), panel->GetPeakHold() * dbRange - dbRange);
            else
               *name += wxT(" ") + wxString::Format(_(" Peak %.2f "), panel->GetPeakHold());

            if(panel->IsClipping())
               *name += wxT(" ") + _(" Clipped ");
         }
      }
      return wxACC_OK;
   }

   wxAccStatus GetRole(int childId, wxAccRole* role) override
   {
      if(childId != wxACC_SELF)
         return wxACC_NOT_IMPLEMENTED;

      if(mAccSilent)
         *role = wxROLE_NONE; // Jaws and nvda both read nothing 
      else
         *role = wxROLE_SYSTEM_PUSHBUTTON;
      return wxACC_OK;
   }

   void SetSilent(bool silent)
   {
      if(mAccSilent != silent)
      {
         mAccSilent = silent;
         if(!silent)
         {
            NotifyEvent(
               wxACC_EVENT_OBJECT_FOCUS,
               GetWindow(),
               wxOBJID_CLIENT,
               wxACC_SELF);
         }
      }
   }
};

#endif

IMPLEMENT_CLASS(MeterToolBar, ToolBar);

////////////////////////////////////////////////////////////
/// Methods for MeterToolBar
////////////////////////////////////////////////////////////

Identifier MeterToolBar::ID()
{
   return wxT("StackedMeters");
}

MeterToolBar::MeterToolBar(AudacityProject &project)
   : ToolBar(project, XO("Playback and Recording Meters"), ID(), true)
{
}

MeterToolBar &MeterToolBar::Get(AudacityProject &project)
{
   return *static_cast<MeterToolBar *>(ToolManager::Get(project).GetToolBar(ID()));
}

void MeterToolBar::ReCreateButtons()
{
   MeterPanel::State playState{ false }, recordState{ false };

   auto &projectAudioIO = ProjectAudioIO::Get( mProject );
   if (mPlayMeter &&
      projectAudioIO.GetPlaybackMeter() == mPlayMeter->GetMeter())
   {
      playState = mPlayMeter->SaveState();
      projectAudioIO.SetPlaybackMeter( nullptr );
   }

   if (mRecordMeter &&
      projectAudioIO.GetCaptureMeter() == mRecordMeter->GetMeter())
   {
      recordState = mRecordMeter->SaveState();
      projectAudioIO.SetCaptureMeter( nullptr );
   }

   ToolBar::ReCreateButtons();

   mPlayMeter->RestoreState(playState);
   if( playState.mSaved  ){
      projectAudioIO.SetPlaybackMeter( mPlayMeter->GetMeter() );
   }
   mRecordMeter->RestoreState(recordState);
   if( recordState.mSaved ){
      projectAudioIO.SetCaptureMeter( mRecordMeter->GetMeter() );
   }
}

void MeterToolBar::Populate()
{
   MakeButtonBackgroundsSmall();
   SetBackgroundColour( theTheme.Colour( clrMedium  ) );

   {
      mRecordSetupButton = safenew AButton(this);
      mRecordSetupButton->SetLabel({});
      mRecordSetupButton->SetName(_("Record Meter"));
      mRecordSetupButton->SetToolTip(XO("Record Meter"));
      mRecordSetupButton->SetImages(
         theTheme.Image(bmpRecoloredUpSmall),
         theTheme.Image(bmpRecoloredUpHiliteSmall),
         theTheme.Image(bmpRecoloredDownSmall),
         theTheme.Image(bmpRecoloredHiliteSmall),
         theTheme.Image(bmpRecoloredUpSmall));
      mRecordSetupButton->SetIcon(theTheme.Image(bmpMic));
      mRecordSetupButton->SetButtonType(AButton::Type::FrameButton);
      mRecordSetupButton->SetMinSize({toolbarSingle, toolbarSingle});

      mRecordMeter = safenew MeterPanel( &mProject,
                                this,
                                wxID_ANY,
                                true,
                                wxDefaultPosition,
                                wxSize( 260, toolbarSingle) );
      /* i18n-hint: (noun) The meter that shows the loudness of the audio being recorded.*/
      mRecordMeter->SetName( XO("Recording Level"));
      /* i18n-hint: (noun) The meter that shows the loudness of the audio being recorded.
       This is the name used in screen reader software, where having 'Meter' first
       apparently is helpful to partially sighted people.  */
      mRecordMeter->SetLabel( XO("Meter-Record") );

#if wxUSE_ACCESSIBILITY
      auto meterButtonAcc = safenew MeterButtonAx(*mRecordSetupButton, *mRecordMeter);
#endif
      mRecordSetupButton->Bind(wxEVT_BUTTON, [=](wxCommandEvent&)
      {
#if wxUSE_ACCESSIBILITY
         meterButtonAcc->SetSilent(true);
#endif
         mRecordMeter->ShowMenu(
            mRecordMeter->ScreenToClient(
               ClientToScreen(mRecordSetupButton->GetClientRect().GetBottomLeft())
            )
         );
#if wxUSE_ACCESSIBILITY
         /* if stop/start monitoring was chosen in the menu, then by this point
         OnMonitoring has been called and variables which affect the accessibility
         name have been updated so it's now ok for screen readers to read the name of
         the button */
         meterButtonAcc->SetSilent(false);
#endif
      });
   }

   {
      mPlaySetupButton = safenew AButton(this);
      mPlaySetupButton->SetLabel({});
      mPlaySetupButton->SetName(_("Playback Meter"));
      mPlaySetupButton->SetToolTip(XO("Playback Meter"));
      mPlaySetupButton->SetImages(
         theTheme.Image(bmpRecoloredUpSmall),
         theTheme.Image(bmpRecoloredUpHiliteSmall),
         theTheme.Image(bmpRecoloredDownSmall),
         theTheme.Image(bmpRecoloredHiliteSmall),
         theTheme.Image(bmpRecoloredUpSmall));
      mPlaySetupButton->SetIcon(theTheme.Image(bmpSpeaker));
      mPlaySetupButton->SetButtonType(AButton::Type::FrameButton);
      mPlaySetupButton->SetMinSize({toolbarSingle, toolbarSingle});


      mPlayMeter = safenew MeterPanel( &mProject,
                              this,
                              wxID_ANY,
                              false,
                              wxDefaultPosition,
                              wxSize( 260, toolbarSingle ) );
      /* i18n-hint: (noun) The meter that shows the loudness of the audio playing.*/
      mPlayMeter->SetName( XO("Playback Level"));
      /* i18n-hint: (noun) The meter that shows the loudness of the audio playing.
       This is the name used in screen reader software, where having 'Meter' first
       apparently is helpful to partially sighted people.  */
      mPlayMeter->SetLabel( XO("Meter-Play"));

#if wxUSE_ACCESSIBILITY
      auto meterButtonAcc = safenew MeterButtonAx(*mPlaySetupButton, *mPlayMeter);
#endif
      mPlaySetupButton->Bind(wxEVT_BUTTON, [=](wxCommandEvent&)
      {
#if wxUSE_ACCESSIBILITY
         meterButtonAcc->SetSilent(true);
#endif
         mPlayMeter->ShowMenu(
            mPlayMeter->ScreenToClient(
               ClientToScreen(mPlaySetupButton->GetClientRect().GetBottomLeft())
            )
         );
#if wxUSE_ACCESSIBILITY
         meterButtonAcc->SetSilent(false);
#endif
      });
   }

   auto *rows = safenew wxBoxSizer(wxVERTICAL);
   const auto addMeter = [&](AButton *button, MeterPanel *meter) {
      auto *row = safenew wxBoxSizer(wxHORIZONTAL);
      row->Add(button, 0, wxALIGN_CENTER_VERTICAL);
      row->Add(meter, 1, wxEXPAND);
      rows->Add(row, 1, wxEXPAND);
   };
   addMeter(mPlaySetupButton, mPlayMeter);
   addMeter(mRecordSetupButton, mRecordMeter);
   Add(rows, 1, wxEXPAND);

   RegenerateTooltips();

   Layout();
}

void MeterToolBar::UpdatePrefs()
{
   RegenerateTooltips();

   SetLabel(XO("Playback and Recording Meters"));

   // Give base class a chance
   ToolBar::UpdatePrefs();
}

void MeterToolBar::UpdateControls()
{
   if ( mPlayMeter )
      mPlayMeter->UpdateSliderControl();

   if ( mRecordMeter )
      mRecordMeter->UpdateSliderControl();
}

bool MeterToolBar::Expose( bool show )
{
   auto &projectAudioIO = ProjectAudioIO::Get( mProject );
   if( show ) {
      if( mPlayMeter ) {
         projectAudioIO.SetPlaybackMeter( mPlayMeter->GetMeter() );
      }

      if( mRecordMeter ) {
         projectAudioIO.SetCaptureMeter( mRecordMeter->GetMeter() );
      }
   } else {
      if( mPlayMeter &&
         projectAudioIO.GetPlaybackMeter() == mPlayMeter->GetMeter() ) {
         projectAudioIO.SetPlaybackMeter( nullptr );
      }

      if( mRecordMeter &&
         projectAudioIO.GetCaptureMeter() == mRecordMeter->GetMeter() ) {
         projectAudioIO.SetCaptureMeter( nullptr );
      }
   }

   return ToolBar::Expose( show );
}

int MeterToolBar::GetInitialWidth()
{
   return 270;
}

void MeterToolBar::ShowOutputGainDialog()
{
   mPlayMeter->ShowDialog();
   mPlayMeter->UpdateSliderControl();
}

void MeterToolBar::ShowInputGainDialog()
{
   mRecordMeter->ShowDialog();
   mRecordMeter->UpdateSliderControl();
}

void MeterToolBar::AdjustOutputGain(int adj)
{
   if (adj < 0) {
      mPlayMeter->Decrease(-adj);
   }
   else {
      mPlayMeter->Increase(adj);
   }

   mPlayMeter->UpdateSliderControl();
}

void MeterToolBar::AdjustInputGain(int adj)
{
   if (adj < 0) {
      mRecordMeter->Decrease(-adj);
   }
   else {
      mRecordMeter->Increase(adj);
   }

   mRecordMeter->UpdateSliderControl();
}

static RegisteredToolbarFactory factory{
   [](AudacityProject &project) {
      return ToolBar::Holder{ safenew MeterToolBar{project} };
   }
};

namespace {
AttachedToolBarMenuItem sAttachment{
   MeterToolBar::ID(), wxT("ShowStackedMetersTB"),
   XXO("Playback and Recording &Meters Toolbar"), {}
};
}

// Now define other related menu items
#include "../commands/CommandContext.h"

namespace {
void OnOutputGain(const CommandContext &context)
{
   auto &project = context.project;
   auto &tb = MeterToolBar::Get( project );
   tb.ShowOutputGainDialog();
}

void OnOutputGainInc(const CommandContext &context)
{
   auto &project = context.project;
   auto &tb = MeterToolBar::Get( project );
   tb.AdjustOutputGain(1);
}

void OnOutputGainDec(const CommandContext &context)
{
   auto &project = context.project;
   auto &tb = MeterToolBar::Get( project );
   tb.AdjustOutputGain(-1);
}

void OnInputGain(const CommandContext &context)
{
   auto &project = context.project;
   auto &tb = MeterToolBar::Get( project );
   tb.ShowInputGainDialog();
}

void OnInputGainInc(const CommandContext &context)
{
   auto &project = context.project;
   auto &tb = MeterToolBar::Get( project );
   tb.AdjustInputGain(1);
}

void OnInputGainDec(const CommandContext &context)
{
   auto &project = context.project;
   auto &tb = MeterToolBar::Get( project );
   tb.AdjustInputGain(-1);
}
   
using namespace MenuTable;
BaseItemSharedPtr ExtraMixerMenu()
{
   static BaseItemSharedPtr menu{
   Menu( wxT("Mixer"), XXO("Mi&xer"),
      Command( wxT("OutputGain"), XXO("Ad&just Playback Volume..."),
         OnOutputGain, AlwaysEnabledFlag ),
      Command( wxT("OutputGainInc"), XXO("&Increase Playback Volume"),
         OnOutputGainInc, AlwaysEnabledFlag ),
      Command( wxT("OutputGainDec"), XXO("&Decrease Playback Volume"),
         OnOutputGainDec, AlwaysEnabledFlag ),
      Command( wxT("InputGain"), XXO("Adj&ust Recording Volume..."),
         OnInputGain, AlwaysEnabledFlag ),
      Command( wxT("InputGainInc"), XXO("I&ncrease Recording Volume"),
         OnInputGainInc, AlwaysEnabledFlag ),
      Command( wxT("InputGainDec"), XXO("D&ecrease Recording Volume"),
         OnInputGainDec, AlwaysEnabledFlag )
   ) };
   return menu;
}

AttachedItem sAttachment4{
   wxT("Optional/Extra/Part1"),
   Indirect(ExtraMixerMenu())
};

}
