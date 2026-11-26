/*!********************************************************************

   Audacity: A Digital Audio Editor

   @file RealtimeEffectPanel.cpp

   @author Vitaly Sverchinsky

**********************************************************************/

#include "RealtimeEffectPanel.h"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/statline.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/menu.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <wx/wupdlock.h>

#include <wx/dcbuffer.h>

#include "HelpSystem.h"
#include "Theme.h"
#include "AllThemeResources.h"
#include "AudioIO.h"
#include "BasicUI.h"
#include "Observer.h"
#include "PluginManager.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectWindow.h"
#include "ProjectWindows.h"
#include "TrackPanelAx.h"
#include "AColor.h"
#include "WaveTrack.h"
#include "effects/EffectUI.h"
#include "effects/EffectManager.h"
#include "RealtimeEffectList.h"
#include "RealtimeEffectState.h"
#include "effects/RealtimeEffectStateUI.h"
#include "UndoManager.h"
#include "Prefs.h"
#include "BasicUI.h"
#include "ListNavigationEnabled.h"
#include "ListNavigationPanel.h"
#include "MovableControl.h"
#include "menus/MenuHelper.h"
#include "Menus.h"
#include "prefs/EffectsPrefs.h"

#if wxUSE_ACCESSIBILITY
#include "WindowAccessible.h"
#endif

namespace
{
   
   constexpr auto favoritesKey = wxT("/GUI/RealtimeEffectFavorites");

   wxArrayString ReadFavorites()
   {
      return wxStringTokenize(gPrefs->Read(favoritesKey, wxString{}), "\n");
   }

   void SaveFavorites(const wxArrayString &favorites)
   {
      gPrefs->Write(favoritesKey, wxJoin(favorites, '\n', '\0'));
      gPrefs->Flush();
   }

   using FavoriteMenuItems = std::vector<std::pair<int, wxString>>;

   FavoriteMenuItems AppendFavoriteItems(wxMenu &menu, const wxArrayString &favorites)
   {
      FavoriteMenuItems items;
      for (const auto &id : favorites) {
         const auto *plugin = PluginManager::Get().GetPlugin(id);
         if (plugin && plugin->IsEnabled() && plugin->IsEffectRealtime()) {
            auto *item = menu.Append(wxID_ANY,
               plugin->GetSymbol().Msgid().Translation());
            items.emplace_back(item->GetId(), id);
         }
      }
      if (items.empty())
         menu.Append(wxID_ANY, _("No favorites yet"))->Enable(false);
      return items;
   }

   class RealtimeEffectsMenuVisitor final : public MenuVisitor
   {
      wxMenu& mMenu;
      wxMenu* mMenuPtr { nullptr };
      int mMenuItemIdCounter { wxID_HIGHEST };
      std::vector<Identifier> mIndexedPluginList;
      int mMenuLevelCounter { 0 };
   public:
      
      RealtimeEffectsMenuVisitor(wxMenu& menu)
         : mMenu(menu), mMenuPtr(&mMenu) { }
      
      void DoBeginGroup( MenuTable::GroupItemBase &item, const Path& ) override
      {
         if(auto menuItem = dynamic_cast<MenuTable::MenuItem*>(&item))
         {
            //Don't create a group item for root
            if (mMenuLevelCounter != 0)
            {
               auto submenu = std::make_unique<wxMenu>();
               mMenuPtr->AppendSubMenu(submenu.get(), menuItem->GetTitle().Translation());
               mMenuPtr = submenu.release();
            }
            ++mMenuLevelCounter;
         }
      }

      void DoEndGroup( MenuTable::GroupItemBase &item, const Path& ) override
      {
         if(auto menuItem = dynamic_cast<MenuTable::MenuItem*>(&item))
         {
            --mMenuLevelCounter;
            if (mMenuLevelCounter != 0)
            {
               assert(mMenuPtr->GetParent() != nullptr);
               mMenuPtr = mMenuPtr->GetParent();
            }
         }
      }

      void DoVisit( MenuTable::SingleItem &item, const Path& ) override
      {
         if(auto commandItem = dynamic_cast<MenuTable::CommandItem*>(&item))
         {
            mMenuPtr->Append(mMenuItemIdCounter, commandItem->label_in.Translation());
            mIndexedPluginList.push_back(commandItem->name);
            ++mMenuItemIdCounter;
         }
      }

      void DoSeparator() override
      {
         mMenuPtr->AppendSeparator();
      }
      
      Identifier GetPluginID(int menuIndex) const
      {
         assert(menuIndex >= wxID_HIGHEST && menuIndex < (wxID_HIGHEST + mIndexedPluginList.size()));
         return mIndexedPluginList[menuIndex - wxID_HIGHEST];
      }
      
   };

   template <typename Visitor>
   void VisitRealtimeEffectStateUIs(SampleTrack& track, Visitor&& visitor)
   {
      if (!track.IsLeader())
         return;
      auto& effects = RealtimeEffectList::Get(track);
      effects.Visit(
         [visitor](auto& effectState, bool)
         {
            auto& ui = RealtimeEffectStateUI::Get(effectState);
            visitor(ui);
         });
   }

   void UpdateRealtimeEffectUIData(SampleTrack& track)
   {
      VisitRealtimeEffectStateUIs(
         track, [&](auto& ui) { ui.UpdateTrackData(track); });
   }

   void ReopenRealtimeEffectUIData(AudacityProject& project, SampleTrack& track)
   {
      VisitRealtimeEffectStateUIs(
         track,
         [&](auto& ui)
         {
            if (ui.IsShown())
            {
               ui.Hide(&project);
               ui.Show(project);
            }
         });
   }
   //fwd
   class RealtimeEffectControl;

   class DropHintLine : public wxWindow
   {
   public:
      DropHintLine(wxWindow *parent,
                wxWindowID id,
                const wxPoint& pos = wxDefaultPosition,
                const wxSize& size = wxDefaultSize)
                   : wxWindow(parent, id, pos, size, wxNO_BORDER, wxEmptyString)
      {
         wxWindow::SetBackgroundStyle(wxBG_STYLE_PAINT);
         Bind(wxEVT_PAINT, &DropHintLine::OnPaint, this);
      }
      
      bool AcceptsFocus() const override { return false; }

   private:
      void OnPaint(wxPaintEvent&)
      {
         wxBufferedPaintDC dc(this);
         const auto rect = wxRect(GetSize());

         dc.SetPen(*wxTRANSPARENT_PEN);
         dc.SetBrush(GetBackgroundColour());
         dc.DrawRectangle(rect);
      }
   };

#if wxUSE_ACCESSIBILITY
   class RealtimeEffectControlAx : public wxAccessible
   {
   public:
      RealtimeEffectControlAx(wxWindow* win = nullptr) : wxAccessible(win) { }

