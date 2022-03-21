//=========================================================================
// Name:            TxRxThread.h
// Purpose:         Implements the main processing thread for audio I/O.
//
// Authors:         Mooneer Salem
// License:
//
//  All rights reserved.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

// This forces us to use freedv-gui's version rather than another one.
// TBD -- may not be needed once we fully switch over to the audio pipeline.
#include "../defines.h"

#include "TxRxThread.h"
#include "paCallbackData.h"

#include "PlaybackStep.h"
#include "EitherOrStep.h"
#include "SpeexStep.h"
#include "EqualizerStep.h"
#include "ResamplePlotStep.h"
#include "ResampleStep.h"
#include "TapStep.h"
#include "LevelAdjustStep.h"
#include "FreeDVTransmitStep.h"
#include "RecordStep.h"
#include "ToneInterfererStep.h"
#include "ComputeRfSpectrumStep.h"
#include "FreeDVReceiveStep.h"

#include <wx/stopwatch.h>

// External globals
// TBD -- work on fully removing the need for these.
extern paCallBackData* g_rxUserdata;
extern int g_analog;
extern int g_nSoundCards;
extern bool g_half_duplex;
extern int g_tx;
extern int g_soundCard1SampleRate;
extern int g_soundCard2SampleRate;
extern int g_dump_fifo_state;
extern int g_verbose;
extern bool endingTx;
extern bool g_playFileToMicIn;
extern int g_sfTxFs;
extern bool g_loopPlayFileToMicIn;
extern float g_TxFreqOffsetHz;
extern struct FIFO* g_plotSpeechInFifo;
extern struct FIFO* g_plotDemodInFifo;
extern struct FIFO* g_plotSpeechOutFifo;
extern int g_mode;
extern bool g_recFileFromModulator;
extern int g_recFromModulatorSamples;
extern int g_txLevel;
extern int g_dump_timing;
extern bool g_queueResync;
extern int g_resyncs;
extern bool g_recFileFromRadio;
extern unsigned int g_recFromRadioSamples;
extern bool g_playFileFromRadio;
extern int g_sfFs;
extern bool g_loopPlayFileFromRadio;
extern int g_SquelchActive;
extern float g_SquelchLevel;
extern float g_tone_phase;
extern float g_avmag[MODEM_STATS_NSPEC];
extern int g_State;
extern int g_channel_noise;
extern float g_RxFreqOffsetHz;
extern float g_sig_pwr_av;

#include <speex/speex_preprocess.h>
extern SpeexPreprocessState* g_speex_st;

#include "../freedv_interface.h"
extern FreeDVInterface freedvInterface;

#include <wx/wx.h>
#include "../main.h"
extern wxMutex txModeChangeMutex;
extern wxMutex g_mutexProtectingCallbackData;
extern wxWindow* g_parent;

#include <samplerate.h>
extern SRC_STATE* g_spec_src;

#include <sndfile.h>
extern SNDFILE* g_sfPlayFile;
extern SNDFILE* g_sfRecFileFromModulator;
extern SNDFILE* g_sfRecFile;
extern SNDFILE* g_sfPlayFileFromRadio;

// TBD -- shouldn't be needed once we've fully converted over
extern int resample(SRC_STATE *src,
            short      output_short[],
            short      input_short[],
            int        output_sample_rate,
            int        input_sample_rate,
            int        length_output_short, // maximum output array length in samples
            int        length_input_short
            );
#include "sox_biquad.h"

