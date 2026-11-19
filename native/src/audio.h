/* See audio.c. */
#ifndef AUDIO_H
#define AUDIO_H

void audio_setup(int live, const char* wav_path, int log); /* log: --audio-log */
void audio_frame(void); /* one 3 ms frame, once the game has finished its voice updates */
void audio_write_report(void);
/* --audio-log: the game asking NW4R to start a sound, and the sound starting. scripts/60_sound_names.py
   --annotate adds the sounds' names to the report. */
void audio_sound_requested(unsigned id);
void audio_sound_started(void);

#endif