      wxAccStatus GetName(int childId, wxString* name) override
      {
         if(childId != wxACC_SELF)
            return wxACC_NOT_IMPLEMENTED;
         
         if(auto movable = wxDynamicCast(GetWindow(), MovableControl))
            //i18n-hint: argument - position of the effect in the effect stack
            *name = wxString::Format(_("Effect %d"), movable->FindIndexInParent() + 1);
         return wxACC_OK;
      }

      wxAccStatus GetChildCount(int* childCount) override
      {
         const auto window = GetWindow();
         *childCount = window->GetChildren().size();
         return wxACC_OK;
      }

      wxAccStatus GetChild(int childId, wxAccessible** child) override
      {
         if(childId == wxACC_SELF)
            *child = this;
         else
         {
            const auto window = GetWindow();
            const auto& children = window->GetChildren();
            const auto childIndex = childId - 1;
            if(childIndex < children.size())
               *child = children[childIndex]->GetAccessible();
            else
               *child = nullptr;
         }
         return wxACC_OK;
      }

      wxAccStatus GetRole(int childId, wxAccRole* role) override
      {
         if(childId != wxACC_SELF)
            return wxACC_NOT_IMPLEMENTED;

         *role = wxROLE_SYSTEM_PANE;
         return wxACC_OK;
      }

      wxAccStatus GetState(int childId, long* state) override
      {
         if(childId != wxACC_SELF)
            return wxACC_NOT_IMPLEMENTED;

         const auto window = GetWindow();
         if(!window->IsEnabled())
            *state = wxACC_STATE_SYSTEM_UNAVAILABLE;
         else
         {
            *state = wxACC_STATE_SYSTEM_FOCUSABLE;
            if(window->HasFocus())
               *state |= wxACC_STATE_SYSTEM_FOCUSED;
         }
         return wxACC_OK;
      }
   };
#endif

   class RealtimeEffectPicker
   {
   public:
      virtual std::optional<wxString> PickEffect(wxWindow* parent, const wxString& selectedEffectID) = 0;
   };

   //UI control that represents individual effect from the effect list
   class RealtimeEffectControl : public ListNavigationEnabled<MovableControl>
   {
      wxWeakRef<AudacityProject> mProject;
      std::shared_ptr<SampleTrack> mTrack;
      std::shared_ptr<RealtimeEffectState> mEffectState;
      std::shared_ptr<EffectSettingsAccess> mSettingsAccess;
      
      RealtimeEffectPicker* mEffectPicker { nullptr };

      ThemedAButtonWrapper<AButton>* mChangeButton{nullptr};
      AButton* mEnableButton{nullptr};
      ThemedAButtonWrapper<AButton>* mOptionsButton{};

      Observer::Subscription mSubscription;

   public:
      RealtimeEffectControl() = default;

      RealtimeEffectControl(wxWindow* parent,
                   RealtimeEffectPicker* effectPicker,
                   wxWindowID winid,
                   const wxPoint& pos = wxDefaultPosition,
                   const wxSize& size = wxDefaultSize)
      {
         Create(parent, effectPicker, winid, pos, size);
      }

      void Create(wxWindow* parent,
                   RealtimeEffectPicker* effectPicker,
                   wxWindowID winid,
                   const wxPoint& pos = wxDefaultPosition,
                   const wxSize& size = wxDefaultSize)
      {
         mEffectPicker = effectPicker;
         
         //Prevents flickering and paint order issues
         MovableControl::SetBackgroundStyle(wxBG_STYLE_PAINT);
         MovableControl::Create(parent, winid, pos, size, wxNO_BORDER | wxWANTS_CHARS);

         Bind(wxEVT_PAINT, &RealtimeEffectControl::OnPaint, this);
         Bind(wxEVT_SET_FOCUS, &RealtimeEffectControl::OnFocusChange, this);
         Bind(wxEVT_KILL_FOCUS, &RealtimeEffectControl::OnFocusChange, this);

         auto sizer = std::make_unique<wxBoxSizer>(wxHORIZONTAL);

         //On/off button
         auto enableButton = safenew ThemedAButtonWrapper<AButton>(this);
         enableButton->SetTranslatableLabel(XO("Power"));
         enableButton->SetImageIndices(0, bmpEffectOff, bmpEffectOff, bmpEffectOn, bmpEffectOn, bmpEffectOff);
         enableButton->SetButtonToggles(true);
         enableButton->SetBackgroundColorIndex(clrEffectListItemBackground);
         mEnableButton = enableButton;

         enableButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {

            mEffectState->SetActive(mEnableButton->IsDown());
            if (mProject)
               UndoManager::Get(*mProject).MarkUnsaved();
         });

         //Central button with effect name, show settings
         const auto optionsButton = safenew ThemedAButtonWrapper<AButton>(this, wxID_ANY);
         optionsButton->SetImageIndices(0,
            bmpHButtonNormal,
            bmpHButtonHover,
            bmpHButtonDown,
            bmpHButtonHover,
            bmpHButtonDisabled);
         optionsButton->SetBackgroundColorIndex(clrEffectListItemBackground);
         optionsButton->SetForegroundColorIndex(clrTrackPanelText);
         optionsButton->SetButtonType(AButton::TextButton);
         optionsButton->Bind(wxEVT_BUTTON, &RealtimeEffectControl::OnOptionsClicked, this);

         //Remove/replace effect
         auto changeButton = safenew ThemedAButtonWrapper<AButton>(this);
         changeButton->SetImageIndices(0, bmpMoreNormal, bmpMoreHover, bmpMoreDown, bmpMoreHover, bmpMoreDisabled);
         changeButton->SetBackgroundColorIndex(clrEffectListItemBackground);
         changeButton->SetTranslatableLabel(XO("Replace effect"));
         changeButton->Bind(wxEVT_BUTTON, &RealtimeEffectControl::OnChangeButtonClicked, this);
         
         auto dragArea = safenew wxStaticBitmap(this, wxID_ANY, theTheme.Bitmap(bmpDragArea));
         dragArea->Disable();
         sizer->Add(dragArea, 0, wxLEFT | wxCENTER, 5);
         sizer->Add(enableButton, 0, wxLEFT | wxCENTER, 5);
         sizer->Add(optionsButton, 1, wxLEFT | wxCENTER, 5);
         sizer->Add(changeButton, 0, wxLEFT | wxRIGHT | wxCENTER, 5);
         mChangeButton = changeButton;
         mOptionsButton = optionsButton;