void TxRxThread::initializePipeline_()
{
    if (m_tx)
    {
        pipeline_ = std::shared_ptr<AudioPipeline>(new AudioPipeline(g_soundCard2SampleRate, g_soundCard1SampleRate));
        
        // Mic In playback step (optional)
        auto eitherOrBypassPlay = new AudioPipeline(g_soundCard2SampleRate, g_soundCard2SampleRate);
        auto eitherOrPlayMicIn = new AudioPipeline(g_soundCard2SampleRate, g_soundCard2SampleRate);
        auto playMicIn = new PlaybackStep(
            g_soundCard2SampleRate, 
            []() { return g_sfTxFs; },
            []() { return g_sfPlayFile; },
            []() {
                if (g_loopPlayFileToMicIn)
                    sf_seek(g_sfPlayFile, 0, SEEK_SET);
                else {
                    printf("playFileFromRadio finished, issuing event!\n");
                    g_parent->CallAfter(&MainFrame::StopPlayFileToMicIn);
                }
            }
            );
        eitherOrPlayMicIn->appendPipelineStep(std::shared_ptr<IPipelineStep>(playMicIn));
        
        auto eitherOrPlayStep = new EitherOrStep(
            []() { return g_playFileToMicIn && (g_sfPlayFile != NULL); },
            std::shared_ptr<IPipelineStep>(eitherOrPlayMicIn),
            std::shared_ptr<IPipelineStep>(eitherOrBypassPlay));
            
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrPlayStep));
        
        // Speex step (optional based on g_speex_st)
        auto speexStep = new SpeexStep(g_soundCard2SampleRate, &g_speex_st);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(speexStep));
        
        // Equalizer step (optional based on filter state)
        auto equalizerStep = new EqualizerStep(
            g_soundCard2SampleRate, 
            &g_rxUserdata->sbqMicInBass,
            &g_rxUserdata->sbqMicInMid,
            &g_rxUserdata->sbqMicInTreble,
            &g_rxUserdata->sbqMicInVol);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(equalizerStep));
        
        // Resample for plot step
        auto resampleForPlotStep = new ResampleForPlotStep(g_soundCard2SampleRate, g_plotSpeechInFifo);
        auto resampleForPlotTap = new TapStep(g_soundCard2SampleRate, resampleForPlotStep);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(resampleForPlotTap));
        
        // FreeDV TX step (analog leg)
        auto doubleLevelStep = new LevelAdjustStep(g_soundCard2SampleRate, []() { return 2.0; });
        auto analogTxPipeline = new AudioPipeline(g_soundCard2SampleRate, g_soundCard1SampleRate);
        analogTxPipeline->appendPipelineStep(std::shared_ptr<IPipelineStep>(doubleLevelStep));
        
        auto digitalTxStep = new FreeDVTransmitStep(freedvInterface, []() { return g_TxFreqOffsetHz; });
        auto digitalTxPipeline = new AudioPipeline(g_soundCard2SampleRate, g_soundCard1SampleRate); 
        digitalTxPipeline->appendPipelineStep(std::shared_ptr<IPipelineStep>(digitalTxStep));
        
        auto eitherOrDigitalAnalog = new EitherOrStep(
            []() { return g_analog; },
            std::shared_ptr<IPipelineStep>(analogTxPipeline),
            std::shared_ptr<IPipelineStep>(digitalTxPipeline));
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrDigitalAnalog));
        
        // Record modulated output (optional)
        auto recordModulatedStep = new RecordStep(
            g_soundCard1SampleRate, 
            []() { return g_sfRecFileFromModulator; }, 
            [](int numSamples) {
                g_recFromModulatorSamples -= numSamples;
                if (g_recFromModulatorSamples <= 0)
                {
                    // call stop record menu item, should be thread safe
                    g_parent->CallAfter(&MainFrame::StopRecFileFromModulator);
                
                    wxPrintf("write mod output to file complete\n", g_recFromModulatorSamples);  // consider a popup
                }
            });
        auto recordModulatedTap = new TapStep(g_soundCard1SampleRate, recordModulatedStep);
        auto bypassRecordModulated = new AudioPipeline(g_soundCard1SampleRate, g_soundCard2SampleRate);
        auto eitherOrRecordModulated = new EitherOrStep(
            []() { return g_recFileFromModulator && (g_sfRecFileFromModulator != NULL); },
            std::shared_ptr<IPipelineStep>(recordModulatedTap),
            std::shared_ptr<IPipelineStep>(bypassRecordModulated));
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrRecordModulated));
        
        // TX attenuation step
        auto txAttenuationStep = new LevelAdjustStep(g_soundCard1SampleRate, []() {
            double dbLoss = g_txLevel / 10.0;
            double scaleFactor = exp(dbLoss/20.0 * log(10.0));
            return scaleFactor; 
        });
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(txAttenuationStep));
    }
    else
    {
        pipeline_ = std::shared_ptr<AudioPipeline>(new AudioPipeline(g_soundCard1SampleRate, g_soundCard2SampleRate));
        
        // Record from radio step (optional)
        auto recordRadioStep = new RecordStep(
            g_soundCard1SampleRate, 
            []() { return g_sfRecFile; }, 
            [](int numSamples) {
                g_recFromRadioSamples -= numSamples;
                if (g_recFromRadioSamples <= 0)
                {
                    // call stop record menu item, should be thread safe
                    g_parent->CallAfter(&MainFrame::StopRecFileFromRadio);
                }
            }
        );
        auto bypassRecordRadio = new AudioPipeline(g_soundCard1SampleRate, g_soundCard1SampleRate);
        auto eitherOrRecordRadio = new EitherOrStep(
            []() { return g_recFileFromRadio && (g_sfRecFile != NULL); },
            std::shared_ptr<IPipelineStep>(recordRadioStep),
            std::shared_ptr<IPipelineStep>(bypassRecordRadio)
        );
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrRecordRadio));
        
        // Play from radio step (optional)
        auto eitherOrBypassPlayRadio = new AudioPipeline(g_soundCard1SampleRate, g_soundCard1SampleRate);
        auto eitherOrPlayRadio = new AudioPipeline(g_soundCard1SampleRate, g_soundCard1SampleRate);
        auto playRadio = new PlaybackStep(
            g_soundCard1SampleRate, 
            []() { return g_sfFs; },
            []() { return g_sfPlayFileFromRadio; },
            []() {
                if (g_loopPlayFileFromRadio)
                    sf_seek(g_sfPlayFileFromRadio, 0, SEEK_SET);
                else {
                    printf("playFileFromRadio finished, issuing event!\n");
                    g_parent->CallAfter(&MainFrame::StopPlaybackFileFromRadio);
                }
            }
        );
        eitherOrPlayRadio->appendPipelineStep(std::shared_ptr<IPipelineStep>(playRadio));
        
        auto eitherOrPlayRadioStep = new EitherOrStep(
            []() { return g_playFileFromRadio && (g_sfPlayFileFromRadio != NULL); },
            std::shared_ptr<IPipelineStep>(eitherOrPlayRadio),
            std::shared_ptr<IPipelineStep>(eitherOrBypassPlayRadio));
            
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrPlayRadioStep));
        
        // Resample for plot step (demod in)
        auto resampleForPlotStep = new ResampleForPlotStep(g_soundCard1SampleRate, g_plotDemodInFifo);
        auto resampleForPlotTap = new TapStep(g_soundCard1SampleRate, resampleForPlotStep);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(resampleForPlotTap));
        
        // Tone interferer step (optional)
        auto bypassToneInterferer = new AudioPipeline(g_soundCard1SampleRate, g_soundCard1SampleRate);
        auto toneInterfererStep = new ToneInterfererStep(
            g_soundCard1SampleRate,
            []() { return wxGetApp().m_tone_freq_hz; },
            []() { return wxGetApp().m_tone_amplitude; },
            []() { return &g_tone_phase; }
        );
        auto eitherOrToneInterferer = new EitherOrStep(
            []() { return wxGetApp().m_tone; },
            std::shared_ptr<IPipelineStep>(toneInterfererStep),
            std::shared_ptr<IPipelineStep>(bypassToneInterferer)
        );
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrToneInterferer));
        
        // RF spectrum computation step
        auto computeRfSpectrumStep = new ComputeRfSpectrumStep(
            []() { return freedvInterface.getCurrentRxModemStats(); },
            []() { return &g_avmag[0]; }
        );
        auto computeRfSpectrumPipeline = new AudioPipeline(
            g_soundCard1SampleRate, g_soundCard1SampleRate);
        computeRfSpectrumPipeline->appendPipelineStep(std::shared_ptr<IPipelineStep>(computeRfSpectrumStep));
        
        auto computeRfSpectrumTap = new TapStep(g_soundCard1SampleRate, computeRfSpectrumPipeline);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(computeRfSpectrumTap));
        
        // RX demodulation step
        auto bypassRfDemodulationPipeline = new AudioPipeline(g_soundCard1SampleRate, g_soundCard2SampleRate);
        auto rfDemodulationPipeline = new AudioPipeline(g_soundCard1SampleRate, g_soundCard2SampleRate);
        auto rfDemodulationStep = new FreeDVReceiveStep(
            freedvInterface,
            []() { return &g_State; },
            []() { return g_rxUserdata->rxoutfifo; },
            []() { return g_channel_noise; },
            []() { return wxGetApp().m_noise_snr; },
            []() { return g_RxFreqOffsetHz; },
            []() { return &g_sig_pwr_av; }
        );
        rfDemodulationPipeline->appendPipelineStep(std::shared_ptr<IPipelineStep>(rfDemodulationStep));
        
        auto eitherOrRfDemodulationStep = new EitherOrStep(
            []() { return g_analog; },
            std::shared_ptr<IPipelineStep>(bypassRfDemodulationPipeline),
            std::shared_ptr<IPipelineStep>(rfDemodulationPipeline)
        );
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(eitherOrRfDemodulationStep));
        
        // Equalizer step (optional based on filter state)
        auto equalizerStep = new EqualizerStep(
            g_soundCard2SampleRate, 
            &g_rxUserdata->sbqSpkOutBass,
            &g_rxUserdata->sbqSpkOutMid,
            &g_rxUserdata->sbqSpkOutTreble,
            &g_rxUserdata->sbqSpkOutVol);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(equalizerStep));
        
        // Resample for plot step (speech out)
        auto resampleForPlotOutStep = new ResampleForPlotStep(g_soundCard2SampleRate, g_plotSpeechOutFifo);
        auto resampleForPlotOutTap = new TapStep(g_soundCard2SampleRate, resampleForPlotOutStep);
        pipeline_->appendPipelineStep(std::shared_ptr<IPipelineStep>(resampleForPlotOutTap));
        
    }
}

