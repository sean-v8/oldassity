/**********************************************************************

Audacity: A Digital Audio Editor

MixAndRender.h
@brief Generate mono or stereo track mixing other given tracks

Paul Licameli split from Mix.h

**********************************************************************/

#ifndef __AUDACITY_MIX_AND_RENDER_H
#define __AUDACITY_MIX_AND_RENDER_H

#include "BasicUI.h"
#include "Mix.h"
#include "SampleFormat.h"
#include "Track.h"

class WaveTrack;
class WaveTrackFactory;

#include <functional>
#include <memory>

/** @brief Mixes together all input tracks, applying any envelopes, amplitude
 * gain, panning, and real-time effects in the process.
 *
 * Takes one or more tracks as input; of all the WaveTrack s,
 * it mixes them together, applying any envelopes, amplitude gain, panning, and
 * real-time effects in the process.  The resulting pair of tracks (stereo) are
 * "rendered" and have no effects, gain, panning, or envelopes. Other sorts of
 * tracks are ignored.
 * If the start and end times passed are the same this is taken as meaning
 * no explicit time range to process, and the whole occupied length of the
 * input tracks is processed.
 * Channel group properties of the result are copied from the first input track.
 *
 * @param newTrackName used only when there is more than one input track (one
 * mono channel or a stereo pair); else the unique track's name is copied
 *
 * @param reportProgress optional progress and cancellation callback, called
 * synchronously with elapsed and total render time in seconds. Supplying it
 * lets the caller own progress presentation; otherwise use the standard dialog.
 *
 * @pre `trackRange` iterates over leaders only
 */
EFFECTS_API TrackListHolder MixAndRender(
   const TrackIterRange<const WaveTrack> &trackRange,
   const Mixer::WarpOptions &warpOptions,
   const wxString &newTrackName,
   WaveTrackFactory *factory,
   double rate, sampleFormat format,
   double startTime, double endTime,
   const std::function<BasicUI::ProgressResult(double, double)> &reportProgress = {});

// A render whose bounded processing steps can be scheduled by a project
// without opening a progress dialog or pumping the window event loop.
// The caller keeps the source tracks alive and stable until the job ends.
class EFFECTS_API MixRenderJob final
{
public:
   static std::unique_ptr<MixRenderJob> Begin(
      const TrackIterRange<const WaveTrack> &tracks,
      const Mixer::WarpOptions &warpOptions, const wxString &newTrackName,
      WaveTrackFactory &factory, double rate, sampleFormat format,
      double startTime, double endTime);

   //! Process up to maxSamples. False means end of input.
   bool ProcessNext(size_t maxSamples);

   // Process on the caller thread, then synchronously dispatch the detached
   // track write to the UI thread. The mixer buffers must remain alive.
   bool ProcessNext(size_t maxSamples,
      const std::function<void(std::function<void()>)> &write);

   double Elapsed();
   double Duration() const { return mEndTime - mStartTime; }
   //! Return the completed (or deliberately stopped) mix. Call once.
   TrackListHolder Finish();
   //! Destroy the job without Finish to discard its detached output.

private:
   MixRenderJob(TrackListHolder result, std::unique_ptr<Mixer> mixer,
      sampleFormat format, double startTime, double endTime);

   TrackListHolder mResult;
   std::unique_ptr<Mixer> mMixer;
   sampleFormat mFormat;
   double mStartTime;
   double mEndTime;
};

enum ChannelName : int;
using ChannelNames = const ChannelName *;

EFFECTS_API
std::vector<MixerOptions::StageSpecification>
GetEffectStages(const WaveTrack &track);

#endif