         auto vSizer = std::make_unique<wxBoxSizer>(wxVERTICAL);
         vSizer->Add(sizer.release(), 0, wxUP | wxDOWN | wxEXPAND, 10);

         SetSizer(vSizer.release());

#if wxUSE_ACCESSIBILITY
         SetAccessible(safenew RealtimeEffectControlAx(this));
#endif
      }

      static const PluginDescriptor *GetPlugin(const PluginID &ID) {
         auto desc = PluginManager::Get().GetPlugin(ID);
         return desc;
      }

      //! @pre `mEffectState != nullptr`
      TranslatableString GetEffectName() const
      {
         const auto &ID = mEffectState->GetID();
         const auto desc = GetPlugin(ID);
         return desc
            ? desc->GetSymbol().Msgid()
            : XO("%s (missing)")
               .Format(PluginManager::GetEffectNameFromID(ID).GET());
      }

      void SetEffect(AudacityProject& project,
         const std::shared_ptr<SampleTrack>& track,
         const std::shared_ptr<RealtimeEffectState> &pState)
      {
         mProject = &project;
         mTrack = track;
         mEffectState = pState;

         mSubscription = mEffectState->Subscribe([this](RealtimeEffectStateChange state) {
            state == RealtimeEffectStateChange::EffectOn 
               ? mEnableButton->PushDown() 
               : mEnableButton->PopUp();

            if (mProject)
               ProjectHistory::Get(*mProject).ModifyState(false);
         });

         TranslatableString label;
         if (pState) {
            label = GetEffectName();
            mSettingsAccess = pState->GetAccess();
         }
         else
            mSettingsAccess.reset();
         if (mEnableButton)
            mSettingsAccess && mSettingsAccess->Get().extra.GetActive()
               ? mEnableButton->PushDown()
               : mEnableButton->PopUp();
         if (mOptionsButton)
         {
            mOptionsButton->SetTranslatableLabel(label);
            mOptionsButton->SetEnabled(pState && GetPlugin(pState->GetID()));
         }
      }

      void RemoveFromList()
      {
         if(mProject == nullptr || mEffectState == nullptr)
            return;

         auto& ui = RealtimeEffectStateUI::Get(*mEffectState);
         // Don't need autosave for the effect that is being removed
         ui.Hide();

         auto effectName = GetEffectName();
         //After AudioIO::RemoveState call this will be destroyed
         auto project = mProject.get();
         auto trackName = mTrack ? mTrack->GetName() : _("Master");

         AudioIO::Get()->RemoveState(*project, mTrack.get(), mEffectState);
         ProjectHistory::Get(*project).PushState(
            /*! i18n-hint: undo history record
             first parameter - realtime effect name
             second parameter - track name
             */
            XO("Removed %s from %s").Format(effectName, trackName),
            /*! i18n-hint: undo history record
             first parameter - realtime effect name */
            XO("Remove %s").Format(effectName)
         );
      }

      void OnOptionsClicked(wxCommandEvent& event)
      {
         if(mProject == nullptr || mEffectState == nullptr)
            return;//not initialized

         const auto ID = mEffectState->GetID();
         const auto effectPlugin = EffectManager::Get().GetEffect(ID);

         if(effectPlugin == nullptr)
         {
            ///TODO: effect is not available
            return;
         }

         auto& effectStateUI = RealtimeEffectStateUI::Get(*mEffectState);

         if (mTrack)
            effectStateUI.UpdateTrackData(*mTrack);
         else
            effectStateUI.UpdateMasterData();
         effectStateUI.Toggle( *mProject );
      }

      void OnChangeButtonClicked(wxCommandEvent& event)
      {
         if(mProject == nullptr)
            return;
         if(mEffectState == nullptr)
            return;//not initialized

         const auto effectID = mEffectPicker->PickEffect(mChangeButton, mEffectState->GetID());
         if(!effectID)
            return;//nothing
         
         if(effectID->empty())
         {
            RemoveFromList();
            return;
         }
         
         auto &em = RealtimeEffectManager::Get(*mProject);
         auto oIndex = em.FindState(mTrack.get(), mEffectState);
         if (!oIndex)
            return;

         auto oldName = GetEffectName();
         auto &project = *mProject;
         if (auto state = AudioIO::Get()
            ->ReplaceState(project, mTrack.get(), *oIndex, *effectID)
         ){
            // Message subscription took care of updating the button text
            // and destroyed `this`!
            auto effect = state->GetEffect();
            assert(effect); // postcondition of ReplaceState
            ProjectHistory::Get(project).PushState(
               /*i18n-hint: undo history,
                first and second parameters - realtime effect names
                */
               XO("Replaced %s with %s")
                  .Format(oldName, effect->GetName()),
               /*! i18n-hint: undo history record
                first parameter - realtime effect name */
               XO("Replace %s").Format(oldName));
         }
      }

      void OnPaint(wxPaintEvent&)
      {
         wxBufferedPaintDC dc(this);
         const auto rect = wxRect(GetSize());

         dc.SetPen(*wxTRANSPARENT_PEN);
         dc.SetBrush(GetBackgroundColour());
         dc.DrawRectangle(rect);

         dc.SetPen(theTheme.Colour(clrEffectListItemBorder));
         dc.SetBrush(theTheme.Colour(clrEffectListItemBorder));
         dc.DrawLine(rect.GetBottomLeft(), rect.GetBottomRight());

         if(HasFocus())
            AColor::DrawFocus(dc, GetClientRect().Deflate(3, 3));
      }

      void OnFocusChange(wxFocusEvent& evt)
      {
         Refresh(false);
         evt.Skip();
      }
   };

   static wxString GetSafeVendor(const PluginDescriptor& descriptor)
   {
      if (descriptor.GetVendor().empty())
         return XO("Unknown").Translation();

      return descriptor.GetVendor();
   }
}