//---------------------------------------------------------------------------------------------
// Main real time processing for tx and rx of FreeDV signals, run in its own threads
//---------------------------------------------------------------------------------------------

void TxRxThread::txProcessing()
{
    wxStopWatch sw;

    paCallBackData  *cbData = g_rxUserdata;

    // Buffers re-used by tx and rx processing.  We take samples from
    // the sound card, and resample them for the freedv modem input
    // sample rate.  Typically the sound card is running at 48 or 44.1
    // kHz, and the modem at 8kHz, however some modems such as FreeDV
    // 2400A/B run at 48 kHz.

    // allocate enough room for 20ms processing buffers at maximum
    // sample rate of 48 kHz.  Note these buffer are used by rx and tx
    // side processing

    short           insound_card[10*N48];
    int             nout, freedv_samplerate;

    // analog mode runs at the standard FS = 8000 Hz
    if (g_analog) {
        freedv_samplerate = FS;
    }
    else {
        // Use the maximum modem sample rate. Any needed downconversion
        // just prior to sending to Codec2 will happen in FreeDVInterface.
        freedv_samplerate = freedvInterface.getRxModemSampleRate();
    }
    //fprintf(stderr, "sample rate: %d\n", freedv_samplerate);

    //
    //  TX side processing --------------------------------------------
    //

    if (((g_nSoundCards == 2) && ((g_half_duplex && g_tx) || !g_half_duplex))) {
        // Lock the mode mutex so that TX state doesn't change on us during processing.
        txModeChangeMutex.Lock();
        
        // This while loop locks the modulator to the sample rate of
        // sound card 1.  We want to make sure that modulator samples
        // are uninterrupted by differences in sample rate between
        // this sound card and sound card 2.

        // Run code inside this while loop as soon as we have enough
        // room for one frame of modem samples.  Aim is to keep
        // outfifo1 nice and full so we don't have any gaps in tx
        // signal.

        unsigned int nsam_one_modem_frame = g_soundCard1SampleRate * freedvInterface.getTxNNomModemSamples()/freedv_samplerate;

     	if (g_dump_fifo_state) {
    	  // If this drops to zero we have a problem as we will run out of output samples
    	  // to send to the sound driver via PortAudio
    	  if (g_verbose) fprintf(stderr, "outfifo1 used: %6d free: %6d nsam_one_modem_frame: %d\n",
                      codec2_fifo_used(cbData->outfifo1), codec2_fifo_free(cbData->outfifo1), nsam_one_modem_frame);
    	}

        int nsam_in_48 = g_soundCard2SampleRate * freedvInterface.getTxNumSpeechSamples()/freedvInterface.getTxSpeechSampleRate();
        assert(nsam_in_48 < 10*N48);
        
        while((unsigned)codec2_fifo_free(cbData->outfifo1) >= nsam_one_modem_frame) {        
            // OK to generate a frame of modem output samples we need
            // an input frame of speech samples from the microphone.

            // infifo2 is written to by another sound card so it may
            // over or underflow, but we don't really care.  It will
            // just result in a short interruption in audio being fed
            // to codec2_enc, possibly making a click every now and
            // again in the decoded audio at the other end.

            // zero speech input just in case infifo2 underflows
            memset(insound_card, 0, nsam_in_48*sizeof(short));
            
            // There may be recorded audio left to encode while ending TX. To handle this,
            // we keep reading from the FIFO until we have less than nsam_in_48 samples available.
            int nread = codec2_fifo_read(cbData->infifo2, insound_card, nsam_in_48);            
            if (nread != 0 && endingTx) break;
            
            short* inputSamples = new short[nsam_in_48];
            memcpy(inputSamples, insound_card, nsam_in_48 * sizeof(short));
            
            auto inputSamplesPtr = std::shared_ptr<short>(inputSamples, std::default_delete<short[]>());
            auto outputSamples = pipeline_->execute(inputSamplesPtr, nsam_in_48, &nout);
            
            if (g_dump_fifo_state) {
                fprintf(stderr, "  nout: %d\n", nout);
            }
            
            codec2_fifo_write(cbData->outfifo1, outputSamples.get(), nout);
        }
        
        txModeChangeMutex.Unlock();
    }

    if (g_dump_timing) {
        fprintf(stderr, "%4ld", sw.Time());
    }
}

