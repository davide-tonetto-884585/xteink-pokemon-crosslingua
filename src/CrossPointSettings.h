#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <HalStorage.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <mutex>

#include "ReaderFontSizeStep.h"
#include "util/ReaderStatusBarConfig.h"

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  mutable std::mutex _mutex;

  CrossPointSettings() = default;
  friend class PersistableStore<CrossPointSettings>;

 public:
  // Access the settings mutex for protecting multi-field reads/writes from other cores.
  // Callers must not re-enter SETTINGS methods that lock _mutex while holding it.
  std::mutex& getMutex() const { return _mutex; }

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    OVERLAY = 6,
    READING_STATS_SLEEP = 7,
    MINIMAL_SLEEP = 8,
    QUICK_RESUME = 9,
    MINIMAL_STATS_SLEEP = 10,
    DASHBOARD_SLEEP = 11,
    // Pokemon builds only: a random party member asleep in its Poke Ball.
    POKEMON_SLEEP = 12,
    // Pokemon builds only: the same room filling the whole screen.
    POKEMON_FULL_SLEEP = 13,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLIDESHOW_SCALE_MODE { SLIDESHOW_FIT = 0, SLIDESHOW_CROP = 1, SLIDESHOW_SCALE_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum BOOK_PERCENTAGE_FORMAT {
    BOOK_PERCENTAGE_WHOLE = 0,
    BOOK_PERCENTAGE_ONE_DECIMAL = 1,
    BOOK_PERCENTAGE_TWO_DECIMALS = 2,
    BOOK_PERCENTAGE_FORMAT_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum STATUS_BAR_TIME_LEFT {
    TIME_LEFT_HIDE = 0,
    TIME_LEFT_CHAPTER = 1,
    TIME_LEFT_BOOK = 2,
    STATUS_BAR_TIME_LEFT_COUNT
  };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_BOTH = 3,
    XTC_STATUS_BAR_MODE_COUNT
  };
  enum HIDE_CLOCK_MODE { HIDE_CLOCK_NEVER = 0, HIDE_CLOCK_IN_READER = 1, HIDE_CLOCK_ALWAYS = 2, HIDE_CLOCK_MODE_COUNT };
  // Persisted date-format values mirror HalClock::DateFormat.
  enum DATE_FORMAT {
    DATE_FORMAT_MONTH_DAY_YEAR_LONG = 0,
    DATE_FORMAT_DAY_MONTH_YEAR_LONG = 1,
    DATE_FORMAT_MONTH_DAY_YEAR_NUMERIC = 2,
    DATE_FORMAT_DAY_MONTH_YEAR_NUMERIC = 3,
    DATE_FORMAT_YEAR_MONTH_DAY_NUMERIC = 4,
    DATE_FORMAT_MONTH_DAY_NUMERIC = 5,
    DATE_FORMAT_DAY_MONTH_NUMERIC = 6,
    DATE_FORMAT_MONTH_DAY_LONG = 7,
    DATE_FORMAT_DAY_MONTH_LONG = 8,
    DATE_FORMAT_COUNT
  };
  enum DATE_SEPARATOR {
    DATE_SEPARATOR_PERIOD = 0,
    DATE_SEPARATOR_HYPHEN = 1,
    DATE_SEPARATOR_SLASH = 2,
    DATE_SEPARATOR_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT {
    PREV_NEXT = 0,
    NEXT_PREV = 1,
    SIDE_BUTTONS_DISABLED = 2,
    NEXT_NEXT = 3,
    SIDE_BUTTON_LAYOUT_COUNT
  };

  enum FRONT_BUTTON_ORIENTATION_AWARE {
    FRONT_ORIENTATION_AWARE_OFF = 0,
    FRONT_ORIENTATION_AWARE_NAV_BUTTONS = 1,
    FRONT_ORIENTATION_AWARE_ALL_BUTTONS = 2,
    FRONT_ORIENTATION_AWARE_COUNT
  };

  enum TWO_FINGER_SWIPE_ACTION {
    TWO_FINGER_SWIPE_NOT_SET = 0,
    TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS,
    TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS,
    TWO_FINGER_SWIPE_INCREASE_WARMTH,
    TWO_FINGER_SWIPE_DECREASE_WARMTH,
    TWO_FINGER_SWIPE_NEXT_CHAPTER,
    TWO_FINGER_SWIPE_PREVIOUS_CHAPTER,
    TWO_FINGER_SWIPE_INCREASE_FONT_SIZE,
    TWO_FINGER_SWIPE_DECREASE_FONT_SIZE,
    TWO_FINGER_SWIPE_ACTION_COUNT,
  };

  // Side button long-press action options
  enum SIDE_LONG_PRESS {
    SIDE_LONG_CHAPTER_SKIP = 0,
    SIDE_LONG_FONT_SIZE = 1,
    SIDE_LONG_OFF = 2,
    SIDE_LONG_ORIENTATION_CHANGE = 3,
    SIDE_LONG_PRESS_COUNT
  };

  // Side-button actions share shortcut IDs with Power. These extra IDs are
  // reader-only actions; keep their persisted values separate and stable.
  enum SIDE_BUTTON_ACTION : uint8_t {
    SIDE_PREVIOUS_CHAPTER = 64,
    SIDE_NEXT_CHAPTER,
    SIDE_INCREASE_FONT,
    SIDE_DECREASE_FONT,
    SIDE_ROTATE_COUNTERCLOCKWISE,
    SIDE_ROTATE_CLOCKWISE,
    SIDE_ROTATE_FLIP,
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName)
  enum FONT_FAMILY { LEXENDDECA = 0, BITTER = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // Font size options
  enum FONT_SIZE { TINY = 0, SMALL = 1, MEDIUM = 2, LARGE = 3, FONT_SIZE_COUNT };
  enum SD_FONT_SIZE_RANGE {
    SD_FONT_RANGE_TEENSY = 0,
    SD_FONT_RANGE_TINY = 1,
    SD_FONT_RANGE_XLARGE = 2,
    SD_FONT_RANGE_NO_EMOJI_LEGACY = 3,
    SD_FONT_RANGE_ALL = 4,
    SD_FONT_SIZE_RANGE_COUNT
  };
  // Legacy persisted values for the old Tight / Normal / Wide line-spacing setting.
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_FREQUENCY_COUNT
  };

  enum FILE_BROWSER_DISPLAY {
    FILE_BROWSER_DISPLAY_1_LINE = 0,
    FILE_BROWSER_DISPLAY_2_LINES = 1,
    FILE_BROWSER_DISPLAY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    TOGGLE_FONT = 4,
    TOGGLE_GUIDE_DOTS = 5,
    TOGGLE_FOCUS_READING = 6,
    TOGGLE_BOOKMARK = 7,
    SYNC_PROGRESS = 8,
    MARK_FINISHED = 9,
    READING_STATS = 10,
    SCREENSHOT = 11,
    CYCLE_PAGE_TURN = 12,
    FILE_TRANSFER = 13,
    TOGGLE_TILT_PAGE_TURN = 14,
    TOGGLE_DARK_MODE = 15,
    FOOTNOTES = 16,
    FILE_BROWSER = 17,
    CALIBRE_WIRELESS = 18,
    JOIN_NETWORK = 19,
    CREATE_HOTSPOT = 20,
    CREATE_CLIPPING = 21,
    LOOKUP_WORD = 22,
    // Values 23-26 are already persisted by the X4 Pro Home-key feature.
    // Keep Quick Actions separate so existing Home-key mappings retain their meaning.
    TOGGLE_HOME_BUTTON_IN_READER = 26,
    QUICK_ACTIONS = 27,
    TOGGLE_FRONTLIGHT = 28,
    TOGGLE_TOUCHSCREEN = 29,
    // Appended after the X4 Pro and Quick Actions values so existing settings
    // files continue to mean exactly the same thing.
    QUICK_LOCK = 30,
    // Shortcut values are persisted. Append new actions; never reuse removed
    // raw values or they can silently change an existing binding's behavior.
    PREVIOUS_PAGE = 31,
    NEARBY_POSITION_SYNC = 32,
    LIBRARY = 33,
    // Power-only choices. Keep SLEEP=1 as the existing Sleep/Wake setting.
    SLEEP_ONLY = 34,
    WAKE_ONLY = 35,
    HOME_READER = 36,
    SHORT_PWRBTN_COUNT
  };

  // Power + Up side-button chord actions. Keep this order aligned with
  // ButtonShortcutController::ChordAction because the runtime casts the
  // persisted value to that enum.
  enum POWER_CHORD_ACTION {
    CHORD_SCREENSHOT = 0,
    CHORD_QUICK_LOCK = 1,
    // Values 2 and 3 were removed Next Page and Previous Page actions. Keep
    // them unused so an interim settings file cannot remap them to another action.
    CHORD_DISABLED = 4,
    CHORD_SLEEP = 5,
    CHORD_PAGE_TURN = 6,
    CHORD_TOGGLE_BOOKMARK = 7,
    CHORD_READING_STATS = 8,
    CHORD_MARK_FINISHED = 9,
    CHORD_FORCE_REFRESH = 10,
    CHORD_TOGGLE_FONT = 11,
    CHORD_TOGGLE_GUIDE_DOTS = 12,
    CHORD_TOGGLE_FOCUS_READING = 13,
    CHORD_CYCLE_PAGE_TURN = 14,
    CHORD_SYNC_PROGRESS = 15,
    CHORD_FILE_TRANSFER = 16,
    CHORD_CALIBRE_WIRELESS = 17,
    CHORD_JOIN_NETWORK = 18,
    CHORD_CREATE_HOTSPOT = 19,
    CHORD_TOGGLE_DARK_MODE = 20,
    CHORD_FOOTNOTES = 21,
    CHORD_FILE_BROWSER = 22,
    CHORD_CREATE_CLIPPING = 23,
    CHORD_LOOKUP_WORD = 24,
    CHORD_TOGGLE_HOME_BUTTON = 25,
    CHORD_QUICK_ACTIONS = 26,
    CHORD_TOGGLE_FRONTLIGHT = 27,
    CHORD_TOGGLE_TOUCHSCREEN = 28,
    CHORD_PREVIOUS_PAGE = 29,
    CHORD_NEARBY_POSITION_SYNC = 30,
    CHORD_LIBRARY = 31,
    CHORD_HOME_READER = 32,
    POWER_CHORD_ACTION_COUNT
  };

  // Home-key shortcuts reuse power-button actions where possible. Keep the
  // dedicated values stable because they are persisted in settings.bin.
  enum HOME_BUTTON_ACTION {
    HOME_BUTTON_BACK_HOME = 23,
    HOME_BUTTON_TOGGLE_FRONTLIGHT = 24,
    HOME_BUTTON_READER_MENU = 25,
    HOME_BUTTON_ACTION_COUNT = 26
  };

  static constexpr uint8_t QUICK_ACTION_SLOT_ACTION_COUNT = 23;

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    FONT_SIZE_CHANGE = 3,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme. Raw values are persisted in settings; keep existing values stable.
  enum UI_THEME {
    CLASSIC = 0,
    LYRA = 1,
    LYRA_3_COVERS = 2,
    ROUNDEDRAFF = 3,
    LYRA_CAROUSEL = 4,
    MINIMAL = 5,
    DASHBOARD = 6,
    BOOKSHELF = 7,
    COVER_GRID = 8,
    UI_THEME_COUNT = 9
  };
  enum RECENT_BOOKS_VIEW { RECENT_BOOKS_LIST = 0, RECENT_BOOKS_GRID = 1, RECENT_BOOKS_VIEW_COUNT };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };
  enum TOUCH_READER_CONTROLS { TOUCH_READER_OFF = 0, TOUCH_READER_ON = 1, TOUCH_READER_CONTROLS_COUNT };
  enum PAGE_TURN_GESTURE {
    TAP_AND_SWIPE = 0,
    TAP_ONLY = 1,
    SWIPE_ONLY = 2,
    INVERTED_TAP = 3,
    PAGE_TURN_GESTURE_DISABLED = 4,
    PAGE_TURN_GESTURE_COUNT
  };

  enum INDEXING_METHOD { INDEXING_INCREMENTAL = 0, INDEXING_FULL_SECTION = 1, INDEXING_METHOD_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_ON = 1, TILT_PAGE_TURN_COUNT };
  enum TILT_PAGE_TURN_DIRECTION {
    TILT_LEFT_RIGHT = 0,
    TILT_LEFT_RIGHT_INVERTED = 1,
    TILT_FORWARD_BACK = 2,
    TILT_FORWARD_BACK_INVERTED = 3,
    TILT_PAGE_TURN_DIRECTION_COUNT
  };

  // Long-press Confirm (menu button) quick action in reader
  enum LONG_PRESS_MENU_ACTION {
    LONG_MENU_OFF = 0,
    LONG_MENU_SLEEP = 1,
    LONG_MENU_CHANGE_FONT = 2,
    LONG_MENU_TOGGLE_GUIDE_DOTS = 3,
    LONG_MENU_TOGGLE_FOCUS = 4,
    LONG_MENU_TOGGLE_BOOKMARK = 5,
    LONG_MENU_REFRESH_SCREEN = 6,
    LONG_MENU_SYNC_PROGRESS = 7,
    LONG_MENU_MARK_FINISHED = 8,
    LONG_MENU_READING_STATS = 9,
    LONG_MENU_SCREENSHOT = 10,
    LONG_MENU_CYCLE_PAGE_TURN = 11,
    LONG_MENU_FILE_TRANSFER = 12,
    LONG_MENU_TOGGLE_TILT_PAGE_TURN = 13,
    LONG_MENU_TOGGLE_DARK_MODE = 14,
    LONG_MENU_FOOTNOTES = 15,
    LONG_MENU_FILE_BROWSER = 16,
    LONG_MENU_CALIBRE_WIRELESS = 17,
    LONG_MENU_JOIN_NETWORK = 18,
    LONG_MENU_CREATE_HOTSPOT = 19,
    LONG_MENU_CREATE_CLIPPING = 20,
    LONG_MENU_LOOKUP_WORD = 21,
    // Appended: values are persisted in settings.bin.
    LONG_MENU_QUICK_ACTIONS = 22,
    LONG_MENU_QUICK_LOCK = 23,
    LONG_MENU_LIBRARY = 24,
    LONG_PRESS_MENU_ACTION_COUNT
  };

  // Clipping storage mode
  enum CLIPPING_STORAGE : uint8_t { SINGLE_FILE = 0, PER_BOOK = 1, CLIPPING_STORAGE_COUNT };
  // Clip selector navigation scheme
  enum CLIP_NAV_MODE : uint8_t { LINE_AWARE = 0, WORD_BY_WORD = 1, CLIP_NAV_MODE_COUNT };
  // Annotation underline visibility
  enum ANNOTATION_VISIBILITY : uint8_t { ANNOT_VISIBLE = 0, ANNOT_HIDDEN = 1, ANNOTATION_VISIBILITY_COUNT };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // UI scale for list-style screens: sizes list fonts and row heights
  // together so touch targets grow uniformly.
  // Lingua feature: how translated text renders on e-ink.
  // VALUE STABILITY: translationDisplayMode persists as this integer in settings.json, so new
  // modes MUST be APPENDED at the end — never inserted or renumbered — or existing on-device
  // saves are silently reinterpreted. LINGUA_TOOLTIP is therefore 7 here even though the upstream
  // fork numbered its tooltip mode 6 (its enum ordered TOOLTIP before its Modal mode); v2 had
  // already shipped that mode as 6 — LINGUA_PAGE_TRANSLATION — so tooltip appends as 7.
  //
  // PERMANENT HOLES: 1 and 2. They were the "Dimmed" / "Dimmed Light" modes, which are now ONE
  // mode (LINGUA_INTERLEAVED) plus the translationShade colour sub-setting. The two values are retired,
  // NEVER selectable (they are absent from LINGUA_SELECTABLE_MODES in LinguaModeCatalog.h) and
  // migrated to LINGUA_INTERLEAVED + shade at load (see fromJson). They are kept as holes — never
  // reused, never renumbered — so an old settings.json is migrated rather than reinterpreted.
  enum LINGUA_MODE : uint8_t {
    LINGUA_NORMAL = 0,
    LINGUA_LEGACY_DIMMED = 1,        // retired hole -> LINGUA_INTERLEAVED + SHADE_DIMMED
    LINGUA_LEGACY_DIMMED_LIGHT = 2,  // retired hole -> LINGUA_INTERLEAVED + SHADE_DIMMED_LIGHT
    LINGUA_ORIGINAL_ONLY = 3,
    LINGUA_TRANSLATION_ONLY = 4,
    LINGUA_SIDE_BY_SIDE = 5,
    LINGUA_PAGE_TRANSLATION = 6,
    LINGUA_TOOLTIP = 7,
    LINGUA_INTERLEAVED = 8,
    // Each sentence's translation on its own small line ABOVE the source line it starts on. The one
    // mode with a layout that is not shared with any other (LinguaLayout::Interlinear).
    LINGUA_INTERLINEAR = 9,
  };
  // LOAD-TIME VALIDITY BOUND ONLY: fromJson() clamps a stored translationDisplayMode >= this to
  // LINGUA_NORMAL. It is deliberately NOT an enumerator and NOT a UI iteration count — the retired
  // holes at 1 and 2 make the value range non-contiguous, so every UI list and cycle walks
  // LINGUA_SELECTABLE_MODES instead (modules/lingua/LinguaModeCatalog.h).
  static constexpr uint8_t LINGUA_MODE_COUNT = LINGUA_INTERLINEAR + 1;

  // Lingua: colour of translated text in Interleaved mode (LINGUA_INTERLEAVED). It selects the
  // renderer's gray level for words carrying the TRANSLATED style bit.
  // DRAWING ONLY: it never changes word measurement, line breaking or pagination, so it must NOT
  // enter the section.bin cache key (ReaderRenderSpec) — switching shade stays instant.
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum TRANSLATION_SHADE : uint8_t { SHADE_DIMMED = 0, SHADE_DIMMED_LIGHT = 1, TRANSLATION_SHADE_COUNT };

  // Lingua: colour of the SECONDARY text in the two modes that render it as its own
  // typographic object rather than inline — the annotation rows in Interlinear and the translation
  // column in Side by Side.
  //
  // NOT the same value space as TRANSLATION_SHADE above, deliberately: that one is Interleaved's,
  // its 0/1 are persisted and documented append-only, and it offers no Black because inline
  // translated text drawn black is indistinguishable from the source (that IS Normal mode). Here
  // the text is separated by position, so Black is meaningful and is the default.
  //
  // The VALUES ARE the renderer's ink levels (0/1/2), so the resolvers below hand them straight to
  // PageFontSet without a mapping table. That coupling is append-safe for PERSISTENCE but not for
  // RENDERING: renderCharImpl's chain tests 0, 1 and 2 explicitly and has no else, so a fourth
  // enumerator would draw nothing at all rather than degrade. A new shade needs a renderer level
  // first.
  // DRAWING ONLY: applied per-line at render time through LineFontRole, never a layout input, so it
  // must NOT enter ReaderRenderSpec, section.bin or the reader's re-layout gate.
  // NEEDS THE GRAYSCALE PASSES: levels 1 and 2 are painted by the LSB/MSB plane passes, which
  // renderContents() runs only when Text Anti-Aliasing is on (needsTextGrayscale). With it off all
  // three levels take the same BW full-coverage fallback and the row has NO visible effect — not a
  // degradation to black, an exact no-op. Pre-existing for Interleaved's shade; deliberately not
  // guarded in the UI, because the setting is still correct and applies the moment AA is turned on.
  // VALUE STABILITY: persisted as an integer; append only, never renumber.
  enum LINGUA_SHADE : uint8_t {
    LINGUA_BLACK = 0,
    LINGUA_GREY = 1,
    LINGUA_GREY_LIGHT = 2,
    LINGUA_SHADE_COUNT,
  };

  // Lingua: type size of the TRANSLATED text relative to the book's own text.
  // SIZE_SMALLER means one step DOWN the active family's point-size ladder, resolved by
  // smallerReaderFontId().
  // ONE ENUM, THREE INDEPENDENT FIELDS: the value space is shared, the choice is not. Each mode
  // that shows translated text owns its own stored size (interleavedTranslationSize,
  // tooltipTranslationSize, pageTranslationSize) because the three answer different questions and
  // have different costs — the Interleaved size is a LAYOUT difference (narrower glyphs re-break
  // lines) and so enters the section cache key, while the two overlay sizes are composited at view
  // time over an unchanged page and must NOT invalidate anything. Sharing one field made shrinking
  // the tooltip silently re-lay out the whole book.
  // AVAILABILITY: SIZE_SMALLER is only offered when the active family actually ships a smaller
  // face; where it does not, smallerReaderFontId() returns 0 and everything behaves as SIZE_SAME
  // without the stored value being rewritten (see smallerReaderFontId()).
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum TRANSLATION_SIZE : uint8_t { SIZE_SAME = 0, SIZE_SMALLER = 1, TRANSLATION_SIZE_COUNT };

  // Lingua: type size of Interlinear's ANNOTATION ROWS. Its own value space, NOT TRANSLATION_SIZE:
  // that enum is relative to the body text ("same" / "one step down the reader ladder"), while these
  // rows have never been on the reader ladder at all -- they are a fixed small UI face sitting above
  // a source line, so the meaningful choice is an absolute point size, not a relation.
  //
  // The three point sizes resolve to the UI faces registered unconditionally in main.cpp
  // (SMALL_FONT_ID / UI_10_FONT_ID / UI_12_FONT_ID), so no build carries a font for this row that it
  // did not already carry: all three are the everyday menu faces, they live in flash as static const
  // bitmaps, and EpdFont holds nothing but a pointer to them. ANNOTATION_BODY is font id 0, the
  // "same as the body font" signal that this resolver already returns for an unsupported script.
  //
  // SCRIPT COVERAGE is identical across the three: edslab_ui_8/10/12 are generated from the same
  // fontconvert source list (EdsLab + Noto Hebrew/Arabic + Ubuntu Vietnamese), so
  // interlinearAnnotationScriptSupported() gates all of them with one predicate.
  // LAYOUT INPUT: unlike the shade, the size changes line breaking and row height, so it reaches
  // ReaderRenderSpec::annotationFontId and the section cache. See getInterlinearAnnotationFontId().
  // VALUE STABILITY: persisted as an integer; append only, never renumber.
  // NO "same as the body font" OPTION, deliberately. It was offered during development and removed
  // before release: an annotation face as wide as the body text needs about as much room as the
  // sentence it translates, and a translation is usually LONGER than its source, so every sentence
  // overflowed its strips. Interlinear answers overflow by carrying the tail forward, which means
  // that option turned the carry on permanently -- the translation drifted away from its sentence
  // everywhere, which is the one thing the mode exists to get right. The three point sizes below all
  // stay narrower than any body size the reader offers (12-18pt), so the carry stays exceptional.
  enum INTERLINEAR_ANNOTATION_SIZE : uint8_t {
    ANNOTATION_8PT = 0,  // the pre-existing fixed face, and still the default
    ANNOTATION_10PT = 1,
    ANNOTATION_12PT = 2,
    INTERLINEAR_ANNOTATION_SIZE_COUNT
  };

  // Lingua feature: translation backend selection
  // Values match upstream fork (crosspoint-reader) to keep JSON-stored indices stable.
  // VALUE STABILITY: persisted as an integer — append only, never renumber.
  enum TRANSLATION_ENGINE : uint8_t {
    ENGINE_GOOGLE_FREE = 0,
    ENGINE_DEEPL = 1,
    ENGINE_DEEPL_PRO = 2,
    ENGINE_OPENAI = 3,
    ENGINE_DEEPSEEK = 4,
    ENGINE_GEMINI = 5,
    ENGINE_GOOGLE_V2 = 6,
    ENGINE_GOOGLE_HTML = 7,
    // Microsoft's Edge-browser translator deployment (api-edge.cognitive.microsofttranslator.com),
    // authenticated with an anonymous short-lived token from edge.microsoft.com/translate/auth.
    // This is NOT the paid Azure Translator resource (api.cognitive.microsofttranslator.com),
    // which would need a subscription key + region — this endpoint is keyless, so no UI is needed.
    ENGINE_AZURE = 8,
    TRANSLATION_ENGINE_COUNT
  };

  // Which physical button pair drives a translation overlay (tooltip sentence stepping /
  // Page Translation scrolling). Shared by tooltipButtons and pageTranslationButtons.
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum OVERLAY_BUTTONS : uint8_t {
    OVERLAY_BUTTONS_FRONT = 0,  // front pair (Left / Right)
    OVERLAY_BUTTONS_SIDE = 1,   // side pair (PageBack / PageForward)
    OVERLAY_BUTTONS_COUNT
  };

  // What tooltip stepping does when it reaches a page boundary (last/first sentence).
  // VALUE STABILITY: persisted as an integer; 0/1 are fixed — append only, never renumber.
  enum TOOLTIP_NAVIGATION : uint8_t {
    TOOLTIP_NAV_LOOP = 0,       // wrap to the first/last sentence, stay on the page
    TOOLTIP_NAV_TURN_PAGE = 1,  // turn the page and continue stepping on the next page
    TOOLTIP_NAVIGATION_COUNT
  };

  enum UI_SCALE { UI_SCALE_SMALL = 0, UI_SCALE_LARGE = 1, UI_SCALE_COUNT };
  static uint8_t defaultUiScale();

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Night mode: inverted output polarity, applied to every activity per render
  // by ActivityManager. Quick Resume preserves it; other sleep screens remain normal.
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarBookPercentageFormat = BOOK_PERCENTAGE_WHOLE;
  uint8_t stablePageNumbers = 0;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarTimeLeft = TIME_LEFT_HIDE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  ReaderStatusBarConfig topReaderStatusBar{};
  ReaderStatusBarConfig bottomReaderStatusBar = [] {
    ReaderStatusBarConfig config;
    config.slots = {ReaderStatusBarItem::Battery,
                    ReaderStatusBarItem::Empty,
                    ReaderStatusBarItem::Empty,
                    ReaderStatusBarItem::TitleChapter,
                    ReaderStatusBarItem::ChapterPageCount,
                    ReaderStatusBarItem::BookProgressPercentage,
                    ReaderStatusBarItem::Empty};
    return config;
  }();
  uint8_t legacyXtcTopUsesBottom = 0;
  DisplayStatusBarConfig displayStatusBar;
  // Clock visibility mode (requires an RTC-backed clock).
  uint8_t hideClock = HIDE_CLOCK_ALWAYS;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Date display format. Values match HalClock::DateFormat; 0 preserves the existing "Jan 01, 2026" default.
  uint8_t dateFormat = DATE_FORMAT_MONTH_DAY_YEAR_LONG;
  // Separator for numeric dates. Text-based date formats do not use it.
  uint8_t dateSeparator = DATE_SEPARATOR_SLASH;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Set once an NTP sync writes both date and time. Kept separate so older
  // time-only syncs do not unlock date display with stale RTC date registers.
  uint8_t clockDateHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t forceParagraphIndents = 0;
  uint8_t textAntiAliasing = 1;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Page-turn gestures remain independently configurable while touch reader controls stay enabled.
  uint8_t pageTurnGesture = TAP_AND_SWIPE;
  uint8_t previousPageGesture = TAP_AND_SWIPE;
  uint8_t customBootscreenEnabled = 1;
  uint8_t tapToHideStatusBar = 1;
  // Disables all touchscreen input while a reader is active. Reader menus temporarily override this.
  uint8_t disableReaderTouchscreen = 0;
  // Available only on multi-touch hardware; defaults on for pinch font resizing.
  uint8_t pinchFontResizeEnabled = 1;
  // Two-finger twist rotates the reader screen. Multi-touch hardware only.
  uint8_t twoFingerRotationEnabled = 1;
  // Configurable two-finger swipes. A non-empty action may be assigned to one direction only.
  uint8_t twoFingerSwipeUp = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t twoFingerSwipeDown = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t twoFingerSwipeLeft = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t twoFingerSwipeRight = TWO_FINGER_SWIPE_NOT_SET;
  // One-finger slides along the screen edges. These can share action choices.
  uint8_t leftEdgeUp = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t leftEdgeDown = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t rightEdgeUp = TWO_FINGER_SWIPE_NOT_SET;
  uint8_t rightEdgeDown = TWO_FINGER_SWIPE_NOT_SET;
  // Short power button action behaviour
  uint8_t shortPwrBtn = IGNORE;
  // Long power button action behaviour
  uint8_t longPwrBtn = SLEEP;
  // Power + Up shortcut action. Disabled by default so the established
  // Power + Down screenshot chord remains screenshot-only.
  uint8_t powerChordAction = CHORD_DISABLED;
  // Up + Down shortcut action. On touch hardware, while the reader touchscreen
  // is disabled, this chord instead opens Settings as the recovery route.
  uint8_t sideButtonChordAction = CHORD_DISABLED;
  // X4 Pro capacitive Home-key actions. Values below SHORT_PWRBTN_COUNT map
  // directly to the matching power-button shortcut action.
  uint8_t homeButtonTapAction = HOME_BUTTON_BACK_HOME;
  uint8_t homeButtonDoubleTapAction = HOME_BUTTON_TOGGLE_FRONTLIGHT;
  uint8_t homeButtonLongPressAction = HOME_BUTTON_READER_MENU;
  // Home-key devices can lock the capacitive Home key while a reader page is
  // active. Reader menus temporarily override this without changing the value.
  uint8_t homeButtonInReaderEnabled = 1;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Legacy layouts are retained for migration only.
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonOrientationAware = FRONT_ORIENTATION_AWARE_OFF;
  uint8_t sideButtonOrientationAware = 0;
  // Legacy shared side-button long action, retained for migration only.
  uint8_t sideButtonLongPress = SIDE_LONG_CHAPTER_SKIP;
  uint8_t sideButtonUpShort = PREVIOUS_PAGE;
  uint8_t sideButtonUpLong = SIDE_PREVIOUS_CHAPTER;
  uint8_t sideButtonDownShort = PAGE_TURN;
  uint8_t sideButtonDownLong = SIDE_NEXT_CHAPTER;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader-specific front button remap (overrides system mapping while in reader activities).
  // readerFrontButtonsEnabled = 0 means the reader uses the system mapping above.
  uint8_t readerFrontButtonsEnabled = 0;
  uint8_t readerFrontButtonBack = FRONT_HW_BACK;
  uint8_t readerFrontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t readerFrontButtonLeft = FRONT_HW_LEFT;
  uint8_t readerFrontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = LEXENDDECA;
  // The physical reader size selected by the user. Built-in and SD-card font
  // families resolve this to their closest available file.
  uint8_t readerFontPointSize = 14;
  // Transient compatibility state for JSON settings written before fontSize
  // stored a point size. SdCardFontSystem resolves it once the family catalog
  // is available, then persists readerFontPointSize.
  uint8_t legacySdFontSizeStep = UINT8_MAX;
  uint8_t sdFontSizeRange = SD_FONT_RANGE_TINY;
  uint8_t lineSpacing = NORMAL;  // migration only; new saves use lineHeightPercent
  uint8_t lineHeightPercent = 100;
  uint8_t wordSpacing = 0;

  // Lingua feature
  // translationLanguage: index into LanguagePickerActivity::LANGUAGES[], 0xFF = unset
  uint8_t translationLanguage = 0xFF;
  // sourceTranslationLanguage: 0xFF = auto-detect, otherwise LANGUAGES[] index
  uint8_t sourceTranslationLanguage = 0xFF;
  uint8_t translationEngine = ENGINE_GOOGLE_V2;
  char translateApiKey[128] = "";
  // Chapter recap (reader menu): Google AI Studio (Gemini API) key and the last chosen excerpt size,
  // an index into the recap length picker. Managed by the recap flow, not SettingsList.
  char recapApiKey[128] = "";
  uint8_t recapLengthIndex = 1;
  uint8_t translationDisplayMode = LINGUA_NORMAL;
  // Interleaved-mode (LINGUA_INTERLEAVED) translated-text colour. Drawing-only; see TRANSLATION_SHADE.
  uint8_t translationShade = SHADE_DIMMED;
  // ONE FIELD PER MODE, never shared — same rule as the three translated-text sizes below. The two
  // modes present the secondary text completely differently (an 8pt row above the line vs a
  // full-size half-width column), so the shade that reads well in one is not the one that reads
  // well in the other. Both default to Black, i.e. exactly what each mode drew before the row
  // existed, so an upgrade changes nothing on screen. See LINGUA_SHADE.
  uint8_t interlinearAnnotationShade = LINGUA_BLACK;
  // Interlinear annotation row size. Defaults to ANNOTATION_8PT, the face the rows were fixed at
  // before this row existed, so an upgrade changes nothing on screen. Mirrored in fromJson().
  uint8_t interlinearAnnotationSize = ANNOTATION_8PT;
  // Interlinear can temporarily hide its annotation rows while keeping their layout space. When
  // enabled, a long press of either button in the selected pair toggles their visibility and takes
  // precedence over the button's normal reader action.
  uint8_t interlinearToggleByLongPress = 1;
  uint8_t interlinearToggleButtons = OVERLAY_BUTTONS_SIDE;
  uint8_t sideBySideTranslationShade = LINGUA_BLACK;
  // Translated-text type size — ONE field per mode that shows translated text, never shared (see
  // TRANSLATION_SIZE). The defaults differ on purpose and each is the mode's own pre-existing
  // behaviour, so an upgrade changes nothing on screen:
  //  - Interleaved draws the translation in the main flow, where a smaller face would re-break every
  //    line; it has always matched the body text, so SIZE_SAME.
  //  - Tooltip and Page Translation composite an overlay over the page, and both have ALWAYS drawn it
  //    one step down the ladder (getTooltipFontId() called smallerReaderFontId() unconditionally
  //    before the row existed), so SIZE_SMALLER. Defaulting these to Same would have handed every
  //    existing user body-size overlays on upgrade.
  // These three defaults are mirrored in fromJson(); keep the pairs in sync.
  uint8_t interleavedTranslationSize = SIZE_SAME;
  uint8_t tooltipTranslationSize = SIZE_SMALLER;
  uint8_t pageTranslationSize = SIZE_SMALLER;
  // Tooltip display mode (LINGUA_TOOLTIP) controls. Ported from the upstream fork.
  // tooltipButtons: which button pair steps through per-sentence tooltips (OVERLAY_BUTTONS).
  //   Default SIDE — the page-turn pair reads as the natural "next sentence" control.
  // tooltipBehavior: what stepping does at a page boundary (TOOLTIP_NAVIGATION).
  //   Default TURN_PAGE — stepping past the last sentence turns the page and continues.
  // Persisted manually in toJson/fromJson alongside the other Lingua fields (they are
  // edited from the Lingua submenu, not the generic on-device Settings list).
  uint8_t tooltipButtons = OVERLAY_BUTTONS_SIDE;
  uint8_t tooltipBehavior = TOOLTIP_NAV_TURN_PAGE;
  // Page Translation display mode (LINGUA_PAGE_TRANSLATION) control: which button pair scrolls/closes
  // the OPEN overlay (OVERLAY_BUTTONS). The overlay still OPENS on a side long-press regardless of
  // this setting. Default SIDE (same pair that opened it). Persisted manually in toJson/fromJson,
  // under the "pageTranslationButtons" key (legacy files stored it as "modalButtons"; fromJson
  // reads that as a fallback and resaves).
  uint8_t pageTranslationButtons = OVERLAY_BUTTONS_SIDE;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // Photo slideshow: seconds between images, Fit/Crop display mode, and
  // whether to shuffle the image order instead of showing it sorted.
  uint16_t slideshowIntervalSeconds = 300;
  uint8_t slideshowScaleMode = SLIDESHOW_FIT;
  uint8_t slideshowRandomOrder = 0;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margins. Legacy single-axis settings initialize both values.
  uint8_t screenMarginVertical = 5;
  uint8_t screenMarginHorizontal = 5;
  // Show EPUB publisher pagebreak labels in the reader margin when present.
  uint8_t publisherPageNumbers = 0;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // OPDS download destination (empty = SD root). Edited from the OPDS server list.
  char opdsDownloadFolder[64] = "";
  // Nearby file receive destination (empty = SD root).
  char nearbyReceiveFolder[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // UI Theme
  uint8_t uiTheme = LYRA;
#if defined(CROSSINK_ENABLE_POKEMON)
  // Show the lead Pokemon on supported home screens. Training continues when hidden.
  uint8_t pokemonHomeScreen = 1;
#endif
  uint8_t swapLibraryFileBrowser = 0;
  bool supportsLibraryFileBrowserSwap() const { return uiTheme == MINIMAL || uiTheme == DASHBOARD; }
  bool isLibraryFileBrowserSwapped() const { return supportsLibraryFileBrowserSwap() && swapLibraryFileBrowser; }
  // Recently Opened layout in Library; keep the original raw values for older settings.
  uint8_t recentBooksView = RECENT_BOOKS_LIST;
  // UI scale (list fonts + row heights); touch boards default one step larger
  uint8_t uiScale = defaultUiScale();
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Quick-return from footnotes when a footnote shortcut is active.
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // EPUB section indexing policy. The current chapter keeps its active build.
  uint8_t indexingMethod = INDEXING_FULL_SECTION;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // Guide Dots - places a middle dot between words to guide the eye
  uint8_t guideReadingEnabled = 0;
  // Per-book EPUB render mode runtime value. This is intentionally not saved as a global setting.
  uint8_t epubRenderMode = 0;
  // SD card font family name, including optional range suffix (empty = use built-in fontFamily)
  char sdFontFamilyName[64] = "";
  // Global dictionary SD-card font (empty = use the reader font).
  char dictionarySdFontFamilyName[64] = "";
  // Zero follows the active reader size.
  uint8_t dictionaryFontPointSize = 0;
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Prefer embedded EPUB titles/authors in Library; disable for filename-only scans.
  uint8_t libraryUseMetadata = 1;
  uint8_t librarySortMethod = 4;
  uint8_t librarySortDescending = 1;
  uint8_t libraryListExpanded = 1;
  uint8_t libraryShowSeries = 1;
  uint8_t libraryShowGenre = 1;
  uint8_t libraryShowEpub = 1;
  uint8_t libraryShowXtc = 1;
  uint8_t libraryShowTxt = 1;
  uint8_t libraryShowMarkdown = 1;
  uint8_t libraryHideFinishedBooks = 0;
  // Hide file extensions in the file browser right-side value column (0 = show, 1 = hide)
  uint8_t hideFileExtension = 0;
  // File browser display row style (0 = one-line theme list, 1 = two-line compact display)
  uint8_t fileBrowserDisplay = FILE_BROWSER_DISPLAY_1_LINE;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when marked as finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Automatically write a dated global reading-stats backup before sleep when an RTC is available (0 = off, 1 = on).
  uint8_t autoBackupStats = 1;
  // Idle threshold for reading stats, stored in 10-second units to fit uint8_t.
  uint8_t readingIdleTimeThresholdUnits = 30;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Long-press Confirm (menu button) quick action in reader (0 = off)
  uint8_t longPressMenuAction = LONG_MENU_OFF;
  // Long-press Back quick action in reader (defaults to the historical file browser shortcut)
  uint8_t longPressBackAction = LONG_MENU_FILE_BROWSER;
  // Five reusable reader commands and their single owning shortcut. Keep these
  // adjacent so old settings files simply retain their default-initialized tail.
  uint8_t quickActionSlots[5] = {IGNORE, IGNORE, IGNORE, IGNORE, IGNORE};
  uint8_t quickActionsTrigger = 0;
  // Tilt-based page turning on devices with a supported IMU (X3 and Sticky).
  uint8_t tiltPageTurn = TILT_OFF;
  uint8_t tiltPageTurnDirection = TILT_LEFT_RIGHT;
  // Frontlight quick-panel state (boards with FREEINK_CAP_FRONTLIGHT, e.g. X4
  // Pro). Applied at boot, edited only from the frontlight panel; persisted as
  // category-less entries so they stay out of the Settings screen. Writes are
  // debounced by the panel (saved once on exit), not per slider tick.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // When enabled, restore a previously-on light after sleep. A previous off
  // state falls through to a complete schedule.
  uint8_t frontlightRestoreOnWake = 1;
  // Daily wake-only schedule, in local minutes since midnight.
  // An unset endpoint keeps the schedule inactive; its value is retained while
  // the schedule toggle is off so it can be re-enabled without re-entry.
  uint8_t frontlightScheduleEnabled = 0;
  uint16_t frontlightScheduleStart = 0xFFFF;
  uint16_t frontlightScheduleEnd = 0xFFFF;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Enabled keyboard layouts. Zero derives a default from the UI language;
  // non-zero bits follow KeyboardLayoutSet::ALL table order.
  uint16_t keyboardLayouts = 0;
  // Custom KOReader sync device display name. Empty means use the hardware default.
  char deviceName[21] = "";
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;
  // Master switch for automatic reading statistics; Time Left pace remains independent.
  uint8_t trackReadingStats = 1;

  ~CrossPointSettings() = default;

  static constexpr uint16_t POWER_BUTTON_LONG_PRESS_MS = 400;
  static constexpr uint16_t POWER_BUTTON_WAKE_SHORT_MS = 10;
  static constexpr uint16_t POWER_BUTTON_WAKE_LONG_MS = POWER_BUTTON_LONG_PRESS_MS;
  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;
  static constexpr uint16_t MIN_SLIDESHOW_INTERVAL_SECONDS = 10;
  static constexpr uint16_t MAX_SLIDESHOW_INTERVAL_SECONDS = 600;
  static constexpr uint8_t SD_FONT_MAX_SIZE_STEPS = 8;
  static constexpr uint8_t MIN_READER_FONT_POINT_SIZE = 8;
  static constexpr uint8_t MIN_LINE_HEIGHT_PERCENT = 70;
  static constexpr uint8_t MAX_LINE_HEIGHT_PERCENT = 200;
  static constexpr uint8_t LINE_HEIGHT_PERCENT_STEP = 1;
  static constexpr uint8_t MIN_SCREEN_MARGIN = 5;
  static constexpr uint8_t MAX_SCREEN_MARGIN = 150;
  static constexpr uint8_t SCREEN_MARGIN_SMALL_STEP = 1;
  static constexpr uint8_t SCREEN_MARGIN_LARGE_STEP = 5;
  static constexpr uint8_t MAX_WORD_SPACING = 4;
  static constexpr uint16_t DEFAULT_READING_IDLE_TIME_THRESHOLD_SECONDS = 5 * 60;
  static constexpr uint16_t MIN_READING_IDLE_TIME_THRESHOLD_SECONDS = 30;
  static constexpr uint16_t MAX_READING_IDLE_TIME_THRESHOLD_SECONDS = 10 * 60;
  static constexpr uint8_t READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS = 10;
  static constexpr uint8_t MIN_READING_IDLE_TIME_THRESHOLD_UNITS =
      MIN_READING_IDLE_TIME_THRESHOLD_SECONDS / READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
  static constexpr uint8_t MAX_READING_IDLE_TIME_THRESHOLD_UNITS =
      MAX_READING_IDLE_TIME_THRESHOLD_SECONDS / READING_IDLE_TIME_THRESHOLD_UNIT_SECONDS;
  static constexpr size_t MIN_DEVICE_NAME_LENGTH = 2;
  static constexpr size_t MAX_DEVICE_NAME_LENGTH = sizeof(deviceName) - 1;

  uint16_t getPowerButtonWakeDuration() const {
    return shortPowerPressWakes() ? POWER_BUTTON_WAKE_SHORT_MS : POWER_BUTTON_WAKE_LONG_MS;
  }

  bool shortPowerPressWakes() const { return shortPwrBtn == SLEEP || shortPwrBtn == WAKE_ONLY; }

  bool shouldTrackReadingStats() const { return trackReadingStats != 0; }
  static const char* getDefaultDeviceName();
  const char* getEffectiveDeviceName() const;
  uint16_t getReadingIdleTimeThresholdSeconds() const;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t pointSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const { return getPowerButtonWakeDuration(); }
  uint16_t getPowerButtonLongPressDuration() const { return POWER_BUTTON_LONG_PRESS_MS; }
  static uint8_t getActiveReaderFontSizeCount();
  static uint8_t getStoredReaderFontSize(FONT_SIZE size);
  static uint8_t getReaderFontPointSize(FONT_SIZE size);
  static uint8_t getSdFontRangePointSize(uint8_t range, uint8_t step);
  static bool isSdFontPointSizeAllowedForRange(uint8_t pointSize, uint8_t range);
  FONT_SIZE getEffectiveReaderFontSize() const;
  uint8_t getSdFontTargetPointSize() const;
  bool changeReaderFontSize(bool larger, FontSizeStepMode mode = FontSizeStepMode::Wrap);
  int getReaderFontId() const;
  // Lingua: the reader font id one step DOWN the active family's point-size ladder, or 0 when the
  // family has no smaller face (SD families, or the smallest built-in size).
  int smallerReaderFontId() const;
  int getBuiltInReaderFontId() const;

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(HalFile& file, bool count_only = false) const;

  bool saveToFile() const;
  bool loadFromFile();
  static const char* getFilePath() { return "/.crosspoint/crossink-settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc, bool importingCrossPoint = false);

  static bool parseReaderStatusBars(JsonVariantConst json, ReaderStatusBarsPayload& config);
  ReaderStatusBarConfig readerStatusBar(ReaderStatusBarPosition position) const;
  void setReaderStatusBar(ReaderStatusBarPosition position, const ReaderStatusBarConfig& config);
  // Lingua: which page LAYOUT a display mode implies. THE mode -> layout mapping.
  static LinguaLayout linguaLayoutForDisplayMode(uint8_t mode);
  // Lingua: font id the TRANSLATED text is drawn in, or 0 for "same as the body font". One resolver
  // per owning mode: the first two are LAYOUT inputs (reach the section cache key), the last two are
  // view-time overlay sizes that must never reach a ReaderRenderSpec.
  int getInterleavedTranslationFontId() const;
  int getInterlinearAnnotationFontId() const;
  int getTooltipTranslationFontId() const;
  int getPageTranslationOverlayFontId() const;
  // Lingua: whether the Interlinear annotation UI face covers the selected target script.
  bool interlinearAnnotationScriptSupported() const;
  int translationFontIdForSize(uint8_t sizeSetting) const;

  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight,
                                    EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault) const;

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static void validateReaderFrontButtonMapping(CrossPointSettings& settings);
  static bool isTwoFingerSwipeActionAvailable(uint8_t action, bool frontlightPresent, bool hasColorTemperature);
  static bool normalizeTwoFingerSwipeActions(CrossPointSettings& settings,
                                             uint8_t CrossPointSettings::* editedField = nullptr);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);
  static uint8_t sleepScreenStorageToMode(uint8_t storedValue);
  static uint8_t sleepScreenModeToStorage(uint8_t mode);
  static uint8_t legacyLineSpacingToPercent(uint8_t legacyValue, uint8_t fontFamily, bool sdFontSelected);
  static uint8_t clampedLineHeightPercent(uint8_t value);
  static uint8_t readingIdleTimeThresholdUnitsForSeconds(uint16_t seconds);
  static uint16_t readingIdleTimeThresholdSecondsForUnits(uint8_t units);
#ifdef SIMULATOR
  static bool verifySleepTimeoutMigrationContract();
  static bool verifySleepScreenMigrationContract();
#endif

 private:
  bool loadFromBinaryFile();
  bool migrateLanguageBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
