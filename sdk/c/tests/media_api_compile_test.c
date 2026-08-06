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
  uint8_t midi_header[] = {'M', 'T', 'h', 'd'};
  app_list_u8_t media_bytes = {midi_header, sizeof(midi_header)};
  app_string_t incorrect_mime;
  app_string_t locator;
  app_string_set(&incorrect_mime, "application/octet-stream");
  app_string_set(&locator, "effect.bin");
  uint32_t source = 0;
  oos_platform_audio_source_limits_t limits;
  oos_platform_audio_get_source_limits(&limits);
  if (limits.maximum_sources &&
      oos_platform_audio_source_create(&media_bytes, &incorrect_mime, &locator,
                                       &source, &audio_error)) {
    if (oos_platform_audio_player_open_source(
            source, OOS_PLATFORM_AUDIO_USAGE_MEDIA, &player, &audio_error))
      oos_platform_audio_player_close(player, &audio_error);
    oos_platform_audio_source_close(source, &audio_error);
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  (void)event;
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  (void)error;
  *next_delay_ms = 1000;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