void TxRxThread::rxProcessing()
{
    wxStopWatch sw;

    paCallBackData  *cbData = g_rxUserdata;

    // Buffers re-used by tx and rx processing.  We take samples from
    // the sound card, and resample them for the freedv modem input
    // sample rate.  Typically the sound card is running at 48 or 44.1
    // kHz, and the modem at 8kHz, however some modems such as FreeDV
    // 2400A/B run at 48 kHz.

    // allocate enough room for 20ms processing buffers at maximum
    // sample rate of 48 kHz.  Note these buffer are used by rx and tx
    // side processing

    short           insound_card[10*N48];
    int             nout;

    //
    //  RX side processing --------------------------------------------
    //
    
    if (g_queueResync)
    {
        if (g_verbose) fprintf(stderr, "Unsyncing per user request.\n");
        g_queueResync = false;
        freedvInterface.setSync(FREEDV_SYNC_UNSYNC);
        g_resyncs++;
    }
    
    // Attempt to read one processing frame (about 20ms) of receive samples,  we 
    // keep this frame duration constant across modes and sound card sample rates
    int nsam = (int)(g_soundCard1SampleRate * FRAME_DURATION);
    assert(nsam <= 10*N48);
    assert(nsam != 0);

    // while we have enough input samples available ... 
    while (codec2_fifo_read(cbData->infifo1, insound_card, nsam) == 0 && ((g_half_duplex && !g_tx) || !g_half_duplex)) {

        // send latest squelch level to FreeDV API, as it handles squelch internally
        freedvInterface.setSquelch(g_SquelchActive, g_SquelchLevel);

        short* inputSamples = new short[nsam];
        memcpy(inputSamples, insound_card, nsam * sizeof(short));
        
        auto inputSamplesPtr = std::shared_ptr<short>(inputSamples, std::default_delete<short[]>());
        auto outputSamples = pipeline_->execute(inputSamplesPtr, nsam, &nout);
        auto outFifo = (g_nSoundCards == 1) ? cbData->outfifo1 : cbData->outfifo2;
        codec2_fifo_write(outFifo, outputSamples.get(), nout);
    }
}