class RealtimeEffectListWindow
   : public wxScrolledWindow
   , public RealtimeEffectPicker
   , public PrefsListener
{
   wxWeakRef<AudacityProject> mProject;
   std::shared_ptr<SampleTrack> mTrack;
   RealtimeEffectList *mStack{};
   AButton* mAddEffect{nullptr};
   AButton* mAddFavorite{nullptr};
   wxWindow* mEffectListContainer{nullptr};

   std::unique_ptr<MenuTable::MenuItem> mEffectMenuRoot;
   
   Observer::Subscription mEffectListItemMovedSubscription;
   Observer::Subscription mPluginsChangedSubscription;

public:
   RealtimeEffectListWindow(wxWindow *parent,
                     wxWindowID winid = wxID_ANY,
                     const wxPoint& pos = wxDefaultPosition,
                     const wxSize& size = wxDefaultSize,
                     long style = wxScrolledWindowStyle,
                     const wxString& name = wxPanelNameStr)
      : wxScrolledWindow(parent, winid, pos, size, style, name)
   {
#ifdef __WXMSW__
      //Fixes flickering on redraw
      wxScrolledWindow::SetDoubleBuffered(true);
#endif
      auto rootSizer = std::make_unique<wxBoxSizer>(wxVERTICAL);

      auto effectListContainer = safenew ThemedWindowWrapper<wxPanel>(this, wxID_ANY);
      effectListContainer->SetBackgroundColorIndex(clrMedium);
      effectListContainer->SetSizer(safenew wxBoxSizer(wxVERTICAL));
      effectListContainer->SetDoubleBuffered(true);
      effectListContainer->Hide();
      mEffectListContainer = effectListContainer;

      const auto addButton = [this](const TranslatableString &text, bool favorite) {
         auto *button = safenew ThemedAButtonWrapper<AButton>(this, wxID_ANY);
         button->SetImageIndices(0, bmpHButtonNormal, bmpHButtonHover,
            bmpHButtonDown, bmpHButtonHover, bmpHButtonDisabled);
         button->SetTranslatableLabel(text);
         button->SetButtonType(AButton::TextButton);
         button->SetBackgroundColorIndex(clrMedium);
         button->SetForegroundColorIndex(clrTrackPanelText);
         button->Bind(wxEVT_BUTTON, [this, button, favorite](wxCommandEvent &) {
            OnAddEffectClicked(*button, favorite);
         });
         return button;
      };
      mAddEffect = addButton(XO("+ Effect"), false);
      mAddFavorite = addButton(XO("+ Favorite"), true);

      //indicates the insertion position of the item
      auto dropHintLine = safenew ThemedWindowWrapper<DropHintLine>(effectListContainer, wxID_ANY);
      dropHintLine->SetBackgroundColorIndex(clrDropHintHighlight);
      dropHintLine->Hide();

      rootSizer->Add(mEffectListContainer, 0, wxEXPAND | wxBOTTOM, 10);
      auto *buttons = safenew wxBoxSizer(wxHORIZONTAL);
      buttons->Add(mAddEffect, 1, wxEXPAND | wxRIGHT, 4);
      buttons->Add(mAddFavorite, 1, wxEXPAND);
      rootSizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 20);

      SetSizer(rootSizer.release());
      SetMinSize({});

      Bind(EVT_MOVABLE_CONTROL_DRAG_STARTED, [dropHintLine](const MovableControlEvent& event)
      {
         if(auto window = dynamic_cast<wxWindow*>(event.GetEventObject()))
            window->Raise();
      });
      Bind(EVT_MOVABLE_CONTROL_DRAG_POSITION, [this, dropHintLine](const MovableControlEvent& event)
      {
         constexpr auto DropHintLineHeight { 3 };//px

         auto sizer = mEffectListContainer->GetSizer();
         assert(sizer != nullptr);

         if(event.GetSourceIndex() == event.GetTargetIndex())
         {
            //do not display hint line if position didn't change
            dropHintLine->Hide();
            return;
         }

         if(!dropHintLine->IsShown())
         {
            dropHintLine->Show();
            dropHintLine->Raise();
            if(auto window = dynamic_cast<wxWindow*>(event.GetEventObject()))
               window->Raise();
         }

         auto item = sizer->GetItem(event.GetTargetIndex());
         dropHintLine->SetSize(item->GetSize().x, DropHintLineHeight);

         if(event.GetTargetIndex() > event.GetSourceIndex())
            dropHintLine->SetPosition(item->GetRect().GetBottomLeft() - wxPoint(0, DropHintLineHeight));
         else
            dropHintLine->SetPosition(item->GetRect().GetTopLeft());
      });
      Bind(EVT_MOVABLE_CONTROL_DRAG_FINISHED, [this, dropHintLine](const MovableControlEvent& event)
      {
         dropHintLine->Hide();

         if(mProject == nullptr)
            return;

         auto& effectList = *mStack;
         const auto from = event.GetSourceIndex();
         const auto to = event.GetTargetIndex();
         if(from != to)
         {
            auto effectName =
               effectList.GetStateAt(from)->GetEffect()->GetName();
            bool up = (to < from);
            effectList.MoveEffect(from, to);
            ProjectHistory::Get(*mProject).PushState(
               (up
                  /*! i18n-hint: undo history record
                   first parameter - realtime effect name
                   second parameter - track name
                   */
                  ? XO("Moved %s up in %s")
                  /*! i18n-hint: undo history record
                   first parameter - realtime effect name
                   second parameter - track name
                   */
                  : XO("Moved %s down in %s"))
                  .Format(effectName, mTrack ? mTrack->GetName() : _("Master")),
               XO("Change effect order"), UndoPush::CONSOLIDATE);
         }
         else
         {
            wxWindowUpdateLocker freeze(this);
            Layout();
         }
      });
      SetScrollRate(0, 20);
      
      mPluginsChangedSubscription = PluginManager::Get().Subscribe(
         [this](PluginsChangedMessage)
         {
            UpdateEffectMenuItems();
         });
      UpdateEffectMenuItems();
   }

   void UpdatePrefs() override
   {
      UpdateEffectMenuItems();
   }
   
   std::optional<wxString> PickEffect(wxWindow* parent, const wxString& selectedEffectID) override
   {
      wxMenu menu;
      if(!selectedEffectID.empty())
      {
         //no need to handle language change since menu creates its own event loop
         menu.Append(wxID_REMOVE, _("No Effect"));
         menu.AppendSeparator();
      }
      
      auto favorites = ReadFavorites();
      const bool alreadyFavorite = favorites.Index(selectedEffectID) != wxNOT_FOUND;
      auto favoriteMenu = std::make_unique<wxMenu>();
      const auto shortcuts = AppendFavoriteItems(*favoriteMenu, favorites);

      int toggleFavorite = wxID_NONE;
      if (!selectedEffectID.empty()) {
         favoriteMenu->AppendSeparator();
         toggleFavorite = favoriteMenu->Append(wxID_ANY, alreadyFavorite
            ? _("Remove current effect from Favorites")
            : _("Add current effect to Favorites"))->GetId();
      }
      menu.AppendSubMenu(favoriteMenu.release(), _("Favorites"));
      menu.AppendSeparator();

      RealtimeEffectsMenuVisitor visitor { menu };
      
      Registry::Visit(visitor, mEffectMenuRoot.get());
      
      int commandId = wxID_NONE;
      
      menu.AppendSeparator();
      menu.Append(wxID_MORE, _("Get more effects..."));
      
      menu.Bind(wxEVT_MENU, [&](wxCommandEvent evt) { commandId = evt.GetId(); });
      
      if(parent->PopupMenu(&menu, parent->GetClientRect().GetLeftBottom()) && commandId != wxID_NONE)
      {
         if(commandId == wxID_REMOVE)
            return wxString {};
         else if(commandId == wxID_MORE)
            OpenInDefaultBrowser("https://plugins.audacityteam.org/");
         else if (commandId == toggleFavorite) {
            if (alreadyFavorite)
               favorites.Remove(selectedEffectID);
            else
               favorites.Add(selectedEffectID);
            SaveFavorites(favorites);
         }
         else {
            const auto shortcut = std::find_if(shortcuts.begin(), shortcuts.end(),
               [commandId](const auto &entry) { return entry.first == commandId; });
            if (shortcut != shortcuts.end()) {
               if (shortcut->second != selectedEffectID)
                  return shortcut->second;
            }
            else
               return visitor.GetPluginID(commandId).GET();
         }
      }
      
      return {};
   }
   
   void UpdateEffectMenuItems()
   {
      using namespace MenuTable;
      auto root = Menu("", TranslatableString{});

      static auto realtimeEffectPredicate = [](const PluginDescriptor& desc)
      {
         return desc.IsEffectRealtime();
      };

      const auto groupby = RealtimeEffectsGroupBy.Read();

      auto analyzeSection = Section("", Menu("", XO("Analyze")));
      auto submenu =
         static_cast<MenuItem*>(analyzeSection->begin()->get());
      MenuHelper::PopulateEffectsMenu(
         *submenu,
         EffectTypeAnalyze,
         {}, groupby, nullptr,
         realtimeEffectPredicate
      );
      
      if(!submenu->empty())
      {
         root->push_back(move(analyzeSection));
      }
   
      MenuHelper::PopulateEffectsMenu(
         *root,
         EffectTypeProcess,
         {}, groupby, nullptr,
         realtimeEffectPredicate
      );
      
      mEffectMenuRoot.swap(root);
   }
   
   void OnEffectListItemChange(const RealtimeEffectListMessage& msg)
   {
      if (msg.type == RealtimeEffectListMessage::Type::Reset) {
         ReloadEffectsList();
         return;
      }
      auto sizer = mEffectListContainer->GetSizer();
      const auto insertItem = [this, &msg](){
         auto& effects = *mStack;
         InsertEffectRow(msg.srcIndex, effects.GetStateAt(msg.srcIndex));
      };
      const auto removeItem = [&](){
         auto& ui = RealtimeEffectStateUI::Get(*msg.affectedState);
         // Don't need to auto-save changed settings of effect that is deleted
         // Undo history push will do it anyway
         ui.Hide();
         
         auto window = sizer->GetItem(msg.srcIndex)->GetWindow();
         sizer->Remove(msg.srcIndex);
         wxTheApp->CallAfter([ref = wxWeakRef { window }] {
            if(ref) ref->Destroy();
         });
         
         if(sizer->IsEmpty())
         {
            if(mEffectListContainer->IsDescendant(FindFocus()))
               mAddEffect->SetFocus();
            
            mEffectListContainer->Hide();
         }
      };

      wxWindowUpdateLocker freeze(this);
      if(msg.type == RealtimeEffectListMessage::Type::Move)
      {
         const auto sizer = mEffectListContainer->GetSizer();

         const auto movedItem = sizer->GetItem(msg.srcIndex);

         const auto proportion = movedItem->GetProportion();
         const auto flag = movedItem->GetFlag();
         const auto border = movedItem->GetBorder();
         const auto window = movedItem->GetWindow();

         if(msg.srcIndex < msg.dstIndex)
            window->MoveAfterInTabOrder(sizer->GetItem(msg.dstIndex)->GetWindow());
         else
            window->MoveBeforeInTabOrder(sizer->GetItem(msg.dstIndex)->GetWindow());
         
         sizer->Remove(msg.srcIndex);
         sizer->Insert(msg.dstIndex, window, proportion, flag, border);
      }
      else if(msg.type == RealtimeEffectListMessage::Type::Insert)
      {
         insertItem();
      }
      else if(msg.type == RealtimeEffectListMessage::Type::WillReplace)
      {
         removeItem();
      }
      else if(msg.type == RealtimeEffectListMessage::Type::DidReplace)
      {
         insertItem();
      }
      else if(msg.type == RealtimeEffectListMessage::Type::Remove)
      {
         removeItem();
      }
      SendSizeEventToParent();
   }

   void ResetTrack()
   {
      SetStack(nullptr, {}, nullptr);
   }

   void SetTrack(AudacityProject &project, const std::shared_ptr<SampleTrack> &track)
   {
      SetStack(&project, track, &RealtimeEffectList::Get(*track));
      UpdateRealtimeEffectUIData(*track);
   }

   void SetMaster(AudacityProject &project)
   {
      SetStack(&project, {}, &RealtimeEffectList::Get(project));
   }

   void SetStack(AudacityProject *project, const std::shared_ptr<SampleTrack> &track,
      RealtimeEffectList *stack)
   {
      if (mStack == stack && mTrack == track)
         return;
      mEffectListItemMovedSubscription.Reset();
      mTrack = track;
      mProject = project;
      mStack = stack;
      if (stack)
         mEffectListItemMovedSubscription = stack->Subscribe(
            *this, &RealtimeEffectListWindow::OnEffectListItemChange);
      ReloadEffectsList();
   }

   void EnableEffects(bool enable)
   {
      if (mStack)
         mStack->SetActive(enable);
   }

   void ReloadEffectsList()
   {
      wxWindowUpdateLocker freeze(this);
      
      //delete items that were added to the sizer
      mEffectListContainer->Hide();
      mEffectListContainer->GetSizer()->Clear(true);

      
      if(!mStack || mStack->GetStatesCount() == 0)
         mEffectListContainer->Hide();
      
      auto isEmpty{true};
      if(mStack)
      {
         auto& effects = *mStack;
         isEmpty = effects.GetStatesCount() == 0;
         for(size_t i = 0, count = effects.GetStatesCount(); i < count; ++i)
            InsertEffectRow(i, effects.GetStateAt(i));
      }
      mAddEffect->SetEnabled(mStack != nullptr);
      mAddFavorite->SetEnabled(mStack != nullptr);
      //Workaround for GTK: Underlying GTK widget does not update
      //its size when wxWindow size is set to zero
      mEffectListContainer->Show(!isEmpty);
      
      SendSizeEventToParent();
   }

   void OnAddEffectClicked(wxWindow &button, bool favorite)
   {
      if(!mStack || mProject == nullptr)
         return;

      std::optional<wxString> effectID;
      if (favorite) {
         wxMenu menu;
         for (const auto &[item, id] : AppendFavoriteItems(menu, ReadFavorites()))
            menu.Bind(wxEVT_MENU, [&, id](wxCommandEvent &) { effectID = id; }, item);
         button.PopupMenu(&menu, button.GetClientRect().GetLeftBottom());
      }
      else
         effectID = PickEffect(&button, {});
      
      if(!effectID || effectID->empty())
         return;

      auto plug = PluginManager::Get().GetPlugin(*effectID);
      if(!plug)
         return;

      if(!PluginManager::IsPluginAvailable(*plug)) {
         BasicUI::ShowMessageBox(
            XO("This plugin could not be loaded.\nIt may have been deleted."),
            BasicUI::MessageBoxOptions()
               .Caption(XO("Plugin Error")));

         return;
      }

      if(auto state = AudioIO::Get()->AddState(*mProject, mTrack.get(), *effectID))
      {
         auto effect = state->GetEffect();
         assert(effect); // postcondition of AddState
         const auto effectName = effect->GetName();
         ProjectHistory::Get(*mProject).PushState(
            /*! i18n-hint: undo history record
             first parameter - realtime effect name
             second parameter - track name
             */
            XO("Added %s to %s").Format(effectName, mTrack ? mTrack->GetName() : _("Master")),
            //i18n-hint: undo history record
            XO("Add %s").Format(effectName));
      }
   }

   void InsertEffectRow(size_t index,
      const std::shared_ptr<RealtimeEffectState> &pState)
   {
      if(mProject == nullptr)
         return;

      // See comment in ReloadEffectsList
      if(!mEffectListContainer->IsShown())
         mEffectListContainer->Show();

      auto row = safenew ThemedWindowWrapper<RealtimeEffectControl>(mEffectListContainer, this, wxID_ANY);
      row->SetBackgroundColorIndex(clrEffectListItemBackground);
      row->SetEffect(*mProject, mTrack, pState);
      mEffectListContainer->GetSizer()->Insert(index, row, 0, wxEXPAND);
   }
};


