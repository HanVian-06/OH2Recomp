#ifndef OH2_PLATFORM_BRIDGES_H
#define OH2_PLATFORM_BRIDGES_H

#include <android/input.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register UIKit Touch/Event, Choreographer/CADisplayLink, and AudioToolbox->AAudio bridges */
int  oh2_platform_init(void);
void oh2_platform_shutdown(void);

/* Forward Android AInputEvent (AMotionEvent) -> UITouch/UIEvent -> FIOSView */
int32_t oh2_platform_handle_input_event(AInputEvent *event);

/* Start AChoreographer VSYNC frame callback driving CADisplayLink */
void oh2_platform_on_choreographer_attach(void);

/* Ensure AAudio output stream is opened and running (48000 / 44100 Hz) */
void oh2_platform_audio_ensure_active(void);

/* Pause/resume AAudio stream cleanly on Android background/foreground transitions */
void oh2_platform_audio_pause(void);
void oh2_platform_audio_resume(void);

/* Real MS-ADPCM (wFormatTag = 0x0002) -> PCM16 decoder for UE4/OH2 sound banks */
int  oh2_platform_decode_ms_adpcm(const uint8_t *src, size_t src_bytes,
                                  int channels, int block_align,
                                  int16_t *dst_pcm, size_t max_samples);

#ifdef __cplusplus
}
#endif

#endif /* OH2_PLATFORM_BRIDGES_H */
