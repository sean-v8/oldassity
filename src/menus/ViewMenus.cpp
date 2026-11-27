#include "../CommonCommandFlags.h"
#include "../Menus.h"
#include "PlayableTrack.h"
#include "Prefs.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "../ProjectSettings.h"
#include "../ProjectWindow.h"
#include "../TrackInfo.h"
#include "../TrackPanel.h"
#include "../TrackPanelAx.h"
#include "../prefs/WaveformSettings.h"
#include "Envelope.h"
#include "ProjectRate.h"
#include "UndoManager.h"
#include "ViewInfo.h"
#include "WaveTrack.h"
#include "../tracks/playabletrack/wavetrack/ui/WaveTrackControls.h"
#include "../commands/CommandContext.h"
#include "../commands/CommandManager.h"
#include "../prefs/GUIPrefs.h"
#include "../prefs/TracksPrefs.h"
#include "../tracks/ui/ChannelView.h"


#include <wx/app.h>
#include <wx/scrolbar.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

// private helper classes and functions
namespace {

struct WaveMeasurement
{
   double minimum = std::numeric_limits<double>::infinity();
   double maximum = -minimum;
   double target = 0;
   double time = 0;
   double distance = std::numeric_limits<double>::infinity();

   void Add(double value, double sampleTime, double cursor, bool point)
   {
      if (!std::isfinite(value))
         return;
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
      const auto delta = std::abs(sampleTime - cursor);
      if (point ? delta < distance
                : !std::isfinite(distance) || std::abs(value) > std::abs(target)
                  || (std::abs(value) == std::abs(target) && sampleTime < time)) {
         target = value;
         time = sampleTime;
         distance = delta;
      }
   }
};

void MeasureWaveform(
   const WaveTrack &track, double start, double end, bool point, WaveMeasurement &result)
{
   std::array<float, 4096> samples;
   for (const auto &channel : track.Channels()) {
      for (const auto &clip : channel->Intervals()) {
         const double origin = clip->GetPlayStartTime();
         const double rate = clip->GetRate() / clip->GetStretchRatio();
         const auto count = clip->GetVisibleSampleCount();
         if (count <= 0 || start >= clip->GetPlayEndTime() ||
             (point ? start < origin : end <= origin))
            continue;

         // Use the same clip-relative time grid as individual sample drawing.
         const auto index = [&](double time) {
            return sampleCount{std::ceil(std::max(0.0, (time - origin) * rate) - 1e-6)};
         };
         const auto center = std::min(count - 1,
            sampleCount{std::round((start - origin) * rate)});
         auto first = point ? std::max(sampleCount{0}, center - 2) : index(start);
         const auto last = std::min(count, point ? center + 3 : index(end));
         while (first < last) {
            const auto length = std::min(last - first, sampleCount{samples.size()}).as_size_t();
            if (!clip->GetSamples(reinterpret_cast<samplePtr>(samples.data()),
                    floatSample, first, length, false))
               break;
            for (size_t i = 0; i < length; ++i) {
               const auto time = origin + (first + i).as_double() / rate;
               const auto gain = clip->GetEnvelope().GetValue(time, 1.0 / clip->GetRate());
               result.Add(samples[i] * gain, time, start, point);
            }
            first += length;
         }
      }
   }
}

double DisplayAmplitude(double value, const WaveformSettings &settings)
{
   if (settings.isLinear() || value == 0.0)
      return value;
   return std::copysign(std::max(0.0,
      1.0 + 20.0 * std::log10(std::abs(value)) / settings.dBRange), value);
}

void SetFramedBounds(const WaveTrack &track, double anchor, double span, double fraction)
{
   constexpr double minimumSpan = 0.001;
   const auto &settings = WaveformSettings::Get(track);
   const auto limit = DisplayAmplitude(2.0, settings);
   anchor = std::clamp(anchor, -limit, limit);
   span = std::clamp(span, minimumSpan, 2.0 * limit);
   const auto bottom = std::clamp(anchor - fraction * span, -limit, limit - span);
   WaveformScale::Get(track).SetDisplayBounds(bottom, bottom + span);
}

double GetZoomOfSelection( const AudacityProject &project )
{
   auto &viewInfo = ViewInfo::Get( project );
   auto &window = ProjectWindow::Get( project );

   const double lowerBound =
      std::max(viewInfo.selectedRegion.t0(),
         window.ScrollingLowerBoundTime());
   const double denom =
      viewInfo.selectedRegion.t1() - lowerBound;
   if (denom <= 0.0)
      return viewInfo.GetZoom();

   // LL:  The "-1" is just a hack to get around an issue where zooming to
   //      selection doesn't actually get the entire selected region within the
   //      visible area.  This causes a problem with scrolling at end of playback
   //      where the selected region may be scrolled off the left of the screen.
   //      I know this isn't right, but until the real rounding or 1-off issue is
   //      found, this will have to work.
   // PRL:  Did I fix this?  I am not sure, so I leave the hack in place.
   //      Fixes might have resulted from commits
   //      1b8f44d0537d987c59653b11ed75a842b48896ea and
   //      e7c7bb84a966c3b3cc4b3a9717d5f247f25e7296
   auto width = viewInfo.GetTracksUsableWidth();
   return (width - 1) / denom;
}

double GetZoomOfPreset( const AudacityProject &project, int preset )
{

   // Sets a limit on how far we will zoom out as a factor over zoom to fit.
   const double maxZoomOutFactor = 4.0;
   // Sets how many pixels we allow for one uint, such as seconds.
   const double pixelsPerUnit = 5.0;

   double result = 1.0;
   auto &window = ProjectWindow::Get( project );
   double zoomToFit = window.GetZoomOfToFit();
   using namespace WaveChannelViewConstants;
   switch(preset) {
      default:
      case kZoomDefault:
         result = ZoomInfo::GetDefaultZoom();
         break;
      case kZoomToFit:
         result = zoomToFit;
         break;
      case kZoomToSelection:
         result = GetZoomOfSelection( project );
         break;
      case kZoomMinutes:
         result = pixelsPerUnit * 1.0/60;
         break;
      case kZoomSeconds:
         result = pixelsPerUnit * 1.0;
         break;
      case kZoom5ths:
         result = pixelsPerUnit * 5.0;
         break;
      case kZoom10ths:
         result = pixelsPerUnit * 10.0;
         break;
      case kZoom20ths:
         result = pixelsPerUnit * 20.0;
         break;
      case kZoom50ths:
         result = pixelsPerUnit * 50.0;
         break;
      case kZoom100ths:
         result = pixelsPerUnit * 100.0;
         break;
      case kZoom500ths:
         result = pixelsPerUnit * 500.0;
         break;
      case kZoomMilliSeconds:
         result = pixelsPerUnit * 1000.0;
         break;
      case kZoomSamples:
         result = 44100.0;
         break;
      case kZoom4To1:
         result = 44100.0 * 4;
         break;
      case kMaxZoom:
         result = ZoomInfo::GetMaxZoom();
         break;
   };
   if( result < (zoomToFit/maxZoomOutFactor) )
      result = zoomToFit / maxZoomOutFactor;
   return result;
}

}

