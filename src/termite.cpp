/* Copyright 2013 Little IO
 *
 * termite is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * termite is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with termite.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "termite.h"

#include "cJSON.h"

#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <ncurses.h>
#include <string>
#include <stdio.h>

#define KEYBOARD "awsedftgyhujkolp;'"
#define EXTENSION ".mite"
#define NUM_CHANNELS 2
#define PITCH_BEND_PORT 224
#define SUSTAIN_PORT 176
#define SUSTAIN_ID 64

namespace {

  void midiCallback(double delta_time, std::vector<unsigned char>* message,
                    void* user_data) {
    UNUSED(delta_time);
    laf::Termite* termite = static_cast<laf::Termite*>(user_data);
    termite->processMidi(message);
  }

  int audioCallback(void *out_buffer, void *in_buffer,
                    unsigned int n_frames, double stream_time,
                    RtAudioStreamStatus status, void *user_data) {
    UNUSED(in_buffer);
    UNUSED(stream_time);
    if (status)
      std::cout << "Stream underflow detected!" << std::endl;

    laf::Termite* termite = static_cast<laf::Termite*>(user_data);
    termite->processAudio((laf::laf_float*)out_buffer, n_frames);
    return 0;
  }
} // namespace

namespace laf {
  Termite::Termite() : state_(STANDARD), pitch_bend_(0) {
    pthread_mutex_init(&mutex_, 0);
  }

  void Termite::start() {
    setupAudio();
    setupMidi();
    setupGui();

    // Wait for input.
    while(textInput(getch()))
      ;

    stop();
  }

  bool Termite::textInput(int key) {
    if (key == KEY_F(1))
      return false;

    std::string current_control = gui_.getCurrentControl();
    Control* control = controls_.at(current_control);
    switch(key) {
      case 'm':
        lock();
        if (state_ != MIDI_LEARN)
          state_ = MIDI_LEARN;
        else
          state_ = STANDARD;
        unlock();
        break;
      case 'c':
        lock();
        eraseMidiLearn(control);
        state_ = STANDARD;
        unlock();
        break;
      case KEY_UP:
        current_control = gui_.getPrevControl();
        state_ = STANDARD;
        gui_.drawControl(control, false);
        break;
      case KEY_DOWN:
        current_control = gui_.getNextControl();
        state_ = STANDARD;
        gui_.drawControl(control, false);
        break;
      case KEY_RIGHT:
        control->current_value +=
            (control->max - control->min) / control->resolution;
        break;
      case KEY_LEFT:
        control->current_value -=
            (control->max - control->min) / control->resolution;
        break;
      default:
        for (size_t i = 0; i < strlen(KEYBOARD); ++i) {
          if (KEYBOARD[i] == key) {
            lock();
            synth_.noteOn(48 + i);
            unlock();
          }
        }
    }
    control = controls_.at(current_control);
    control->current_value =
      CLAMP(control->current_value, control->min, control->max);

    control->value->set(control->current_value);
    gui_.drawControl(control, true);
    gui_.drawControlStatus(control, state_ == MIDI_LEARN);

    return true;
  }

  void Termite::setupAudio() {
    if (dac_.getDeviceCount() < 1) {
      std::cout << "No audio devices found.\n";
      exit(0);
    }

    RtAudio::StreamParameters parameters;
    parameters.deviceId = dac_.getDefaultOutputDevice();
    parameters.nChannels = NUM_CHANNELS;
    parameters.firstChannel = 0;
    unsigned int sample_rate = 44100;
    unsigned int buffer_frames = 64;

    synth_.setSampleRate(sample_rate);

    try {
      dac_.openStream(&parameters, NULL, RTAUDIO_FLOAT64, sample_rate,
                      &buffer_frames, &audioCallback, (void*)this);
      dac_.startStream();
    }
    catch (RtError& error) {
      error.printMessage();
      exit(0);
    }
  }

  void Termite::setupGui() {
    gui_.start();

    controls_ = synth_.getControls();
    gui_.addControls(controls_);

    Control* control = controls_.at(gui_.getCurrentControl());
    gui_.drawControl(control, true);
    gui_.drawControlStatus(control, false);
  }

  void Termite::processAudio(laf_float *out_buffer, unsigned int n_frames) {
    lock();
    synth_.process();
    unlock();
    const laf_float* buffer = synth_.output()->buffer;
    for (size_t i = 0; i < n_frames; ++i) {
      for (int c = 0; c < NUM_CHANNELS; ++c)
        out_buffer[NUM_CHANNELS * i + c] = buffer[i];
    }
  }

  void Termite::eraseMidiLearn(Control* control) {
    if (control->midi_learn) {
      midi_learn_.erase(control->midi_learn);
      control->midi_learn = 0;
    }
  }

  void Termite::setupMidi() {
    RtMidiIn* midi_in = new RtMidiIn();
    if (midi_in->getPortCount() < 1) {
      std::cout << "No midi devices found.\n";
    }
    for (unsigned int i = 0; i < midi_in->getPortCount(); ++i) {
      RtMidiIn* device = new RtMidiIn();
      device->openPort(i);
      device->setCallback(&midiCallback, (void*)this);
      midi_ins_.push_back(device);
    }

    delete midi_in;
  }

  void Termite::processMidi(std::vector<unsigned char>* message) {
    if (message->size() < 3)
      return;

    lock();
    int midi_port = message->at(0);
    int midi_id = message->at(1);
    int midi_val = message->at(2);
    Control* selected_control = controls_.at(current_control_);
    if (midi_port >= 144 && midi_port < 160) {
      int midi_note = midi_id;
      int midi_velocity = midi_val;

      if (midi_velocity)
        synth_.noteOn(midi_note, (1.0 * midi_velocity) / MIDI_SIZE);
      else
        synth_.noteOff(midi_note);
    }
    if (midi_port >= 128 && midi_port < 144) {
      int midi_note = midi_id;
      synth_.noteOff(midi_note);
    }
    else if (midi_port == PITCH_BEND_PORT) {
      pitch_bend_->value->set((2.0 * midi_val) / (MIDI_SIZE - 1) - 1);
      gui_.drawControl(pitch_bend_, selected_control == pitch_bend_);
    }
    else if (midi_port == SUSTAIN_PORT && midi_id == SUSTAIN_ID) {
      if (midi_val)
        synth_.sustainOn();
      else
        synth_.sustainOff();
    }
    else if (state_ == MIDI_LEARN && midi_port < 254) {
      eraseMidiLearn(selected_control);

      midi_learn_[midi_id] = selected_control;
      selected_control->midi_learn = midi_id;
      state_ = STANDARD;
      gui_.drawControlStatus(selected_control, false);
    }

    if (midi_learn_.find(midi_id) != midi_learn_.end()) {
      Control* midi_control = midi_learn_[midi_id];
      laf_float resolution = midi_control->resolution;
      int index = resolution * midi_val / (MIDI_SIZE - 1);
      midi_control->current_value = midi_control->min +
          index * (midi_control->max - midi_control->min) / resolution;
      midi_control->value->set(midi_control->current_value);
      gui_.drawControl(midi_control, selected_control == midi_control);
      gui_.drawControlStatus(midi_control, false);
    }
    unlock();
  }

  void Termite::stop() {
    pthread_mutex_destroy(&mutex_);
    gui_.stop();
    try {
      dac_.stopStream();
    }
    catch (RtError& error) {
      error.printMessage();
    }

    if (dac_.isStreamOpen())
      dac_.closeStream();
  }
} // namespace laf