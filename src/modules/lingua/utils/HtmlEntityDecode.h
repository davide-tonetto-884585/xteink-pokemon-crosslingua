#pragma once

#include <string>

// ── HTML entity decoding for translation-engine output ──────────────────────
//
// Translation engines hand back PLAIN text, which the reader then escapes and
// writes into the book's HTML (TranslationHtmlRewriter) or draws directly
// (Tooltip / Page Translation overlays). Some engines answer in HTML instead:
// Google's translateHtml endpoint returns "dell&#39;alba" and "&quot;ciao&quot;",
// and LLMs occasionally echo entities too. Left as-is, the rewriter escapes the
// '&' again and the reader shows "&#39;" literally.
//
// Decodes, in place:
//   numeric   &#39;  &#x27;  &#X27;           -> the code point, as UTF-8
//   named     &quot; &amp; &nbsp; &rsquo; ... -> lookupHtmlEntity()'s table
// Anything that is not a complete, valid entity (no ';', unknown name, a code
// point of 0, a surrogate, or beyond U+10FFFF) is left untouched, so plain text
// such as "AT&T" or "a & b" passes through unchanged. Single pass: the output
// of one entity is never re-scanned ("&amp;quot;" -> "&quot;", not '"').
namespace lingua {

void decodeHtmlEntitiesInPlace(std::string& text);

}  // namespace lingua
