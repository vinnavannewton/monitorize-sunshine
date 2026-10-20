#include "src/listener_codec_policy.h"
#include <gtest/gtest.h>

TEST(ListenerCodec, AutoAdvertisesOnlySuccessfullyTestedCodecs) {
  for (unsigned supported=0; supported<8; ++supported)
    EXPECT_EQ(listener_codec::advertise(supported,0,0),supported);
}
TEST(ListenerCodec, ForcedModesNeverAdvertiseAnAlternativeAdvancedCodec) {
  EXPECT_EQ(listener_codec::advertise(7,1,2),5U);
  EXPECT_EQ(listener_codec::advertise(7,2,1),3U);
  EXPECT_EQ(listener_codec::advertise(7,1,1),1U);
}
TEST(ListenerCodec, ForcedCodecMismatchIsRejectedInsteadOfSubstitution) {
  for (int format=0; format<3; ++format) {
    EXPECT_EQ(listener_codec::accepts(format,1,2),format==2);
    EXPECT_EQ(listener_codec::accepts(format,2,1),format==1);
    EXPECT_EQ(listener_codec::accepts(format,1,1),format==0);
    EXPECT_TRUE(listener_codec::accepts(format,0,0));
  }
  EXPECT_FALSE(listener_codec::accepts(-1,0,0));
  EXPECT_FALSE(listener_codec::accepts(3,0,0));
}

TEST(ListenerCodec, UnsupportedForcedCodecDoesNotAdvertiseFallback) {
  EXPECT_EQ(listener_codec::advertise(1,1,2),0U);
  EXPECT_EQ(listener_codec::advertise(1,2,1),0U);
}
