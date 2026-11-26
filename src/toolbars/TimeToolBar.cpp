/**********************************************************************

 Audacity: A Digital Audio Editor

 TimeToolBar.cpp

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 *//*******************************************************************/



// For compilers that support precompilation, includes "wx/wx.h".
#include <wx/wxprec.h>

#include <wx/setup.h> // for wxUSE_* macros

#ifndef WX_PRECOMP
#include <wx/sizer.h>
#endif

#include "TimeToolBar.h"
#include "SelectionBarListener.h"
#include "ToolManager.h"

#include "AudioIO.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectNumericFormats.h"
#include "ProjectRate.h"
#include "ProjectTimeSignature.h"
#include "ViewInfo.h"

#include "NumericConverterFormats.h"
#include "ProjectSnap.h"
#include "widgets/BasicMenu.h"
#include "wxWidgetsWindowPlacement.h"
#include <wx/checkbox.h>
#include <wx/combo.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#if wxUSE_ACCESSIBILITY
#include "WindowAccessible.h"
#endif

namespace
{
const TranslatableString SnapLabel = XO("Snap");
TranslatableString GetSnapToLabel(Identifier snapTo)
{
   auto item = SnapFunctionsRegistry::Find(snapTo);
   return item != nullptr ? item->label : XO("Unknown");
}

// Build a popup menu based on snap functions registry
struct PopupMenuBuilder final : public SnapRegistryVisitor
{
   template<typename Callback>
   PopupMenuBuilder(Callback callback)
       : menuStack { &menu }
       , snapModeUpdated { std::move(callback) }
   {
      SnapFunctionsRegistry::Visit(*this);
   }

   void BeginGroup(const SnapRegistryGroup& item) override
   {
      if (item.Inlined())
         return;

      auto menu = safenew wxMenu;

      menuStack.back()->AppendSubMenu(menu, item.Label().Translation());
      menuStack.push_back(menu);
   }

   void EndGroup(const SnapRegistryGroup& item) override
   {
      assert(!menuStack.empty());

      if (item.Inlined())
      {
         menuStack.back()->AppendSeparator();
         return;
      }

      menuStack.pop_back();
   }

   void Visit(const SnapRegistryItem& item) override
   {
      auto menuItem = menuStack.back()->AppendCheckItem(wxID_ANY, item.label.Translation());

      if (ReadSnapTo() == item.name)
         menuItem->Check();

      menuStack.back()->Bind(
         wxEVT_MENU,
         [this, id = item.name](wxCommandEvent&) { snapModeUpdated(id); },
         menuItem->GetId());
   }

   wxMenu menu;
   std::vector<wxMenu*> menuStack;
   std::function<void(const Identifier& id)> snapModeUpdated;
};

// Build a linear list from all the items in the snap functions registry
struct SnapToListBuilder final : public SnapRegistryVisitor
{
   SnapToListBuilder()
   {
      SnapFunctionsRegistry::Visit(*this);
   }

   void BeginGroup(const SnapRegistryGroup& item) override
   {
   }

   void EndGroup(const SnapRegistryGroup& item) override
   {
   }

   void Visit(const SnapRegistryItem& item) override
   {
      List.push_back(item.name);
   }

   std::vector<Identifier> List;
};

/*
 * This class provides a hack to use popup menu instead of the dropdown list.
 * This allows to organize the list of items in a more user-friendly way.
 */
class SnapModePopup final : public wxComboPopup
{
public:
   explicit SnapModePopup(AudacityProject& project)
       : mProject { project }
       , mSnappingModeChangedSubscription(ProjectSnap::Get(project).Subscribe(
            [this](auto& msg) {
               UpdateCurrentIndex(msg.newSnapTo);

               auto comboCtrl = GetComboCtrl();

               comboCtrl->SetValue(
                  GetSnapToLabel(msg.newSnapTo).Translation());

               comboCtrl->SetName(GetComboCtrl()->GetValue());
            }))
   {
   }

   void Init () override
   {
      mSnapToList = std::move(SnapToListBuilder().List);

      UpdateCurrentIndex(ReadSnapTo());
   }