struct RealtimeEffectPanel::PrefsListenerHelper : PrefsListener
{
   AudacityProject& mProject;

   explicit PrefsListenerHelper(AudacityProject& project)
       : mProject { project }
   {}

   void UpdatePrefs() override
   {
      auto& trackList = TrackList::Get(mProject);
      for (auto waveTrack : trackList.Any<WaveTrack>())
         ReopenRealtimeEffectUIData(mProject, *waveTrack);
   }
};

namespace {
AttachedWindows::RegisteredFactory sKey{
[](AudacityProject &project) -> wxWeakRef<wxWindow> {
   constexpr auto EffectsPanelMinWidth { 255 };

   const auto pProjectWindow = &ProjectWindow::Get(project);
   auto effectsPanel = safenew ThemedWindowWrapper<RealtimeEffectPanel>(
      project, pProjectWindow->GetContainerWindow(), wxID_ANY);
   effectsPanel->SetMinSize({EffectsPanelMinWidth, -1});
   effectsPanel->SetName(_("Realtime effects"));
   effectsPanel->SetBackgroundColorIndex(clrMedium);
   effectsPanel->Hide();//initially hidden
   return effectsPanel;
}
};
}

RealtimeEffectPanel &RealtimeEffectPanel::Get(AudacityProject &project)
{
   return GetAttachedWindows(project).Get<RealtimeEffectPanel>(sKey);
}

