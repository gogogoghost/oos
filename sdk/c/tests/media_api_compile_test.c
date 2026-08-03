#include "app.h"

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)error;
  oos_platform_audio_opened_pcm_t stream;
  oos_platform_audio_error_code_t audio_error;
  if (oos_platform_audio_pcm_open(22050, 2, 2048,
                                  OOS_PLATFORM_AUDIO_USAGE_MEDIA, &stream,
                                  &audio_error)) {
    int16_t silence[64] = {0};
    app_list_s16_t samples = {silence, 64};
    uint64_t accepted = 0;
    oos_platform_audio_pcm_write(stream.handle, &samples, &accepted,
                                 &audio_error);
    oos_platform_audio_pcm_close(stream.handle, &audio_error);
  }
  app_string_t path;
  app_string_set(&path, "audio/startup.mid");
  uint32_t player = 0;
  if (oos_platform_audio_player_open_asset(
          &path, OOS_PLATFORM_AUDIO_USAGE_MEDIA, &player, &audio_error)) {
    oos_platform_audio_player_play(player, &audio_error);
    oos_platform_audio_player_close(player, &audio_error);
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  (void)event;
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  (void)error;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
