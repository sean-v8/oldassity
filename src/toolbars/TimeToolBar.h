/**********************************************************************

  Audacity: A Digital Audio Editor

  TimeToolBar.h

  Jonatã Bolzan Loss

**********************************************************************/

#ifndef __AUDACITY_TIME_TOOLBAR__
#define __AUDACITY_TIME_TOOLBAR__

#include <wx/defs.h>

#include "ToolBar.h"
#include "../widgets/NumericTextCtrl.h"
#include "Observer.h"

class NumericTextCtrl;
class wxComboCtrl;
class wxCheckBox;
class wxBoxSizer;
class TimeToolBarListener;

class TimeToolBar final : public ToolBar
{
public:
   static Identifier ID();

   TimeToolBar(AudacityProject &project);
   virtual ~TimeToolBar();
   
   DockID DefaultDockID() const override;

   static TimeToolBar &Get(AudacityProject &project);
   static const TimeToolBar &Get(const AudacityProject &project);
   
   void Populate() override;
   void Repaint(wxDC * WXUNUSED(dc)) override {};
   void EnableDisableButtons() override {};
   void UpdatePrefs() override;
   void RegenerateTooltips() override {};
   int GetInitialWidth() override { return 310; }
   int GetMinToolbarWidth() override {return 50;}
   int GetMinToolbarHeight() override { return toolbarTriple; }
   void SetToDefaultSize() override;
   wxSize GetDockedSize() override;
   void SetDocked(ToolDock *dock, bool pushed) override;
   void SetListener(TimeToolBarListener *l);
   void SetAudioTimeFormat(const NumericFormatSymbol & format);
   void ResizingDone() override;

private:
   void SetResizingLimits();
   void AddSnapControls(wxBoxSizer &rows);
   void OnSnapModeChanged();
   wxBoxSizer *mTimeRow{};
   wxBoxSizer *mSnapRow{};
   wxCheckBox *mSnapModeCheckBox{};
   wxComboCtrl *mSnapToCombo{};
   Observer::Subscription mSnapSubscription;
   wxSize ComputeSizing(int digitH);
   
   void OnUpdate(wxCommandEvent &evt);
   void OnSize(wxSizeEvent &evt);
   void OnIdle(wxIdleEvent &evt);

   TimeToolBarListener *mListener;
   NumericTextCtrl *mAudioTime;
   float mDigitRatio;
   bool mSettingInitialSize;

   static const int minDigitH = 32;
   static const int maxDigitH = 100;

   Observer::Subscription mFormatChangedToFitValueSubscription;

public:
   
   DECLARE_CLASS(TimeToolBar)
   DECLARE_EVENT_TABLE()
};

#endif