const RealtimeEffectPanel &
RealtimeEffectPanel::Get(const AudacityProject &project)
{
   return Get(const_cast<AudacityProject &>(project));
}

RealtimeEffectPanel::RealtimeEffectPanel(
   AudacityProject& project, wxWindow* parent, wxWindowID id, const wxPoint& pos,
   const wxSize& size,
   long style, const wxString& name)
      : wxPanel(parent, id, pos, size, style, name)
      , mProject(project)
      , mPrefsListenerHelper(std::make_unique<PrefsListenerHelper>(project))
{
   auto vSizer = std::make_unique<wxBoxSizer>(wxVERTICAL);

   auto header = safenew ThemedWindowWrapper<ListNavigationPanel>(this, wxID_ANY);
#if wxUSE_ACCESSIBILITY
   safenew WindowAccessible(header);
#endif
   header->SetBackgroundColorIndex(clrMedium);
   {
      auto hSizer = std::make_unique<wxBoxSizer>(wxHORIZONTAL);
      auto toggleEffects = safenew ThemedAButtonWrapper<AButton>(header);
      toggleEffects->SetImageIndices(0, bmpEffectOff, bmpEffectOff, bmpEffectOn, bmpEffectOn, bmpEffectOff);
      toggleEffects->SetButtonToggles(true);
      toggleEffects->SetTranslatableLabel(XO("All realtime effects power"));
      toggleEffects->SetBackgroundColorIndex(clrMedium);
      mToggleEffects = toggleEffects;

      toggleEffects->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
         RealtimeEffectList::Get(mProject).SetProjectEffectsEnabled(mToggleEffects->IsDown());
         SavePowerSettings();
      });

      hSizer->Add(toggleEffects, 0, wxSTRETCH_NOT | wxALIGN_CENTER | wxLEFT, 5);
      {
         auto vSizer = std::make_unique<wxBoxSizer>(wxVERTICAL);

         auto headerText = safenew ThemedWindowWrapper<wxStaticText>(header, wxID_ANY, wxEmptyString);
         headerText->SetFont(wxFont(wxFontInfo().Bold()));
         headerText->SetTranslatableLabel(XO("Realtime Effects"));
         headerText->SetForegroundColorIndex(clrTrackPanelText);

         vSizer->Add(headerText);

         hSizer->Add(vSizer.release(), 1, wxEXPAND | wxALL, 10);
      }
      auto close = safenew ThemedAButtonWrapper<AButton>(header);
      close->SetTranslatableLabel(XO("Close"));
      close->SetImageIndices(0, bmpCloseNormal, bmpCloseHover, bmpCloseDown, bmpCloseHover, bmpCloseDisabled);
      close->SetBackgroundColorIndex(clrMedium);

      close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

      hSizer->Add(close, 0, wxSTRETCH_NOT | wxALIGN_CENTER | wxRIGHT, 5);

      header->SetSizer(hSizer.release());
   }
   vSizer->Add(header, 0, wxEXPAND);
   const auto addStackHeader = [&](const TranslatableString &powerLabel,
      const TranslatableString &title, AButton *&power) {
      auto *row = safenew wxBoxSizer(wxHORIZONTAL);
      auto *button = safenew ThemedAButtonWrapper<AButton>(this);
      button->SetImageIndices(0, bmpEffectOff, bmpEffectOff, bmpEffectOn,
         bmpEffectOn, bmpEffectOff);
      button->SetButtonToggles(true);
      button->SetTranslatableLabel(powerLabel);
      button->SetBackgroundColorIndex(clrMedium);
      power = button;
      auto *label = safenew ThemedWindowWrapper<wxStaticText>(this, wxID_ANY,
         wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
      label->SetTranslatableLabel(title);
      label->SetForegroundColorIndex(clrTrackPanelText);
      row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
      row->Add(label, 1, wxALIGN_CENTER_VERTICAL);
      vSizer->Add(row, 0, wxEXPAND | wxALL, 6);
      return label;
   };

   addStackHeader(XO("Master effects power"), XO("Master"), mMasterPower);
   auto *MasterList = safenew ThemedWindowWrapper<RealtimeEffectListWindow>(this, wxID_ANY);
   MasterList->SetBackgroundColorIndex(clrMedium);
   mMasterList = MasterList;
   vSizer->Add(mMasterList, 0, wxEXPAND);
   vSizer->Add(safenew wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

   mTrackTitle = addStackHeader(XO("Track effects power"), {}, mTrackPower);
   auto *EffectList = safenew ThemedWindowWrapper<RealtimeEffectListWindow>(this, wxID_ANY);
   EffectList->SetBackgroundColorIndex(clrMedium);
   mEffectList = EffectList;
   vSizer->Add(mEffectList, 1, wxEXPAND);
   mHeader = header;
   mMasterList->SetMaster(mProject);
   mTrackPower->Disable();
   mMasterPower->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      mMasterList->EnableEffects(mMasterPower->IsDown());
      SavePowerSettings();
   });
   mTrackPower->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
      mEffectList->EnableEffects(mTrackPower->IsDown());
      SavePowerSettings();
   });
   UpdatePowerButtons();

   SetSizerAndFit(vSizer.release());

   Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
      event.Skip();

      // Keep the track stack usable when a long master stack needs to scroll.
      const int contentHeight = mMasterList->GetSizer()->CalcMin().y;
      const int height = std::min(contentHeight, GetClientSize().y / 2);
      if (mMasterList->GetMinSize().y != height)
         mMasterList->SetMinSize(wxSize(-1, height));
      Layout();
   });

   Bind(wxEVT_CHAR_HOOK, &RealtimeEffectPanel::OnCharHook, this);
   mTrackListChanged = TrackList::Get(mProject).Subscribe([this](const TrackListEvent& evt) {
         auto track = evt.mpTrack.lock();
         auto waveTrack = std::dynamic_pointer_cast<WaveTrack>(track);

         if (waveTrack == nullptr)
            return;
         
         switch (evt.mType)
         {
         case TrackListEvent::TRACK_DATA_CHANGE:
            if (mCurrentTrack.lock() == waveTrack)
               mTrackTitle->SetLabel(track->GetName());
            UpdateRealtimeEffectUIData(*waveTrack);
            break;
         case TrackListEvent::DELETION:
            if (evt.mExtra == 0)
               mPotentiallyRemovedTracks.push_back(waveTrack);
            break;
         case TrackListEvent::ADDITION:
            // Addition can be fired as a part of "replace" event.
            // Calling UpdateRealtimeEffectUIData is mostly no-op,
            // it will just create a new State and Access for it.
            UpdateRealtimeEffectUIData(*waveTrack);
            break;
         default:
            break;
         }
   });

   mUndoSubscription = UndoManager::Get(mProject).Subscribe(
      [this](UndoRedoMessage message)
      {
         if (
            message.type == UndoRedoMessage::Type::Purge ||
            message.type == UndoRedoMessage::Type::BeginPurge ||
            message.type == UndoRedoMessage::Type::EndPurge)
            return;

         auto& trackList = TrackList::Get(mProject);

         // Realtime effect UI is only updated on Undo or Redo
         auto waveTracks = trackList.Any<WaveTrack>();
         
         if (
            message.type == UndoRedoMessage::Type::UndoOrRedo ||
            message.type == UndoRedoMessage::Type::Reset)
         {
            UpdatePowerButtons();
            for (auto waveTrack : waveTracks)
               UpdateRealtimeEffectUIData(*waveTrack);
         }

         // But mPotentiallyRemovedTracks processing happens as fast as possible.
         // This event is fired right after the track is deleted, so we do not
         // hold the strong reference to the track much longer than need.
         if (mPotentiallyRemovedTracks.empty())
            return;

         // Collect RealtimeEffectUIs that are currently shown
         // for the potentially removed tracks
         std::vector<RealtimeEffectStateUI*> shownUIs;
         
         for (auto track : mPotentiallyRemovedTracks)
         {
            // By construction, track cannot be null
            assert(track != nullptr);

            VisitRealtimeEffectStateUIs(
               *track,
               [&shownUIs](auto& ui)
               {
                  if (ui.IsShown())
                     shownUIs.push_back(&ui);
               });
         }

         // For every UI shown - check if the corresponding state
         // is reachable from the current track list.
         for (auto effectUI : shownUIs)
         {
            bool reachable = false;
            
            for (auto track : waveTracks)
            {               
               VisitRealtimeEffectStateUIs(
                  *track,
                  [effectUI, &reachable](auto& ui)
                  {
                     if (effectUI == &ui)
                        reachable = true;
                  });

               if (reachable)
                  break;
            }

            if (!reachable)
               // Don't need to autosave for an unreachable state
               effectUI->Hide();
         }

         mPotentiallyRemovedTracks.clear();
      });

   mFocusChangeSubscription = TrackFocus::Get(project)
      .Subscribe([this](const TrackFocusChangeMessage& msg) {
         if (IsShown())
         {
            auto& trackFocus = TrackFocus::Get(mProject);
            ShowPanel(dynamic_cast<SampleTrack *>(trackFocus.Get()), false);
         }
      });

   Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
      HidePanel(); });
}

