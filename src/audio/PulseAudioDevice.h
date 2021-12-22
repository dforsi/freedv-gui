//=========================================================================
// Name:            PulseAudioDevice.h
// Purpose:         Defines the interface to a PulseAudio device.
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

#ifndef PULSE_AUDIO_DEVICE_H
#define PULSE_AUDIO_DEVICE_H

#include <string>
#include <pulse/pulseaudio.h>
#include "IAudioEngine.h"
#include "IAudioDevice.h"

class PulseAudioDevice : public IAudioDevice
{
public:
    virtual ~PulseAudioDevice();
    
    virtual int getNumChannels() { return numChannels_; }
    
    virtual void start();
    virtual void stop();
    
protected:
    // PulseAudioDevice cannot be created directly, only via PulseAudioEngine.
    friend class PulseAudioEngine;
    
    PulseAudioDevice(pa_threaded_mainloop *mainloop, pa_context* context, std::string devName, IAudioEngine::AudioDirection direction, int sampleRate, int numChannels);
    
private:
    pa_context* context_;
    pa_threaded_mainloop* mainloop_;
    pa_stream* stream_;
    
    std::string devName_;
    IAudioEngine::AudioDirection direction_;
    int sampleRate_;
    int numChannels_;
    
    static void StreamReadCallback_(pa_stream *s, size_t length, void *userdata);
    static void StreamWriteCallback_(pa_stream *s, size_t length, void *userdata);
    static void StreamUnderflowCallback_(pa_stream *p, void *userdata);
    static void StreamOverflowCallback_(pa_stream *p, void *userdata);
};

#endif // PULSE_AUDIO_DEVICE_H