namespace {
void DoZoomFitV(AudacityProject &project)
{
   auto &viewInfo = ViewInfo::Get( project );
   auto &tracks = TrackList::Get( project );

   // Only nonminimized audio tracks will be resized
   // Assume all channels of the track have the same minimization state
   auto range = tracks.Any<AudioTrack>()
      - [](const Track *pTrack){
         return ChannelView::Get(*pTrack->GetChannel(0)).GetMinimized(); };
   auto count = range.sum(&Track::NChannels);
   if (count == 0)
      return;

   // Find total height to apportion
   auto height = viewInfo.GetHeight();
   height -= 28;
   
   // The height of minimized and non-audio tracks cannot be apportioned
   height -=
      tracks.Any().sum(ChannelView::GetChannelGroupHeight)
         - range.sum(ChannelView::GetChannelGroupHeight);
   
   // Give each resized track the average of the remaining height
   // Bug 2803: Cast count to int, because otherwise the result of 
   // division will be unsigned too, and will be a very large number 
   // if height was negative!
   height = height / (int)count;
   // Use max() so that we don't set a negative height when there is
   // not enough room.
   height = std::max( (int)TrackInfo::MinimumTrackHeight(), height );

   for (auto t : range)
      for (auto pChannel : t->Channels())
         ChannelView::Get(*pChannel).SetExpandedHeight(height);
}
}

