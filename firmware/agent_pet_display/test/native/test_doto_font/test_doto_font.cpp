#include <unity.h>

#include "doto_font.h"

void setUp(void) {}

void tearDown(void) {}

bool glyphHasInk(DotoFontSize size, char ch) {
  const int advance = dotoGlyphAdvance(size, ch);
  const int height = dotoTextHeight(size);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < advance; ++x) {
      if (dotoGlyphPixel(size, ch, static_cast<uint8_t>(x), static_cast<uint8_t>(y))) {
        return true;
      }
    }
  }
  return false;
}

int textInkPixels(DotoFontSize size, const char* text) {
  int ink = 0;
  for (const char* cursor = text; *cursor != 0; ++cursor) {
    const int advance = dotoGlyphAdvance(size, *cursor);
    const int height = dotoTextHeight(size);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < advance; ++x) {
        if (dotoGlyphPixel(size, *cursor, static_cast<uint8_t>(x), static_cast<uint8_t>(y))) {
          ++ink;
        }
      }
    }
  }
  return ink;
}

bool rowHasInk(DotoFontSize size, char ch, int y) {
  for (int x = 0; x < dotoGlyphAdvance(size, ch); ++x) {
    if (dotoGlyphPixel(size, ch, static_cast<uint8_t>(x), static_cast<uint8_t>(y))) {
      return true;
    }
  }
  return false;
}

int lastInkRow(DotoFontSize size, char ch) {
  for (int y = dotoTextHeight(size) - 1; y >= 0; --y) {
    if (rowHasInk(size, ch, y)) return y;
  }
  return -1;
}

void test_font_ascii_glyphs_and_small_details_are_preserved(void) {
  const DotoFontSize sizes[] = {DotoFontSize::Small, DotoFontSize::Medium, DotoFontSize::Hero};
  for (DotoFontSize size : sizes) {
    const int height = dotoTextHeight(size);
    TEST_ASSERT_FALSE(glyphHasInk(size, ' '));
    for (int code = '!'; code <= '~'; ++code) {
      const char ch = static_cast<char>(code);
      const int advance = dotoGlyphAdvance(size, ch);
      TEST_ASSERT_TRUE(advance > 0);
      TEST_ASSERT_TRUE(glyphHasInk(size, ch));
      TEST_ASSERT_FALSE(dotoGlyphPixel(size, ch, static_cast<uint8_t>(advance), 0));
      TEST_ASSERT_FALSE(dotoGlyphPixel(size, ch, 0, static_cast<uint8_t>(height)));
    }
    bool dot = false;
    bool gap = false;
    bool stem = false;
    for (int y = 0; y < height; ++y) {
      const bool ink = rowHasInk(size, 'i', y);
      if (ink && gap) stem = true;
      if (!ink && dot) gap = true;
      if (ink) dot = true;
    }
    TEST_ASSERT_TRUE(stem);
    TEST_ASSERT_TRUE(lastInkRow(size, 'g') > lastInkRow(size, 'o'));
  }
}

void test_doto_font_has_metrics_for_clock_and_agent_text(void) {
  TEST_ASSERT_TRUE(dotoTextHeight(DotoFontSize::Hero) >= 22);
  TEST_ASSERT_TRUE(dotoTextWidth(DotoFontSize::Hero, "09:52:15") > 80);
  TEST_ASSERT_TRUE(dotoTextWidth(DotoFontSize::Medium, "THINKING") > 40);
}

void test_doto_font_keeps_ascii_punctuation_needed_by_rlcd_ui(void) {
  TEST_ASSERT_TRUE(glyphHasInk(DotoFontSize::Medium, ':'));
  TEST_ASSERT_TRUE(dotoTextWidth(DotoFontSize::Medium, "35.0k / 258k") > 70);
  TEST_ASSERT_EQUAL(0, dotoTextWidth(DotoFontSize::Medium, ""));
}

// The face used to be the dotted "Doto" font (ink < 22% of the cell area);
// since the switch to rasterized Inter the glyphs are solid, so the sanity
// band moves accordingly: not empty, not a solid blob.
void test_font_glyphs_have_reasonable_ink_coverage(void) {
  const char* sample = "THINKING";
  const int totalPixels = dotoTextWidth(DotoFontSize::Hero, sample) * dotoTextHeight(DotoFontSize::Hero);
  const int inkPixels = textInkPixels(DotoFontSize::Hero, sample);

  TEST_ASSERT_TRUE(inkPixels * 100 > totalPixels * 10);
  TEST_ASSERT_TRUE(inkPixels * 100 < totalPixels * 60);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_doto_font_has_metrics_for_clock_and_agent_text);
  RUN_TEST(test_doto_font_keeps_ascii_punctuation_needed_by_rlcd_ui);
  RUN_TEST(test_font_glyphs_have_reasonable_ink_coverage);
  RUN_TEST(test_font_ascii_glyphs_and_small_details_are_preserved);
  return UNITY_END();
}