RealtimeEffectPanel::~RealtimeEffectPanel()
{
}

void RealtimeEffectPanel::ShowPanel(SampleTrack* track, bool focus)
{
   wxWindowUpdateLocker freeze(this);
   if (track)
      SetTrack(track->SharedPointer<SampleTrack>());
   else
      ResetTrack();
   ShowPanelWindow(focus);
}

void RealtimeEffectPanel::ShowMaster(bool focus)
{
   ShowPanel(dynamic_cast<SampleTrack *>(TrackFocus::Get(mProject).Get()), false);
   if (focus)
      mMasterPower->SetFocus();
}

void RealtimeEffectPanel::ShowPanelWindow(bool focus)
{
   auto &projectWindow = ProjectWindow::Get(mProject);
   const auto pContainerWindow = projectWindow.GetContainerWindow();
   if (pContainerWindow->GetWindow1() != this)
   {
      //Restore previous effects window size
      pContainerWindow->SplitVertically(
         this,
         projectWindow.GetTrackListWindow(),
         this->GetSize().GetWidth());
   }
   if(focus)
      SetFocus();
   projectWindow.Layout();
}

void RealtimeEffectPanel::HidePanel()
{
   wxWindowUpdateLocker freeze(this);

   auto &projectWindow = ProjectWindow::Get(mProject);
   const auto pContainerWindow = projectWindow.GetContainerWindow();
   const auto pTrackListWindow = projectWindow.GetTrackListWindow();
   if (pContainerWindow->GetWindow2() == nullptr)
      //only effects panel is present, restore split positions before removing effects panel
      //Workaround: ::Replace and ::Initialize do not work here...
      pContainerWindow->SplitVertically(this, pTrackListWindow);

   pContainerWindow->Unsplit(this);
   pTrackListWindow->SetFocus();
   projectWindow.Layout();
}

