#include "decode.h"
#include "fakes/fake_pdm.h"
#include "t.h"

static void fields_and_word_swap(void) {
  RawEvent r = ev_app_pol(0x0100abcd00001234ull, AE_IN_FOCUS, POLICY_LOG_ONLY, 777);
  Event e;
  decode_event(&r, &e);
  CHECK(e.pid == 0x0100abcd00001234ull);
  CHECK_EQ(e.play_type, PT_APPLET);
  CHECK_EQ(e.applet_id, APPLET_ID_APPLICATION);
  CHECK_EQ(e.storage_id, 5);
  CHECK_EQ(e.log_policy, POLICY_LOG_ONLY);
  CHECK_EQ(e.applet_event, AE_IN_FOCUS);
  CHECK_EQ(e.ts_steady, 777);
  CHECK_EQ(e.ts_user, 1700000777ull);
  CHECK_EQ(e.ts_net, 1700000777ull);
}

static void hand_built_offsets(void) {
  RawEvent r;
  memset(&r, 0, sizeof r);
  r.b[0] = 0x78; r.b[1] = 0x56; r.b[2] = 0x34; r.b[3] = 0x12;   /* word 0 = 0x12345678 */
  r.b[4] = 0xf0; r.b[5] = 0xde; r.b[6] = 0xbc; r.b[7] = 0x9a;   /* word 1 = 0x9abcdef0 */
  r.b[0x0c] = 1; r.b[0x0d] = 2; r.b[0x0e] = 3; r.b[0x0f] = 4;
  r.b[0x1c] = 4;
  r.b[0x20] = 0x01; r.b[0x27] = 0x80;
  r.b[0x28] = 0x02;
  r.b[0x30] = 0x03; r.b[0x37] = 0x7f;
  Event e;
  decode_event(&r, &e);
  CHECK(e.pid == 0x123456789abcdef0ull);
  CHECK_EQ(e.applet_id, 1); CHECK_EQ(e.storage_id, 2); CHECK_EQ(e.log_policy, 3); CHECK_EQ(e.applet_event, 4);
  CHECK_EQ(e.play_type, 4);
  CHECK(e.ts_user == 0x8000000000000001ull);
  CHECK(e.ts_net == 2);
  CHECK(e.ts_steady == 0x7f00000000000003ull);
  CHECK_EQ(e.power_value, 0x78);
}

static void power_value_and_padding(void) {
  RawEvent r = ev_power(7, 100);
  Event e;
  decode_event(&r, &e);
  CHECK_EQ(e.play_type, PT_POWER);
  CHECK_EQ(e.power_value, 7);
  CHECK_EQ(sizeof(RawEvent), 56);
  CHECK_EQ(PDM_EVENT_SIZE, 0x38);
}

int main(void) {
  RUN(fields_and_word_swap);
  RUN(hand_built_offsets);
  RUN(power_value_and_padding);
  return t_done("decode");
}