namespace ViewActions {

// Menu handler functions

struct Handler final
   : CommandHandlerObject // MUST be the first base class!
   , ClientData::Base
{

struct FramedTrack
{
   std::weak_ptr<const WaveTrack> track;
   float minimum, maximum;
};

std::optional<std::pair<double, double>> sampleView;
std::vector<FramedTrack> framedTracks;
double framedAmplitude = 0.0;

void OnCloseSampleView(const CommandContext &context)
{
   auto &project = context.project;
   auto &view = ViewInfo::Get(project);
   auto &window = ProjectWindow::Get(project);
   if (sampleView) {
      const auto [zoom, left] = *sampleView;
      sampleView.reset();
      view.SetZoom(zoom);
      view.h = left;
      window.FixScrollbars();
      window.TP_ScrollWindow(left);
      view.h = left;
      TrackPanel::Get(project).Refresh(false);
      return;
   }

   const auto width = view.GetTracksUsableWidth();
   if (width <= 0)
      return;
   sampleView.emplace(view.GetZoom(), view.h);
   auto rate = ProjectRate::Get(project).GetRate();
   if (const auto track = dynamic_cast<const WaveTrack *>(TrackFocus::Get(project).Get()))
      rate = track->GetRate();

   view.SetZoom(width * rate / 32.0);
   window.FixScrollbars();
   window.TP_ScrollWindow(view.selectedRegion.t0() - width / (2.0 * view.GetZoom()));
}

void OnFrameWaveform(const CommandContext &context)
{
   auto &project = context.project;
   auto &tracks = TrackList::Get(project);
   if (!framedTracks.empty()) {
      for (const auto &saved : framedTracks) {
         if (const auto track = saved.track.lock(); track && track->GetOwner().get() == &tracks)
            WaveformScale::Get(*track).SetDisplayBounds(saved.minimum, saved.maximum);
      }
      framedTracks.clear();
      auto &panel = TrackPanel::Get(project);
      panel.UpdateVRulers();
      panel.Refresh(false);
      return;
   }

   std::vector<const WaveTrack *> targets;
   for (const auto track : tracks.Any<WaveTrack>())
      if (track->GetSelected())
         targets.push_back(track);
   if (targets.empty()) {
      if (const auto track = dynamic_cast<const WaveTrack *>(TrackFocus::Get(project).Get()))
         targets.push_back(track);
   }

   auto &selection = ViewInfo::Get(project).selectedRegion;
   const bool point = selection.isPoint();
   WaveMeasurement measurement;
   for (const auto track : targets)
      MeasureWaveform(*track, selection.t0(), selection.t1(), point, measurement);
   if (!std::isfinite(measurement.distance))
      return;

   framedAmplitude = measurement.target;
   for (const auto track : targets) {
      float minimum, maximum;
      WaveformScale::Get(*track).GetDisplayBounds(minimum, maximum);
      framedTracks.push_back({track->SharedPointer<const WaveTrack>(), minimum, maximum});
      const auto &settings = WaveformSettings::Get(*track);
      const auto bottom = DisplayAmplitude(measurement.minimum, settings);
      const auto top = DisplayAmplitude(measurement.maximum, settings);
      const auto span = std::max(0.001, (top - bottom) * 1.12);
      SetFramedBounds(*track, (bottom + top) * 0.5, span, 0.5);
   }

   if (!point) {
      selection.setTimes(measurement.time, measurement.time);
      ProjectHistory::Get(project).ModifyState(false);
   }
   auto &panel = TrackPanel::Get(project);
   panel.UpdateVRulers();
   panel.Refresh(false);
}

void ZoomFramedWaveform(const CommandContext &context, double factor)
{
   auto &project = context.project;
   for (const auto &saved : framedTracks) {
      const auto track = saved.track.lock();
      if (!track || track->GetOwner().get() != &TrackList::Get(project))
         continue;
      float minimum, maximum;
      WaveformScale::Get(*track).GetDisplayBounds(minimum, maximum);
      const auto anchor = DisplayAmplitude(framedAmplitude, WaveformSettings::Get(*track));
      const double span = maximum - minimum;
      if (span <= 0)
         continue;
      const auto fraction = std::clamp((anchor - minimum) / span, 0.0, 1.0);
      SetFramedBounds(*track, anchor, span * factor, fraction);
   }
   if (!framedTracks.empty()) {
      auto &panel = TrackPanel::Get(project);
      panel.UpdateVRulers();
      panel.Refresh(false);
   }
}

void OnFramedZoomIn(const CommandContext &context) { ZoomFramedWaveform(context, 0.5); }
void OnFramedZoomOut(const CommandContext &context) { ZoomFramedWaveform(context, 2.0); }

void OnZoomIn(const CommandContext &context)
{
   auto &project = context.project;
   auto &trackPanel = TrackPanel::Get( project );
   auto &window = ProjectWindow::Get( project );
   window.ZoomInByFactor( 2.0 );
   trackPanel.Refresh(false);
}

void OnZoomNormal(const CommandContext &context)
{
   auto &project = context.project;
   auto &trackPanel = TrackPanel::Get( project );
   auto &window = ProjectWindow::Get( project );

   window.Zoom(ZoomInfo::GetDefaultZoom());
   trackPanel.Refresh(false);
}

void OnZoomOut(const CommandContext &context)
{
   auto &project = context.project;
   auto &window = ProjectWindow::Get( project );
   window.ZoomOutByFactor( 1 /2.0 );
}

void OnZoomSel(const CommandContext &context)
{
   auto &project = context.project;
   auto &selectedRegion = ViewInfo::Get( project ).selectedRegion;
   auto &window = ProjectWindow::Get( project );

   window.Zoom( GetZoomOfSelection( project ) );
   window.TP_ScrollWindow(selectedRegion.t0());
}

void OnZoomToggle(const CommandContext &context)
{
   auto &project = context.project;
   auto &viewInfo = ViewInfo::Get( project );
   auto &trackPanel = TrackPanel::Get( project );
   auto &window = ProjectWindow::Get( project );

//   const double origLeft = viewInfo.h;
//   const double origWidth = viewInfo.GetScreenEndTime() - origLeft;

   // Choose the zoom that is most different to the current zoom.
   double Zoom1 = GetZoomOfPreset( project, TracksPrefs::Zoom1Choice() );
   double Zoom2 = GetZoomOfPreset( project, TracksPrefs::Zoom2Choice() );
   double Z = viewInfo.GetZoom();// Current Zoom.
   double ChosenZoom =
      fabs(log(Zoom1 / Z)) > fabs(log( Z / Zoom2)) ? Zoom1:Zoom2;

   window.Zoom(ChosenZoom);
   trackPanel.Refresh(false);
//   const double newWidth = GetScreenEndTime() - viewInfo.h;
//   const double newh = origLeft + (origWidth - newWidth) / 2;
//   TP_ScrollWindow(newh);
}

void OnZoomFit(const CommandContext &context)
{
   auto &project = context.project;
   auto &window = ProjectWindow::Get( project );
   window.DoZoomFit();
}

void OnZoomFitV(const CommandContext &context)
{
   auto &project = context.project;
   auto &window = ProjectWindow::Get( project );

   DoZoomFitV(project);

   window.GetVerticalScrollBar().SetThumbPosition(0);
   ProjectHistory::Get( project ).ModifyState(true);
}

void OnAdvancedVZoom(const CommandContext &context)
{
   auto &project = context.project;
   auto &commandManager = CommandManager::Get( project );

   bool checked = !gPrefs->Read(wxT("/GUI/VerticalZooming"), 0L);
   gPrefs->Write(wxT("/GUI/VerticalZooming"), checked);
   gPrefs->Flush();
   commandManager.Check(wxT("AdvancedVZoom"), checked);
   MenuCreator::RebuildAllMenuBars();
}

void OnCollapseAllTracks(const CommandContext &context)
{
   auto &project = context.project;
   auto &tracks = TrackList::Get( project );
   auto &window = ProjectWindow::Get( project );

   for (auto t : tracks)
      for (auto pChannel : t->Channels())
         ChannelView::Get(*pChannel).SetMinimized(true);

   ProjectHistory::Get( project ).ModifyState(true);
}

void OnExpandAllTracks(const CommandContext &context)
{
   auto &project = context.project;
   auto &tracks = TrackList::Get( project );
   auto &window = ProjectWindow::Get( project );

   for (auto t : tracks)
      for (auto pChannel : t->Channels())
         ChannelView::Get(*pChannel).SetMinimized(false);

   ProjectHistory::Get( project ).ModifyState(true);
}

void FinishTrackHeightChange(AudacityProject &project)
{
   ProjectHistory::Get(project).ModifyState(false);
   auto &panel = TrackPanel::Get(project);
   panel.MakeParentRedrawScrollbars();
   panel.Refresh(false);
}

void OnCycleTrackHeight(const CommandContext &context)
{
   auto &project = context.project;
   if (auto *track = dynamic_cast<WaveTrack *>(TrackFocus::Get(project).Get())) {
      WaveTrackControls::CycleHeight(*track);
      FinishTrackHeightChange(project);
   }
}

void OnCycleAllTrackHeights(const CommandContext &context)
{
   auto &project = context.project;
   if (WaveTrackControls::CycleAllHeights(TrackList::Get(project)))
      FinishTrackHeightChange(project);
}

void OnSetTrackHeightsByPan(const CommandContext &context)
{
   auto &project = context.project;
   WaveTrackControls::SetHeightsByPan(TrackList::Get(project));
   FinishTrackHeightChange(project);
}

void OnGoSelStart(const CommandContext &context)
{
   auto &project = context.project;
   auto &viewInfo = ViewInfo::Get( project );
   auto &selectedRegion = viewInfo.selectedRegion;
   auto &window = ProjectWindow::Get( project );

   if (selectedRegion.isPoint())
      return;

   window.TP_ScrollWindow(
      selectedRegion.t0() - ((viewInfo.GetScreenEndTime() - viewInfo.h) / 2));
}

void OnGoSelEnd(const CommandContext &context)
{
   auto &project = context.project;
   auto &viewInfo = ViewInfo::Get( project );
   auto &selectedRegion = viewInfo.selectedRegion;
   auto &window = ProjectWindow::Get( project );

   if (selectedRegion.isPoint())
      return;

   window.TP_ScrollWindow(
      selectedRegion.t1() - ((viewInfo.GetScreenEndTime() - viewInfo.h) / 2));
}

void OnShowExtraMenus(const CommandContext &context)
{
   auto &project = context.project;
   auto &commandManager = CommandManager::Get( project );

   bool checked = !gPrefs->Read(wxT("/GUI/ShowExtraMenus"), 0L);
   gPrefs->Write(wxT("/GUI/ShowExtraMenus"), checked);
   gPrefs->Flush();
   commandManager.Check(wxT("ShowExtraMenus"), checked);
   MenuCreator::RebuildAllMenuBars();
}

void OnShowClipping(const CommandContext &context)
{
   auto &project = context.project;
   auto &commandManager = CommandManager::Get( project );
   auto &trackPanel = TrackPanel::Get( project );

   bool checked = !gPrefs->Read(wxT("/GUI/ShowClipping"), 0L);
   gPrefs->Write(wxT("/GUI/ShowClipping"), checked);
   gPrefs->Flush();
   commandManager.Check(wxT("ShowClipping"), checked);

   PrefsListener::Broadcast(ShowClippingPrefsID());

   trackPanel.Refresh(false);
}

void OnShowNameOverlay(const CommandContext &context)
{
   auto &project = context.project;
   auto &commandManager = CommandManager::Get( project );
   auto &trackPanel = TrackPanel::Get( project );

   bool checked = !gPrefs->Read(wxT("/GUI/ShowTrackNameInWaveform"), 0L);
   gPrefs->Write(wxT("/GUI/ShowTrackNameInWaveform"), checked);
   gPrefs->Flush();
   commandManager.Check(wxT("ShowTrackNameInWaveform"), checked);

   PrefsListener::Broadcast(ShowTrackNameInWaveformPrefsID());

   trackPanel.Refresh(false);
}

// Not a menu item, but a listener for events
void OnUndoPushed(UndoRedoMessage message)
{
   if (message.type == UndoRedoMessage::Pushed) {
      const auto &settings = ProjectSettings::Get( mProject );
      if (settings.GetTracksFitVerticallyZoomed())
         DoZoomFitV( mProject );
   }
}

Handler( AudacityProject &project )
   : mProject{ project }
{
   mUndoSubscription = UndoManager::Get(mProject)
      .Subscribe(*this, &Handler::OnUndoPushed);
}

Handler( const Handler & ) = delete;
Handler &operator=( const Handler & ) = delete;

Observer::Subscription mUndoSubscription;
AudacityProject &mProject;

}; // struct Handler

} // namespace

