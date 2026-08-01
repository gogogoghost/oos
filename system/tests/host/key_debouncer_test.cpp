#include "oos/input/key_input.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct RecordedEvent {
  int64_t timestamp_us;
  uint16_t code;
  oos::input::KeyAction action;
};

void recordEvent(void *context, const oos::input::KeyEvent &event) {
  static_cast<std::vector<RecordedEvent> *>(context)->push_back(
      {event.timestamp_us, event.code, event.action});
}

oos::input::KeyEvent event(int64_t timestamp_us, uint16_t code,
                           oos::input::KeyAction action) {
  return {timestamp_us, code, action, "/dev/input/event0", "matrix_keypad"};
}

void testContactBounceIsMerged() {
  oos::input::KeyDebouncer debouncer(30000);
  std::vector<RecordedEvent> output;
  assert(debouncer.process(event(100000, 10, oos::input::KeyAction::Pressed),
                           recordEvent, &output) == 1);
  assert(debouncer.process(event(189786, 10, oos::input::KeyAction::Released),
                           recordEvent, &output) == 0);
  assert(debouncer.process(event(209808, 10, oos::input::KeyAction::Pressed),
                           recordEvent, &output) == 0);
  assert(debouncer.process(event(239816, 10, oos::input::KeyAction::Released),
                           recordEvent, &output) == 0);
  assert(debouncer.flush(269815, recordEvent, &output) == 0);
  assert(debouncer.flush(269816, recordEvent, &output) == 1);
  assert(output.size() == 2);
  assert(output[0].action == oos::input::KeyAction::Pressed);
  assert(output[0].timestamp_us == 100000);
  assert(output[1].action == oos::input::KeyAction::Released);
  assert(output[1].timestamp_us == 269816);
}

void testDistinctKeysRemainImmediate() {
  oos::input::KeyDebouncer debouncer(30000);
  std::vector<RecordedEvent> output;
  debouncer.process(event(100000, 2, oos::input::KeyAction::Pressed),
                    recordEvent, &output);
  debouncer.process(event(150000, 2, oos::input::KeyAction::Released),
                    recordEvent, &output);
  debouncer.process(event(160000, 3, oos::input::KeyAction::Pressed),
                    recordEvent, &output);
  assert(output.size() == 2);
  assert(output[0].code == 2);
  assert(output[1].code == 3);
  assert(output[1].timestamp_us == 160000);
  assert(debouncer.flush(180000, recordEvent, &output) == 1);
  assert(output[2].code == 2);
  assert(output[2].action == oos::input::KeyAction::Released);
  assert(output[2].timestamp_us == 180000);
}

void testCapturedNokia8110BouncesAreMerged() {
  oos::input::KeyDebouncer debouncer(30000);
  std::vector<RecordedEvent> output;
  for (const auto &captured : {
           event(31387230177, 7, oos::input::KeyAction::Pressed),
           event(31387240160, 7, oos::input::KeyAction::Released),
           event(31387250152, 7, oos::input::KeyAction::Pressed),
           event(31387270142, 7, oos::input::KeyAction::Released),
       })
    debouncer.process(captured, recordEvent, &output);
  debouncer.flush(31387300142, recordEvent, &output);

  for (const auto &captured : {
           event(31392210377, 7, oos::input::KeyAction::Pressed),
           event(31392220162, 7, oos::input::KeyAction::Released),
           event(31392230171, 7, oos::input::KeyAction::Pressed),
           event(31392250140, 7, oos::input::KeyAction::Released),
       })
    debouncer.process(captured, recordEvent, &output);
  debouncer.flush(31392280140, recordEvent, &output);

  assert(output.size() == 4);
  assert(output[0].action == oos::input::KeyAction::Pressed);
  assert(output[1].action == oos::input::KeyAction::Released);
  assert(output[2].action == oos::input::KeyAction::Pressed);
  assert(output[3].action == oos::input::KeyAction::Released);
  assert(output[1].timestamp_us == 31387300142);
  assert(output[3].timestamp_us == 31392280140);
}

void testReleaseTimestampsStayOrderedAcrossKeys() {
  oos::input::KeyDebouncer debouncer(30000);
  std::vector<RecordedEvent> output;
  debouncer.process(event(100000, 2, oos::input::KeyAction::Pressed),
                    recordEvent, &output);
  debouncer.process(event(110000, 2, oos::input::KeyAction::Released),
                    recordEvent, &output);
  debouncer.process(event(120000, 3, oos::input::KeyAction::Pressed),
                    recordEvent, &output);
  debouncer.flush(140000, recordEvent, &output);

  assert(output.size() == 3);
  assert(output[0].timestamp_us == 100000);
  assert(output[1].timestamp_us == 120000);
  assert(output[2].timestamp_us == 140000);
}

void testRawModePreservesEveryEvent() {
  oos::input::KeyDebouncer debouncer(0);
  std::vector<RecordedEvent> output;
  for (const auto action :
       {oos::input::KeyAction::Pressed, oos::input::KeyAction::Released,
        oos::input::KeyAction::Pressed, oos::input::KeyAction::Released})
    assert(debouncer.process(event(100000 + output.size(), 10, action),
                             recordEvent, &output) == 1);
  assert(output.size() == 4);
  assert(debouncer.nextDeadlineUs() == -1);
}

} // namespace

int main() {
  testContactBounceIsMerged();
  testDistinctKeysRemainImmediate();
  testCapturedNokia8110BouncesAreMerged();
  testReleaseTimestampsStayOrderedAcrossKeys();
  testRawModePreservesEveryEvent();
  return 0;
}
