/**********************************************************************

  Audacity: A Digital Audio Editor

  MeterToolbar.h

  Dominic Mazzoni
  Leland Lucius

  ToolBar to hold the VU Meter

**********************************************************************/

#ifndef __AUDACITY_METER_TOOLBAR__
#define __AUDACITY_METER_TOOLBAR__

#include <functional>
#include <vector>
#include "ToolBar.h"

class wxDC;
class wxGridBagSizer;
class wxSizeEvent;
class wxWindow;

class AudacityProject;
class MeterPanel;
class MeterToolBar final : public ToolBar {
public:
   static Identifier ID();
   explicit MeterToolBar(AudacityProject &project);
   static MeterToolBar &Get(AudacityProject &project);
   DockID DefaultDockID() const override { return BotDockID; }

   void Populate() override;
   void ReCreateButtons() override;
   void Repaint(wxDC * WXUNUSED(dc)) override {};
   void EnableDisableButtons() override {};
   void UpdatePrefs() override;
   void UpdateControls();

   bool Expose(bool show) override;

   int GetInitialWidth() override;
   int GetMinToolbarWidth()  override { return 150; }
   int GetMinToolbarHeight() override { return toolbarTriple; }
   wxSize GetDockedSize() override { return {GetSize().x, toolbarTriple}; }

   void ShowOutputGainDialog();
   void ShowInputGainDialog();

   void AdjustOutputGain(int adj);
   void AdjustInputGain(int adj);

 private:
   void RegenerateTooltips() override {}
   AButton* mPlaySetupButton{nullptr};
   MeterPanel *mPlayMeter{nullptr};
   AButton* mRecordSetupButton{nullptr};
   MeterPanel *mRecordMeter{nullptr};

 public:

   DECLARE_CLASS(MeterToolBar)

};

#endif