   bool Create(wxWindow* parent) override
   {
      mControl = safenew wxWindow(parent, wxID_ANY);

      // This call cannot happen in Init(), because the combobox is not yet in a valid
      // state. Doing a deferred call from Init() is unsafe,
      // as in some cases Audacity recreates the combobox multiple times before the next
      // event loop iteration.
      GetComboCtrl()->SetValue(GetStringValue());

      return mControl;
   }

   wxWindow* GetControl() override
   {
      return mControl;
   }

   wxString GetStringValue() const override
   {
      return GetSnapToLabel(ReadSnapTo()).Translation();
   }

   void OnPopup() override
   {
      PopupMenuBuilder menuBuilder { [this](const auto& id) {
         ProjectSnap::Get(mProject).SetSnapTo(id);
      } };

      BasicMenu::Handle { &menuBuilder.menu }.Popup(
         wxWidgetsWindowPlacement { GetComboCtrl() },
         { 0, GetComboCtrl()->GetSize().y });

      // Hide the combobox list after the menu was closed
      BasicUI::CallAfter([this] { Dismiss(); });
   }

   void SetStringValue(const wxString& value) override
   {
       wxComboPopup::SetStringValue(value);
   }

   bool FindItem(const wxString& item, wxString* trueItem = NULL) override
   {
      return wxComboPopup::FindItem(item, trueItem);
   }

   void OnComboKeyEvent(wxKeyEvent& event) override
   {
      const auto keyCode = event.GetKeyCode();

      if (keyCode == WXK_RETURN || keyCode == WXK_NUMPAD_ENTER)
      {
         GetComboCtrl()->ShowPopup();
         return;
      }

      int direction = 0;

      if (
         keyCode == WXK_UP || keyCode == WXK_NUMPAD_UP || keyCode == WXK_LEFT ||
         keyCode == WXK_NUMPAD_LEFT)
         direction = -1;
      else if (
         keyCode == WXK_DOWN || keyCode == WXK_NUMPAD_DOWN ||
         keyCode == WXK_RIGHT || keyCode == WXK_NUMPAD_RIGHT)
         direction = 1;

      if (direction == 0)
         return;

      const auto newIndex = std::clamp<ptrdiff_t>(
         mCurrentIndex + direction, 0, mSnapToList.size() - 1);

      if (newIndex == mCurrentIndex)
         return;

      mCurrentIndex = newIndex;

      ProjectSnap::Get(mProject).SetSnapTo(mSnapToList[mCurrentIndex]);
   }

   void OnComboCharEvent(wxKeyEvent& event) override
   {
      // Consume the event to prevent editing
   }

   void UpdateCurrentIndex(const Identifier& identifier)
   {
      if (
         mCurrentIndex < mSnapToList.size() &&
         mSnapToList[mCurrentIndex] == identifier)
         return;

      mCurrentIndex = static_cast<size_t>(std::distance(
         mSnapToList.begin(),
         std::find(mSnapToList.begin(), mSnapToList.end(), identifier)));
   }

private:
   AudacityProject& mProject;
   wxWeakRef<wxWindow> mControl;

   std::vector<Identifier> mSnapToList;
   std::ptrdiff_t mCurrentIndex { -1 };

   Observer::Subscription mSnappingModeChangedSubscription;
};
}



IMPLEMENT_CLASS(TimeToolBar, ToolBar);

// Having a fixed ID for the Audio Position is helpful for
// the Jaws screen reader script for Audacity.
enum {
   TimeBarFirstID = 2800,
   AudioPositionID
};

BEGIN_EVENT_TABLE(TimeToolBar, ToolBar)
   EVT_COMMAND(AudioPositionID, EVT_TIMETEXTCTRL_UPDATED, TimeToolBar::OnUpdate)
   EVT_SIZE(TimeToolBar::OnSize)
   EVT_IDLE(TimeToolBar::OnIdle)
END_EVENT_TABLE()

Identifier TimeToolBar::ID()
{
   return wxT("Time");
}

TimeToolBar::TimeToolBar(AudacityProject &project)
:  ToolBar(project, XO("Time"), ID(), true),
   mListener(NULL), mAudioTime(NULL)
{
}

TimeToolBar::~TimeToolBar()
{
}

ToolBar::DockID TimeToolBar::DefaultDockID() const
{
   return BotDockID;
}

TimeToolBar &TimeToolBar::Get(AudacityProject &project)
{
   auto &toolManager = ToolManager::Get(project);
   return *static_cast<TimeToolBar*>(toolManager.GetToolBar(ID()));
}