void RealtimeEffectPanel::SetTrack(const std::shared_ptr<SampleTrack>& track)
{

   //Avoid creation-on-demand of a useless, empty list in case the track is of non-wave type.
   if(track && dynamic_cast<WaveTrack*>(&*track) != nullptr)
   {
      mTrackTitle->SetLabel(track->GetName());
      mTrackPower->Enable();
      mEffectList->SetTrack(mProject, track);
      mCurrentTrack = track;
      UpdatePowerButtons();
      //i18n-hint: argument - track name
      mHeader->SetName(wxString::Format(_("Realtime effects for %s"), track->GetName()));
   }
   else
      ResetTrack();
}

void RealtimeEffectPanel::ResetTrack()
{
   mTrackTitle->SetLabel(wxEmptyString);
   mTrackPower->Disable();
   mTrackPower->PopUp();
   mEffectList->ResetTrack();
   mCurrentTrack.reset();
   mHeader->SetName(wxEmptyString);
}

void RealtimeEffectPanel::SavePowerSettings()
{
   ProjectHistory::Get(mProject).ModifyState(false);
   UndoManager::Get(mProject).MarkUnsaved();
}

void RealtimeEffectPanel::UpdatePowerButtons()
{
   const auto &master = RealtimeEffectList::Get(mProject);
   master.AreProjectEffectsEnabled() ? mToggleEffects->PushDown() : mToggleEffects->PopUp();
   master.IsActive() ? mMasterPower->PushDown() : mMasterPower->PopUp();
   const auto track = mCurrentTrack.lock();
   track && RealtimeEffectList::Get(*track).IsActive()
      ? mTrackPower->PushDown() : mTrackPower->PopUp();
}

void RealtimeEffectPanel::SetFocus()
{
   mHeader->SetFocus();
}

void RealtimeEffectPanel::OnCharHook(wxKeyEvent& evt)
{
   if(evt.GetKeyCode() == WXK_ESCAPE && IsShown() && IsDescendant(FindFocus()))
      Close();
   else
      evt.Skip();
}

void RealtimeEffectPanel::ManageFavorites(wxWindow &parent)
{
   auto favorites = ReadFavorites();
   wxDialog dialog(&parent, wxID_ANY, _("Manage Favorites"));
   auto *layout = safenew wxBoxSizer(wxVERTICAL);
   auto *list = safenew wxListBox(&dialog, wxID_ANY, wxDefaultPosition, wxSize(360, 200));
   list->SetName(_("Favorite realtime effects"));
   layout->Add(list, 1, wxEXPAND | wxALL, 8);

   auto *actions = safenew wxBoxSizer(wxHORIZONTAL);
   auto *up = safenew wxButton(&dialog, wxID_ANY, _("Move Up"));
   auto *down = safenew wxButton(&dialog, wxID_ANY, _("Move Down"));
   auto *remove = safenew wxButton(&dialog, wxID_ANY, _("Remove"));
   actions->Add(up, 0, wxRIGHT, 4);
   actions->Add(down, 0, wxRIGHT, 4);
   actions->Add(remove);
   layout->Add(actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
   layout->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);

   const auto updateButtons = [&] {
      const auto row = list->GetSelection();
      up->Enable(row > 0);
      down->Enable(row != wxNOT_FOUND && static_cast<size_t>(row + 1) < favorites.size());
      remove->Enable(row != wxNOT_FOUND);
   };
   const auto populate = [&](int row) {
      wxArrayString names;
      for (const auto &id : favorites) {
         const auto *plugin = PluginManager::Get().GetPlugin(id);
         names.Add(plugin ? plugin->GetSymbol().Msgid().Translation()
                          : XO("Unavailable: %s").Format(id).Translation());
      }
      list->Set(names);
      if (row >= 0 && static_cast<size_t>(row) < favorites.size())
         list->SetSelection(row);
      updateButtons();
   };
   const auto move = [&](int offset) {
      const auto row = list->GetSelection();
      const auto target = row + offset;
      if (row != wxNOT_FOUND && target >= 0 && static_cast<size_t>(target) < favorites.size()) {
         std::swap(favorites[row], favorites[target]);
         populate(target);
      }
   };
   up->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { move(-1); });
   down->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { move(1); });
   remove->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      const auto row = list->GetSelection();
      if (row != wxNOT_FOUND) {
         favorites.RemoveAt(row);
         populate(std::min(row, static_cast<int>(favorites.size()) - 1));
      }
   });
   list->Bind(wxEVT_LISTBOX, [&](wxCommandEvent &) { updateButtons(); });
   populate(0);
   dialog.SetSizerAndFit(layout);
   dialog.CentreOnParent();
   if (dialog.ShowModal() == wxID_OK)
      SaveFavorites(favorites);
}
