#pragma once

void feedAudioData(float *buffer0, float *buffer1, int samples);
void discardMutedAudioSamples();
void setBufferedAudioSamples(int samples);
void setAudioGain(double val);

extern int bufferedAudioSamples;
