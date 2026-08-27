#ifndef ASNX_FMOD_AUDIO_H
#define ASNX_FMOD_AUDIO_H
void fmod_audio_bind(void *getinfo,void *process,void *micdata);
int fmod_audio_start(void);
void fmod_audio_stop(void);
#endif