const TimeToolBar &TimeToolBar::Get(const AudacityProject &project)
{
   return Get(const_cast<AudacityProject&>(project)) ;
}

void TimeToolBar::Populate()
{
   const auto &formats = ProjectNumericFormats::Get(mProject);

   // Get the default time format
   auto format = formats.GetAudioTimeFormat();

   // Create the read-only time control
   mAudioTime = safenew NumericTextCtrl(FormatterContext::ProjectContext(mProject), this, AudioPositionID, NumericConverterType_TIME(), format, 0.0);
   mAudioTime->SetName(XO("Audio Position"));
   mAudioTime->SetReadOnly(true);

   auto *rows = safenew wxBoxSizer(wxVERTICAL);
   mTimeRow = safenew wxBoxSizer(wxHORIZONTAL);
   mTimeRow->Add(mAudioTime, 0, wxALIGN_CENTER);
   rows->Add(mTimeRow, 1, wxEXPAND);
   AddSnapControls(*rows);
   Add(rows, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

   // Calculate the width to height ratio
   wxSize digitSize = mAudioTime->GetDigitSize();
   mDigitRatio = (float)digitSize.x / digitSize.y;
   mAudioTime->UseCompactLabels();
   mAudioTime->HideTrailingSamplesLabel();
   mAudioTime->SetDigitSize(minDigitH * mDigitRatio, minDigitH);

   // During initialization, we need to bypass some resizing to prevent the "best size"
   // from being used as we want to ensure the saved size is used instead. See SetDocked()
   // and OnUpdate() for more info.
   mSettingInitialSize = true;

   // Establish initial resizing limits
   //   SetResizingLimits();
   mFormatChangedToFitValueSubscription = mAudioTime->Subscribe(
      [this](auto)
      {
         SetResizingLimits();
         wxSizeEvent e;
         OnSize(e);
      });
}

void TimeToolBar::UpdatePrefs()
{
   // Since the language may have changed, we need to force an update to accommodate
   // different length text
   wxCommandEvent e;
   e.SetString(mAudioTime->GetFormatName().Internal());
   OnUpdate(e);

   // Language may have changed so reset label
   SetLabel(XO("Time"));

   // Give the toolbar a chance
   ToolBar::UpdatePrefs();
}

void TimeToolBar::SetToDefaultSize()
{
   // Reset
   SetMaxSize(wxDefaultSize);
   SetMinSize(wxDefaultSize);

   // Set the default time format
   SetAudioTimeFormat(NumericConverterFormats::HoursMinsSecondsFormat());

   // Set the default size
   SetSize(GetInitialWidth(), GetMinToolbarHeight());
   SetResizingLimits();

   // Inform others the toobar has changed
   Updated();
}

wxSize TimeToolBar::GetDockedSize()
{
   return wxSize(GetSize().x, GetMinToolbarHeight());
}

void TimeToolBar::SetDocked(ToolDock *dock, bool pushed)
{
   // It's important to call this FIRST since it unhides the resizer control.
   // Not doing so causes the calculated best size to be off by the width
   // of the resizer.
   ToolBar::SetDocked(dock, pushed);

   // Recalculate the min and max limits
   SetResizingLimits();

   // When moving from floater to dock, fit to toolbar since the resizer will
   // be mispositioned
   if (dock && GetSizer()) {
      // During initialization, the desired size is already set, so do not
      // override it with the "best size". See OnUpdate for further info.
      if (!mSettingInitialSize) {
         // Fit() while retaining height
         SetSize(GetBestSize().x, GetSize().y);

         // Inform others the toolbar has changed
         Updated();
      }
   }
}

void TimeToolBar::SetListener(TimeToolBarListener *l)
{
   // Remember the listener
   mListener = l;

   // Get (and set) the saved time format
   SetAudioTimeFormat(mListener->TT_GetAudioTimeFormat());

   // During initialization, if the saved format is the same as the default,
   // OnUpdate() will not be called and need it to set the initial size.
   if (mSettingInitialSize) {
      wxCommandEvent e;
      e.SetString(mAudioTime->GetFormatName().Internal());
      OnUpdate(e);
   }
}

void TimeToolBar::SetAudioTimeFormat(const NumericFormatSymbol & format)
{
   // Set the format if it's different from previous
   if (mAudioTime->SetFormatName(format)) {
      // Simulate an update since the format has changed.
      wxCommandEvent e;
      e.SetString(format.Internal());
      OnUpdate(e);
   }
}

// The intention of this is to get the resize handle in the
// correct position, after we've let go in dragging.
void TimeToolBar::ResizingDone()
{
   SetResizingLimits();

   // Inform others the toobar has changed
   Updated();
}

void TimeToolBar::SetResizingLimits()
{
   if (!GetSizer())
      return;

   // Reset limits
   SetMinSize(wxDefaultSize);
   SetMaxSize(wxDefaultSize);

   const int minH = GetMinToolbarHeight();

   const int chrome = GetSizer()->GetMinSize().x -
      std::max(mTimeRow->GetMinSize().x, mSnapRow->GetMinSize().x) + 2;
   const int width = chrome + std::max(ComputeSizing(minDigitH).x,
      mSnapRow->GetMinSize().x);
   SetMinSize({width, minH});
   if (GetSize().x < width || GetSize().y < minH)
      SetSize(wxMax(GetSize().x, width), wxMax(GetSize().y, minH));
}

// Called when the format drop downs is changed.
// This causes recreation of the toolbar contents.
void TimeToolBar::OnUpdate(wxCommandEvent &evt)
{
   evt.Skip(false);

   ToolBar::ReCreateButtons();

   // Reset to allow resizing to work
   SetMinSize(wxDefaultSize);
   SetMaxSize(wxDefaultSize);

   // Save format name before recreating the controls so they resize properly
   if (mListener) {
      mListener->TT_SetAudioTimeFormat(evt.GetString());
   }
   
   // During initialization, the desired size will have already been set at this point
   // and the "best" size" would override it, so we simply send a size event to force
   // the content to fit inside the toolbar.
   if (mSettingInitialSize) {
      mSettingInitialSize = false;
      SendSizeEvent();
   }
   // Go set the new size limits
   SetResizingLimits();
   SendSizeEvent();

   // Inform others the toobar has changed
   Updated();
}

void TimeToolBar::OnSize(wxSizeEvent &evt)
{
   evt.Skip();

   // Can fire before we're ready
   if (!mAudioTime || !GetSizer()) {
      return;
   }

   // Make sure everything is where it's supposed to be
   Layout();

   // Get the sizer's size and remove any borders the time control might have.
   wxSize sizerBR(mTimeRow->GetSize().x,
      GetClientSize().y - mSnapRow->GetMinSize().y - 3);
   sizerBR -= mAudioTime->GetSize() - mAudioTime->GetClientSize();

   // Get the content size of the time control. This can be different than the
   // control itself due to borders and sizer enduced changes.
   wxSize timeBR = mAudioTime->GetDimensions();

   // Get the current digit box height
   int h = mAudioTime->GetDigitSize().y;

   // Increase current size to find the best fit within the new size
   if (sizerBR.x >= timeBR.x && sizerBR.y >= timeBR.y) {
      do {
         h++;
         timeBR = ComputeSizing(h);
      } while (h < maxDigitH && sizerBR.x >= timeBR.x && sizerBR.y >= timeBR.y);
      h--;
   }
   // In all other cases, we need to decrease current size to fit within new size
   else if (sizerBR.x < timeBR.x || sizerBR.y < timeBR.y) {
      while (h > minDigitH && (sizerBR.x < timeBR.x || sizerBR.y < timeBR.y)) {
         h--;
         timeBR = ComputeSizing(h);
      }
   }

   if (h != mAudioTime->GetDigitSize().y) {
      mAudioTime->SetDigitSize(h * mDigitRatio, h);
   }

   // Redraw the display immediately to smooth out resizing
   Update();
}

void TimeToolBar::OnIdle(wxIdleEvent &evt)
{
   evt.Skip();

   double audioTime;

   auto &projectAudioIO = ProjectAudioIO::Get(mProject);
   if (projectAudioIO.IsAudioActive()) {
      auto gAudioIO = AudioIO::Get();
      audioTime = gAudioIO->GetStreamTime();
   }
   else {
      const auto &playRegion = ViewInfo::Get(mProject).playRegion;
      audioTime = playRegion.GetStart();
   }

   mAudioTime->SetValue(wxMax(0.0, audioTime));
}

wxSize TimeToolBar::ComputeSizing(int digitH)
{
   return mAudioTime->ComputeSizing(false, digitH * mDigitRatio, digitH);
}

void TimeToolBar::AddSnapControls(wxBoxSizer &rows)
{
   mSnapRow = safenew wxBoxSizer(wxHORIZONTAL);
   mSnapModeCheckBox = safenew wxCheckBox(this, wxID_ANY, {});

   mSnapModeCheckBox->SetName(SnapLabel.Stripped().Translation());
#if wxUSE_ACCESSIBILITY
   // so that name can be set on a standard control
   mSnapModeCheckBox->SetAccessible(
      safenew WindowAccessible(mSnapModeCheckBox));
#endif

   mSnapModeCheckBox->SetToolTip(SnapLabel.Translation());
   mSnapRow->Add(mSnapModeCheckBox, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

   const bool snapEnabled =
      ProjectSnap::Get(mProject).GetSnapMode() != SnapMode::SNAP_OFF;

   mSnapModeCheckBox->SetValue(snapEnabled);


   mSnapToCombo = safenew wxComboCtrl(
      this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize /*, wxCB_READONLY*/);
#if wxUSE_ACCESSIBILITY
   // so that name can be set on a standard control
   mSnapToCombo->GetTextCtrl()->SetAccessible(
      safenew WindowAccessible(mSnapToCombo->GetTextCtrl()));
#endif

   mSnapToCombo->SetPopupControl(safenew SnapModePopup(mProject));
   /* i18n-hint: combo box is the type of the control/widget */
   mSnapToCombo->GetTextCtrl()->SetName(XO("Snap to combo box").Translation());
   /* Narrator screen reader by default reads the accessibility name of the
   containing window, which by default is combobox, so set it to an empty string. */
   mSnapToCombo->SetLabel(wxT(""));
   mSnapToCombo->Enable(snapEnabled);
   mSnapToCombo->SetMinSize(wxSize(150, -1));

   mSnapRow->Add(mSnapToCombo, 1, wxEXPAND);

   mSnapModeCheckBox->Bind(
      wxEVT_CHECKBOX, [this](auto&) { OnSnapModeChanged(); });

   mSnapModeCheckBox->Bind(
      wxEVT_CHAR_HOOK,
      [this](auto& evt)
      {
         const auto keyCode = evt.GetKeyCode();

         if (keyCode != WXK_NUMPAD_ENTER && keyCode != WXK_RETURN)
         {
            evt.Skip();
            return;
         }

         mSnapModeCheckBox->SetValue(!mSnapModeCheckBox->GetValue());

         OnSnapModeChanged();
      });

   mSnapToCombo->Bind(wxEVT_KILL_FOCUS,
      [this](auto &event) { mSnapToCombo->SelectNone(); event.Skip(); });
   mSnapSubscription = ProjectSnap::Get(mProject).Subscribe([this](auto settings) {
      const bool enabled = settings.newSnapMode != SnapMode::SNAP_OFF;
      mSnapModeCheckBox->SetValue(enabled);
      mSnapToCombo->Enable(enabled);
   });
   rows.Add(mSnapRow, 0, wxEXPAND | wxBOTTOM, 3);
}

void TimeToolBar::OnSnapModeChanged()
{
   const bool snapEnabled = mSnapModeCheckBox->GetValue();

   ProjectSnap::Get(mProject).SetSnapMode(
      snapEnabled ? SnapMode::SNAP_NEAREST : SnapMode::SNAP_OFF);

   mSnapToCombo->Enable(snapEnabled);


   // wxEVT_KILL_FOCUS is not always sent by wxWidgets.
   // Remove any selection from the combo box if we've disabled it.
   if (!snapEnabled)
      mSnapToCombo->SelectNone();
}

static RegisteredToolbarFactory factory{
   []( AudacityProject &project )
   {
      return ToolBar::Holder{ safenew TimeToolBar{ project } };
   }
};

namespace {
AttachedToolBarMenuItem sAttachment
{
   TimeToolBar::ID(),
   wxT("ShowTimeTB"),
   /* i18n-hint: Clicking this menu item shows the toolbar
      for viewing actual time of the cursor */
   XXO("&Time Toolbar"),
   {
      Registry::OrderingHint::After,
      "ShowSelectionTB"
   }
};
}