// Handler needs a back-reference to the project, so needs a factory registered
// with AudacityProject.
static const AudacityProject::AttachedObjects::RegisteredFactory key{
   []( AudacityProject &project ) {
      return std::make_unique< ViewActions::Handler >( project ); } };

static CommandHandlerObject &findCommandHandler(AudacityProject &project) {
   return project.AttachedObjects::Get< ViewActions::Handler >( key );
};

// Menu definitions

#define FN(X) (& ViewActions::Handler :: X)

// Under /MenuBar
namespace {
using namespace MenuTable;
BaseItemSharedPtr ViewMenu()
{
   using Options = CommandManager::Options;

   static BaseItemSharedPtr menu{
   ( FinderScope{ findCommandHandler },
   Menu( wxT("View"), XXO("&View"),
      Section( "Basic",
         Menu( wxT("Zoom"), XXO("&Zoom"),
            Section( "",
               Command( wxT("ZoomIn"), XXO("Zoom &In"), FN(OnZoomIn),
                  ZoomInAvailableFlag(), wxT("Ctrl+1") ),
               Command( wxT("ZoomNormal"), XXO("Zoom &Normal"), FN(OnZoomNormal),
                  TracksExistFlag() ),
               Command( wxT("CloseSampleView"), XXO("Toggle Close Sample View"), FN(OnCloseSampleView),
                  WaveTracksExistFlag(), wxT("Ctrl+2") ),
               Command( wxT("FrameWaveform"), XXO("Toggle Waveform Framing"), FN(OnFrameWaveform),
                  WaveTracksExistFlag(), wxT("Shift+2") ),
               Command( wxT("FramedWaveformZoomIn"), XXO("Zoom In on Framed Waveform"), FN(OnFramedZoomIn),
                  WaveTracksExistFlag(), wxT("Shift+1") ),
               Command( wxT("FramedWaveformZoomOut"), XXO("Zoom Out on Framed Waveform"), FN(OnFramedZoomOut),
                  WaveTracksExistFlag(), wxT("Shift+3") ),
               Command( wxT("ZoomOut"), XXO("Zoom &Out"), FN(OnZoomOut),
                  ZoomOutAvailableFlag(), wxT("Ctrl+3") ),
               Command( wxT("ZoomSel"), XXO("&Zoom to Selection"), FN(OnZoomSel),
                  TimeSelectedFlag(), wxT("Ctrl+E") ),
               Command( wxT("ZoomToggle"), XXO("Zoom &Toggle"), FN(OnZoomToggle),
                  TracksExistFlag(), wxT("Shift+Z") )
            ),
            Section( "",
               Command( wxT("AdvancedVZoom"), XXO("Advanced &Vertical Zooming"),
                  FN(OnAdvancedVZoom), AlwaysEnabledFlag,
                  Options{}.CheckTest( wxT("/GUI/VerticalZooming"), false ) )
            )
         ),

         Menu( wxT("TrackSize"), XXO("T&rack Size"),
            Command( wxT("FitInWindow"), XXO("&Fit to Width"), FN(OnZoomFit),
               TracksExistFlag(), wxT("Ctrl+F") ),
            Command( wxT("FitV"), XXO("Fit to &Height"), FN(OnZoomFitV),
               TracksExistFlag(), wxT("Ctrl+Shift+F") ),
            Command( wxT("CollapseAllTracks"), XXO("&Collapse All Tracks"),
               FN(OnCollapseAllTracks), TracksExistFlag(), wxT("Ctrl+Shift+C") ),
            Command( wxT("ExpandAllTracks"), XXO("E&xpand Collapsed Tracks"),
               FN(OnExpandAllTracks), TracksExistFlag(), wxT("Ctrl+Shift+X") ),
            Command( wxT("CycleTrackHeight"), XXO("Cycle Focused Track Height"),
               FN(OnCycleTrackHeight), WaveTracksExistFlag() ),
            Command( wxT("CycleAllTrackHeights"), XXO("Cycle All Audio Track Heights"),
               FN(OnCycleAllTrackHeights), WaveTracksExistFlag() ),
            Command( wxT("SetTrackHeightsByPan"), XXO("Set Audio Track Heights by Pan"),
               FN(OnSetTrackHeightsByPan), WaveTracksExistFlag() )
         ),

         Menu( wxT("SkipTo"), XXO("Sk&ip to"),
            Command( wxT("SkipSelStart"), XXO("Selection Sta&rt"),
               FN(OnGoSelStart), TimeSelectedFlag(),
               Options{ wxT("Ctrl+["), XO("Skip to Selection Start") } ),
            Command( wxT("SkipSelEnd"), XXO("Selection En&d"), FN(OnGoSelEnd),
               TimeSelectedFlag(),
               Options{ wxT("Ctrl+]"), XO("Skip to Selection End") } )
         )
      ),

      Section( "Windows" ),

      Section( "Other",
         Command( wxT("ShowExtraMenus"), XXO("Enable &Extra Menus"),
            FN(OnShowExtraMenus), AlwaysEnabledFlag,
            Options{}.CheckTest( wxT("/GUI/ShowExtraMenus"), false ) ),
         Command( wxT("ShowTrackNameInWaveform"), XXO("Show Track &Name as overlay"),
            FN(OnShowNameOverlay), AlwaysEnabledFlag,
            Options{}.CheckTest( wxT("/GUI/ShowTrackNameInWaveform"), false ) ),
         Command( wxT("ShowClipping"), XXO("&Show Clipping in Waveform"),
            FN(OnShowClipping), AlwaysEnabledFlag,
            Options{}.CheckTest( wxT("/GUI/ShowClipping"), false ) )
      )
   ) ) };
   return menu;
   
}

AttachedItem sAttachment1{
   wxT(""),
   Indirect(ViewMenu())
};
}

#undef FN
